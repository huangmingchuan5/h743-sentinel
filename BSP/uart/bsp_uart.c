/**
  ******************************************************************************
  * @file    bsp_uart.c
  * @brief   USART1（115200 8N1）调试串口 BSP。
  *
  *          TX：
  *            - printf()/fputc：逐字节阻塞发送，只留给"调度器启动前"的代码
  *              （banner 等）；调度器跑起来之后的任务里改用下面这套：
  *            - Uart_Printf / Uart_Write / Uart_Putc：
  *                取 uartMutex → HAL_UART_Transmit_IT → 等 txDoneSem
  *                （发送完成中断给出）→ 释放锁；
  *                一整串输出期间持有互斥量，多任务打印不会互相插花。
  *
  *          RX：
  *            - HAL_UART_Receive_IT 逐字节中断接收 → 256 B 环形缓冲；
  *            - 每收到一个字节通知一次 s_rxNotifyTask（vTaskNotifyGiveFromISR）；
  *            - 任务侧用 Uart_RxGet() 从缓冲里取字节（不阻塞）。
  *
  *          中断优先级：USART1 = 5（见 usart.c 的 MspInit）。
  ******************************************************************************
  */

#include "bsp_uart.h"
#include "usart.h"
#include "semphr.h"
#include <stdio.h>
#include <stdarg.h>

#define UART_RX_BUF_SIZE   256u            /* 必须是 2 的幂（用位与代替取模） */

/* ---- TX 侧 RTOS 对象（调度器启动前创建） ---- */
static SemaphoreHandle_t s_uartMutex = NULL;   /* 串口互斥量：保证一整串输出不被打断 */
static SemaphoreHandle_t s_txDoneSem = NULL;   /* 发送完成信号量：HAL 回调里给出 */

/* ---- RX 侧：环形缓冲 ---- */
static volatile uint8_t  s_rxBuf[UART_RX_BUF_SIZE];
static volatile uint16_t s_rxHead = 0u;        /* ISR 写入位置 */
static volatile uint16_t s_rxTail = 0u;        /* 任务读出位置 */
static uint8_t           s_rxByte = 0u;        /* HAL 单字节接收的落点 */
static TaskHandle_t      s_rxNotifyTask = NULL;/* 收到字节后通知哪个任务 */
static volatile uint16_t s_rxDropped = 0u;     /* 缓冲满丢弃计数 */

/* -------------------------------------------------------------------------- */
/* 调度器启动前可用：printf 重定向（阻塞逐字节）                               */

int fputc(int ch, FILE *f)
{
  uint8_t byte = (uint8_t)ch;

  /* 阻塞发送 1 个字节，超时 1000 ms */
  HAL_UART_Transmit(&huart1, &byte, 1, 1000);

  (void)f;   /* f 未使用，这样写可以让编译器不报警告 */
  return ch;
}

/* -------------------------------------------------------------------------- */
/* TX：RTOS 版本                                                              */

void Uart_RtosInit(void)
{
  s_uartMutex = xSemaphoreCreateMutex();
  s_txDoneSem = xSemaphoreCreateBinary();
}

/* 内部函数：调用者必须已持有 uartMutex
*  多个任务共用 UART1 时，不会同时调用 HAL_UART_Transmit_IT 导致发送冲突或数据交错。
*/
static void Uart_Tx_Locked(const uint8_t *buf, uint16_t len)
{
  if (len == 0u)
  {
    return;
  }

  if (HAL_UART_Transmit_IT(&huart1, (uint8_t *)buf, len) == HAL_OK)
  {
    (void)xSemaphoreTake(s_txDoneSem, pdMS_TO_TICKS(1000));   /* 等"发送完成"中断 */
  }
}

void Uart_Write(const void *buf, uint16_t len)
{
  if (s_uartMutex == NULL)             /* 调度器还没起来（不该在此时调用） */
  {
    return;
  }

  if (xSemaphoreTake(s_uartMutex, portMAX_DELAY) == pdPASS)
  {
    Uart_Tx_Locked((const uint8_t *)buf, len);
    xSemaphoreGive(s_uartMutex);
  }
}

void Uart_Putc(uint8_t c)
{
  Uart_Write(&c, 1u);
}

void Uart_Printf(const char *fmt, ...)
{
  char    buf[192];
  va_list ap;
  int     n;

  if (s_uartMutex == NULL)
  {
    return;
  }

  if (xSemaphoreTake(s_uartMutex, portMAX_DELAY) != pdPASS)
  {
    return;
  }

  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (n < 0)
  {
    n = 0;
  }
  if ((size_t)n >= sizeof(buf))        /* 超长时 vsnprintf 会截断，这里修正长度 */
  {
    n = (int)(sizeof(buf) - 1u);
  }

  Uart_Tx_Locked((const uint8_t *)buf, (uint16_t)n);
  xSemaphoreGive(s_uartMutex);
}

/* -------------------------------------------------------------------------- */
/* HAL 回调（在 USART1 中断里执行）                                            */

//发送完成回调函数
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  BaseType_t woken = pdFALSE;

  if (huart == &huart1)
  {
    xSemaphoreGiveFromISR(s_txDoneSem, &woken);
    portYIELD_FROM_ISR(woken);
  }
}


//接收完成回调函数
//这个回调把 UART1 收到的每个字节放进环形缓冲区，重新武装接收，并通知任务来处理。它是 UART 接收中断与任务之间的桥梁。
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  BaseType_t woken = pdFALSE;		//HighPriorityTaskWoken

  if (huart == &huart1)
  {
	//x & (UART_RX_BUF_SIZE - 1u)	等价于	x % UART_RX_BUF_SIZE			x % UART_RX_BUF_SIZE		(但位与运算比取模运算快得多，所以嵌入式环形缓冲区常用这种写法。)
	  //等同于对256取模，只不过这个更快
	  uint16_t next = (uint16_t)((s_rxHead + 1u) & (UART_RX_BUF_SIZE - 1u));		//后继指针next(指向下一个可以写入的位置)

    if (next != s_rxTail)
    {
      s_rxBuf[s_rxHead] = s_rxByte;
      s_rxHead = next;
    }
    else
    {
		s_rxDropped++;                   /* 缓冲满：丢掉这个字节并计数 ，但是并不停止接收*/
    }

    (void)HAL_UART_Receive_IT(&huart1, &s_rxByte, 1u);   /* 重新武装"收下一个" *///只要没停止就可以继续接收从而循环往复

    if (s_rxNotifyTask != NULL)
    {
      vTaskNotifyGiveFromISR(s_rxNotifyTask, &woken);		//通知任务来处理(通知ConsoleTask()来进行处理)，给出信号量
      portYIELD_FROM_ISR(woken);
    }
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart == &huart1)
  {
    /* 清掉溢出/校验/帧/噪声等错误标志，重新武装接收，别让接收中断停摆 */
    __HAL_UART_CLEAR_FLAG(&huart1, UART_CLEAR_OREF | UART_CLEAR_NEF | UART_CLEAR_FEF | UART_CLEAR_PEF);
    (void)HAL_UART_Receive_IT(&huart1, &s_rxByte, 1u);		//中断方式接收UART数据
  }
}

/* -------------------------------------------------------------------------- */
/* RX：任务侧接口                                                             */

void Uart_RxStart(TaskHandle_t notifyTask)		//notifTask实际上传入了当前任务句柄(xTaskGetCurrentHandle())
{
  s_rxNotifyTask = notifyTask;
  (void)HAL_UART_Receive_IT(&huart1, &s_rxByte, 1u);
	
	//之后每次接收完成中断回调里会自己重新调用 HAL_UART_Receive_IT，形成“收一个 → 中断 → 重新武装 → 收下一个”的循环。
}

int Uart_RxGet(void)
{
  int c = -1;

  taskENTER_CRITICAL();
  if (s_rxTail != s_rxHead)
  {
	  
	//读取后把 s_rxTail 向前移动一格，并用掩码 & (UART_RX_BUF_SIZE - 1u) 实现环形回绕
    c = (int)s_rxBuf[s_rxTail];
    s_rxTail = (uint16_t)((s_rxTail + 1u) & (UART_RX_BUF_SIZE - 1u));
  }
  taskEXIT_CRITICAL();

  return c;
}

uint16_t Uart_RxDropped(void)
{
  return s_rxDropped;
}

/********************************* 文件结束 *******************************/
