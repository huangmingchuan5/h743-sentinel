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
static void        Cmd_Log(const char *arg);
static void        Cmd_Dump(const char *arg);
static void        Cmd_Clear(const char *arg);
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
  Uart_Printf("  log <text>                      append one record (max 60 bytes)\r\n");
  Uart_Printf("  dump | clear                    list all / erase the log area\r\n");
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
    Uart_Printf("OK  saved: color=%u brightness=%u count=%u\r\n",
           (unsigned)Console_GetColor(), (unsigned)Console_GetBrightness(),
           (unsigned)Config_GetCount());
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
  Uart_Printf("log count  = %u\r\n", (unsigned)Log_GetCount());

  st = Ee_Read(CFG_ADDR, b, CFG_SIZE);
  if (st == EE_OK)
  {
    Uart_Printf("EEPROM[0..7] =");
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

/* log <文字>：往 Flash 里追加一条记录 */
static void Cmd_Log(const char *arg)
{
  uint8_t st;

  if (*arg == '\0')
  {
    Uart_Printf("ERR usage: log <text>  (max %u bytes)\r\n", (unsigned)LOG_TEXT_MAX);
    return;
  }

  st = Log_Append(arg);
  if (st == 0u)
  {
    Uart_Printf("OK  log #%u saved", (unsigned)Log_GetCount());
    if (arg[LOG_TEXT_MAX] != '\0')   /* 输入比 60 字节长，被截断了 */
    {
      Uart_Printf("(text cut to %u bytes)", (unsigned)LOG_TEXT_MAX);
    }
    Uart_Printf("\r\n");
  }
  else if (st == 1u)
  {
    Uart_Printf("ERR log area is full (%u records), use 'clear'\r\n",
           (unsigned)LOG_MAX_RECORDS);
  }
  else
  {
    Uart_Printf("ERR flash write failed (0x%02X)\r\n", (unsigned)st);
  }
}

/* dump：按序号打印全部记录 */
static void Cmd_Dump(const char *arg)
{
  uint8_t  rec[LOG_RECORD_SIZE];
  char     text[LOG_TEXT_MAX + 1u];
  uint16_t n = Log_GetCount();
  uint16_t i;

  (void)arg;

  if (n == 0u)
  {
    Uart_Printf("(no records)\r\n");
    return;
  }

  for (i = 0u; i < n; i++)
  {
    if (Log_ReadAt(i, rec) != FLASH_OK)
    {
      Uart_Printf("ERR read record #%u failed\r\n", (unsigned)(i + 1u));
      return;
    }

    (void)Log_TextOf(rec, text, (uint16_t)sizeof(text));
    Uart_Printf("#%u  %s\r\n", (unsigned)(i + 1u), text);
  }

  Uart_Printf("total %u record(s)\r\n", (unsigned)n);
}

/* clear：擦掉整个日志区并把条数清零（EEPROM 也一起更新） */
static void Cmd_Clear(const char *arg)
{
  uint8_t st;

  (void)arg;

  st = Log_Clear();
  if (st != FLASH_OK)
  {
    Uart_Printf("ERR flash erase failed (0x%02X)\r\n", (unsigned)st);
    return;
  }

  if (Config_Save() != EE_OK)        /* 把"条数 = 0"写进 EEPROM，保持两边一致 */
  {
    Uart_Printf("WARN log cleared, but config write failed\r\n");
  }

  Uart_Printf("OK  log area erased\r\n");
}



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
  else if (Str_EqualNoCase(cmd, "log") != 0u)
  {
    Cmd_Log(arg);
  }
  else if (Str_EqualNoCase(cmd, "dump") != 0u)
  {
    Cmd_Dump(arg);
  }
  else if (Str_EqualNoCase(cmd, "clear") != 0u)
  {
    Cmd_Clear(arg);
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
	  (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* 睡到"有数据"的通知为止 *///传入参数为pdTRUE==二值信号量；pdFALSE==计数信号量

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
