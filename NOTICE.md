# 来源与许可

- BMS 业务结构、五串三元锂默认阈值、OCV 表与均衡策略参考 PLKJ Development Team 的 `bms-education` RT-Thread 工程。改编源码保留原项目署名，原许可为 CC BY-NC 4.0，全文见 `LICENSE-ORIGINAL`。
- `third_party/FreeRTOS-Kernel` 固定在官方 V11.3.1（提交 `3a22924e0a9ddbbc8b0758881c33b3422a5cc20d`），其 MIT 许可见该目录 `LICENSE.md`。
- `board/stm32f103` 复制并整理了课程 FreeRTOS 板端工程的 BMS、STM32 初始化及驱动源码；课程作者与非商业许可同上。该目标使用本项目固定的 FreeRTOS V11.3.1 内核，不复制课程内核。
- 板端 STM32F1 HAL 与设备头文件保留 STMicroelectronics 著作权和 BSD-3-Clause 声明，许可全文见 `board/stm32f103/Drivers/STM32F1xx_HAL_Driver/LICENSE.md`；CMSIS Core 与 CMSIS-RTOS2 封装的 Apache-2.0 许可全文见 `board/stm32f103/Drivers/CMSIS/LICENSE.txt`。
- PySide6 由环境安装，未复制到项目。使用时须遵守其 [许可条款](https://doc.qt.io/qtforpython-6/licenses.html)。Arm GNU Toolchain 也不随项目分发。
- 本项目的电池模型用于软件逻辑演示，未经实板、采样精度或实时性能验证。
