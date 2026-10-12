/**
  ******************************************************************************
  * @file    bsp_led.c
  * @brief   板载三色 LED 驱动：三路硬件 PWM + 呼吸效果。
  *
  *          红 = PB0 -> TIM3_CH3，绿 = PB1 -> TIM3_CH4，蓝 = PA3 -> TIM2_CH4，
  *          PWM 频率 1 kHz（换算细节在 pwm.c，全工程只写一次）。
  *
  *          呼吸：TIM6 每 10 ms 中断一次，在中断里把呼吸比例推一格并刷新 CCR，
  *          所以主循环卡住也照样呼吸。设定亮度是呼吸的幅度上限：
  *          实际亮度 = 设定亮度 × 呼吸比例(0~100%) ÷ 100。
  ******************************************************************************
  */

#include "bsp_led.h"
#include "pwm.h"
#include "tim.h"                  /* htim6 和 TIM6 */

#define BREATH_TICKS   200u       /* 200 × 10 ms = 2 秒一个完整呼吸周期 */

static App_Color_t s_color      = COLOR_GREEN;   /* 上电默认：绿色呼吸灯 */
static uint8_t     s_brightness = 100u;          /* pwm 命令设的亮度（呼吸幅度上限） */
static uint16_t    s_phase      = 0u;            /* 三角波位置：0 → 100 → 0 */

/* 按"当前颜色 + 呼吸瞬时值"刷新三路 PWM */
static void Led_Update(void)
{
  uint16_t factor;                  /* 0~100：呼吸到哪一步了 */
  uint8_t  level;                   /* 0~100：这一路该输出的亮度 */
  uint8_t  r = 0u;
  uint8_t  g = 0u;
  uint8_t  b = 0u;

  /* 三角波：前 100 步上升，后 100 步下降 */
  factor = (s_phase < (BREATH_TICKS / 2u)) ? s_phase : (BREATH_TICKS - s_phase);

  /* 再平方一下变成二次曲线：亮起/暗下更接近人眼感觉，不会显得"线性发死" */
  factor = (uint16_t)((factor * factor) / 100u);

  level = (uint8_t)(((uint32_t)s_brightness * factor) / 100u);

  switch (s_color)
  {
    case COLOR_RED:   r = level; break;
    case COLOR_GREEN: g = level; break;
    case COLOR_BLUE:  b = level; break;
    case COLOR_WHITE: r = level; g = level; b = level; break;   /* 三路同步呼吸 = 白光 */
    case COLOR_OFF:
    default:          break;                                     /* 三路都保持 0 */
  }

  Pwm_SetBrightness(PWM_CH_RED, r);
  Pwm_SetBrightness(PWM_CH_GREEN, g);
  Pwm_SetBrightness(PWM_CH_BLUE, b);
}

/* TIM6 的 10 ms 中断回调（stm32h7xx_it.c 里的 TIM6_DAC_IRQHandler 会转到这） */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM6)
  {
    s_phase++;
    if (s_phase >= BREATH_TICKS)
    {
      s_phase = 0u;
    }
    Led_Update();
  }
}

void Led_Init(void)
{
  Pwm_Init();                 /* 启动三路 PWM + TIM6 中断 */
  s_phase = 0u;
  Led_Update();               /* 先刷一次，不用等第一个中断 */
}

void Led_AllOff(void)
{
  Pwm_SetBrightness(PWM_CH_RED, 0u);
  Pwm_SetBrightness(PWM_CH_GREEN, 0u);
  Pwm_SetBrightness(PWM_CH_BLUE, 0u);
}

void Led_SetColor(App_Color_t color)
{
  s_color = color;            /* 下一个呼吸中断（≤10 ms）就按新颜色输出 */
}

void Led_SetBrightness(uint8_t brightness)
{
  if (brightness <= 100u)
  {
    s_brightness = brightness;
  }
}
