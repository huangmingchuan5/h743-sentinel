/**
  ******************************************************************************
  * @file    health.c
  * @brief   健康监控 + IWDG 实现（见 health.h）。
  ******************************************************************************
  */

#include "health.h"
#include "FreeRTOS.h"
#include "task.h"

/* 调试开关：用调试器单步/长时间停断点时改成 0（或编译时 -DHEALTH_IWDG_ENABLE=0），
   否则暂停超过 2 s 会被 IWDG 复位。发布与验收保持 1。 */
#ifndef HEALTH_IWDG_ENABLE
#define HEALTH_IWDG_ENABLE 1
#endif

/* 每个任务允许的最大"沉默时间"（ms）：周期 + 余量 */
#define HEALTH_GAP_ENV    400u
#define HEALTH_GAP_MOT    500u
#define HEALTH_GAP_UI     400u
#define HEALTH_GAP_CON    2500u
#define HEALTH_GAP_LOG    6000u
#define HEALTH_GAP_LED    2500u
#define HEALTH_GAP_MON    5000u

#if (HEALTH_IWDG_ENABLE != 0)
static IWDG_HandleTypeDef s_hiwdg;
#endif
static volatile TickType_t s_last[HEALTH_COUNT];
static volatile uint8_t    s_healthy    = 1u;
static volatile uint8_t    s_lastFail   = 0xFFu;
static uint32_t            s_resetFlags = 0u;

static uint8_t  s_wdStopActive = 0u;
static TickType_t s_wdStopUntil = 0u;
static uint8_t  s_wdgOk      = 0u;   /* IWDG 初始化成功才允许刷新 */

static uint16_t Health_GapOf(HealthId_t id)
{
  switch (id)
  {
    case HEALTH_ENV: return HEALTH_GAP_ENV;
    case HEALTH_MOT: return HEALTH_GAP_MOT;
    case HEALTH_UI:  return HEALTH_GAP_UI;
    case HEALTH_CON: return HEALTH_GAP_CON;
    case HEALTH_LOG: return HEALTH_GAP_LOG;
    case HEALTH_LED: return HEALTH_GAP_LED;
    case HEALTH_MON: return HEALTH_GAP_MON;
    default:         return 1000u;
  }
}

void Health_Report(HealthId_t id)
{
  if ((uint32_t)id < (uint32_t)HEALTH_COUNT)
  {
    s_last[id] = xTaskGetTickCount();
  }
}

static void HealthTask(void *argument)
{
  (void)argument;

  for (;;)
  {
    TickType_t now = xTaskGetTickCount();
    uint8_t    ok = 1u;
    uint8_t    fail = 0xFFu;
    uint32_t   i;

    for (i = 0u; i < (uint32_t)HEALTH_COUNT; i++)
    {
      if ((TickType_t)(now - s_last[i]) > pdMS_TO_TICKS(Health_GapOf((HealthId_t)i)))
      {
        ok = 0u;
        fail = (uint8_t)i;
        break;
      }
    }

    s_healthy  = ok;
    s_lastFail = fail;

    if (s_wdStopActive != 0u)
    {
      if ((int32_t)(now - s_wdStopUntil) < 0)
      {
        /* wd test 期间故意不喂狗（约 2s 后 IWDG 复位） */
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      s_wdStopActive = 0u;            /* 到点恢复（若还没复位） */
    }

    if ((ok != 0u) && (s_wdgOk != 0u))
    {
#if (HEALTH_IWDG_ENABLE != 0)
      (void)HAL_IWDG_Refresh(&s_hiwdg);
#endif
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void Health_Start(void)
{
  uint32_t i;
  TickType_t now = xTaskGetTickCount();

	//初始化时把 7 个条目全部填成当前时间，表示"所有任务此刻都默认是健康的"
  for (i = 0u; i < (uint32_t)HEALTH_COUNT; i++)			//HEALTH_COUNT == 7
  {
    s_last[i] = now;
  }

  /* 先建任务：任务建不起来就不启用看门狗，避免"没人喂狗"的死复位循环 (先确保喂狗任务成功创建，再启动看门狗)*/
  if (xTaskCreate(HealthTask, "HLTH", 256u, NULL, 1u, NULL) != pdPASS)
  {
    s_healthy = 0u;
    s_wdgOk   = 0u;
    return;
  }

  /* IWDG：LSI≈32kHz，64 分频 → 500Hz；重装 1000 → 超时 2s */
#if (HEALTH_IWDG_ENABLE != 0)
  s_hiwdg.Instance       = IWDG1;
  s_hiwdg.Init.Prescaler = IWDG_PRESCALER_64;
  s_hiwdg.Init.Reload    = 1000u;
  s_hiwdg.Init.Window    = IWDG_WINDOW_DISABLE;
  if (HAL_IWDG_Init(&s_hiwdg) != HAL_OK)
  {
    s_healthy = 0u;
    s_wdgOk   = 0u;
    return;
  }
  s_wdgOk = 1u;
#else
  s_wdgOk = 0u;                       /* 调试版：不启用看门狗 */
#endif
}

uint8_t Health_GetHealthy(void)
{
  return s_healthy;
}

uint8_t Health_GetLastFail(void)
{
  return s_lastFail;
}

void Health_SetResetFlags(uint32_t rsr)
{
  s_resetFlags = rsr;
}

uint32_t Health_GetResetFlags(void)
{
  return s_resetFlags;
}

void Health_SetWdgTest(uint32_t seconds)
{
  if (seconds == 0u)
  {
    s_wdStopActive = 0u;
    return;
  }
  if (seconds > 60u)
  {
    seconds = 60u;
  }

  s_wdStopUntil = (TickType_t)(xTaskGetTickCount() + pdMS_TO_TICKS(seconds * 1000u));
  s_wdStopActive = 1u;
}
