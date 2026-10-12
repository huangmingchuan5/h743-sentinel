/**
  ******************************************************************************
  * @file    log.c
  * @brief   日志记录层实现：追加、遍历、清空。
  *
  *          它不认识 QSPI 寄存器（交给 bsp_qspi_flash），也不认识命令行
  *          （交给 console），只负责"这 64 个字节怎么解释"。
  ******************************************************************************
  */

#include "log.h"
#include "bsp_qspi_flash.h"
#include "config.h"

static uint16_t s_count = 0u;       /* 当前有效记录条数 */

/* 序号 + 长度 + 正文 逐字节 //16 位累加，用来判断一条记录是否完整 */
static uint16_t Log_Checksum(const uint8_t *rec)
{
  uint16_t sum = (uint16_t)rec[0] + (uint16_t)rec[1];
  uint8_t  i;

  for (i = 0u; i < (uint8_t)LOG_TEXT_MAX; i++)
  {
    sum = (uint16_t)(sum + (uint16_t)rec[2u + i]);
  }
  return sum;
}


/* 第 index 条记录在 Flash 里的地址 */
static uint32_t Log_AddrOf(uint16_t index)
{
  return FLASH_LOG_BASE + ((uint32_t)index * LOG_RECORD_SIZE);
}

/* 一条记录是否有效：序号对得上、长度合法、校验和一致 
在从 Flash 读取一条日志记录后，通过三重校验，判断这条记录是否完整、合法。
*/
static uint8_t Log_IsValid(const uint8_t *rec, uint16_t index)
{
  uint16_t chk;

  if (rec[0] != (uint8_t)index)		//rec[0]是序号--》序号校验
  {
    return 0u;
  }
  if (rec[1] > (uint8_t)LOG_TEXT_MAX)		//正文的实际长度校验
  {
    return 0u;
  }

  chk = (uint16_t)rec[62] | ((uint16_t)rec[63] << 8);
  if (chk != Log_Checksum(rec))
  {
    return 0u;
  }

  return 1u;
}


//上电时扫描整个 Flash 日志区，统计出到底存了多少条有效记录，并更新系统状态
uint8_t Log_Init(void)
{
  uint8_t  rec[LOG_RECORD_SIZE];
  uint16_t i;

  s_count = 0u;

  /* 从第 0 条挨个读：遇到全 0xFF 或校验不过，就认为后面没有记录了 */
  for (i = 0u; i < LOG_MAX_RECORDS; i++)
  {
    if (Flash_Read(Log_AddrOf(i), rec, LOG_RECORD_SIZE) != FLASH_OK)
    {
      return FLASH_ERR;             /* QSPI 读失败 */
    }

    if (rec[0] == 0xFFu)            /* 擦除后的空位 */
    {
      break;
    }

    if (Log_IsValid(rec, i) == 0u)  /* 半条记录（掉电打断）或损坏：到此为止 */
    {
      break;
    }

    s_count++;
  }

  Config_SetCount(s_count);         /* 用 Flash 里的真实条数刷新 EEPROM 侧的状态 */
  return FLASH_OK;
}

uint16_t Log_GetCount(void)
{
  return s_count;
}


//将一条新的文本日志（text）打包成固定 64 字节的记录，并追加写入到 Flash 的下一个空位中。
uint8_t Log_Append(const char *text)
{
  uint8_t  rec[LOG_RECORD_SIZE];
  uint16_t chk;
  uint8_t  len = 0u;
  uint8_t  i;
  uint8_t  st;

  if (s_count >= LOG_MAX_RECORDS)
  {
    return 1u;                      /* 写满了，先 clear 再写 */
  }

  /* 统计正文长度（超长的截断到 60 字节） */
  while ((text[len] != '\0') && (len < (uint8_t)LOG_TEXT_MAX))
  {
    len++;
  }

  /* 组装这 64 字节 */
  rec[0] = (uint8_t)s_count;
  rec[1] = len;
  for (i = 0u; i < (uint8_t)LOG_TEXT_MAX; i++)
  {
    rec[2u + i] = (i < len) ? (uint8_t)text[i] : 0x00u;
  }

  chk     = Log_Checksum(rec);
  rec[62] = (uint8_t)(chk & 0x00FFu);
  rec[63] = (uint8_t)((chk >> 8) & 0x00FFu);

  /* 64 字节记录是页内对齐的，一条写不会跨页 */
  st = Flash_PageWrite(Log_AddrOf(s_count), rec, LOG_RECORD_SIZE);
  if (st == FLASH_OK)
  {
    s_count++;
    Config_SetCount(s_count);
  }

  return st;
}




uint8_t Log_ReadAt(uint16_t index, uint8_t *rec)
{
  if (index >= s_count)
  {
    return FLASH_ERR;
  }

  if (Flash_Read(Log_AddrOf(index), rec, LOG_RECORD_SIZE) != FLASH_OK)
  {
    return FLASH_ERR;
  }

  if (Log_IsValid(rec, index) == 0u)		//核对代码
  {
    return FLASH_ERR;
  }

  return FLASH_OK;
}




uint8_t Log_TextOf(const uint8_t *rec, char *out, uint16_t out_size)
{
  uint8_t len = rec[1];
  uint8_t i;

  if (len > (uint8_t)(out_size - 1u))
  {
    len = (uint8_t)(out_size - 1u);   /* 防止调用者给的缓冲太小 */
  }

  for (i = 0u; i < len; i++)
  {
    out[i] = (char)rec[2u + i];
  }
  out[len] = '\0';

  return len;
}

uint8_t Log_Clear(void)
{
  uint8_t st = Flash_EraseLogArea();

  if (st == FLASH_OK)
  {
    s_count = 0u;
    Config_SetCount(0u);
  }

  return st;
}
