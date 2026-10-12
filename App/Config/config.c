/**
  ******************************************************************************
  * @file    config.c
  * @brief   EEPROM 配置项的打包/解包、保存与恢复。
  *
  *          这个文件不认识 I2C 寄存器（交给 bsp_ee），也不认识命令行
  *          （交给 console），它只负责"这 8 个字节怎么解释"。
  ******************************************************************************
  */

#include "config.h"
#include "bsp_ee.h"
#include "console.h"

/* 日志条数：Day 3 的 Flash 模块会用 Config_SetCount() 更新它 */
static uint16_t s_count = 0u;

/* 把当前系统状态打包成 8 字节 */
static void Cfg_Pack(uint8_t *b)
{
  b[0] = (uint8_t)CFG_MAGIC0;
  b[1] = (uint8_t)CFG_MAGIC1;
  b[2] = (uint8_t)CFG_MAGIC2;
  b[3] = (uint8_t)CFG_MAGIC3;						//前四个是魔数，固定的，检查有效性
  b[4] = Console_GetColor();
  b[5] = Console_GetBrightness();
  b[6] = (uint8_t)(s_count & 0x00FFu);				//保留低八位
  b[7] = (uint8_t)((s_count >> 8) & 0x00FFu);		//保留高八位
}

/* 把 8 字节还原成系统状态（设置颜色会立即刷新 LED）		(刚才的Cfg_pack函数的逆操作) */
static void Cfg_Apply(const uint8_t *b)
{
  s_count = (uint16_t)((((uint16_t)b[7]) << 8) | (uint16_t)b[6]);		//重新组合
  Console_SetBrightness(b[5]);											//重新设置亮度和颜色
  Console_SetColor(b[4]);
}

uint8_t Config_Save(void)
{
  uint8_t b[CFG_SIZE];													//8u

  Cfg_Pack(b);						//把当前系统状态打包成 8 字节放进b[8]
  return Ee_Write(CFG_ADDR, b, CFG_SIZE);
}

uint8_t Config_Reset(void)
{
  Console_SetBrightness((uint8_t)CFG_DEF_BRIGHTNESS);
  Console_SetColor((uint8_t)CFG_DEF_COLOR);
  s_count = 0u;
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

  //核对魔数
  if ((b[0] != (uint8_t)CFG_MAGIC0) || (b[1] != (uint8_t)CFG_MAGIC1) ||
      (b[2] != (uint8_t)CFG_MAGIC2) || (b[3] != (uint8_t)CFG_MAGIC3))
  {
    /* EEPROM 里是 0xFF 或别的旧数据：当第一次使用，写一份默认配置 */
    st = Config_Reset();
    if (st != EE_OK)
    {
      return st;
    }
    return 1u;                          /* 1 = 用了默认值 */
  }

  Cfg_Apply(b);		//把 8 字节还原成系统状态（设置颜色会立即刷新 LED）
  return 0u;
}

uint16_t Config_GetCount(void)
{
  return s_count;
}

void Config_SetCount(uint16_t count)
{
  s_count = count;
}
