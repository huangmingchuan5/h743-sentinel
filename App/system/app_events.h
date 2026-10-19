/**
  ******************************************************************************
  * @file    app_events.h
  * @brief   Step 7：全局事件组（UiTask 与各任务之间的"状态板"）。
  ******************************************************************************
  */

#ifndef __APP_EVENTS_H
#define __APP_EVENTS_H

#include "main.h"
#include "FreeRTOS.h"
#include "event_groups.h"

/* 事件位 */
#define EVT_ALARM_LIGHT      (1u << 0)   /* 光照报警已触发 */
#define EVT_ALARM_MOTION     (1u << 1)   /* 运动报警已触发 */
#define EVT_FLASH_BUSY       (1u << 2)   /* LoggerTask 正在写 Flash */
#define EVT_SAMPLING_PAUSED  (1u << 3)   /* 预留：采样暂停 */
#define EVT_LOG_FULL         (1u << 4)   /* 日志写盘出错/丢点 */
#define EVT_ALARM_ANY        (EVT_ALARM_LIGHT | EVT_ALARM_MOTION)

extern EventGroupHandle_t sysEvents;

/** 调度器启动前调用：创建事件组。 */
void AppEvents_Init(void);

#endif /* __APP_EVENTS_H */
