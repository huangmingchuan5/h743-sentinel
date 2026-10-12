/**
  ******************************************************************************
  * @file    pwm.h
  * @brief   三路 LED PWM 的底层封装（TIM3_CH3/CH4 + TIM2_CH4，频率 1 kHz）。
  ******************************************************************************
  */

#ifndef __PWM_H
#define __PWM_H

#include "main.h"

/* 三个通道的编号 */
#define PWM_CH_RED     0u      /* 红：PB0 -> TIM3_CH3 */
#define PWM_CH_GREEN   1u      /* 绿：PB1 -> TIM3_CH4 */
#define PWM_CH_BLUE    2u      /* 蓝：PA3 -> TIM2_CH4 */

/** 启动三路 PWM 输出，并打开 TIM6 的 10 ms 呼吸中断。 */
void Pwm_Init(void);

/** 设置某一路的亮度：0 = 全灭，100 = 最亮。 */
void Pwm_SetBrightness(uint8_t ch, uint8_t brightness);

#endif /* __PWM_H */
