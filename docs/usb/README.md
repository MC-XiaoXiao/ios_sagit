# SDM835 USB / OTG（实现中）

已添加默认关闭的 CDC ACM 串口控制台，使用独占 Device personality、
标准控制请求和有界内核日志/输入缓冲。实现、开关和 Linux 使用方式见
[CDC ACM 控制台](cdc-acm.md)；依然受下述真实 USB 资源交接前提约束，
该 CDC personality 尚未进行硬件枚举验证。

目标是 Apple IOKit 匹配和原生 USB Family 下的 SDM835 驱动。目前已有平台层
`QcomUSB.kext`，通过 arm64 Kext 构建、导入和虚表检查。Host 与 Device 已是可实例化
的 Apple 原生框架子类，使用 `QcomUSBController` + `USBRole` personalities。
**真机已完成480Mbps枚举、原生 Apple Mux v2、固件信息读取及连续三轮软件重连；
最新版的物理拔插也已验证不重启，电脑重新识别并继续读取固件信息**。
2026-10-22 已开始连接设备的分层调试，当前状态见下节。
不能把寄存器模型、arm64 目标文件或测试通过当作 USB 枚举成功。
独立的 `QcomUSBPHY` 服务已实现 QUSB2 / QMP USB3 冷启停及真实 GCC/RPM 资源调用；
新增 `QcomUSBResources` 聚合服务已连接 PHY 与 PMIC 连接器，按独占租约管理 controller
时钟/GDSC、固件 DMA 停止、复位与速率切换；只有真实 NoC 和设备 IOMapper 或
已验证物理 DMA 窗口就绪才发布 `USBPlatformResources`。NoC 和直接 DMA 软件路径已加入，
如需翻译，已加入 `QcomUSBMapper` 原生子类，按实际交接的 USB 域建立私有页表；
真实板级交接生产端的硬件验证仍在进行。
`QcomPMIC.kext` 新增独立匹配的 `QcomUSBConnector`，可通过真实 SPMI 采样 PMI8998
CC/VBUS、生成连接代次并管理独占租约。Type-C/Rp 初始化、CC2 勘误恢复、过流锁存
和连接器电源状态已接入；供电由明确电流预算和实时快照授权，完整 OTG 仍在实现。

## 2026-10-22 真机调通记录

当前候选保留已验证的 CA_CLIENT/工作间隔内核修正、桌面/触摸驱动和当前 Metal
插件，合入配套 USB/电源后端、只读电池驱动及原固件 Apple USB Family，整包包含99个 Kext。
Device 路径继续使用原生 `IOUSBDeviceFamily`、配置器及 `AppleUSBDeviceMux`；
当前选择原生 `standardBringup` 配置，最高速度为板级确认的 High-Speed。

已在实际设备验证：

* USB0 的安装后 ACPI `_HID`/`_CCA`/`_CRS` 和 IORT 拓扑可交接；该节点没有 `_ADR`，
  不再把合法的缺省情况当作采集失败。USB IORT 的地址宽度为36位。
* EDK2 Fastboot 将 Host 配置句柄重新绑定为 Device，管理库仍保留 Host override。
  在固定映像/句柄校验、关闭生产回调后，按真实别名断开并退休旧协议，完成 post-EFI
  停机及 BCR 复位证明；随后可启动完整 iOS 并进入 root Bash。
* 同一物理 nub 上的控制器和资源 personality 不再因 `IOResourceMatch` 相互等待。
  RPM 传输、片上互连查询、资源生产服务和异步重匹配均已在 IOKit 注册表观察到。
* PMI8998 初始快照为 Type/Subtype/TypeCConfig/OTGCommand/OTGStatus =
  `51 15 ca 01 00`。驱动在排他所有权下清除状态机已关闭时遗留的 OTG 请求并读回，
  保留实际活动 source 的拒绝逻辑。配置及全部6路中断注册/启用已在真机通过。
* 首次时钟失败已定位为 `RetainClock(116)`：BCR 保持时，主时钟分支不能完成 NOC
  握手。改为先保持 fabric、验证 DMA 并准备 PHY，再解除 BCR、申请本地时钟。
  调整后控制器时钟资源和真实 NoC 租约已在真机通过。
* 实际 SMMU 为 CR0=`0x200626`、IDR0=`0x48017a10`、IDR1=`0x20008006`、
  IDR2=`0x7111`；16个 stream 均无效且 S2CR 为 FAULT，6个 context bank 未启用。
  未匹配 stream 会 fault，不能按 bypass 授权 DMA。新增显式 `fresh-context` 请求，
  仅在完整空闲状态被重复观察、实际 IRQ 路由通过且 NoC/BCR 仍成立时，才由
  原有 `QcomUSBMapper` / Apple `IOMapper` 路径建立私有页表。实际 context、TTBR、
  TCR、MAIR 和 S2CR 均已写入并读回，但 SMR 的 SID 可写、VALID 位始终读回0，
  该路径尚未获得可用的 stream 匹配。
* 对照原机 `UsbfnDwc3Dxe` 的 uncached 物理缓冲区分配及 TRB 地址填充，增加显式
  `firmware-physical32` 调试策略。仅分配真实32位 uncached 物理内存，重复核验 guest
  SMMU 仍为完整空闲状态，不改全局寄存器、不声称已证明 architected bypass。
  已进入原生 Device Family/Configurator。修复下述总线复位后，主机已从真实 Mi 6
  端口 `1-2.4` 读到原生设备描述符 `05ac:12ac`、版本 `6.01` 并完成 SET_ADDRESS；
  这验证了该候选的 EP0 DMA 读写，但配置描述符及 Mux 数据传输仍待验证。
* 原生配置器首轮空指针异常已定位为遗漏 `IOUSBDeviceController::readyForHost()`。
  该原生非虚入口先创建 USBStringIndex、控制请求 tracker 和缺省设备描述，再发布
  控制器。已按当前固件调用点/机器码绑定这一入口，替代直接 `registerService()`。
* Native 收到 USBRST 时曾执行全核 halt/重启，使主机无法完成端口复位。现复用
  已有总线复位逻辑，保持 DCTL.RUN、保留已武装的 SETUP，等待活动端点的真实
  END 后再回收。覆盖所有30个数据端点，最后一个端点和迟到 XferComplete 的
  所有权测试已通过。实际主机由端口复位失败推进到读取设备描述符；随后解决了
  `bNumConfigurations=0 / no configurations / -22`（见下一条）。
* 已确认原生 Mux 管道创建成功、端点为 `01/81`。继续对照原固件发现
  `connectedWithAccessoryCable()` 表示 `USBHostAltConfig` / `USB3HostAltConfig`，
  普通主机连接不能返回真；另补齐原 Apple 控制器的最低速率 slot208（FS=1）。
  修正后已在两个候选冷启动中枚举为 `05ac:12ac`、480Mbps、配置1，Mux接口
  `ff/fe/02`、Bulk端点 `01/81`、包长512。主机连续10轮读取设备/配置描述符和
  GET_CONFIGURATION 成功，usbmuxd 收到原生 Mux v2.0 应答。
* 主机在开机早期访问 lockdownd 失败，未把目标加入应用可见列表。新增显式
  `usbctl reconnect`，通过原生 `GoOffAndOnBus` 命令复测；早期候选的描述符 STALL
  已进一步定位到小缓冲区实际物理地址超过32位，修正及复测见下文。
  `usbctl native-debug` 可开启原生控制器诊断；这两个命令要求 root 且仅选择唯一
  的 QcomUSBDevice，`registry` 仍保持只读。
* 用户反馈充电图标未出现，检查发现98-Kext候选未装入 QcomBattery，也没有
  IOPMPowerSource。随后合入已有只读电池驱动（99个Kext）：真机采样有效，
  外接电源和充电状态均为真，电量47%、3750mV、净电流-298mA。系统 IOPS 接口
  仍未列出此源；用户随后确认界面出现充电连接提示。详情见 `docs/battery/README.md`。
* 物理拔线已抓到原生 AppleUSBDeviceMux 定时器空指针异常：实际 PC
  `0xfffffff006d597d0`，对应原固件 `0xfffffff006be57d0`，Mux 的 command gate
  已在 free 中清空。Mux 的正常 WasDeactivated 消息会取消两路定时器，free
  本身不会。候选改为在 IOKit phase1 的 requestTerminate 中先关闭准入、
  退休停机后的 TD，再调用原生 handleUSBCableDisconnect；在共享 gate 外加入
  policy/DMA worker 的退出等待，并关闭 provider，让同步退出及重新匹配继续。
  用户已确认两轮物理拔插不重启；最新版随后又完成一次实际拔插，用户确认是手动
  操作。主机记录断开/重枚举，USBRoleEpoch=3、原 Bash PID=94保持，新的控制器
  SelectedConfiguration=1，标准工具和全部固件查询继续成功。
* 移除本轮无须继续输出的 GPU/display 只读探针，USB 的四次全 ACPI 命名空间
  查找改为一次组件查找及标准 SDT GetChild/GetOption 读取；保留资源核验。
  真机 EFI 到已验证 Bash 从55.744秒降至25.854秒，冷启动枚举/Mux握手仍通过。
  固件源码中的全命名空间转储也改为仅 verbose 级别；尚未写入设备固件，当前
  已安装固件仍会打印剩下的一次查找。
* `idevice_id -l` 曾成功列出本机 UUID；后续通过同一实际 Mi 6 端口及当前
  Mux handle 成功读取 `ProductVersion=18.7.10`、`BuildVersion=22H374`、
  `ProductType=iPhoneMi6,1`，再连续5轮读取全部通过。开机早期 lockdownd 尚未
  就绪，当前主机 usbmuxd 1.1.1 的首轮 preflight 失败后没有自动发布该设备。
  软件重连曾出现 EP0 STALL；下述地址分配修正后，连续三轮重连、应用列举及查询已通过。
* 原固件 AppleUSBDeviceMux `writeToUSBPipe`（`0xfffffff006be7fdc`）在包长整除
  MPS 时另行提交一个非空描述符、逻辑长度0的 ZLP。后端现在与原生控制器一致，
  对长度0的客户端描述符仅 retain/release，跳过无需进行的 prepare/complete。
  IN/OUT零长度、不能 prepare 的空描述符、越界字节拒绝及引用平衡测试通过；
  此修正已合入最终实机候选；没有把它单独作为解决 STALL 的依据。
* DMA 诊断抓到8字节控制请求的实际地址为5855019008（超过4GiB），后端按32位
  窗口正确拒绝，原生控制请求完成 Invalid，电脑因而看到 STALL。实际系统
  IOMapper 不存在：根因是 Apple `IOKernelAllocateWithPhysicalRestrict` 对单页、
  32位限制改用 KMA_LOMEM，而本移植的 vm_lopage_needed 未启用，
  `vm_page_grablo` 会退回无地址约束的普通分配。SDM835 路径现保留数值上限，
  继续交给原生 `kmem_alloc_contig`，包括单页分配。
  另为显式物理窗口增加私有 HostPhysicalMask 选项；当存在系统 IOMapper 时，
  它保留真实物理分配限制及原生 IOVA map specification，不占用缓存模式位。
  仍复用 Apple IOBufferMemoryDescriptor/IODMACommand；未放宽32位 DMA 检查。
* 最终候选 `build/usb-bringup-20261022/numeric-mask-bridge/XnuBridge.efi` 已 RAM 启动。
  连续三轮原生软件重连均重新枚举，每轮 QueryType 及 ProductVersion/BuildVersion/
  ProductType 全部成功，`idevice_id` 列出目标、`ideviceinfo -s` 读到18.7.10。
  最新一次物理拔插后相同查询继续通过，另有10轮设备/配置描述符和
  GET_CONFIGURATION 读取成功。最终注册表无 DMA 错误或控制器 fault，EP0 完成
  Success；启动详细日志保持关闭，EFI 到已验证 Bash 约26秒。

目前保留的兼容性边界：本机 Linux usbmuxd 1.1.1 在冷启动过早执行 preflight，
lockdownd 尚未就绪时会放弃发布该次设备；系统就绪后需拔插一次，或执行
`usbctl reconnect`。热插拔后的标准工具识别与查询已验证，配对及文件服务未列入
本轮验证。`firmware-physical32` 仍是显式调试策略，不能据此宣称已证明 SMMU 的
architected bypass 或完成通用 Host/OTG、USB3 功能验证。

构建配方、按轮次保存的 UART/电脑端 USB 事件、完整性校验和当前未决项位于
`build/usb-bringup-20261022/`，状态索引为 `validation-progress.json`。只读诊断工具为
`usbctl registry [IOKit-class [property]]`，通过原生 IOKit 读取实际服务及等待匹配的 nub。
`usbctl mux-debug on|off` 可临时设置原生 Mux DebugLevel；要求 root 和唯一 Mux，
默认关闭。构建与硬件证据见 `numeric-mask-evidence.json`、`numeric-final-regression.json`、
`final-descriptor-regression.json` 和 `final-physical-replug-firmware.json`。
后文较早阶段的“尚未进行真机测试”描述以本节和具体运行记录为准。

## 原生等时请求、匹配及发布

Device 已接入 DWC3 的真实周期 Link 环、XferNotReady 帧种子、Start/Update/End
命令及逐帧完成。Bus Expiry 先等待真实 END，再用新的帧种子重试；取消、超时、
挂起、断开与重置也等待 END 或全核 halt 才回收已发布 DMA。尚未提交的帧直接
取消，正在映射的帧等待独立工作票据返回，旧请求仍只完成一次。

Apple 的帧结构固定为24字节：状态、请求/实际长度及 AbsoluteTime 分别位于
0、4/6、8，尾部8字节保持不动。High-Speed packet 参数按服务间隔总字节数
拆成 MPS/事务数。Full-Speed 等时使用 Family 实际写入描述符的 `bInterval`，
不再将激活参数二次换算，避免硬件周期与线上描述符不同。硬件 DEPCFG 字段限制为
0..13，软件调度保留 bInterval1..16 的完整间隔。Native 创建入口另限制等时输入
interval 为1..8；更大的输入会使当前 Family 的 FS 描述符周期非法，因此在发布前拒绝。
每控制器最多预取64帧，每原生请求最多1024帧；按端点顺序发布，可跨请求预取。
每帧解除 DMA 并回写有效 OUT 前缀后，整份帧表才交给原生 Family 回调。
原生请求的 frame 指针 union 不会被字节数覆盖，DMA 工作线程不访问 Family tag。
详见 [Device 等时审计](device-isochronous-audit.json)。

Device 按真实资源契约公布 Full/High 或 Full/High/SuperSpeed 能力；已核对的板级
上限仍为 High-Speed。通用 SS 路径在发布控制器前，将标准能力项追加到 Apple Family
的原生 BOS 列表，继续由 Family 生成响应、校验 SET/CLEAR_FEATURE、处理 SET_SEL
及 GET_STATUS。slot233 的速度/选择器和 Family 策略字节已按原生代码核对；DWC3
只应用其 U1/U2 结果与周期参数。超时或读回异常会关闭准入，禁止软件与硬件策略分离。
接口级 function wake 继续使用原生 slot203。详见 [USB3 链路审计](device-link-policy-audit.json)。
pipe property 的设置条件检查的是管道状态，USB2 音频也会设置1/2。通用路径接受
0/1/2，不消费这些不改变 Family 描述符的提示；不将其猜成 burst/stream 参数。
管道创建 slot172 已加入参数准入，继续使用 Family 的工厂与描述符生成器。已核对
创建参数的 type/direction 和描述符 synchronization 字段；它与激活时的 runtime pipe
property 不同。SS 后端的 Bulk burst 与 Family 固定的 bMaxBurst15 一致，周期端点
限定单包预算。SS ISO 默认预算收敛到1024，显式的更大预算在创建阶段返回 Unsupported，
避免 Family 生成与伴随描述符不一致的配置。CONNECT_DONE 按实际 SS/HS/FS 建链
更新 EP0；High-Speed 资源契约不会接受 SS 状态。
Host 支持普通 bulk 环，并明确拒绝可选 streams。原生 PM 通知负责调用 `createPorts`
并广播 `kUSBHostMessagePortsCreated`；端口先执行 Apple handler，再原子开放硬件
变化通知。停止状态不能被迟到的 PortsCreated 通知重新开放。

统一 kernelcache 配置包含 Apple Hub、Composite 与 DeviceMux，53 个 Kext 的
依赖及独立离线校验均已通过。AppleSMC 按原生 arm64 专用字典和兼容版本解析；
默认/架构元数据均保留，未删除依赖或加入替代框架的空壳。
详见 [原生提供者闭包](native-provider-closure.json)。后文较早阶段的实现记录以本节
和验证记录的 `current` 字段为准。

## 取消、关闭及冷休眠生命周期

普通取消、超时、关闭管道现在也走真实 Stop/Reset → Native HS hub EP0
`CLEAR_TT_BUFFER` → 真实 Set Dequeue 顺序，不再依赖 Native 错误清除标志。
仅为实际 HS TT 下的 LS/FS Control/Bulk 请求维护脏状态；失败保留脏状态、管道和
DMA 所有者，独立 TT worker 定时重试。关闭请求先撤销准入，由独立 close worker
排空普通请求和 TT 维护后再移除端点，避免持有 Native 锁等待上游 hub EP0。
Hub EP0 关闭会结束其 hub 注册及子设备准入；旧 TT 所属域不得在原 slot 生命周期重用。

系统 PM 使用冷挂起策略：先关闭准入并等待角色 worker，关断 VBUS/VCONN，证明
硬件停止并同步终止旧 Native nub，排空 DMA worker 后才交还资源。平台协议升为
version4，包含 Suspend/Resume 和 Isolate，保留外层租约，释放并重新获取内部资源。恢复后重新
检查 mapper/直接 DMA 窗口、PHY、NoC 和连接器，重建控制器、USB 地址和帧时钟。
旧连接快照、地址和帧 ID 不跨越该边界。保留上下文和 USB 远程系统唤醒尚未实现。

`setPowerState` 使用独立 worker 和延迟确认；priority sleep 通知保存原始 powerRef，
完整排空后才确认，关断或停止失败则否决睡眠并保留资源。Native Host 发布期间
持有活动态请求；已核对原生 powerDown(182)/powerUp(183) 的 unsigned long 参数及
实际函数指纹，基类帧字段更新与 IRQ 观察者使用同一短锁。

Device 策略事件带独立连接代次。reset/disconnect/offline 丢弃旧策略通知，同时仍
按原请求 cookie 排空 DMA 完成；恢复不会继承 suspended 标志。部分 online 失败统一
执行 offline 回滚，迟到的端点操作检查当前准入与运行状态。审计和边界见
[控制器生命周期](controller-lifecycle-audit.json)。这些 Native worker 尚未在内核运行。

## 小米官方板级资料核对

已核对小米官方 `sagit-n-oss` / `sagit-p-oss` 的 USB 配置，固定 P 分支提交
`5853fcdbec23aa0ea3be20bc7739581e0285eff0`；来源、原始路径和 SHA-256 位于
[reference/sources.json](reference/sources.json)，硬件结论位于
[board-resources.json](board-resources.json)。这些资料确认最高速率为 High Speed、
QUSB2 的四路供电和 TCSR 钳位、不同的 Host/Device 校准表，以及 PMI8998 SMB2
VCONN 路径。相应支持已加入通用驱动，没有新增机型命名的驱动类或构建脚本。

官方 DTS 没有给出 USB 的 iommus/stream ID，不能据此认定当前固件处于 bypass。
引导生产端仍需交接真实 IOMapper 或验证过的物理 DMA 窗口；不从 Linux DMA mask、
参考 ACPI 或缺少 DT 属性推导当前硬件的翻译/缓存一致性状态。板级 USB 节点仍未启用。

EFI 拓扑采集现已同时绑定控制器资源：从实际安装的 FADT 选择 DSDT，通过标准
`EFI_ACPI_SDT_PROTOCOL` 查找所选组件的 `_HID`、`_ADR`、`_CCA` 和 `_CRS`，
只解析已实现的有界静态资源构造，不执行 AML。得到的40字节资源记录与 IORT
一同交接；Native mapper 会比较实际 core/QScratch 范围、核心 GIC 中断及一致性属性。
退出 EFI 前后的配置表入口、父表、命名对象和两份输出均需保持一致，变化会清空输出。
公共固件 DSC 已启用该标准协议，固件及桥接均完成离线编译。记录仍不授予 DMA、
context 或时钟所有权；真实停机和资源租约的生产端继续补齐。

同一桥接入口新增 `--usb-rpm-snapshot`，要求同时选择 USB 拓扑。它先以实际
`EFI_LOADED_IMAGE_PROTOCOL` 定位 NpaDxe，比较固定映像的完整代码段和只读协议表，
按实际加载基址核对212处 DIR64 重定位；随后只读取有界 EFI RAM 中的请求缓存。
version2 的20544字节记录保存134个现有 RPM 时钟、MAX限制、带宽、域和供电键的观察，区分资源/键缺失、
active/next 共用的已提交槽、active/sleep/next 待提交值及各级 dirty 状态。
退出 EFI 后不调用 NPA/GLINK，只比较先前记录的内存和输出；变化会清空记录。
缓存缺项不推导为零投票。该版本在序列化时更新已提交缓存，错误回包也会退休消息，
因此已提交值、空队列及 GLINK Close 都不能证明 RPM ACK 成功。记录的 grants 固定为0，
尚未转换成内核资源租约或 DMA 许可。
固件的 channelUp 字节只在收到连接事件时置位，断开时不会清零，也不能作为实时
GLINK 连接证明；后续通道接管仍需检查真实关闭和新的握手/ACK。

`--usb-firmware-stop` 已接入固件软件停机，并自动包含上述 RPM 观察。它先校验
UsbConfigDxe 的完整代码和不可变协议模板、78处加载重定位及实际管理对象位置，
再读取客户端字段或调用接口。同 GUID/版本号的未知对象不能被当作资源所有者。
在 TPL_NOTIFY 下关闭轮询、重新配置、模式切换和退出四类事件，清空其已拥有的
事件槽，再恢复 TPL_APPLICATION 调用 StopController。随后还会解除旧客户端句柄
别名的驱动绑定，先撤下客户端协议、最后撤下管理协议，并确认没有剩余接口。
退出回调被关闭后，不再执行其迟到的 PHY 断电和内部 core 释放；这些资源留给后续
接管阶段重新验证。首次关闭事件后遇到失败，桥接保留引导内存和资源，停在重置边界；
不会返回 EFI 或进入内核。64字节软件停机记录只在 EFI 成功退出、代码与状态复核后
写入，grants 仍为0。原始 ARM64 Image 入口拒绝这个需要 EFI 的交接模式。
该步骤尚不证明寄存器 halt/BCR、时钟和 NoC 租约或 DMA 所有权。

`--usb-rpm-cold-handoff` 在同一入口包含软件停机和 RPM 观察，并增加冷通道交接。
它绑定实际安装的 GlinkDxe，核对完整代码、协议表、83处加载重定位、退出事件 GUID、
唯一 RPM transport、实际 SRAM TOC 和 FIFO 描述。在 EFI 退出后，只有软件停机记录
有效、这些绑定未变且 TX/RX 的四个游标全部为0，才发布40字节记录。固件原有退出
回调会向 `glink_ssr` 发送 APSS cleanup 并等待游标复位；桥接不额外调用该回调，也
不把该观察当成资源 ACK。回调顺序造成迟到的 Close 或其他非零游标时，交接失败。

QcomRPM 1.5 使用 handoff version3 接收冷记录，以 NpaDxe 的实际下一消息编号开始
新的握手，不清零旧游标。允许先到达的有界 VERSION 流量，保留其内容；旧数据、
关闭消息或本地游标变化会拒绝接管。新的资源请求仍需匹配 ACK，消息编号回绕跳过0。
与 GPU cold start 同时选择时，由桥接消费这份冷记录一次，再通过现有48字节在线
通道记录交给 Native version2 接收，避免启动两个通道。现有27个时钟、8个总线和4个
电源域的基线要求保留；该记录不会补写缺失的资源投票或授予 DMA。

`--usb-hardware-stop` 还会在退出 EFI 后、进入 XNU 前处理真实控制器停机，并自动
包含上述冷 RPM 交接。三种停机/冷通道选项现在共同执行完整的软件退休、冷通道检查
和硬件隔离序列。它先检查固件软件停机和实际控制器绑定；若 USB BCR 尚未
置位，检查继承的 USB GDSC/五路叶时钟仍可用，使用同一 DWC3 Core 实现完成
Host/Device halt 与中断屏蔽，复核这些状态未变后才置位专用 BCR。已经处于 reset
的控制器不再读取失去时钟的 core。64字节 `chosen/qcom,usb-hardware-stop` 只证明
DMA 已隔离，不授予映射或资源租约；进入 XNU 前再次比较输出并读取 BCR。
失败会保留引导内存并停在重置边界。这个入口不重调时钟、不改 PHY/NoC/SMMU，
原始 Image 入口继续拒绝需要 EFI 的模式。实际后端与固件入口已离线编译，未启动。

已加入原生 `QcomUSBMapper`（174 槽），只作为资源租约下的设备 mapper，不替换
系统 mapper。它要求引导资源所有者提供128字节、version1 的
`qcom,usb-smmu-handoff`，并同时核对实际安装的 IORT、控制器/translator aperture、
精确 stream 路由、已有 AArch64 stage1 context、有效 ASID、其他可见 bank 的停用状态和实际故障中断。
QcomUSB 1.6 保留观察到的8位 CBAR.VMID，但不把 guest VMID 当作物理命名空间的所有权依据。
小米官方 ROM 的 HYP 会保存 guest CBAR，再按 SID 分配表选择底层 VMID；不同 guest VMID
或 ASID 仍可能发生全局 TLB 条目冲突。当前路径拒绝其他任何可见活动 bank，直到具备可信
资源所有者的完整授予依据。运行中观察到条件失效会锁存故障，停机拆根并重建后才能恢复。
16位 VMID 模式尚未实现。初次接管前必须证明 NoC/GCC 租约、USB BCR 及完整寄存器快照；不会从参考 IORT
分配 context 或猜测空闲 ASID。当前接受已有活动 context 的交接，空 bank 的分配仍由
可信资源所有者负责。板级节点仍未启用，资源策略及空 bank 的生产路径继续补齐。

QcomUSB 1.1 还支持 `qcom,usb-smmu-capture=<1>`：先要求上述两份软件/硬件停机
记录与实际安装的 IORT、控制器绑定一致。原生资源服务取得真实 GCC/NoC 租约并
确认 BCR 后，才读取活动的精确 stream、现有 stage1 bank、ASID 和8位 VMID，
比较全部可支持的 SMR/S2CR，并确认其他可见 bank 均已停用，再生成原有128字节记录。
整个采集过程不写 SMMU。已有记录的接收路径会再次核对寄存器，然后按既有隔离
流程换根。未知通配路由、共享 bank、其他可见活动 bank、过大的 SMR 数量、状态变更、空
bank 和 bypass 均不能生成这份活动 context 记录。
平台中断可提供 core 加一个已知 context IRQ，或 core 加 IORT 的完整 context IRQ
列表；mapper 按实际 bank 选择并检查来源，角色 nub 仍只向 Apple USB Family 提供
core IRQ。这个选择字段只请求采集，不代替软件/硬件停机、NoC/GCC 租约或实际寄存器。

官方 ROM 的 `rpm.mbn`、`hyp.mbn`、`tz.mbn`、`devcfg.mbn`、`xbl.elf` 已通过范围读取
和 CRC32/字节哈希核对，来源与静态反汇编证据见
[`stock-smmu-namespace-evidence.json`](../../build/usb/stock-smmu-namespace-evidence.json)。
该资料不证明设备当前固件、AP 的 active/sleep 基线或运行时资源授予；尚未执行固件或真机测试。

QcomUSB 1.2 增加 `qcom,usb-dma-capture=<1>`，与活动 context 采集互斥。它在同样
的软件/硬件停机、实际 IORT/控制器绑定、GCC/NoC 租约和 BCR 检查后，只读采集
精确的 SMMU `S2CR.TYPE=BYPASS` 路由。64字节 `qcom,usb-smmu-bypass` 保存实际
全局状态、ID、SMR/S2CR；窗口按 IORT 与硬件输入/输出位宽约束生成，继续使用
现有32字节 `qcom,usb-dma-window`。无匹配项、通配重叠、FAULT、禁用 context
及其他翻译状态均不能生成这个窗口。采集不访问 context bank，也不写 SMMU。
Qualcomm 固件可能把 BYPASS 写成 FAULT，上游用专用 context 模拟旁路；这里要求
实际读回的精确 BYPASS，不进行写探测或全局改写。
该模式的第三段设备内存仍是实际 SMMU aperture，中断只提供 core source。资源服务
保留只读映射，准入、Sample、角色验证和归还时持续检查原窗口与硬件路由；先验证
NoC 可访问再读取 SMMU。变化会关闭准入并保留原租约，完整排空且重新证明后才归还。
冷 PM 可重建该映射，但不会静默接收另一份路由。原生 Family 继续使用现有物理 DMA
分配/地址限制，不创建替代 mapper。完整板级资源策略、空 bank 和共享 bank 的所有权路径仍未完成。

QcomUSB 1.3 增加独立选择值 `qcom,usb-dma-capture=<2>`，只继承已存在的禁用
stage1/S2-bypass bank。仍要求同样的停机、实际 IORT、GCC/NoC 租约和当前 BCR。
精确 SMR 必须以 TYPE=TRANS 指向独占 bank；ID1 与 IORT 都必须覆盖该 bank，
CBAR 必须精确为 `0x10000`，SCTLR 必须为0，FSR 除 FORMAT[10:9] 外必须为0，
CBA2R 只能为0或1。其他
有效 stream 使用同一 bank、stage2、启用翻译、故障/停顿或未知位均被拒绝。
这一路径生成80字节 version2 观察记录，原 TYPE=BYPASS 的64字节 version1 不变；
两者都使用同一32字节窗口和资源生命周期。只读检查仅增加所选 bank 的控制/故障
以及对应 CBAR/CBA2R，不读取 TTBR、ASID 或其他 bank，不写 SMMU，也不创建
空 context。运行期间 bank 被启用或共享、路由/记录变更、NoC 丢失均关闭准入并
保留租约，归还必须恢复原观察并重新证明 BCR。该配置对应上游 Qualcomm 的
[禁用 context 旁路形式](https://github.com/torvalds/linux/blob/v6.12/drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c)，
这里继承实际配置，不执行上游的写探测或 bank 预留。模型覆盖每次 MMIO 读取失败、
租约/复位丢失、bank 属性变化、共享路由、4KiB/64KiB 寄存器布局和 DMA 归还。

QcomUSB1.13增加 `qcom,usb-dma-capture=<3>`，显式选择未匹配stream的旁路观察。
依据[ARM SMMUv2规范2.3.6](https://documentation-service.arm.com/static/5f900d34f86e16515cdc08fb)，
只有实际启用Stream matching、完整匹配表没有该SID的有效匹配，且实际
`sCR0.USFCFG=0`时，才生成64字节version3记录。slot固定为UINT32_MAX，SMR/S2CR
字段为0，表示没有对应条目，不代表创建了空闲条目。精确或通配的任何匹配都会拒绝
这一模式；不会自动退回其他DMA模式。现有version1/2记录与选择值不变。
该路径沿用实际IORT、EFI软件/硬件停机、NoC/GCC租约及BCR要求，采集只读取全局
寄存器和完整SMR/S2CR表，不读取context、ASID或VMID，也不写SMMU。
运行时及归还前继续检查原全局状态和无匹配条件；出现匹配、USFCFG改变或fabric丢失
会关闭准入并保留DMA租约，冷PM也不能接收不同观察。完整AP基线仍为独立前提，
不会从缺失iommus或参考DTS推导此模式当前可用。

同版本修正禁用context旁路的FSR判断：FORMAT字段独立于故障，关闭的bank中其值
还可能为UNKNOWN，因此不再要求整个FSR为0。依据ARM规范的 `FaultSetIn`，只忽略
FORMAT[10:9]，其余任何非零位仍拒绝采集或归还。模型使用无故障AArch64读数
`0x400`复现修复前的拒绝，并验证四种FORMAT编码、故障位以及原DMA租约保留。

资源生产端现在统一在 `scripts/build-bridge.py --usb-resource-profile`，没有新增构建
脚本。`make usb-bridge` 会依次构建集合、运行桥接及工具链回归，再构建 EFI；默认
只预留采集记录，仍不启用 USB 节点。已有可信板级策略时可使用：

```sh
make usb-bridge USB_RESOURCE_PROFILE=/absolute/path/usb-policy.json
```

策略是 version2 JSON，所有资源请求和限制必须显式提供，不设置推测的默认值：

| 字段 | 内容 |
| --- | --- |
| `version`、`component` | 2，以及实际 ACPI 组件路径，例如 `\\_SB_.USB0` |
| `maximum_speed`、`core_clock_hz` | Native speed3=HS、4=SS；60/120/150MHz，SS至少120MHz |
| `dma` | `mode` 为 `active-context`、`architected-bypass`、`disabled-bypass` 或 `unmatched-bypass`；显式 `address_bits`32..40、`smmu_base`0x1680000、`smmu_bytes`0x10000；活动 context 还需 `context_irqs`。整数明确选择电平 GIC INTID；`[INTID,IORT flags]` 可显式选择电平0或边沿1，必须匹配实际 IORT |
| `phy.usb2` | `supplies` 的 `[RPM id, operating uV]`；布尔 `differential_ref`、`otp_trim`；`device_init`、`host_init` 的 `[offset,value]` 数组。Host序列必须正确包围 power down/up |
| `phy.usb3` | 仅SS策略提供，包含两路 `supplies`；生成QMP及独立TCSR资源 |
| `connector` | 显式 `sid`2、`adc_base`0x4500、`source_microamps` 和布尔 `vconn`；不把充电输入限流当作OTG输出预算 |
| `regulator_limits` | `[id,min uV,max uV,boot floor uV]`，覆盖全部PHY供电及需要保留的其他AP轨；使用Native同一份PM8998目录校验范围和步进 |
| `rpm_domains`、`rpm_clocks`、`rpm_interconnect` | 上述完整4/27/8组显式AP策略；USB主时钟60/120/150MHz要求持久active CX floor至少64/128/256，MX active非零；三个NoC时钟floor与clock表完全一致 |
| `rpm_clock_limits` | 12条 `[index,activeMax,sleepMax]`，身份为0..8、12、19、26。0明确释放AP的LIMIT_MAX请求；其他值保留对应wire上限。请求不得超过非零限制，三个NoC的本地请求范围也必须落在两个set的限制内 |

生产端检查USB固定240MB/s平均、800MB/s峰值预算，以及BIMC利用率/双通道公式；
与GPU共用时再检查合并后的EBI平均流量和BIMC时钟。共同RPM策略必须一致，不能由
后一个消费者覆盖。USB冷交接与GPU策略组合要求GPU cold startup，不能把未保护的
warm GPU带过AP资源清理。两者共用一条RPM通道，最终由Native接管。

资源树在副本中完整校验后一次提交：保留无关GCC和PMIC所有权，冲突、错误地址
映射、缺失provider aperture及外部MMIO占用均拒绝。生成core/QSCRATCH/SMMU、
PHY、NoC和PMI8998中断/连接器属性，按Apple DT规则转换数值及混合宽度记录，
匹配现有AppleARMIODevice及原生Family personalities。实际集合必须含足够版本的
九个本地Kext；当前资源策略要求QcomUSB1.13、QcomRPM1.13，以及能转发频率限定请求的
GenericARMPlatform1.1和GenericClockController1.3。检查实际Mach-O
inventory，不能用sidecar版本替代。

QcomRPM1.8 将固件中已定义的 PIMEM 加入完整基线，追加 index26，保留原0..25的
身份。项目固定 ClockDxe 的 `/clk/pmem` 使用 `pmem0`、`KHz`，旧26项策略遗漏了
这一路。USB与GPU现都要求显式27项 active/sleep 值，固件观察也包含该键；缺项、
重复身份和超范围值会拒绝交接。两个 PIMEM set 都得到成功ACK后，才继续NoC及
全局缩放；未知完成保留资源并禁止缩放。低功耗表里的0及最大值均不作为生产默认值。
实际旧QcomRPM1.7集合已验证不能通过新策略门槛，原冷通道观察ABI仍兼容。
QcomRPM1.9 进一步限制 index19 的 QDSS STATE 为0..4。它不是KHz频率，RPM/NPA
可能先钳位状态再返回ACK，不能据此声称原输入已生效。原生和USB/GPU引导入口同时
拒绝范围外的 active/sleep 值；真实报文模型覆盖全部25种合法组合，策略生成覆盖两个
消费者。旧1.8集合也会被完整策略入口拒绝。详见
[STATE校验及验证记录](../../build/usb/clock-state-evidence.json)。
QcomRPM1.10 在同一请求中同时交接主键和显式 `max`，避免固件保留的旧上限继续钳位
已确认的最低请求。12项限制不得缺失、重复或由另一个消费者覆盖；NoC范围不一致会在
连接RPM前拒绝。新观察格式只增加这些键的缓存记录，grants仍为0。旧策略版本1和旧
运行时不能代替这部分交接，见[限制交接验证](../../build/usb/clock-limit-evidence.json)。

QcomRPM1.11 还对同一 PMIC 时钟的 `swen` / `pccb` 按各 set 的目标状态排序。
目标软件使能为1时先发软件请求；目标仅引脚使能时先发引脚请求。前项 ACK 成功后
才发另一项，明确拒绝只重试当前项，丢 ACK 则锁存故障，避免撤掉旧控制后留下空档。
目标全关不会临时新增使能。官方 RPM 的硬件 apply 顺序仍是先 SW 后 PIN，因此
合并 KVP 不能替代这个排序。6组时钟、32种初态/目标/引脚组合和两套 set 共384条
时钟轨迹已通过真实 GLINK/RPM 报文模型检查；88个拒绝/丢 ACK 步骤也检查旧控制保留。
消息总数、27项请求、12项 MAX 和版本2策略格式不变；USB/GPU 的实际 collection 均
要求RPM1.11，见[控制交接验证](../../build/usb/clock-order-evidence.json)。这些是离线模型结果。
官方XBL的固件卷经GZIP、FV和FFS校验后解出69个UEFI映像；ClockDxe的12项远程资源
描述与项目固件一致，但映像本身不同，因此没有套用旧偏移或扩展运行时pin。
这些静态上限和低功耗请求仍不能提供完整生产AP基线，见
[官方固件资源核对](../../build/usb/stock-xbl-resource-evidence.json)。
静态定义、调用及公开BSP的范围见
[固件资源追溯证据](../../build/usb/firmware-clock-namespace-evidence.json)。公开源码树主要
面向SDM660；其MSM8998 ClockChipset XML只关联配置名，没有给出本引导链的完整AP
基线。SMMU接口只暴露init/map/unmap，DALSys的ANOC1表只列寄存器区域；对项目30个
固件模块的两类GUID字面量检索未检出GUID字节匹配。这些结果没有产生空bank、ASID或
VMID授予，生产数据和相应所有权路径仍需补齐。

QcomUSB1.5同时接受IORT声明的电平和边沿context IRQ，通过共享生产代码核对
Apple GIC specifier及实际 `getInterruptType`，支持完整bank IRQ表或一个所选bank。
IRQ与DWC3 core重叠、多个bank共享一条故障线、未知标志、控制器/触发类型不符
均拒绝。PM激活前重新匹配原始IRQ号、触发类型及event source索引，不能改用
新线路；失败时不启用中断、不挂接新根页表。停止、采集和mapper建立继续使用
原有生命周期，验证不写
SMMU寄存器。模型覆盖1/6/128个bank、混合触发类型、每次provider读取失败及
最后一次读取发生变更。桥接生产端保持每行显式触发标志，不再统一生成电平。

小米公开N/P源码中ANOC1节点相同：SPI364..369为边沿，转换后INTID396..401。
项目中的候选IORT则含八个电平context条目。这些都是参考；驱动以实际安装且
校验通过的IORT和GIC类型为准，不据此改写固件表或断言运行时配置。
参见[小米公开DTS](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/5853fcdbec23aa0ea3be20bc7739581e0285eff0/arch/arm/boot/dts/qcom/msm-arm-smmu-8998.dtsi)。

生成的GCC/IRQ表声明调用者显式策略中的控制范围；节点选择运行时采集，不生成
DMA窗口、context记录、运行时ready状态或租约。自QcomUSB1.4起，对
`qcom,usb-resource-handoff=<1>` 先校验安装IORT与固定软件/硬件停机记录，读取
实际BCR18和GCC主时钟163的继承频率，再向QcomRPM1.13发送限定consumer9的
`PrepareUSBResources`，`id` 覆盖继承频率与目标频率中的较高者，`value/flags` 为0。
RPM在发送AP资源请求前核对持久active CX基线是否支持该频率；另一消费者临时
抬高的ACKed投票不能替代基线。BCR不会关闭继承的主时钟，因此目标60MHz不能
授权在继承150MHz尚未降频时使用LOW_SVS基线。
它取得完整AP电源、域、时钟及NoC基线ACK并复查通道，USB再查询ACKed CX档位，
确认继承频率未变、满足所需档位且BCR仍为1，
才开始GCC domain/clock/reset变更。已有外部资源所有者路径继续使用原契约。
统一生产端同时在RPM节点写入 `qcom,rpm-usb-core-clock-hz`，一个little-endian UInt32
目标Hz。共享初始化器在所有消费者的AP基线操作前执行同一检查，包含仅电压请求及
EFI GPU cold startup；不能由较早启动的PHY或GPU绕过USB的频率保护。该属性本身
不授予资源。未知时钟或不足基线会拒绝AP资源发送，已有GLINK握手不等于资源ACK。
对应共享模型验证每个准备阶段失败、复位撤销、未知ACK保留及PM重新获取顺序。
进入XNU前的安装IORT及固定固件停机证明、Native的真实GCC/NoC/PHY/连接器租约、
RPM成功ACK和SMMU读取仍是必要条件。48项桥接回归使用明确标识的合成策略，
覆盖四种采集、HS/SS、OTP选择、字节序、冲突回滚、版本和双消费者预算。
合成策略只在临时目录中测试，不作为可启动的板级生产策略。

频率与CX最低档位来自小米官方固定版本内核的
[`usb30_master_clk_src` FMAX表](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/5853fcdbec23aa0ea3be20bc7739581e0285eff0/drivers/clk/msm/clock-gcc-8998.c#L252)、
`vdd-level-8998.h` 和 RPM level枚举，已保存在 `reference/board/` 并记录哈希。
这些数值只补齐USB自身的最低要求，完整AP资源基线仍须显式提供。
同版本的RPM clock handoff会主动发送保守请求，SMMU驱动会保存实际SMR/S2CR并统一
管理context位图；其ASID/VMID公式不能作为本驱动分配空闲或共享命名空间的依据。

小米6已核对的板级参数仍是HS、60MHz、四路QUSB2供电及PMI8998 VCONN路径；
完整且可信的AP资源基线数据尚未补齐，不能从NPA请求缓存、参考DTS或测试数值
直接推导。因此默认构建继续保持USB节点关闭，空闲/共享context所有权也仍待完成。

原厂固件的静态提取已并入同一工具链，不增加独立脚本入口：

```sh
make analyze-usb-resources
```

输入目录由 `USB_FIRMWARE_DIRECTORY` 指定，默认使用已核验的
`build/usb/firmware/stock`；输出为
[`firmware-resource-extraction.json`](../../build/usb/firmware-resource-extraction.json)。
命令完全离线，检查五份原厂映像与五份内核源码的SHA-256，只解析ELF中实际有文件
字节的区间，拒绝把BSS或运行时RAM当作原厂文件内容。输出是证据报告，不能作为
version2 USB资源策略使用；未提取到的生产输入保留为null。

现已逐个核对原厂 RPM 的初始代理投票调用：APSS 的 `spm0/Enable` 为 active1、
sleep0，`clk0.0/Enable`（XO）为 active1，`rwcx0/vlvl`（CX）为 active256。
另两项 SPM 调用属于 MPSS。提取器记录调用地址、常量地址和实际指令字节；这些
初始值可能被后续请求替换，缺少的 sleep 值不补0。现有本地 EFI 观察选定134项键，
其中106项对应的资源缺失、28项目标资源存在但指定键未找到。记录没有给出每个
资源的完整键目录，不能据此断言整个缓存为空；这些结果也不能补齐生产基线。依据见
[`rpm-initial-proxy.evidence.json`](../../build/usb/rpm-initial-proxy.evidence.json)。
已再次以实际 NpaDxe 的键查找、键分配和资源初始化函数核对解析偏移；现有 MAX
用例也验证了资源有其他键时，当前选定键仍可为 `presence=1`。核对过程见
[`rpm-cache-revalidation.evidence.json`](../../build/usb/rpm-cache-revalidation.evidence.json)。

当前原厂HYP包含六个SMMU描述符，ANOC1的静态分配条目包含参考IORT中的SID
`0x1400`及VMID值3；同条目还有其他SID，且SID查询所检查的字段在原始文件中为0。
因此不能将条目序号6当作USB的空闲bank，也不能把3直接用作运行时物理VMID。
已确认的SMC `0x02000c18`入口先把外部instance映射为内部设备，再要求MMNOC
设备4；USB所在ANOC1是设备0。这个安全域切换接口没有提供USB context/ASID
分配契约。RPM普通请求路径及已识别的SystemDB调试路径也没有建立完整AP
active/sleep基线的返回契约。反汇编及公开BSP接口核对保存在
[`firmware-interface-analysis.json`](../../build/usb/firmware-interface-analysis.json)。
这些结果补齐了静态归属与接口语义，尚不能替代运行时资源交接；没有执行固件、
Native内核或真机测试。

映射器在发布 PTE 前持有实际 MD/command；失败后保留 IOVA、页表及调用者令牌。
角色切换先 halt，再在 gate 外通过 Isolate 拆除旧根页表，随后同步终止 Native nub。
只有实际 BCR、根页表移除及 TLBSYNC 证明后，才能确认被 Native complete 消耗的
失败映射，随后只清空描述符，不重复 complete。外部未知 mapper 继续保留故障。
冷 PM 保留 context/ASID/VMID 的所有权，但在归还 NoC 前释放已排空的私有表页；恢复重建
根页表和 DWC3 视图。Native nub 只得到 core IRQ，SMMU IRQ 由 mapper 独立处理。
详见 [IOMapper 审计](iomapper-abi-audit.json)。这些 Native 调用路径尚未在内核运行。

已经进入 Pinned 的普通和等时清理现在也可在实际根页表拆除后重试一次。Nub 保存
本代隔离证明；旧代或外部客户的故障通知不能影响新控制器。Device 同时排空已经
交还 Native 错误回调、但仍保留 DMA 的无上下文节点。工作票据不读取旧 tag，也不
重复原生回调；Host 保留原有帧顺序、有效数据和 copy 参数。全部队列、线程和分配
清理完成后才解除自身保留引用，并发故障上报不会重复 retain。

IOMulti/Interleaved 等 Native 内存描述符在 complete 返回错误前可能已经完成部分
子描述符，因此这类错误保持保留，不重复 complete。只有命令映射的实际退休证明
允许清理被消耗的 IODMACommand 状态。内部状态错误、不确定的 Native enqueue 和
再次失败的清理继续保持 Pinned 并否决睡眠。内存描述符的实例大小、complete 槽28
和缓冲区公共方法另有当前内核样本的 ABI 校验。这些属于离线模型及编译验证。

DMA 缓冲区和页表的非一致性存储使用 Normal NC。`kIOMemoryMapperNone` 与
IOBuffer 的缓存位域重叠，已从分配选项中移除；页表明确要求 host 物理连续。
IOBuffer 按内核页大小扩大 backing capacity 后恢复请求的逻辑长度，避免16 KiB
内核页下的4 KiB表页及短 DMA 缓冲区失败。UPL 到4 KiB SMMU页的拆分已覆盖
4/16/64 KiB 内核页和非零首偏移的离线模型。

## 分层与 Apple 接口

SDM835 的 DWC3 同时提供 xHCI Host 和 DWC3 Device 数据通路。Qualcomm 层负责
QSCRATCH、PHY/时钟资源和角色切换；Host、Device 适配层分别接入原生
`AppleUSBHostController` / `AppleUSBHostPort` 和 `IOUSBDeviceController`。
USB 设备、接口、管道、描述符策略及用户客户端应继续由 Apple Family 管理。
不将 Apple SoC 的寄存器操作、PHY 或 DWC2 数据通路用于 SDM835。

平台 personality 已使用 `IOProviderClass=AppleARMIODevice`、
`IONameMatch=qcom,msm8998-dwc3` 绑定平台层，使用 IOKit nub/personality 匹配
两种控制器适配层。`QcomUSBController` nub 按角色命名为 `usb-host` / `usb-device`，
在工作循环上关闭旧请求准入、屏蔽其实际 nub 的中断并证明 DMA 停止，然后在 gate
外同步终止旧 nub，之后才发布新一代 nub。失败时保留旧对象、mapper 和平台资源。
不能直接构造控制器子类来绕过 matching，也不能用兼容字符串伪装 Apple USB 硬件。

[iOS 18 模型证据](ios18-model.json) 来自当前项目的 iPad7c arm64 kernelcache，
映像 SHA-256 为 `aa65c36c486a8b5cb1d7a2d565d1d6b3cc228b3c172f8c69c5da664ee19832cf`。
样本的内部 SDK 为 iPhoneOS 18.7 / 22H368；编译工具链使用 iPhoneOS 18.6 SDK。
分析沿用项目的 `kernelcache_patcher inspect-abi`；Device Family 的 `start` 路径
另外以 IDA 反编译核对：调用原生 `IOService::start`，通过虚函数取得工作循环，
建立 command gate，取得设备 IOMapper 和 command pool。因此适配层必须先准备
`getWorkLoop()`，再进入 Family 的 `start`。

| 类 | 当前样本实例大小 | 当前虚表槽数 | 参考项目 iOS 12.4 槽数 |
| --- | ---: | ---: | ---: |
| AppleUSBHostController | 1384 | 269 | 260 |
| AppleUSBHostPort | 920 | 235 | 230 |
| IOUSBHostIOSource | 96 | 37 | — |
| IOUSBHostPipe | 184 | 67 | — |
| IOUSBDeviceController | 560 | 235 | 225 |
| IOUSBDeviceControlRequest | 160 | 19 | — |
| AppleUSBHostRequest | 64 | 53 | 44 |
| AppleUSBHostDMACommand | 144 | 33 | 32 |

旧 Kext 的带符号虚表已提取到
[参考记录](reference/apple-reference-vtables.json)，记录输入文件 SHA-256。
它们只提供方法名称和分层线索；不能按固定偏移平移套用到 iOS 18。
当前模型 JSON 保留类发现记录。Device Family 已新增
`drivers/include/IOKit/usb/IOUSBDeviceController.h` 和
`IOUSBDeviceControlRequest.h` / `IOUSBDeviceTypes.h` / `IOUSBDeviceInterface.h`，以及供统一校验器使用的
`drivers/abi-22H374/apple-usb-device-abi.json`。契约固定实例大小、父类、235 槽及
78 个原生导入，其中 59 个通过当前样本的虚表绑定；另有 10 个原生非虚方法的
可执行字节指纹。编译探针还检查继承的
`free/setProperties/start/stop/callPlatformFunction` 覆盖位置，防止它们退回 IOService。

Host 端口和控制器已有当前 iOS18 的投影与编译探针。新增
`AppleUSBHostDMACommand.h` / `AppleUSBHostRequest.h` 后，连同 IO source/pipe 投影，Host 契约覆盖八个类、
300 个导入、274 个虚表绑定及 32 处可执行字节指纹；另一个
`apple-usb-common-abi.json` 覆盖 AppleUSBRequest / AppleUSBRequestCompleter，
固定 56 个导入、50 个虚表绑定及 1 处指纹。Common 类由 `com.apple.driver.usb.AppleUSBCommon`
提供，Kext 已分别声明这两个 Apple 依赖。完整 Host 服务尚未添加 matching personality。

[Host ABI 审计](host-abi-audit.json) 记录了当前映像的重要差异：slot168 reset 接收
options，slot169 接收 milliseconds 和 options；slot204 返回原生 device 指针。
端口的地址锁已移到 slots208/209，slot210 createDevice 只收 options，再委托
控制器创建并启动 Family device。旧版 createSetAddressPipe 接口没有套用到当前
控制器。Host Family 的 start 自行创建 workloop（控制器偏移160/gate168，端口偏移656/gate664），
不能照搬 Device 端的 getWorkLoop 覆盖来假定所有 gate 已共享。原生 HCD 还需显式连接
角色 nub 的硬件 gate，并避免跨 loop 等待产生锁顺序反转。
`QcomUSBController::runHardwareAction` 已提供同步桥接：nub 保留角色工作循环，
进入该 gate 后重新检查角色代次和连接准入，过期 nub 无法访问寄存器；
`registers/deviceCore` 也检查当前 gate。硬件 callback 返回后再执行 Family 策略与
客户端完成，锁顺序为 pipe/device → controller → role，端口路径为 port → controller → role。`QcomUSBHost` 初始化路径已调用这条桥；完整端口/请求运行路径仍在接入。
`AppleUSBHostController.h` 固定 1384 字节与 269 槽，已补齐管道/I/O/stream/DMA
硬件钩子的参数类型，closePipe / closeStream 及 gated 入口均按当前调用点返回 IOReturn。
Host 契约现覆盖 9 个类、300 个导入、274 个虚表绑定与 34 处指纹；尚未审计的基类
slot 仍不供运行代码调用。原生 helper slot194 通过 UInt8 speed / UInt16 address
初始化真实 IOUSBHostDevice；端口公共代码负责 attach/start。createPorts 已是一次性
数组分配实现，原生数组/数量字段为 1216/1224。

新增原生 `IOUSBHostIOSource`（96 字节/37 槽）和 `IOUSBHostPipe`（184 字节/67 槽）
投影。描述符集合自然对齐为 32 字节，endpoint/SS/SSP 指针偏移为 8/16/24；仅 wire
描述符使用 packed。管道初始化的 controller/device/interface owner 由 Family 保留，
速度位于原生基类尾部填充偏移92，当前投影不访问这些私有字段。IOSource slot22
返回 device，slot27 写出状态，并非 controller getter。

`QcomUSBHost.cpp` 已实现原生基类 start/stop 周围的硬件准备：按角色代次进行
halt/reset、读取协议能力、按控制器实际地址宽度分配 DMA、配置 command/event/DCBAA/
ERST/scratchpad。64 KiB 对齐的 16 KiB arena 防止单段环跨越 64 KiB；scratchpad
逐页准备。硬件寄存器发布前记录 DMA 所有权，释放前要求 halt 或 provider 完成隔离，
失败则保留对象、nub 和 DMA。DMA 分配/释放及 reset 等待位于 gate 外，Family 的
workloop 保留。初始化路径现已执行 run 并安装专用电平 IRQ；通过真实 CommandQueue 的 No-op 完成事件
检查命令/事件通路。一级 filter 不碰状态和 MMIO，由 IOKit 屏蔽电平中断，Native
workloop 的 action 再经 role gate 按批量128处理事件。命令等待使用原生 commandSleep/
commandWakeup，避免阻塞处理 IRQ 的工作线程；命令超时保留所有者，完成后由独立
thread call 在 gate 外释放。stop 关闭准入、唤醒等待者，证明 halt/隔离，再拆 IRQ、
timer、worker 与 gate。未在内核执行验证；metaclass 仍 abstract，没有 personality，
端口创建/通知已编入代码，完整设备/管道/传输适配及注册生命周期尚待完成。

[事件审计](host-event-audit.json) 记录 ERDP.EHB 的竞态修正：先清 USBSTS.EINT 和 IMAN.IP，
再写回 ERDP 释放 EHB，避免把刚到的新事件清除。模型测试在 EHB 释放瞬间注入中断，
并覆盖混合事件预算、错误端口、未知事件和命令隔离。端口变化标记由 worker 在 controller/role gate 外交给原生端口；尚无
Native pipe 的传输事件会触发故障隔离。


`QcomUSBHostPort` 已实现当前 Native 235 槽接口，实例大小936字节；Native 部分仍为920字节。
HCD createPorts 先调用原生一次性表分配，再按原生 EHCI 的模型 attach/start 端口，
提供4字节 OSData("port")。Native 端口的 controller retain 位于偏移768；原生
free 负责释放它。外置端口类型按当前 EHCI 返回0，不套用参考项目的固定3。
电源、挂起、恢复和复位通过 Host/role gate 与共享的 xHCI 端口状态机执行；等待时
释放 controller gate，shutdown 唤醒并排空等待者。W1C 确认会保留复位/恢复使用的
完成位；通知复制端口 retain 后在 gate 外调用原生 signalInterrupt(0)。
[端口审计](host-port-runtime-audit.json) 记录了这些接口与离线验证边界。

Native locationID 解析已加入：高字节为总线号，23:20 为 root port，其余5个半字节
为 hub 路径。转换成 xHCI Route String 时移除 root port 并反转 hub 半字节顺序，
拒绝不同总线和有洞的路径。根设备、注册 hub 下的设备及其 Enable/Address/Disable Slot
已接入代码。Control/Bulk 错误及 Address Device 失败时的 TT 清理也已接入；
普通取消/超时/关闭的 TT 清理已接入；Native 电流池和完整注册生命周期仍需完成，Host
继续为 abstract，也没有 matching personality。


根设备 slot 路径已加入 Native createDevice(slot192)/retireDevice(slot193)。每个设备
使用64 KiB对齐的16 KiB分配，分别容纳 input/output context 和 EP0 环。DMA 准备与
清理由独立 worker 在 gate 外执行，其屏障对象不含 MMIO 引用。控制器侧表先持有
分配，再发布 DCBAA；Address Device 成功只解除 input 冻结，output/EP0 仍归硬件。
检查输出 slot 为 Addressed、root/speed/context 数正确、EP0 处于原始 dequeue/cycle，
再把实际1..127的 USB 地址交给原生 helper，不能使用 slot ID 代替。Native Device
经过 IOUSBDevice 兼容基类继承 IOService（中间基类136字节/168槽）；当前只用336字节
opaque view 和已审计 getter，保留其真实 Native metaclass。

退休回调保留 Native device 并排入 worker，等待真实 Disable Slot 成功、清 DCBAA 后
才解除映射。超时则隔离整个控制器；无法证明停止或完成 unmap 时保留 controller、
nub 与 DMA policy。Native 引用由另一个 worker 在 gate 外释放，避免阻塞准备新设备。
[slot 审计](host-slot-audit.json) 和模型验证了地址73/slot3、两种 context stride、四种
速度、输出/命令错误及 unmap 重试。注册 hub 下的 route/TT 路径也已接入，尚未在内核执行。

Native hub 注册使用当前 iOS18 的 slots195/196/197 包装器和198/199/200 gated hooks，
没有沿用旧头文件的槽号。包装器参数、USB2/SS 描述符、32位 flags 和调用者均已核对；
USB2 flags bit0 仅在 Native Hub 已选择多 TT interface 后出现。HCD 使用实际
Configure Endpoint 命令、仅设置 Slot Add 标志更新 Hub/ports/TTT/MTT，检查真实保留
命令记录及输出上下文后才开放下游枚举，不用 Evaluate Context 更新 hub 属性。
该命令不重设 EP0 dequeue、最大包长或传输环。

下游设备使用父 hub 的真实侧表记录与当前 Supported Protocol speed ID。低速/全速
设备引用最近的高速 hub slot 及其下行端口；经过全速 hub 时仍保留上游 TT，而全速
hub 自己的 MTT 位清零。USB 地址不作为 TT slot ID。Native locationID 的五级路径
与当前 Native hub 的15端口范围共同检查；SS 与 USB2 拓扑不相互降速或混用。

父引用在异步准备之前取得。hub 注销立即关闭新子设备准入并退休整个子树，但保留
已安装的硬件 Hub/TT 配置；父 slot 只有在子设备 Disable Slot 和所有 DMA unmap
成功后才可释放。失败清理、尚未完成的创建和旧位置占用均保留这些引用。已安装的
Hub/MTT/TTT 属性不能通过注销后重注册改变，需要新的设备/slot 生命周期。
带 TT 后代的高速 hub 注销后，也要求新的 USB 设备/slot 生命周期，避免沿用未经复位的 TT 状态。
[hub 与 TT 审计](host-hub-audit.json) 记录了真实命令证明、混合速度树、深度边界和
子设备 unmap 失败时保留父 slot 的模型。普通取消/超时/关闭的 TT 清理现已接入；完整 Native hub 生命周期仍需内核运行验证。

Native 管道 create/adjust/close（controller slots217/223/224）已接入：使用标准
OSTypeAlloc 分配真实 IOUSBHostPipe（184字节/67槽），其 metaClass 指针和原生分配器
均已核对。原生 initializer 继续持有 device/controller/interface 与工作循环；原始描述符
指针来自 Family，不能指向先于 Native 管道释放的 HCD 侧表。HCD 单独复制描述符用于
硬件编码，Native 的策略描述符更新仍由 Family 完成。

除 EP0 外，每个端点使用独立的4 KiB环并按64 KiB对齐；Configure Endpoint 的 Add、
Drop 和 Replace 都使用真实命令队列及被冻结的输入上下文。检查输出端点状态、最大包长、
dequeue/cycle，并核对输出 slot 的地址和拓扑；关闭后只有新的 Drop 完成或 Disable Slot、
控制器隔离才能解除 DMA 所有权。环为空时调整端点使用当前 producer 地址和 cycle，
不能回到环首重新执行旧 TRB。EP0 复用设备分配中的环，最大包长通过 Evaluate Context
更新，保留当前 dequeue。DMA 映射和解除仍在 worker 中运行，Native 对象由独立 worker
释放。[管道审计](host-pipe-runtime-audit.json) 记录了两种 stride、错误命令/掩码、环绕回、
EP0 包长与映射清理的模型验证。Control/Bulk/Interrupt 请求的提交、完成及取消已接入
代码；ISO 多帧请求和 Control/Bulk TT 错误恢复也已接入。多缓冲区请求、电流预算、
冷挂起 PM 和普通取消/超时/关闭的 TT 清理已接入，完整注册尚未完成，控制器继续
abstract，尚无 matching personality，也未做真机测试。

帧时钟已接入原生控制器：slot233/234 返回64位毫秒帧号，输出参数接收 AbsoluteTime，
并非 IOReturn。slot266 读取 xHCI MFINDEX 的14位原始微帧，slot267 按
`(rollovers << 11) + (raw >> 3)` 合成帧号。回绕计数及时间戳的字段布局由 iOS18
原生 EHCI 过滤器与 Family getter 交叉确认；不把 EHCI 寄存器模型套在 SDM835 上。

USBCMD.EWE 产生回绕事件。中断过滤器使用独立只读游标扫描真实 event ring，先更新
Family 时钟，正常事件消费者仍负责 ERDP 和命令/传输完成。扫描、事件确认前的补扫
以及关闭均由 IRQ-safe 锁串行化，避免工作线程延迟、重复扫描或硬件投递竞态导致漏记。
停止前先关闭观察器，再 halt 和释放 DMA。帧计数器由 provider 提供可撤销的单次读取
接口；getter 不访问可释放的 HCD State，也不等待 workloop gate。角色撤销、close 和
halt 均会关闭读取权限。Native 两个 getter 继续由 Family 执行，HCD 只提供底层计数
和回绕状态；HCD 的查询入口也会在 Native 快照前补扫回绕事件，覆盖电平 IRQ 暂时
被屏蔽、action 仍在等待 gate 的情况。[帧时钟审计](host-frame-audit.json) 记录了布局、离线模型和剩余的
IST/等时调度、PM 重同步工作；尚无内核运行验证。

ISO 多帧请求已接入 Native 入口、DMA/TD 后端和完成队列。原生事务记录为28字节，旧帧记录为24字节；
`getFrameList` 已改为实际交易类型，原生三个 getter 的64/64/32位读取指令也已固定。
原生 I/O 长度取 offset+requestCount 的最大覆盖范围并按 MD 长度截断，不能当作
各帧字节数之和。HCD 按单个事务的 offset、requestCount 和 Wrap 标志复制独立 payload，
支持稀疏范围和多次跨越 MD 末尾。IN 只回写实际完成的字节，保留空洞和未完成尾部；
只解除该事务的 DMA 后才复制数据，其他事务可以继续由硬件持有。Family 的原始 MD
仍只 retain，不重复 prepare/complete。

Core 已读取 HCSPARAMS2.IST 和 HCCPARAMS1.CFC。调度辅助代码保留64位毫秒帧号，
按 ESIT 推进事务；指定帧窗口使用 `currentFrame + ceil(IST/8) + 1` 到
`currentFrame + 895`，并检查 ESIT 边界。ASAP 使用 SIA。CFC 表示连续 TD 的 Frame ID 检查能力，不是首帧 Frame ID 的支持开关。
无 CFC 时，连续流首个 TD 使用 Frame ID，后续 TD 使用 SIA；有 CFC 时按 TD 设置帧号。等待队列容量的 TD 会按最新 producer 位置重新计算环尾填充，
已提交的 TD 不受影响。

预取可跨 Native 请求继续，每 pipe 最多128个 payload，全控制器最多256个；不按整份
帧表分配 DMA。提交维持端点 FIFO，CPU 回写和逐帧发布也跨请求维持顺序。原生 slot19
`update()` 的 IOReturn 签名与指令已固定；每帧解除 DMA 并复制有效 IN 数据后，写入
现代帧的完成数、时间戳和状态，再调用一次 Native update 转换可选旧帧。全部帧处理完后
才 detach 原始请求并交给 Native completer。Native complete 自行发送 packet filter
stage1，HCD 已去掉重复通知；stage0 按原生 HCD 在接纳和拒绝时各报告对应状态。

Missed Service 按实际 payload 地址退休已跳过的 TD；规范允许事件环满后只报告最后一笔，
因此也处理有序的跳过前缀。No-op/ED 地址不能作为这类完成证明。ISO 的缓冲区错误、
Babble、USB transaction error 和 ISO overrun 按帧报告，保留硬件确认的有效前缀，
后续 TD 继续执行。取消和超时则覆盖端点受影响的所有 Native 请求，等实际 Stop/Reset +
Set Dequeue 或控制器隔离证明后清理 DMA；未准备的帧分批发布取消结果，不分配 DMA。
硬件已完成的帧不被迟来的取消覆盖。worker 有工作量上限并穿插 EP0 和最终完成处理。

这些代码通过了真实 TransferQueue/CommandQueue、120帧跨3个请求的流水线、逐帧错误、
取消、故障注入、ASan/UBSan 与 arm64 编译校验。Native worker 尚未在内核执行；
PM 重同步和其他适配器工作仍见
[等时传输审计](host-isochronous-audit.json)。

每端点现有独立的 `IsochronousFlow`，按实际发布的 TD 保持已知时基。软件队列为空不代表
硬件数据流结束；非 CFC 请求间需要间隔时，先等待真实 Ring Overrun/Underrun，再使用新
Frame ID。最后一笔 Native 请求交还后收到环空通知仍属正常。长 ESIT 的已知连续流使用
SIA，不把895毫秒 Frame ID 上限套到 SIA TD 上。准备好的私有 payload 可在发布前调整
Frame ID/SIA；发布后保持不变。

非 CFC 或含 SIA 的数据流发生 Missed Service 时，保留尚未提交的帧，隔离已经提交且
可能失步的 TD。实际 Stop/Reset + Set Dequeue 完成记录负责退回 DMA；随后用真实
Configure Endpoint Drop+Add 重建端点，核对输出状态、dequeue 和 DCS 后才恢复提交。
未提交的 payload 只重绑定 dequeue/cycle 和调度字段，不重复映射或复制客户端数据。
端点重建不等待被其他 pipe 持有的 device lease，取消和 CPU 清理仍可使用同一 worker
推进。强制 Stopped 事件可指向 producer gap 或 Link，只有新的实际 Stop 命令仍被队列
保留时才接纳这类通知；通知本身不作为释放 DMA 的证明。

设备级 `suspendPipesGated` / `resumePipesGated`（Native slots221/222）已接入实际 HCD。
Apple 的 slots205/206 包装器负责校验设备所属控制器，并通过原生 command gate 传入
`IOUSBHostDevice*`。HCD 关闭该设备的新请求和 DMA 发布，等待已有 worker 退出准备或
恢复阶段，再对运行中的端点发出真实 `Stop Endpoint(SP=1)`；核对命令完成与输出端点
Stopped 状态后，保留普通 Control/Bulk/Interrupt 的 DMA、DATA/STATUS 进度和控制器
内部部分传输状态。正常恢复只重新敲 doorbell，不改 Set Dequeue，也不重放 SETUP。

暂停期间取消普通请求需要新的真实 Set Dequeue 完成记录才能退休 DMA；Stop(SP) 的
证明只在队列连续禁止提交和 doorbell 时有效，不能跨恢复或控制器隔离复用。原命令
核对后即可归还命令槽，避免长期暂停耗尽共享命令池。所有成功 Stop 都至少保持10ms
后才恢复，超时按设备关闭准入的时间延长；暂停失败会恢复已停端点，恢复失败则隔离
整个控制器。成功 Stop 后若输出上下文不可信，也直接隔离，不能当作未执行的 Stop
重新放行。

等时传输在设备暂停时中止：已发布帧保留到真实 Stop(SP)+Set Dequeue 或控制器隔离，
未发布帧由 worker 清理并按序发布取消结果。恢复后的新 ISO 工作先执行已验证的
Configure Endpoint Drop+Add；空 ISO 队列也使用新上下文，避免继承旧 Frame ID/SIA
状态。Native 最终完成仍由 Family 执行。相关证据与测试边界见
[设备队列暂停审计](host-power-audit.json)。这一步保持运行中的 MFINDEX，完整控制器/
系统睡眠现采用上文的整控制器冷挂起和新帧时钟策略，尚未在内核运行。

原生请求的 slot23 返回 completer，slot24 返回已 prepare 的内存描述符，
不是 DMA command 和 timeout。具体 AppleUSBIORequest 的对应字段为 624/104。
slot25 是字段512中的32位毫秒超时；slot26 才是字段520中的64位 I/O 长度；
slot27 返回字段64中的原始控制客户端 IOService，不能用于 CPU 数据复制。
这三项已按原始 LDR 指令、prepare 参数及调用者重新校正，并加入具体 getter 的
可执行指纹，不能只凭 fallback getter 的返回值猜测字段。当前 slot21 是 complete、
slot22 是 cancel，状态/长度/时间戳 setter
为 slots32/33/34。硬件请求退休并完成 IN 数据复制后，将原始请求交给 slot29 的
AppleUSBRequestCompleter::enqueueRequest；Family 根据请求自身策略选择 thread call 或
工作循环，调用客户端完成并完成 MD/归还请求池。提交给 completer 前必须清空 HCD
侧表，并清理 HCD 的 DMA command，以免原生 MD complete 遇到未解除的 DMA 引用。
分析数据库原先把 complete 入口并入 prepare；已仅修正数据库函数边界，未修改内核
映像字节。complete 的依据为原始汇编，失败的反编译结果没有作为证据。

基础 Host 请求通路已接入原生 controller slots218/219/220，使用同一份
XHCI::RequestMemory 与真实 TransferQueue/CommandQueue。每个 pipe 保留软件 FIFO，
基础 Control/Bulk/Interrupt 一次只发布一笔硬件请求，以便控制端点按 IOService 客户端取消时保留其他客户端尚未
发布的请求。普通请求上限为每 pipe 64笔、controller 255笔；EP0 普通请求上限63笔，
保留一笔 EP0/controller 容量供 Native TT 维护使用，总容量仍为64/256笔。超时从原生入口接纳时计算，0表示
不设置 deadline。准备与数据复制在 worker 中运行；HCD 只 prepare 私有 DMA 缓冲区，
不再次 prepare/complete Family 的 MD。64 KiB对齐的 bounce 与环尾 No-op TD 前缀
避免把真实请求的包或 burst 拆过 Link。

QcomUSB1.7 支持原生 bundled 完成策略。`isBundled` 是 completion kind3，
Native prepare 仍交出单独的 MD、字节长度及超时；HCD 按普通 Bulk/Interrupt 请求
接纳、退休和取消，不读取或填充 Family 私有的完成数组。Native completer 按
target/action/context 将最多16笔已完成请求合并回调；HCD 在移交前清空侧表并退休
DMA，移交后只释放既有引用。模型覆盖32种组合、536笔实际 TD 退休、短包、超时、
隔离及同步复用，也检查了 enqueue 返回不确定时不重复移交。bundled 标志不能用于
ISO 帧布局，Bulk streams 仍按公开能力返回不支持。依据见
[原生 bundled 证据](../../build/usb/bundled-native-evidence.json)。

传输事件按真实 slot/endpoint/cookie 路由；Control DATA 短包仍需等待 STATUS 完成。
取消、超时和已知硬件错误通过新的 Stop/Reset Endpoint 与 Set Dequeue 完成记录恢复，
这些记录在队列核验前不会 acknowledge。普通端点的错误码2/3/4/6/36会保留独立的
Native clearStall 阻塞状态，Stop/Reset+Set Dequeue、重配和 DMA 回收都不会提前解除。
STALL 使用原生0xe0005000状态；非控制端点的 Native clearStall 取消所有请求、替换空
上下文并复位主机端 toggle，随后由 Native Pipe 选择是否发送 CLEAR_FEATURE。
LS/FS Control/Bulk 使用独立 TT worker：真实 Stop/Reset 完成后，通过实际高速祖先
的 Native Device slot207 发送 CLEAR_TT_BUFFER，再取得真实 Set Dequeue 完成记录并
回收子设备 DMA。控制端点清理两个方向，bulk 保留原方向；single-TT index为1，
multi-TT 使用实际分支端口。普通请求 worker 同时处理父 hub EP0；TT 请求不占 HCD
或 role gate。最多保留16笔 TT Stop 记录，并预留 Native 请求容量。

这份 iOS18 镜像的 Hub194 在 TT wValue 中使用 hub 自身地址，已核对原始 ARM64。
HCD 的维护请求使用真实下游 USB 地址；TT slot 仅用于定位真实 Native 祖先。Native
HubPort 还会在清理失败后继续调用 HCD，因此只有 HCD 自己确认清理成功才允许解除
阻塞。Native control-error completer 的 clearStall(false) 流程仍保留；若维护重试成功，
也通过该 Native 入口完成控制管道恢复。维护失败、epoch 撤销和隔离均不会提前重启。
[TT 错误恢复审计](host-tt-recovery-audit.json) 区分了实际共享队列模型、arm64 编译证据
与尚未执行的 Native workers。

Address Device 的真实完成码4会在 acknowledge 前锁存默认地址恢复状态，核对实际
queue/epoch/owner/slot/input 与提交前的 ticket 下限。工厂先返回失败，随后仍由同一
TT worker 通过真实高速祖先清理地址0的两个控制方向，不创建假的 Native 子设备或
pipe。同一 TT 上的新枚举和 Control/Bulk 发布在成功前保持关闭，失败至少间隔500ms
重试。全速中间 hub 退休时，缓冲区仍由实际高速 TT owner 管理；其父引用与 slot
保持到清理成功或 owner 生命周期关闭。清理 TT 不释放 DMA：仍须 Disable Slot/
隔离及成功 unmap 才能归还子节点。

未知或畸形事件仍使 controller 停机隔离。命令引用释放使用独立 worker，避免 Native
对象析构等待设备工作循环时阻塞恢复命令。

完成 worker 先解除 HCD DMA、复制有效 IN 数据，再清空 HCD 侧表及 pipe 计数，写入
原始请求的状态、长度和 AbsoluteTime，并在 gate 外交给原生 completer。Native 的
普通完成负责 MD complete、客户端回调与池归还；不能提前调用 cancel 代替硬件停止。
close/设备退役先排空 HCD 请求，unmap 失败则保留请求、Native MD、controller 和
DMA policy。[请求运行时审计](host-request-runtime-audit.json) 记录了离线队列/内存
验证及边界；工作线程调度和 Native 生命周期目前只编译，尚未在内核执行。

`QcomUSBHostDMACommand` 已链接进 Kext，保持原生 144 字节基类、33 槽和 144 字节
子类布局（不添加实例状态）。客户端 command 固定 Host64、64 位地址、alignment=1、kUnmapped、无
mapper；普通/扩展 MD setter、get/clear、两种构造和 prepare specification 入口均
保留 CPU 复制语义，clone 仍返回该 Host 子类。原生 Host setter 的隐藏复制缓冲区和
完成时的缓存失效不用于这条客户端路径。所有硬件 TRB 必须使用另行创建的
IOMemoryAPI / DMARegion 缓冲区及真实 mapper/显式物理窗口。Host 初始化适配层的 getDMACommand 已返回该子类，
缓存、原生请求及数据复制的内核运行顺序仍须验证，不能把 ABI 编译通过当作运行成功。
`USBHostDMA.h` 新增 HostBounce：仅 retain Family 已 prepare 的客户端 MD，使用独立
DMARegion 提供硬件地址。IN 复制在硬件 command clear 后执行，随后由 Family completer
完成客户端 MD 和回调。离线测试覆盖失败重试、短传输尾部、隔离与零长度请求；该模型
尚未绑定 Native request 侧表及工作线程。

原生端口状态的速度字段不是项目内部 `USB::Speed` 枚举：Apple Full=1、Low=2，
High=3、Super=4。后端从 xHCI speed ID 经 Supported Protocol/PSI 解码，再显式
转成 Apple 状态；OHCI/EHCI 原生状态实现分别核实低/全速和高速编码。
设备创建回调使用另一套枚举：Low=0、Full=1、High=2、Super=3，已按原生端口
分发表 `[1,0,2,3,4,5]` 提供显式双向转换；不把 SuperSpeedPlus 冒充为 SuperSpeed。

[Device ABI 审计记录](device-abi-audit.json) 保存原生调用和请求布局证据。
Family 分配的请求为 104 字节，缓冲区、DMA、链表、状态、超时、长度和完成存储的
偏移都带编译断言。未消费的方法仍保留为 `nativeSlot`，不能直接调用或覆盖；
普通完成回调使用 `target / parameter / IOReturn / UInt32 bytesTransferred`，等时完成
使用帧指针。适配层结束并清除 DMA 后调用原生 `completeEndpointIO`，由 Family
执行过滤器、回调和普通请求/缓冲区释放；内部请求的所有权由 Family 保留。
原生控制请求对象负责描述符和功能策略，EP0 后端只负责传输阶段。
对象在 OSObject 尾部填充区使用状态字段，头文件必须保留完整 160 字节，不能让
子类状态覆盖最后的工作循环指针。端点激活也必须转发第五个 pipe-property 参数。
`USBDeviceABIProbe` / `USBControlRequestABIProbe` 仅供编译校验，没有链接成 Kext，
也没有注册或接触硬件。必需纯虚钩子已补齐参数类型；其中 panic 和强制断开两个钩子
仍使用编号名称，已有当前样本调用证据。pipe-property、等时接口和未消费的非纯虚
槽仍需审计。Family 速率编号 FS=1 / HS=2 / SS=3，与后端 `USB::Speed` 不同。
Interface/Pipe 仅投影原生非虚 getter 和原生 metaclass，用于接口的 alternate setting
与端点编号分配；没有自行实例化、继承或重建这两个原生对象。其大小分别为 312 / 120 字节。

`QcomUSBDevice` 使用角色 nub 的共享工作循环，先建立工作循环、DMA 和事件源再进入
原生 Family 的 start。描述符、配置与功能分发由原生控制请求对象处理；驱动转发
SETUP、OUT 数据和状态完成。收到新 SETUP 前先 reset 旧 tracker，避免原生状态 4
在 inputData/inputSetup 中触发 panic。tracker options=1 表示硬件负责短响应 ZLP，
与 DWC3 后端一致，避免原生 Family 再提交一个独立 ZLP。

普通请求从原生 `ioGated` 进入 64 槽侧表，Family 的 104 字节请求不扩展、不借用
未审计字段。每个端点按序处理一个有限 TD；请求序号与控制事务/总线代次分别检查。
DMA 准备和清理使用独立 thread call，不调用 Family 策略或客户端回调。完成通知先占
策略 FIFO 的位置，清理完成后才执行原生 completeEndpointIO，使新 SETUP 不会越过
旧事务的完成回调。同步取消会释放共享递归 gate 等待真实 EndTransfer 完成与清理，
并在返回前排空原生回调。停止失败的 DMA 留在隔离区；完成回调可报错退出，但后续
不会再访问已释放的 Family tag 或复制到客户缓冲区，资源保持到显式隔离恢复。
这部分已完成 arm64 编译和共享请求模型测试，尚未在 IOKit 内核运行。

xHCI 传输后端现已实现，见 [传输审计](host-transfer-audit.json)。它与 input context 共用
`encodeEndpoint` 校验，支持 SG 的 64 KiB 拆分、TD Size、零长度和独立 Bulk OUT ZLP，
以及 Isoch 的 burst / Frame ID 编码。Control 的 SETUP、DATA、STATUS 分为不同 TD。
DATA 与最终完成各用一个 Event Data 标识；客户端长度来自累计值，DATA 短包后继续
等待 STATUS，不能按普通传输的短包路径提前完成。

Event Data cookie 由同一控制器的真实 CommandQueue 分配，端口/管道重建也不重置；
旧 cookie 不会命中新端点的首个请求。元数据、请求引用和环计数先于有效 cycle 位
发布，故障 doorbell 仍返回由队列持有的 ticket。超时、物理错误事件和准入关闭会
保留 DMA。退休必须检查同一 CommandQueue 中隔离开始后新发出的 Stop/Reset
Endpoint 与 Set TR Dequeue Pointer 完成，连同 owner、slot、DCI、DCS 及目标地址。
也可等待后端提供真实控制器复位隔离证明；软件状态位不作停止依据。

quiesce 是端点范围操作；Native 控制管道按 client 选择取消时，只停止当前受影响的
硬件请求，其他 client 的未发布请求仍保留。Isoch 已连接 MFINDEX/IST 调度、Native
多帧请求和 DMA worker。传输后端的离线测试不能证明原生 HCD 或 USB 枚举可用。

## 已实现的硬件后端

代码位于 `drivers/usb/QcomUSB`，名称不含具体机型前后缀。

| 文件 | 当前实现 |
| --- | --- |
| QcomUSB.h / QcomUSB.cpp / Info.plist | Apple IOKit 平台服务、独占角色 nub、共享工作循环、连接器通知/采样、故障时保留资源 |
| QcomUSBResources.h / QcomUSBResources.cpp | 独立 IOKit 匹配的资源聚合层、PHY/连接器租约与通知桥接、真实 mapper 保留及部分失败清理 |
| USBControllerResources.h | GCC 冷/热交接、固件时钟引用接管、停止/复位证明、根速率与 GDSC 生命周期 |
| USBDirectDMA.h | 显式物理窗口、NoC/BCR 授权、地址模式互斥及故障后的资源保留 |
| USBInterconnect.h | 独占 NoC 带宽租约协议，Acquire/Check 必须返回真实授权 |
| QcomInterconnect/QcomInterconnect.cpp / InterconnectResources.h | 原生 IOKit NoC 服务、AP QoS、GCC 叶时钟、RPM 路径投票与实际 BCR 释放保护 |
| QcomRPM/RPMInterconnect.h / RPMClockHandoff.h | 64 位带宽累加、时钟最大值、交接基线和全局时钟缩放的 ACK 顺序 |
| QcomUSBHostDMACommand.h / QcomUSBHostDMACommand.cpp | 原生 Host DMA 子类、客户端复制用准备/清理和 clone；硬件 DMA 另行分配，完整 HCD 尚未接入 |
| QcomUSBDevice.h / QcomUSBDevice.cpp | 原生 Device Family 适配、端点编号/配置、EP0 策略连接、普通请求 worker、同步取消与完成；实例分配和 matching 仍关闭 |
| QcomUSBPHY.h / QcomUSBPHY.cpp | AppleARMIODevice 匹配的 PHY 服务、独占 open/租约、实际 GCC/RPM 调用、QFPROM/TCSR 读取及失败资源保留 |
| USBPhy.h / USBPhyTables.h | QUSB2 校准与 PLL 检查、MSM8998 QMP 两组通道校准、方向选择、PCS 就绪与复位停止证明 |
| USBPhyResources.h / USBPhyProtocol.h | PHY 时钟/电压投票、复位、部分失败回滚、共享轨投票隔离与内核租约协议 |
| USBDeviceRequests.h | 64 槽请求侧表、独立 DMA 工作票据、端点顺序、超时、旧序号拒绝与隔离后的清理 |
| QcomPMIC/QcomUSBConnector.cpp / PMICUSB.h | PMIC 连接器服务、实际 SPMI/IRQ、CC/VBUS、RRADC/升压、条件供电授权与电源状态 |
| QcomPMIC/PMICTypeC.h | Type-C/Rp 冷初始化、异步 CC2 检测与寄存器恢复、Sink 安全退出 |
| USBConnectorProtocol.h | 连接器独占租约、采样/供电 RPC、通知排空协议 |
| USBPlatform.h | 平台资源获取/释放、真实连接器采样和供电请求的版本化内核接口 |
| USBProtocol.h | 平台资源契约、DMA 地址范围、USB setup 包解码 |
| USBMMIO.h | arm64 设备内存访问、边界检查、DMA 屏障和短轮询延迟 |
| DWC3Core.h | IP/能力探测、Host/Device 停止、角色选择、Device 复位、事件环、端点命令、USB2 远程唤醒和 UTMI-as-PIPE 时钟切换 |
| DWC3Endpoint.h | Control/Bulk/Interrupt 的 Full/High/SuperSpeed 参数检查、端点命令编码 |
| DWC3Transfer.h | 有限 SG TD、OUT 尾包容量检查、短包/ZLP、异步取消、完成和 DMA 隔离 |
| DWC3Control.h | EP0 SETUP/DATA/STATUS、延迟状态阶段、新 SETUP 抢占、STALL 恢复、速率协商和异步取消 |
| USBDMA.h | 描述符/请求缓冲区所有权、尾包 bounce、失败回滚、DMA 隔离和可重试清理 |
| USBIODMA.h / DMACommandState.h | IOMemoryDescriptor / IODMACommand / 设备 IOMapper 绑定、显式完成与粘性解除映射故障 |
| QcomSMMU/PageTables.h / ContextTLB.h / IOTableMemory.h | ARM64 stage-1 页表、ASID/TLBSYNC 与 IOKit 表页分配 |
| QcomSMMU/HardwareContext.h | 独占 stream/context 交接、FAULT 写入差异、真实读回及根卸载；尚未绑定原生服务 |
| QcomSMMU/MappingLedger.h / MappingEngine.h | 保护页、对象身份、映射引用、隔离与确认后复用的 IOVA 生命周期 |
| XHCICore.h | xHCI 能力检查、停止、异步复位状态、DMA 寄存器配置、doorbell 和事件确认 |
| XHCIPower.h / XHCITransfers.h | 设备暂停准入与超时栅栏、真实 Stop(SP) 连续停止证明、部分传输保留及暂停期间取消 |
| XHCIPorts.h | Supported Protocol/PSI、原生端口状态、W1C 安全写入与异步端口复位/挂起/恢复 |
| XHCIContexts.h | 32/64 字节 input/device context、slot/TT/端点参数、共享无副作用端点编码、命令冻结与匹配完成后复用 |
| XHCICommands.h | 命令编码、完成地址/slot 校验、引用保留、超时隔离及确认后的清理 |
| XHCITransfers.h | Control/Bulk/Interrupt/Iso TD 编码、分阶段 Event Data、端点队列与命令证明后的 DMA 退休；尚未接入原生 HCD |
| XHCIRing.h | TRB 环、Link/cycle 所有权、事件消费、完成退休和 64 KiB 边界拆分 |
| USBRole.h | 连接代次/方向/供电角色、通知准入 stamp、立即停止旧连接、防抖、过期 RPC 回滚与 DMA 隔离 |

硬件逻辑由 MMIO 模板与 IOKit 对象生命周期分开；测试直接编译同一份后端。
DMA 环要求 CPU 与设备能互相观察内存更新。IOKit 绑定在平台确认硬件缓存一致性时
使用普通分配，否则明确请求 `kIOMapInhibitCache`，供环和独立 bounce 使用。
仅有屏障不足以支持直接映射的非 coherent 客户缓冲区；这条路径仍不允许绕过 bounce。
原生 DMA 命令单独保存完成状态，关闭自动 prepare，显式 prepare/complete 后才调用
`clearMemoryDescriptor(false)`。iOS18 的自动清理会忽略 complete 错误，而 complete
即使失败也会消耗活动计数和映射元数据。因此未知完成后保持故障，不把再次清理返回
成功当成解除映射证明；命令、已 prepare 的缓冲区和资源租约继续保留。
明确的 mapper 隔离与恢复协议仍待原生 SMMU 服务实现。
角色 nub 只在平台确认硬件 snooping 时发布 `dma-coherent`，不会把禁缓存分配等同于
硬件一致性。当前只验证了此分配路径的 arm64 编译，实际缓存属性仍需内核运行验证。

关键约束如下：

- 没有停止证明就不替换 DMA 地址、不释放描述符、不切换控制器。
- xHCI 的单段环除了 Link TRB 外还留一个空槽，避免最后一个数据 TRB 完成后，
  软件在硬件读取旧 Link 前改写其 cycle。整条 TD 先写入，再最后发布首 TRB。
- DWC3 EndTransfer 的 CMDACT 清除只表示命令已接受。返回 `Pending` 并阻止该
  端点的新命令，直到消费 EndTransfer 的 EPCmdCmplt；失败也保留停止中的状态。
- 角色状态机的 stop 回调必须停止并排空实际控制器；失败时故障保持，保留旧控制器，
  不自动启动另一种角色。显式恢复要求平台先完成复位和 DMA 隔离。
- connector 后端负责 VBUS 的来源及供电角色判断。供电由明确的 Source/Sink 角色决定，
  不由 Host/Device 数据角色推断。切换检查不能放电外部 VBUS；QSCRATCH 的 VBUS
  override 不等于 PMIC 供电控制。这一层不执行 PD 协商。
- 端点启用要等 Configure 与 Resource 命令完成；停止中的端点不能重新配置。
  活动端点仅允许 CONNECT_DONE 为正在接收 SETUP 的 EP0 修改最大包长；DATA/STATUS
  没有该例外。EP0 OUT 的传输资源编号 0 是合法值，不能按未分配处理。
  OUT 请求的映射容量按最大包长取整，额外容量必须来自 bounce buffer；不把相邻内存
  作为尾包空间。IN 发送完成长度必须与映射长度相符，需要时添加 ZLP。
- 取消与 XferComplete 竞争时，仍等待 EndTransfer 的完成事件。错误的 HWO/剩余长度
  或命令超时会隔离 TD；只有控制器停止证明才能回收。控制器切代次时拒绝旧完成。
- SETUP_PENDING 会使当前 DATA/STATUS 事务失效；DATA 阶段要等状态阶段就绪后再
  STALL/重收 SETUP。Family 的过期数据或状态批准不能应用到新事务。
- DMA 分配、映射和完成拷贝在准备/完成线程运行，不能在工作循环 gate 中调用可能
  阻塞的 IODMACommand。DMA 时仍保留请求和平台资源，清理失败也不能丢弃状态。
  初始请求路径采用最多 1 MiB 的独立 bounce；仅复制请求实际完成的长度。

## 平台资源

Linux v6.12 的 MSM8998 DT 确认 DWC3 aperture 为 `0x0a800000 / 0xcd00`，
核心中断 SPI 131；QSCRATCH 为 `0x0a8f8800 / 0x400`，QMP USB3 PHY 为
`0x0c010000 / 0x1000`，QUSB2 PHY 为 `0x0c012000 / 0x2a8`。
这些是 SoC 资源；VBUS 接线、连接器/PMIC、efuse trim 和最大速率必须由平台提供。

首个资源契约覆盖持续保留的时钟、电源、已校准 PHY、NoC 和独占控制器所有权，
以及设备 DMA 地址位宽。无 IOMapper 的路径要求引导层明确交接物理地址窗口；不能根据属性缺失推断硬件旁路。
`QcomUSBResources` 已实现聚合接口，以同一 AppleARMIODevice 上的独立
`IOMatchCategory=USBResources` 匹配，等待 `USBConnectorResources` 和
`USBInterconnectResources`。`qcom,usb-controller-resources` 为三个 little-endian
UInt32：core 目标 Hz（60/120/150 MHz）、DMA 地址位宽（32..40）、最大速率
（High=3 / Super=4）。这份配置只描述能力，不证明资源已经就绪。

Acquire 校验明确的 DMA 模式并取得连接器租约，再保留 GDSC2。翻译模式保留实际设备 IOMapper。若 BCR18 已断言，
从实际复位状态进入冷交接；否则先保留现有五个时钟并停止固件遗留 DWC3 DMA，
之后才允许 GCC 接管对应固件时钟引用。保持 BCR 断言期间释放旧叶时钟、配置
根163与164，再保留 cfg71、master116、aggre1-USB32、sleep118、mock117。
新的 `AdoptClock` 仅允许内核 USB consumer9 操作这五个已授权时钟；共享引用仍阻止
重配。`GetResetState` 读取实际 BCR，不能用上次释放保存的状态推断下一次启动。
获得真实 NoC 授权和校准 PHY 后才解除 BCR；USB2 路径通过 QSCRATCH 的三个步骤
切到 UTMI-as-PIPE，SuperSpeed 恢复真正 PIPE。PHY 专用时钟现有带实际 BCR 检查的固件 pin 交接。

NoC 请求采用固定平台带宽 240 MB/s 平均、800 MB/s 峰值，来源于固定 commit 的
MSM8998 总线向量。生产者必须持有完整 QoS/根时钟路径并获得 RPM 带宽 ACK；
只打开 GCC 的两个 AXI 叶时钟不能证明带宽有效。`QcomInterconnect` 已实现该路径的实际生产者，详见下节。
聚合服务接受两种互斥的地址模式：活动的设备 IOMapper 对象，或显式物理地址窗口。
翻译模式不接受未解析的 `iommu-parent` ID。物理模式不调用系统 mapper，不写全局 SMMU
旁路寄存器，也不从 `iommu-parent` / `iommus` 缺失推断地址映射。两种描述同时出现时拒绝启动。
上述生产者缺失时不会发布资源，不会用 DT 就绪标志或空成功返回代替。

聚合层已桥接连接器通知、实时路由校验、供电预算和部分失败租约。SuperSpeed
Source 要求至少 900 mA 明确预算；Sink-only 可使用零 Source 预算。Prepare 在真实
控制器停止后才允许翻转 QMP，Prepare/Validate 完成前再次采样连接器。Release
先关闭自身供电并停止/复位 controller，再退 PHY、NoC、连接器、时钟/GDSC。
释放开始即收回寄存器访问权；部分失败仍保留租约，但调用者不得再次访问可能已经
断钟的 MMIO。聚合层的系统 PM 和显式故障恢复仍待实现。


### NoC 与 RPM 时钟交接

`QcomInterconnect.kext` 使用 `IOProviderClass=AppleARMIODevice` 和
`IONameMatch=qcom,msm8998-a1noc`，保留独占 open 和调用者绑定的租约。
其唯一 MMIO 资源是 `0x01660000 / 0x60000`；`qcom,owned-qos-ports` 必须明确
授权 USB qport2。QoS priority/mode 地址为 aperture 内 `0xb008 / 0xb00c`，
从 fabric offset `0x9000` 加 qport2 的 `0x1000` stride 得到。
服务只改对应 priority/mode 字段，依次写 P1、P0、fixed mode；释放时恢复原字段，
并保留其他位。修改 QoS 和降低任何投票前都读取 GCC 的实际 BCR18，必须证明
DWC3 已复位；不能只依赖客户端声称已停止。GCC71/32 的额外引用覆盖 QoS 访问。

RPM 后端使用现有 GenericClockController 工作循环串行化所有电压/时钟/带宽报文。
驱动内部资源 ID 如下，不是 Linux phandle 或 clock binding ID：

| ID | RPM 类型 / ID | 内容 |
| --- | --- | --- |
| 0x2000 | aggr / 1 | A1NoC 时钟 Hz，报文 key=KHz |
| 0x2001 | clk1 / 1 | SNOC 时钟 Hz，报文 key=KHz |
| 0x2002 | clk2 / 0 | BIMC 时钟 Hz，报文 key=KHz |
| 0x2100 | bslv / 142 | A1NoC-SNOC slave 平均 B/s |
| 0x2101 | bmas / 111 | A1NoC-SNOC master 平均 B/s |
| 0x2102 | bslv / 24 | SNOC-BIMC slave 平均 B/s |
| 0x2103 | bmas / 3 | SNOC-BIMC master 平均 B/s |
| 0x2104 | bslv / 0 | EBI slave 平均 B/s |

AP-owned USB master 不重复发送远端带宽；五个远端节点用 64 位 `bw` KVP。
平均带宽与交接基线相加，时钟投票取最大值。根频率按节点总平均带宽、USB 峰值、
总线宽度计算，BIMC 的平均带宽另使用 153% 系数及两个 channel；峰值不除 channel。
USB 默认向量在没有较高基线时请求 A1NoC/SNOC 50 MHz、BIMC 100 MHz。
已有固件流量加入总平均值，避免仅取 firmware/USB 两个频率最大值造成低投票。
频率向上取整到 kHz；它表示已确认的最低请求，不能当成实际测频结果。

RPM provider 的配置必须来自实际平台交接，不能复制占位模板：

- `qcom,rpm-handoff-version`：little-endian UInt32，按既有通道来源选择1/2/3；USB统一冷交接使用3。
- `qcom,rpm-interconnect-handoff`：完整八组 32 字节记录，依次为
  `id:u32 / reserved=0:u32 / maximum:u64 / activeFloor:u64 / sleepFloor:u64`。
  时钟单位 Hz，带宽单位 B/s；NoC 退出后仍保留 floor。
- `qcom,rpm-clock-handoff`：完整 27 组 16 字节记录，依次为
  `index:u32 / activeWireFloor:u32 / sleepWireFloor:u32 / reserved=0:u32`。
  index 固定在 `RPMClockHandoff.h` 的 MSM8998 表中；rate key 的 wire 单位为 kHz，
  branch key 为 0/1，QDSS 为其 state 值。XO 的 active floor 必须为1，
  PCNoC active floor 至少为19200。三个 USB 根的 floor 必须与上述 Hz floor 完全一致。
- `qcom,rpm-clock-limit-handoff`：12组16字节记录，依次为
  `index:u32 / activeMax:u32 / sleepMax:u32 / reserved=0:u32`。
  它保存AP的 `max` 请求，单位与对应时钟主键一致。0释放该限制；XO仅接受0/1，
  QDSS仅接受0..4，频率限制保留完整u32。限制与主键在同一KVP报文中获得ACK。

时钟缩放是全局操作，不能只为 USB 打开。首次请求先逐个确认这 27 个资源的 active/
sleep 请求及12组限制，再按 sleep、active 顺序确认全局 scaling-enable，之后才允许 USB 投票。
这些基线归常驻 RPM 服务保留，不随 USB 租约释放。缺失完整配置时不提供 NoC 能力；
当前设备树尚未发布这些交接记录。明确拒绝 ACK 可在失败步骤重试，未知完成会隔离
相应投票/交接，不能通过重发降低投票来猜测硬件状态。

NoC Grant 只在全部根/带宽 ACK、实际 GCC gate 和 QoS 读回成功后成立。
Prepare/Validate/Source 检查授权；周期 Sample 也检查它，并消费 RPM 远端 close/reset，
使旧 nub 关闭准入并停止。停止证明失败或任何释放状态未知都保留租约。
该服务和 RPM 绑定已通过 arm64 构建、共享模型及 sanitizer 验证，尚未在原生 IOKit
内核运行。系统 PM、父总线复位和显式隔离恢复仍待整合。

### xHCI 端口后端

端口通过扩展能力链识别 USB2 / USB3，而非假设物理端口序号。
解析限制在 xHCI 的 `0xc100` 字节窗口，拒绝范围覆盖、漏口、过短 capability、
损坏链和不支持的 BIOS 所有权交接；最多访问 64 个 capability。
支持标准速度 ID 及显式 PSI 的 1.5 / 12 / 480 Mbit/s、5 Gbit/s 对称链路，
保留硬件 slot type。未知速度、非对称链路和更高速率不会冒充已支持的 SDM835 功能。

PORTSC 写入只保留 RWS 字段。PED、变化位、复位 strobe 和保留位不从读值盲目回写；
清除旧复位完成位后必须读回确认，后续复位等待新的 PRC/WRC。USB2 常规复位和 USB3
warm reset 都有超时及 10 ms 恢复期。USB2 resume 保持至少 20 ms 信号，再等待 U0 和
10 ms 恢复；USB3 直接请求 U0。所有等待由 advance/timer 推进，持锁期间不 sleep。

逻辑端口 PP 与 PMIC Source 是不同资源；端口后端不打开外部 VBUS，也不释放任何
slot、transfer ring 或 DMA。过流期间仍允许请求 PP 断电；无 PPC 能力时拒绝假装断电。
时间倒退、断连、失效 MMIO、未完成清除和超时均有检查。故障不会自动重新开放端口。
这些硬件模板已通过 arm64 编译与模型验证，尚未绑定原生端口实例及角色准入。
寄存器语义参考固定版本的
[xHCI hub 实现](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/xhci-hub.c)
及 [扩展能力定义](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/xhci-ext-caps.h)。

### Host context 与命令所有权

Input context 按 33 个硬件 context、device output 按 32 个处理，支持 32/64 字节 stride。
slot 参数包含 route、真实 Supported Protocol speed ID、root port、hub/MTT 和 TT 路由。
端点编码校验不同速度的 packet/interval、HS 多事务、SS companion burst/mult 与 ESIT；
Control/OUT/IN 使用对应 DCI。流能力不会隐式启用 streams。Context 参数无效时不写内存，
输入在命令发布后冻结；只在匹配完成或实际 reset 隔离后才重新开放。
输出 view 只读并使用 DMA read barrier；它的 endpoint state 不作为释放 DMA 的停止证明。

CommandQueue 使用既有 producer ring，支持 Enable/Disable Slot、Address Device、
Configure/Evaluate Context、Reset/Stop Endpoint、Set Dequeue、Reset Device 与 No-op。
命令元数据与 owner 引用在 doorbell 前安装；完成必须匹配当前队首的 DMA 地址和 slot。
返回的 ticket 即使伴随 doorbell 错误也仍然有效，不能据错误结果直接释放 owner。
停止/中止、异常完成、准入关闭和超时保留所有未完成引用，实际 reset 证明后才允许确认清理。
确认返回的 owner 在 gate 外释放；确认成功只表示引用可清理，并不把失败命令变成成功命令。

InputContext 的 submit/finish 已与该队列直接相连，校验输入地址、owner 和 ticket，
包括 Address Device 的 slot/EP0 add flags。模型覆盖 5000 轮命令环回绕、迟延确认造成的
背压、即时完成、错误 slot/地址、超时、doorbell 失败及输入 context 隔离恢复。
这些后端和原生 ABI 声明已完成 arm64 编译；原生 HCD 尚未接入真实设备创建、DCBAA/slot
寿命、请求完成和取消，不能发布 Host 控制器。寄存器与 context 格式参考固定版本
[xHCI context 实现](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/xhci-mem.c)
及 [命令环实现](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/xhci-ring.c)。

### 显式直接 DMA 地址窗口

`USBDirectDMA.h` 已接入 `QcomUSBResources`。引导层可在控制器 AppleARMIODevice 上
提供 `qcom,usb-dma-window`，恰好 32 字节，均为 little-endian：

| 偏移 | 类型 | 含义 |
| --- | --- | --- |
| 0 | UInt32 | 版本，必须为 1 |
| 4 | UInt32 | 长度，必须为 32 |
| 8 | UInt64 | USB 总线地址起点，当前必须为 0 |
| 16 | UInt64 | 系统物理地址起点，当前必须为 0 |
| 24 | UInt64 | 窗口长度，必须等于 `1 << addressBits`，位宽来自 controller 配置 |

单独提供这份数据时，它仍是可信引导层对地址拓扑的交接契约。QcomUSB1.2 的
`qcom,usb-dma-capture` 路径可以在真实租约与隔离证明下，从实际 SMMU BYPASS
路由生成该属性，并保留独立的64字节观察及只读映射。驱动拒绝非零偏移、子窗口、
孔洞及并存的 `iommu-parent`，避免分配器产生
窗口之外的物理地址。缺少两种模式的任一种完整描述时不会默认使用直接 DMA。

Acquire 必须取得实际 NoC 带宽授权、读回 GCC BCR18 已断言，并再次核对地址描述。
成功后才允许解除复位并返回 DMABypass 契约。每次使用资源时重新核对窗口和 NoC；
属性删除/变化、mapper 冲突或 NoC 授权失效都会锁住后续访问。恢复描述不能直接重新开放
旧授权；必须停止 DMA、读回复位并释放旧授权后重新 Acquire。失败的 Release 保留相关资源。

实际内存仍通过 Apple `IOBufferMemoryDescriptor` / `IODMACommand` 管理。物理窗口分配
使用物理连续、Normal NC 缓存属性及设备地址位宽的 physical mask；命令使用
`kUnmapped` / OutputHost64，逐段核对实际地址，避免系统 mapper 等待及隐藏宽度 bounce。
对固定 iOS18 映像的只读反汇编已确认 `setSpecification` 中 mapping type 3 会跳过系统
mapper 路径；该证据不证明 SDM835 的地址拓扑或缓存属性。

主线 MSM8998 USB 节点及固定厂商 commit 的 USB 节点均未给 USB 指定 SMMU stream。
厂商 [USB glue 驱动](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/3fb35ef1f0fe1f714be3df39724cbc8eb217181e/drivers/usb/dwc3/dwc3-msm.c)
只设置 DMA mask，没有建立 IOMMU domain；
[SMMU 节点](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/3fb35ef1f0fe1f714be3df39724cbc8eb217181e/arch/arm/boot/dts/qcom/msm-arm-smmu-8998.dtsi)
还使用 skip-init 保留固件配置。这些证据支持保留显式直接 DMA 的软件选项，不能独立证明当前固件旁路。
ANOC1 PCIe 的 0x1480、USB 总线 master ID 和 QoS port 都不作为 USB SID 使用。

### SMMU 与原生 IOMapper

当前 USB DT 没有 SMMU stream ID，不能仅凭缺少 `iommus` 推断硬件旁路，
也不能为 USB 猜一个 SID。`anoc1_smmu` 的已知 aperture 为
`0x01680000 / 0x10000`；该资源的存在本身也不证明 USB 的实际 DMA 路由。
mapper 绑定消费明确交接的上下文与 stream，保持旧映射直到旧 DMA 停止后才换根。
原生 mapper、已有活动 context 采集及精确 BYPASS 观察路径均已接入；所需的实际
安装表、停机和租约检查见本章前部。完整板级资源策略与空 context 路径仍在补齐。

`QcomSMMU/PageTables.h` 实现 ARM64 stage-1、4 KiB granule、32..40 位 IOVA/PA，
按 IOVA 宽度从 L0 或 L1 开始；没有借用 CPU 的 16 KiB 页大小。非连续物理页映射
为有限 4 KiB 页，包含 AF、nG、只读权限及 execute-never。MAIR index0 要求为
Normal-NC，shareability 为 outer；上下文配置尚待原生服务安装。stage-1 无法表达
write-only，该请求按硬件模型使用 RW，read-only 仍受 AP 位保护。
现有映射不能被原地覆盖；无效范围和重叠在写入前被拒绝。分配或页来源失败时回滚
已写页表，父表先断开子表，再经过完整失效同步才释放子表。

`ContextTLB.h` 根据 ID0/1/2 验证 stage-1、4 KiB 支持和 context aperture，
只操作被交接上下文的 TLBIASID/TLBSYNC，支持包括0在内的16位 ASID。
它不复位整个 SMMU，也不借用其他 SMR 做固件 quirk 探测。同步前核对实际 TTBR0 的 ASID；对超时、无效读回和身份变化保持
故障，保留表页及客户映射。页表修改后的 NotReady/Busy/Denied 也归为未知完成，
不能作为可释放缓冲区的普通映射拒绝。MSM8998 的 LPASS 13-bank 报告按已知勘误排除最后一 bank。
`IOTableMemory.h` 使用原生 IOBufferMemoryDescriptor 分配 uncached、物理连续且
不经过另一 mapper 的 4 KiB 表页。销毁要求 root 已从硬件卸载并完成同步，
单个 USB 请求完成不足以释放整棵页表。实际缓存属性和内核生命周期尚未运行验证。

`HardwareContext.h` 已实现实际寄存器交接顺序。接口要求真实 fabric/clock 租约和
控制器 reset 观察；只有注册了 context fault 中断的原生 owner 才可启用 context。
配置读取硬件 ID、输入/输出位宽、context 和 SMR 数量；检查每个被交接 SID 的精确
匹配，拒绝未拥有的范围覆盖或其他有效 stream 共用目标 bank。现有活动 stage-1
bank 的 ASID 必须与交接一致；中断事件源和实际 USB SID 仍待服务层接入。

安装时先把已拥有 stream 的 S2CR 写为 FAULT 并读回，再停 context、终止遗留的
stalled fault、失效旧 ASID，之后配置 AArch64 CBAR/CBA2R、TCR、TTBR 和 MAIR，
同步后才启用 context 和 TRANSLATE 路由。Qualcomm 某些固件会忽略 FAULT 写入、
将 BYPASS 写入转换为 FAULT；实现仅在已拥有且 controller reset 的条目中处理这个差异，
每次都以实际 FAULT 读回为准。该行为来源于固定版本的
[Qualcomm SMMU 实现](https://github.com/torvalds/linux/blob/v6.12/drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c)。

这是一条 HLOS 接管路径：退出时不恢复已交出的固件映射。只有新的 reset 证明、
全部已拥有 stream 的 FAULT 读回、context 关闭、相关 ASID 同步和 TTBR 清零都成立，
才报告 root 已卸载。其余 bank、stream 和全局配置不写入。部分失败仍持有硬件及表页，
可在资源和 reset 证明恢复后重试卸载；所有权被其他有效 stream 占用时拒绝清理。
64 位 TTBR 访问使用有边界和对齐检查的单次 arm64 MMIO，不拆成可见的两次写入。

`MappingLedger.h` 为有限映射分配带前后保护页的 IOVA，检查地址位宽、页内偏移、
对齐、固定地址冲突和内存/命令身份。未知完成进入隔离状态，不能取消或复用。
`MappingEngine.h` 已把它接到同一份页表和硬件 context：成功 map 后才登记可用映射，
成功 unmap/TLBSYNC 后才释放实际 descriptor/command 的保留引用。页表失败但回滚
完整时取消新租约；未知结果保留引用和地址，并关闭新映射。

已消费的失败 Native complete 不能再次假装 unmap。必须先由硬件 owner 卸载 root，
再将映射标为已隔离，最后由原身份确认回收；期间仍不复用 IOVA。集成模型覆盖真实
寄存器后端、页表、引用、保护地址和下一轮租约。原生 IOMapper 服务、UPL/page-list
来源、fault IRQ/通知、翻译模式的平台 DMA 租约与原生命令恢复 RPC 尚未连接，当前不能提供
SMMU 翻译的 USB DMA 资源。这些后端没有作为可匹配的 SMMU Kext 发布。

[原生 DMA 审计](iomapper-abi-audit.json) 与
`drivers/abi-22H374/apple-iomapper-abi.json` 固定当前 iOS18 映像的 IOMapper
208 字节/174 槽、IODMACommand 120 字节/31 槽，以及已消费生命周期方法的绑定。
编译探针同时核对 DMA map specification 的 40 字节和 page list 的 16 字节布局。
Native Map 回调参数遵循 SDK，目前只完成 ABI 编译检查，尚无完整原生 map/unmap
集成证明。该探针不能加载、matching 或访问硬件。

PHY 服务使用同一 Kext 内的独立 IOKit personality，匹配
`qcom,msm8998-qusb2-phy` / `qcom,msm8998-qmp-usb3-phy`。其资源定义如下：

| 类型 | index0 | index1 | qcom,usb-phy-supplies |
| --- | --- | --- | --- |
| QUSB2 | `0x0c012000 / 0x2a8` | TCSR 数字钳位 `0x01fcb24c / 4` | 三组或四组 RPM rail ID/uV，四组包含 vdda12 |
| QMP USB3 | `0x0c010000 / 0x1000` | TCSR clamp 字 `0x01fcb244 / 4` | 两组 RPM rail ID/uV：vdda-phy、vdda-pll |

supplies 是 little-endian UInt32 二元组；具体 rail ID、电压与板级供电连接一致，由
现有 RPM 服务的电压范围及所有权再次检查。USB2 的
`qcom,usb-phy-differential-ref` 为可选 UInt32 0/1，默认使用此 SoC 的单端参考方案。
USB2 可选 index2 为 QFPROM `0x00784238 / 4`；未提供时不做 OTP 覆盖。板级模拟
校准使用 `qcom,usb-phy-device-init` 和 `qcom,usb-phy-host-init`，每项为小端 offset/value
二元组，最多16项，只接受已知调校寄存器。PHY 请求升级为 version2，32字节大小不变，
最后一项为数据角色；切换角色必须先停止 DMA，再 Disable/Enable。USB2 钳位在供电前
解除、确认复位后且撤销时钟/电压前重新钳位。首次 PHY 专用时钟接管读取真实 core/PHY
BCR；共享 AHB2PHY 和振荡器引用保留固件固定状态。
板级节点尚未在当前启动设备树中发布；这里定义的是驱动实际消费的资源，不是就绪标志。
GCC 必须实际允许 PHY 时钟/复位请求：cfg AHB=121、USB2 differential ref=170、
USB3 aux=119、ref=166、pipe=120；USB2 reset=106，USB3 reset=70/69。
USB3 aux 根165按 1.2 MHz 配置，pipe 的 GCC 报告频率必须为 125 MHz。

QUSB2 从 QFPROM 的 `0x23a` 字节低四位取得 HSTX trim，只覆盖 Tune1 高四位；
零值使用 SoC 默认值，trim15 是合法 OTP 数据。USB3 使用 Linux v6.12
`phy-qcom-qmp-usbc.c` 的 **MSM8998** 数据，两组 TX/RX 均校准，PCS_MISC 选择
Type-C 正反方向。普通 qmp-usb 或 Combo 的其他 SoC 参数不能代用。
这些校准数据单独保留 GPL-2.0-only 标识，驱动控制流程与 Linux 框架分离。

内核调用者先独占 open 服务，再调用 `USBPhyRequest` Enable/Check/Disable，协议为
32 字节，param2 是拥有者 IOService。启停、变更方向之前必须停止 controller DMA；
PHY 本身没有权限排空 DWC3/xHCI 请求。Enable 会真实提交 RPM 电压投票和 GCC
时钟/复位请求，独立 consumer7/8 确保一个 PHY 退出不撤销另一个的共享轨投票。
RPM 的成功回复是已确认投票，不是电压实测值。原生平台函数和可能阻塞的资源操作
只在可睡眠调用线程中执行，不能在 USB 工作循环 gate 或 IRQ 中发起。
Enable 失败若返回非零 lease，调用者必须保留 open 和对象，继续 Disable 清理，
不能因返回错误就释放拥有者。PLL 失败可在复位证明后撤销已知资源；复位失败保留
全部持有。未知时钟状态或 RPM ACK 超时会保留资源，不能把重新发送零电压视为恢复。
测试同时使用真实的共享 `VoltageVotes` 策略验证该隔离行为。

`QcomUSBConnector` 位于已有 `QcomPMIC.kext` 中，使用
`IOProviderClass=QcomPMIC`、`IOMatchCategory=USBConnector` 和
`PMICUSBConnectorAvailable=true` 匹配，与按钮客户端共存。它发布
`USBConnectorResources`，不会冒充完整的 `USBPlatformResources`。SPMI provider 的
`qcom,usb-connector` 为三个小端 32 位 cell：PMI8998 SID、RRADC 外设基址、
source 电流预算（uA，0 或 500000..1500000，250000 步长）。0 为仅 Sink；非零
预算允许硬件完成初始化后进入 DRP。1.5 A 预算广告 1.5 A Rp，其余预算广告默认 Rp。
`USBSourceCurrentMicroamps` 暴露限流预算，完整平台提供方和 Host 适配层仍需消费该
上限，约束端口功率与可用速率。`USBSourceAuthorized` 反映当前租约的源端准备状态；
真正开启仍必须携带实时 Host/Source 路由，并通过外部 VBUS、过流与输出电压检查。
`qcom,usb-connector` 也可提供第四个 UInt32 cell：0 禁用 VCONN，1 表示板级接入
SMB2 VCONN 开关。三 cell 旧格式保持 VCONN 关闭。启用后支持 Rd/Ra 供电线缆，
Normal 向 CC2、Reverse 向 CC1 供电；方向在新连接代次中重新核对。断开、角色变化、
观察失效、过流、暂停和 Release 都关断 VCONN；关断失败仍尝试停止 VBUS，并保留租约。
同方向的供电线缆变为普通线缆也推进连接代次。调试、音频附件和 PD 协商尚未实现。
该配置及下列 IRQ 尚未写入板级 DT；不从某一机型名称猜测 SID/接线。

`qcom,owned-irqs` 必须包含同 SID 的 USBIN(0x13) bit7/4/3 和 OTG(0x11)
bit0/1/2，使用明确的 edge trigger。PMIC 对 USBIN/OTG/DCIN/RRADC 实施客户端
独占访问，charger MISC 只允许 0x1652、0x16e9 及解锁字，不能访问其看门狗等
无关寄存器；读写仍受真实 SPMI APID 写所有权限制。仅匹配/发布服务不做硬件写入；
Acquire 确认 PMI8998 subtype、Type-C 模式和原有 source 未开启后才配置软件 VBUS
控制并注册 IRQ。源端 VBUS 自身变化不推进连接代次：Type-C 子中断只开 CC 状态
变化，plugin 中断仅对已观察到的 Sink 会话计数。IRQ 路径只更新短锁保护的计数并
调度通知，SPMI 操作在可睡眠线程进行；20 ms 采样补充状态检查。硬件已经合并为
一个 latch 的多个边沿无法逐一恢复，但一次 CC 通知即使身份相同仍会使旧连接失效。

`PMICTypeC.h` 按固定版本的 Qualcomm 下游源码初始化 CC FSM、关闭 factory/
并发/调试供电、设置 Rp 和 Try.SNK、保留硬件 VBUS<1V 互锁、配置 PBS workaround，
并将 VCONN 设为软件关闭。修改模式前让 FSM 空闲 200 ms。供电动作的 RRADC
新转换有 50 ms 上限，避免测量无期限延迟供电。寄存器写入都检查读回值。
版本识别使用实际 REV4；PMI8998 v2.0 的 CC2 Sink 丢失 VBUS 时进入专用恢复：
保存五组掩码字段，依次执行 10 ms 双角色、30 ms Sink 检测，直到 debounce 和
TIMER_STAGE_2 同时消失。阶段等待使用状态机和时间戳，采样线程不在等待中睡眠。
VBUS 恢复会取消检测，所有退出均恢复保存的字段并要求新采样。仅屏蔽该恢复产生的
CC 通知风暴，plugin/故障计数仍保留；source-off 不取消尚在进行的 CC2 检测。
任何部分写失败均保留恢复记录，未知状态禁止新供电，Release 可明确重试。

过流/过压 IRQ 使用独立非回绕故障计数，实时位已落下也不会丢失故障。锁存只在
已证明 source-off、无实时故障且连接实际断开后清除；同一根线释放再获取租约不能
绕过锁存。采样发现新代次、翻转、角色变化或观察失效时立即关断旧升压，随后才
返回新路由。升压启动中的过流在关断证明后返回 Aborted，使上层等待拔线；关断
不能证明则保留输入隔离和资源，不再接受新的供电。

连接器通过 IOKit PMinit/joinPMtree/registerPowerDriver 参与 PMIC 电源树。PMIC
仅在活动态向子服务导出电源，防止活动连接器的 IRQ 域被提前暂停。连接器暂停先
撤销通知准入、关断升压并强制 Sink，保留租约；已证明停止后可在父总线暂停期间
继续接收幂等 source-off。唤醒重新初始化 CC，并推进下次有效采样的代次，旧快照
不能恢复供电。控制器冷挂起已排空连接器和 mapper 生命周期；原生系统 PM 的实际执行
尚未验证，连接器 `SystemSuspendSupported` 保持 false，能力声明不等同于真机验证。

连接器接口是 version1、48 字节 `USBConnectorRequest`，与上层平台 version3
协议分开。必须先 open，再获取绑定拥有者的非零租约；失败的 Acquire 也可能保留
lease，调用者必须完成 Release。24 字节通知的计数0会立即撤销准入，后续 Sample
才提供可信计数。通知在独立 thread_call 中发出，不持 SPMI gate 或连接器睡眠锁；
Release 撤销通知目标并排空回调。回调中重入 Release 返回 Busy，需要调用者稍后重试。
观察暂时失效后恢复、Sink 会话丢失、多个 IRQ 合并都不会重用旧代次。

`PMICUSB.h` 已实现并经故障注入覆盖的供电寄存器流程包含：RRADC 清日志并强制新
USBIN 电压转换、双读一致性检查、vSafe0V 检查、保留原输入 suspend、设定 OTG
限流/慢启动/热保护、secure access 解锁、升压完成及输出电压验证。每一步之后的
新连接会取消旧快照。关闭 source 只撤销自身升压，不主动放电外部 VBUS；未证明
boost 停止就不恢复输入充电路径；工程位只恢复本租约保存的原值，设备模式不会
清除固件原有工程配置。丢失写 ACK 或清理失败保留 lease/对象，允许明确
重试。条件供电路径已编入 Kext，但板级节点、完整资源桥接和内核运行均尚未完成，
不能把模型验证视为实际端口已供电。

平台 Kext 使用 Apple 的 `IOResourceMatch=USBPlatformResources` 等待资源服务就绪，
然后向该 IOService 的 `callPlatformFunction("USBPlatformRequest")` 取得实际资源租约。
提供方通过 `IOService::publishResource` 发布自身，调用者的 provider 是目标
AppleARMIODevice。version3 的 72 字节请求和 24 字节连接器通知定义在 `USBPlatform.h`；Acquire
必须真实保留资源，并返回非零、绑定调用者的 lease。后续 Sample/Source/SafePower/
Release 必须保留请求头和 lease，不能将成功回复归给另一代租约。提供方需要保留
通知接收者，并在 Release 时排空通知。连接器通知只调度线程重新读取硬件；10 ms
采样作为补充，PMIC 自身仍需独立处理过流断电。

连接器快照为 16 字节，包含非回绕 connection 计数、Normal/Reverse 方向以及独立的
Source/Sink 供电角色。生产者必须记录 Sample 之间合并掉的重插/方向/角色变化；
Sink 的外部会话丢失也要推进代次，软件控制自身供电产生的预期 VBUS 变化不能伪造
新的连接。未附着快照的方向/角色必须为空，计数0只表示初始/未知观察。
通知携带最新计数；较新计数或未知观察通知会立即关闭旧 nub 的准入 stamp，随后由
可睡眠角色 worker 停止 DMA、同步排空/终止旧 nub。即使观察故障很快恢复到同一连接，
旧 nub 的 stamp 也不会重新有效，worker 会重新发布控制器。

`SafePower(next)`、`Prepare`、`Source(true)`、`Validate` 携带原始目标快照，提供方
需要用实时采样核对路由。`platformTargetCurrent` 检查代次、方向、角色、Sink 的 VBUS
会话和过流状态；实际电压、外部电源来源及硬件供电互锁仍需由资源服务独立验证。
输入字段/lease 必须在回复中保留。Prepare 只在旧 controller 已停止、
回调排空后重配 PHY，Validate 在发布 nub 前再次检查而不改变硬件。快照过期返回
`kIOReturnAborted`，角色 worker 移除供电、再次证明停止，再重新采样防抖；这类
正常热插拔竞争不会直接成为永久故障。停止或移除供电失败仍保留资源并关闭准入。
Source(false) 不依赖一个有效连接快照；临时观察故障不能阻止关断自身供电。
旧版56字节协议不再被接受；这些操作已接入聚合服务，完整硬件闭环仍待完成。

**当前聚合服务、NoC 租约、原生 IOMapper、活动 context、精确 BYPASS 与独占禁用 context 旁路生产路径已有实现；板级策略及空 context 路径仍缺**。
缺少有效服务或 Acquire 失败时拒绝启动。板级节点需要把两个 MMIO 资源按
core / QSCRATCH 排列，只暴露核心 SPI131 的一个中断 source；
不是直接套用 Linux wrapper 的三个 PHY/电源 IRQ。

角色 nub 使用平台实际 IOMapper 对象设置自己的 `iommu-parent`，因为当前 XNU 的
`IOMapper::copyMapperForDevice` 只读调用对象本身的属性，不遍历 provider 祖先。
无 mapper 的路径使用下述显式窗口及资源授权。角色关闭和供电操作不会释放隔离
失败的 DMA 所有者；显式故障恢复仍待控制器适配层和平台提供方共同接入。
翻译模式的 Acquire 必须先解析并保留所需的设备 IOMapper，在 AppleARMIODevice 上安装真实
mapper 对象；驱动拒绝未解析的 ID，以免在 IOMapper 的无期限匹配等待中挂住启动。
交给角色 nub 的 DMA 地址位宽取平台与硬件能力的较小值。

硬件资料固定为 Linux v6.12 及清单中固定 commit 的 Qualcomm 下游 SMB2 源码，
保存在 `reference/`，版本、URL 和哈希在 `sources.json` 中。
reference 目录中的 Linux 文件不参与编译；QMP 校准数值单独放入带 GPL 标识的数据表，
硬件访问、资源生命周期和状态机在驱动中实现。
Apple 的开源 Kext 组织形式参考 [IOUSBFamily](https://github.com/apple-oss-distributions/IOUSBFamily)。
硬件资料入口为 [MSM8998 DT](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/boot/dts/qcom/msm8998.dtsi)
和 [DWC3 寄存器定义](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/dwc3/core.h)。
PHY 的 SoC 参数核对 [QUSB2](https://github.com/torvalds/linux/blob/v6.12/drivers/phy/qualcomm/phy-qcom-qusb2.c)
与 [QMP USBC](https://github.com/torvalds/linux/blob/v6.12/drivers/phy/qualcomm/phy-qcom-qmp-usbc.c)。
Rp/PBS 流程参考 [Qualcomm SMB2 Type-C 控制](https://android.googlesource.com/kernel/msm.git/+/e6a9e729094cbc8f6f81ee0839270954ea4d3c3e/drivers/power/supply/qcom/smb-lib.c)，
CC2 流程与芯片版本定义见清单固定 commit 的 `smb-lib.c` 和 `qpnp-revid.h`。

## 构建验证与剩余工作

统一入口：

```sh
make usb-driver
make test-usb
make test-usb-sanitize
make verify-usb-driver
make test-usb-tools
make usb-bridge
```

`make test-usb` 先构建并校验八个本地 Kext 与原生导入（QcomGCC、QcomRPM、
GenericClockController、QcomPowerController、QcomSPMI、QcomPMIC、QcomInterconnect、QcomUSB），再使用 `scripts/toolchain.py` 和 `Build.json`
执行 GCC / USB / xHCI / DWC3 TD / EP0 / Device ISO / DMA / 请求所有权 / PHY / OTG 路由 / PMI8998 连接器 / controller 资源 / NoC / SMMU / context 与 IOVA
主机模型测试，并用真实 XNU SDK、arm64 编译器、Apple Kext 参数实例化全部硬件
模板和真实 IOKit DMA 绑定，编译 Device Family 探针并检查其 235 / 19 槽虚表，以及 269 / 235 槽 Host 控制器/端口探针，
重新分析并校验原生 Device ABI。探针新增的原生类导入必须存在于已审计契约中，
参数类型改变导致的 C++ 符号变化也会使检查失败。
编译探针目标文件位于 `build/tests/kernel/usb/`，不是 Kext。平台 Kext 位于
`build/usb/Extensions/QcomUSB.kext`，报告为 `build/usb/verification-drivers.json`；
平台、nub、PHY 和聚合服务类各有 168 个虚表槽；NoC 类为 168 槽，
GCC 检查 169 槽、RPM 检查 170 槽及 execute/signal 绑定，Device 适配类检查 235 槽及实际 ioGated 绑定。
产物已链接原生 Host/Device Family，并带按角色匹配的两个控制器 personalities。
构建工具共用 SDK 初始化、编译参数和语言参数，没有新增 USB 构建脚本。
`make test-usb-sanitize` 使用同一份 USB_TESTS 列表，经统一工具链开启 ASan/UBSan。
两个统一测试目标随后串行运行桥接45项与工具链14项回归，避免与Native SDK输出并发。
ABI 分析使用独立临时输出，解析成功后原子发布报告，避免并发构建互相截断 JSON；
临时目录在成功或失败后都会清理。

测试涵盖 DMA 越界、错误 IP、停止/复位超时、端点命令冲突、EndTransfer 异步完成、
事件环回绕、错误事件计数、热插拔、防抖、过流、启动失败和旧代次回调拒绝；
xHCI 另有 5 万轮随机队列/完成/回绕及 1.6 万个事件的所有权测试。端口测试覆盖 USB2/USB3
顺序互换、定制 PSI、能力边界、W1C 保留、旧完成拒绝、实际恢复时长、断连及过流断电。
统一入口现包含17组模型，新增共用 RPM 资源交接的电源测试；普通与
AddressSanitizer / UndefinedBehaviorSanitizer 使用同一选择列表。
请求测试还覆盖映射期间 SETUP 抢占、端点顺序、异步取消后延迟清理、回调上下文分离、
DMA 隔离、清理失败重试、请求槽复用、超时及非 2 的幂次中断包长。
PHY 测试覆盖资源服务缺失后重试、每一步资源获取失败、QUSB2 fuse trim、USB3
两组通道和正反方向、PLL 超时、失效 MMIO、复位失败、部分释放重试、共享轨投票，
以及真实 VoltageVotes 的 ACK 未知隔离策略。
OTG 路由测试覆盖同角色快速重插、Type-C 翻转、独立供电角色、实时快照失效、每个
切换阶段的取消回滚、通知先于采样、旧通知/旧采样、同连接瞬时观察故障后的重新发布，
以及关断供电失败时仍尝试停止 DMA。该测试使用与内核平台层相同的状态机和准入代码。
连接器测试覆盖不同 CC 模式、快速重插、翻转、会话丢失、SPMI 观察失败、外部低压
VBUS、ADC/升压超时、Rp 预算、CC2 两个等待阶段/取消/恢复、瞬时故障锁存、暂停/
恢复/旧快照拒绝，以及初始化、供电、关断、CC2 和 PM 各阶段前后丢失 ACK 的恢复。
controller 资源测试覆盖冷热交接、重复获取后的实际 BCR 观察、固件 pin 接管、
速率/复位顺序、失败与未知 ACK 的资源保留；GCC 回归直接检查真实接管实现和共享引用。
NoC 测试覆盖全局时钟交接的 56 个步骤、独立 active/sleep 投票、64 位累加/溢出、
现有流量对根频率的影响、QoS 写入顺序/恢复，以及获取/释放各阶段前后丢失 ACK。
SMMU 测试覆盖跨 L0/L1/L2 边界、SG 物理页、权限、分配/来源失败回滚、表页回收、
2000 次稀疏映射重用、实际 ASID 检查和 TLB 超时保留。context 模型另覆盖两种
FAULT 写入行为、未配置条目、共享 bank/SID 冲突、每步写入后的未知读回、卸载重试，
以及从映射失败到实际 root 卸载、身份确认、引用释放与下一轮映射的完整后端链路。
共享 DMA 命令状态另外验证
已消耗的失败完成不能重试，也不能释放命令或缓冲区。
GLINK 回归另验证 64 位 KVP、216 字节混合报文上限、畸形 KVP 的零发送、畸形回复
导致的未知完成，以及无新写入时的远端关闭观察。
IOKit DMA 绑定当前只完成 arm64 编译检查，尚未在内核中执行；故障注入使用同一份
缓冲区生命周期代码和离线 API，不能代替真实 IOMapper 验证。
本阶段的源码哈希、命令和产物边界见 [离线验证记录](validation.json)。

距离原始目标仍缺以下部分，目标保持进行中：

1. 完成可信引导资源生产端。已接入原生 IOMapper 的 UPL/page-list、context/stream、
   页表、IOVA、fault IRQ 和命令退休证明。实际安装的 IORT、进入 XNU 前的 halt/BCR、
   活动 context/ASID/VMID、精确 BYPASS 及独占禁用 stage1/S2-bypass 观察已关联；
   仍需生产 NoC/RPM/连接器/PHY 的板级策略，并补齐无可继承 context 或共享
   bank/命名空间的所有权路径。
   公开源码和参考 IORT 不授予运行时权限。

   `--usb-topology-component '\_SB_.USB0'` 通过现有桥接构建入口预留空记录，EFI 代码
   校验 RSDP/XSDT/IORT 和 ACPI 内存范围后填入，并在退出 EFI 后复查。
   控制器 aperture 还必须匹配实际 USB Config revision0x10003 的 primary-core 查询；
   枚举有界，拒绝冲突实例，允许旧句柄指向同一对象。所有固件接口查询都在 EFI
   退出前完成，退出回调可卸载协议并释放内部 core，因此退出后只复查 ACPI 和副本。
   这些身份绑定不授予 DMA 或时钟所有权。
   `--usb-rpm-snapshot` 已补实际 NPA 缓存的映像绑定、观察与退出复核；仍需在真实
   控制器停止和通道接管后，以成功 ACK 建立资源基线，不能直接消费缓存为所有权。
2. 完成资源交接后，再验证原生 PM、配置器、接口、工作线程与回调的
   内核执行生命周期。控制器 personalities 已有源码/产物检查，但尚未在内核执行。

通用 Device 的 SS BOS、U1/U2/SET_SEL 硬件回调及 SS/HS/FS 回落已接入源码，
仍仅有离线模型和 arm64 构建验证。固定版本的固件事件关闭、StopController、旧句柄
别名清理和协议撤下已接入源码；其退出记录区分软件停止与真实硬件/资源所有权。
成功返回不能替代寄存器停机、资源保留和 DMA 交接证明。
Clock/NPA/GLINK 的固定版本也已核对：Clock 协议使用共享 DAL 客户端；NPA 的
active state 与 request state 不是同一字段，退出时调用 GLINK Close 并卸载协议。
Close 返回不是 RPM 请求全部得到 ACK 的证明。已有每资源 RPM 请求缓存的布局和
发送路径证据；资源交接还需验证实际加载的固件、最终请求及完成状态。

上述驱动工作完成后再考虑真机测试。本阶段只生成离线构建产物，没有刷写或操作 USB 真机。

`make usb-kernelcache` 和 `make verify-usb` 已完成 53 个 Kext 的闭包与独立结构验证。
AppleSMC 的 `OSBundleLibraries_arm64` 和 `OSBundleCompatibleVersion_arm64` 应覆盖默认键；
此前报告的 IOACPIFamily/watchdog 缺失来自错误选择默认字典。解析器现遵循 XNU
`OSKext::getPropertyForHostArch`，保留全部默认/架构字典，并检查它们在序列化后未改变。
同时补齐同页 ADRP 定义溯源、GOT 尾调用、真实 stack guard 终止路径及继承内核方法的映射。
未解析重定位仍禁止输出。详细证据见 [提供者闭包](native-provider-closure.json)。

EDK2 的原始 IORT 描述 `USB0 -> SID0x1400 -> ANOC1 SMMU0x1680000`，其原始 AML
校验和无效，不能直接传给驱动。测试只模拟安装器修正校验和以核对格式；生产代码只接受
实际已安装的有效表，不修改输入校验和，也不预设 SID、共享上下文或全局旁路。
此处尚未执行固件表采集、原生 Family 工作线程或 USB 硬件。

RPM 1.2.0 还可消费完整的 `qcom,rpm-domain-handoff`（四条 `id/activeLevel/sleepLevel/reserved`，
每条 16 bytes）。声明时会先确认所有域的 active/sleep 基线，再转移时钟缩放和总线请求；
USB 的第一笔投票仍先做聚合校验，其消费者值不会被基线初始化覆盖。
GPU 可独立触发同一初始化路径，详见 [GPU 平台交接](../gpu/hardware-submission.md#rpm-初始基线和服务启动顺序)。

RPM 1.3.0 在同一 command gate 中维护共享 BIMC 最低时钟。USB 与 GPU 的 EBI 平均流量
共同参与 153% 利用率、双通道、8-byte 宽度的频率计算；各客户端峰值时钟请求继续取最大值。
先提时钟再提带宽、先降带宽再降时钟；未知完成会保留整组资源。GPU consumer 12 只能写
BIMC/EBI 两个共享资源，不操作 USB 专用节点。

RPM 1.6.0 将已声明的非零电源 floor 纳入完整交接顺序：先在独立 active/sleep
集合中取得 rail ACK，再取得四个 domain、27组 clock 和八个 NoC 资源的 ACK，
最后才开启全局时钟缩放。未声明或为0的 rail floor 不会生成关断请求，也不代表
已知关断状态。已确认的基线由 boot slot 保留，USB 消费者归还自己的投票不会
清除它；未知 ACK 会保留已确认资源并阻止继续交接，明确的拒绝则可从失败项重试。
原生服务和 GPU 引导共同使用 `RPMResourceHandoff.h`，不再各自维护这段顺序。

电源/域请求在开启交接前先验证参数、范围及所有权；domain 写入要求完整四域
策略，NoC 的各交接版本也都要求该策略。原生 Domain capability 只在策略完整时
公布；CX/MX 的 active floor 必须非零，与引导策略检查一致。初始化在消费者
投票事务之前完成，之后再计算包含基线的聚合值。入口、
交接末尾和电压/域查询都会检查实际 RPM 通道；缓存命中不能绕过关闭/复位检查。
统一电源模型使用真实 GLINK/RPM 报文解析和匹配 ACK，覆盖四路 PHY 电源、
域、clock、NoC 到全局缩放的88笔请求，逐笔注入拒绝与丢失 ACK，并检查部分
初始化重试、基线保留、零 floor 不发送、无完整策略时不继续握手，以及通道关闭。
这些 ACK 逻辑仍需完整、可信的板级策略输入；请求缓存不能自行提供这种授权。
