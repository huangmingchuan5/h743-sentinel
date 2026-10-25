/**
  ******************************************************************************
  * @file    bsp_i2c_bus.c
  * @brief   I2C4 总线互斥量 + 器件探测（见 bsp_i2c_bus.h 的说明）。
  ******************************************************************************
  */

#include "bsp_i2c_bus.h"
#include "i2c.h"                   /* CubeMX 生成的 hi2c4 */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#if (I2C_LOCK_TEST_SKIP == 0)
static SemaphoreHandle_t s_i2cMutex = NULL;
#endif

void I2cBus_Init(void)
{
#if (I2C_LOCK_TEST_SKIP == 0)
  s_i2cMutex = xSemaphoreCreateMutex();				//创建互斥信号量
#endif
}


//尝试获取 I2C 总线的访问权（互斥锁），互斥访问
uint8_t I2cBus_Take(uint32_t timeout_ms)
{
#if (I2C_LOCK_TEST_SKIP != 0)
  (void)timeout_ms;
  return 1u;                       /* 故障注入：不加锁，直接放行 */
#else
  if (s_i2cMutex == NULL)
  {
    return 1u;                     /* 还没创建：无需加锁 */
	//"无需加锁"，视为获取成功
  }

  /* 调度器启动前是单线程阶段，不碰互斥量：此时 xSemaphoreTake/Give 内部的
     临界区会把 BASEPRI 卡在 0x50（port.c 的 uxCriticalNesting 初值是哨兵
     0xaaaaaaaa，退出时判断 "==0" 不成立、屏蔽不会清），会连带屏蔽 TIM7
     （HAL 时基）和 TIM6（呼吸灯）中断。 */
  if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)		//判断调度器处于没有启动的状态，是则进入循环(也是视为成功)
  {
    return 1u;
  }

  return (xSemaphoreTake(s_i2cMutex, pdMS_TO_TICKS(timeout_ms)) == pdPASS) ? 1u : 0u;		//操作是进行消费信号量
#endif
}

void I2cBus_Give(void)
{
#if (I2C_LOCK_TEST_SKIP == 0)
  if ((s_i2cMutex != NULL) && (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED))		//创建过互斥信号量且调度器已经开始运行
  {
    (void)xSemaphoreGive(s_i2cMutex);
  }
#endif
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
