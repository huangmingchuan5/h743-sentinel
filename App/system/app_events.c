/**
  ******************************************************************************
  * @file    app_events.c
  * @brief   全局事件组实现。
  ******************************************************************************
  */

#include "app_events.h"

EventGroupHandle_t sysEvents = NULL;

void AppEvents_Init(void)
{
  sysEvents = xEventGroupCreate();
}
