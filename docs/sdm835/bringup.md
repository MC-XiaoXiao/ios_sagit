# SDM835 / 小米 6 XNU 目标

当前项目基于 Apple XNU 11417.140.69，已有 Linux 本地 arm64 Mach-O 工具链。
新增 `DEVELOPMENT_ARM64_SDM835`，继续使用 iPhoneOS 18.6 内核 SDK。
构建完整链接的 XNU 内核。现已由 EDK2 EFI 桥在 Mi 6 真机验证内核入口、
页表初始化、Darwin/VM 串口日志和屏幕文字，详见 [引导桥](boot-bridge.md)。

```sh
make collect-sdm835
make test
make build TARGET=SDM835 JOBS=3
make verify TARGET=SDM835
# 原虚拟平台仍可选择
make build TARGET=VMAPPLE JOBS=3
```

产物为 `build/output/mach.development.sdm835`、同名 `.unstripped` 和
`verification-sdm835.json`。带符号副本保留 UART、GIC 和 PSCI 实现，验证器
要求这些符号实际链接进去，检查 Mach-O 的入口、架构及未定义符号。

## 硬件资料

[结构化数据](hardware.json)及[下载来源与 SHA-256](sources.json)记录硬件依据。
Linux 资料固定到 v6.12；小米 sagit-n-oss 固定到提交
`3fb35ef1f0fe1f714be3df39724cbc8eb217181e`，参考源码原样保存在 `reference/`。
`make collect-sdm835` 校验现有副本，缺失时按固定 URL 恢复，校验失败不会覆盖现有文件。
这些 Linux/厂商文件保留各自 GPL 许可证，仅作为参考资料；没有作为 XNU 编译单元。

| 项目 | 参数 | 依据 |
| --- | --- | --- |
| CPU | 8 个 Kryo 280，MPIDR 0..3、0x100..0x103 | Linux msm8998.dtsi CPUs |
| GICD | 0x17a00000，大小 0x10000 | Linux / 小米 msm8998.dtsi |
| GICR | 0x17b00000，大小 0x100000，步长 0x20000 | 同上 |
| 虚拟定时器 | INTID 19（PPI 3），19.2 MHz | 小米 msm8998.dtsi timer |
| 串口 | UARTDM v1.4，0x0c1b0000，大小 0x1000 | 同上 uartblsp2dm1 |
| 串口中断 | SPI 114 / INTID 146，本驱动不启用 | Linux msm8998.dtsi |
| 串口引脚 | GPIO4、GPIO5，blsp_uart8_a | 小米 msm8998-pinctrl.dtsi |
| Mi 6 串口选择 | uartblsp2dm1，状态 ok | 小米 sagit-msm8998-mtp.dtsi |
| DRAM | 基址 0x80000000，实际大小由 loader 提供 | Linux msm8998.dtsi memory |

SDM835 的 CPU 特性使用保守配置：ARMv8-A + CRC + Crypto，4 KiB 内核及用户
页表，40 bit 物理地址，两个物理 cluster。SDM835 的软件页大小固定为 4 KiB，
避免 Apple 默认按内存容量把软件页大小切换到 16 KiB。没有声明 Apple PAC、AIC、Apple
PMU、SME、range TLBI、硬件 PAN 或 Apple CPU_OVRD。未接入异构 AMP 调度；
CPU 节点使用普通 SMP cluster 类型。CPU 频率和可用特性仍需通过真机寄存器确认。
多核实现、当前验证记录和性能对比见[多核支持](../smp/README.md)。该目标选择
XNU 原生 dualq 调度器管理两个物理集群。

## UART 实现与设备树约定

`pexpert/arm/pe_serial_uartdm.inc` 接入 XNU 的 `pe_serial.c`，增加
`SERIAL_QCOM_UARTDM`。通过 `/chosen/serial-device` 的 phandle 选择 UART 节点。
该接口接收 **Apple 展平设备树**，不是 Linux FDT；不能把 `.dtb` 直接交给 XNU。
设备树属性使用主机小端数据：

```text
/arm-io
  name = "arm-io"
  device_type = "soc"
  ranges = uint64[3] { 0, 0, 0x100000000 }
  /uart
    name = "uart"
    phandle = uint32 { 1 }
    compatible = "qcom,msm-uartdm-v1.4\0"
    reg = uint64[2] { 0x0c1b0000, 0x1000 }
/chosen
  serial-device = uint32 { 1 }
```

`reg` 是 SoC 基址的偏移和长度；本例的 SoC 基址为 0，已允许该物理平台使用
零基址。不要在该轮询 UART 节点上要求中断或 BAM DMA。

初始化屏蔽 UART IRQ、关闭 DMA、设置 8N1、清除错误并启用收发；保留启动加载器
设置的 CSR 波特率分频、GCC 时钟及 TLMM 引脚复用。loader 必须先使能 UART 的
core/iface 时钟、选择正确引脚并设置波特率（例如 115200）。本内核不猜测 GCC
时钟寄存器；冷态、未初始化 UART 不满足启动接口约定。

每个发送字符设置 NCF_TX=1；接收缓存完整 32 bit FIFO 字的 4 个字节。短输入
通过 FORCE_STALE 刷出，等待 ISR stale 完成，按 RX_TOTAL_SNAP 扣除已读取字节。
这样延迟完成、刷新过程中追加的字节和非 4 字节长度输入都不会当作填充字节返回。
UARTDM 使用 IRQ 安全的发送事务锁；console 与 kprintf 共用输出锁。
驱动不提供独立用户态 tty 设备，Bash 使用现有 `/dev/console`。

## GIC、PSCI 与启动接口

`osfmk/arm64/sdm835.c` 初始化本 CPU redistributor，只开启 Group1NS 的虚拟
定时器 INTID 19 和 SGI0；`sleh_irq` 经 ICC_IAR1/EOIR1 分发定时器与 IPI。
SGI 用 MPIDR Aff1/Aff0 路由，deferred IPI 立即发送，取消只清理软件 pending bit。
PSCI CPU_ON/CPU_OFF 使用 SMC；CPU_ON 进入 XNU 的 LowResetVectorBase，使用已有
ResetHandlerData/const_boot_args 进入 secondary 启动路径。每核通过真实 self-SGI
启用 XNU 软件信号，当前真机结果见[多核支持](../smp/README.md)。

loader/安全固件必须允许 EL1 的 GIC system register 接口，允许对应 SGI/PPI
配置，启用 distributor 的非安全 affinity routing 和 Group1NS forwarding，并
提供正确 CNTFRQ、CNTVOFF、CNTKCTL 和 CNTV_* 访问。错误 SRE、找不到 redistributor、
wake 超时和 PSCI 错误会报告 panic。外设 SPI 现在经 Apple PassthruInterruptController
分发到 [AppleARMGICv3 Kext](../gic.md)；驱动加载前的未知 IRQ 仍报告 panic。

进入 Mach-O 入口需要构造本版本 `boot_args`、建立要求的虚拟/物理映射并提供
Apple DT。`/cpus` 中各 CPU 的 `reg` 为上述 MPIDR，启动核带 `state="running"`，
`timebase-frequency=19200000`；实际内存与保留区必须来自目标设备固件。
`hardware.json` 中的 upstream 保留区只能用于核对，不能替代真机保留区。
EFI 引导桥已提供 Mach-O/boot_args 加载及标准 FDT 到 Apple DT 的转换；
原始 ARM64 入口和 Android boot v0 包装也已生成。原厂 ABL 的直接启动仍待验证。

从原始 XNU 归档重放所有补丁，已确认 35 个受影响文件与工作源码一致（含引导诊断、双控制台和 panic 输出后端）。

IOKit CPU 枚举已接入 IOCPU 和 IOCPUInterruptController；其他平台/驱动工作包括
SPI 真机验证、GCC/RPM/PMIC 电源、
UFS、USB、显示、触控、SMMU、无线和 suspend/resume。Apple SART/UAT 的空 bootstrap
仅表示硬件不存在，不能代替高通 SMMU 或 GPU 驱动。现有 SDK/运行时适配边界仍见
项目 README；此内核没有提供完整 iOS 运行环境。

## 验证

`make test` 运行生产 UART 代码的寄存器/FIFO 模型测试及真实目标头的编译断言。
覆盖发送、1/2/3 字节短输入、完整 FIFO 字、连续输入、延迟 stale、刷新中追加字节、
溢出恢复、错误 reg 长度/对齐/溢出和缺失 reg。断言检查独立物理平台、CPU 拓扑、
4 KiB 页表、40 bit PA 和禁止的新 ISA 功能。

SDM835 已完成完整编译及 Mach-O 静态校验，早期构建的 SHA-256 和验证结果记录在
[validation.json](validation.json)。模拟与静态校验不能
单独确认真实 UART、PSCI 或 GIC 的时序。后续真机引导证据见
[bridge-validation.json](bridge-validation.json)。这些文件保存早期验证范围；最新
SMP、APFS、Bash 和 watchdog 证据见[多核支持](../smp/README.md)。
