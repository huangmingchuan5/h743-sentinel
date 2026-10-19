/**
  ******************************************************************************
  * @file    bsp_i2c_bus.h
  * @brief   I2C4 总线共享资源（PB8 = SCL / PB9 = SDA）。
  *
  *          一条总线上挂着三个器件：EEPROM(0x50)、AP3216C(0x1E)、MPU6050(0x68)。
  *          RTOS 多任务环境下必须串行化访问，本模块提供：
  *            - I2cBus_Init()：调度器启动前创建互斥量；
  *            - I2cBus_Take/Give：抢/放总线（带超时）；
  *            - I2cBus_Probe：探测某个 7 位地址是否有器件应答（扫描用）。
  ******************************************************************************
  */

#ifndef __BSP_I2C_BUS_H
#define __BSP_I2C_BUS_H

#include "main.h"

#define I2C_BUS_TIMEOUT_MS        100u   /* 抢锁超时（别的任务正占着总线时最多等这么久） */
#define I2C_BUS_PROBE_TIMEOUT_MS  3u     /* 单次器件探测超时 */
#define I2C_BUS_ERR_BUSY          0x21u  /* 抢总线超时（各 I2C 驱动统一的"忙"错误码） */

/* 故障注入开关（默认 0）：改成 1 后 Take/Give 变成空操作，用于做
   "加锁 vs 不加锁"对照实验——并发访问 EEPROM 与传感器时会出现偶发 NACK，
   live/dump 里的 ERR 标志会给出来。验证完务必改回 0。 */
#ifndef I2C_LOCK_TEST_SKIP
#define I2C_LOCK_TEST_SKIP        0
#endif

/** 调度器启动前调用：创建总线互斥量。 */
void I2cBus_Init(void);

/** 抢总线：1 = 拿到（可以开始访问 I2C）；0 = 超时（别访问）。 */
uint8_t I2cBus_Take(uint32_t timeout_ms);

/** 释放总线。 */
void I2cBus_Give(void);

/** 探测 7 位地址 addr7 上是否有器件应答：1 = 有，0 = 没有。内部自带加锁。 */
uint8_t I2cBus_Probe(uint8_t addr7);

#endif /* __BSP_I2C_BUS_H */
