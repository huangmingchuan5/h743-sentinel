/**
  ******************************************************************************
  * @file    bsp_uart.h
  * @brief   调试串口（USART1）辅助函数：printf 重定向 + 阻塞读。
  ******************************************************************************
  */

#ifndef __BSP_UART_H
#define __BSP_UART_H

#include "main.h"

/**
  * @brief  以轮询方式从 USART1 读取一个字节。
  * @param  timeout_ms  等待一个字节的最长时间
  * @retval 收到字节时返回 0..255，超时（没有输入）时返回 -1
  */
int Uart_ReadByte(uint32_t timeout_ms);

#endif /* __BSP_UART_H */
