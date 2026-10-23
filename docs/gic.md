# AppleARMGIC

`drivers/platform/AppleARMGIC` 是独立编写的 GIC-500 / GICv3 非安全 EL1 驱动，
继承本项目 XNU 11417.140.69 的 `IOInterruptController`，目标为 iPhoneOS 18.6、
arm64/ALL。类名、bundle 和源码没有手机型号前后缀，也不链接参考项目中的文件。
这是项目自己的实现，bundle ID 为 `org.darwin.driver.AppleARMGIC`，不是 Apple 原厂二进制。

## 构建

```sh
make drivers TARGET=SDM835 JOBS=3
make test-drivers
make build TARGET=SDM835 JOBS=3
make verify TARGET=SDM835
make verify-drivers TARGET=SDM835
```

继续使用 `scripts/toolchain.py` 作为统一入口；没有增加构建脚本。现有平台 Kext
也由 `make drivers` 一起构建。产物是正常 Apple Kext 格式：

```text
build/output/Extensions/AppleARMGIC.kext/
  Contents/Info.plist
  Contents/MacOS/AppleARMGIC
build/output/verification-drivers.json
```

使用本版 XNU 的 `c_start.c` / `c_stop.c`、`KMOD_EXPLICIT_DECL`、C++ metaclass
初始化段和 ld64 的 `-kext`，直接生成 `MH_KEXT_BUNDLE`，不修改 Mach-O 头冒充 Kext。
编译禁用异常、RTTI 和 SIMD 使用。主中断控制器常驻；Kext stop 返回失败，
`requestTerminate` 拒绝运行中的卸载。

## Apple 匹配与设备树

主 personality 与后期虚拟平台一致：`IOClass=AppleARMGICv3`、
`IOProviderClass=AppleARMIODevice`、`IONameMatch=ARM,gicv3`、score 1000。
本项目 GenericARMIO 已通过原生工厂发布真实 AppleARMIODevice。
另有 score 900 的 `IOPlatformDevice` personality，供其他平台发布者使用；两个 personality 都走 Apple IOKit
匹配机制，不通过自建总线或机型字符串选择驱动。

GIC 的两个 MMIO 资源来自 provider，经 `mapDeviceMemoryWithIndex` 映射为设备内存。
驱动不硬编码物理地址。SDM835 的 Apple DT 必须与早期内核的 GICD/GICR 配置一致：

```text
/arm-io                          # ranges 使用 64-bit 小端 SoC 映射
  /gic
    name = "gic"
    compatible = "ARM,gicv3\0"
    device_type = "interrupt-controller\0"
    interrupt-controller = "master\0"
    AAPL,phandle = uint32 { 2 }
    #interrupt-cells = uint32 { 2 }
    reg = uint64[4] { 0x17a00000, 0x10000, 0x17b00000, 0x100000 }
  /device
    interrupt-parent = uint32 { 2 }
    interrupts = uint32[2] { 146, 1 }
```

这里是 **Apple 展平设备树**，所有数值小端，不是 Linux FDT。
specifier 是 `{绝对 INTID, kIOInterruptType}`：edge=0，level=1。
例如 UART SPI114 对应 INTID146，level 描述为 `{146,1}`。这不启用现有轮询串口的 IRQ。
Linux 的 `{0,114,4}` 三单元描述和 type=4 都会拒绝；不根据长度猜测 Linux ABI。
设备树发布者应通过 Apple `IODTMapInterrupts` 生成 `IOInterruptControllers` /
`IOInterruptSpecifiers`，或由 `IODTPlatformExpert` 的初始映射提供这些属性。

## 中断链与 ABI

```text
sleh_irq
  pe_sdm835_handle_irq -> ICC_IAR1_EL1（仅一次）
    INTID0  -> 提前 EOI -> cpu_signal_handler
    INTID19 -> rtclock_intr
    SPI     -> PE_dispatch_irq
                 PassthruInterruptController::handleInterrupt(source=INTID)
                   AppleARMGICv3::handleInterrupt
                     IOInterruptVector.handler(target, refCon, nub, source)
    dsb sy -> ICC_EOIR1_EL1（SPI / timer 各一次）
```

早期定时器和 SGI0 仍由 XNU 管理。Kext 接收已经确认的 SPI，不再读取 IAR 或写 EOI，
不会在 IOKit 加载时重置 distributor，也不短暂停用定时器/IPI。
1020..1023 特殊返回值不做 EOI。外设 handler 的设备写操作在 EOI 前由 DSB 完成。

`IOPlatformExpert::start` 在发布设备之前创建本版的 Apple
`PassthruInterruptController`；后者保留唯一 child，使用 release/acquire 发布
回调。`PE_dispatch_irq` 只用于内核内部，没有新增 Kext KPI。此接法对 SDM835
目标启用，VMAPPLE 的原有调用约定保持原样。

驱动直接使用本版 `IOInterruptVectorNumber`（int32）、`IOInterruptVector`、
`OSDefineMetaClassAndStructors` 和 Family 虚方法。不照搬 macOS 26 驱动私有 vtable。
注册检查 specifier 长度、编号、类型和重复占用，先完成硬件配置，再发布 handler。
一个 SPI 仅允许一个 consumer；enable/disable/unregister 使用本版 superclass 的
soft/hard disable 和 active 同步逻辑。未注册或软屏蔽 IRQ 到达时在硬件屏蔽，防止空转。

## 硬件处理与边界

- 支持 GICv3 PIDR2、GICD_TYPER 向量数发现、128 KiB GICR 帧及 boot CPU affinity 校验。
- SPI 32..1019 中，实际实现的向量才能注册；SGI/PPI 不作为外设向量暴露。
- 注册时屏蔽指定 SPI、等待 RWP、清理 pending、设置优先级及 edge/level，
  用 `GICD_IROUTER = 0x6000 + INTID*8` 路由到 topology 的 boot CPU。
- 修改配置字时由 controllerLock 保护，保留邻居向量；enable/pending 使用 W1S/W1C 单 bit。
- 不写 IGROUPR、IGRPMODR、DS 或安全组控制；firmware 必须已设置 Group1NS、
  ARE_NS、Group1NS forwarding 和 EL1 SRE。安全寄存器可能 RAZ/WI，不能通过组位读回
  推断归属；配置通过 priority/config/route 读回检查，失败拒绝注册。
- IPI 接口从逻辑 CPU 查询本版 XNU topology，按 Aff1/Aff0 编码 SGI0；不会把 CPU4
  错当 Aff0=4。deferred 按早期内核策略立即发送，取消不清硬件 SGI，避免丢掉并发 IPI。

当前不支持 v2、ITS/LPI、VLPIS、SPI affinity 迁移、共享 consumer 或电源塌陷后的恢复。
SPI 固定到 boot CPU，运行期间不能将该 CPU 下线；完整 suspend/resume 需后续平台电源驱动
配合。GIC Kext 不提供 CPU 枚举、PMGR、安全固件配置或 AppleARMPlatform 替代品。
必须将 Kext 及平台依赖放入后续 kernel collection；单独构建 bundle 没有自动装入内核。

## 验证与参考

`tests/gicv3_test.cpp` 运行生产 distributor 代码，覆盖全部 SPI 编号、两个 cluster 的
SGI 编码、Aff2/Aff3 编码、两单元 Apple 描述、非法类型/长度、邻居位保护、IROUTER
地址、secure RAZ/WI、RWP 延迟和超时。`make verify-drivers` 验证实际 Mach-O、metaclass、
匹配 plist，GIC 每个 import 均检查本版已声明 KPI exports 和链接内核中的实际符号。
完整内核验证额外要求 `_PE_dispatch_irq` 确实已链接。

初版验证记录见 [gic-validation.json](gic-validation.json)，其范围是离线编译与
寄存器模型。后续[多核真机验证](smp/README.md)已覆盖 2/4/8 核加载、每核
SGI0、timer PPI19、Bash 和原生 watchdog 恢复；外设 SPI 仍需按驱动单独验证。

参考依据：

- 本项目 `kernel/iokit/IOKit/IOInterruptController.h`、
  `Kernel/IOInterruptController.cpp`、`Kernel/PassthruInterruptController.cpp` 及本版 exports。
- `/home/xxiao/Projects/Project_iOS2ARM/docs/vma2-gic-family-re.md` 与
  `tools/macos-kernel-collection/examples/qemu-platform-compat/reverse_engineered/AppleARMGIC.hpp`：
  只参考 matching 和职责；没有引入新版私有 ABI、QEMU 地址、IPI 别名或来源文件。
- [Apple 开源 IOInterruptController](https://github.com/apple/darwin-xnu/blob/main/iokit/Kernel/IOInterruptController.cpp)：
  Kext Family 风格。实际编译接口以本项目固定版本为准。
- [Arm GICv3/v4 架构规范 IHI0069F](https://documentation-service.arm.com/static/6012f0024ccc190e5e68124f)：
  非安全寄存器视图、RWP、affinity routing、优先级与 EOI。
- SDM835 MMIO、CPU affinity 和定时器来源沿用 `docs/sdm835/sources.json` 的固定快照。
