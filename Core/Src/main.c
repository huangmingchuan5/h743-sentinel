/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "i2c.h"
#include "quadspi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "bsp_uart.h"
#include "bsp_i2c_bus.h"
#include "bsp_ap3216c.h"
#include "bsp_mpu6050.h"
#include "console.h"
#include "config.h"
#include "bsp_qspi_flash.h"
#include "log.h"
#include "bsp_led.h"
#include "FreeRTOS.h"
#include "task.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */

static void LedTask(void *argument);
static void MonTask(void *argument);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_I2C4_Init();
  MX_QUADSPI_Init();
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_TIM6_Init();
  /* USER CODE BEGIN 2 */

	  printf("\r\n========================================\r\n");
	  printf(" P0 Logger  -  STM32H743XIH6\r\n");
	  printf(" USART1 @115200 8-N-1\r\n");
	  printf(" built: %s %s\r\n", __DATE__, __TIME__);
	  printf(" type 'help' for the command list\r\n");
	  printf("========================================\r\n");

  /* 打印上次复位原因（读后清除，便于下次判断）：
     POR=上电  BOR=掉电/欠压  PIN=复位引脚  SFT=软件复位  IWDG/WWDG=看门狗  LPWR=低功耗复位 */
  {
    printf("rst    :");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST)   != 0u) { printf(" POR"); }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST)   != 0u) { printf(" BOR"); }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST)   != 0u) { printf(" PIN"); }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST)   != 0u) { printf(" SFT"); }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDG1RST) != 0u) { printf(" IWDG"); }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDG1RST) != 0u) { printf(" WWDG"); }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_LPWR1RST) != 0u) { printf(" LPWR"); }
    printf("\r\n");
    __HAL_RCC_CLEAR_RESET_FLAGS();
  }

  /* 创建串口 RTOS 对象（uartMutex / txDoneSem）：必须在调度器启动前 */
  Uart_RtosInit();

  /* 创建 I2C 总线互斥量（EEPROM / 传感器共用一条 I2C4）：必须在调度器启动前 */
  I2cBus_Init();

  /* 重要：在调度器启动前调用 FreeRTOS 的"创建类"API（xSemaphoreCreateXxx
     内部的 xQueueGenericReset 有 taskENTER_CRITICAL）会留下 BASEPRI=0x50 的
     中断屏蔽——port.c 里 uxCriticalNesting 的初值是哨兵 0xaaaaaaaa，退出临界区
     时判断 "==0" 不成立，屏蔽不会被清掉。后果：TIM7（HAL 时基）和 TIM6（呼吸灯）
     中断一起被屏蔽，启动阶段任何 HAL_Delay 都会死循环（5.2 首次踩中）。
     这里在所有"创建类"调用之后、任何依赖中断的代码（Led_Init / 传感器初始化）
     之前，手动清一次屏蔽；调度器启动后，第一个临界区退出会自动恢复正常。 */
  __set_BASEPRI(0u);

  Console_Init();

  /* 启动三路 PWM + TIM6 呼吸中断：上电默认绿色呼吸灯 */
  Led_Init();

  /* 上电自动恢复 EEPROM 里的配置（第一次使用会写入默认值） */
  {
    uint8_t cfg_st = Config_Load();

    if (cfg_st == 0u)
    {
      printf("config : loaded (color=%s, brightness=%u)\r\n",
             Console_ColorName(), (unsigned)Console_GetBrightness());
    }
    else if (cfg_st == 1u)
    {
      printf("config : first use, defaults written (color=%s, brightness=%u)\r\n",
             Console_ColorName(), (unsigned)Console_GetBrightness());
    }
    else
    {
      printf("config : EEPROM error (0x%02X)\r\n", (unsigned)cfg_st);
    }
  }

  /* Day 3：检测 QSPI Flash，并扫描日志区里已有的记录条数 */
  {
    uint32_t flash_id = 0u;

    if (Flash_ReadID(&flash_id) == FLASH_OK)
    {
      printf("flash  : ID = %06X\r\n", (unsigned)flash_id);

      if (Log_Init() == FLASH_OK)
      {
        printf("log    : %u record(s)\r\n", (unsigned)Log_GetCount());
      }
      else
      {
        printf("log    : scan failed\r\n");
      }
    }
    else
    {
      printf("flash  : no response (check QSPI wiring/clock)\r\n");
    }
  }

  /* Step 5.2：初始化 I2C 传感器（失败只影响对应读数，不阻塞启动） */
  {
    uint8_t ap_st = Ap3216c_Init();
    uint8_t mp_st = Mpu6050_Init();

    printf("sensor : AP3216C %s, MPU6050 %s (ID=0x%02X)\r\n",
           (ap_st == 0u) ? "ok" : "FAIL",
           (mp_st == 0u) ? "ok" : "FAIL",
           (unsigned)Mpu6050_WhoAmI());
  }
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  /* ---- Step 3：创建3个验证任务，然后启动调度器 ---- */
  if (xTaskCreate(LedTask, "LED", 128u, NULL, 2u, NULL) != pdPASS)
  {
    printf("rtos   : create LED task failed\r\n");
  }
  if (xTaskCreate(MonTask, "MON", 256u, NULL, 2u, NULL) != pdPASS)
  {
    printf("rtos   : create MON task failed\r\n");
  }

  if (xTaskCreate(ConsoleTask, "CON", 512u, NULL, 3u, NULL) != pdPASS)
  {
    printf("rtos   : create CON task failed\r\n");
  }

  printf("rtos   : starting scheduler...\r\n");
  vTaskStartScheduler();		//创建IdleTask和软件定时器任务

  /* 正常情况不会到这里；只有堆不足、连 Idle/定时器任务都建不起来时才会返回 */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 5;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/**
  * @brief  Step 3 验证任务①：LED 心跳。
  *         每 500 ms 在"绿呼吸"和"熄灭"之间翻转一次，肉眼确认调度器在周期性运行。
  *         栈 128 word = 512 B（只调用 BSP 小函数，configMINIMAL_STACK_SIZE 够用）。
  *         优先级 2：低优先级，不抢任何东西（本项目业务任务将来用 2~6）。
  */
static void LedTask(void *argument)
{
  TickType_t lastWake = xTaskGetTickCount();
  uint8_t    ledOn    = 1u;

  (void)argument;

  for (;;)
  {
    Led_SetColor(ledOn ? COLOR_WHITE : COLOR_OFF);
    ledOn ^= 1u;		//取反
	vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(500));   /* 绝对延时（自从上一次唤醒时间 开始延时，避免任务执行时间偏差）：500ms 一次，不累计漂移 */
  }
}

/**
  * @brief  Step 3 验证任务②：运行监视。
  *         每 3 s 打印 FreeRTOS tick / HAL tick 及各自增量、堆水位、任务列表。
  *         两个 tick 增量应同步 ≈3000（G1 的"增量对比"校验就靠这行输出）。
  *         栈 256 word = 1 KB（printf / vTaskList 的格式化比较吃栈）。
  *         优先级 2：与 LedTask 同级 —— 靠时间片轮转共享 CPU，长打印不会独占。
  */
static void MonTask(void *argument)
{
  TickType_t lastWake = xTaskGetTickCount();
  TickType_t prevTick = lastWake;
  uint32_t   prevHal  = HAL_GetTick();
  static char listBuf[512];                  /* vTaskList 要求 ≥512 B 缓冲 */

  (void)argument;

  for (;;)
  {
    TickType_t tickNow = xTaskGetTickCount();
    uint32_t   halNow  = HAL_GetTick();

    Uart_Printf("[rtos=%lu (+%lu)] [hal=%lu (+%lu)] free=%u min=%u rxdrop=%u\r\n",
                (unsigned long)tickNow, (unsigned long)(tickNow - prevTick),
                (unsigned long)halNow,  (unsigned long)(halNow - prevHal),
                (unsigned)xPortGetFreeHeapSize(),
                (unsigned)xPortGetMinimumEverFreeHeapSize(),
                (unsigned)Uart_RxDropped());

    prevTick = tickNow;
    prevHal  = halNow;

    vTaskList(listBuf);                      /* 需要 TRACE_FACILITY=1 + STATS_FORMATTING=1 */
    Uart_Write(listBuf, (uint16_t)strlen(listBuf));
    Uart_Printf("\r\n");

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(3000));  /* 3 s 一次 */
  }
}

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
