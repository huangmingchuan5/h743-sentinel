/**
  ******************************************************************************
  * @file    bsp_ap3216c.c
  * @brief   AP3216C 驱动实现（见 bsp_ap3216c.h 的说明）。
  ******************************************************************************
  */

#include "bsp_ap3216c.h"
#include "i2c.h"                   /* CubeMX 生成的 hi2c4 */
#include "bsp_i2c_bus.h"           /* I2C4 总线互斥量 */

/* 寄存器地址（数据手册 / 野火例程） */
#define AP3216C_RA_SYS_CONFIG   0x00u   /* bit2 = SW reset；bit[1:0] = 模式 */
#define AP3216C_RA_ALS_DATA_L   0x0Cu
#define AP3216C_RA_PS_DATA_L    0x0Eu

#define AP3216C_MODE_SW_RESET   0x04u
#define AP3216C_MODE_ALS_AND_PS 0x03u

#define AP3216C_IO_TIMEOUT_MS   100u

/* ---- 原始寄存器操作（不加锁；由下面的公开函数统一持锁） ---- */

static uint8_t Ap_WriteReg(uint8_t reg, uint8_t val)
{
  return (uint8_t)HAL_I2C_Mem_Write(&hi2c4, (uint16_t)(AP3216C_ADDR7 << 1), reg,
                                    I2C_MEMADD_SIZE_8BIT, &val, 1u, AP3216C_IO_TIMEOUT_MS);
}

static uint8_t Ap_ReadRegs(uint8_t reg, uint8_t *buf, uint16_t len)
{
  return (uint8_t)HAL_I2C_Mem_Read(&hi2c4, (uint16_t)(AP3216C_ADDR7 << 1), reg,
                                   I2C_MEMADD_SIZE_8BIT, buf, len, AP3216C_IO_TIMEOUT_MS);
}

/* ---- 公开接口 ---- */

uint8_t Ap3216c_Init(void)
{
  uint8_t st;

  /* 1) 软复位 */
  if (I2cBus_Take(I2C_BUS_TIMEOUT_MS) == 0u)		//获取信号量
  {
    return I2C_BUS_ERR_BUSY;
  }
  st = Ap_WriteReg(AP3216C_RA_SYS_CONFIG, AP3216C_MODE_SW_RESET);		//把复位0x04u写进我们的配置
  I2cBus_Give();														//释放总线的互斥量

  if (st != 0u)
  {
    return st;
  }

  HAL_Delay(100);                     /* 等复位完成（本函数在开机阶段调用，HAL_Delay 可用） */

  /* 2) 打开 ALS + PS 功能 */
  if (I2cBus_Take(I2C_BUS_TIMEOUT_MS) == 0u)		//获取到锁
  {
    return I2C_BUS_ERR_BUSY;
  }
  st = Ap_WriteReg(AP3216C_RA_SYS_CONFIG, AP3216C_MODE_ALS_AND_PS);		//写ALS+PS功能
  I2cBus_Give();

  if (st != 0u)
  {
	  return st;				//返回值不是0即报错
  }

  HAL_Delay(100);                     /* 模式切换后至少等 100 ms 再读数据 */
  return 0u;
}

uint8_t Ap3216c_ReadAls(uint16_t *raw)
{
  uint8_t buf[2];
  uint8_t st;

  if (I2cBus_Take(I2C_BUS_TIMEOUT_MS) == 0u)
  {
    return I2C_BUS_ERR_BUSY;
  }
  /* 先读低字节、再读高字节（顺序读取会让芯片锁存这组数据，保证高低字节配套） */
  st = Ap_ReadRegs(AP3216C_RA_ALS_DATA_L, buf, 2u);
  I2cBus_Give();

  if (st != 0u)
  {
    return st;
  }

  *raw = (uint16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
  return 0u;
}

uint8_t Ap3216c_ReadPs(uint16_t *ps)
{
  uint8_t  buf[2];
  uint16_t d;
  uint8_t  st;

  if (I2cBus_Take(I2C_BUS_TIMEOUT_MS) == 0u)
  {
    return I2C_BUS_ERR_BUSY;
  }
  st = Ap_ReadRegs(AP3216C_RA_PS_DATA_L, buf, 2u);
  I2cBus_Give();

  if (st != 0u)		//fail
  {
    return st;
  }

  d = (uint16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));		//拼接16位

  /* 低字节 bit[3:0] + 高字节 bit[5:0] 拼成 10 位数据；bit15 是"靠近"标志 */
  *ps = (uint16_t)(((d & 0x000Fu) | ((uint16_t)((d >> 8) & 0x3Fu) << 4))
                   | (uint16_t)(d & 0x8000u));

  /* IR_OF / PS_OF 溢出标志：物体贴得很近或强红外环境，数据仅作参考 */
  if (((buf[0] & 0x40u) != 0u) || ((buf[1] & 0x40u) != 0u))
  {
    return 1u;
  }
  return 0u;
}
