/**
  ******************************************************************************
  * @file    bsp_i2c_bus.c
  * @brief   I2C4 总线互斥量 + 器件探测（见 bsp_i2c_bus.h 的说明）。
  ******************************************************************************
  */

#include "bsp_i2c_bus.h"
#include "i2c.h"                   /* CubeMX 生成的 hi2c4 */
#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t s_i2cMutex = NULL;

void I2cBus_Init(void)
{
  s_i2cMutex = xSemaphoreCreateMutex();				//创建互斥信号量
}


//尝试获取 I2C 总线的访问权（互斥锁），互斥访问
uint8_t I2cBus_Take(uint32_t timeout_ms)
{
  if (s_i2cMutex == NULL)
  {
    return 1u;                     /* 调度器启动前的单线程阶段：无需加锁 */
  }

  return (xSemaphoreTake(s_i2cMutex, pdMS_TO_TICKS(timeout_ms)) == pdPASS) ? 1u : 0u;
}

void I2cBus_Give(void)
{
  if (s_i2cMutex != NULL)
  {
    (void)xSemaphoreGive(s_i2cMutex);
  }
}

uint8_t I2cBus_Probe(uint8_t addr7)
{
  uint8_t ok = 0u;

  if (I2cBus_Take(I2C_BUS_TIMEOUT_MS) == 0u)
  {
    return 0u;
  }

  /* HAL 的 DevAddress 参数是"7 位地址左移 1 位" 
  *I2C 设备探测函数，用来判断某个 7 位地址上是否真的挂着设备
  *retval ==1	在线  /		retval == 0		无应答
  *@param	2u是尝试次数，表示失败后重试2次
  */
  if (HAL_I2C_IsDeviceReady(&hi2c4, (uint16_t)(addr7 << 1), 2u, I2C_BUS_PROBE_TIMEOUT_MS) == HAL_OK)
  {
    ok = 1u;
  }

  I2cBus_Give();
  return ok;
}
