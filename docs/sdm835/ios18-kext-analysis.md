# iOS 18 Kext 与 SDM835 驱动优先级

应先完成 loader 交接、平台枚举、外设 GIC 中断和内核基础服务，再接 RAM disk，最后推进 UFS、USB、显示及触控。当前 228 个原厂 Kext 中，有可作为软件接口候选的 Family，也有必须由高通实现替代的 Apple 硬件驱动。依赖闭包完整仅说明清单完整，不能证明二进制能在小米 6 上工作。

## 构建与样本

`kernelcache_patcher` 已复制到本项目并完成 Release 构建。宿主依赖为 CMake、C++17 编译器、tinyxml2 11.0.0、capstone 5.0.9、keystone 0.9，以及带 AArch64 反汇编器的 libLLVM（本机 23.1）。参考文件 SHA-256 和修改说明保存在 [MI6-origin.json](../../tools/kernelcache_patcher/MI6-origin.json)。运行分析与构建不再引用参考项目。

```sh
make kernelcache-patcher JOBS=3
make test-patcher
make analyze-kexts
make analyze-abi
make kernelcache
make verify-kernelcache
```

当前通过 5 项 C++ 测试、16 项 Python 测试、17 项真实缓存损坏/发布测试，以及 2,944 个 QEMU 原指令与降级指令差分用例。回归覆盖 MetaClass 误识别、原生虚表优先、长度不一致时拒绝映射、反向分支的 ADRP 消费者、Apple XML IDREF、缺失依赖及结构校验拒绝行为。

| 项目 | 实测值 |
| --- | --- |
| 输入 IPSW | `firmware/iPad_10.2_18.7.10_22H374_Restore.ipsw` |
| BuildManifest 版本 | iPadOS 18.7.10，22H374，iPad7,11 / iPad7,12 |
| 内核 | `kernelcache.release.ipad7c` |
| 解压格式 | arm64 / subtype 0 / MH_EXECUTE，传统 prelinked，非 fileset |
| 内核版本 | Darwin 24.6.0，XNU 11417.140.69.706.66，RELEASE_ARM64_T8010 |
| 解压大小 | 48,562,176 字节 |
| 解压 SHA-256 | `aa65c36c486a8b5cb1d7a2d565d1d6b3cc228b3c172f8c69c5da664ee19832cf` |
| Kext 数量 | 228，独立 ipsw 枚举的 Bundle ID 集合一致 |
| 本地对照 | `build/output/mach.development.sdm835.unstripped`，XNU 11417.140.69 |

参考项目另有 iPhone16,2 样本，本次未将其作为驱动来源。当前 iPad A10 样本的普通 arm64 格式更适合本项目的 arm64 目标，但原厂代码仍需检查页大小、运行时服务和 Apple 硬件访问。

完整元数据见 [inventory.json](ios18/inventory.json)，可扫描清单见 [inventory.tsv](ios18/inventory.tsv)。分析脚本直接解析 `__PRELINK_INFO,__info` 并展开 XML IDREF；复用字符串和字典不会变成空版本或空匹配条件。

## 已确认的 ABI 差异

原始分析输出中的 19 项拒绝是误判，真实核对发现的是一项虚表长度差异和一项实例大小差异。仅使用原生符号、不进行自动命名时，211 张可比较虚表中，210 张通过当前槽数及已有命名锚点检查，`OSKext` 被拒绝。该检查不覆盖所有函数签名或私有结构体。

| 类 | 原厂缓存 | 本地内核 | 原因与影响 |
| --- | --- | --- | --- |
| OSKext | 80 个函数槽 | 82 个函数槽 | 本地 `CONFIG_DTRACE=1` 在 `setLinkedExecutable` 后插入 `registerWithDTrace` 和 `unregisterWithDTrace`，后续方法不能按同槽重定位 |
| IOMemoryMap | 实例 88 字节，24 槽 | 实例 368 字节，24 槽 | 本地 `IOTRACKING=1` 添加 280 字节的 `IOTrackingUser fTracking`；虚方法数一致，但私有实例布局不同 |

槽号从 0 开始，不计 Itanium 的两项表头。`OSKext` 的关键位置为：

| 函数槽 | 原厂方法 | 本地方法 |
| --- | --- | --- |
| 35 | setLinkedExecutable | setLinkedExecutable |
| 36 | start(bool) | registerWithDTrace |
| 37 | stop() | unregisterWithDTrace |
| 38 | setVMAttributes | start(bool) |
| 39 | segmentShouldBeWired | stop() |

原厂虚表地址 `0xfffffff00716d058`，本地虚表地址 `0xfffffff0071c8220`。原厂槽 36 的函数 `0xfffffff0077e4084` 保存布尔参数，依次虚调用 `isStarted`、`isInterface`、`isKernelComponent`、`isLoaded`；其槽偏移与去除 DTrace 两槽后的布局一致，函数流程也与公开源码 `OSKext::start` 一致。槽 37 的 `0xfffffff0077e4408` 对应 stop 流程。原厂函数身份由布局、指令和源码共同推断，未使用自动注入的函数名作证据。

`IOMemoryMap` 的大小来自两侧 OSMetaClass 注册调用的 `w3` 实参；其差值为 `16 + 4 + 1 + 1 + 2 + 16*8 + 16*8 = 280` 字节，与 `IOTrackingUser` 对齐后的大小一致。122 个两侧都恢复到的类名中，其他 121 个注册大小相同。大小相同不能证明每个字段的类型和偏移相同。另有 9 个类未找到原厂实例虚表，这些 0 槽记录是覆盖缺口，不能计为 ABI 不兼容。

两项配置差异都在 XNU 私有层。`OSKext` 的不同槽位不等于所有外部 Kext 都会调用这些方法；`IOMemoryMap` 的新增字段位于末尾，也不等于只用公开虚方法的驱动必然失败。直接访问私有字段、按旧尺寸分配或派生相关类的二进制需要逐项检查。

下一步应建立单独的原厂 ABI 对照配置，关闭 `CONFIG_DTRACE` 和 `IOTRACKING` 后重新比较；当前 SDM835 内核配置没有被修改。不要通过强行接受错位槽或把对象大小改小来掩盖差异。

源码依据：[Apple OSKext.h](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/libkern/libkern/c%2B%2B/OSKext.h)、[Apple IOMemoryDescriptor.h](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/iokit/IOKit/IOMemoryDescriptor.h)。本地分别见 `libkern/libkern/c++/OSKext.h:575`、`iokit/IOKit/IOMemoryDescriptor.h:898` 和 `iokit/IOKit/IOKitDebug.h:267`。

### 原来的 19 项为何误报

实例表的邻接识别把 MetaClass 表当作实例表，随后自动命名又覆盖了原生导出。9 项拒绝发生在元类析构槽 0，10 项发生在实例槽 2 的 release 方法。

- 元类 9 项：`IOReporter::MetaClass`、`IODataQueue::MetaClass`、`IOEventSource::MetaClass`、`IOUserClient2022::MetaClass`、`IOMemoryDescriptor::MetaClass`、`IOInterruptController::MetaClass`、`IOInterruptEventSource::MetaClass`、`OSString::MetaClass`、`IOCommand::MetaClass`。
- 实例类 10 项：`OSObject`、`IODispatchSource`、`IOEventSource`、`IOInterruptEventSource`、`IOCommand`、`IOMemoryDescriptor`、`IOInterruptController`、`IOUserClient2022`、`IODataQueue`、`IOReporter`。

例如，原厂 `OSObject` 的正确导出表位于 `0xfffffff0071b08e0`，旧识别结果却是 `0xfffffff00716e358`，后者实际为 `OSString::MetaClass`。因此旧输出的槽 2 成了 `OSMetaClass::release(int)`，与本地 `OSObject::release(int)` 不同。这是识别错误，不是 OSObject 的 ABI 变化。

patcher 已优先采用原生实例表、跳过已命名的其他虚表、保留已有符号，并在槽数不同时停止同槽映射和命名。拒绝的虚表基址也不会进入解析器地址映射。原始误报见 [before 日志](ios18/patcher-analyze.before.log)，独立复核见 [native 日志](ios18/patcher-analyze.native.log)，大小与覆盖缺口见 [ABI 数据](ios18/abi-comparison.json)。

## 首批 Kext 候选

这些是原厂元数据的完整依赖闭包，清单位于 `ios18/stages/`。01/02 已通过离线生成，其余组尚待链接验证；所有组都需要运行验证。阶段编号是分析组别，实际调试应一次添加一组。

| 阶段 | 根组件 | 闭包数 | 当前用途或阻碍 |
| --- | --- | --- | --- |
| 01 crypto | corecrypto | 7 | 六个 KPI 资源加 corecrypto；已生成并通过离线门槛，crypto/PRNG 注册和 POST 仍需运行验证 |
| 02 platform | 上一组 + AppleARMPlatform | 11 | 追加 IOReportFamily、IOSlowAdaptiveClockingFamily、IOStorageFamily；已生成并通过离线门槛，作为平台私有类接口候选 |
| 03 ramdisk hfs | 上一组 + DiskImages.RAMBackingStore、hfs.kext、AppleBSDKextStarter | 16 | 减少首次 mountroot 对 UFS 和 APFS 的依赖；只用于 bring-up 文件系统 |
| 04 apfs security | apfs、AMFI、sandbox | 23 | 包含 CoreTrust、AppleImage4、CoreAnalyticsFamily、IOCryptoAcceleratorFamily、ApplePMGR、Watchdog、EffaceableStorage、AppleMatch 和 dsep KPI |
| 05 input | IOHIDFamily、IOHIDEventDriver | 20 | 自带 AMFI/Image4/PMGR 闭包，不能只加两个 HID Family |
| 06 usb host | IOUSBHostFamily | 已有 24，缺 2 | AppleSMC 声明依赖 watchdog 与 IOACPIFamily，样本内没有；未生成可执行 kexts.txt |
| 07 graphics | IOSurface、IOMobileGraphicsFamily、IOHIDFamily | 30 | 进一步引入 AppleM2ScalerCSCDriver、IODARTFamily、IOAV/Audio/HDCP 等，需逐个检查接口及硬件路径 |

最先使用 [01 清单](ios18/stages/01-crypto/kexts.txt) 和 [02 清单](ios18/stages/02-platform/kexts.txt)。不要把 `com.apple.kpi.dsep` 的资源存在误当作已提供 SEP 服务。闭包详情见 [closures.json](ios18/closures.json)。

本样本的 `AppleInterruptController` 匹配 `aic,1`，不是 GICv3 驱动；`AppleARMWatchdogTimer` 匹配 `wdt,s5l8960x`；`ApplePMGR` 的 bring-up personality 匹配 `pmgr1,bring-up`，实际 T8010 驱动另有 `AppleT8010PMGR`。这些名称和寄存器接口不能直接用于 SDM835。不要仅删依赖或改 compatible 就当作完成了驱动移植。

AppleARMPlatform 同时包含 AppleARMPE、AppleARMCPU、NOR NVRAM 等 personality；应先确认自定义平台 expert、CPU nub 与原厂类接口如何协作，防止错误匹配。原厂 NVRAM 访问 NOR，不能当作本项目的 EFI 变量后端。`AppleSEPKeyStore`、AppleSEPManager 与 AppleA7IOP/DART 链也不能由高通 TrustZone 自动替代。

## 应先编写的驱动

以下是最初规划的模块名。当前 `drivers` 已加入平台、GIC、时钟及其他驱动目录，具体集合以 `drivers/Build.json` 为准；存在实现不等于所有硬件功能已验收。

| 优先级 | 模块 | 首个可验收目标 |
| --- | --- | --- |
| P0 | loader handoff | 从 EDK2/加载器构造对应 boot_args、Apple DT、物理映射、真实 DRAM 保留区及 boot entropy，进入 XNU 并输出 UART；Linux FDT 不能直接传入 |
| P0 | SDM835Platform、CPU 与总线枚举 | 正确发布 arm-io/CPU/外设 nub，明确 platform expert；验证每核 PSCI、timer/IPI，完成基础服务匹配 |
| P0 | SDM835GICv3 | 接入 IOKit SPI 注册、mask/unmask、触发方式、路由和派发；现有 sdm835.c 仅 timer/IPI，其他中断会 panic |
| P0 | SDM835NVRAM 与 crypto/PRNG 适配 | NVRAM 先用明确的 RAM 后端，后续接持久化；安装 crypto/PRNG 回调并接真实熵源，记录返回值与初始化阶段 |
| P1 | QcomGCC、RPM、SPMI/PMIC、TLMM | 外设时钟、reset、regulator、电源域、pinmux 与 GPIO IRQ；UART 暂可依赖 loader 已开启的资源 |
| P1 | QcomSMMU 与 DMA | 验证设备 stream ID、DMA 映射、cache 同步和故障处理，为 UFS/USB/显示提供可靠 DMA |
| P1 | QcomUFS 与 PHY | 以 IOStorageFamily / 自定义 IOBlockStorageDevice 接块设备，先只读核对；不能复用 Apple NVMe/NAND 硬件驱动 |
| P1 | QcomDWC3、QUSB2 与角色控制 | 先选定 host 或 device 调试路径并处理 PHY、时钟、供电；USB Host 的 SMC/缺失依赖需先解决 |
| P2 | SDM835Framebuffer、MDSS/DSI/背光 | 先接 loader 提供的有效 framebuffer 显示色条，再实现扫描输出；内核 framebuffer 成功不等于 iOS 图形用户态可用 |
| P2 | BLSP I2C、TLMM IRQ、Synaptics DSX 触控 | 参考 sagit 的 I2C5 地址 0x20、GPIO125 IRQ、GPIO89 reset，再接 IOHIDEvent 服务；需按真机屏幕/触控变体复核 |
| P3 | Adreno、Wi-Fi/BT、音频、modem、相机、休眠 | 等基础启动、存储和交互可复现后推进；GPU 还需要相应用户态接口，不能套用 AGX 驱动 |

第一里程碑建议限定为 UART 稳定输出、平台和 CPU 枚举、timer/IPI 与一个 SPI 成功、crypto/PRNG 安装，然后用 RAM disk 到达可观测的用户态。APFS/AMFI/Image4 的接口和启动策略与此并行分析；不要在首次验证中一次装入全部 228 个 Kext。

## 历史初版校验记录（已由 ARMv8 版替代）

以下 11/7-Kext SHA 与报告是早期快照，不再代表默认输出。旧版未覆盖 XNU KCGEN 资源头遍历步长和 ARMv8 扩展指令，不能按新版门禁通过；请使用下节的最新产物与报告。

早期输出为 `build/output/kernelcache.sdm835.platform`，21,565,440 字节，包含 11 个 Kext。SHA-256 为 `c2922b33bb2e9a048ceef8d87a718c2eb7d11bc280564169502d8bc2f0457166`，重复构建一致。独立 ipsw 枚举的 11 个 Bundle ID 与清单一致，LLVM Mach-O 头和节读取成功。完整结果见 [验证报告](ios18/platform-verification.json)。

生成器另以 AddressSanitizer 和 UndefinedBehaviorSanitizer 执行同一 11-Kext 生成流程，未报告内存访问或未定义行为错误，输出与正式产物逐字节一致。该次关闭旧工具的泄漏统计，并在内存/UB 错误时立即中止；见 [sanitizer 日志](ios18/sanitizer-run.log) 和 [验证汇总](ios18/validation-summary.json)。

7-Kext 输出为 `build/ios18-kext-analysis/kernelcache.crypto-verified`，20,963,328 字节，SHA-256 为 `5d2d5d78db80aacd4d5eed54a687b6e45d0746c6e362195480d3cb4461d1fbc3`；见 [验证报告](ios18/crypto-verification.json)。

生成器先写入目标目录的临时文件，强制执行独立 Python 校验，通过后才原子替换正式输出。缺少校验器、未解出的重定位、冲突页面或任一结构检查失败时返回非零，并删除临时文件。以前验证通过的产物保持不变。校验包含：

- Mach-O 头、ncmds/sizeofcmds、命令大小、段和节的文件及 VM 边界、重叠、对齐、权限、zerofill。
- 符号表、动态符号表、重定位表范围、索引与每个重定位写入位置。
- Kext 头、外层段归属、VM 与文件偏移一致性、Bundle ID 集合、依赖闭包、版本兼容范围、原厂匹配元数据保持不变。
- 内核入口、构造/析构表、GOT、ARM64 导入跳板到 GOT 及目标函数的连接、kmod 名称/版本和 start/stop。
- 非 prelink 的原内核段内容和命令保持不变；源 Kext 的两张完整 SHA-256 常量表逐字节保持不变。

已修复的序列化问题包括：资源型 KPI 头的分配未计入 32 字节文件头、源固件资源头中被省略的 UUID 命令仍计入 ncmds、占位 prelink 段没有运行权限、zerofill 偏移减法下溢、已移除 LINKEDIT 的命令仍保留旧偏移，以及 kmod 内的旧名称/模板名称和版本未规范化。KPI 原始资源头会保留供符号解析，只有生成副本按实际命令规范化，不原样复制残留元数据。

首轮日志中的 4 个 ADRP 警告实际来自两张完整 SHA-256 常量表中的 `0xb00327c8`、`0x90befffa`，位置为 `0xfffffff0059fb4c0` 和 `0xfffffff005a002f0`。生成器只跳过完整匹配的 256 字节常量表，并独立核对输出内容；其他未知消费者仍会阻止输出。平台组另有两个真正经反向分支到达的 ADRP 消费者，已修复并用控制流样本验证，最新生成日志无未解决项。最新日志见 [crypto](ios18/patcher-link-crypto-verified.log) 和 [platform](ios18/patcher-link-platform-verified.log)。

6 项真实缓存反向测试分别损坏命令数量、KPI 命令边界、kmod 入口、crypto 常量、原内核代码，以及强制校验进程失败；全部被拒绝，最后一项还确认旧产物字节不变且临时文件已删除。结果见 [gate-tests.json](ios18/gate-tests.json)。旧的未验证候选只保存在 `build/ios18-kext-analysis/rejected/*.invalid`。

结构检查通过后，真机工作仍需处理本报告中的 OSKext/IOMemoryMap 私有布局差异、crypto POST、4 KiB 页及 Apple 平台服务与硬件假设。最新真机记录已进入 XNU VM 与 IOKit 初始化，尚未完整启动，见下节。

## ARMv8 静态降级与最新真机记录（2026-10-15）

默认生成流程已接入 `scripts/armv8_compat.py`：原厂 Kext 中超出
ARMv8-A + CRC + AES/SHA1/SHA256 的指令改为直接 `B` 跳转到
KC 内核的 `__ARMV8_EXEC` 段，再跳回下一条指令。内核提供该段的页表映射，
生成 KC 时写入降级辅助函数；无需非法指令异常模拟，也不在启动时修改代码。
源代码补丁为 `tools/patches/zzzzzz-sdm835-armv8-aot.patch`。

当前默认平台输出包含 21 个原厂组件及 7 个本地驱动，共 28 个 Kext，
25,395,200 字节，SHA-256：
`89d36b13451fad883ac97ed434f62e381b4f24ef11bb541e0b3a7205e0a5b106`。
包含 AppleImage4、AppleMobileFileIntegrity、Sandbox 及其 CoreTrust、AppleMatch、
IOCryptoAcceleratorFamily、CoreAnalyticsFamily、ApplePMGR、MACFramework 依赖。
已降级的 208 条均在 corecrypto 中：

| 指令 | 数量 |
| --- | ---: |
| SHA512H / SHA512H2 | 40 / 40 |
| SHA512SU0 / SHA512SU1 | 32 / 32 |
| EOR3 / BCAX | 10 / 25 |
| RAX1 / XAR | 5 / 24 |

辅助代码共 38,688 字节，使用整数与基础 SIMD 指令；保留非输出 GPR、
全部其他 SIMD 寄存器、NZCV 和 SP。每次密码降级使用 144 字节临时栈。
另外已实现 CAS、SWP、LDADD、LDCLR、LDEOR、LDSET、LDSMAX、LDSMIN、
LDUMAX、LDUMIN 的 8/16/32/64 位及 acquire/release 变体，采用
LDXR/LDAXR 和 STXR/STLXR 重试循环；每次使用 256 字节临时栈。
LDAPR 使用排序更强的 LDAR 替代。LL/SC 方式可参照
[LLVM compiler-rt 原子操作实现](https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/builtins/aarch64/lse.S)。
这批实际 Kext 没有 LSE 指令，相关实现通过独立用例验证。

未实现的 CASP、SM4、FP16、dot product、SVE 等指令，以及无法解码的代码字，
会直接阻止发布。该目标保留 SDM835 已支持的可选 CRC/Crypto 功能；不宣称覆盖
所有 ARMv8.1+ 扩展或 arm64e 指针认证 ABI。

离线门禁现在还会检查：

- 每个 Kext 指令和生成的辅助代码均能在目标 ISA 解码。
- 内嵌 `__patches` 记录与原始固件逐条对应，重新生成辅助代码后逐字节核对，检查跳出和返回分支。
- 原内核仅允许在零填充头部空闲区追加段命令，原有命令、段和节内容保持一致；旧内核缺少映射能力会被拒绝。
- 完整匹配的 13 处密码常量跨度排除出指令扫描，并与原始固件逐字节核对，不把数据误降级。
- 按 XNU `kalloc_type_view_parse` 的规则顺序遍历 `__PRELINK_TEXT`，检查 64 字节步长、split-segment 标记和末尾全零填充。
- 在所有重定位及 ISA 降级后重算 corecrypto `__text` 的 HMAC-SHA256，
  写入 `__TEXT/__fips_hmacs`，独立校验器再核对。原厂源映像哈希和原厂 HMAC
  均必须通过；未知版本或任意损坏的内嵌值会拒绝发布。没有关闭 POST。

真机首次发现 KPI 资源头规范化后仍按旧大小留下内部空隙，触发
`kalloc.c:1064` 的“剩余部分必须全零”断言。生成器已修正为按规范化
`sizeofcmds` 的 64 字节对齐步长紧密排列；该问题已有离线回归，后续真机已通过该阶段。

QEMU 用 `max` 执行原扩展指令，用 `cortex-a53` 执行实际生成的降级机器码。
2,944 个用例逐字节比较 31 个 GPR、SP、NZCV、32 个 SIMD 寄存器及原子内存，
覆盖随机输入、全零/全一、寄存器重叠、零寄存器、SP 寻址及 CAS 成功/失败。
原子差分用例是单线程验证，未代表真机多核竞争压力测试。
另有 8 项结构/原子发布测试和 9 项 ISA 损坏测试全部拒绝错误输入；
ASAN/UBSAN 生成结果与 Release 输出完全相同。

早期 16-Kext KC 已通过 EFI 临时加载进入真机 XNU：VM 初始化、kalloc 遍历、IOLib、
C++ 运行时及 IOMachPort 初始化完成。最后日志为
`[XNU IOKit] root nub init begin`，没有对应 done；停止范围位于
`IOPlatformExpertDevice::init(void *dtRoot)`，接下来应区分其内部的
`IODeviceTreeAlloc(dtRoot)` 与 `IOService::init(dt, gIODTPlane)`。
这尚未进入原厂 corecrypto 启动/POST，因此不把真机交接成功算作密码辅助函数已在真机完成验证。
15 秒看门狗正常恢复到 EDK2 Fastboot，未刷写分区。
阶段日志补丁为 `tools/patches/zzzzzzz-sdm835-iokit-trace.patch`。

后续真机细分日志定位到 `IODTMapOneInterrupt(timer)`：Linux DTS 的
`interrupt-parent` 继承没有转换成 XNU 要求的节点级属性，解析普通父节点时循环。
当前 `msm8998.dtsi` 为各中断节点显式指定 GIC。越过该阶段后，原厂 POST 报
`-2074 = -2000 + CCPOST_INTEGRITY_ERROR(-74)`。原厂 `__text` 的 HMAC
与内嵌值一致；链接及 ISA 降级改变代码后，旧内嵌值仍保留，造成完整性失败。
使用包含新 panic 输出的同一内核，仅更新 32 字节 HMAC 的真机对照已通过
`record_startup_extensions`、`InitIOKit` 和 `ConfigureIOKit`，然后暴露出缺少
Image4 接口。平台集合现已加入对应原厂提供者；后续记录见
[安全模块启动验证](ios18/boot-security-validation.json)。
`scripts/kernelcache_integrity.py` 已接到 `kernelcache_patcher` 的原子发布前流程，
`verify-kernelcache.py` 包含独立 HMAC 门禁。新增测试覆盖代码和摘要损坏、截断/
重复/缺失/重叠节、未知提供者、不修改代码及幂等性。

23-Kext 真机记录已输出 `Darwin Image4 Extension Version 7.0.0` 和
`Security policy loaded: AppleImage4 hooks (AppleImage4)`。随后定位到缺少
`/product`、`/chosen/manifest-properties`、`/options`；DTS 已补真实产品描述和
必要容器，没有填造签名或熔丝属性。引导桥现已提供完整 bank 参数及 16 KiB
CHRP v2 `nvram-proxy-data`，两轮真机均解析成功并完成 `ConfigureIOKit`，
原来的 `IONVRAMCHRPHandler creation failed` 已解决。随后补上
`/chosen/crypto-hash-method="sha2-384"`，真机也通过了 Image4 的哈希算法检查。
23-Kext 集合随后停在 `amfi interface not available @kern_trustcache.c:589`。
加载日志确认 AMFI 依赖解析成功、module start 返回成功，但它的 `_realmain`
为空，只有 C++ 类被注册。原厂 Sandbox 缺失使 AMFI 的早期接口注册没有发生。
平台集合现已补入 Sandbox 和自动解析的 AppleMatch；详细原厂对照见下文。
NVRAM 交接对照见
[NVRAM 与哈希方法验证](ios18/nvram-handoff-validation.json)。不能把打包成功或 Image4
策略注册等同于 AMFI 初始化完成、可信启动通过或完整系统启动。

### AMFI 与原厂/VMA 启动顺序

本项目 VMAPPLE、SDM835 和原厂 T8010 的关键顺序一致：

1. `PE_init_iokit` 建立注册表和 Kext 记录。
2. `mac_policy_initmach` 加载标记 `AppleSecurityExtension=true` 的安全扩展。
3. Sandbox 的 `_kmod_start` 调用 AMFI 的 `amfi_register_mac_policy`，AMFI
   初始化策略并调用内核 `amfi_interface_register`。
4. 初始化 trust-cache，完成 lockdown，再启动通用 IOKit matching。
5. 启动 BSD、发布 `IOBSD`，启动后续匹配的服务。

原厂 VMA2（25G83）二进制中，Sandbox 的调用点 `0xfffffe00096ddacc` 直接调用
导出符号 `_amfi_register_mac_policy`（`0xfffffe0008463dbc`）。T8010 样本中，
对应调用点为 `0xfffffff006a3f398`，经 Sandbox import stub/GOT 到
`0xfffffff005a7a3c0`，再进入 AMFI 初始化及内核接口注册。AMFI IOService 的
`start()` 也能进入同一初始化函数，但它是后续路径，不能据此认为原厂要等
`IOBSD` 才注册接口。Sandbox 依赖 AMFI，AMFI 不反向依赖 Sandbox，因此仅从
AMFI 做依赖闭包会漏掉这个早期调用者。

最初为定位问题编译过提前启动 AMFI IOService 的实验，但没有上机，相关
源码改动已全部撤回。最终使用原厂 Sandbox 调用链，没有调整 XNU 的阶段顺序。
二进制地址及样本哈希在 `build/amfi-diagnosis/order-comparison.json`。

2026-10-15 真机验证：EFI 使用 `-mstrict-align` 后通过原来的固件对齐异常，
随后 AMFI 报缺少 `/chosen/amfi-only-platform-code`。原生 T8010 AMFI 读取
该属性并要求长度为 4 字节；DTS 已补 `<1>`，桥转换为小端 uint32，保留
平台代码限制。第二轮依次输出 AMFI、Sandbox 和 vnguard 策略加载成功，
进入 trust-cache 阶段并继续到平台 IOKit matching，原来的接口 panic 已解决。

补齐 AMFI 后的停点是 `IOSystemStateNotification::initialize()` 在
`IOUserServer.cpp:7048` 的断言：`StateNotificationItemCreate` 返回
`kIOReturnIPCError (0xe00002bf)`。本地旧版 IIG 缓存的调用桩仅初始化
`IORPC.message/reply/sendSize/replySize`，遗漏当前内核新增的 `kernelContent`，
而 `OSMetaClassBase::Invoke` 要求该字段非空。原有 IIG 兼容转换只适配
dispatcher 的消息解析。两轮 AMFI 测试使用同一份 25-Kext KC，HMAC 未变化；设备均由看门狗恢复，最新
Fastboot 查询成功。详细检查、镜像摘要和完整回溯见
[AMFI 真机验证](ios18/amfi-startup-validation.json) 和
[串口日志](ios18/amfi-startup-hardware-uart.txt)。尚未完成完整系统启动。

后续 IIG 修复在 `scripts/toolchain.py` 的生成转换阶段为 100 处缓存调用桩
显式填写 `rpc.kernelContent = &buf.msg.content.__hdr`，仅在 `KERNEL` 构建生效，
覆盖同步和单向调用。原始镜像缓存及来源哈希门禁保持有效，未知调用桩布局拒绝
转换，不改动 `Invoke` 的检查或状态通知初始化断言。
`make test-iig` 使用当前内核的真实 `IORPC.h` 编译缓存中的状态通知请求和
回复代码：旧调用桩复现 IPCError，修复后内核及用户态模式均完成参数传递和
回复校验。两轮相同 EFI 的真机测试均越过旧断言，输出 BSD 的
`MAC Framework successfully initialized`、缓冲区初始化及 CoreAnalyticsHub
启动完成。详细后续观察见 [IIG 兼容验证](ios18/iig-compat-validation.json)。
另用同一内核运行 90 秒 watchdog 包，仍未出现 panic；最后的内核输出仍是
上述 BSD/IOKit 初始化信息，随后看门狗恢复到 Fastboot。当前后续等待位置
尚未精确定位，不能把 manifest/profile 缺失提示直接认定为停点，也不能把
本轮无 panic 等同于完整启动。

### RTC 等待与 Apple arm-io

后续通过 `IOKitInitializeTime` 的阶段日志确认停在 `IORTC` 等待。
原厂 VMA2 DT 中，总线为 `/arm-io`，两个地址/大小单元均为 2，并在其下描述
RTC 设备。sagit 的桥已使用 Apple DT 格式，但板级总线原来只有 `simple-bus`
兼容标识，无法匹配 `SDM835IO`；现已补 `arm-io,sdm835` / `arm-io,generic`，
继续由原生 `AppleARMIODevice` 发布高通设备。

平台集合加入 QcomSPMI 及自动解析的 QcomPowerController。SPMI 完成真实 MMIO
映射后，启动早期只读 PM8998 enable/seconds，校验错误与计数器低字节翻转。
本版 `IODelay` 在 CPU idle latency 未知时可能进入线程等待，导致短硬件轮询
停住；QcomPowerService 现对小于 1 ms 的轮询使用单调计数器忙等。
RTC 启动样本交给原生 `IORTC` 子类后才发布资源，平台专家转发日历接口。
没有缩短 XNU 原有等待、调整启动阶段或发布空的就绪标志。

串口诊断包和默认双输出包均完成 `IORTC ready`、`calendar initialized`，
继续到 `mbinit: done`，随后因缺少 pthread Kext 停在
`pthread_shims.c:318`。PMIC 原始读数分别为 94607、94722；厂商 UTC 偏移尚未
导入，日历时间未校准，持久化设时与闹钟亦未实现。
详见 [RTC 真机验证](ios18/rtc-validation.json)。

随后按 panic 补入原厂 `com.apple.kec.pthread`。其原生
`AppleKernelExternalComponent=true` 属性保留，由 XNU 原有
`KLDBootstrap::loadKernelComponentKexts` 在早期加载，通过
`pthread_kext_register` 建立函数表，没有使用替代回调或跳过空指针检查。
28-Kext 默认双输出包在真机越过 `pthread_init()`，继续到网络线程和 `lo0`
初始化；当前等待 `Content=Apple_HFS` 的 `IOMedia` 根设备。平台集合尚未提供
可挂载的根文件系统，完整用户空间未启动。
详见 [pthread 验证](ios18/pthread-validation.json)。

Sandbox 还暴露了链接器对 Clang 有界跳转表的控制流识别缺口：两个 ADRP
消费者只在后续 switch 分支中使用。链接器现识别 `cmp/b.hi/adr/ldrsw/add/br`
结构，核对表大小、表内偏移和所有目标后逐分支追踪，不再将间接跳转当作
线性落入首个 case。未知或越界表仍拒绝重定位；回归样本覆盖首分支覆盖寄存器、
后续有效消费者、越界目标及不支持的边界。

[汇总报告](ios18/armv8-validation.json) ·
[当前 KC 校验](ios18/pthread-kernelcache-verification.json) ·
[差分执行](ios18/armv8-differential.json) ·
[结构门禁](ios18/armv8-structural-gate.json) ·
[ISA 门禁](ios18/armv8-isa-gate.json) ·
[真机串口日志](ios18/armv8-hardware-uart.txt)

```sh
make build TARGET=SDM835
make kernelcache TARGET=SDM835
make verify-kernelcache
make test-armv8
```

## 复现提取和分析

`tools/prefix/bin/ipsw` 是从参考项目复制的静态宿主工具，版本与 SHA-256 已记录。它只负责解压和独立核对；Kext 清单解析脚本使用 Python 标准库。以下命令从 IPSW 重建默认输入：

```sh
python3 - <<'PY'
from pathlib import Path
import zipfile
out = Path('build/ios18-kext-analysis')
out.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile('firmware/iPad_10.2_18.7.10_22H374_Restore.ipsw') as z:
    (out / 'kernelcache.release.ipad7c').write_bytes(z.read('kernelcache.release.ipad7c'))
PY
tools/prefix/bin/ipsw kernel dec build/ios18-kext-analysis/kernelcache.release.ipad7c \
  -o build/ios18-kext-analysis/decoded
cp build/ios18-kext-analysis/decoded/kernelcache.release.ipad7c.decompressed \
  build/ios18-kext-analysis/kernelcache.release.ipad7c.macho
make test-patcher
make analyze-kexts
make analyze-abi
make kernelcache
make verify-kernelcache
build/kernelcache_patcher/kernelcache_patcher analyze \
  build/output/mach.development.sdm835.unstripped \
  build/ios18-kext-analysis/kernelcache.release.ipad7c.macho --details
python3 scripts/test-kernelcache-gate.py build/output/kernelcache.sdm835.platform \
  --source build/ios18-kext-analysis/kernelcache.release.ipad7c.macho \
  --local build/output/mach.development.sdm835.unstripped \
  --report build/ios18-kext-analysis/gate-tests.json
```

反向以本地内核作为扫描对象可复核其注册大小。分析输出存入 `build/ios18-kext-analysis` 后，需以两侧 `class` 行的 `instance-size` 比较，不能用单侧命名猜测替代原始布局证据。外部工具用法参见 [ipsw 内核文档](https://blacktop.github.io/ipsw/docs/guides/kernel/)。
