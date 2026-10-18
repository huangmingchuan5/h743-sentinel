/**
  ******************************************************************************
  * @file    log.c
  * @brief   32B 快照环形日志实现（设计说明见 log.h）。
  ******************************************************************************
  */

#include "log.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include <string.h>

#define LOG_META_MAGIC    0x534E5431u   /* "SNT1" */
#define LOG_META_VERSION  1u

/* 64B 检查点（字段排布固定，末尾 CRC16） */
typedef struct
{
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t epoch;      /* 每次逻辑擦除 +1 */
  uint32_t wrAddr;     /* 下一条记录的绝对地址 */
  uint32_t seqNext;    /* 下一条记录的序号 */
  uint32_t recTotal;   /* 本次擦除以来累计条数 */
  uint8_t  rsv[38];
  uint16_t crc;
} LogMeta_t;

typedef char LogMeta_SizeCheck_t[(sizeof(LogMeta_t) == LOG_META_SIZE) ? 1 : -1];

/* ---- 运行状态 ---- */
static SemaphoreHandle_t s_flashMutex  = NULL;
static TaskHandle_t      s_loggerTask  = NULL;
static uint32_t          s_epoch       = 0u;
static uint32_t          s_wrAddr      = 0u;   /* 下一条记录的绝对地址 */
static uint32_t          s_seqNext     = 0u;
static uint32_t          s_recTotal    = 0u;
static uint32_t          s_metaSector  = LOG_META_A_ADDR;  /* 当前检查点扇区基址 */
static uint32_t          s_metaOffset  = 0u;   /* 扇区内下一个空槽偏移 */
static uint16_t          s_writeErrors = 0u;

/* -------------------------------------------------------------------------- */
/* 基础工具                                                                    */

static uint16_t Crc16(const uint8_t *data, uint32_t len)
{
  uint16_t crc = 0xFFFFu;
  uint32_t i;

  while (len-- > 0u)
  {
    crc ^= (uint16_t)((uint16_t)(*data++) << 8);
    for (i = 0u; i < 8u; i++)
    {
      if ((crc & 0x8000u) != 0u)
      {
        crc = (uint16_t)((crc << 1) ^ 0x1021u);
      }
      else
      {
        crc = (uint16_t)(crc << 1);
      }
    }
  }
  return crc;
}

/* 加锁：调度器启动前是单线程，直接放行（避免临界区哨兵把中断屏蔽卡住） */
static uint8_t Log_Lock(void)
{
  if (s_flashMutex == NULL)
  {
    return 1u;
  }
  if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
  {
    return 1u;
  }
  return (xSemaphoreTake(s_flashMutex, pdMS_TO_TICKS(1000)) == pdPASS) ? 1u : 0u;
}

static void Log_Unlock(void)
{
  if ((s_flashMutex != NULL) &&
      (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED))
  {
    (void)xSemaphoreGive(s_flashMutex);
  }
}

static void Log_BuildMeta(LogMeta_t *m)
{
  memset(m, 0, sizeof(*m));
  m->magic    = LOG_META_MAGIC;
  m->version  = LOG_META_VERSION;
  m->epoch    = s_epoch;
  m->wrAddr   = s_wrAddr;
  m->seqNext  = s_seqNext;
  m->recTotal = s_recTotal;
  m->crc      = Crc16((const uint8_t *)m, (uint32_t)(sizeof(*m) - 2u));
}

/* 在某个元数据扇区里找"最后一个有效检查点"。
   slotIndex 可传 NULL；1 = 找到。 */
static uint8_t Log_FindLastValid(uint32_t sectorBase, LogMeta_t *out, uint32_t *slotIndex)
{
  uint32_t  slot;
  uint8_t   found = 0u;
  LogMeta_t m;

  for (slot = 0u; slot < LOG_META_SLOTS; slot++)
  {
    uint32_t addr = sectorBase + (slot * LOG_META_SIZE);

    if (Flash_Read(addr, (uint8_t *)&m, LOG_META_SIZE) != FLASH_OK)
    {
      break;
    }
    if (m.magic != LOG_META_MAGIC)
    {
      continue;
    }
    if (m.version != LOG_META_VERSION)
    {
      continue;
    }
    if (m.crc != Crc16((const uint8_t *)&m, (uint32_t)(sizeof(m) - 2u)))
    {
      continue;
    }

    if (out != NULL)
    {
      *out = m;
    }
    if (slotIndex != NULL)
    {
      *slotIndex = slot;
    }
    found = 1u;
  }
  return found;
}

/* 读一条记录并校验 CRC；1 = 有效 */
static uint8_t Log_RecordValid(uint32_t addr, Sample_t *rec)
{
  if (Flash_Read(addr, (uint8_t *)rec, LOG_RECORD_SIZE) != FLASH_OK)
  {
    return 0u;
  }
  return (rec->crc == Crc16((const uint8_t *)rec, (uint32_t)(LOG_RECORD_SIZE - 2u))) ? 1u : 0u;
}

/* 提交一个检查点；当前扇区满时轮换到另一份。
   硬规则：当前份必须有有效检查点，才允许擦另一份。 */
static uint8_t Log_CommitMetaLocked(void)
{
  LogMeta_t m;
  uint8_t   buf[LOG_META_SIZE];
  uint8_t   rb[LOG_META_SIZE];

  if ((s_metaOffset + LOG_META_SIZE) > FLASH_ERASE_SIZE)
  {
    uint32_t other = (s_metaSector == LOG_META_A_ADDR) ? LOG_META_B_ADDR : LOG_META_A_ADDR;

    if (Log_FindLastValid(s_metaSector, NULL, NULL) == 0u)
    {
      return LOG_ERR;                 /* 当前份都无效：绝不擦另一份 */
    }
    if (Flash_EraseSector(other) != FLASH_OK)
    {
      return LOG_ERR;
    }
    s_metaSector = other;
    s_metaOffset = 0u;
  }

  Log_BuildMeta(&m);
  memcpy(buf, &m, sizeof(buf));

  if (Flash_PageWrite(s_metaSector + s_metaOffset, buf, LOG_META_SIZE) != FLASH_OK)
  {
    return LOG_ERR;
  }
  if ((Flash_Read(s_metaSector + s_metaOffset, rb, LOG_META_SIZE) != FLASH_OK) ||
      (memcmp(rb, buf, LOG_META_SIZE) != 0))
  {
    return LOG_ERR;                   /* 读回校验失败：不推进 offset */
  }

  s_metaOffset += LOG_META_SIZE;
  return LOG_OK;
}

/* 批量写数据（调用者持锁）：按页切片、跨扇区先擦除、写后读回校验。
   记录在写入时统一分配 seq + 计算 CRC。 */
static uint8_t Log_WriteBatchLocked(Sample_t *recs, uint16_t n)
{
  const uint32_t dataEnd = LOG_DATA_BASE + LOG_DATA_SIZE;
  uint16_t i = 0u;

  while (i < n)
  {
    uint32_t addr = s_wrAddr;
    uint32_t room;

    /* 进入新扇区：先擦除回收（该扇区是最旧的数据） */
    if ((addr % FLASH_ERASE_SIZE) == 0u)
    {
      if (Flash_EraseSector(addr) != FLASH_OK)
      {
        s_writeErrors++;
        return LOG_ERR;
      }
    }

    /* 一次页写不跨页；32B 记录天然对齐，不会切到半条 */
    room = FLASH_PAGE_SIZE - (addr % FLASH_PAGE_SIZE);
    if (room > ((uint32_t)(n - i) * LOG_RECORD_SIZE))
    {
      room = (uint32_t)(n - i) * LOG_RECORD_SIZE;
    }
    if ((addr + room) > dataEnd)
    {
      room = dataEnd - addr;
    }

    {
      uint16_t cnt = (uint16_t)(room / LOG_RECORD_SIZE);
      uint16_t k;
      uint8_t  tmp[FLASH_PAGE_SIZE];

      for (k = 0u; k < cnt; k++)
      {
        recs[i + k].seq = s_seqNext++;
        recs[i + k].crc = Crc16((const uint8_t *)&recs[i + k],
                                (uint32_t)(LOG_RECORD_SIZE - 2u));
      }

      if (Flash_PageWrite(addr, (uint8_t *)&recs[i], room) != FLASH_OK)
      {
        s_writeErrors++;
        return LOG_ERR;
      }
      if ((Flash_Read(addr, tmp, room) != FLASH_OK) ||
          (memcmp(tmp, (const uint8_t *)&recs[i], room) != 0))
      {
        s_writeErrors++;
        return LOG_ERR;
      }

      s_recTotal += cnt;
      s_wrAddr = addr + room;
      i = (uint16_t)(i + cnt);
    }

    /* 到数据区末尾：先回卷再提交检查点，保证恢复扫描不跨回卷点、且指针合法 */
    if (s_wrAddr == dataEnd)
    {
      s_wrAddr = LOG_DATA_BASE;
      if (Log_CommitMetaLocked() != LOG_OK)
      {
        return LOG_ERR;
      }
    }
  }
  return LOG_OK;
}

/* -------------------------------------------------------------------------- */
/* LoggerTask：攒批写盘（优先级 2）                                            */

static void LoggerTask(void *argument)
{
  static Sample_t batch[LOG_BATCH_MAX];   /* 1KB，放 BSS 不占任务栈 */
  uint16_t        n           = 0u;
  uint32_t        recsSinceMeta = 0u;
  TickType_t      lastFlush   = xTaskGetTickCount();
  TickType_t      lastMeta    = xTaskGetTickCount();

  (void)argument;

  for (;;)
  {
    TickType_t now     = xTaskGetTickCount();
    TickType_t elapsed = (TickType_t)(now - lastFlush);
    TickType_t remain;

    if (elapsed >= pdMS_TO_TICKS(LOG_FLUSH_TIMEOUT_MS))
    {
      remain = 0u;
    }
    else
    {
      remain = (TickType_t)(pdMS_TO_TICKS(LOG_FLUSH_TIMEOUT_MS) - elapsed);
    }

    /* 采样任务每投递一条就通知一次；Step 7 起报警事件也会通知 */
    (void)ulTaskNotifyTake(pdTRUE, remain);

    while ((n < LOG_BATCH_MAX) &&
           (xQueueReceive(Sample_QueueHandle(), &batch[n], 0u) == pdPASS))
    {
      n++;
    }

    now = xTaskGetTickCount();
    if ((n >= LOG_BATCH_MAX) ||
        ((n > 0u) &&
         ((TickType_t)(now - lastFlush) >= pdMS_TO_TICKS(LOG_FLUSH_TIMEOUT_MS))))
    {
      if (Log_Lock() != 0u)
      {
        if (Log_WriteBatchLocked(batch, n) == LOG_OK)
        {
          recsSinceMeta += n;
        }

        /* 检查点：每 256 条或 30s 提交一次 */
        if ((recsSinceMeta >= LOG_META_COMMIT_RECORDS) ||
            ((TickType_t)(now - lastMeta) >= pdMS_TO_TICKS(LOG_META_COMMIT_MS)))
        {
          if (Log_CommitMetaLocked() == LOG_OK)
          {
            recsSinceMeta = 0u;
            lastMeta = xTaskGetTickCount();
          }
        }
        Log_Unlock();
      }

      n = 0u;
      lastFlush = xTaskGetTickCount();
    }
    else if (n == 0u)
    {
      /* 没有待写数据：重置计时，避免 remain=0 时任务空转 */
      lastFlush = xTaskGetTickCount();
    }
  }
}

/* -------------------------------------------------------------------------- */
/* 对外接口                                                                    */

uint8_t Log_Init(void)
{
  LogMeta_t ma;
  LogMeta_t mb;
  uint32_t  sa = 0u;
  uint32_t  sb = 0u;
  uint8_t   va = Log_FindLastValid(LOG_META_A_ADDR, &ma, &sa);
  uint8_t   vb = Log_FindLastValid(LOG_META_B_ADDR, &mb, &sb);
  LogMeta_t *best = NULL;
  uint32_t   bestSector = 0u;
  uint32_t   bestSlot = 0u;

  if ((va != 0u) && (vb != 0u))
  {
    /* 取 (epoch, seqNext) 更大的那份 */
    if ((ma.epoch > mb.epoch) ||
        ((ma.epoch == mb.epoch) && (ma.seqNext >= mb.seqNext)))
    {
      best = &ma;
      bestSector = LOG_META_A_ADDR;
      bestSlot = sa;
    }
    else
    {
      best = &mb;
      bestSector = LOG_META_B_ADDR;
      bestSlot = sb;
    }
  }
  else if (va != 0u)
  {
    best = &ma;
    bestSector = LOG_META_A_ADDR;
    bestSlot = sa;
  }
  else if (vb != 0u)
  {
    best = &mb;
    bestSector = LOG_META_B_ADDR;
    bestSlot = sb;
  }

  if (best == NULL)
  {
    /* 没有任何有效检查点：当成新盘初始化 */
    (void)Flash_EraseSector(LOG_META_A_ADDR);
    (void)Flash_EraseSector(LOG_META_B_ADDR);

    s_epoch      = 1u;
    s_wrAddr     = LOG_DATA_BASE;
    s_seqNext    = 0u;
    s_recTotal   = 0u;
    s_metaSector = LOG_META_A_ADDR;
    s_metaOffset = 0u;

    return (Log_CommitMetaLocked() == LOG_OK) ? LOG_OK : LOG_ERR;
  }

  s_epoch      = best->epoch;
  s_wrAddr     = best->wrAddr;
  s_seqNext    = best->seqNext;
  s_recTotal   = best->recTotal;
  s_metaSector = bestSector;
  s_metaOffset = (bestSlot + 1u) * LOG_META_SIZE;

  /* 合法性保护：指针必须落在数据环内且 32B 对齐 */
  if ((s_wrAddr < LOG_DATA_BASE) ||
      (s_wrAddr >= (LOG_DATA_BASE + LOG_DATA_SIZE)) ||
      ((s_wrAddr % LOG_RECORD_SIZE) != 0u))
  {
    s_wrAddr   = LOG_DATA_BASE;
    s_seqNext  = 0u;
    s_recTotal = 0u;
  }

  /* 向前扫描"数据已写、检查点未提交"的记录（CRC 有效 + seq 连续） */
  {
    uint32_t addr    = s_wrAddr;
    uint32_t scanned = 0u;
    const uint32_t dataEnd = LOG_DATA_BASE + LOG_DATA_SIZE;

    while ((addr + LOG_RECORD_SIZE) <= dataEnd)
    {
      Sample_t rec;

      if (Log_RecordValid(addr, &rec) == 0u)
      {
        break;
      }
      if (rec.seq != s_seqNext)
      {
        break;
      }

      s_seqNext++;
      s_recTotal++;
      addr += LOG_RECORD_SIZE;
      scanned++;

      if (scanned >= LOG_CAPACITY)
      {
        break;                        /* 防御：不可能扫超过一圈 */
      }
    }

    s_wrAddr = addr;
    if (s_wrAddr >= dataEnd)
    {
      s_wrAddr = LOG_DATA_BASE;
    }
  }

  return LOG_OK;
}

void Log_Start(void)
{
  s_flashMutex = xSemaphoreCreateMutex();

  (void)xTaskCreate(LoggerTask, "LOG", 320u, NULL, 2u, &s_loggerTask);
}

void Log_Notify(void)
{
  if (s_loggerTask != NULL)
  {
    (void)xTaskNotifyGive(s_loggerTask);
  }
}

uint32_t Log_GetCount(void)
{
  return s_recTotal;
}

uint32_t Log_GetAvailable(void)
{
  return (s_recTotal > LOG_CAPACITY) ? LOG_CAPACITY : s_recTotal;
}

uint16_t Log_GetWriteErrors(void)
{
  return s_writeErrors;
}

uint8_t Log_FillDump(uint32_t startIndex, uint32_t n, Sample_t *out)
{
  uint32_t avail;
  uint32_t wr;
  uint32_t i;

  if (Log_Lock() == 0u)
  {
    return 3u;
  }

  avail = (s_recTotal > LOG_CAPACITY) ? LOG_CAPACITY : s_recTotal;
  if ((startIndex >= avail) || ((startIndex + n) > avail))
  {
    Log_Unlock();
    return 1u;
  }

  wr = s_wrAddr - LOG_DATA_BASE;          /* 当前写位置（环内偏移） */

  for (i = 0u; i < n; i++)
  {
    uint32_t idx  = startIndex + i;
    uint32_t back = avail - idx;          /* 距最新一条有多远（含自身） */
    uint32_t pos  = (wr + LOG_DATA_SIZE - (back * LOG_RECORD_SIZE)) % LOG_DATA_SIZE;

    if (Log_RecordValid(LOG_DATA_BASE + pos, &out[i]) == 0u)
    {
      Log_Unlock();
      return 4u;                          /* CRC 校验失败 */
    }
  }

  Log_Unlock();
  return 0u;
}

uint8_t Log_Erase(void)
{
  uint8_t st = LOG_ERR;

  if (Log_Lock() == 0u)
  {
    return LOG_ERR;
  }

  s_epoch++;
  s_wrAddr   = LOG_DATA_BASE;
  s_seqNext  = 0u;
  s_recTotal = 0u;

  /* 擦两份元数据，从 A 重新开始；同时擦干净数据区第一个扇区，便于立即写入 */
  if ((Flash_EraseSector(LOG_META_A_ADDR) == FLASH_OK) &&
      (Flash_EraseSector(LOG_META_B_ADDR) == FLASH_OK) &&
      (Flash_EraseSector(LOG_DATA_BASE) == FLASH_OK))
  {
    s_metaSector = LOG_META_A_ADDR;
    s_metaOffset = 0u;

    if (Log_CommitMetaLocked() == LOG_OK)
    {
      st = LOG_OK;
    }
  }

  Log_Unlock();
  return st;
}
