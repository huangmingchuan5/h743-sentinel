/**
  ******************************************************************************
  * @file    log.h
  * @brief   Step 6：32B 快照环形日志（QSPI Flash，W25Q256 32MB）。
  *
  *          Flash 布局：
  *            扇区 0        元数据副本 A（64B 检查点 × 64 个）
  *            扇区 1        元数据副本 B
  *            扇区 2..末尾  数据环（32B × N 条，约 14.5 小时 @20Hz）
  *
  *          掉电安全：
  *            - 数据先写、检查点后提交（每 256 条或 30s 一次）；
  *            - 检查点轮换顺序：当前份有有效检查点才允许擦另一份；
  *              新检查点写入并读回校验成功后才使用；
  *            - 启动恢复：取最新有效检查点，再向前扫描"CRC 有效且 seq 连续"
  *              的记录，补齐"数据已写、检查点未提交"的部分；
  *            - 回卷到数据区开头之前强制提交一次检查点。
  *
  *          写入：LoggerTask 消费 logQueue（攒够 32 条或 2s 超时写一批），
  *                每批按页切片写、写后读回校验，跨扇区前先擦除回收。
  ******************************************************************************
  */

#ifndef __LOG_H
#define __LOG_H

#include "main.h"
#include "bsp_qspi_flash.h"     /* FLASH_ERASE_SIZE / FLASH_PAGE_SIZE / 返回码 */
#include "sample_task.h"        /* Sample_t */

/* ---- 记录与批量参数 ---- */
#define LOG_RECORD_SIZE          32u     /* 一条快照 = 32B */
#define LOG_BATCH_MAX            32u     /* 一批最多 32 条（1KB） */
#define LOG_FLUSH_TIMEOUT_MS     2000u   /* 攒不满 32 条时，最多等 2s 就写盘 */
#define LOG_META_COMMIT_RECORDS  256u    /* 每写 256 条提交一次检查点 */
#define LOG_META_COMMIT_MS       30000u  /* 或每 30s 提交一次 */

/* ---- Flash 布局 ---- */
#define LOG_META_A_ADDR   0x00000000u
#define LOG_META_B_ADDR   FLASH_ERASE_SIZE
#define LOG_META_SIZE     64u
#define LOG_META_SLOTS    (FLASH_ERASE_SIZE / LOG_META_SIZE)   /* 每份 64 个检查点 */
#define LOG_DATA_BASE     0x00002000u                          /* 数据环从扇区 2 开始 */

/* 覆盖测试开关：临时把数据环缩到 8 个扇区（32KB，约 50 秒写满一圈）。
   正常使用保持 0；验证环形覆盖时改成 1，测完改回 0 重新编译。 */
#define LOG_TEST_SMALL_DATA   0u
#define LOG_DATA_SECTORS      ((LOG_TEST_SMALL_DATA != 0u) ? 8u : 8190u)
#define LOG_DATA_SIZE         (LOG_DATA_SECTORS * FLASH_ERASE_SIZE)
#define LOG_CAPACITY          (LOG_DATA_SIZE / LOG_RECORD_SIZE)

/* 返回值 */
#define LOG_OK    0x00u
#define LOG_ERR   0x01u

/** 上电调用（读元数据 + 恢复写指针 + 扫描未提交记录）。0 = 成功。 */
uint8_t Log_Init(void);

/** 调度器启动前调用：创建 flashMutex + LoggerTask（优先级 2）。 */
void Log_Start(void);

/** 采样任务入队后调用：通知 LoggerTask 来收数据。 */
void Log_Notify(void);

/** 报警/事件快照投递到 evtQueue（LoggerTask 优先落盘）。0 = 成功。 */
uint8_t Log_EventSend(const Sample_t *evt);

/** 本次 erase 以来写入的总条数（可能大于环形容量）。 */
uint32_t Log_GetCount(void);

/** 当前可读条数 = min(总条数, 环形容量)。 */
uint32_t Log_GetAvailable(void);

/** dump 用批量读取：从"可读窗口"的第 startIndex 条开始读 n 条。
    内部按块加锁（锁不跨输出）。0 = 成功；4 = CRC 校验失败。 */
uint8_t Log_FillDump(uint32_t startIndex, uint32_t n, Sample_t *out);

/** 逻辑擦除：重置元数据与写指针（不整片擦 32MB，秒级完成）。0 = 成功。 */
uint8_t Log_Erase(void);

/** 批量写回读校验失败累计（诊断用）。 */
uint16_t Log_GetWriteErrors(void);

/** 暂停/恢复日志消费（仅测试用：暂停后 logQueue 会填满以验证 evtQueue 优先级）。 */
void Log_SetPaused(uint8_t paused);

#endif /* __LOG_H */
