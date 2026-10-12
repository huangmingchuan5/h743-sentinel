/**
  ******************************************************************************
  * @file    log.h
  * @brief   QSPI Flash 上的日志记录层（固定 64 字节一条）。
  *
  *          记录格式：
  *            [0]      序号 0..255（未写过的区域是 0xFF）
  *            [1]      正文长度（≤60）
  *            [2..61]  正文，不足部分补 0x00
  *            [62..63] 校验和（序号 + 长度 + 正文逐字节累加，小端）
  *
  *          日志区 = Flash 最前面 4 个 4KB 扇区 = 16 KB ÷ 64 B = 256 条。
  ******************************************************************************
  */

#ifndef __LOG_H
#define __LOG_H

#include "main.h"
#include "bsp_qspi_flash.h"     /* 记录层的返回值直接沿用 Flash 的错误码 */

#define LOG_RECORD_SIZE   64u      /* 一条记录 64 字节（正好是 256 字节页的 1/4，天然不跨页） */
#define LOG_TEXT_MAX      60u      /* 正文最多 60 字节 */
#define LOG_MAX_RECORDS   256u     /* 4 个 4KB 扇区 ÷ 64 字节 */

/** 上电时调用：扫描 Flash，数出已有多少条有效记录；0 = 成功。 */
uint8_t Log_Init(void);

/** 当前条数。 */
uint16_t Log_GetCount(void);

/** 追加一条记录：0 = 成功；1 = 已写满；其他 = Flash 错误码。 */
uint8_t Log_Append(const char *text);

/** 读取第 index 条记录（64 字节到 rec），校验不过返回错误。 */
uint8_t Log_ReadAt(uint16_t index, uint8_t *rec);

/** 把记录里的正文取出来并补 '\0'，方便打印；返回正文长度。 */
uint8_t Log_TextOf(const uint8_t *rec, char *out, uint16_t out_size);

/** 清空日志区（擦除 4 个扇区），条数归零。 */
uint8_t Log_Clear(void);

#endif /* __LOG_H */
