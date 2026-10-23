# 驱动目录

驱动按功能分类。Kext、对应的用户态适配和 DriverKit 扩展放在同一功能类别。

| 目录 | 模块 |
| --- | --- |
| `platform/` | AppleARMGIC、GenericARMPlatform、SDM835、SM8150、QcomSMEM、QcomWatchdog |
| `bus/` | GenericI2C、GenericSPI、QcomI2C、QcomSPI、QcomGPIO、QcomInterconnect、QcomSMMU、QcomSPMI |
| `clock/` | GenericClockController、QcomGCC、QcomGPUCC、QcomMMCC、QcomOSM |
| `power/` | QcomPMIC、QcomPowerController、QcomRPM、QcomBattery |
| `security/` | GenericAES、QcomSCM |
| `storage/` | QcomUFS |
| `usb/` | QcomUSB |
| `display/` | QcomDisplay、QcomMIPIDSI、PMI8998WLED |
| `gpu/` | QcomGPU、QcomMTL 用户态 Metal 插件 |
| `audio/` | QcomAudio 及 ADSP 后端 |
| `input/` | FingerprintHID、GoodixFingerprint、FPCFingerprint、QcomButtons、SynapticsTouch、SPIGyro、PMI8998Haptics |
| `wifi/` | QcomWLAN、QcomWLANDriver DriverKit 扩展 |
| `bluetooth/` | QcomBluetooth、`userspace/` 中的传输和 BlueTool 适配 |
| `baseband/` | QcomBaseband |
| `include/` | 跨模块协议、Apple 接口声明和公共头文件 |
| `shared/` | 内核与驱动共用的代码 |
| `abi-22H374/` | iPadOS 18.7.10 样本的内核、Metal、QuartzCore 和 DriverKit ABI 契约 |

`Build.json` 统一维护相对此目录的构建、测试和集合路径。构建器递归读取
`Info.plist`，只将 `CFBundlePackageType=KEXT` 的模块作为内核扩展构建；
Metal 和 DriverKit 继续使用各自的构建入口。

ABI 目录使用来源固件的 `ProductBuildVersion`。当前样本是 `22H374`；
Kext 内的 `DTPlatformBuild=22H368` 是组件编译标记，仍保留在契约中。
固件版本及 kernelcache 哈希见[样本记录](../docs/sdm835/ios18-kext-analysis.md#构建与样本)。

本目录提供基于 Apple IOKit 的平台专家、CPU 服务和 SoC 总线。
SDM835 的早期 UART、timer、SGI 和 PSCI CPU_ON/OFF 由本项目 XNU machine 层提供。
本阶段交付代码、arm64 Kext 和离线校验的 prelinked kernelcache。

## 类与依赖

| 类 | 基类 | 职责 |
| --- | --- | --- |
| `PMI8998Haptics` | Apple `IOService` | PMI8998 direct LRA、SPMI、定时/强度/制动、故障锁定和受限诊断客户端；见[振动文档](../docs/haptics/README.md)，默认禁用，未真机测试 |
| `QcomAudio` | 原生 iOS 18 `IOAudio2Device` | 原生用户客户端、PCM 输出映射、启停、通知和 PM；具体 `QcomADSP` 后端支持保留固件的 GLINK/APR/ASM 路径，实际板级交接待完成，见[音频输出文档](../docs/audio/README.md) |
| `GenericARMPlatform` | 原生 `AppleARMPE` | 平台初始化、CPU/顶层节点发布、根电源域、PSCI reset/off |
| `SDM835` | `GenericARMPlatform` | SMC 固件约定、MSM8998 CPU affinity 与重复节点校验 |
| `GenericARMCPU` | Apple `IOCPU` | 按 XNU 已解析的拓扑注册 processor，完成 CPU 初始化通知 |
| `GenericARMIO` | 原生 `AppleARMIO` | 校验 SoC ranges、发布原生 `AppleARMIODevice`、解析 MMIO |
| `SDM835IO` | `GenericARMIO` | 40-bit PA、零基址 4 GiB SoC aperture、Mi 6 UART 资源约定 |
| `GenericClockController` | Apple `IOService` | 通用时钟/电源/电压协议、串行事务、IOKit PM、IOReport 和受限用户客户端 |
| `QcomGCC` | `GenericClockController` | MSM8998 外设 RCG2 调频、分支门控、GDSC vote 和局部复位 |
| `QcomPowerService` | `GenericClockController` | QcomPowerController Kext 内的公共 MMIO、跨控制器调用及错误处理 |
| `QcomRPM` / `QcomSPMI` | `QcomPowerService` | GLINK-RPM 电压/level vote 与 AP-owned PM8005 电压管理 |
| `QcomInterconnect` | Apple `IOService` | MSM8998 USB NoC QoS、GCC 引用、RPM 带宽/根时钟和复位交接保护 |
| `QcomUSB` / `QcomUSBResources` / `QcomUSBPHY` | Apple `IOService` | USB 角色 nub、资源聚合与 PHY；原生 USB Family 适配仍在实现 |
| `QcomPMICRTC` | Apple `IORTC` | PM8998 原始秒计数交接、单调时钟推进及 `IORTC` 资源发布 |
| `QcomBattery` / `QcomBatteryPowerSource` | Apple `IOService` / `IOPMPowerSource` | PMI8998 FG Gen3/SMB2 只读遥测、原生属性与变化通知；见[电池文档](../docs/battery/README.md)，未真机验证 |
| `QcomMMCC` / `QcomGPUCC` | `QcomClockBackend` | 共用时钟后端，电压排序、PLL/RCG/门控、GDSC 与复位 |
| `QcomOSM` | `QcomPowerService` | 校准 LUT、XNU perfcontrol 自动策略、CPU 目标频率/上限、RootDomain 状态 |
| `QcomDisplay` | 原生 iOS 18 `IOMobileFramebufferLegacy` | 固定模式 LCD、IOSurface 提交、MDP 双缓冲和真实 VBL |
| `QcomMIPIDSI` | 原生 `AppleARMMIPIDSIController` | 6G DSI 命令 DMA、短/长包收发及 Apple device nub 发布 |
| `QcomPanel` | Apple `IOService` | Apple MIPI device 上的通用面板初始化与 DCS 亮度 |
| `PMI8998WLED` | Apple `IOService` | PMI8998 WLED4 背光，经 QcomSPMI 保留资源交接；关屏/唤醒及亮度保存与恢复 |
| `QcomGPU` | Apple `IOService` | Adreno 540 CP 提交、IOMapper DMA、硬件栅栏与超时隔离；需要固件/mapper 交接 |
| `QcomWLAN` | Apple `IOService` | WCN3990 SNOC 传输框架、受限 IOUserClient、PM 和资源事务；硬件后端及原生 WLAN 接口待接通 |
| `QcomBluetooth` | Apple `IOService` | WCN3990 UART/H4/IBS 初步框架、受限 IOUserClient、PM 与资源回滚；真实 UART/固件和原生蓝牙栈后端待接通 |
| `GenericI2CController` / `GenericI2CDevice` | Apple `IOService` | 统一 I²C 事务串行化、地址设备发布和独占客户端接口 |
| `QcomI2C` | `GenericI2CController` | sagit QUP5 tag v2、FIFO/block PIO、GCC 时钟引用和有界错误恢复 |
| `SynapticsTouch` | 原生 `IOHIDDevice` | RMI4 F01/F11/F12 多指报告、抬手与恢复，接原生 IOHIDEventDriver |

`AppleARMPlatform` 是 `com.apple.driver.AppleARMPlatform` 的 Kext 名称；平台专家
C++ 基类是 `AppleARMPE`。本项目保留原生 Kext 的标识、类和依赖闭包。
`GenericARMIO` 通过原生 `AppleARMIO::createDeviceWithProvider` 工厂建立真实
`AppleARMIODevice`，保持 Apple 外设 personality 的 provider-class 匹配。

私有接口声明放在 `include/AppleARMPlatform`，是当前 provider 的子类 ABI 投影。
provider 是本项目 iPad7c 缓存中的 arm64 AppleARMPlatform 1.0.2，内部 SDK 18.7，
build 22H368；项目编译 SDK 为 18.6。契约记录映像 SHA-256、实例大小、父类、槽数和
经过审阅的虚函数符号绑定。更换缓存必须重新审阅契约，版本字符串相同也不能跳过。

对照了参考项目中的 BCM2837 IOKit 总线实现，以及用户提供的符号完整映像
`Project_iOS2ARM/artifacts/macos26.6.2/hypervisor/kexts/com.apple.driver.AppleVirtualPlatform`。
后者把 `AppleVirtualPlatformARMPE`、`AppleVirtualPlatformIO`、`ApplePSCI` 分开，
是本项目分层的参考。macOS 26 的 arm64e ABI 包含额外 PSD/reserved 槽；本项目使用
iOS arm64 实际恢复的布局：AppleARMPE 296 bytes / 215 slots，AppleARMIO
216 bytes / 186 slots，AppleARMIODevice 216 bytes / 178 slots。

通用平台启动路径调用 XNU `IOPlatformExpert::start` 和 `IODTPlatformExpert`
的发布机制，避免调用 AppleARMPE 的 Apple 专用 RTC/PMGR 等待路径。
RTC 由 Qualcomm PMIC 实现；当前没有实现 Apple PMGRAON 安全锁存。
AMFI 对该硬件的开发适配必须由启动桥显式启用并记录，见 APFS 启动记录。
CPU 使用 `IOCPU`、`IOCPUInterruptController`、`ml_processor_register` 和
`ml_cpu_init_completed`；SDM835 的 machine 层负责实际启动、定时器和 IPI。
每核 online 后通过真实 self-SGI 启用软件信号；GICR 和 reset 数据发布、
跨集群调度及真机/速度证据见[多核支持](../docs/smp/README.md)。
此 CPU 路径面向 `USE_APPLEARMSMP=0` 的 XNU 板级后端。新的 ARM 板需要对应
machine 层实现和设备树；仅更换 compatible 不能补齐一个新的 SoC。

## Apple 匹配与设备树

蓝牙使用 `qcom,wcn3990-bt` 匹配与独立 `bluetooth` collection；构建、协议测试、
iOS 18 内核/用户态依据和实现边界见 [蓝牙驱动文档](../docs/bluetooth/README.md)。
运行 `make bluetooth-driver`、`make bluetooth-client`、`make test-bluetooth`、
`make bluetooth`、`make verify-bluetooth`，继续使用统一工具链。

`QcomWatchdog` 匹配 `qcom,msm-watchdog`，只接受已验证的 MSM8998
`0x17817000/0x1000` 寄存器范围和 32765 Hz 时钟。驱动继承固件原厂 `IOWatchdog`，每 5 秒先运行原生监控检查再喂狗，
硬件 31 秒未收到复位计数器写入即重启；20 位阈值拒绝溢出。
关机关闭硬件，重启保留 5 秒后备复位；实际系统重启/关机由平台 PSCI 路径执行。
`qcom-watchdog-test=1` 为显式硬件自测参数：驱动 arm 后不喂狗，用于验证到期复位。
原厂框架提供 watchdogd 用户客户端、root/entitlement 检查和签到超时策略；
`apple-watchdog-abi.json` 对提供映像及私有子类 ABI 做固定校验。
`handoff-76` 已验证独立后端的喂狗及 panic 后硬件复位；框架接入后的结果见启动记录。

sagit 的标准 FDT 仍用 `soc` 表示 SoC，总线转换后为 Apple DT 的 `/arm-io`，
`compatible` 为 `arm-io,sdm835` / `arm-io,generic`，地址和大小单元均为 2，
`ranges` 与子设备 `reg` 转为 ARM64 Apple DT 的小端 64 位元组。
`SDM835IO` 在此节点上发布原生 `AppleARMIODevice`，SPMI 控制器匹配其
`qcom,spmi-pmic-arb` 子设备。没有使用 Apple SoC 的寄存器地址。

sagit 通过 SPMI 节点的 `qcom,pmic-rtc=<0 0x6000>` 指定 PM8998 的 slave 和
RTC peripheral。QcomSPMI 在公开控制器之前只读 enable 和 seconds 寄存器，
检查读取错误并处理秒计数低字节翻转。启动样本交给同一 Kext 内的
`QcomPMICRTC`，此后用 XNU uptime 推进，取时路径不阻塞 SPMI 事务；只有后端
准备好才发布 `IORTC`。平台专家的取时/设时方法转发给这个原生 `IORTC` 子类。
短于 1 ms 的 QcomPowerService 硬件轮询使用单调计数器忙等，避免早期
`IODelay` 转为线程等待或依赖 Apple 的 WFE 超时行为。

当前不修改 PMIC 时间/闹钟，设时修正只保存在 RAM。PMIC 原始秒数并不等于
经过校准的 UTC；厂商 Android/UEFI 偏移尚未导入，不能把本次等待修复当成
日历时间校准。运行 `make test-drivers TEST_FLAGS="--test rtc"`，真机结果见
[RTC 验证记录](../docs/sdm835/ios18/rtc-validation.json)。

所有 personality 使用 `IOProviderClass`、`IONameMatch`、`IOPropertyMatch` 和
`IOProbeScore`。SDM835 专用项优先级 1000，通用平台项为 100。
CPU personality 仅匹配通用平台发布并标记的 CPU nub，优先级 10000。
`cpus=N` 排除的 CPU 不发布为可绑定 nub。

设备树为 Apple 展平 DT。数值使用小端；根与 arm-io 均显式声明
`#address-cells=2`、`#size-cells=2`。CPU 的 `reg` 按本版 XNU 的接口使用 32-bit MPIDR。

```text
/
  compatible = "qcom,msm8998\0xiaomi,sagit\0generic,arm-platform\0"
  #address-cells = uint32 { 2 }
  #size-cells = uint32 { 2 }
  /psci
    compatible = "arm,psci-0.2\0"
    method = "smc\0"
  /cpus
    /cpu0 ... /cpu7
      device_type = "cpu\0"
      reg = uint32 { 0..3, 0x100..0x103 }  # 每节点一个值
      state = "running\0"                 # 仅实际 boot CPU
      timebase-frequency = uint32 { 19200000 }
  /arm-io
    compatible = "arm-io,sdm835\0arm-io,generic\0"
    #address-cells = uint32 { 2 }
    #size-cells = uint32 { 2 }
    ranges = uint64[3] { 0, 0, 0x100000000 }
    /uart
      compatible = "qcom,msm-uartdm-v1.4\0"
      reg = uint64[2] { 0x0c1b0000, 0x1000 }
```

CPU 的其余拓扑、boot state、内存、保留区和 chosen 属性必须由 loader 按实际固件
提供。GIC 的 phandle、中断单元和资源见 [GIC 文档](../docs/gic.md)。
`IODTMapInterrupts` 在 Apple DT 导入时生成标准中断属性。

总线先验证所有启用的子节点，再创建、attach 并发布，失败会回收未发布 nub。
`reg`、`ranges` 检查长度、对齐、溢出和物理范围，禁用节点不发布。
外设时钟、电源域、RPM/PMIC 电压和 OSM CPU 档位通过 `GenericClockController`
对接各 Qualcomm 后端。loader 交接资源所有权、板级接线和安全供电点；
未交接的写操作返回不支持。
Apple gate/pin 数组仍被拒绝，不将 Apple gate ID 当作 Qualcomm 寄存器。
引脚、受保护 OSM/CPRh 校准和 PLL 模拟初始化仍需固件；电压/数字时钟由已交接的
后端管理，详见 [时钟与电源管理](../docs/power.md)。
系统 sleep 未启用；未知 SError 继续由 XNU 报告。平台/CPU/总线服务常驻。

USB NoC 通过同一串行 RPM 服务提交 64 位平均带宽和根时钟请求；开启全局时钟缩放前，
必须逐项确认完整 MSM8998 时钟交接基线。未提供真实配置时不发布 NoC 资源。
USB 构建和验证仍共用 `scripts/toolchain.py`；实现边界见 [USB/OTG 文档](../docs/usb/README.md)。

Wi-Fi 使用独立 `wifi` collection 和既有构建入口。`make wifi-driver`、
`make wifi-client`、`make wifi-driverkit`、`make test-wifi`、`make wifi`、`make verify-wifi` 用于
Kext、DriverKit DEXT、iOS 诊断客户端的构建、离线测试及集合校验。iOS 18 模型、
已编译的固件/DMA/CE/DriverKit 组件、原生 WLAN 桥接及剩余平台绑定见 [Wi-Fi 文档](../docs/wifi/README.md)。

## 统一构建与产物

```sh
make drivers
make test-drivers
make verify-drivers
make kernelcache
make verify-kernelcache
```

入口为 `scripts/toolchain.py`。工具自动发现子目录的 `Info.plist`，读取实际的
CFBundleIdentifier/Executable；支持 C 和 C++ Kext。使用 XNU 的 kmod stubs、
元类初始化段以及 ld64 `-kext`，生成 `MH_KEXT_BUNDLE`。
C++ Kext 当前要求常驻，避免宣称尚未实现的元类卸载生命周期；C-only Kext 可按配置卸载。

具体公共 include 路径、宿主测试、外部 ABI 契约和 collection roots 放在
`Build.json`。分析阶段根清单放在 `tools/kext-collections.json`。
工具代码不按 Kext 名称或具体类名分支。`inspect-abi` 输出通用类/符号/槽清单，
Python 校验器按契约筛选；KPI 检查遵循各 bundle 声明的库，且仅允许从声明的
本地依赖或外部 provider 导入符号。

显示驱动使用原生 Apple 用户客户端与队列，私有接口按当前 iOS 18 映像恢复并验证。
`make kernelcache COLLECTION=display` / `make verify-kernelcache COLLECTION=display`
使用独立显示产物路径；资源交接、功能边界和旧 Window 日志分析见
[显示驱动文档](../docs/display/README.md)。

可以用 `--drivers-root`、`--driver`、`--driver-output`、`--provider-image` 和
`--collection` 选择其他驱动集合。添加任意 Kext 目录无需修改工具。
`tests/test_driver_toolchain.py` 实际构建了无平台依赖、名称不同的 C-only Kext 和
C++ IOService Kext，并验证自动发现、筛选、重复标识与非法路径拒绝。

GPU 使用独立 `gpu` collection，产物为 `build/output/kernelcache.gpu`。
运行 `make gpu-client`、`make test-gpu`、`make gpu`、`make verify-gpu`；
Apple 原生 GPU Family 的分析证据与当前不支持的功能见
[GPU 文档](../docs/gpu/README.md)。

GPIO 与平台按键使用独立 `input` collection，产物为 `build/output/kernelcache.input`。
`QcomGPIO` 管理 SDM835 TLMM，`QcomPMIC` 在 QcomSPMI 工作循环中分发 PMIC IRQ，
`QcomButtons` 使用原生 IOHIDDevice 和 IOHIDEventDriver。前置 Home 的芯片驱动为
`GoodixFingerprint` / `FPCFingerprint`，共享 `FingerprintHID`，固件准备为 `make prepare-input`。
运行 `make input-drivers`、
`make test-input`、`make input`、`make verify-input`；接口、接线、原生 ABI 证据及
当前支持范围见 [GPIO/按键文档](../docs/gpio/README.md)。

统一 I²C 与 Synaptics 触摸屏已加入 `input`，也可以使用 `make touch-drivers`、
`make test-touch`、`make test-touch-sanitized`、`make touch` 和
`make verify-touch`。设备节点默认禁用，要求显式的共享电源/时钟/引脚交接；
Apple 框架参考、接口、离线验证与尚待真机验证的边界见
[触摸屏文档](../docs/touch/README.md)。

platform collection 当前包含 corecrypto、Image4、AMFI、Sandbox、pthread 等
21 个原厂组件，以及 GenericARMPlatform、SDM835、AppleARMGIC、
GenericClockController、QcomGCC、QcomPowerController 和 QcomSPMI 共 7 个
本地驱动，形成 28 项闭包。其主机测试明确选择平台配置、GIC、GCC 和 RTC。
`power` 包含电压、MMCC/GPUCC、OSM 后端并保留原生 ApplePMGR 软件库（23 项）；
`power-security` 加入 AMFI 的完整依赖闭包（29 项）。
`make test-perfcontrol` 检查调度回调、QoS 策略及 OSM 联动；需要更新后的本地 XNU
私有所有权扩展。`make power-client` 构建内部 arm64 iOS 诊断客户端；用户态兼容边界见电源文档。
可选 UFS 等驱动由各自 collection roots 决定。
构建自动解析依赖，保留原生 AppleARMPlatform 和需要的 KPI/Family。

```text
build/output/Extensions/GenericARMPlatform.kext
build/output/Extensions/SDM835.kext
build/output/verification-drivers.json
build/output/kernelcache.sdm835.platform
build/output/kernelcache.sdm835.platform.manifest.json
build/output/kernelcache.sdm835.platform.verification.json
```

缓存生成器在临时文件中完成重定位、段权限封装与独立校验，然后原子发布。
每次打包在独立目录保存 kernel、provider 和本地 Kext 输入快照；清单按产物目录的
相对路径记录快照与哈希，后续 `make verify-kernelcache` 使用同一组输入复验。
校验包含元数据和版本、完整依赖、导入跳板、构造函数/GOT 指针、kmod 记录、
段映射与权限，以及本地 XNU 非 prelink 段的逐字节保留。
这些结果不证明 CPU/外设实际运行、crypto POST 或完整私有 ABI 兼容；本阶段未进行真机测试。
