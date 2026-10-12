/**
  ******************************************************************************
  * @file    bsp_qspi_flash.c
  * @brief   W25Q256JV 驱动实现（阻塞式 + 自动轮询等 WIP，全部带超时）。
  *
  *          指令表：
  *            0x06 写使能 | 0x05 读状态1 | 0x9F 读 JEDEC ID
  *            0x03 读数据 | 0x02 页写     | 0x20 4KB 扇区擦除
  ******************************************************************************
  */

#include "bsp_qspi_flash.h"
#include "quadspi.h"                /* CubeMX 生成的 hqspi */

#define FLASH_CMD_WRITE_ENABLE   0x06u				//0x06u 	写使能
#define FLASH_CMD_READ_STATUS1   0x05u
#define FLASH_CMD_READ_ID        0x9Fu
#define FLASH_CMD_READ_DATA      0x03u
#define FLASH_CMD_PAGE_PROGRAM   0x02u
#define FLASH_CMD_SECTOR_ERASE   0x20u

/* 把命令结构体的公共字段填好：单线指令、不发地址/数据，其余都清空 
*	QSPI 命令结构体初始化辅助函数。它的作用是先给 QSPI_CommandTypeDef 结构体赋予一个“基础模板”配置
*/
static void Flash_FillCmd(QSPI_CommandTypeDef *cmd, uint8_t instr)
{
  cmd->Instruction        = instr;
  cmd->InstructionMode    = QSPI_INSTRUCTION_1_LINE;
  cmd->Address            = 0u;
  cmd->AddressSize        = QSPI_ADDRESS_24_BITS;      /* 3 字节地址 */
  cmd->AddressMode        = QSPI_ADDRESS_NONE;
  cmd->AlternateBytes     = 0u;
  cmd->AlternateBytesSize = QSPI_ALTERNATE_BYTES_8_BITS;
  cmd->AlternateByteMode  = QSPI_ALTERNATE_BYTES_NONE;
  cmd->DummyCycles        = 0u;
  cmd->DataMode           = QSPI_DATA_NONE;
  cmd->NbData             = 0u;
  cmd->DdrMode            = QSPI_DDR_MODE_DISABLE;
  cmd->DdrHoldHalfCycle   = QSPI_DDR_HHC_ANALOG_DELAY;
  cmd->SIOOMode           = QSPI_SIOO_INST_EVERY_CMD;
}

static uint8_t Flash_WriteEnable(void)
{
  QSPI_CommandTypeDef cmd = {0};

  Flash_FillCmd(&cmd, FLASH_CMD_WRITE_ENABLE);			//写使能

  //如果 HAL_QSPI_Command 执行成功（返回 HAL_OK），那么这个函数就返回 FLASH_OK；否则返回 FLASH_ERR。（三元运算符）
  return (HAL_QSPI_Command(&hqspi, &cmd, FLASH_TIMEOUT_MS) == HAL_OK) ? FLASH_OK : FLASH_ERR;
}


//在发送写（Write）或擦除（Erase）命令后，阻塞等待，直到 Flash 芯片内部操作完成（空闲）为止
uint8_t Flash_WaitBusy(void)
{
  QSPI_CommandTypeDef    cmd  = {0};
  QSPI_AutoPollingTypeDef poll = {0};

  Flash_FillCmd(&cmd, FLASH_CMD_READ_STATUS1);
  cmd.DataMode = QSPI_DATA_1_LINE;
  cmd.NbData   = 1u;

  /* 一直读到 状态寄存器 bit0(WIP) = 0 为止；芯片写/擦期间它不会理我们 */
  poll.Match           = 0x00000000u;
  poll.Mask            = 0x00000001u;
  poll.MatchMode       = QSPI_MATCH_MODE_AND;
  poll.StatusBytesSize = 1u;
  poll.Interval        = 0x10u;
  poll.AutomaticStop   = QSPI_AUTOMATIC_STOP_ENABLE;

  return (HAL_QSPI_AutoPolling(&hqspi, &cmd, &poll, FLASH_TIMEOUT_MS) == HAL_OK) ? FLASH_OK : FLASH_ERR;
}

uint8_t Flash_ReadID(uint32_t *id)
{
  QSPI_CommandTypeDef cmd = {0};
  uint8_t buf[3];

  Flash_FillCmd(&cmd, FLASH_CMD_READ_ID);
  cmd.DataMode = QSPI_DATA_1_LINE;
  cmd.NbData   = 3u;

  if (HAL_QSPI_Command(&hqspi, &cmd, FLASH_TIMEOUT_MS) != HAL_OK)
  {
    return FLASH_ERR;
  }
  if (HAL_QSPI_Receive(&hqspi, buf, FLASH_TIMEOUT_MS) != HAL_OK)
  {
    return FLASH_ERR;
  }

  //数据拼接
  *id = ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | (uint32_t)buf[2];
  return FLASH_OK;
}


//从外部 QSPI Flash 的指定地址（addr）开始，连续读取指定长度（len）的数据，并存入到内存缓冲区（buf）中。
uint8_t Flash_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
  QSPI_CommandTypeDef cmd = {0};

  if (len == 0u)
  {
    return FLASH_OK;
  }

  Flash_FillCmd(&cmd, FLASH_CMD_READ_DATA);		//默认配置
  cmd.Address     = addr;
  cmd.AddressMode = QSPI_ADDRESS_1_LINE;
  cmd.DataMode    = QSPI_DATA_1_LINE;
  cmd.NbData      = len;

  if (HAL_QSPI_Command(&hqspi, &cmd, FLASH_TIMEOUT_MS) != HAL_OK)
  {
    return FLASH_ERR;
  }
  if (HAL_QSPI_Receive(&hqspi, buf, FLASH_TIMEOUT_MS) != HAL_OK)
  {
    return FLASH_ERR;
  }
  return FLASH_OK;
}

uint8_t Flash_PageWrite(uint32_t addr, uint8_t *buf, uint32_t len)
{
  QSPI_CommandTypeDef cmd = {0};

  if ((len == 0u) || (len > FLASH_PAGE_SIZE))
  {
    return FLASH_ERR;                  /* 一条页写最多 256 字节 */
  }

  if (Flash_WriteEnable() != FLASH_OK) /* 写之前必须先发写使能 */
  {
    return FLASH_ERR;
  }

  Flash_FillCmd(&cmd, FLASH_CMD_PAGE_PROGRAM);
  cmd.Address     = addr;
  cmd.AddressMode = QSPI_ADDRESS_1_LINE;
  cmd.DataMode    = QSPI_DATA_1_LINE;
  cmd.NbData      = len;

  if (HAL_QSPI_Command(&hqspi, &cmd, FLASH_TIMEOUT_MS) != HAL_OK)
  {
    return FLASH_ERR;
  }
  if (HAL_QSPI_Transmit(&hqspi, buf, FLASH_TIMEOUT_MS) != HAL_OK)
  {
    return FLASH_ERR;
  }

  return Flash_WaitBusy();             /* 等内部编程完成（典型 0.7 ms） */
}

//擦除指定地址所在的 4KB 扇区（Sector Erase）
uint8_t Flash_EraseSector(uint32_t addr)
{
  QSPI_CommandTypeDef cmd = {0};

  if (Flash_WriteEnable() != FLASH_OK)
  {
    return FLASH_ERR;
  }

  Flash_FillCmd(&cmd, FLASH_CMD_SECTOR_ERASE);
  cmd.Address     = addr;
  cmd.AddressMode = QSPI_ADDRESS_1_LINE;

  if (HAL_QSPI_Command(&hqspi, &cmd, FLASH_TIMEOUT_MS) != HAL_OK)
  {
    return FLASH_ERR;
  }

  return Flash_WaitBusy();             /* 4KB 扇区擦除典型 45 ms、最大 400 ms */
}


//批量擦除整个日志存储区域（Log Area）
//由于 Flash 的擦除最小单位是扇区（通常是 4KB），无法一次性擦除一整块区域，因此必须通过循环，逐个扇区进行擦除
uint8_t Flash_EraseLogArea(void)
{
  uint8_t i;
  uint8_t st;

  for (i = 0u; i < (uint8_t)FLASH_LOG_SECTORS; i++)
  {
    st = Flash_EraseSector(FLASH_LOG_BASE + ((uint32_t)i * FLASH_ERASE_SIZE));
    if (st != FLASH_OK)
    {
      return st;
    }
  }
  return FLASH_OK;
}
