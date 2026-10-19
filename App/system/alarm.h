/**
  ******************************************************************************
  * @file    alarm.h
  * @brief   Step 7：两段式报警（光照 / 运动）。
  *
  *          - 光照：ALS 低于阈值"连续 10 个 100ms 采样"（≈1s 确认窗）才触发；
  *          - 运动：相邻加速度样本差值超过阈值，泄漏计数器累计到
  *                  "1000ms ÷ 采样周期"（默认 50ms → 20 个样本 ≈1s 净扰动）才触发；
  *                  采样周期改变时确认窗仍保持 ≈1s（计数器阈值按周期换算）；
  *          - 读取失败的样本不参与判定：光照计数清零、运动基线作废，
  *            避免 I2C 报错被误判成"变暗/晃动"；
  *          - 触发后置事件组位、投递一条事件快照到 evtQueue（优先落盘）；
  *          - 30s 自动静音（只停蜂鸣器，红灯报警继续，直到 clear）。
  *
  *          阈值来自 config（cfg 命令可改），默认 thr_light=10、thr_motion=1200。
  ******************************************************************************
  */

#ifndef __ALARM_H
#define __ALARM_H

#include "main.h"
#include "FreeRTOS.h"
#include "task.h"

#define ALARM_CONFIRM_LIGHT_SAMPLES  10u      /* 10 × 100ms = 1s */
#define ALARM_CONFIRM_MOTION_MS      1000u    /* 运动确认窗：按采样周期换算样本数 */
#define ALARM_AUTOMUTE_MS            30000u   /* 30s 自动静音 */

/** 调度器启动前调用：创建自动静音软件定时器。 */
void Alarm_Init(void);

/** EnvTask（100ms）调用：喂入环境光原始值。valid=0 表示本拍读取失败（不判定）。 */
void Alarm_FeedLight(uint16_t als, uint8_t valid);

/** MotionTask（每个采样周期）调用：喂入三轴加速度原始值。
    valid=0 表示本拍读取失败（作废基线，不判定）。 */
void Alarm_FeedMotion(int16_t ax, int16_t ay, int16_t az, uint8_t valid);

/** 1 = 有报警（光照或运动，直到 clear）。 */
uint8_t Alarm_IsActive(void);

/** 1 = 蜂鸣器已静音（手动或 30s 自动）。 */
uint8_t Alarm_IsMuted(void);

/** 设置静音（1=静音，0=解除静音）。 */
void Alarm_SetMuted(uint8_t muted);

/** 清除报警（灭报警灯、停蜂鸣器、清事件位、记一条 CLEAR 事件）。 */
void Alarm_Clear(void);

uint8_t Alarm_LightActive(void);
uint8_t Alarm_MotionActive(void);

/** 最近一次触发报警时的 tick（UiTask 用来量"确认 → 声光"延迟）。 */
TickType_t Alarm_GetTriggerTick(void);

#endif /* __ALARM_H */
