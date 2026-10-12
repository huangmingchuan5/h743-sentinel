/**
  ******************************************************************************
  * @file    pwm.c
  * @brief   三路 LED PWM 底层封装。
  *
  *          CubeMX 已经把三路都配好：1 kHz、PWM mode 1、极性 High、ARR = 999。
  *          这里只做两件事：启动输出，以及把 0~100 的亮度换算成 CCR。
  *
  *          换算（LED 是低电平点亮，所以要"反过来算"）：
  *              CCR = (100 - 亮度) × (ARR + 1) ÷ 100
  *              亮度 0   -> CCR = 1000 > ARR，输出恒为高电平 -> 灯真正全灭
  *              亮度 100 -> CCR = 0，输出恒为低电平          -> 灯最亮
  *          全工程只有这一处做这个换算，别在别处再取反一次。
  ******************************************************************************
  */

#include "pwm.h"
#include "tim.h"

#define PWM_PERIOD_TICKS   1000u    /* ARR + 1 = 999 + 1 */

static uint32_t Pwm_CcrOf(uint8_t brightness)
{
  if (brightness > 100u)            /* 保险：超过 100 按 100 处理 */
  {
    brightness = 100u;
  }

  return ((uint32_t)(100u - brightness) * PWM_PERIOD_TICKS) / 100u;		//因为 LED 低电平点亮，亮度越高，CCR 应该越小。
}

void Pwm_Init(void)
{
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_3);   /* 红 PB0 */
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_4);   /* 绿 PB1 */
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_4);   /* 蓝 PA3 */

  HAL_TIM_Base_Start_IT(&htim6);              /* 10 ms 更新中断：呼吸的"心跳" */
}

void Pwm_SetBrightness(uint8_t ch, uint8_t brightness)
{
  uint32_t ccr = Pwm_CcrOf(brightness);

  switch (ch)
  {
    case PWM_CH_RED:
      __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_3, ccr);
      break;

    case PWM_CH_GREEN:
      __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, ccr);
      break;

    case PWM_CH_BLUE:
      __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_4, ccr);		//快速更新占空比
      break;

    default:
      break;
  }
}
