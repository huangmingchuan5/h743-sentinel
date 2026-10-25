/**
  ******************************************************************************
  * @file    alarm.c
  * @brief   两段式报警实现（见 alarm.h）。
  ******************************************************************************
  */

#include "alarm.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "app_events.h"
#include "config.h"
#include "log.h"
#include "sample_task.h"
#include "bsp_rtc.h"
#include <string.h>

static uint16_t s_lightCnt  = 0u;    /* 连续"暗"采样计数（0..10） */
static uint16_t s_motionCnt = 0u;    /* 泄漏扰动计数（0..需确认的样本数） */
static uint8_t  s_lightAlarm = 0u;
static uint8_t  s_motionAlarm = 0u;
static uint8_t  s_muted     = 0u;
static TickType_t s_triggerTick = 0u;   /* 最近一次触发时刻（延迟指标用） */

static int16_t  s_lastAx = 0;
static int16_t  s_lastAy = 0;
static int16_t  s_lastAz = 0;
static uint8_t  s_haveLast = 0u;

static TimerHandle_t s_muteTimer = NULL;

/* 报警事件落盘：投递一条"事件快照"到 evtQueue（LoggerTask 优先写它） */
static void Alarm_LogEvent(uint16_t evtFlag)
{
  Sample_t ev;
  uint32_t epoch = 0u;

  memset(&ev, 0, sizeof(ev));
  ev.flags = evtFlag;

  if (Rtc_GetEpoch(&epoch) != 0u)
  {
    ev.tstamp = epoch;
    ev.flags |= SAMPLE_F_TS_REAL;
  }
  else
  {
    ev.tstamp = (uint32_t)xTaskGetTickCount();
  }

  (void)Log_EventSend(&ev);
}

//软件定时器到期后的回调函数（将s_muted 由0变成1）
static void Alarm_MuteTimerCb(TimerHandle_t timer)
{
  (void)timer;
  s_muted = 1u;                       /* 30s 到：只静音，红灯继续报警 */
}

void Alarm_Init(void)
{
  s_muteTimer = xTimerCreate("alm", pdMS_TO_TICKS(ALARM_AUTOMUTE_MS), pdFALSE, NULL,
	Alarm_MuteTimerCb);										//pdFALSE表示单次触发，pdTRUE表示自动重装载
}

void Alarm_FeedLight(uint16_t als, uint8_t valid)
{
  uint8_t  trigger = 0u;
  uint16_t thr = Config_GetThrLight();

  taskENTER_CRITICAL();
  if (valid == 0u)
  {
    s_lightCnt = 0u;                  /* 读取失败：本拍不参与判定，确认窗重头计 */
  }
  else if (s_lightAlarm == 0u)
  {
    if (als < thr)
    {
      if (s_lightCnt < ALARM_CONFIRM_LIGHT_SAMPLES)
      {
        s_lightCnt++;
      }
    }
    else
    {
      s_lightCnt = 0u;                /* 变亮：确认窗重头计 */
    }

    if (s_lightCnt >= ALARM_CONFIRM_LIGHT_SAMPLES)
    {
      s_lightAlarm = 1u;
      trigger = 1u;
    }
  }
  taskEXIT_CRITICAL();

  if (trigger != 0u)
  {
    s_triggerTick = xTaskGetTickCount();
    s_muted = 0u;                     /* 新报警要先响 */
    if (s_muteTimer != NULL) { (void)xTimerStart(s_muteTimer, 0u); }
    xEventGroupSetBits(sysEvents, EVT_ALARM_LIGHT);
    Alarm_LogEvent(SAMPLE_F_EVT_LIGHT);
  }
}

void Alarm_FeedMotion(int16_t ax, int16_t ay, int16_t az, uint8_t valid)
{
  uint8_t  trigger = 0u;
  uint32_t thr = (uint32_t)Config_GetThrMotion();
  uint32_t rate = (uint32_t)Config_GetRateMs();
  uint16_t need;                      /* 确认窗换算成样本数：1000ms ÷ 周期 */

  if (rate == 0u)
  {
    rate = 50u;
  }
  need = (uint16_t)((ALARM_CONFIRM_MOTION_MS + rate - 1u) / rate);
  if (need < 2u)
  {
    need = 2u;
  }

  taskENTER_CRITICAL();
  if (valid == 0u)
  {
    s_haveLast = 0u;                  /* 读取失败：作废基线，下一拍重新起算 */
  }
  else if (s_haveLast == 0u)
  {
    s_lastAx = ax;
    s_lastAy = ay;
    s_lastAz = az;
    s_haveLast = 1u;
  }
  else
  {
    int32_t dx = (int32_t)ax - (int32_t)s_lastAx;
    int32_t dy = (int32_t)ay - (int32_t)s_lastAy;
    int32_t dz = (int32_t)az - (int32_t)s_lastAz;
    uint32_t mag2 = (uint32_t)(dx * dx + dy * dy + dz * dz);

    s_lastAx = ax;
    s_lastAy = ay;
    s_lastAz = az;

    if (s_motionAlarm == 0u)
    {
      if (mag2 > (thr * thr))
      {
        if (s_motionCnt < need)
        {
          s_motionCnt++;
        }
      }
      else if (s_motionCnt > 0u)
      {
        s_motionCnt--;                /* 泄漏：偶发单点噪声不触发 */
      }

      if (s_motionCnt >= need)
      {
        s_motionAlarm = 1u;
        trigger = 1u;
      }
    }
  }
  taskEXIT_CRITICAL();

  if (trigger != 0u)
  {
    s_triggerTick = xTaskGetTickCount();
    s_muted = 0u;
    if (s_muteTimer != NULL) { (void)xTimerStart(s_muteTimer, 0u); }
    xEventGroupSetBits(sysEvents, EVT_ALARM_MOTION);
    Alarm_LogEvent(SAMPLE_F_EVT_MOTION);
  }
}

uint8_t Alarm_IsActive(void)
{
  return ((s_lightAlarm != 0u) || (s_motionAlarm != 0u)) ? 1u : 0u;
}

uint8_t Alarm_IsMuted(void)
{
  return s_muted;
}

void Alarm_SetMuted(uint8_t muted)
{
  s_muted = (muted != 0u) ? 1u : 0u;
}

void Alarm_Clear(void)
{
  uint8_t was;

  taskENTER_CRITICAL();
  was = Alarm_IsActive();
  s_lightAlarm  = 0u;
  s_motionAlarm = 0u;
  s_lightCnt    = 0u;
  s_motionCnt   = 0u;
  s_muted       = 0u;
  taskEXIT_CRITICAL();

  xEventGroupClearBits(sysEvents, EVT_ALARM_ANY);   /* 无论是否触发过都清干净 */

  if (was != 0u)
  {
    if (s_muteTimer != NULL) { (void)xTimerStop(s_muteTimer, 0u); }
    Alarm_LogEvent(SAMPLE_F_EVT_CLEAR);
  }
}

uint8_t Alarm_LightActive(void)
{
  return s_lightAlarm;
}

uint8_t Alarm_MotionActive(void)
{
  return s_motionAlarm;
}

TickType_t Alarm_GetTriggerTick(void)
{
  return s_triggerTick;
}
