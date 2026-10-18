# Third-Party Notices

本仓库自研代码（`App/`、`BSP/`、`User/`、`docs/`）以 **MIT** 许可发布，见根目录 `LICENSE`。
以下组件版权归各自作者所有，按各自许可证分发，**不适用**根目录 MIT。

---

## 1. FreeRTOS Kernel V9.0.0

| 项 | 内容 |
|---|---|
| 位置 | `FreeRTOS/src/`、`FreeRTOS/include/`、`FreeRTOS/port/` |
| 版权 | Copyright (C) 2016 Real Time Engineers Ltd. / FreeRTOS.org |
| 许可 | **GNU GPL v2.0 + FreeRTOS exception**（`GPL-2.0 WITH FreeRTOS-exception`） |
| 全文 | `LICENSES/GPL-2.0.txt`、`LICENSES/FreeRTOS-exception.txt` |
| 来源 | https://www.freertos.org/ |

> 注意：V9.0.0 采用 GPLv2 + exception，**不是 MIT**（FreeRTOS 自 V10.0.0 起才改为 MIT）。
> 例外条款允许把内核与其他许可的组件组合分发，而不要求这些组件整体开源；
> 内核源码本身仍受 GPLv2 约束。

## 2. STM32H7 HAL Driver / CMSIS Device

| 项 | 内容 |
|---|---|
| 位置 | `Drivers/STM32H7xx_HAL_Driver/`、`Drivers/CMSIS/Device/ST/STM32H7xx/` |
| 版权 | Copyright (c) 2017-2026 STMicroelectronics. All rights reserved. |
| 许可 | **BSD-3-Clause** |
| 依据 | `Drivers/STM32H7xx_HAL_Driver/LICENSE.txt`：随 STM32Cube 包分发时按 `Package_license`；本仓库未包含该文件，故适用 BSD-3-Clause |
| 全文 | `LICENSES/BSD-3-Clause.txt` |

## 3. STM32CubeMX 生成代码

| 项 | 内容 |
|---|---|
| 位置 | `Core/Inc/`、`Core/Src/`（`main.c` 等）、`MDK-ARM/startup_stm32h743xx.s` |
| 版权 | Copyright (c) 2026 STMicroelectronics |
| 许可 | BSD-3-Clause（同 STM32Cube 组件条款） |
| 说明 | 生成文件中标注为用户代码区（`USER CODE BEGIN/END`）的部分由本项目编写，按 MIT 发布 |

## 4. CMSIS Core / CMSIS Include

| 项 | 内容 |
|---|---|
| 位置 | `Drivers/CMSIS/Include/`、`Drivers/CMSIS/Core/` |
| 版权 | Copyright (c) 2009-2019 Arm Limited |
| 许可 | **Apache License 2.0**（全文见 `Drivers/CMSIS/LICENSE.txt`，副本 `LICENSES/Apache-2.0.txt`） |
| 来源 | https://github.com/ARM-software/CMSIS_5 |

## 5. 传感器驱动

`BSP/sensor/bsp_ap3216c.*`、`BSP/sensor/bsp_mpu6050.*` 为本项目自行编写：

- 寄存器地址、位定义、上电时序取自 AP3216C / MPU-6050 **公开数据手册**（事实性信息，不受版权保护）
- 开发过程中参考了野火（WildFire）教学例程的寄存器操作思路
- 代码结构、函数命名、错误码与 `bsp_i2c_bus` 总线加锁逻辑均为本项目原创，**未原样复制**第三方源文件

> 野火的教学例程、数据手册 PDF、原理图与板卡图片**未包含**在本仓库中。

## 6. 工具链相关（不随仓库分发）

Keil MDK 工程文件（`.uvprojx`、`.dbgconf`、`RTE_Components.h`）由 Keil uVision 生成；
`FreeRTOS/port/RVDS/ARM_CM7/` 为 FreeRTOS 官方 RVDS/armcc 移植层（随内核分发）。
仓库不包含 Keil 安装包、Device Family Pack 或 ARM Compiler。
