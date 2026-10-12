/**
  ******************************************************************************
  * @file    bsp_ee.c
  * @brief   AT24C02 EEPROM 驱动实现（阻塞式，带超时，不会死等）。
  ******************************************************************************
  */

#include "bsp_ee.h"
#include "i2c.h"                    /* CubeMX 生成的 hi2c4 */

//判断 EEPROM 芯片是否正常连接并响应 I2C 通信。
uint8_t Ee_Check(void)
{
  /* 连试 3 次，每次最多等 100 ms；器件应答就返回 HAL_OK(0) */
  return (uint8_t)HAL_I2C_IsDeviceReady(&hi2c4, EE_DEV_ADDR, 3u, EE_TIMEOUT_MS);
}

uint8_t Ee_Read(uint8_t addr, uint8_t *buf, uint16_t len)
{
  if (len == 0u)
  {
    return EE_OK;
  }
  if (((uint16_t)addr + len) > 256u)      /* 只有 256 字节，越界直接报错 */
  {
    return EE_ERR_RANGE;
  }

  /* 读的时候器件内部地址会自动递增，一次读完即可 */
  return (uint8_t)HAL_I2C_Mem_Read(&hi2c4, EE_DEV_ADDR, (uint16_t)addr,
                                   I2C_MEMADD_SIZE_8BIT, buf, len, EE_TIMEOUT_MS);
}

//一次 I2C 写事务最多只能写到当前页的末尾。
//如果一次写跨越了页边界，芯片会回卷（wrap around）到本页开头，把前面的数据覆盖掉
//把一次可能跨页的写，拆成若干"不跨页"的小写，每小段后等 EEPROM 完成内部写周期，逐段推进直到全部写完。
uint8_t Ee_Write(uint8_t addr, uint8_t *buf, uint16_t len)
{
  uint16_t done = 0u;

  if (len == 0u)
  {
    return EE_OK;
  }
  if (((uint16_t)addr + len) > 256u)
  {
    return EE_ERR_RANGE;
  }

  /* 一次页写最多写到本页末尾，所以按"本页还剩几字节"切成若干段 */
  while (done < len)
  {
	uint16_t pos       = (uint16_t)(addr + done);                        // 当前写到 EEPROM 的哪个绝对地址
	uint16_t page_left = (uint16_t)(EE_PAGE_SIZE - (pos % EE_PAGE_SIZE)); // 本页还剩几字节到页尾
	uint16_t chunk     = (uint16_t)(len - done);                          // 还没写的字节数
    uint8_t  st;

    if (chunk > page_left)
    {
      chunk = page_left;                /* 本段最多写到页边界 */
    }

    st = (uint8_t)HAL_I2C_Mem_Write(&hi2c4, EE_DEV_ADDR, pos,
                                    I2C_MEMADD_SIZE_8BIT, &buf[done], chunk,
                                    EE_TIMEOUT_MS);
    if (st != EE_OK)
    {
      return st;
    }

    /* 等芯片把这一页真正写进存储单元，这期间它不会应答 I2C */
    HAL_Delay(EE_WRITE_CYCLE_MS);

    done = (uint16_t)(done + chunk);
  }

  return EE_OK;
}
