/**
  ******************************************************************************
  * @file    bsp_led.h
  * @brief   板载三色 LED 驱动（三路硬件 PWM + 呼吸效果）。
  *
  *          接线（引脚已交给定时器直接驱动，LED 低电平点亮）：
  *              红 = PB0 -> TIM3_CH3
  *              绿 = PB1 -> TIM3_CH4
  *              蓝 = PA3 -> TIM2_CH4
  *          亮度换算在 pwm.c，呼吸算法在 bsp_led.c。
  ******************************************************************************
  */

#ifndef __BSP_LED_H
#define __BSP_LED_H

#include "main.h"

/* ---- 逻辑颜色：命令层和 config 模块共用 ---- */
typedef enum
{
  COLOR_OFF = 0,
  COLOR_RED,
  COLOR_GREEN,
  COLOR_BLUE,
  COLOR_WHITE
} App_Color_t;

/** 启动三路 PWM 和 TIM6 呼吸中断；上电默认"绿色呼吸灯"。 */
void Led_Init(void);

/** 三个灯全灭。 */
void Led_AllOff(void);

/** 换颜色：下一拍呼吸（≤10 ms）起按新颜色输出，呼吸不停。 */
void Led_SetColor(App_Color_t color);

/** 设置亮度 0~100，它同时是呼吸的幅度上限。 */
void Led_SetBrightness(uint8_t brightness);

#endif /* __BSP_LED_H */
