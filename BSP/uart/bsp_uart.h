/**
  ******************************************************************************
  * @file    bsp_uart.h
  * @brief   调试串口（USART1）BSP：printf 重定向 + RTOS 收发接口。
  *
  *          - 调度器启动前的代码可以继续用 printf()（阻塞逐字节发送）；
  *          - 任务里统一用 Uart_Printf / Uart_Write / Uart_Putc
  *            （互斥量 + 中断发送 + 发送完成信号量，见 bsp_uart.c）；
  *          - 接收走"中断 + 256 B 环形缓冲 + 任务通知"，
  *            由 ConsoleTask 调用 Uart_RxStart() 启动。
  ******************************************************************************
  */

#ifndef __BSP_UART_H
#define __BSP_UART_H

#include "main.h"
#include "FreeRTOS.h"
#include "task.h"

/** 调度器启动前调用：创建 uartMutex / txDoneSem 等 RTOS 对象。 */
void Uart_RtosInit(void);

/** 启动"中断收字节"；notifyTask = 收到字节后要被唤醒的任务（通常是控制台任务）。 */
void Uart_RxStart(TaskHandle_t notifyTask);

/** 从环形缓冲取一个已收到的字节；无数据返回 -1。不阻塞。 */
int Uart_RxGet(void);

/** 因环形缓冲满而丢掉的字节数（统计用，G2 验收参考）。 */
uint16_t Uart_RxDropped(void);

/** 任务里用的线程安全发送：取互斥量 → 中断发送 → 等发送完成。 */
void Uart_Write(const void *buf, uint16_t len);
void Uart_Putc(uint8_t c);
void Uart_Printf(const char *fmt, ...);

#endif /* __BSP_UART_H */
