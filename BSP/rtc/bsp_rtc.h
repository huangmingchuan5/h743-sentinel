/**
  ******************************************************************************
  * @file    bsp_rtc.h
  * @brief   Step 5.4：板载 RTC（LSE 32.768kHz，VBAT 掉电保持）。
  *
  *          设计：
  *            - 优先 LSE；LSE 起不来则退到 LSI（精度差，但仍是日历时间）；
  *            - 备份寄存器 BKP0R 存"已校时"魔数：上电读魔数判断日历是否可信；
  *              不可信时快照时间戳退化为"上电 tick(ms)"，由 flags bit15 区分；
  *            - 对外统一用"2000-01-01 起的秒数"（32 位，够用到 2136 年）。
  ******************************************************************************
  */

#ifndef __BSP_RTC_H
#define __BSP_RTC_H

#include "main.h"

#define RTC_EPOCH_YEAR   2000u    /* 时间戳零点：2000-01-01 00:00:00 */
#define RTC_MAGIC        0x32F2u  /* 备份寄存器里的"已校时"魔数 */

/** 调度器启动前调用（内部会等 LSE，依赖 HAL tick；必须在 BASEPRI 清理之后）。
  * @retval 0 = LSE 正常；1 = LSE 失败、已退到 LSI；2 = RTC 初始化失败 */
uint8_t Rtc_Init(void);

/** 1 = 用户校过时（魔数在，日历可信）；0 = 未校时（时间戳应退化为 tick）。 */
uint8_t Rtc_IsSet(void);

/** 读当前时间（2000-01-01 起的秒数）。1 = 成功；0 = 未校时/不可用。 */
uint8_t Rtc_GetEpoch(uint32_t *epoch2000);

/** 写入时间（2000-01-01 起的秒数）并置"已校时"魔数。1 = 成功。 */
uint8_t Rtc_SetEpoch(uint32_t epoch2000);

/** 用公历字段校时（内部换算成 epoch）。1 = 成功。 */
uint8_t Rtc_SetCivil(uint16_t y, uint8_t m, uint8_t d,
                     uint8_t hh, uint8_t mi, uint8_t ss);

/** 清除"已校时"魔数（测试回退用：之后时间戳改回上电 tick）。 */
void Rtc_ClearSet(void);

/** 把 epoch(2000) 格式化成 "YYYY-MM-DD hh:mm:ss"。buf 至少 20 字节。 */
void Rtc_FormatEpoch(uint32_t epoch2000, char *buf, uint16_t len);

#endif /* __BSP_RTC_H */
