/**
  ******************************************************************************
  * @file    config.c
  * @brief   EEPROM 配置项的打包/解包、保存与恢复（16B，magic + CRC16）。
  ******************************************************************************
  */

#include "config.h"
#include "bsp_ee.h"
#include "console.h"

static uint16_t s_thrLight  = CFG_DEF_THR_LIGHT;
static uint16_t s_thrMotion = CFG_DEF_THR_MOTION;
static uint8_t  s_rateMs    = CFG_DEF_RATE_MS;

static uint16_t Cfg_Crc16(const uint8_t *data, uint32_t len)
{
  uint16_t crc = 0xFFFFu;
  uint32_t i;

  while (len-- > 0u)
  {
    crc ^= (uint16_t)((uint16_t)(*data++) << 8);
    for (i = 0u; i < 8u; i++)
    {
      if ((crc & 0x8000u) != 0u)
      {
        crc = (uint16_t)((crc << 1) ^ 0x1021u);
      }
      else
      {
        crc = (uint16_t)(crc << 1);
      }
    }
  }
  return crc;
}

//把配置参数打包成 16 字节
static void Cfg_Pack(uint8_t *b)
{
  uint16_t crc;

  b[0]  = 'P';
  b[1]  = '0';
  b[2]  = 'L';
  b[3]  = '2';                       
	/*魔数： magic（旧版是 'G'，用于格式升级） */
  b[4]  = CFG_VERSION;				//版本号
  b[5]  = Console_GetColor();		
  b[6]  = Console_GetBrightness();
  b[7]  = s_rateMs;
  b[8]  = (uint8_t)(s_thrLight & 0xFFu);
  b[9]  = (uint8_t)((s_thrLight >> 8) & 0xFFu);
  b[10] = (uint8_t)(s_thrMotion & 0xFFu);
  b[11] = (uint8_t)((s_thrMotion >> 8) & 0xFFu);

	//标准的 CRC-16/CCITT-FALSE 算法(CRC校验)
  crc = Cfg_Crc16(b, 12u);
  b[12] = (uint8_t)(crc & 0xFFu);
  b[13] = (uint8_t)((crc >> 8) & 0xFFu);
  b[14] = 0u;
  b[15] = 0u;
}

/* 1 = 内容有效并已生效；0 = 无效 */
//解包读取配置
static uint8_t Cfg_Apply(const uint8_t *b)
{
  uint16_t crc;
  uint8_t  color     = b[5];
  uint8_t  brightness = b[6];
  uint8_t  rate      = b[7];
  uint16_t light     = (uint16_t)(((uint16_t)b[9] << 8) | (uint16_t)b[8]);
  uint16_t motion    = (uint16_t)(((uint16_t)b[11] << 8) | (uint16_t)b[10]);

  if ((b[0] != 'P') || (b[1] != '0') || (b[2] != 'L') || (b[3] != '2'))
  {
    return 0u;
  }
  if (b[4] != CFG_VERSION)
  {
    return 0u;
  }

  crc = (uint16_t)(((uint16_t)b[13] << 8) | (uint16_t)b[12]);
  if (crc != Cfg_Crc16(b, 12u))		//CRC校验
  {
    return 0u;
  }

  if ((color > (uint8_t)COLOR_WHITE) || (brightness > 100u))
  {
    return 0u;
  }

  if (rate < 20u)   { rate = 20u; }
  if (rate > 200u)  { rate = 200u; }

  Console_SetBrightness(brightness);
  Console_SetColor(color);

  s_rateMs    = rate;
  s_thrLight  = light;
  s_thrMotion = motion;
  return 1u;
}

uint8_t Config_Save(void)
{
  uint8_t b[CFG_SIZE];

  Cfg_Pack(b);
  return Ee_Write(CFG_ADDR, b, CFG_SIZE);
}

uint8_t Config_Reset(void)
{
  Console_SetBrightness(CFG_DEF_BRIGHTNESS);
  Console_SetColor(CFG_DEF_COLOR);

  s_rateMs    = CFG_DEF_RATE_MS;
  s_thrLight  = CFG_DEF_THR_LIGHT;
  s_thrMotion = CFG_DEF_THR_MOTION;

  return Config_Save();
}

uint8_t Config_Load(void)
{
  uint8_t b[CFG_SIZE];
  uint8_t st;

  st = Ee_Read(CFG_ADDR, b, CFG_SIZE);
  if (st != EE_OK)
  {
    return st;                          /* I2C 读失败，交给调用者打印 */
  }

  if (Cfg_Apply(b) == 0u)
  {
    /* 无效/旧格式：写一份默认配置 */
    st = Config_Reset();
    if (st != EE_OK)
    {
      return st;
    }
    return 1u;                          /* 1 = 用了默认值 */
  }

  return 0u;
}

uint16_t Config_GetThrLight(void)
{
  return s_thrLight;
}

uint16_t Config_GetThrMotion(void)
{
  return s_thrMotion;
}

uint8_t Config_GetRateMs(void)
{
  return s_rateMs;
}

void Config_SetThrLight(uint16_t v)
{
  s_thrLight = v;
}

void Config_SetThrMotion(uint16_t v)
{
  s_thrMotion = v;
}

void Config_SetRateMs(uint8_t v)
{
  if (v < 20u)
  {
    v = 20u;
  }
  if (v > 200u)
  {
    v = 200u;
  }
  s_rateMs = v;
}
