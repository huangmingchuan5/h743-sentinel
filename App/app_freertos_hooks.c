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
#include <stdio.h>

/* 栈溢出钩子：内核在上下文切换时检测到某任务栈越界后调用（pcTaskName 就是肇事任务） */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
  (void)xTask;
  taskDISABLE_INTERRUPTS();                              /* 已经出乱子，先停世界 */
  printf("\r\n!! STACK OVERFLOW: %s\r\n", pcTaskName);   /* 尽力打印（阻塞串口） */
  for (;;) { }
}

/* 堆分配失败钩子：pvPortMalloc 失败（堆耗尽）时调用 */
void vApplicationMallocFailedHook(void)
{
  taskDISABLE_INTERRUPTS();
  printf("\r\n!! MALLOC FAILED (heap exhausted)\r\n");
  for (;;) { }
}
