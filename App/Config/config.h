/**
  ******************************************************************************
  * @file    config.h
  * @brief   掉电保存在 EEPROM 里的配置项（共 8 字节，正好一页）。
  *
  *          布局：[0..3] magic | [4] 颜色 | [5] 亮度 | [6..7] 日志条数（小端）
  ******************************************************************************
  */

#ifndef __CONFIG_H
#define __CONFIG_H

#include "main.h"
#include "bsp_led.h"                /* 用 COLOR_xxx 表示颜色 */

#define CFG_ADDR           0x00u    /* 配置放在 EEPROM 的 0x00 处 */
#define CFG_SIZE           8u

/* magic：4 个字节全对才认为里面是有效配置，否则视为"第一次使用" */
#define CFG_MAGIC0         'P'
#define CFG_MAGIC1         '0'
#define CFG_MAGIC2         'L'
#define CFG_MAGIC3         'G'

#define CFG_DEF_COLOR      ((uint8_t)COLOR_GREEN)
#define CFG_DEF_BRIGHTNESS 100u

/**
  * @brief  上电时调用：把 EEPROM 里的配置读回来并生效。
  * @retval 0 = 恢复成功；1 = 首次使用，已写入默认值；其他 = I2C 错误码
  */
uint8_t Config_Load(void);

/** 把当前状态写回 EEPROM：0 = 成功，其他 = 错误码。 */
uint8_t Config_Save(void);

/** 恢复默认值并写回 EEPROM。 */
uint8_t Config_Reset(void);

/* 日志条数：Day 3 的 Flash 模块通过这两个接口读写它 */
uint16_t Config_GetCount(void);
void     Config_SetCount(uint16_t count);

#endif /* __CONFIG_H */
