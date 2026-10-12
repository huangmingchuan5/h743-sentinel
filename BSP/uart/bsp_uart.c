/**
  ******************************************************************************
  * @file    bsp_uart.c
  * @brief   把 C 库的 printf() 重定向到 USART1。
  *
  *          fputc 的写法与火哥例程（20-USART / User / usart /
  *          bsp_debug_usart.c）思路一致：在阻塞模式下用 HAL_UART_Transmit()
  *          一次发送一个字节。
  *          唯一的区别是这里使用 CubeMX 生成的句柄 huart1，
  *          而不是再单独建一个私有的 UART 句柄。
  *
  *          前提：Keil -> Options for Target -> Target -> 勾选 Use MicroLIB
  ******************************************************************************
  */

#include "bsp_uart.h"
#include "usart.h"
#include <stdio.h>

/**
  * @brief  printf() 每发送一个字符就会调用这个函数。
  * @param  ch  要发送的字符（C 库以 int 形式传入）
  * @param  f   数据流，这里没有使用
  * @retval 已发送的字符
  */
int fputc(int ch, FILE *f)
{
  /* 只保留低 8 位：ch 是 int，不能把 4 个字节都发出去 */
  uint8_t byte = (uint8_t)ch;

  /* 阻塞发送 1 个字节，超时 1000 ms */
  HAL_UART_Transmit(&huart1, &byte, 1, 1000);

  (void)f;   /* f 未使用，这样写可以让编译器不报警告 */
  return ch;
}

/**
  * @brief  从 USART1 读取一个字节（阻塞，但带超时）。
  *         火哥例程是在 fgetc 里做同样的事，这里写成普通函数，
  *         方便控制台表达“要么收到一个字节，要么返回 -1”。
  * @param  timeout_ms  等待一个字节的时间
  * @retval 收到字节时返回 0..255，超时返回 -1
  */
int Uart_ReadByte(uint32_t timeout_ms)
{
  uint8_t byte = 0;

  if (HAL_UART_Receive(&huart1, &byte, 1, timeout_ms) != HAL_OK)
  {
    /* 如果存在未清除的溢出错误，接收器会一直卡住，
       所以在回到主循环之前先把它清掉。 */
    __HAL_UART_CLEAR_OREFLAG(&huart1);
    return -1;
  }

  return (int)byte;
}

/********************************* 文件结束 *******************************/
