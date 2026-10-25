/**
  ******************************************************************************
  * @file    bsp_rtc.c
  * @brief   板载 RTC 实现（见 bsp_rtc.h）。
  *
  *          说明：本工程 .ioc 在 Step 0 已冻结，RTC 没有经过 CubeMX 配置，
  *          这里手写一份等价于 CubeMX 生成代码的初始化流程（备份域/LSE/
  *          RTC 时钟源/分频/日历）。
  ******************************************************************************
  */

#include "bsp_rtc.h"
#include <stdio.h>

static RTC_HandleTypeDef s_hrtc;
static uint8_t  s_ready    = 0u;   /* 1 = RTC 初始化成功 */
static uint8_t  s_timeSet  = 0u;   /* 1 = 魔数在（日历可信） */
static uint32_t s_cached   = 0u;   /* 最近一次读到的 epoch(2000) */
static uint8_t  s_hasCache = 0u;

#define DAYS_1970_TO_2000   10957u   /* 2000-01-01 距 1970-01-01 的天数 */

/* ---- 日期 <-> 天数 换算（Howard Hinnant 的 civil 算法，纯整数） ---- */

/* 公历日期 → 距 1970-01-01 的天数 */
static uint32_t DaysFromCivil(uint16_t y, uint8_t m, uint8_t d)
{
  int32_t  yy  = (int32_t)y - ((m <= 2u) ? 1 : 0);
  int32_t  era = ((yy >= 0) ? yy : (yy - 399)) / 400;
  uint32_t yoe = (uint32_t)(yy - era * 400);
  uint32_t doy = (153u * (uint32_t)((int32_t)m + ((m > 2u) ? -3 : 9)) + 2u) / 5u + (uint32_t)d - 1u;
  uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;

  return (uint32_t)(era * 146097 + (int32_t)doe - 719468);
}

/* 距 1970-01-01 的天数 → 公历日期 */
static void CivilFromDays(uint32_t days1970, uint16_t *y, uint8_t *m, uint8_t *d)
{
  int32_t  z   = (int32_t)days1970 + 719468;
  int32_t  era = ((z >= 0) ? z : (z - 146096)) / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);
  uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  int32_t  yr  = (int32_t)yoe + era * 400;
  uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  uint32_t mp  = (5u * doy + 2u) / 153u;
  int32_t  month;

  *d    = (uint8_t)(doy - (153u * mp + 2u) / 5u + 1u);
  month = (int32_t)mp + ((mp < 10u) ? 3 : -9);
  *m    = (uint8_t)month;
  *y    = (uint16_t)(yr + ((month <= 2) ? 1 : 0));
}

static uint32_t EpochFromCivil(uint16_t y, uint8_t m, uint8_t d,
                               uint8_t hh, uint8_t mi, uint8_t ss)
{
  uint32_t days = DaysFromCivil(y, m, d) - DAYS_1970_TO_2000;

  return days * 86400u + (uint32_t)hh * 3600u + (uint32_t)mi * 60u + (uint32_t)ss;
}

static void CivilFromEpoch(uint32_t epoch, uint16_t *y, uint8_t *m, uint8_t *d,
                           uint8_t *hh, uint8_t *mi, uint8_t *ss)
{
  uint32_t days = epoch / 86400u;
  uint32_t rem  = epoch % 86400u;

  *hh  = (uint8_t)(rem / 3600u);
  rem %= 3600u;
  *mi  = (uint8_t)(rem / 60u);
  *ss  = (uint8_t)(rem % 60u);

  CivilFromDays(days + DAYS_1970_TO_2000, y, m, d);
}

/* HAL 的星期编号：1 = 周一 … 7 = 周日 */
static uint8_t WeekDayOf(uint32_t epoch)
{
  uint32_t days1970 = epoch / 86400u + DAYS_1970_TO_2000;

  return (uint8_t)(((days1970 + 3u) % 7u) + 1u);   /* 1970-01-01 是周四(4) */
}

/* -------------------------------------------------------------------------- */

uint8_t Rtc_Init(void)
{
  RCC_OscInitTypeDef       osc  = {0};
  RCC_PeriphCLKInitTypeDef pclk = {0};
  uint8_t lse_ok = 1u;

  HAL_PWR_EnableBkUpAccess();          /* 允许写备份域（LSE / RTC / 备份寄存器） */

  /* 1) 优先启动 LSE（32.768kHz 晶振） */
  osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
  osc.LSEState       = RCC_LSE_ON;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK)
  {
    lse_ok = 0u;

    /* 2) LSE 起不来 → 退到 LSI（内部 ~32kHz；精度差但日历仍可用） */
    osc.OscillatorType = RCC_OSCILLATORTYPE_LSI;
    osc.LSIState       = RCC_LSI_ON;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
    {
      return 2u;                       /* RTC 不可用：时间戳退化为 tick */
    }
  }

  /* 3) 选择 RTC 时钟源并使能 RTC 总线时钟 */
  pclk.PeriphClockSelection = RCC_PERIPHCLK_RTC;
  pclk.RTCClockSelection    = lse_ok ? RCC_RTCCLKSOURCE_LSE : RCC_RTCCLKSOURCE_LSI;
  if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK)
  {
    return 2u;
  }
  __HAL_RCC_RTC_ENABLE();

  /* 4) RTC 初始化：1Hz 计数（LSE: 128×256=32768；LSI: 128×250≈32000） 
  * 		RTC 的分频公式为：
  *			f_ck_spre = f_rtc_clk / ((AsynchPrediv + 1) × (SynchPrediv + 1))
  */
  s_hrtc.Instance            = RTC;
  s_hrtc.Init.HourFormat     = RTC_HOURFORMAT_24;
  s_hrtc.Init.AsynchPrediv   = 127u;
  s_hrtc.Init.SynchPrediv    = (lse_ok != 0u) ? 255u : 249u;
  //是否输出、映射到哪个引脚、极性、输出类型
  s_hrtc.Init.OutPut         = RTC_OUTPUT_DISABLE;
  s_hrtc.Init.OutPutRemap    = RTC_OUTPUT_REMAP_NONE;
  s_hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;		//高电平有效
  s_hrtc.Init.OutPutType     = RTC_OUTPUT_TYPE_OPENDRAIN;		//设置输出引脚是PP还是OD
  if (HAL_RTC_Init(&s_hrtc) != HAL_OK)
  {
    return 2u;
  }

  s_ready   = 1u;		//标记RTC初始化完成
  //从 RTC 备份寄存器 DR0 读出值，若等于约定的魔数 RTC_MAGIC，则把 s_timeSet 置 1（表示时间已设置过、有效），否则置 0（表示首次上电/时间无效）
  s_timeSet = (HAL_RTCEx_BKUPRead(&s_hrtc, RTC_BKP_DR0) == RTC_MAGIC) ? 1u : 0u;		//备份寄存器（掉电不丢失）

  return (lse_ok != 0u) ? 0u : 1u;
}

uint8_t Rtc_IsSet(void)
{
	return s_timeSet;		//1-->表示已经timeset; 0-->表示没有timeSet
}

uint8_t Rtc_GetEpoch(uint32_t *epoch2000)
{
  RTC_TimeTypeDef t = {0};
  RTC_DateTypeDef d = {0};

  if ((s_ready == 0u) || (s_timeSet == 0u) || (epoch2000 == NULL))
  {
    return 0u;
  }

  /* 读日历（HAL 内部有锁：和 time set 并发时可能短暂 BUSY，
     这时沿用上次缓存，避免同一批快照的时间戳语义跳变） */
  if ((HAL_RTC_GetTime(&s_hrtc, &t, RTC_FORMAT_BIN) == HAL_OK) &&
      (HAL_RTC_GetDate(&s_hrtc, &d, RTC_FORMAT_BIN) == HAL_OK))
  {
    s_cached   = EpochFromCivil((uint16_t)(RTC_EPOCH_YEAR + d.Year), d.Month, d.Date,
                                t.Hours, t.Minutes, t.Seconds);
    s_hasCache = 1u;
  }

  if (s_hasCache == 0u)
  {
    return 0u;
  }

  *epoch2000 = s_cached;
  return 1u;
}

uint8_t Rtc_SetEpoch(uint32_t epoch2000)
{
  RTC_TimeTypeDef t = {0};
  RTC_DateTypeDef d = {0};
  uint16_t y;
  uint8_t  m;
  uint8_t  dd;
  uint8_t  hh;
  uint8_t  mi;
  uint8_t  ss;

  if (s_ready == 0u)
  {
    return 0u;
  }

  CivilFromEpoch(epoch2000, &y, &m, &dd, &hh, &mi, &ss);

  t.Hours          = hh;
  t.Minutes        = mi;
  t.Seconds        = ss;
  t.TimeFormat     = RTC_HOURFORMAT_24;
  t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  t.StoreOperation = RTC_STOREOPERATION_RESET;

  d.WeekDay = WeekDayOf(epoch2000);
  d.Month   = m;
  d.Date    = dd;
  d.Year    = (uint8_t)(y - RTC_EPOCH_YEAR);

  if (HAL_RTC_SetTime(&s_hrtc, &t, RTC_FORMAT_BIN) != HAL_OK)
  {
    return 0u;
  }
  if (HAL_RTC_SetDate(&s_hrtc, &d, RTC_FORMAT_BIN) != HAL_OK)
  {
    return 0u;
  }

  /* 写入"已校时"魔数（VBAT 掉电保持；电池丢失则下次上电自动回退到 tick） */
  HAL_RTCEx_BKUPWrite(&s_hrtc, RTC_BKP_DR0, RTC_MAGIC);

  s_timeSet  = 1u;
  s_cached   = epoch2000;
  s_hasCache = 1u;
  return 1u;
}

uint8_t Rtc_SetCivil(uint16_t y, uint8_t m, uint8_t d,
                     uint8_t hh, uint8_t mi, uint8_t ss)
{
  if ((y < RTC_EPOCH_YEAR) || (y > (RTC_EPOCH_YEAR + 99u)) ||
      (m < 1u) || (m > 12u) || (d < 1u) || (d > 31u) ||
      (hh > 23u) || (mi > 59u) || (ss > 59u))
  {
    return 0u;
  }

  return Rtc_SetEpoch(EpochFromCivil(y, m, d, hh, mi, ss));
}

void Rtc_ClearSet(void)
{
  if (s_ready != 0u)
  {
    HAL_RTCEx_BKUPWrite(&s_hrtc, RTC_BKP_DR0, 0u);
  }

  s_timeSet  = 0u;
  s_hasCache = 0u;
}

void Rtc_FormatEpoch(uint32_t epoch2000, char *buf, uint16_t len)
{
  uint16_t y;
  uint8_t  m;
  uint8_t  d;
  uint8_t  hh;
  uint8_t  mi;
  uint8_t  ss;

  if ((buf == NULL) || (len < 20u))
  {
    return;
  }

  CivilFromEpoch(epoch2000, &y, &m, &d, &hh, &mi, &ss);
  (void)snprintf(buf, len, "%04u-%02u-%02u %02u:%02u:%02u",
                 (unsigned)y, (unsigned)m, (unsigned)d,
                 (unsigned)hh, (unsigned)mi, (unsigned)ss);
}
