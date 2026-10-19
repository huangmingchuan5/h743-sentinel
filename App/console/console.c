/**
  ******************************************************************************
  * @file    console.c
  * @brief   P0 记录仪的命令控制台（Step 4：已任务化）。
  *
  *          读取层：USART1 中断 → 256 B 环形缓冲 → 通知 ConsoleTask 来取
  *          行处理层：回显、退格、80 字节行缓冲、CR / CRLF / LF、超长丢弃
  *          命令层：help / color / pwm / info / save / load / log / dump / clear
  *
  *          输出统一走 Uart_Printf()（互斥量 + 中断发送），多任务同时打印
  *          不会互相插花；printf 只留给调度器启动前的代码。
  ******************************************************************************
*/

#include "console.h"
#include "bsp_uart.h"
#include "bsp_led.h"
#include "bsp_ee.h"
#include "config.h"
#include "log.h"
#include "bsp_i2c_bus.h"
#include "bsp_ap3216c.h"
#include "bsp_mpu6050.h"
#include "sample_task.h"
#include "bsp_rtc.h"
#include "alarm.h"
#include "ui_task.h"
#include "health.h"
#include <stdio.h>

#define CONSOLE_LINE_MAX         80u   /* 行缓冲大小，包含 '\0'（log 要放 60 字节正文） */

/* -------------------------------------------------------------------------- */
/* 私有变量                                                                   */

static char     s_line[CONSOLE_LINE_MAX];
static uint16_t s_len;
static uint8_t  s_dropping;   /* 1 = 行太长，在回车之前忽略输入              */
static uint8_t  s_skip_lf;    /* 1 = 期待 CRLF 中的那个 '\n'                 */

/* 系统状态。Day 2 存进 EEPROM，Day 4 用来驱动 LED。 */
static uint8_t  s_color      = COLOR_GREEN;
static uint8_t  s_brightness = 100u;

/* -------------------------------------------------------------------------- */
/* 私有函数声明                                                               */

static void        Prompt(void);
static void        Line_Execute(void);
static void        Dispatch(char *cmd, char *arg);
static void        Help_Print(void);
static void        Cmd_Color(const char *arg);
static void        Cmd_Pwm(const char *arg);
static void        Cmd_Save(const char *arg);
static void        Cmd_Load(const char *arg);
static void        Cmd_Info(const char *arg);
static void        Cmd_Dump(const char *arg);
static void        Cmd_Erase(const char *arg);
static void        Cmd_I2c(const char *arg);
static const char *I2c_AddrName(uint8_t addr7);
static void        Cmd_Sensor(const char *arg);
static void        Cmd_Live(const char *arg);
static void        Cmd_Time(const char *arg);
static void        Cmd_Qtest(const char *arg);
static void        Cmd_Alarm(const char *arg);
static void        Cmd_Cfg(const char *arg);
static void        Cmd_Status(const char *arg);
static void        Cmd_Wd(const char *arg);
static void        Cmd_LogPause(const char *arg);
static uint8_t     ParseTwoU32(const char *s, uint32_t *a, uint32_t *b);
static void        Trim_Tail(char *s);
static char        Lower(char c);
static uint8_t     Str_EqualNoCase(const char *a, const char *b);
static uint8_t     Parse_U32(const char *s, uint32_t *out);
static const char *Color_Name(uint8_t color);

/* -------------------------------------------------------------------------- */
/* 辅助函数                                                                   */

static void Prompt(void)
{
  Uart_Printf("> ");
}

static char Lower(char c)
{
  if (c >= 'A' && c <= 'Z')
  {
    return (char)(c - 'A' + 'a');
  }
  return c;
}

static uint8_t Str_EqualNoCase(const char *a, const char *b)
{
  while (*a != '\0' && *b != '\0')
  {
    if (Lower(*a) != Lower(*b))
    {
      return 0u;
    }
    a++;
    b++;
  }
  return (uint8_t)((*a == '\0') && (*b == '\0'));
}

/* 严格的十进制解析：只能是数字，不允许符号，也不允许尾部有多余字符 */
static uint8_t Parse_U32(const char *s, uint32_t *out)
{
  uint32_t v = 0u;

  if (*s == '\0')
  {
    return 0u;
  }

  while (*s != '\0')
  {
    if (*s < '0' || *s > '9')
    {
      return 0u;
    }
    if (v > 429496728u)          /* v * 10 + 9 会超出 32 位范围 */
    {
      return 0u;
    }
    v = (v * 10u) + (uint32_t)(*s - '0');
    s++;
  }

  *out = v;
  return 1u;
}

static void Trim_Tail(char *s)
{
  char *end = s;

  while (*end != '\0')
  {
    end++;
  }
  while (end > s && *(end - 1) == ' ')
  {
    end--;
  }
  *end = '\0';
}

static const char *Color_Name(uint8_t color)
{
  switch (color)
  {
    case COLOR_RED:   return "red";
    case COLOR_GREEN: return "green";
    case COLOR_BLUE:  return "blue";
    case COLOR_WHITE: return "white";
    case COLOR_OFF:   return "off";
    default:          return "?";
  }
}

/* -------------------------------------------------------------------------- */
/* 命令                                                                       */

static void Help_Print(void)
{
  Uart_Printf("Commands (case insensitive):\r\n");
  Uart_Printf("  help                            show this list\r\n");
  Uart_Printf("  color red|green|blue|white|off  set the LED colour\r\n");
  Uart_Printf("  pwm 0..100                      set the brightness in percent\r\n");
  Uart_Printf("  info                            show state + raw EEPROM bytes\r\n");
  Uart_Printf("  save | load                     store / restore the config\r\n");
  Uart_Printf("  dump [n|all]                    print snapshot log (newest n; default 20)\r\n");
  Uart_Printf("  erase                           erase the snapshot log (logical reset)\r\n");
  Uart_Printf("  i2c scan                        probe the I2C bus and list devices\r\n");
  Uart_Printf("  sensor                          one-shot read of ALS/PS/accelerometer\r\n");
  Uart_Printf("  live [n]                        print the latest 20Hz sample (n times)\r\n");
  Uart_Printf("  time [set YYYY-MM-DD hh:mm:ss]  show / set the RTC calendar\r\n");
  Uart_Printf("  qtest                           QSPI 32MB R/W test at the 16MB boundary\r\n");
  Uart_Printf("  alarm [clear|mute|unmute]       show / control the alarm\r\n");
  Uart_Printf("  cfg [show|save|load|reset|...]  configuration (thr, rate)\r\n");
  Uart_Printf("  status                          health / reset / alarm / log summary\r\n");
  Uart_Printf("  wd test <s>                     stop feeding IWDG (demo reset)\r\n");
  Uart_Printf("  logpause on|off                 pause logger (debug: fill logQueue)\r\n");
}

static void Cmd_Color(const char *arg)
{
  if (*arg == '\0')
  {
    Uart_Printf("ERR usage: color red|green|blue|white|off\r\n");
    return;
  }

  if (Str_EqualNoCase(arg, "red") != 0u)
  {
    s_color = COLOR_RED;
  }
  else if (Str_EqualNoCase(arg, "green") != 0u)
  {
    s_color = COLOR_GREEN;
  }
  else if (Str_EqualNoCase(arg, "blue") != 0u)
  {
    s_color = COLOR_BLUE;
  }
  else if (Str_EqualNoCase(arg, "white") != 0u)
  {
    s_color = COLOR_WHITE;
  }
  else if (Str_EqualNoCase(arg, "off") != 0u)
  {
    s_color = COLOR_OFF;
  }
  else
  {
    Uart_Printf("ERR unknown color '%s'\r\n", arg);
    Uart_Printf("    try: red|green|blue|white|off\r\n");
    return;
  }

  Led_SetColor((App_Color_t)s_color);              /* 真正去点灯 */
  Uart_Printf("OK  color = %s\r\n", Color_Name(s_color));
}

static void Cmd_Pwm(const char *arg)
{
  uint32_t value = 0u;

  if (*arg == '\0')
  {
    Uart_Printf("ERR usage: pwm 0..100\r\n");
    return;
  }

  if (Parse_U32(arg, &value) == 0u)
  {
    Uart_Printf("ERR not a number: '%s'\r\n", arg);
    return;
  }

  if (value > 100u)
  {
    Uart_Printf("ERR brightness %lu out of range 0..100\r\n", (unsigned long)value);
    return;
  }

  Console_SetBrightness((uint8_t)value);   /* 亮度转给 LED 模块，呼吸幅度立刻跟着变 */
  Uart_Printf("OK  brightness = %u%%\r\n", (unsigned)Console_GetBrightness());
}

/* save：把当前 {颜色, 亮度, 日志条数} 写进 EEPROM */
static void Cmd_Save(const char *arg)
{
  uint8_t st;

  (void)arg;                            /* save 不带参数 */

  st = Config_Save();
  if (st == EE_OK)
  {
    Uart_Printf("OK  saved: color=%u brightness=%u rate=%ums thr=%u/%u\r\n",
                (unsigned)Console_GetColor(), (unsigned)Console_GetBrightness(),
                (unsigned)Config_GetRateMs(),
                (unsigned)Config_GetThrLight(), (unsigned)Config_GetThrMotion());
  }
  else
  {
    Uart_Printf("ERR EEPROM write failed (0x%02X)\r\n", (unsigned)st);
  }
}

/* load：从 EEPROM 读回并立即生效 */
static void Cmd_Load(const char *arg)
{
  uint8_t st;

  (void)arg;

  st = Config_Load();
  if (st == 0u)
  {
    Uart_Printf("OK  loaded: color=%u brightness=%u\r\n",
           (unsigned)Console_GetColor(), (unsigned)Console_GetBrightness());
  }
  else if (st == 1u)
  {
    Uart_Printf("OK  no valid config, defaults written\r\n");
  }
  else
  {
    Uart_Printf("ERR EEPROM read failed (0x%02X)\r\n", (unsigned)st);
  }
}

/* info：打印当前状态 + EEPROM 里那 8 字节的原始内容 */
static void Cmd_Info(const char *arg)
{
  uint8_t b[CFG_SIZE];
  uint8_t i;
  uint8_t st;

  (void)arg;

  Uart_Printf("color      = %s\r\n", Color_Name((uint8_t)Console_GetColor()));
  Uart_Printf("brightness = %u %%\r\n", (unsigned)Console_GetBrightness());
  Uart_Printf("rate       = %u ms\r\n", (unsigned)Config_GetRateMs());
  Uart_Printf("threshold  = light %u / motion %u\r\n",
              (unsigned)Config_GetThrLight(), (unsigned)Config_GetThrMotion());
  Uart_Printf("log count  = %lu (available %lu)\r\n",
              (unsigned long)Log_GetCount(), (unsigned long)Log_GetAvailable());

  st = Ee_Read(CFG_ADDR, b, CFG_SIZE);
  if (st == EE_OK)
  {
    Uart_Printf("EEPROM[0..15] =");
    for (i = 0u; i < CFG_SIZE; i++)
    {
      Uart_Printf(" %02X", (unsigned)b[i]);
    }
    Uart_Printf("\r\n");
  }
  else
  {
    Uart_Printf("ERR EEPROM read failed (0x%02X)\r\n", (unsigned)st);
  }
}

/* dump [n|all]：按时间顺序打印快照日志（默认最近 20 条）。
   分块读取（一次 8 条）——读锁不跨输出。 */
static void Cmd_Dump(const char *arg)
{
  uint32_t avail = Log_GetAvailable();
  uint32_t count;
  uint32_t first;
  uint32_t done;
  Sample_t buf[8];

  if (avail == 0u)
  {
    Uart_Printf("(no records)\r\n");
    return;
  }

  if (*arg == '\0')
  {
    count = 20u;
  }
  else if (Str_EqualNoCase(arg, "all") != 0u)
  {
    count = avail;
  }
  else if ((Parse_U32(arg, &count) == 0u) || (count == 0u))
  {
    Uart_Printf("ERR usage: dump [n|all]\r\n");
    return;
  }

  if (count > avail)
  {
    count = avail;
  }
  first = avail - count;                 /* 默认显示最近 count 条 */

  if ((count >= avail) && (count > 200u))
  {
    Uart_Printf("dump: warning - 'all' may take a long time\r\n");
  }

  Uart_Printf("dump: %lu available, showing %lu (from index %lu)\r\n",
              (unsigned long)avail, (unsigned long)count, (unsigned long)first);

  for (done = 0u; done < count; )
  {
    uint32_t n = count - done;
    uint32_t k;
    uint8_t  st;

    if (n > 8u)
    {
      n = 8u;
    }

    st = Log_FillDump(first + done, n, buf);
    if (st != 0u)
    {
      Uart_Printf("ERR log read failed (%u) at index %lu\r\n",
                  (unsigned)st, (unsigned long)(first + done));
      return;
    }

    for (k = 0u; k < n; k++)
    {
      char tsbuf[24];

      if ((buf[k].flags & SAMPLE_F_TS_REAL) != 0u)
      {
        Rtc_FormatEpoch(buf[k].tstamp, tsbuf, (uint16_t)sizeof(tsbuf));
      }
      else
      {
        (void)snprintf(tsbuf, sizeof(tsbuf), "t=%lums", (unsigned long)buf[k].tstamp);
      }

      Uart_Printf("#%lu %s als=%u ps=%u near=%u acc=(%d,%d,%d) f=0x%04X\r\n",
                  (unsigned long)buf[k].seq,
                  tsbuf,
                  (unsigned)buf[k].als,
                  (unsigned)(buf[k].ps & 0x03FFu),
                  (unsigned)((buf[k].ps >> 15) & 1u),
                  (int)buf[k].ax, (int)buf[k].ay, (int)buf[k].az,
                  (unsigned)buf[k].flags);
    }

    done += n;
  }

  Uart_Printf("dump: done (%lu record(s))\r\n", (unsigned long)count);
}

/* erase（clear 为旧名别名）：逻辑擦除日志——只重置元数据与写指针，
   不整片擦 32MB（秒级完成），旧数据随后被新数据逐步覆盖。 */
static void Cmd_Erase(const char *arg)
{
  (void)arg;

  if (Log_Erase() != LOG_OK)
  {
    Uart_Printf("ERR log erase failed\r\n");
    return;
  }

  if (Config_Save() != EE_OK)
  {
    Uart_Printf("WARN log erased, but config write failed\r\n");
  }

  Uart_Printf("OK    : log erased (seq reset, ring starts empty)\r\n");
}

/* i2c scan：探测 0x08..0x77 所有 7 位地址，列出有应答的器件。
 * 先扫完、再打印——总线锁不跨输出。 */
static void Cmd_I2c(const char *arg)
{
  uint8_t found[16];
  uint8_t n = 0u;
  uint8_t addr;
  uint8_t i;

  if (Str_EqualNoCase(arg, "scan") == 0u)
  {
    Uart_Printf("ERR usage: i2c scan\r\n");
    return;
  }

  Uart_Printf("i2c scan: probing 0x08..0x77 ...\r\n");

  for (addr = 0x08u; addr <= 0x77u; addr++)
  {
    if ((I2cBus_Probe(addr) != 0u) && (n < (uint8_t)sizeof(found)))
    {
      found[n] = addr;
      n++;
    }
  }

  Uart_Printf("i2c scan: %u device(s) found\r\n", (unsigned)n);
  for (i = 0u; i < n; i++)
  {
    Uart_Printf("  0x%02X  %s\r\n", (unsigned)found[i], I2c_AddrName(found[i]));
  }
  if (n == 0u)
  {
    Uart_Printf("  (none - check PB8/PB9 wiring and pull-ups)\r\n");
  }
}

/* 常见地址的器件名（未知地址显示 "-"） */
static const char *I2c_AddrName(uint8_t addr7)
{
  switch (addr7)
  {
    case 0x50u: return "EEPROM (AT24C02)";
    case 0x1Eu: return "AP3216C (light/proximity)";
    case 0x68u: return "MPU6050 (accel/gyro)";
    default:    return "-";
  }
}

/* sensor：把三个 I2C 器件的实时值各读一次（Step 5.2 数据通路验证） */
static void Cmd_Sensor(const char *arg)
{
  uint16_t als = 0u;
  uint16_t ps  = 0u;
  int16_t  ax = 0;
  int16_t  ay = 0;
  int16_t  az = 0;
  uint8_t  st;

  (void)arg;

  /* 环境光：原始 counts；按默认量程 0.35 lux/count 折算一个参考值 */
  st = Ap3216c_ReadAls(&als);
  if (st == 0u)
  {
    Uart_Printf("ALS : raw=%u  (~%lu lux)\r\n",
                (unsigned)als, (unsigned long)(((uint32_t)als * 35u) / 100u));
  }
  else
  {
    Uart_Printf("ALS : read failed (%u)\r\n", (unsigned)st);
  }

  /* 接近：低 10 位数据 + bit15 靠近标志；溢出时数据仅供参考 */
  st = Ap3216c_ReadPs(&ps);
  if (st == 0u)
  {
    Uart_Printf("PS  : %u  near=%u\r\n",
                (unsigned)(ps & 0x03FFu), (unsigned)((ps >> 15) & 1u));
  }
  else if (st == 1u)
  {
    Uart_Printf("PS  : %u  near=%u  (overflow flag)\r\n",
                (unsigned)(ps & 0x03FFu), (unsigned)((ps >> 15) & 1u));
  }
  else
  {
    Uart_Printf("PS  : read failed (%u)\r\n", (unsigned)st);
  }

  /* 加速度：±4g 量程，8192 LSB = 1g */
  st = Mpu6050_ReadAccel(&ax, &ay, &az);
  if (st == 0u)
  {
    Uart_Printf("ACC : x=%d y=%d z=%d  (raw LSB; 8192 LSB = 1g)\r\n",
                (int)ax, (int)ay, (int)az);
  }
  else
  {
    Uart_Printf("ACC : read failed (%u)\r\n", (unsigned)st);
  }
}

/* live [n]：打印"最新快照"（时间戳/光照/接近/加速度/新鲜度）。
   不带参数打一行；live 5 连续打 5 行（约 100ms 一行）。 */
static void Cmd_Live(const char *arg)
{
  uint32_t n = 1u;
  uint32_t i;

  if (*arg != '\0')
  {
    if ((Parse_U32(arg, &n) == 0u) || (n == 0u))
    {
      Uart_Printf("ERR usage: live [count 1..200]\r\n");
      return;
    }
    if (n > 200u)
    {
      n = 200u;
    }
  }

  for (i = 0u; i < n; i++)
  {
    Sample_t s;
    uint32_t age = 0u;
    char     tsbuf[24];

    if (Sample_GetLatest(&s, &age) != 0u)
    {
      Uart_Printf("live: no sample yet (report as bug if sampling tasks are running)\r\n");
      return;
    }

    if ((s.flags & SAMPLE_F_TS_REAL) != 0u)
    {
      Rtc_FormatEpoch(s.tstamp, tsbuf, (uint16_t)sizeof(tsbuf));
    }
    else
    {
      (void)snprintf(tsbuf, sizeof(tsbuf), "t=%lums", (unsigned long)s.tstamp);
    }

    Uart_Printf("[%s age=%lums] als=%u ps=%u near=%u acc=(%d,%d,%d) flags=0x%04X q=%u/%u drop=%u\r\n",
                tsbuf,
                (unsigned long)age,
                (unsigned)s.als,
                (unsigned)(s.ps & 0x03FFu),
                (unsigned)((s.ps >> 15) & 1u),
                (int)s.ax, (int)s.ay, (int)s.az,
                (unsigned)s.flags,
                (unsigned)Sample_QueueCount(), (unsigned)SAMPLE_QUEUE_LEN,
                (unsigned)Sample_DropCount());

    if ((i + 1u) < n)
    {
      vTaskDelay(pdMS_TO_TICKS(100));   /* 连续打印时约 100ms 一行 */
    }
  }
}



/* ---- time 命令用的小工具 ---- */

static uint8_t Digits2(const char *s, uint8_t *v)
{
  if ((s[0] < '0') || (s[0] > '9') || (s[1] < '0') || (s[1] > '9'))
  {
    return 0u;
  }
  *v = (uint8_t)((s[0] - '0') * 10 + (s[1] - '0'));
  return 1u;
}

static uint8_t Digits4(const char *s, uint16_t *v)
{
  if ((s[0] < '0') || (s[0] > '9') || (s[1] < '0') || (s[1] > '9') ||
      (s[2] < '0') || (s[2] > '9') || (s[3] < '0') || (s[3] > '9'))
  {
    return 0u;
  }
  *v = (uint16_t)(((s[0] - '0') * 1000) + ((s[1] - '0') * 100) +
                  ((s[2] - '0') * 10) + (s[3] - '0'));
  return 1u;
}

static uint8_t DaysInMonth(uint16_t y, uint8_t m)
{
  static const uint8_t dim[12] = {31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u};
  uint8_t d = dim[m - 1u];

  if ((m == 2u) &&
      (((y % 4u) == 0u) && (((y % 100u) != 0u) || ((y % 400u) == 0u))))
  {
    d = 29u;
  }
  return d;
}

/* time：查看 / 设置 RTC 日历
   time                          → 显示当前日历与状态
   time set YYYY-MM-DD hh:mm:ss  → 校时（并置"已校时"魔数）
   time clear                    → 清除魔数（测试回退：时间戳改用上电 tick） */
static void Cmd_Time(const char *arg)
{
  if (*arg == '\0')
  {
    uint32_t ep = 0u;

    if ((Rtc_IsSet() != 0u) && (Rtc_GetEpoch(&ep) != 0u))
    {
      char ts[24];

      Rtc_FormatEpoch(ep, ts, (uint16_t)sizeof(ts));
      Uart_Printf("time   : %s  (epoch2000=%lu, valid)\r\n", ts, (unsigned long)ep);
    }
    else
    {
      Uart_Printf("time   : not set (samples use boot tick, flags bit15=0)\r\n");
      Uart_Printf("         use: time set YYYY-MM-DD hh:mm:ss\r\n");
    }
    return;
  }

  if ((Lower(arg[0]) == 's') && (Lower(arg[1]) == 'e') && (Lower(arg[2]) == 't') &&
      (arg[3] == ' '))
  {
    const char *p = &arg[4];
    uint16_t y = 0u;
    uint8_t  mo = 0u;
    uint8_t  dd = 0u;
    uint8_t  hh = 0u;
    uint8_t  mi = 0u;
    uint8_t  ss = 0u;

    if ((p[4] == '-') && (p[7] == '-') && (p[10] == ' ') &&
        (p[13] == ':') && (p[16] == ':') && (p[19] == '\0') &&
        (Digits4(p, &y) != 0u) &&
        (Digits2(p + 5, &mo) != 0u) && (Digits2(p + 8, &dd) != 0u) &&
        (Digits2(p + 11, &hh) != 0u) && (Digits2(p + 14, &mi) != 0u) &&
        (Digits2(p + 17, &ss) != 0u))
    {
      if ((y < 2000u) || (y > 2099u) || (mo < 1u) || (mo > 12u) ||
          (dd < 1u) || (dd > DaysInMonth(y, mo)) ||
          (hh > 23u) || (mi > 59u) || (ss > 59u))
      {
        Uart_Printf("ERR   : date/time out of range\r\n");
        return;
      }

      if (Rtc_SetCivil(y, mo, dd, hh, mi, ss) != 0u)
      {
        Uart_Printf("OK    : time set to %04u-%02u-%02u %02u:%02u:%02u\r\n",
                    (unsigned)y, (unsigned)mo, (unsigned)dd,
                    (unsigned)hh, (unsigned)mi, (unsigned)ss);
      }
      else
      {
        Uart_Printf("ERR   : RTC write failed (is the RTC initialized?)\r\n");
      }
      return;
    }

    Uart_Printf("ERR usage: time set YYYY-MM-DD hh:mm:ss\r\n");
    return;
  }

  if (Str_EqualNoCase(arg, "clear") != 0u)
  {
    Rtc_ClearSet();
    Uart_Printf("OK    : RTC 'set' mark cleared (timestamps fall back to boot tick)\r\n");
    return;
  }

  Uart_Printf("ERR usage: time | time set YYYY-MM-DD hh:mm:ss | time clear\r\n");
}

/* qtest：QSPI 32MB 寻址标定——在 16MB 边界附近取 4 个地址做"擦-写-读回"。
   注意：会擦除这 4 个 4KB 扇区（都在日志区之外，不影响已有日志）。 */
static void Cmd_Qtest(const char *arg)
{
  static const uint32_t addrs[4] =
  {
    0x00F00000u,   /* 15MB：第一 bank 内 */
    0x00FFF000u,   /* 16MB 边界的前一扇区 */
    0x01000000u,   /* 16MB 边界后的第一扇区 */
    0x01800000u    /* 24MB：第二 bank 内 */
  };
  uint8_t  wr[FLASH_PAGE_SIZE];
  uint8_t  rd[FLASH_PAGE_SIZE];
  uint8_t  k;
  uint16_t i;

  (void)arg;

  Uart_Printf("qtest: erase+write+readback @ 4 addresses (~2-3 s)\r\n");

  for (k = 0u; k < 4u; k++)
  {
    uint32_t a = addrs[k];
    uint8_t  st;

    for (i = 0u; i < FLASH_PAGE_SIZE; i++)
    {
      wr[i] = (uint8_t)((uint16_t)i ^ 0xA5u);
    }

    st = Flash_EraseSector(a);
    if (st != FLASH_OK)
    {
      Uart_Printf("  0x%08lX  erase FAIL (%u)\r\n", (unsigned long)a, (unsigned)st);
      continue;
    }

    st = Flash_PageWrite(a, wr, FLASH_PAGE_SIZE);
    if (st != FLASH_OK)
    {
      Uart_Printf("  0x%08lX  write FAIL (%u)\r\n", (unsigned long)a, (unsigned)st);
      continue;
    }

    st = Flash_Read(a, rd, FLASH_PAGE_SIZE);
    if (st != FLASH_OK)
    {
      Uart_Printf("  0x%08lX  read FAIL (%u)\r\n", (unsigned long)a, (unsigned)st);
      continue;
    }

    for (i = 0u; i < FLASH_PAGE_SIZE; i++)
    {
      if (rd[i] != wr[i])
      {
        break;
      }
    }

    if (i == FLASH_PAGE_SIZE)		//说明全部匹配，不然上面break
    {
      Uart_Printf("  0x%08lX  OK\r\n", (unsigned long)a);
    }
    else
    {
      Uart_Printf("  0x%08lX  MISMATCH at +%u (wrote 0x%02X, read 0x%02X)\r\n",
                  (unsigned long)a, (unsigned)i, (unsigned)wr[i], (unsigned)rd[i]);
    }
  }

  Uart_Printf("qtest: done\r\n");
}

/* ---- 报警 / 配置 / 状态 / 看门狗 命令 ---- */

/* 解析"两个十进制数（空格分隔）" */
static uint8_t ParseTwoU32(const char *s, uint32_t *a, uint32_t *b)
{
  char     buf[16];
  uint16_t n = 0u;

  while (*s == ' ') { s++; }
  while ((*s >= '0') && (*s <= '9') && (n < (uint16_t)(sizeof(buf) - 1u)))
  {
    buf[n++] = *s++;
  }
  buf[n] = '\0';
  if ((n == 0u) || (Parse_U32(buf, a) == 0u)) { return 0u; }

  while (*s == ' ') { s++; }
  n = 0u;
  while ((*s >= '0') && (*s <= '9') && (n < (uint16_t)(sizeof(buf) - 1u)))
  {
    buf[n++] = *s++;
  }
  buf[n] = '\0';
  if ((n == 0u) || (Parse_U32(buf, b) == 0u)) { return 0u; }

  while (*s == ' ') { s++; }
  return (*s == '\0') ? 1u : 0u;
}

/* alarm：查看 / 清除 / 静音 */
static void Cmd_Alarm(const char *arg)
{
  if (*arg == '\0')
  {
    uint32_t lat = 0u;

    Uart_Printf("alarm : light=%u motion=%u muted=%u (thr light=%u motion=%u)\r\n",
                (unsigned)Alarm_LightActive(), (unsigned)Alarm_MotionActive(),
                (unsigned)Alarm_IsMuted(),
                (unsigned)Config_GetThrLight(), (unsigned)Config_GetThrMotion());
    if (Ui_GetAlarmLatency(&lat) != 0u)
    {
      Uart_Printf("alarm : last confirm->action = %lu ms\r\n", (unsigned long)lat);
    }
    return;
  }
  if (Str_EqualNoCase(arg, "clear") != 0u)
  {
    Alarm_Clear();
    Uart_Printf("OK    : alarm cleared\r\n");
    return;
  }
  if (Str_EqualNoCase(arg, "mute") != 0u)
  {
    Alarm_SetMuted(1u);
    Uart_Printf("OK    : buzzer muted\r\n");
    return;
  }
  if ((Str_EqualNoCase(arg, "unmute") != 0u) || (Str_EqualNoCase(arg, "resume") != 0u))
  {
    Alarm_SetMuted(0u);
    Uart_Printf("OK    : buzzer unmuted\r\n");
    return;
  }
  Uart_Printf("ERR usage: alarm [clear|mute|unmute]\r\n");
}

/* cfg：查看 / 保存 / 加载 / 恢复默认 / 修改阈值与采样周期 */
static void Cmd_Cfg(const char *arg)
{
  if ((*arg == '\0') || (Str_EqualNoCase(arg, "show") != 0u))
  {
    Uart_Printf("cfg   : color=%u brightness=%u rate=%ums thr_light=%u thr_motion=%u\r\n",
                (unsigned)Console_GetColor(), (unsigned)Console_GetBrightness(),
                (unsigned)Config_GetRateMs(),
                (unsigned)Config_GetThrLight(), (unsigned)Config_GetThrMotion());
    return;
  }

  if (Str_EqualNoCase(arg, "save") != 0u)
  {
    Uart_Printf((Config_Save() == EE_OK) ? "OK    : cfg saved\r\n"
                                         : "ERR   : EEPROM write failed\r\n");
    return;
  }

  if (Str_EqualNoCase(arg, "load") != 0u)
  {
    uint8_t st = Config_Load();

    if (st == 0u)      { Uart_Printf("OK    : cfg loaded\r\n"); }
    else if (st == 1u) { Uart_Printf("OK    : no valid cfg, defaults written\r\n"); }
    else               { Uart_Printf("ERR   : EEPROM read failed (0x%02X)\r\n", (unsigned)st); }
    return;
  }

  if (Str_EqualNoCase(arg, "reset") != 0u)
  {
    Uart_Printf((Config_Reset() == EE_OK) ? "OK    : defaults restored & saved\r\n"
                                          : "ERR   : EEPROM write failed\r\n");
    return;
  }

  if ((Lower(arg[0]) == 't') && (Lower(arg[1]) == 'h') && (Lower(arg[2]) == 'r') &&
      (arg[3] == ' '))
  {
    uint32_t a = 0u;
    uint32_t b = 0u;

    if ((ParseTwoU32(&arg[4], &a, &b) != 0u) && (a <= 65535u) && (b <= 65535u))
    {
      Config_SetThrLight((uint16_t)a);
      Config_SetThrMotion((uint16_t)b);
      Uart_Printf("OK    : thr set (run 'cfg save' to keep)\r\n");
    }
    else
    {
      Uart_Printf("ERR usage: cfg thr <light> <motion>\r\n");
    }
    return;
  }

  if ((Lower(arg[0]) == 'r') && (Lower(arg[1]) == 'a') && (Lower(arg[2]) == 't') &&
      (Lower(arg[3]) == 'e') && (arg[4] == ' '))
  {
    uint32_t v = 0u;

    if ((Parse_U32(&arg[5], &v) != 0u) && (v >= 20u) && (v <= 200u))
    {
      Config_SetRateMs((uint8_t)v);
      Uart_Printf("OK    : rate=%u ms (run 'cfg save' to keep)\r\n", (unsigned)v);
    }
    else
    {
      Uart_Printf("ERR usage: cfg rate <20..200>\r\n");
    }
    return;
  }

  Uart_Printf("ERR usage: cfg [show|save|load|reset|thr <light> <motion>|rate <ms>]\r\n");
}

/* status：系统状态一屏（健康/复位原因/报警/日志/RTC） */
static void Cmd_Status(const char *arg)
{
  uint32_t rsr;
  uint32_t ep = 0u;
  char     ts[24];

  (void)arg;

  rsr = Health_GetResetFlags();
  Uart_Printf("status : up=%lu ms  health=%u  last_fail=%u\r\n",
              (unsigned long)xTaskGetTickCount(),
              (unsigned)Health_GetHealthy(), (unsigned)Health_GetLastFail());
  Uart_Printf("status : reset flags=0x%08lX", (unsigned long)rsr);
  if ((rsr & RCC_RSR_PORRSTF)   != 0u) { Uart_Printf(" POR"); }
  if ((rsr & RCC_RSR_BORRSTF)   != 0u) { Uart_Printf(" BOR"); }
  if ((rsr & RCC_RSR_PINRSTF)   != 0u) { Uart_Printf(" PIN"); }
  if ((rsr & RCC_RSR_SFTRSTF)   != 0u) { Uart_Printf(" SFT"); }
  if ((rsr & RCC_RSR_IWDG1RSTF) != 0u) { Uart_Printf(" IWDG"); }
  if ((rsr & RCC_RSR_WWDG1RSTF) != 0u) { Uart_Printf(" WWDG"); }
  if ((rsr & RCC_RSR_LPWRRSTF) != 0u) { Uart_Printf(" LPWR"); }
  Uart_Printf("\r\n");
  Uart_Printf("status : alarm light=%u motion=%u muted=%u\r\n",
              (unsigned)Alarm_LightActive(), (unsigned)Alarm_MotionActive(),
              (unsigned)Alarm_IsMuted());
  Uart_Printf("status : log count=%lu avail=%lu err=%u q=%u qpeak=%u drop=%u rxdrop=%u\r\n",
              (unsigned long)Log_GetCount(), (unsigned long)Log_GetAvailable(),
              (unsigned)Log_GetWriteErrors(),
              (unsigned)Sample_QueueCount(), (unsigned)Sample_QueuePeak(),
              (unsigned)Sample_DropCount(),
              (unsigned)Uart_RxDropped());
  if ((Rtc_IsSet() != 0u) && (Rtc_GetEpoch(&ep) != 0u))
  {
    Rtc_FormatEpoch(ep, ts, (uint16_t)sizeof(ts));
    Uart_Printf("status : rtc %s\r\n", ts);
  }
  else
  {
    Uart_Printf("status : rtc not set (tick timestamps)\r\n");
  }
}

/* wd test <s>：停止喂狗 s 秒，演示 IWDG 复位（约 2s 后复位） */
static void Cmd_Wd(const char *arg)
{
  uint32_t s = 0u;

  if ((Lower(arg[0]) == 't') && (Lower(arg[1]) == 'e') &&
      (Lower(arg[2]) == 's') && (Lower(arg[3]) == 't') && (arg[4] == ' ') &&
      (Parse_U32(&arg[5], &s) != 0u) && (s >= 1u) && (s <= 60u))
  {
    Health_SetWdgTest(s);
    Uart_Printf("WARN  : IWDG feeding stopped for %lu s (reset in ~2s)\r\n",
                (unsigned long)s);
    return;
  }

  Uart_Printf("ERR usage: wd test <1..60>\r\n");
}

/* logpause on|off：暂停/恢复日志消费（测试 evtQueue 优先级用） */
static void Cmd_LogPause(const char *arg)
{
  if (Str_EqualNoCase(arg, "on") != 0u)
  {
    Log_SetPaused(1u);
    Uart_Printf("OK    : logging paused (logQueue will fill)\r\n");
    return;
  }
  if (Str_EqualNoCase(arg, "off") != 0u)
  {
    Log_SetPaused(0u);
    Uart_Printf("OK    : logging resumed\r\n");
    return;
  }
  Uart_Printf("ERR usage: logpause on|off\r\n");
}

//匹配并执行相对应命令函数
static void Dispatch(char *cmd, char *arg)
{
  if (Str_EqualNoCase(cmd, "help") != 0u)
  {
    Help_Print();
  }
  else if (Str_EqualNoCase(cmd, "color") != 0u)
  {
    Cmd_Color(arg);
  }
  else if (Str_EqualNoCase(cmd, "pwm") != 0u)
  {
    Cmd_Pwm(arg);
  }
  else if (Str_EqualNoCase(cmd, "save") != 0u)
  {
    Cmd_Save(arg);
  }
  else if (Str_EqualNoCase(cmd, "load") != 0u)
  {
    Cmd_Load(arg);
  }
  else if (Str_EqualNoCase(cmd, "info") != 0u)
  {
    Cmd_Info(arg);
  }
  else if (Str_EqualNoCase(cmd, "dump") != 0u)
  {
    Cmd_Dump(arg);
  }
  else if ((Str_EqualNoCase(cmd, "erase") != 0u) || (Str_EqualNoCase(cmd, "clear") != 0u))
  {
    Cmd_Erase(arg);
  }
  else if (Str_EqualNoCase(cmd, "i2c") != 0u)
  {
    Cmd_I2c(arg);
  }
  else if (Str_EqualNoCase(cmd, "sensor") != 0u)
  {
    Cmd_Sensor(arg);
  }
  else if (Str_EqualNoCase(cmd, "live") != 0u)
  {
    Cmd_Live(arg);
  }
  else if (Str_EqualNoCase(cmd, "time") != 0u)
  {
    Cmd_Time(arg);
  }
  else if (Str_EqualNoCase(cmd, "qtest") != 0u)
  {
    Cmd_Qtest(arg);
  }
  else if (Str_EqualNoCase(cmd, "alarm") != 0u)
  {
    Cmd_Alarm(arg);
  }
  else if (Str_EqualNoCase(cmd, "cfg") != 0u)
  {
    Cmd_Cfg(arg);
  }
  else if (Str_EqualNoCase(cmd, "status") != 0u)
  {
    Cmd_Status(arg);
  }
  else if (Str_EqualNoCase(cmd, "wd") != 0u)
  {
    Cmd_Wd(arg);
  }
  else if (Str_EqualNoCase(cmd, "logpause") != 0u)
  {
    Cmd_LogPause(arg);
  }
  else
  {
    Uart_Printf("ERR unknown command '%s', type 'help'\r\n", cmd);
  }
}

/* -------------------------------------------------------------------------- */
/* 行处理层                                                                   */

/* 把 "  cmd   arg  " 拆成命令和参数并执行 */
static void Line_Execute(void)
{
  char *cmd;
  char *arg;

  if (s_dropping != 0u)              /* 这一行已被截断，直接丢弃 */
  {
    s_dropping = 0u;
    s_len      = 0u;
    return;
  }

  s_line[s_len] = '\0';
  s_len         = 0u;

  cmd = s_line;
  while (*cmd == ' ')
  {
    cmd++;                           /* 跳过前导空格 */
  }

  arg = cmd;
  while (*arg != '\0' && *arg != ' ')
  {
    arg++;                           /* 找到命令单词的结尾 */
  }

  if (*arg != '\0')
  {
    *arg = '\0';                     /* 在这里切断命令单词 */
    arg++;
    while (*arg == ' ')
    {
      arg++;                         /* 跳过参数前面的空格 */
    }
  }

  Trim_Tail(arg);

  if (*cmd == '\0')                  /* 空行，或者只有空格 */
  {
    return;
  }

  Dispatch(cmd, arg);
}

void Console_Init(void)
{
  s_len      = 0u;
  s_dropping = 0u;
  s_skip_lf  = 0u;
  Led_SetColor((App_Color_t)s_color);              /* 上电默认颜色（绿）立即生效 */
  /* 本函数在调度器启动前调用：RTOS 还没跑起来，不能用 Uart_Printf，继续用阻塞 printf */
  printf("> ");
}

/* 处理"已经收到的"**一个字节（原 Console_Task 里的逐字节逻辑，原样保留） */
static void Console_FeedByte(int c)
{
  if (c == '\n' && s_skip_lf != 0u)  /* "\r\n" 中的那个 '\n' */
  {
    s_skip_lf = 0u;
    return;
  }
  s_skip_lf = 0u;

  if (c == '\r' || c == '\n')        /* 行结束 */
  {
    if (c == '\r')
    {
      s_skip_lf = 1u;
    }
    Uart_Printf("\r\n");
    Line_Execute();
    Prompt();
    return;
  }

  if (c == 0x08 || c == 0x7F)        /* 退格或 Delete */
  {
    if (s_len > 0u)
    {
      s_len--;
      Uart_Printf("\b \b");          /* 在屏幕上擦掉这个字符 */
    }
    return;
  }

  if (c < 0x20 || c > 0x7E)          /* 忽略其它控制字符 */
  {
    return;
  }

  if (s_dropping != 0u)              /* 已经超长，等回车 */
  {
    return;
  }

  if (s_len >= CONSOLE_LINE_MAX - 1u)
  {
    s_dropping = 1u;
    Uart_Printf("\r\nERR line too long (max %u characters), press Enter\r\n",
                (unsigned)(CONSOLE_LINE_MAX - 1u));
    return;
  }

  s_line[s_len] = (char)c;
  s_len++;
  Uart_Putc((uint8_t)c);             /* 本地回显，让你看到自己输入的内容 */
}

/* Step 4：控制台任务。平时阻塞在任务通知上，醒来后把环形缓冲里的字节全部处理掉。 */
void ConsoleTask(void *argument)
{
  int c;

	//(void)argument 是为了消除“未使用参数”的编译警告。
  (void)argument;

  /* 绑定"收到字节后通知谁"，并启动接收中断（此刻调度器已经在跑） */
  Uart_RxStart(xTaskGetCurrentTaskHandle());

  for (;;)
  {
	  //这个任务本来就是一直等待RX接受回调函数释放出信号量，这里当作二值信号量获得，然后执行
	  Health_Report(HEALTH_CON);
	  (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));   /* 睡到"有数据"的通知为止 *///传入参数为pdTRUE==二值信号量；pdFALSE==计数信号量

    while ((c = Uart_RxGet()) >= 0)                  /* 一次取空 */
    {
		Console_FeedByte(c);		//c 是一个个发送的，在循环直至发完（把缓冲区的数据全部取完）
    }
  }
}

uint8_t Console_GetColor(void)
{
  return s_color;
}

uint8_t Console_GetBrightness(void)
{
  return s_brightness;
}

void Console_SetColor(uint8_t color)
{
  if (color <= (uint8_t)COLOR_WHITE)   /* 挡住 EEPROM 里的垃圾数据 */
  {
    s_color = color;
    Led_SetColor((App_Color_t)s_color); /* 状态一变，灯立刻跟着变 */
  }
}

void Console_SetBrightness(uint8_t brightness)
{
  if (brightness <= 100u)
  {
    s_brightness = brightness;
    Led_SetBrightness(brightness);        /* 通知 LED 模块：呼吸的幅度上限变了 */
  }
}

const char *Console_ColorName(void)
{
  return Color_Name((uint8_t)Console_GetColor());
}

/********************************* 文件结束 *******************************/
