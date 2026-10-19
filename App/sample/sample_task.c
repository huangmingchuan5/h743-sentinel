/**
  ******************************************************************************
  * @file    sample_task.c
  * @brief   采样链路实现（见 sample_task.h）。
  ******************************************************************************
  */

#include "sample_task.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "bsp_ap3216c.h"
#include "bsp_mpu6050.h"
#include "bsp_rtc.h"
#include "log.h"
#include "alarm.h"
#include "health.h"
#include "config.h"
#include <string.h>

/* 环境缓存：EnvTask 写、MotionTask 读，两侧都用临界区保护 */
typedef struct
{
  uint16_t als;
  uint16_t ps;
  uint8_t  als_st;   /* Ap3216c_ReadAls 的返回状态（0 = OK） */
  uint8_t  ps_st;    /* Ap3216c_ReadPs 的返回状态（0 = OK，1 = 溢出） */
} EnvData_t;

static QueueHandle_t s_logQueue   = NULL;
static EnvData_t     s_env        = {0};
static Sample_t      s_latest     = {0};
static TickType_t    s_latestTick = 0u;
static uint8_t       s_haveLatest = 0u;
static uint16_t      s_dropCount  = 0u;

/* -------------------------------------------------------------------------- */
/* EnvTask：100ms 读一次 ALS/PS，更新环境缓存                                  */

static void EnvTask(void *argument)
{
  TickType_t lastWake = xTaskGetTickCount();

  (void)argument;

  for (;;)
  {
    uint16_t als = 0u;
    uint16_t ps  = 0u;

    uint8_t als_st = Ap3216c_ReadAls(&als);
    uint8_t ps_st  = Ap3216c_ReadPs(&ps);

    /* 更新环境缓存：写的一侧用临界区保护（几字节拷贝，极短） */
    taskENTER_CRITICAL();
    s_env.als    = als;
    s_env.ps     = ps;
    s_env.als_st = als_st;
    s_env.ps_st  = ps_st;
    taskEXIT_CRITICAL();

    /* 只有读取成功才参与报警判定：I2C 报错不能被当成"变暗" */
    Alarm_FeedLight(als, (als_st == 0u) ? 1u : 0u);
    Health_Report(HEALTH_ENV);

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(ENV_PERIOD_MS));
  }
}

/* -------------------------------------------------------------------------- */
/* MotionTask：50ms 一拍，合成 20Hz 快照 → logQueue + 最新快照缓存             */

static void MotionTask(void *argument)
{
  TickType_t lastWake = xTaskGetTickCount();

  (void)argument;

  for (;;)
  {
    Sample_t s;
    int16_t  ax = 0;
    int16_t  ay = 0;
    int16_t  az = 0;

    uint8_t acc_st = Mpu6050_ReadAccel(&ax, &ay, &az);
    TickType_t nowTick = xTaskGetTickCount();

    /* 1) 取环境缓存的拷贝（临界区保护） */
    {
      uint8_t als_st;
      uint8_t ps_st;

      taskENTER_CRITICAL();
      s.als    = s_env.als;
      s.ps     = s_env.ps;
      als_st   = s_env.als_st;
      ps_st    = s_env.ps_st;
      taskEXIT_CRITICAL();

      /* 2) 组装快照（32B） */
      s.seq    = 0u;                            /* Step 6 由 LoggerTask 分配 */
      s.tstamp = (uint32_t)nowTick;             /* 下面按 RTC 状态填写 */
      s.ax     = ax;
      s.ay     = ay;
      s.az     = az;
      s.flags  = 0u;
      if (als_st != 0u)          { s.flags |= SAMPLE_F_ALS_ERR; }
      if (ps_st == 1u)           { s.flags |= SAMPLE_F_PS_OVF; }
      else if (ps_st != 0u)      { s.flags |= SAMPLE_F_PS_ERR; }
      if (acc_st != 0u)          { s.flags |= SAMPLE_F_ACC_ERR; }

      /* 时间戳：RTC 已校时 → 2000-01-01 起的秒数 + flags bit15；
         未校时（没电/从未设置）→ 退化为上电 tick(ms)，bit15 保持 0 以示区分 */
      {
        uint32_t epoch = 0u;

        if (Rtc_GetEpoch(&epoch) != 0u)
        {
          s.tstamp = epoch;
          s.flags |= SAMPLE_F_TS_REAL;
        }
      }

      memset(s.rsv, 0, sizeof(s.rsv));
      s.crc = 0u;
    }

    /* 3) 入队：不阻塞——队列满就丢这一拍并计数（Step 6 接上 LoggerTask 后会归零） */
    if (xQueueSend(s_logQueue, &s, 0u) == pdPASS)
    {
      Log_Notify();                     /* 叫醒 LoggerTask 来收这一条 */
    }
    else
    {
      s_dropCount++;
    }

    /* 4) 更新"最新快照"缓存（给 live 命令用） */
    taskENTER_CRITICAL();
    s_latest     = s;
    s_latestTick = nowTick;
    s_haveLatest = 1u;
    taskEXIT_CRITICAL();

    /* 读取失败时作废运动基线，避免恢复瞬间的"0 → 真实值"跳变误触发 */
    Alarm_FeedMotion(ax, ay, az, (acc_st == 0u) ? 1u : 0u);
    Health_Report(HEALTH_MOT);

    /* 采样周期来自配置（cfg rate ...，20..200ms，默认 50ms=20Hz） */
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS((uint16_t)Config_GetRateMs()));
  }
}

/* -------------------------------------------------------------------------- */
/* 对外接口                                                                    */

void Sample_Start(void)
{
  /* logQueue：128 槽 × 32B（内存从 FreeRTOS 堆里出，48KB 里约占 4KB） */
  s_logQueue = xQueueCreate(SAMPLE_QUEUE_LEN, sizeof(Sample_t));
  if (s_logQueue == NULL)
  {
    return;   /* 堆不足：configUSE_MALLOC_FAILED_HOOK 会先把系统拦下来 */
  }

  /* ENV：优先级 6——每 100ms 才跑一次，但要让环境数据先就位 */
  (void)xTaskCreate(EnvTask, "ENV", 160u, NULL, 6u, NULL);

  /* MOT：优先级 5——比 console(3)/monitor(2) 高，保证 20Hz 节拍不被字符输出拖慢 */
  (void)xTaskCreate(MotionTask, "MOT", 160u, NULL, 5u, NULL);
}

uint8_t Sample_GetLatest(Sample_t *out, uint32_t *age_ms)
{
  TickType_t now  = xTaskGetTickCount();
  uint8_t    have;
  uint32_t   t;

  taskENTER_CRITICAL();
  have = s_haveLatest;
  *out = s_latest;				//注意：out 在 have 判断之前就被赋值
  t    = (uint32_t)s_latestTick;
  taskEXIT_CRITICAL();


	//如果have 不是 1，说明没更新s_latest
	if (have == 0u)		//s_haveLatest == 1 才能获得最新的Sample
  {
    return 1u;
  }

  if (age_ms != NULL)		//防止传入空地址
  {
	  //获取新鲜度
	  *age_ms = (uint32_t)(now - (TickType_t)t);		//t是上个s的xTaskGetTickCount（Ticks）
  }
  return 0u;
  //成功return 0u;
}

uint16_t Sample_QueueCount(void)
{
  if (s_logQueue == NULL)
  {
    return 0u;
  }
  return (uint16_t)uxQueueMessagesWaiting(s_logQueue);			//尚未被消费的消息数，(返回队列中存储的消息数)
}

uint16_t Sample_DropCount(void)
{
  return s_dropCount;
}

QueueHandle_t Sample_QueueHandle(void)
{
  return s_logQueue;
}
