# SDM835 多核支持

支持在现有 APFS、launchd 和串口 Bash 启动链上运行完整 8 个 Kryo 280 核心。
按用户当前选择，项目默认以四核运行，后续调优后再评估八核；保留 watchdogd
对 backboardd 等服务的原生超时处理。

## Apple 框架与来源

`GenericARMCPU : IOCPU` 位于 GenericARMPlatform.kext，由 Apple IOKit 匹配
IOPlatformDevice、IONameMatch=cpu 和平台发布的 CPU 标记。沿用本版
IOCPUInterruptController、ml_processor_register、processor_boot 及
ml_cpu_init_completed；USE_APPLEARMSMP=0 时注册已启动 processor，不再重复启动。
原子位图拒绝同一逻辑 CPU 重复注册，关键虚函数槽由编译器契约验证。

Apple 开源参考为 [IOCPU](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/iokit/Kernel/IOCPU.cpp)、
[processor_boot](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/processor.c)、
[ARM machine 层](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/machine_routines.c)
及 [次级 CPU 入口](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/start.s)。
参考项目的 BCM2837 和 AppleVirtualPlatform 仅用于对照模型，构建不引用参考项目。

`ios18-model.json` 保存提供映像哈希和类结构：AppleARMCPU 继承 IOCPU，184 槽、
248 字节；IOCPU 为 184 槽、184 字节；IOCPUInterruptController 为 189 槽、184 字节。
原厂 personality 同样匹配 IOPlatformDevice/cpu。本实现直接继承本项目 XNU 的
IOCPU。硬件依据项目固定参考，并对照 [MSM8998 设备树](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/boot/dts/qcom/msm8998.dtsi)。

## 实现

- 两个 SMP 集群，affinity 为 0..3、0x100..0x103，逻辑编号来自 XNU 拓扑。
  设备树提供分集群 L2 ID、物理集群和核编号，当前默认 cpus=4。
  八个物理 CPU 节点和完整驱动能力保留，可通过 --cpus 8 显式启用。
- 当前源码选择本版 XNU 原生 Clutch/Edge，声明 AMP 并通过设备树的
  `cluster-type` 提供 E/P 类型。早期移植使用 dualq；下方 2/4/8 核速度数据
  属于该旧版。第 82 轮已确认 Edge 四核启动，后台调度有改善，但 SpringBoard
  仍超时；Edge 的八核 E/P 分配和性能控制策略尚未验证。
- SMC64 PSCI_CPU_ON 进入 Apple LowResetVectorBase，再使用 CpuDataEntries、
  原有页表、每核栈和 arm_init_cpu。检查固件版本、入口和 affinity，正确解释
  PSCI 的有符号 32 位返回值。
- 启核前将 ResetHandlerData、const_boot_args、sysreg_restore、cpu_ttep、
  bootstrap 页表和目标 CPU 数据清到 PoC，供 MMU/cache 关闭的核心读取。
- 启动核创建并以 release/acquire 发布 GICR 映射。每核按 GICR_TYPER 查找
  redistributor，拒绝缺失、重复 affinity 和不支持的布局。
- 每核唤醒 GICR，等待 ChildrenAsleep/RWP，清理 SGI0/PPI19 的旧 pending/active
  状态，设置优先级和 timer 电平触发，读回检查授权和使能。保留其他 PPI/SPI，
  等待受计数器期限和次数上限约束。
- 安全固件保留 GIC 安全扩展时（GICD_CTLR.DS=0），分组寄存器在 NS EL1
  读零、忽略写入，不能拿零值判定授权失败；保持固件分组，通过 NS 可见的
  priority、trigger 和 enable 读回验证。DS=1 时才配置 IGROUPR。
  依据 [Arm GIC-500 TRM](https://documentation-service.arm.com/static/5e9085b8c8052b1608761814)
  及 [Linux GICv3 实现](https://github.com/torvalds/linux/blob/v6.12/drivers/irqchip/irq-gic-v3.c)。
- 每核配置 ICC 系统寄存器、Group1 和 EOI 模式；SGI0 先 EOI 再处理软件信号。
  Aff1/TargetList 支持跨集群，deferred signal 立即发送，取消仅清软件位。
- IOCPU 初始化并设置 StartedState 后排队真实 self-SGI，由首次 IPI handler
  清除 SIGPdisabled，允许 XNU 跨核调度及 cross-call。CPU_ON 返回成功还不够。
- UARTDM 将 NCF_TX、状态检查和 FIFO 写入作为同一个加锁事务；console drain
  与 kprintf 共享输出锁，防止多核启动记录被切碎。panic 路径绕过可能由停核
  持有的发送锁，保持原生 panic 和 watchdog 恢复日志可用。
- SDM835 的引导 framebuffer 位于保留 DRAM。video console 使用 XNU 现成的
  VM_WIMG_WCOMB（本 ARMv8 目标的 MAIR 为 0x44，Normal Non-Cacheable）映射，
  避免 Device 内存逐像素写入拖慢串口与屏幕共享的输出路径。没有启用 Apple
  私有的 UC 异常特性，也没有将 GIC/UART 等寄存器改为 Normal 内存。
  编码对照 [ARM64 内存类型](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/include/asm/memory.h)
  及 [ARM64 ioremap_wc](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/include/asm/io.h)。

`[ARM SMP]` 分别记录注册、实际 online、每核 GIC 和首次 timer/IPI。
CPU_ON 成功或设备树中存在八个节点不能单独证明多核运行。
系统 suspend、IOCPU halt 和异构性能调度沿用项目现有边界。

## 真机启动速度

本轮最终内核的 APFS collection SHA-256 为
`8a90b33eb7db58a00b49f16829c07e77800d25acedfa8a2b1c0b07ffb0a4d698`。
2/4/8 核使用同一内核、256 MiB EFI 内存窗口、System/Data、固件、trust cache
和诊断启动参数，保留屏幕与串口的全部日志。本轮按用户要求没有测试单核。

| 核心数 | EFI → Bash 横幅 | EFI → 实际 Bash 命令执行确认 |
| --- | ---: | ---: |
| 2 | 34.279 秒 | 40.834 秒 |
| 4 | 26.170 秒 | 27.870 秒 |
| 8 | 35.201 秒 | 36.368 秒 |

这是每档一次最终配置启动的主机计时，不能视为稳定中位数。调度顺序、服务
等待、缓存和 APFS I/O 会产生波动。当前样本中四核最快，较双核缩短约 32%；
八核可完整运行，但启动没有随核心数线性加速。
`make multicore-bridge` 配合现有 BRIDGE_FLAGS 默认生成四核载荷；需要后续八核
测试时使用 `CPUS=8`。四核配置对应簇 0，八核会加入簇 1；尚未单独测量跨簇
迁移、频率或缓存通信成本，不能将本轮速度差异归因于单一因素。

在显示映射修正前的同一版多核内核中，双核/四核分别为 111.854/64.406 秒；八核的
两个样本为 116.808/47.967 秒，其中首个样本在 Bash 横幅后等待约 52 秒才
显示提示符。这个等待在第二次启动没有复现，完整样本均保留，不将最快一次
作为稳定性能。修正后的八核样本约 36.4 秒，全部 CPU 注册在约 2.6 秒完成，
主要耗时在后续用户空间启动。当前 EFI 仅交给 XNU 256 MiB；日志中仍可见
既有 APFS checkpoint ENOSPC，未在本轮修改磁盘布局或用户空间服务策略。

最终结果、每核证据和产物哈希见 [validation.json](validation.json)；原始 UART
和时间戳位于 `build/multicore/hardware-{2,4,8}-fast-console/`。更早的同版多核
对照位于 `hardware-*-verified/` 和 `hardware-8-boot-repeat/`，对应载荷保存在
`build/multicore/attempts/device-framebuffer/`。

## 统一入口

```sh
make multicore-drivers
make test-multicore
make multicore JOBS=3
make multicore-bridge BRIDGE_FLAGS="...现有 APFS/固件/开发启动参数..."
python3 scripts/test-bridge.py \
  --payload build/multicore/bridge-4/XnuBridge.efi \
  --output build/multicore/hardware-4 --seconds 310 --bash-probe --expect-cpus 4 \
  --expect-backboardd-watchdog
```

multicore 构建内核与现有 apfs collection，输出 build/multicore/kernelcache 及
manifest、独立结构校验报告。multicore-bridge 调用现有 build-bridge.py，
--cpus N 替换已有 cpus= 并拒绝与 cpumask= 混用，无需复制 DTS。
交付载荷目录的 invocation.json 保存完整构建命令和单核配置来源。

默认直接复用开机进入的 EDK2 Fastboot，通过 profile 指定的 serial 查询产品并
下载。仅在该实例连接失效时使用 `--from-shell` 恢复，无需每轮重开 Fastboot。

先完成代码和离线校验，再经 EDK2 Fastboot 暂存并执行 EFI，按 2/4/8 核递增。
测试独立检查每核 registered/online/interrupts/timer/ipi 和 Bash 真实变量展开；
`--expect-backboardd-watchdog` 要求真实 Bash、指定核心数的全部证据、原生
backboardd timeout panic 报告和后续固件恢复都成立才返回成功。Bash 验证后
立即写入 boot-progress.json，完整 UART、时间戳和最终结果保存在 report.json。
寄存器模型覆盖八个 affinity、打乱的 GICR 顺序、
旧状态清理范围、超时、固件拒绝授权和错误布局。所有内核补丁从固定 Apple 归档
重放后，多核源码逐字节一致。设备证据位于 build/multicore/hardware-*。

`--benchmark` 自动验证 Bash，随后仅用 Bash 内建命令启动八个计算子进程，
每个执行 20,000 次相同的整数计算并检查最终值。父进程逐一 wait，三次均成功
才接受耗时中位数。比较各配置时保持总工作量、内核、固件、trust cache 和
用户空间一致，只更换 cpus=2/4/8。本轮按用户要求不进行单核测试。
脚本不向设备写入临时脚本。
启动速度以 EFI 入口到真实 Bash 命令执行确认的主机时间为主，另列横幅时间。
计时包含约一秒的串口命令发送/接收，不包含 USB 下载及进入 Fastboot 的时间。
日志输出、存储和服务等待会影响启动耗时，不能用日志滚动或计算吞吐推断
整个系统同等加速。
