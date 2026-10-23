# SDM835 Wi-Fi 驱动与离线验证

本轮已交叉编译 `QcomWLAN.kext`、`QcomWLANDriver.dext` 和 iOS `wifictl`。
内核固件客户端、Apple IOKit DMA 后端、WCN3990 CE 寄存器后端及原生 WLAN
注册桥接都有可编译代码。统一入口为 `scripts/toolchain.py` / `make wifi`。

**仍不能扫描、关联或收发网络数据。** `QcomWLAN` 的平台资源入口保持
`NotReady`：新组件尚未接入实际 modem GLINK、SCM MSA 授权、时钟/NoC、
ANOC2 SMMU 上下文和 CE 中断；IO80211 的扫描、关联、密钥与 WMI/HTT 数据面也未完成。
没有启用设备树节点、安装驱动或进行真机测试。

## 本轮代码

| 部分 | 实现及边界 |
| --- | --- |
| Apple 驱动模型 | `AppleARMIODevice → QcomWLAN : IOService`；Apple 元类、IOKit personalities、工作循环、命令门、PM 与管理员 IOUserClient |
| 固件客户端 | `WLANFirmwareClient.cpp` 将 QRTR/QMI/WLFW 状态机编入 Kext；保留 endpoint；协议缓冲区在堆上 |
| WLFW | 服务发现、指示注册、8-bit host-cap quirk、MSA_INFO/READY、芯片和板号校验、6144-byte BDF 分片、CAL_REPORT、FW_READY、WLAN_CFG、mission/OFF 模式 |
| MSA 所有权 | 校验区域范围、对齐、重叠；SCM 调用部分失败也保留所有权，OFF 或撤权失败拒绝释放；真实 SCM 操作由 endpoint 提供 |
| 板级数据 | 严格 board-2 解析及精确 identity 选择；统一 ZIP/hash 导入；Kext 读取版本化配置和 BDF OSData；没有自动套用其他板的校准数据 |
| DMA | `WLANDeviceDMA.cpp` 使用实际 IOBufferMemoryDescriptor / IODMACommand 和显式 IOMapper；uncached CPU 存储、35-bit IOVA、单段检查、逆序回滚及粘性 complete 失败 |
| CE | `WLANDeviceCopyEngine.cpp` 实例化所有 host ring；实际寄存器和 doorbell、描述符屏障、TX/RX completion、硬件下标和长度检查；CE6 留给固件 |
| DriverKit | 真正的 arm64 DriverKit 24.6 `MH_EXECUTE`；Apple `__osclassinfo` 类注册和继承的 Start/Stop RPC；严格链接真实 DriverKit 库 |
| 原生 WLAN | `NativeWLAN.cpp` 调用实际 `IOUserNetworkWLAN::registerWlanInterface`、QueueSet/LogicalLink Create；私有对象保持 opaque；完整 IO80211 控制和队列生产者仍待绑定 |
| Apple 网络集合 | `wifi` collection 共 33 个 Kext，包含 IOSkywalkFamily、IONetworkingFamily、IONetworkFamily；通过打包器重定位和集合校验 |

DMA 分配、映射、撤销和同步固件事务必须在可睡眠且未持有命令门的工作线程中运行。
现有资源入口只做诊断，不能直接把新后端接到命令门中的同步 power 请求上。
CE 寄存器配置默认屏蔽中断；实际平台接线还需要 12 个 Apple 中断 event source。

Native command 的 `complete()` 出错后可能已经消耗内部准备状态，因此后端保留
command、descriptor 和 mapper。尚未加入特定 ANOC2 mapper 在完整隔离后的故障映射回收协议。
超时、producer/consumer 相等以及 IORegistry 布尔值均不能作为停止 DMA 的证明。

## 构建

```sh
make prepare-wifi
make wifi-driver wifi-client wifi-driverkit
make test-wifi
make test-wifi-sanitized
make wifi
make verify-wifi
```

主要产物：

```text
build/output/Extensions/QcomWLAN.kext
build/output/QcomWLANDriver.dext/QcomWLANDriver
build/output/QcomWLANDriver.dext/build.json
build/output/wifictl
build/output/kernelcache.wifi
build/output/kernelcache.wifi.manifest.json
build/output/kernelcache.wifi.verification.json
```

`prepare-wifi` 从项目现有 iOS system cryptex 提取 DriverKit 24.6 cache 和参考库，
校验固定哈希。需要时临时复制现有 APFS reader，应用
`tools/patches/go-apfs-sealed-readonly.patch`，提取后自动删除临时源码和工具。
补丁仅用于读取 sealed catalog/extent，未修改通用 APFS 写入器，也未声称验证 APFS seal。
公开 DriverKit 头位于 `tools/driverkit/include`，来源、不可变提交和哈希在其 `sources.json`。
没有新增独立构建 Python/Shell 脚本。

DriverKit 编译使用公开头和项目已有 Darwin C 头；链接使用本地固件实际 DriverKit
库。`driverkit-abi.json` 固定了 24.6 cache、库、函数和公开 IOService vtable，
并核对 OSObject/IOService 的 40/48-byte 布局和生命周期 RPC ID。
这是一套限定接口的构建输入，完整私有 DriverKit SDK 尚未具备。

本机没有 Apple IIG。`.iig` 保留接口声明，类注册由
`QcomWLANDriverRPC.cpp` 按公开 `OSClassLoadInformation` 格式实现；Start/Stop
交由 Apple 的 `*_Invoke` 完成方法级 wire 校验。产物没有 main/thread 入口，
通过 `__osclassinfo` 加载。构建报告明确记录此方式。

DEXT 使用开发用 ad-hoc 签名；没有复制 Apple 私有 entitlement 或宣称 Apple 平台身份。
这次只验证交叉编译和静态 ABI，没有验证系统启动 DEXT 的授权或运行行为。

## 固件输入

项目当前没有经过验证的 WCN3990 BDF/校准资产、远端 modem 固件和 MSA 交接。
导入策略格式见 [firmware-profile.schema.json](firmware-profile.schema.json)，
芯片 ID、板号、ZIP 哈希、成员名和成员哈希都必须显式提供。

```sh
python3 scripts/toolchain.py prepare-wifi \
  --firmware-archive /path/to/firmware.zip \
  --firmware-profile /path/to/policy.json
```

输出为 `build/wifi/firmware/{board.bin,configuration.bin,profile.json,manifest.json}`。
`board-2` 还要求与 QMI ID 一致的精确 identity；缺失或重复匹配会失败。
导入 BDF 不表示远端固件已经启动，也不分配或猜测 MSA 物理地址。

Kext 的板级输入为 provider 上的 `wlan-board-data` 和
`wlan-firmware-configuration` OSData。配置是六个 little-endian uint32：
版本 1、长度 24、预期 chip ID、预期 board ID、BDF 字节数、保留值 0。
固件握手还会核对 WLFW 返回的 chip/board ID。

`QcomWLANFirmwareEndpoint` 定义了保留的内核通道边界：限时收发 QRTR datagram、
远端重启失效，以及可确认 CPU/远端所有权的 SCM 操作。
目前没有该 GLINK endpoint 的具体提供者；RPM intentless GLINK 不能直接充当 modem 通道。

## 原生接入与验证范围

QRTR 的原始 GLINK 服务发现采用控制端口 HELLO / NEW_SERVER 公告；
RESUME_TX 对应原始收发端口。每次连接由通道所有者分配不复用的主机端口，
以隔离旧连接的 QMI 响应；优雅关闭发送 DEL_CLIENT。

用户服务匹配使用 `IOClass=IOUserService`、`IOUserClass=QcomWLANDriver` 和
`CFBundleIdentifierKernel=com.apple.kpi.iokit`。子接口属性使用实际的
`IOClass/IOUserClass=IOUserNetworkWLAN`、`com.apple.iokit.IOSkywalkFamily` 和
Infrastructure role。`NativeInterface.plist` 保留完整 IO80211 子类的后续接口配方。

`NativeWLAN` 注册桥接接受真实框架的 logical link、TX pool 和 RX pool。
目前没有固件就绪后的队列生产者，Start 继续拒绝发布 WLAN 接口。
`IO80211DriverKit` 已从本地固件提取用于分析；扫描、关联、密钥和系统 Wi-Fi
控制 RPC 尚未移植，因此 DEXT 没有宣称这些能力。

离线测试覆盖完整模拟 QRTR/WLFW 启动、提前到达的指示、过期响应、错误芯片、
越界 MSA、部分授权失败、BDF 分片、配置编码、撤权失败、DMA 清理失败、
CE doorbell/完成和非法硬件下标；同时运行 ASan/UBSan、真实 kernel SDK 编译、
DriverKit Mach-O/类注册检查、源文件哈希和 Apple DT 转换检查。
模拟 endpoint/寄存器测试不能证明真实固件或硬件工作。

结果和产物哈希见 [validation.json](validation.json)，
Apple 内核/DEXT 模型见 [ios18-model.json](ios18-model.json)，
DriverKit 二进制依据见 [driverkit-abi.json](../../drivers/abi-22H374/driverkit-abi.json)。
硬件协议参考及各自的上游许可保存在 `reference/`，不参与驱动链接。

设备树节点保持 `disabled`：MMIO `0x18800000/0x800000`，
CE SPI 413–418、420–425。ANOC2 streams 为 `0x1900/0x1901`。
下一步需要先完成真实平台服务绑定和 IO80211/WMI/HTT 控制、数据面，再安排真机测试。
