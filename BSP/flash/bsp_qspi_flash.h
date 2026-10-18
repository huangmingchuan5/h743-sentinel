/**
  ******************************************************************************
  * @file    bsp_qspi_flash.h
  * @brief   W25Q256JV（32 MB，QUADSPI Bank1）驱动。
  *
  *          上电后由 Flash_Init() 发 0xB7 进入"4 字节地址模式"，之后
  *          读 0x03 / 页写 0x02 / 4KB 扇区擦 0x20 全部使用 32 位地址，
  *          可以访问完整的 32 MB。全部走间接模式，不用内存映射(XIP)。
  ******************************************************************************
  */

#ifndef __BSP_QSPI_FLASH_H
#define __BSP_QSPI_FLASH_H

#include "main.h"

#define FLASH_PAGE_SIZE      256u      /* 一页 256 字节（一条页写不能跨页） */
#define FLASH_ERASE_SIZE     4096u     /* 最小擦除单位 4 KB（注意别叫 FLASH_SECTOR_SIZE，
                                          那个名字已经被芯片头文件占用，指的是内部 Flash） */
#define FLASH_TIMEOUT_MS     1000u     /* 单次操作超时（扇区擦除最长 400 ms） */

#define FLASH_OK             0x00u
#define FLASH_ERR            0x01u

/* 日志区：Flash 最前面 4 个扇区 = 16 KB */
#define FLASH_LOG_BASE       0x000000u
#define FLASH_LOG_SECTORS    4u

/** 读 JEDEC ID（W25Q256JV 应该是 0xEF4019）：0 = 成功。 */
uint8_t Flash_ReadID(uint32_t *id);

/** 上电初始化：读 JEDEC ID + 进入 4 字节地址模式（32MB 可寻址）。
    id 可传 NULL。0 = 成功。 */
uint8_t Flash_Init(uint32_t *id);

/** 从地址 addr 连续读 len 字节。 */
uint8_t Flash_Read(uint32_t addr, uint8_t *buf, uint32_t len);

/** 页写：len 不能超过 256，且调用者要保证不跨页（本工程 64 字节记录天然不跨页）。 */
uint8_t Flash_PageWrite(uint32_t addr, uint8_t *buf, uint32_t len);

/** 擦除一个 4 KB 扇区（地址任意落在该扇区内）。 */
uint8_t Flash_EraseSector(uint32_t addr);

/** 擦除整个日志区（4 个扇区）。 */
uint8_t Flash_EraseLogArea(void);

/** 等芯片内部操作完成（轮询状态寄存器 WIP 位）。 */
uint8_t Flash_WaitBusy(void);

#endif /* __BSP_QSPI_FLASH_H */
