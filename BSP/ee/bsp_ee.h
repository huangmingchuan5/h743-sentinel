/**
  ******************************************************************************
  * @file    bsp_ee.h
  * @brief   AT24C02 EEPROM 驱动（I2C4：PB8 = SCL，PB9 = SDA）。
  *
  *          只负责"往某个地址读写 N 个字节"，不认识颜色、亮度这些业务概念。
  *          参数：容量 256 字节、每页 8 字节、写完一页要等约 5 ms。
  ******************************************************************************
  */

#ifndef __BSP_EE_H
#define __BSP_EE_H

#include "main.h"

/* AT24C02：7 位地址 0x50，左移一位是 0xA0（HAL 里就填 0xA0） */
#define EE_DEV_ADDR        0xA0u
#define EE_PAGE_SIZE       8u       /* 每页 8 字节 */
#define EE_WRITE_CYCLE_MS  5u       /* 页写后的内部写周期，数据手册最大 5 ms */
#define EE_TIMEOUT_MS      100u     /* 单次 I2C 传输超时 */

/* 返回值统一约定：0 = 成功；其余 = 错误码
   （1/2/3 是 HAL 的 HAL_ERROR/HAL_BUSY/HAL_TIMEOUT，0x20 是我们自己的越界检查） */
#define EE_OK              0x00u
#define EE_ERR_RANGE       0x20u

/** 检查器件是否应答：0 = 在线，非 0 = 失败。 */
uint8_t Ee_Check(void);

/** 从 EEPROM 地址 addr 连续读 len 字节。 */
uint8_t Ee_Read(uint8_t addr, uint8_t *buf, uint16_t len);

/** 从 EEPROM 地址 addr 连续写 len 字节（内部自动按 8 字节分页并等写周期）。 */
uint8_t Ee_Write(uint8_t addr, uint8_t *buf, uint16_t len);

#endif /* __BSP_EE_H */
