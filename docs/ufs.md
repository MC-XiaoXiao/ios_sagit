# QcomUFS

`drivers/storage/QcomUFS` 为 SDM835 的 UFSHCI / QMP v3 实现，使用 Apple
IOKit matching、IOCommandGate、IOWorkLoop、IOInterruptEventSource、IODMACommand
和 IOStorageFamily。Bundle ID 为 `org.darwin.driver.QcomUFS`，不是 Apple 原厂驱动。
源码、类名、bundle、测试入口均没有手机型号前后缀。

## 构建与产物

```sh
make drivers
make test-drivers
make verify-drivers
python3 scripts/toolchain.py pack-drivers --target SDM835 --collection storage -j 3
```

统一入口仍为 `scripts/toolchain.py`，模块、测试、ABI 合约和 collection 在
`drivers/Build.json` 中配置。没有增加构建或测试脚本。

产物：

```text
build/output/Extensions/QcomUFS.kext/Contents/Info.plist
build/output/Extensions/QcomUFS.kext/Contents/MacOS/QcomUFS
build/output/kernelcache.sdm835.storage
build/output/kernelcache.sdm835.storage.verification.json
build/output/kernelcache.sdm835.storage.manifest.json
build/output/verification-drivers.json
```

Kext 由 Apple 的 kmod 启停代码、metaclass 和 ld64 `-kext` 生成，类型为
`MH_KEXT_BUNDLE`、arm64/ALL。发布后的控制器常驻，不支持运行中卸载。
storage collection 包含 UFS、平台/GIC、原厂 IOStorageFamily 及依赖闭包。
collection 的结构验证不证明真机可启动，也不认证所有私有 ABI 或 crypto POST。

## Apple 模型与 iOS 18 参考

主 personality 匹配 `IOProviderClass=AppleARMIODevice`、
`IONameMatch=qcom,msm8998-ufshc`，另有 `IOPlatformDevice` fallback；二者都由
Apple IOKit 匹配，不建立私有总线。控制器从 provider 获取两个 MMIO 和第 0 个 IRQ，
不在驱动中写死 MMIO 地址、中断号或手机名称。

每个启用的普通 LUN 发布 `QcomUFSBlockDevice : IOBlockStorageDevice`，由原厂
`IOBlockStorageDriver` 的 `device-type=Generic` personality 继续发布 IOMedia/BSD
磁盘及分区。没有自行伪造 IOMedia 或 BSD 磁盘对象。LUN 0..7 的启用状态、容量、
块大小、队列深度和写保护均从设备查询；容量与 READ CAPACITY(16) 交叉检查。
RPMB、boot well-known LUN、配置描述符写入和分区重配置不作为普通磁盘导出。

iOS 18 样本中的 `AppleNANDConfigAccess` 没有 matching personality；硬件存储链由
IONVMeFamily 和 IOStorageFamily 实现。这里只参考服务分层、依赖和 Apple matching，
没有将 NAND/NVMe 的 Apple 寄存器或 IOP/SEP 接口搬到高通硬件上。
元数据快照见 `docs/ufs/ios18-storage.json`。

公开 IOStorageFamily-323 头文件在 `drivers/include/IOKit/storage`，以 iOS
目标编译。核对本地 iOS 18.7.10 样本后记录的基类大小为 144 字节、实例虚表为
190 槽，IOService 为 136 字节、168 槽。`apple-storage-abi.json` 记录镜像 SHA-256、
所需导出和去掉 Itanium 表头后的槽号；编译器布局核对记录位于
`build/ufs-vtable-layout.log`，原厂函数复核记录位于
`build/ufs-native-storage-disassembly.txt`。例如原厂 doSynchronize 槽 188 跳转至
槽 171 的 doSynchronizeCache，默认不支持的操作返回 `0xe00002c7`。

结构匹配不是全部函数签名兼容性的证明。合约按固定样本验证；换版本必须重新核对。
collection 构建会把已核对的符号交给链接器，不能只在导入检查中把符号当作存在。
patcher 的 `@符号清单` 模式只读显式清单，不自动读取旁边的 symbols.json。

## 读写、DMA 与链路

- 适用范围内使用 READ/WRITE(10)，较大 LBA/传输使用 READ/WRITE(16)，单请求最多 1 MiB；使用 Apple 标准
  `IOMaximum*` / `IOMinimumSegmentAlignmentByteCount` 属性让 IOStorageFamily 拆分请求。
- 使用 CAP 发现并使用全部硬件槽，最多 32 个，按 LUN 的队列深度限流；支持乱序完成。
  UTRLDBR 只写新请求的单个 W1S 位，避免用过期 doorbell 值重发已完成的写入。
- 每请求最多 256 个 PRDT 项，每项最多 256 KiB，检查 4 字节对齐、地址宽度、长度、
  溢出和总传输量。正常数据直接通过 SG DMA，无 PIO 数据读写或固定低速 bounce 路径。
- 有 device IOMapper 时使用设备地址域；没有 mapper 时必须有平台明确交接的物理
  DMA bypass。物理模式逐 PRD 检查地址，不默认绕过 SMMU，也不把 IOVA 当物理地址。
- UTRDL/UCD/UTMRDL 使用独立、常驻映射的无缓存内存，避免两个 32 字节 UTRD 共享
  cache line 时的回写/失效竞争。数据缓存同步在提交前和完成后进行。
  非一致性数据要求 64 字节边界，内部 32 字节 capacity 响应单独占有整个 cache line。
- SDM835 的 `__ARM_COHERENT_IO__` 必须保持未定义。若设为 1，IOKit 的
  `performOperation` 会跳过缓存同步，底层 cache 函数也只剩 DSB；真机表现为
  READ CAPACITY 成功但 CPU 读到全零。平台配置已修正，生成代码包含实际
  `dc civac` 与 `dc cvac`。不能用 `dma-coherent` 属性绕过这个问题。
- DMA 准备在调用线程中执行；来自本控制器工作循环 gate 的异步请求移交 thread_call
  准备。DMA 解除映射、complete 和客户端回调在 gate 外执行。
- SYNCHRONIZE CACHE(16) 实施控制器级顺序屏障，等待旧命令并阻止新命令越过 flush；
  Force Unit Access 转换为 SCSI FUA。IsEncrypted 表示调用方已加密，数据按原字节传输；
  Static 作为提示接受，未知 IOStorage options 返回不支持。没有提供 ICE 加密接口。
- 初始化 QMP v3 SerDes、TX/RX、PCS、Rate-B，检查 PHY ready、TX Hibern8 和 LinkStartup；
  配置 200 MHz HCI / 150 MHz UniPro 对应的计时参数、UniPro flow/replay 超时和 LCC。
  查询双方 HS gear 和连接 lane，协商最高 HS-G3 / Rate-B。实际使用单/双 lane 取决于
  平台布线及设备，不将所有 SDM835 固定描述为双 lane。
- 采用 FASTAUTO 处理旧 Qcom peer-DME 约束；处理 Samsung/WDC 的 PA_TACTIVATE
  相对时序和 SK Hynix SaveConfigTime 扩展。未声称覆盖所有厂商/型号的全部 quirks。

中断负责正常完成，10 ms watchdog 同时按 doorbell 回收丢失 IRQ 的完成并检查
30 秒请求期限。检查 OCS、UPIU transaction/tag/LUN、query 长度、SCSI status、sense、
residual 和溢出；短传输不会报完整成功。错误响应、链路/总线错误或超时会关闭 admission，
停止控制器并确认 HCE disable，然后失败完成请求。不会重放失败的写操作。
无法确认 DMA 静止时 panic，不能释放仍可能被 DMA 写入的调用方内存。
本版失败后保持 offline，自动重建控制器和在线恢复留待后续实现。
合法的 UPIU device exception 不当作协议损坏：门锁外查询 EE_STATUS，紧急 BKOPS
事件启用器件自动后台操作，并发布诊断属性。终止错误通过 Apple media-state 消息通知存储栈。

Apple PM 的低状态先排空已接收的映射任务、flush 各 LUN，再进入 UIC Hibern8；
高状态退出 Hibern8。两个状态均要求平台保留时钟、电源、参考时钟和 mapper domain。
这不是完整的掉电 suspend/resume；本版没有宣称支持 SoC power collapse。

## 平台与 Apple 设备树交接

本 Kext 实现 PHY/UFS 传输。GCC、RPM/PMIC regulator、TLMM RESET_N 和 NoC 带宽
必须由启动端或平台服务先准备并持续保留；这些平台电源驱动没有包含在本 Kext 中。
驱动不会靠写一个布尔属性就替硬件开启这些资源。EFI bridge 已增加经真机验证的
warm handoff；冷启动平台资源配置与完整电源管理仍需独立实现。

v1 冷启动契约要求启动端停止 firmware 的 UFS Block I/O、交出控制器独占权，保留 UFS HBA、
UniPro、PHY aux/ref、TX/RX symbol clocks、各供电、解除器件 reset，并固定满足
HS-G3 的 NoC 带宽。`qcom,ufs-handoff-flags` 表示已经完成的操作：

| 位 | 启动端保证 |
| --- | --- |
| 0 | 所有相关 clocks 已打开并保持，HCI=200 MHz、UniPro=150 MHz |
| 1 | HBA/PHY/device regulators 已打开且保持 |
| 2 | 器件 RESET_N 已释放，firmware 已交出控制器与存储访问权 |
| 3 | 内存与配置 NoC vote 足以运行 HS-G3，并保持 |
| 4 | 可选：UFS 的 DMA 路径已核对为物理 bypass；没有此位必须提供 device IOMapper |

没有真实完成交接时不能填这些标志。使用 SMMU 的启动路径应发布正确的
`iommu-parent`，并由平台的 Apple IOMapper 实现 stream/domain 映射；仅有数值 phandle
而没有服务会等待该 provider。GCC 低频、未开启供电或错误 DMA 地址域不能通过补齐
DT 属性来修复。

v2 是受限的固件 warm handoff：当前 sagit 实机 HBA 版本为 `0x30010000`，
固件已训练为单 lane HS-G2 / Rate-B；桥接端记录实际 FAST/FASTAUTO 模式。桥接端通过 EFIClock 协议保持相关时钟，
校验 GCC 时钟、门控、GDSC、PHY ready 与链路属性。ExitBootServices 后停旧 DMA，
使用私有物理描述符重新初始化命令列表，读取 GPT 并与 EFI 阶段的结果完整比较，
再读取 ios-apfs 的 NXSB。成功后再次停止 HCE，才发布 v2/flags=31，交给 XNU
分配自己的 DMA 列表。v2 的位 3 表示已经实测保留的互连路径可完成接管后的读请求。HCE 复位后的
检查使用重新启动的链路，不应把 EFI 前采样的 HS-G2 档位当作这些检查的实际速率；
该标志不表示已测定 HS-G2 全速 NoC 带宽或实现新的 RPM vote。

供电沿用固件已开启的 rails；桥接端没有实现 PMIC 冷启动或掉电恢复。GICD_CTLR
从 0 转为本机可用的非安全 Group1 转发/ARE 状态 `0x12`，保留安全侧所有权。
Apple DT 中 GIC specifier 使用两个小端 cell，不能保留 Linux 的三个 cell。
MSM8998 的虚拟定时器为 PPI 3 / INTID 19，不能套用通用 ARM 的 INTID 27。
小米下游和 Linux 上游参考树均列出 PPI 1、2、3、0；Apple DT 转换将这些固定
低电平 PPI 编为 level 类型，内核使用 INTID 19 并设置对应的 GICR 优先级字。
只有 EFI 入口执行该契约，带 UFS handoff 的 RawImage 入口会明确拒绝启动。

以下是 **Apple 展平设备树** 的语义，数值均为小端，不可将 Linux FDT 原样传给驱动：

```text
/arm-io/ufs
  name = "ufs"
  compatible = "qcom,msm8998-ufshc\0"
  reg = uint64[4] { 0x01da4000, 0x2500, 0x01da7000, 0x1000 }
  interrupt-parent = uint32 { <GIC phandle> }
  interrupts = uint32[2] { 297, 1 }       # SPI265 + 32，level
  lanes-per-direction = uint32 { <实际布线 1 或 2> }
  qcom,ufs-handoff-version = uint32 { 2 }
  qcom,ufs-handoff-flags = uint32 { <真实交接标志> }
  qcom,ufs-core-clock-hz = uint32 { 200000000 }
  qcom,ufs-unipro-clock-hz = uint32 { 150000000 }
  qcom,ufs-hs-power-mode = uint32 { <EFI 实测 FAST=0x11 或 FASTAUTO=0x44> }
  qcom,ufs-hs-gear = uint32 { 2 }        # v2：经固件和真机核验的档位上限
  qcom,ufs-polled-completion = uint32 { 0 }
  dma-address-bits = uint32 { 40 }       # CAP 无 64 位支持时进一步收紧为 32
```

省略 `dma-coherent` 使用非一致性缓存同步；只有确认 DMA 硬件一致性时才发布空 OSData
或 kOSBooleanTrue。错误类型、false 或带任意数据的该属性会拒绝。IRQ provider 必须由
Apple 的设备树中断映射生成 `IOInterruptControllers` / `IOInterruptSpecifiers`。
物理地址只在这个平台描述中出现，驱动从 provider 映射。

`device_trees/msm8998.dtsi` 中只添加 disabled 的 Linux 资料节点，不伪造以上交接；
`build-bridge.py --ufs-handoff` 为 EFI 路径启用节点，运行时核验后填写交接标志。
已有下游树使用单 lane，必须以实际
布线为准，不依据 SoC 最大能力填 2。

## 离线验证与后续边界

`tests/ufs_test.cpp` 直接使用生产协议、调度、寄存器时序及 PHY 表，覆盖：
大小端/READ-WRITE(16)/FUA/ordered、边界 LBA、1 万组 SG 地址与长度、32 槽及 bit31、
逆序完成/slot reuse/flush 屏障/LUN depth、错误 UPIU/tag/LUN/OCS/sense/residual、
query 拒绝、HCE 延迟与拒绝停机、UIC 超时/错误/power status、Hibern8 和厂商时序。
这不是运行整个 IOKit 驱动的硬件模拟器。主机 ASan/UBSan 另外检查这些生产核心代码。

Kext 和 storage collection 均完成离线格式、依赖、导入及链接产物检查。完整记录见
`docs/ufs/validation.json`。2026-10-15 已通过 Fastboot stage/run-efi 真机测试，
没有刷写 boot。后 EFI 的物理 DMA GPT/NXSB 读取已通过；XNU 自己的 DMA 列表、
NOP、fDeviceInit、三星设备描述符、HS-G2 协商及 LUN0 TEST UNIT READY 已通过。
后续已验证全部六个原生 LUN 容量、GPT 的 ios-apfs 分区，以及原生 APFS
checkpoint XID 2 和 System/Data 卷的 BSD 客户端发布。虚拟定时器按 MSM8998
资料改为 PPI 3（INTID 19），真机已记录中断。启动桥通过 `/chosen/root-matching`
按 System 卷 UUID 匹配 IOMedia；该 iOS provider 没有 macOS 的
AppleFileSystemDriver，单独等待 `boot-uuid-media` 不会完成选择。
handoff-24 已进入 System 的原生 `apfs_vfsop_mountroot`，随后在 zone 分配路径
崩溃；另有卷块数记账警告。System/Data 挂载及 Bash 执行尚未通过。

全速的实现目标是：平台在正确 clocks/NoC 下运行，协商设备及布线共同支持的最高
HS-G3 / Rate-B，使用全部可用队列和 SG DMA。**实际 MB/s、IOPS、稳定性、DMA/cache
正确性和电源恢复仍需真机确认**，不能以编译通过或模型测试替代。
RPMB、ICE inline encryption、HPB、UFS 3.x/MCQ、TRIM/UNMAP 和断电恢复未实现，
不会伪报支持。现有 iOS/APFS/SEP 加密环境不能仅靠 UFS 块设备获得兼容性。

## 来源与许可

- [Apple IOStorageFamily-323](https://github.com/apple-oss-distributions/IOStorageFamily/tree/IOStorageFamily-323)：
  公开 Family 接口，保留 APPLE_LICENSE。头文件 URL/哈希见其目录的 sources.json。
- 本项目 XNU 11417.140.69 的 IOKit、DMA、IOMapper、PM、workloop 与导出接口。
- 本地 iOS 18.7.10 arm64 kernelcache：只核对匹配元数据与 Family ABI。
- [Linux v6.12 UFS core](https://github.com/torvalds/linux/blob/v6.12/drivers/ufs/core/ufshcd.c)、
  [Qcom host](https://github.com/torvalds/linux/blob/v6.12/drivers/ufs/host/ufs-qcom.c)、
  [QMP UFS PHY](https://github.com/torvalds/linux/blob/v6.12/drivers/phy/qualcomm/phy-qcom-qmp-ufs.c)：
  UFSHCI/UniPro、Qcom v2 和 msm8998 对应的 QMP v3 校准。固定快照、URL 和 SHA-256 在
  `docs/ufs/reference/sources.json`；统一驱动验证入口检查这些快照及公开头文件校验值。
- `/home/xxiao/Projects/Project_iOS2ARM` 用于检查既有 Family 分层；构建不引用该项目路径。

新写的传输/块设备代码采用 BSD-2-Clause；PHY 校准表改编自 Linux，保留 GPL-2.0-only
及原版权说明，组合驱动包含该 GPL 代码。许可全文位于 QcomUFS/LICENSES。
