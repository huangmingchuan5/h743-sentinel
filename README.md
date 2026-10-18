# H743 Sentinel

基于 **STM32H743XI（Cortex-M7）** 的多任务环境与姿态监测记录仪：在 CubeMX 生成的 HAL 工程上**手动移植 FreeRTOS V9.0.0**，把裸机轮询控制台改造为事件驱动任务，三路 I2C 传感器共享总线并发采样，数据以定长快照写入 QSPI Flash。

> **当前进度**：Step 5.5（采样管线 + RTC 时间戳 + 中止台事件化）已完成并上板验证；
> Step 6（掉电安全日志升级）、Step 7（报警与 UI）、Step 8（HealthTask + IWDG）进行中。
> 本仓库只包含已完成并验证过的部分，未完成的设计在 [路线图](#路线图) 中列出。

---

## 亮点

- **手动移植，不是套模板**：内核文件、`FreeRTOSConfig.h`、`SysTick` 接管、HAL 时基重分配全部手改并逐项验证，提交历史按 Step 拆分（`git log --oneline` 可看到完整施工顺序）
- **时基冲突处理**：SysTick 交给内核后，HAL 时基迁至 TIM7（TIM6 已被呼吸灯占用），并删掉 `SysTick_Handler` 里残留的 `HAL_IncTick()`，避免 tick 被"双喂"
- **中断优先级一致性**：NVIC 4 位分组，`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`，所有调用 `...FromISR()` 的中断统一为 5
- **控制台事件化**：RX 改中断 + 256 B 环形缓冲 + 任务通知；TX 改 `HAL_UART_Transmit_IT` + 完成信号量 + 带互斥的 `Uart_Printf`，长输出分块、锁不跨输出
- **共享总线**：EEPROM / AP3216C / MPU6050 三器件挂在同一条 I2C4 上，用互斥量保护并统一句柄
- **自研代码约 5.9k 行**（`App/` + `BSP/` + `Core/Src` + `User/`，不含 HAL 与 FreeRTOS 内核）

---

## 硬件

| 器件 | 型号 | 接口 / 地址 |
|---|---|---|
| MCU | STM32H743XIH6（Cortex-M7，480 MHz） | — |
| 光照 + 接近 | AP3216C | I2C4，0x1E |
| 六轴 IMU | MPU6050 | I2C4，0x68 |
| EEPROM | 24C02（8 B 配置页） | I2C4，0x50 |
| 外部 Flash | W25Q256（32 MB） | QSPI |
| 板载 | 3 路 PWM 呼吸灯（PB0/PB1/PA3）、TIM6 10 ms 中断、RTC + VBAT | — |

---

## 软件架构

```
USART1 IRQ ──► 256B ring ──► task notify ──► ConsoleTask (prio 3)
                                                │  Uart_Printf (uartMutex + txDoneSem)
I2C4 (共享总线, i2cMutex)
   ├─ AP3216C ─┐
   ├─ MPU6050 ─┼─► EnvTask (prio 6, 100 ms)  ─┐
   └─ EEPROM   │   MotionTask (prio 5, 50 ms) ─┴─► logQueue (128 × 32 B)
                                                │
                                            LoggerTask (Step 6，未实现)
                                                │
                                          QSPI W25Q256
```

### 任务表（当前实现）

| 任务 | 优先级 | 栈（word） | 周期 | 职责 |
|---|---|---|---|---|
| `ENV` | 6 | 160 | 100 ms | 读光照 / 接近，更新环境快照 |
| `MOT` | 5 | 160 | 50 ms | 读加速度，合成 20 Hz 快照入队 |
| `CON` | 3 | 512 | 事件驱动 | 命令行解析、回显、行编辑（任务通知唤醒） |
| `LED` | 2 | 128 | 500 ms | 心跳灯（`vTaskDelayUntil`） |
| `MON` | 2 | 256 | 3 s | 打印 tick/HAL tick 增量、heap、`vTaskList` |

> 栈水位（`uxTaskGetStackHighWaterMark`）与 30 min 采样丢点统计：**待补**（见[路线图](#路线图)）。

### 缓冲区与参数

| 参数 | 值 | 说明 |
|---|---|---|
| `SAMPLE_QUEUE_LEN` | 128 | logQueue 深度，20 Hz 下约 6.4 s 缓冲 |
| 快照大小 | 32 B | `seq(4) + 时间戳(4) + als(2) + ps(2) + ax/ay/az(6) + flags(2) + 保留 + CRC16(2)` |
| `configTOTAL_HEAP_SIZE` | 48 KB | `heap_4` |
| `configMAX_PRIORITIES` | 32 | 应用最高优先级 6 |
| `configCHECK_FOR_STACK_OVERFLOW` | 2 | 调试期开启 |
| 时间戳 | RTC 日历 / 上电 tick | 无有效 RTC 时退化为 boot tick，`flags` bit15 = 0 标识 |

### 资源占用（AC5 V5.06u7 实编译）

| 项 | 大小 |
|---|---|
| Code | 47.9 KB |
| RO-data | 1.7 KB |
| RW-data | 0.2 KB |
| ZI-data | 51.3 KB（含 48 KB FreeRTOS heap） |
| 编译结果 | **0 Error(s), 0 Warning(s)** |

---

## 串口命令

波特率 115200-8-N-1，行编辑支持退格与超长丢弃，命令大小写不敏感。

| 命令 | 作用 |
|---|---|
| `help` | 命令列表 |
| `info` | 版本、配置、log 计数 |
| `live <n>` | 打印 n 条实时快照（时间戳 / 光照 / 接近 / 加速度 / 新鲜度） |
| `sensor` | 传感器状态与原始值 |
| `i2c scan` | 扫描 0x08–0x77，应看到 0x1E / 0x50 / 0x68 |
| `time` / `time set YYYY-MM-DD hh:mm:ss` | RTC 读取 / 设置（LSE，掉电由 VBAT 保持） |
| `log` / `dump <n\|all>` / `clear` | 日志查看 / 导出 / 擦除 |
| `color` / `pwm` / `save` / `load` | 灯效与配置（存 EEPROM） |
| `qtest` | 队列压力测试 |

---

## 快速开始

1. **工具链**：Keil MDK 5 + **ARM Compiler 5（V5.06u7）** + MicroLIB；已安装 `Keil.STM32H7xx_DFP`
   > FreeRTOS V9.0.0 的 `port/RVDS/ARM_CM7` 是 armcc 语法，**不要切到 AC6**（切了编不过）。
2. 打开 `MDK-ARM/demo2_1.uvprojx` → Rebuild → 下载
3. 串口 115200 连接，输入 `help`
4. `i2c scan` 确认三个器件在位，`live 5` 看实时数据，`time set 2026-10-18 10:00:00` 校时

> 仓库不含 Keil 的 `Objects/`、`Listings/` 与 `.uvoptx`，clone 后首次打开需重新选择调试器（ST-Link / J-Link）。

---

## 目录结构

```
App/        应用层：console（命令行）、sample（采样任务）、log（日志）、Config（EEPROM 配置）
BSP/        板级驱动：uart、i2c（总线互斥）、sensor（AP3216C/MPU6050）、flash（QSPI）、ee、rtc、led、pwm
Core/       CubeMX 生成的 HAL 初始化与中断向量（stm32h7xx_it.c 中 SysTick 已交给内核）
FreeRTOS/   内核 V9.0.0：src / include / port(RVDS ARM_CM7) / heap_4
User/       FreeRTOSConfig.h
docs/       移植施工指南与设计记录
LICENSES/   第三方许可证全文
```

---

## 设计取舍与踩坑记录

| 问题 | 现象 | 处理 |
|---|---|---|
| HAL 时基与内核抢 SysTick | 接上 FreeRTOS 后 `HAL_GetTick()` 与实际时间不符 | 时基改 TIM7；`SysTick_Handler` 只调 `xPortSysTickHandler()`，删掉 `HAL_IncTick()` |
| BASEPRI 泄漏 | TIM6/TIM7 中断被长期屏蔽，`uxCriticalNesting` 哨兵值掩盖了问题 | 调度器启动前的 I2C 操作跳过互斥量；RTOS 对象创建后显式清 BASEPRI |
| 三器件共享 I2C | 传感器读取与 EEPROM 保存并发时偶发 NACK | 统一 I2C4 句柄 + `i2cMutex`（带 100 ms 超时），所有访问走锁封装 |
| QSPI 4 字节地址模式 | 32 MB 容量下线性读超过 16 MB 边界失效 | 初始化时进入 4-byte address mode |
| 阻塞式 `printf` 拖慢任务 | 长输出时其他任务被饿死 | TX 改中断 + 完成信号量；`dump` 分块输出，锁不跨输出 |

---

## 已知限制

- 日志模块仍是旧的 64 B × 256 条线性追加（4 个扇区），**掉电安全与环形覆盖在 Step 6 实现**
- 未启用 IWDG（Step 8），当前靠 `MON` 任务人工观察
- 报警链未实现（Step 7），`evtQueue` / 事件组尚未接入
- RTC 未接电池时时间戳退化为上电 tick，`flags` bit15 置 0 区分

---

## 路线图

| Step | 内容 | 状态 |
|---|---|---|
| 0–4 | 时基迁移、内核移植、控制台任务化 | ✅ 完成 |
| 5 | I2C 总线锁、三传感器驱动、20 Hz 采样、RTC | ✅ 完成 |
| 6 | 32 B 快照 + 元数据双副本 + 环形覆盖 + 跨页切片 | 🚧 进行中 |
| 7 | 事件组 + evtQueue、两段式报警、UI / 按键 | ⏳ 计划 |
| 8 | HealthTask 健康判据 + IWDG、故障注入、长跑 | ⏳ 计划 |
| — | README 截图与实测数据（`v0.2`） | ⏳ 待补 |

---

## 致谢

- FreeRTOS 内核与移植模板来自 [FreeRTOS](https://www.freertos.org/) V9.0.0 与野火 H743 Pro 教学例程（仅作移植参考）
- 传感器寄存器操作参考野火《AP3216C 全功能例程》《MPU6050 基本数据读取》，**代码为本项目重写**（自有 API、总线加锁、错误码）
- HAL / CMSIS 由 STM32CubeMX 生成

## 许可

本项目自研代码（`App/` `BSP/` `User/` `docs/`）采用 **MIT**，见 [LICENSE](LICENSE)。
`FreeRTOS/`、`Drivers/`、`Core/` 等第三方组件按各自许可证分发，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
