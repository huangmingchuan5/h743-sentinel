/**
  ******************************************************************************
  * @file    config.h
  * @brief   掉电保存在 EEPROM 里的配置项（16 字节，magic + CRC16）。
  *
  *          布局：
  *            [0..3] magic "P0L2" | [4] 版本=2 | [5] 颜色 | [6] 亮度
  *            [7] 采样周期 ms | [8..9] 光照阈值（ALS 原始值，低于=暗）
  *            [10..11] 运动阈值（相邻加速度差，LSB） | [12..13] CRC16
  *            [14..15] 保留
  *
  *          版本升级说明：magic 从旧版 "P0LG" 改为 "P0L2"，旧配置会按
  *          "首次使用"处理并写入默认值（颜色/亮度会回到默认，属预期）。
  ******************************************************************************
  */

#ifndef __CONFIG_H
#define __CONFIG_H

#include "main.h"
#include "bsp_led.h"                /* 用 COLOR_xxx 表示颜色 */

#define CFG_ADDR           0x00u    /* 配置放在 EEPROM 的 0x00 处 */
#define CFG_SIZE           16u
#define CFG_VERSION        2u

#define CFG_DEF_COLOR      ((uint8_t)COLOR_GREEN)
#define CFG_DEF_BRIGHTNESS 100u
#define CFG_DEF_RATE_MS    50u      /* 采样周期：50ms = 20Hz */
#define CFG_DEF_THR_LIGHT  10u      /* ALS 原始值低于 10 判为"暗" */
#define CFG_DEF_THR_MOTION 1200u    /* 相邻样本加速度差（LSB）门限 */

/**
  * @brief  上电时调用：把 EEPROM 里的配置读回来并生效。
  * @retval 0 = 恢复成功；1 = 首次使用/格式升级，已写入默认值；其他 = I2C 错误码
  */
uint8_t Config_Load(void);

/** 把当前状态写回 EEPROM：0 = 成功，其他 = 错误码。 */
uint8_t Config_Save(void);

/** 恢复默认值并写回 EEPROM。 */
uint8_t Config_Reset(void);

/* ---- 运行参数（供采样/报警模块读取） ---- */
uint16_t Config_GetThrLight(void);
uint16_t Config_GetThrMotion(void);
uint8_t  Config_GetRateMs(void);     /* 20..200，已做范围钳制 */

/* ---- 修改（cfg 命令用；改完要 cfg save 才进 EEPROM） ---- */
void Config_SetThrLight(uint16_t v);
void Config_SetThrMotion(uint16_t v);
void Config_SetRateMs(uint8_t v);

#endif /* __CONFIG_H */
