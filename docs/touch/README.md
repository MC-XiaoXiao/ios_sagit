# sagit 触摸屏与统一 I²C 总线

本轮检查发现，项目原有 `input` 集合覆盖侧键、GPIO、指纹 Home，但没有
I²C 控制器、触摸设备服务、Synaptics RMI4 协议或触摸 HID 报告。
现已补入 `GenericI2C`、`QcomI2C` 和 `SynapticsTouch`，接入统一构建、
`input`/`touch` 集合、设备树转换与离线测试。初始实现未进行真机测试；
后续启动接入见本文的运行时初始化说明，实际结果以对应启动记录为准。

2026-10-22 已通过原厂 IOHIDEventDriver / BackBoard 的真机页面测试：
语言点击可进入地区页，语言页二次 Home 菜单的 Cancel 可返回列表。
关键修复位于 QcomDisplay：内置 framebuffer 不再发布 `IOMFBUUID`，
面板交接 UUID 保留为 `QcomPanelUUID`。原因、候选与验证边界见
[iOS 18 主屏触摸投递修复](main-display-routing-fix-20261022.md)。
当前真机验证索引为 [validation-main-display-20261022.json](validation-main-display-20261022.json)。
Hello 控件未出现，信息按钮按用户要求跳过；没有将壁纸点击计为通过。

## Apple 框架与职责分工

```text
AppleARMIODevice（QUP5 MMIO）
  QcomI2C : GenericI2CController : IOService
    GenericI2CDevice（0x20，独占 open/close，TLMM125 中断）
      SynapticsTouch : 原生 IOHIDDevice
        原生 IOHIDInterface
          原生 IOHIDEventDriver
```

当前 iPad7c iOS 18 映像中，`AppleARMIICController` 为 264 bytes /
180 vtable slots，`AppleARMIICDevice` 为 160 bytes / 174 slots。其控制器与
设备的划分用于本实现的结构参考；私有传输参数和调用语义尚未完成恢复，
所以本轮没有宣称兼容这些类的二进制接口。以后接入原厂 IIC 客户端，需要
另行恢复调用契约、增加哈希与虚表约束，并将其请求适配到统一事务接口。
不应把当前本地 nub 的类型改名冒充 AppleARMIICDevice。

触摸输入直接使用已有 `apple-hid-abi.json` 约束的原生 IOHIDDevice，
207 个槽位的子类布局在构建时校验。HID personality 按 I2C transport、
`Mi 6 Touchscreen` product 和 Digitizer/TouchScreen（0x0d/0x04）匹配
IOHIDEventDriver，设置 `DigitizerCollectionDispatch` 和 `DisplayIntegrated`。
原生类结构及映像中属性字符串的位置见
[Apple 框架证据](apple-framework-evidence.json)。事件解析参考项目已固定的
[Apple IOHIDEventDriver 源码](../gpio/reference/IOHIDEventDriver.cpp)。

统一 I²C 层有实际复用接口：

* `GenericI2CController` 从 DT 子节点发布设备，拒绝重复和保留地址，
  在一个锁内串行执行完整事务，禁止在硬中断上下文发起同步总线操作。
* `GenericI2CDevice::transfer(client, messages, count, timeoutUS)` 要求客户端先
  独占打开设备。设备地址不可被请求改写；同一请求中的多条消息使用重复
  START，最后一条才发送 STOP。当前支持 7 位地址、最多 8 条消息、
  单条 256 字节、总计 512 字节及 1–100 ms 超时。
* `QcomI2C` 实现 MSM8998 QUP v2.2.1 的 tag v2、FIFO/block PIO、非整字尾部、
  NACK/总线错误与超时复位。只有整个事务成功后才把读取内容交给客户端。
  不提供 SMBus 特殊命令、10 位地址或 BAM DMA。
* 其他 I²C 传感器可以匹配自己的 GenericI2CDevice，复用消息接口；
  新控制器可实现 `GenericI2CController::execute`。当前硬件后端仅接受
  sagit QUP5 地址范围，不能仅修改 compatible 就宣称支持其他总线。

## 触控处理

RMI4 扫描 10 页 PDT，读取 F01 身份和状态，优先选择 F12，缺少 F12 时
支持 F11 的 5 字节绝对坐标格式。F12 根据 Query5/Query8 的存在位图确定
Control8/31、Control23、Control28 和 Data1；支持最多 10 指、手套指，
过滤掌触、悬停、笔与未知对象。固件升级模式、异常状态、不支持的布局、
越界坐标及传输失败均不会转换为正常触摸。

F01/F12 中断 mask 支持跨字节分布。CRC 等待有上限，完成后重新扫描 PDT。
F12 配置 8 字节对象格式，并保存原始配置以便失败回滚和正常停止时恢复。
芯片重置后先释放 HID 中已接受的手指，再重新发现功能表；回滚失败不会被
下一次初始化误当成新的原始配置。

HID 使用十个独立 Finger collection，一份 94 字节并行输入报告包含稳定的
contact ID、Tip/InRange/Confidence、归一化 XY、宽高、触点数量、
100 µs 单位的扫描时间。最大触点数通过 Feature report 提供。坐标范围来自
芯片；交换轴、翻转、0/90/180/270 度旋转由交接配置指定。显示旋转与触摸
旋转必须使用同一坐标约定，当前不在运行中自动跟随显示服务旋转。
RMI strength 仅作诊断，不声明 HID 压力轴；报告的宽高字段与描述符对齐。
抬起帧保留最后一次被原生 HID 接受的坐标，避免手指路径在结束时跳到原点。

IRQ、定时器、报告快照和 PM 操作使用同一 workloop/command gate。
该 workloop 在调用原生 HID 的 `start` 前创建，并保持到原生 `free` 完成，
避免原生报告锁提前缓存另一个循环或在停止阶段引用已释放的循环。
短暂 I²C 故障先重试，连续失败、固件重置和 PM 关闭会发送中性报告；
HID 拒收时保留释放请求，成功释放前不发送新的按下。按住时每 20 ms
补采样，空闲每秒检查一次；恢复最多尝试 10 次，失败后保持 offline，
下一次 PM 启用可以重新尝试。停止路径取消 IRQ/定时器、恢复寄存器并关闭设备。

IORegistry 提供 `RMIProductID`、`RMIFunction`、`RMIMaxContacts`、
`RMILastError`、`RMIRecoveryAttempts` 和 `HIDInputOffline` 诊断属性。
错误值采用 `RMI4::Result` 枚举；这些属性是运行状态，不代表验证结果。

## 板级资源与启用边界

小米参考树给出 QUP5 `0x0c179000/0x600`、I²C 地址 0x20、GPIO87/88
总线引脚、GPIO125 低电平触控中断、GPIO89 触控复位及 GPIO8 面板复位。
S3330、S3331 与 TD4322 有不同的固件/面板变体。参考文件及其 SHA-256
固定在 [reference/sources.json](reference/sources.json)。

设备树中的总线和触控节点默认禁用。启动桥新增
`--touch-handoff-profile <已完成资源交接的配置文件>`，在完整验证后原子启用，
将数值转成 Apple DT 小端格式，并记录 profile 哈希和
`hardware_verified: false`。该开关不会初始化原厂触控固件或自动准备供电。

[handoff-template.json](handoff-template.json) 故意把资源确认项设为 false，
直接使用会被拒绝。只有启动固件实际完成下面的交接，才能将对应项设为 true：

* 停止固件的 I²C/BAM 客户端及其中断，独占移交 QUP5 和 0x20 设备。
* 保持 BLSP1 AHB 和 QUP5 核心时钟，核心源为 19.2 MHz。
  驱动通过已有 QcomGCC 读取真实源频率，并持有 clock 37/46 引用；
  桥只追加这两个时钟的授权，不修改共享根时钟或 PLL。
* 保持 GPIO87/88 的 I²C 配置，以及触控电源、LAB/IBB、显示共享电源和
  正确的复位释放状态。GPIO89、GPIO8 和共享供电继续由显示/固件路径管理。
* GPIO125 交给 QcomGPIO 分发低电平中断。桥合并现有授权，保留指纹等设备。

本阶段不切断共享电源、不执行 GPIO 硬复位、不下载触控固件，也不支持
触控手势唤醒或系统深睡恢复。PM 状态切换用于停止/恢复输入，在两个状态中
都要求资源保持供电。F1A 电容按键、F51/特殊 force 通道和笔输入尚未实现；
前置指纹 Home 继续使用原有驱动。

### EFI 退出后的运行时初始化

若固件没有初始化触摸屏，使用 `--touch-handoff-profile` 并添加
`--touch-prepare`。这种模式允许模板中的资源确认项为 false，构建时把总线与
触控 handoff 的版本设为 0；native 驱动不会把它当成已完成的交接。
EFI 入口只检查 QUP5 专用核心时钟处于关闭状态并准备 MMIO 映射，拒绝接管
仍在运行、可能有其他固件客户端的控制器。

退出 EFI 后，桥接器通过现有 GCC 实现启用 BLSP1 AHB 和 QUP5，专用根源设为
19.2 MHz，配置 GPIO87/88 的 function 3，启用板级 GPIO50 固定电源并释放
原来处于低电平的 GPIO89。GPIO125 配为输入，留给 native GPIO 中断域。
不触碰面板 GPIO8，不降低共享电源，不改写 PMIC 电压，不重新下载固件。
真实 RMI 功能发现、身份和坐标查询成功并恢复临时寄存器配置后，才把两个
handoff 的版本发布为 1。失败时保留版本 0、输出诊断并继续启动其余系统。
原始 ARM64 Image 没有 EFI producer，不能使用此模式取得触摸交接。

2026-10-20 的真实 QUP5 快照暴露了 PIO 路径误用 BAM 服务标志屏蔽的问题：
接收 FIFO 已有数据且计数完成，但服务标志被屏蔽，轮询直到超时。
PIO 现使用零 operational mask，GIC 控制器中断仍不注册；宿主模型同时模拟
服务标志被屏蔽的行为。FIFO 写完成也处理原厂驱动记录的
`MAX_OUTPUT_DONE` 晚于服务标志的情况，并要求 FIFO 为空且总线空闲。

## 离线验证与产物

```sh
make touch-drivers
make test-touch
make test-touch-sanitized
make touch
make verify-touch
make input-client
```

`input-drivers`、`test-input` 和 `input` 同时包含新增驱动。
独立 `touch` 集合在 `input` 之上提供聚焦测试入口，产物为：

```text
build/output/Extensions/GenericI2C.kext
build/output/Extensions/QcomI2C.kext
build/output/Extensions/SynapticsTouch.kext
build/output/kernelcache.touch
build/output/kernelcache.touch.manifest.json
build/output/kernelcache.touch.verification.json
```

宿主测试覆盖真实生产协议代码的 QUP FIFO/block/tag、重复 START、错误与截止
时间，以及 RMI 功能发现、跨字节 IRQ、F11/F12 报告、CRC 重扫描、回滚、
抬手重试、坐标变换和 HID 描述符位数。Python 集成测试实际编译板级 DT，
检查地址单元、字节序、资源合并、拒绝错误配置、原生 HID 匹配与依赖闭包。
ASan/UBSan 检查宿主内存访问；arm64 Kext 编译与集合校验检查 ABI、导入与重定位。

这些离线检查不覆盖真实 I²C 电气时序、固件留下的实际供电状态、每种屏幕的
查询布局、GPIO125 电平及系统对输入事件的实际消费。历史离线验证记录见
[validation.json](validation.json)。2026-10-22 已另外验证 S3330/F12 在线，
以及驱动报告经原厂解析、BackBoard 投递后引起的真机页面变化；本轮没有新的
物理手指验收，远程页面结果与电气/感测验证的边界见上述当前索引。

诊断工具位于 `build/output/inputctl`，运行 `inputctl registry` 可查看
总线、触屏、HOME 和原生 IOHIDEventDriver 的匹配、触点和报告计数、错误及
HOME 安全调用耗时。`monitor` 只读订阅原生事件；`hid-tap` 需要 root 和
启动时显式启用的诊断 personality，经同一驱动报告和原厂解析器测试页面投递，
标记 `physical_input=0`。它不代替物理手指验收。
