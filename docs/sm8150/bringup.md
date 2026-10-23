# Snapdragon 855 / SM8150 bring-up

本目录记录 SM8150 与原有 MSM8998/SDM835 平台的实际差异。当前实现覆盖
XNU 的平台配置、CPU affinity/PSCI/GIC/timer 接入、IOKit 平台服务，以及可选的
GENI UART 轮询控制台。`sm8150-generic.dts` 是移植模板，尚不是某款手机的完整板级支持。
没有 SM8150 实机启动、UART 波形或 USB 枚举测试结果。

资料固定为 Linux **v6.12**，提交
`adc218676eef25575469234709c2d87185ca223a`，便于重复审阅，不声称这是最新内核。
[upstream sm8150.dtsi](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/arch/arm64/boot/dts/qcom/sm8150.dtsi)
是地址、interrupt、CPU 和 PHY 类型的依据。源码快照、原始路径和 SHA-256 在
[sources.json](sources.json) 与 `reference/` 中；GENI 的寄存器事实来自同一提交的
`qcom_geni_serial.c`、`qcom-geni-se.c` 和 `geni-se.h`。

## 平台差异

| 项目 | MSM8998 / Snapdragon 835 | SM8150 / Snapdragon 855 |
| --- | --- | --- |
| CPU | Kryo 280，4+4 | Kryo 485，4+3+1 频率组 |
| CPU `reg` / MPIDR affinity | `0..3, 0x100..0x103` | `0x000, 0x100, …, 0x700`；Aff0 全部为零 |
| GICD | `0x17a00000`，`0x10000` 字节 | 相同 |
| GICR | `0x17b00000` | **`0x17a60000`**，总长 `0x100000` |
| 通用计时器 | 19.2 MHz，PPI 1/2/3/0 | 相同；中断 cell 保留 active-low 描述 |
| 调试 UART | UARTDM v1.4，sagit 的 `0x0c1b0000` | GENI，upstream uart2 为 `0x00a90000`，长度 `0x4000`，SPI 357 |
| 电源与时钟 | RPM、MSM8998 GCC | RPMh、SM8150 GCC；旧平台 ID/rail 表不适用 |
| USB2 PHY | QUSB2，`0x0c012000` | Synopsys 7nm HS PHY，`0x088e2000` / `0x088e3000` |

Linux 的 `cpu-map` 将 SM8150 八个核放在一个共享 DSU/L3 cluster 中；每核有独立
L2。这里的 XNU `die-cluster-id=0/1/2` 是对应 4+3+1 频率组的调度分组，不代表三个
物理 DSU。`cluster-core-id` 分别为 `0..3`、`0..2`、`0`，`l2-cache-id` 为 `0..7`。
PSCI `CPU_ON`、GICR 匹配和 SGI 路由始终使用真实 `reg` affinity，不把调度分组编号
写入 MPIDR。

`SM8150` Kext 匹配 `qcom,sm8150`，校验 SMC conduit、CPU affinity 唯一性及调度属性。
`SM8150IO` 匹配 `arm-io,sm8150`，校验 GIC 资源与 4 GiB 外设恒等映射窗口，并拒绝
UARTDM 设备声明。该窗口限制的是已准入外设 MMIO，不能用来推断设备的 RAM 容量。
不声明 `arm-io,generic` 回退，以免专用平台校验失败后另一个 personality 接管。

## 板级模板与启动前提

模板位于 `device_trees/sm8150-generic.dts`，共用
`device_trees/sm8150.dtsi`。默认 `cpus=1`，无 UART phandle/stdout-path，
`xnu,allow-no-uart=1` 显式允许无串口启动；UART 和两个 USB 控制器均为 disabled。
它不会复制 sagit 的 PMIC、GPIO、UFS、显示、Wi-Fi、SMEM 或保留内存布局。

板级移植需提供以下真实信息：

1. 启动固件可用内存、全部 secure/remoteproc/framebuffer 等保留区，以及 EL1/PSCI
   交接条件。EFI bridge 只使用固件分配的合法 arena；raw Image 入口必须得到真实内存
   映射。模板中的 memory/pram/DRAM 值都是零占位，不是可写物理地址。
2. 已开启的 GICv3 系统寄存器接口及可访问的 GICD/GICR；验证 19.2 MHz timer
   与固件实际 `CNTFRQ_EL0` 一致。先验证 boot CPU 和 timer，再通过构建参数逐步增加
   `cpus`，测试每个核的 PSCI 启动和双向 SGI。模板将 affinity 0 的 CPU 标记为
   `state="running"`；交接 CPU 不同的固件必须据实修改这个标记。
3. 与具体手机匹配的供电、显示、存储与 DMA/SMMU ownership。基础平台支持不能推导出
   Adreno 640、UFS、显示、基带等外设已经可用。
4. 真实的芯片 identity，或者已经验证的板级固件读取路径。通用模板故意不声明
   `unique-chip-id`/`chip-id`：零值占位会触发 bridge 现有的 MSM8998 Qualcomm ChipInfo
   protocol/vtable 调用，这套固件 ABI 尚未在 SM8150 上验证。缺失 identity 不影响基础
   GenericARMPlatform 接入；依赖 identity 的上层服务需要后续板级补充。

Linux/FDT 的大端 cell 必须经过 `scripts/build-bridge.py` 转换成 Apple DT；
`reg`、`ranges`、CPU `reg` 的编码不是直接透传。`dtc` 对两个零长度占位节点与
`soc@0` 的重复 unit address 会发出警告；它们由 bridge 填充，不应为消除警告而编造 RAM。

构建入口：

```sh
make test-sm8150
make sm8150 JOBS=4
make sm8150-drivers KERNELCACHE=/absolute/path/to/original/kernelcache
make sm8150-platform KERNELCACHE=/absolute/path/to/original/kernelcache
make sm8150-bridge
make test-usb-cdc TEST_FLAGS=--sanitize
```

`sm8150` 构建并检查 `build/output/mach.development.sm8150`；
`sm8150-drivers` 用本机已有、ABI 匹配的原版 kernelcache 构建
GenericARMPlatform/SM8150/AppleARMGIC；`sm8150-bridge` 在 `build/sm8150/bridge` 生成
加载器产物，默认嵌入 `sm8150-platform` 生成的基础平台 kernelcache，并把未移植的
MSM8998 watchdog 设置为关闭。该集合只包含基础平台与必要的原生依赖，不包含
SDM835 外设驱动或完整文件系统/用户态支持。`SM8150_KERNEL` 可选择其他已验证载荷；
单独的 `mach.development.sm8150` 不含平台 Kext，不能代替该集合。
`BRIDGE_ARGS` 可以传入已验证
的板级参数；不要把 sagit 的 USB/RPM/显示/UFS handoff profile 直接用于 SM8150。
这些目标只生成本地产物，不刷写设备。内核、Kext、加载器通过构建不代表完整 iOS 系统
或所有外设已经通过实机验证。

## 可选 GENI 控制台

`kernel/pexpert/pexpert/arm/qcom_geni_console.h` 实现独立于 IOKit 的 C 轮询后端，
供早期内核和 bridge 使用。它与 UARTDM 后端分开选择，不能只替换 UARTDM 的基地址。

调用方先取得该 serial engine 的独占控制权，并保证 MMIO 可访问、固件不再并发访问、
时钟/引脚/baud 设置会持续保留。驱动在任何写入前检查 UART protocol、FIFO 模式、
32-bit FIFO、4×8 little-endian packing、8N1、有效时钟 divisor 和 RX stale timeout。
不满足条件就拒绝接管，寄存器保持原状。启动前已在运行的 TX 也会被拒绝。

初始化成功后关闭本引擎的 TX/RX IRQ enable，必要时开始 UART RX。单字节 TX 使用
GENI `TX_TRANS_LEN`、main command 与 FIFO，等待次数有限；出错时只中止自己的 TX，
随后停用此后端。RX 处理 FIFO word 缓存及 `RX_LAST_BYTE_VALID=0` 表示四字节的规则；
遇到 parity、break、FIFO 错误或非法尾字节计数时停止接收，避免把错误数据送入调试器。
不支持 DMA、动态 baud、时钟/引脚初始化、低功耗恢复或 UART IRQ。

固件采用别的 FIFO packing、在交接时仍启用 DMA，或者关闭调试 UART 时，需要实现
相应的板级初始化；本后端不会猜测配置。平台默认关闭 GENI，启用开关只是声明交接前提，
不能让未供电的寄存器读取变得安全。

确认使用 upstream `uart2` 的板子可以在自己的 DTS 中添加：

```dts
&uart2 {
    status = "okay";
    qcom,geni-console-handoff = <1>;
};
/ {
    aliases { serial0 = &uart2; };
    // XNU reads defaults; the bridge reads chosen.
    defaults { serial-device = <&uart2>; };
    chosen {
        /delete-property/ xnu,allow-no-uart;
        serial-device = <&uart2>;
        stdout-path = "serial0:115200n8";
        bootargs = "-v debug=0x14a keepsyms=1 cpus=1 serial=3";
    };
};
```

如果实际板子选用另一个 GENI serial engine，必须使用该 engine 的真实地址、IRQ 和
交接配置；`qcom,geni-console-handoff` 是由板级负责人保证的资源声明，不是自动探测。
该后端保留固件 baud，`current-speed`/stdout 文本不会重新设置 baud divisor。

`tests/geni_console_test.c` 通过 emulated MMIO 覆盖错误协议/模式的零写入拒绝、正常 TX、
RX stale 尾部、缓存、非法字节数、错误状态和 TX/abort 双超时；测试通过
AddressSanitizer/UndefinedBehaviorSanitizer。这是软件逻辑验证，不能替代实机测试。
内核适配层对初始化、收发和缓存状态共用 trylock；同核调试器重入或其他 CPU 占用时
立即跳过本次操作，避免 panic 路径等待已经停住的锁持有者。读字符在同一个临界区
完成就绪检查与消费，写寄存器后有 DSB 屏障。
`tests/geni_adapter_test.c` 直接包含生产适配层，覆盖 DT 准入、MMIO 回调中的同核重入、
四个发送线程与一个接收线程并发运行；同样通过 ASan/UBSan。

## USB 与无 UART 调试

CDC ACM 是 DWC3 **device/gadget** 功能；USB host 串口适配器和 CDC gadget 不是同一层。
CDC class descriptor、控制请求、endpoint ring 和 console byte stream 可以共用，
但 SoC glue、PHY、电源、Type-C、DMA 隔离必须按平台选择。

| SM8150 路径 | DWC3 core / IRQ | QSCRATCH | SMMU stream | PHY |
| --- | --- | --- | --- | --- |
| Primary | `0x0a600000` / SPI 133 | `0x0a6f8800` | `0x140` | HS `0x088e2000`；QMP USB3+DP `0x088e8000` |
| Secondary | `0x0a800000` / SPI 138 | `0x0a8f8800` | `0x160` | HS `0x088e3000`；QMP USB3 uniPHY `0x088eb000` |

两个 core 窗口都是 `0xcd00`，QSCRATCH 都是 `0x400`。Primary QMP 窗口 `0x3000`，
secondary QMP `0x1000`。MSM8998 的 `0x0a800000` 控制器使用 SPI 131，地址碰巧相同
并不能证明 SM8150 secondary 与它等价。SM8150 upstream master clock 是 200 MHz，
而 MSM8998 是 120 MHz；两者的 GCC reset/gate ID、PMIC rail 以及 wake interrupt
也不能混用。

SM8150 USB 实机准入仍需一个匹配具体板子的后端：

- SM8150 GCC/RPMh 和 interconnect 生命周期；保留固件资源时应验证实际 ownership，
  不能把 `qcom,msm8998-*` handoff record 改名使用。
- Synopsys 7nm HS PHY 和对应 QMP PHY 的上电、reset、calibration，以及 Type-C mux/
  VBUS/orientation 路由。板子只引出 USB2 时可以限制 high-speed，但仍需要真实 HS PHY。
- Primary/secondary 的 SMMU stream/context、DMA carveout、缓存一致性和可证明的停止路径。
  旧平台 bypass/context 证明不能直接复用。
- 对真正连接的插座执行 device-mode ownership 交接、枚举/拔插/复位测试，再启用 CDC。

因此通用 SM8150 模板不自动开放 USB MMIO/PHY，不声称插线即可获得 `ttyACM*`。
可选 CDC 功能在驱动成功完成设备模式接管后可替代交互串口，但不提供该接管之前的
Boot ROM、EFI 或早期崩溃输出。早期无 UART 的调试应使用可回收的内存日志/显示记录，
并在 USB 就绪后读取。
