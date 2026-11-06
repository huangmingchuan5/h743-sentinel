/**
  ******************************************************************************
  * @file    app_freertos_hooks.c
  * @brief   FreeRTOS 钩子函数：任务栈溢出 / 堆分配失败。
  *
  *          与 User/FreeRTOSConfig.h 里的开关配套（缺了就链接报 L6218E）：
  *            configCHECK_FOR_STACK_OVERFLOW = 2 -> 需要 vApplicationStackOverflowHook
  *            configUSE_MALLOC_FAILED_HOOK   = 1 -> 需要 vApplicationMallocFailedHook
  *
  *          这两个钩子只会在系统已经"不可继续"时被调用：
  *          先关中断把世界停下来，再尽力从串口打一条遗言，
  *          然后死循环——等你用调试器抓现场，或从串口日志定位问题。
  ******************************************************************************
  */

#include "FreeRTOS.h"
#include "task.h"

/* 致命错误专用输出：直接轮询 USART1 的 TDR，不依赖 HAL 句柄、中断、
   HAL_GetTick 超时或 RTOS 互斥量；可在关中断 / 调度器启动前 / ISR 上下文使用。
   正常路径的 printf 仍走 bsp_uart.c 的重定向，这里只在"世界已停止"时兜底。 */
extern void Uart_PanicPuts(const char *s);

/* 栈溢出钩子：内核在上下文切换时检测到某任务栈越界后调用（pcTaskName 就是肇事任务） */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
  (void)xTask;
  taskDISABLE_INTERRUPTS();                    /* 已经出乱子，先停世界 */
  Uart_PanicPuts("\r\n!! STACK OVERFLOW: ");
  Uart_PanicPuts(pcTaskName);
  Uart_PanicPuts("\r\n");
  for (;;) { }
}

/* 堆分配失败钩子：pvPortMalloc 失败（堆耗尽）时调用 */
void vApplicationMallocFailedHook(void)
{
  taskDISABLE_INTERRUPTS();
  Uart_PanicPuts("\r\n!! MALLOC FAILED (heap exhausted)\r\n");
  for (;;) { }
}
