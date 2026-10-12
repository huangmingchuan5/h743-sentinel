/**
  ******************************************************************************
  * @file    console.h
  * @brief   Day 1 命令控制台：help / color / pwm。
  ******************************************************************************
  */

#ifndef __CONSOLE_H
#define __CONSOLE_H

#include "main.h"

/* 颜色取值（COLOR_xxx）集中放在驱动层 bsp_led.h 的 App_Color_t 里，
   本文件不再重复定义。 */

/** 复位行缓冲并打印第一个提示符。 */
void Console_Init(void);

/** 在主循环中调用。它最多等待 10 ms 来接收一个字节。 */
void Console_Task(void);

/** 当前状态，后续由 PWM 驱动以及 save/load 使用。 */
uint8_t Console_GetColor(void);
uint8_t Console_GetBrightness(void);

/** 直接设置状态（供 config 模块恢复 EEPROM 配置用；颜色会立即刷新 LED）。 */
void Console_SetColor(uint8_t color);
void Console_SetBrightness(uint8_t brightness);

/** 当前颜色的英文名（red/green/blue/white/off），用于打印。 */
const char *Console_ColorName(void);

#endif /* __CONSOLE_H */
