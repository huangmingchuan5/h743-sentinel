/**
  ******************************************************************************
  * @file    health.h
  * @brief   Step 8：任务健康监控 + 独立看门狗（IWDG）。
  *
  *          - 每个任务周期/处理完业务后调用 Health_Report()；
  *          - HealthTask（优先级 1，1s）检查所有任务的"最近上报间隔"，
  *            全部合格才 HAL_IWDG_Refresh()，否则停喂 → 约 2s 后复位；
  *          - 复位后由 main 在启动时调用 Health_SetResetFlags() 保存复位原因。
  ******************************************************************************
  */

#ifndef __HEALTH_H
#define __HEALTH_H

#include "main.h"

typedef enum
{
	HEALTH_ENV = 0,   // 显式指定为 0
	HEALTH_MOT,       // 0 + 1 = 1
	HEALTH_UI,        // 2
	HEALTH_CON,       // 3
	HEALTH_LOG,       // 4
	HEALTH_LED,       // 5
	HEALTH_MON,       // 6
	HEALTH_COUNT      // 7
} HealthId_t;

/** 调度器启动前调用：初始化登记表 + 启动 IWDG（2s）+ 创建 HealthTask。 */
void Health_Start(void);

/** 任务上报"我还活着"（周期任务每圈、事件任务每次处理完业务）。 */
void Health_Report(HealthId_t id);

/** 1 = 所有任务健康；0 = 有任务超时未上报。 */
uint8_t Health_GetHealthy(void);

/** 最近一次检查中失败的任务编号（0xFF = 无）。 */
uint8_t Health_GetLastFail(void);

/** 保存/读取启动时抓到的 RCC 复位标志（main 在清标志前调用）。 */
void     Health_SetResetFlags(uint32_t rsr);
uint32_t Health_GetResetFlags(void);

/** wd test <s>：停止喂狗 s 秒（演示 IWDG 复位；约 2s 后复位）。 */
void Health_SetWdgTest(uint32_t seconds);

#endif /* __HEALTH_H */
