/**
  ******************************************************************************
  * @file    bsp_mpu6050.c
  * @brief   MPU6050 驱动实现（见 bsp_mpu6050.h 的说明）。
  ******************************************************************************
  */

#include "bsp_mpu6050.h"
#include "i2c.h"                   /* CubeMX 生成的 hi2c4 */
#include "bsp_i2c_bus.h"           /* I2C4 总线互斥量 */

/* 寄存器地址（MPU6050 数据手册寄存器映射） */
#define MPU_RA_SMPLRT_DIV     0x19u
#define MPU_RA_CONFIG         0x1Au
#define MPU_RA_GYRO_CONFIG    0x1Bu
#define MPU_RA_ACCEL_CONFIG   0x1Cu
#define MPU_RA_ACCEL_XOUT_H   0x3Bu
#define MPU_RA_PWR_MGMT_1     0x6Bu
#define MPU_RA_WHO_AM_I       0x75u			//MPU6050 内部寄存器的地址（WHO_AM_I 寄存器）

#define MPU_IO_TIMEOUT_MS     100u

static uint8_t s_whoami = 0u;

/* ---- 原始寄存器操作（不加锁；由下面的公开函数统一持锁） ---- */

static uint8_t Mpu_WriteReg(uint8_t reg, uint8_t val)
{
  return (uint8_t)HAL_I2C_Mem_Write(&hi2c4, (uint16_t)(MPU6050_ADDR7 << 1), reg,
                                    I2C_MEMADD_SIZE_8BIT, &val, 1u, MPU_IO_TIMEOUT_MS);
}

static uint8_t Mpu_ReadRegs(uint8_t reg, uint8_t *buf, uint16_t len)
{
  return (uint8_t)HAL_I2C_Mem_Read(&hi2c4, (uint16_t)(MPU6050_ADDR7 << 1), reg,
                                   I2C_MEMADD_SIZE_8BIT, buf, len, MPU_IO_TIMEOUT_MS);
}

/* ---- 公开接口 ---- */

uint8_t Mpu6050_Init(void)
{
  uint8_t id     = 0u;
  uint8_t seq_ok = 1u;

  HAL_Delay(100);                     /* 上电后等稳定（本函数在开机阶段调用） */

  if (I2cBus_Take(I2C_BUS_TIMEOUT_MS) == 0u)
  {
    return I2C_BUS_ERR_BUSY;
  }

  /* 把WHOAMI寄存器内的（只是从0x75处开始读）内容读入&id*/
  if (Mpu_ReadRegs(MPU_RA_WHO_AM_I, &id, 1u) != 0u)		//不为0报错
  {
    I2cBus_Give();
    return 0x01u;					//IIC 读取错误
  }
  s_whoami = id;			//全局静态变量
  if (id != MPU6050_ADDR7)			//WHO_AM_I 寄存器(0x75)处读出来的的默认值恰好是 0x68
  {
    I2cBus_Give();
    return 0x30u;		//ID disMATCH错误
  }

  /* 2) 一次持锁，将配置写入寄存器内部（前面的只是起始地址）*/
  if (Mpu_WriteReg(MPU_RA_PWR_MGMT_1,   0x00u) != 0u) { seq_ok = 0u; }  /* 唤醒 */
  if (Mpu_WriteReg(MPU_RA_SMPLRT_DIV,   0x07u) != 0u) { seq_ok = 0u; }  /* 1kHz/(1+7) = 125Hz */
  if (Mpu_WriteReg(MPU_RA_CONFIG,       0x03u) != 0u) { seq_ok = 0u; }  /* DLPF：加速度 44Hz */
  if (Mpu_WriteReg(MPU_RA_GYRO_CONFIG,  0x18u) != 0u) { seq_ok = 0u; }  /* 陀螺 ±2000 dps */
  if (Mpu_WriteReg(MPU_RA_ACCEL_CONFIG, 0x08u) != 0u) { seq_ok = 0u; }  /* 加速度 ±4g */

  I2cBus_Give();

  HAL_Delay(50);                      /* 等配置生效 */

  return seq_ok ? 0u : 0x01u;		//Init函数返回0成功
}

uint8_t Mpu6050_WhoAmI(void)
{
  return s_whoami;
}

uint8_t Mpu6050_ReadAccel(int16_t *ax, int16_t *ay, int16_t *az)
{
  uint8_t buf[6];
  uint8_t st;

  if (I2cBus_Take(I2C_BUS_TIMEOUT_MS) == 0u)
  {
    return I2C_BUS_ERR_BUSY;
  }
  st = Mpu_ReadRegs(MPU_RA_ACCEL_XOUT_H, buf, 6u);		//6字节
  I2cBus_Give();

  if (st != 0u)
  {
    return st;
  }

  //三轴加速度拼接
  *ax = (int16_t)((uint16_t)((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
  *ay = (int16_t)((uint16_t)((uint16_t)buf[2] << 8) | (uint16_t)buf[3]);
  *az = (int16_t)((uint16_t)((uint16_t)buf[4] << 8) | (uint16_t)buf[5]);
  return 0u;
}
