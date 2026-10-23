此目录只保存硬件连接、UART 电源脉冲、IBS 和 QCA 固件格式的上游参考。
文件版本、来源与 SHA-256 见 sources.json；统一工具链在验证驱动时校验这些文件。

Linux C 驱动及 MSM8998 DTS 使用原文件标注的 GPL-2.0；设备树绑定使用
GPL-2.0 OR BSD-2-Clause。相应许可文本随附，原文件版权标记保持原样。
这些参考源码不参与 Kext 编译或链接。新写的 Apple IOKit 框架与协议组件位于
drivers/bluetooth/QcomBluetooth，使用其自身的 BSD-2-Clause 许可。
