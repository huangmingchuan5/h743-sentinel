/**
  ******************************************************************************
  * @file    sample_task.h
  * @brief   Step 5.3：采样链路 —— logQueue + EnvTask + MotionTask + 最新快照缓存。
  *
  *          数据流：
  *            EnvTask（100ms，优先级 6）   读 ALS/PS → 更新环境缓存（临界区保护）
  *            MotionTask（50ms，优先级 5）读加速度 + 取环境缓存 → 合成 20Hz 快照
  *                                        → 投递 logQueue（Step 6 由 LoggerTask 消费）
  *                                        → 更新"最新快照"（给 live 命令看）
  *
  *          快照格式与 Step 6 的 32B 日志记录一致（seq/crc 先留空，由 LoggerTask 填）。
  ******************************************************************************
  */

#ifndef __SAMPLE_TASK_H
#define __SAMPLE_TASK_H

#include "main.h"

#define SAMPLE_QUEUE_LEN   128u   /* logQueue：128 槽 × 32B（@20Hz 约 6.4 秒缓冲） */
#define SAMPLE_PERIOD_MS   50u    /* MotionTask 周期：50ms = 20Hz */
#define ENV_PERIOD_MS      100u   /* EnvTask 周期：100ms = 10Hz */

/* 快照 flags 位定义 */
#define SAMPLE_F_ALS_ERR   0x0001u   /* 本拍 ALS 读取失败 */
#define SAMPLE_F_PS_ERR    0x0002u   /* 本拍 PS 读取失败 */
#define SAMPLE_F_PS_OVF    0x0004u   /* 本拍 PS 溢出（物体很近/强红外） */
#define SAMPLE_F_ACC_ERR   0x0008u   /* 本拍加速度读取失败 */
#define SAMPLE_F_TS_REAL   0x8000u   /* bit15：1=时间戳是日历时间（RTC，Step 5.4）；0=相对 tick */

/* 一条快照 == 未来日志记录的前身（32 字节，无填充空洞） */
typedef struct
{
  uint32_t seq;        /* 序号：Step 6 由 LoggerTask 统一分配（现在保持 0） */
  uint32_t tstamp;     /* 时间戳：现在 = FreeRTOS tick(ms)；Step 5.4 起改用 RTC 日历时间 */
  uint16_t als;        /* 环境光原始值 */
  uint16_t ps;         /* 接近：低 10 位数据 | bit15 "接近"标志 */
  int16_t  ax;         /* 加速度原始值（±4g 量程：8192 LSB = 1g） */
  int16_t  ay;
  int16_t  az;
  uint16_t flags;      /* SAMPLE_F_xxx */
  uint8_t  rsv[10];    /* 保留（Step 6 可放其它字段） */
  uint16_t crc;        /* CRC16：Step 6 写盘前计算（现在保持 0） */
} Sample_t;

/* 编译期断言：Sample_t 必须正好 32 字节（对齐出错了编译就会报错） */
typedef char Sample_SizeCheck_t[(sizeof(Sample_t) == 32u) ? 1 : -1];

/** 调度器启动前调用：创建 logQueue 与两个采样任务。 */
void Sample_Start(void);

/** 取"最新快照"（临界区拷贝）。返回 0 = 成功，1 = 还没有产生过快照。
    age_ms（可传 NULL）：距快照生成过去了多少毫秒（新鲜度）。 */
uint8_t Sample_GetLatest(Sample_t *out, uint32_t *age_ms);

/** 统计：当前 logQueue 里积压的条数。 */
uint16_t Sample_QueueCount(void);

/** 统计：因队列满而丢掉的快照数（Step 6 接上 LoggerTask 后应停止增长）。 */
uint16_t Sample_DropCount(void);

#endif /* __SAMPLE_TASK_H */
