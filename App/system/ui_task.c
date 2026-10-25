/**
  ******************************************************************************
  * @file    ui_task.c
  * @brief   UiTask 实现（见 ui_task.h）。
  *
  *          50ms 一轮：等事件组（报警/写盘/日志满）→ 处理按键通知 →
  *          切 LED 模式 → 驱动蜂鸣器（200ms 响 / 300ms 停）→ 上报健康。
  ******************************************************************************
  */

#include "ui_task.h"
#include "FreeRTOS.h"
#include "task.h"
#include "app_events.h"
#include "alarm.h"
#include "bsp_led.h"
#include "health.h"

#define BUZZER_PIN          GPIO_PIN_8      /* PI8：有源蜂鸣器 */
//#define BUZZER_ACTIVE_HIGH  1u              /* 1=高电平响；若上电常响就改成 0 */	//用同一份代码适配两种不同硬件接法的蜂鸣器电路（没用处）
#define KEY1_PIN            GPIO_PIN_0      /* PA0：静音切换 */
#define KEY2_PIN            GPIO_PIN_13     /* PC13：清除报警 */
#define KEY_DEBOUNCE_MS     20u
#define UI_PERIOD_MS        50u

#define BUZZ_ON_MS          200u            /* 响 200ms */
#define BUZZ_OFF_MS         300u            /* 停 300ms */
#define BUZZ_PERIOD_MS      (BUZZ_ON_MS + BUZZ_OFF_MS)

static TaskHandle_t s_uiTask = NULL;
static uint8_t      s_alarmLatencyValid = 0u;
static uint32_t     s_alarmLatencyMs    = 0u;

static void Ui_BuzzerSet(uint8_t on)
{
  HAL_GPIO_WritePin(GPIOI, BUZZER_PIN, (GPIO_PinState)on);			//初始默认低电平
}

static void Ui_HwInit(void)
{
	GPIO_InitTypeDef g = {0};
	
	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOC_CLK_ENABLE();
	__HAL_RCC_GPIOI_CLK_ENABLE();
	__HAL_RCC_SYSCFG_CLK_ENABLE();
	
  /* 蜂鸣器：PI8 推挽输出，默认静音 */
	g.Pin   = BUZZER_PIN;
	g.Mode  = GPIO_MODE_OUTPUT_PP;
	g.Pull  = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOI, &g);
	Ui_BuzzerSet(0u);				//设置为默认的低电平，不响
	
	/* 按键：上拉输入，按下为低电平 → 下降沿中断 */
	g.Mode = GPIO_MODE_IT_FALLING;
	g.Pull = GPIO_PULLUP;
	g.Pin = KEY1_PIN;
	HAL_GPIO_Init(GPIOA, &g);
	
	g.Pin = KEY2_PIN;
	HAL_GPIO_Init(GPIOC, &g);
	
	/* 调用 FreeRTOS FromISR 接口的中断：优先级必须 ≥5,不然无法被屏蔽 */
	HAL_NVIC_SetPriority(EXTI0_IRQn, 5u, 0u);
	HAL_NVIC_EnableIRQ(EXTI0_IRQn);
	HAL_NVIC_SetPriority(EXTI15_10_IRQn, 5u, 0u);
	HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

/* EXTI 回调（中断上下文）：只负责叫醒 UiTask，消抖留给任务 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  BaseType_t woken = pdFALSE;

  if ((xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) &&
      ((GPIO_Pin == KEY1_PIN) || (GPIO_Pin == KEY2_PIN)) && (s_uiTask != NULL))
  {
    vTaskNotifyGiveFromISR(s_uiTask, &woken);
    portYIELD_FROM_ISR(woken);
  }
}

static void UiTask(void *argument)
{
  TickType_t keyEdge     = 0u;
  uint8_t    keyPending  = 0u;
  uint8_t    alarmPrev   = 0u;
  uint16_t   buzzPhase   = 0u;

  (void)argument;

  for (;;)
  {
    EventBits_t bits = xEventGroupWaitBits(sysEvents,
                                           EVT_ALARM_ANY | EVT_FLASH_BUSY |
                                           EVT_LOG_FULL | EVT_SAMPLING_PAUSED,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(UI_PERIOD_MS));
    TickType_t now   = xTaskGetTickCount();
    uint8_t    alarm = Alarm_IsActive();

    /* 0) 报警刚触发：量一次"确认 → 声光"延迟（供 alarm 命令读出） */
    if ((alarm != 0u) && (alarmPrev == 0u))
    {
      s_alarmLatencyMs    = (uint32_t)((now - Alarm_GetTriggerTick()) * portTICK_PERIOD_MS);
      s_alarmLatencyValid = 1u;
      buzzPhase           = 0u;          /* 从"响"开始，避免最多 300ms 的静默 */
    }
    else if (alarm == 0u)
    {
      buzzPhase = 0u;
    }
    alarmPrev = alarm;

    /* 1) 按键：EXTI 通知 → 20ms 后读电平确认（消抖） */
    if (ulTaskNotifyTake(pdTRUE, 0u) > 0u)
    {
      keyEdge = now;
      keyPending = 1u;
    }

    if ((keyPending != 0u) &&
        ((TickType_t)(now - keyEdge) >= pdMS_TO_TICKS(KEY_DEBOUNCE_MS)))
    {
      keyPending = 0u;

      if (HAL_GPIO_ReadPin(GPIOA, KEY1_PIN) == GPIO_PIN_RESET)
      {
        Alarm_SetMuted((Alarm_IsMuted() != 0u) ? 0u : 1u);
      }
      else if (HAL_GPIO_ReadPin(GPIOC, KEY2_PIN) == GPIO_PIN_RESET)
      {
        Alarm_Clear();
      }
    }

    /* 2) LED 模式：报警 > 写盘 > 正常 */
    if (alarm != 0u)
    {
      Led_SetMode(LED_MODE_ALARM);          /* 红快闪 */
    }
    else if ((bits & EVT_FLASH_BUSY) != 0u)
    {
      Led_SetMode(LED_MODE_FLASH);          /* 蓝闪 */
    }
    else
    {
      Led_SetMode(LED_MODE_NORMAL);         /* 呼吸 */
    }

    /* 3) 蜂鸣器：报警且未静音 → 200ms 响 / 300ms 停（相位在报警起点清零） */
    if ((alarm != 0u) && (Alarm_IsMuted() == 0u))
    {
      Ui_BuzzerSet((buzzPhase < BUZZ_ON_MS) ? 1u : 0u);
      buzzPhase = (uint16_t)((buzzPhase + UI_PERIOD_MS) % BUZZ_PERIOD_MS);
    }
    else
    {
      Ui_BuzzerSet(0u);
    }

    Health_Report(HEALTH_UI);
  }
}

void Ui_Start(void)
{
  Ui_HwInit();
  (void)xTaskCreate(UiTask, "UI", 384u, NULL, 4u, &s_uiTask);
}

uint8_t Ui_GetAlarmLatency(uint32_t *ms)
{
  if ((s_alarmLatencyValid == 0u) || (ms == NULL))
  {
    return 0u;
  }
  *ms = s_alarmLatencyMs;
  return 1u;
}
