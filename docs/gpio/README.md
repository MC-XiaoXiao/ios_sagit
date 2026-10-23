# GPIO 与平台按键

`QcomGPIO.kext` 实现 SDM835 TLMM GPIO；`QcomPMIC.kext` 实现 PM8998 GPIO/PON
输入和 PMIC 中断域；`QcomButtons.kext` 将锁屏、音量加、音量减转换为原生 Apple
HID 输入。`GoodixFingerprint.kext` / `FPCFingerprint.kext` 通过共享的
`FingerprintHID.kext` 实现前置 Home 输入，传感器协议来自实际 HAL 和原厂固件分析。
驱动使用 IOKit 的 personality、OSMetaClass、IOInterruptController、
IOWorkLoop、IOCommandGate、IOInterruptEventSource 和 IOTimerEventSource。
已在真机通过 EDK2 暂存并运行 EFI，未刷写分区。2026-10-17 的运行时集合沿用
Setup 调试任务的四核、全内存及 APFS 基线；TLMM、PMIC、侧键 HID 和 FPC Home
均成功启动，侧键与 Home 均由原生 IOHIDEventDriver 打开，随后进入 root Bash。
实际按下/松开和 iOS 锁屏、音量策略仍须结合物理按键采样确认。
Home 通过 QSEE 安全应用确认按下/松开，IRQ121 仅触发处理，不当作按键电平。
固件接入、支持的传感器协议和运行前提见 [Home 驱动说明](home.md)。

## 框架与匹配

```text
AppleARMIODevice (qcom,msm8998-pinctrl)
  QcomGPIO : IOInterruptController
    TLMM summary SPI208 -> AppleARMGICv3

AppleARMIODevice (qcom,spmi-pmic-arb)
  QcomSPMI : QcomPowerService
    QcomPMIC : IOInterruptController
      PMIC summary SPI326 -> AppleARMGICv3
      QcomButtons : IOHIDDevice
        IOHIDInterface (Consumer Control)
          IOHIDEventDriver (原厂 IOHIDFamily)

AppleARMIODevice (goodix,fingerprint / fpc,fpc1020)
  GoodixFingerprint / FPCFingerprint : FingerprintHID : IOHIDDevice
    QSEE trusted application -> confirmed finger down/up -> Consumer Menu
    IOHIDInterface -> IOHIDEventDriver (原厂 IOHIDFamily)
```

原生 iOS 18 的 AppleGPIOICController 继承 IOInterruptController，按硬件类型匹配
AppleARMIODevice；AppleGPIODevice 是其客户端对象。AppleM68Buttons 继承
AppleEmbeddedHIDEventService。结构、虚表和 personality 证据保存在
[`ios18-model.json`](ios18-model.json)。参考项目的 GPIO 头将通用名称映射到
AppleS5L8960XGPIOIC；本实现不使用这个硬件相关别名，也不调用 Apple SoC 的寄存器实现。

本项目采用 Apple 公共的 IOInterruptController 和 IOHIDDevice 扩展模型。
GPIO/PMIC Kext 通过自己的 personality 匹配，按键由 QcomPMIC provider 匹配。
IOHIDDevice 负责描述符解析、IOHIDInterface 发布和报告分发；按键 Kext 不直接操作
SpringBoard，不合成系统消息，不提供私有用户态按键注入接口。

样本的 Consumer Control personality 限定 `Transport=FIFO`，不能匹配真实的 SPMI
输入。因此 QcomButtons 的 plist 另包含一个 personality，使用
`IOProviderClass=IOHIDInterface`、`Transport=SPMI`、`Product=Platform Buttons`
和 Consumer Control usage pair，实例化原厂 `IOHIDEventDriver`。该 personality
的 `CFBundleIdentifier` 指向其真实提供者 `com.apple.iokit.IOHIDFamily`，并声明依赖。
统一校验器验证提供者依赖、元类和原生类布局，拒绝没有证据的外部 IOClass。

原生 IOHIDDevice 的 arm64 实例为 208 字节、207 个虚表槽；IOHIDEventDriver 为
264 字节、220 槽。最小声明头与 [`apple-hid-abi.json`](../../drivers/abi-22H374/apple-hid-abi.json)
绑定指定 provider 的 SHA-256、类布局、元类、构造/析构、虚表和虚方法地址。
未调用的私有槽保留原厂实现。Apple OSS 的公开接口用于核对 API 语义，实际二进制
ABI 以本项目 iOS 18 样本为准；SDK 为 iPhoneOS 18.6，样本元数据为 iOS 18.7。

## TLMM

普通 GPIO 为 0..149。SDM835 的 NORTH/WEST/EAST 分区不是连续的统一寄存器数组；
`TLMMPins.h` 记录全部 150 个引脚的实际偏移，资源范围为 `0x03400000/0x00c00000`。
支持 GPIO 输入、输出、拉电阻、驱动强度和五种中断触发方式。写输出时先设置数据，
再打开输出使能。中断状态使用 MSM8998 的写零清除规则；屏蔽边沿 IRQ 时保留
RAW_STATUS_EN，屏蔽电平 IRQ 时关闭该位，避免重锁存和中断风暴。

设备树的 `qcom,owned-gpios` 是可修改和注册中断的引脚列表。不配置时控制器只提供
读操作，summary IRQ 保持屏蔽。没有遍历重置所有引脚，也不会自动重配 UART、
显示、存储的固件 pinmux。注册 IRQ 要求引脚已处于 GPIO 输入模式；重复注册失败。
summary 只分发已启用且目标为 KPSS 的 pending 源；屏蔽期间保留的边沿状态不会被
误报。迟到的空中断允许有界重试，真实的未接管源或连续空中断风暴会关闭该域。
中断 specifier 是 `<pin trigger>`，trigger 为 1 上升、2 下降、3 双边沿、4 高电平、
8 低电平。桥只转换 GIC specifier 的编号和 edge/level 编码，保留子中断域的极性。

内核客户端使用 `callPlatformFunction`：

| 函数 | param1 | param2 | 其他参数 |
| --- | --- | --- | --- |
| `getGPIOData` | 引脚号，整数转换为指针 | `UInt32 *` 返回 0/1 | 空 |
| `setGPIOData` | 引脚号 | 0/1，整数转换为指针 | 空 |
| `setGPIOCFG` | 引脚号 | pull[1:0]、drive[8:6]、output[9] | 空 |
| `acknowledgeGPIOInterrupt` | 已注册该中断的 provider `IOService *` | source 索引，整数转换为指针 | 空 |

pull 编码 0 无拉、1 下拉、2 keeper、3 上拉；drive 编码 0..7 对应 2..16mA。
配置输出时初值为低；需要高初值的客户端应使用扩展配置位 initial-high[10]。
已有中断客户端时拒绝改方向、pull、drive 或输出值。这里没有用户态 MMIO 接口。
中断确认接口要求 source 已由同一个 provider 注册且处于软件屏蔽状态，
只清除对应 pin 的锁存状态，用于设备事务已消费通知后的重新布防。

## PMIC 与按键接线

QcomSPMI 新增的总线入口与现有稳压器操作共用同一工作循环串行执行，读取和写入
仍遵守现有 arbiter APID 映射和写所有权。QcomPMIC 只接管 `qcom,owned-irqs` 声明
的 SID/peripheral/bit；拒绝 APID 属于其他执行环境的配置。PON 的两个按键共用一个
APID，但各自配置触发位、屏蔽位和锁存位；RMW 保留同一外设的其他配置位。
若该外设存在未声明的已启用 IRQ，拒绝接管，防止把固件 IRQ 引入自己的 summary。

PMIC 主 IRQ 在 GIC 层由 IOInterruptEventSource 延迟处理。硬中断不执行 SPMI
事务；工作循环读取 arbiter IRQ_STATUS、清除 PIC 与 PMIC 锁存，再通知子事件源。
出现总线错误或未声明的 pending bit 时屏蔽父 IRQ并发布故障状态，避免持续重入。
连续没有本域待处理位的父中断也有次数上限，防止其他固件 APID 引发无限重入。
输入路径由硬件 IRQ 驱动；计时器用于防抖、短暂错误重试和按住期间的释放检查。

接线来自 Linux v6.12 的 PM8998 和板级 DTS，保存在参考清单中；只有现有板级 DTS
配置具体线路，驱动代码不包含具体手机名。

| 按键 | 硬件来源 | 按下状态 | HID Consumer usage |
| --- | --- | --- | --- |
| 锁屏/电源 | SID0 PON，peripheral 0x08，bit0 | PON RT_STS bit0=1 | 0x30 Power |
| 音量加 | SID0 PM8998 GPIO6，peripheral 0xc5，bit0 IRQ | GPIO RT_STS bit0=0 | 0xe9 Volume Increment |
| 音量减 | SID0 PON RESIN，peripheral 0x08，bit1 | PON RT_STS bit1=1 | 0xea Volume Decrement |

`qcom,owned-irqs` 每项四个 LE32 cell：SID、peripheral、bit、trigger。
`qcom,button-map` 每项五个 LE32 cell：usage、kind、SID、line、debounce-ms；
kind=0 为 PON，line=0/1；kind=1 为 PMIC GPIO，line=1..26。必须恰好声明三个不同
usage、不同线路，并具有对应的双边沿中断所有权。源 DTS 仍采用标准大端 cell，
由现有 `scripts/build-bridge.py` 转换为 Apple DT；没有新增设备树转换脚本。

按键 GPIO 自动配置正常输入、30uA 上拉，保留固件 VIN 和输出驱动强度。
支持 PMIC legacy、LV、MV GPIO 布局；PON 只配置所需上拉，不更改长按复位、
PS_HOLD 或固件硬件防抖。软件防抖按键分别计时，当前板级配置均为 15ms。
HID 报告为一个字节的三个独立位，支持同时按下、组合键和同时松开；不重复发送
长按状态。启动时已按住的键经过防抖后正常发布；停止或逻辑暂停时发送中性报告。
连续读取失败后停止输入并释放已发布状态；发送失败保留重试，避免静默吞掉松开。

PMIC GPIO 内核配置入口 `getPMICGPIO` / `configurePMICGPIO` 的 param1 指向
`QcomPMICInput::GPIORequest {sid,pin,pull,output,value}`，其余参数为空。
仅允许声明了对应 GPIO 中断所有权的引脚；已注册 IRQ 时拒绝配置。
pull=0..3 为上拉档位、4 下拉、5 无拉；GPIO 编号按 PMIC 文档从 1 开始。

## 构建、验证和范围

```sh
make input-drivers
make test-input
make input
make verify-input
make prepare-input
```

完整系统调试使用 `input-runtime`，继承现有 `display-runtime` 和 `input` 的依赖。
可用 `INPUT_FLAGS="--local-kernel /absolute/path/to/mach.development.sdm835.unstripped"`
明确选择已验证的基础内核；`input` 同样支持该选项。打包器冻结该文件并在 manifest
记录哈希，避免误用全局输出目录中的较早内核。当前真机基线与完整桥接命令记录在
`build/input/runtime-invocation.json`；通过原有 `build-bridge.py` / `test-bridge.py`
构建和运行，不另建设备控制脚本。`input-report-trace=1` 可临时记录成功提交的 HID
状态变化。系统已有 `ioreg` 和 `hidutil`，无须安装额外诊断客户端。

本轮修复了本地 Kext 指向 `__TEXT` 常量表的重定位遗漏：SPI 频率表的指针原为
`0x1128`，导致 GCC 初始化异常。链接器现按 Mach-O 本地重定位记录处理，并通过
对应回归和 851 个实际本地指针的逐项核对。证据见
`build/input/local-pointer-validation.json`，启动与恢复日志在 `build/input/hardware-*`。

全部构建和固件准备进入原有 `scripts/toolchain.py`。`input_firmware.py` 是该入口与
现有桥接器共用的固件解析模块，不另设下载、设备树转换或测试运行入口。
原厂依赖、包括 personality-only 的 IOHIDEventDriver bundle，由 `input` collection
自动解析。打包器的符号路由同时支持有代码和仅 personality 的 Kext。

产物为 `build/output/Extensions/` 下的 `QcomGPIO`、`QcomPMIC`、`QcomButtons`、
`GoodixFingerprint`、`FPCFingerprint`、`FingerprintHID` Kext 和
`build/output/kernelcache.input`；manifest 与 verification JSON 位于同目录。
`test-input` 覆盖全部引脚偏移与上游硬件表的一致性、五种 IRQ 模式、输出顺序、
PON/GPIO 极性、GPIO 子类型、APID 所有权、失败注入、HID 描述符、逐键防抖、组合键、
Apple DT 字节序、原生 HID personality/provider 校验、Home 协议字段、错误/空中断
过滤、报告失败重试、固件分段检查与所有权合并。验证汇总见
[`validation.json`](validation.json)。

当前 PM 状态处理保留供电和上下文，只支持逻辑暂停/恢复。`GPIODeepSleepWakeSupported`
和 `PMICDeepSleepWakeSupported` 明确为 false；平台尚无 MPM/PDC 深度休眠唤醒交接，
本驱动不声称实现了断电保存恢复或深度休眠唤醒。锁屏键发布标准 HID Power，具体锁屏、
长按、音量重复和组合键策略由原厂 HID/iOS 上层处理，实际运行效果留待真机阶段确认。

前置条件是固件已允许 HLOS 访问声明的 TLMM/SPMI 资源、保留 PMIC 供电，并将 summary
IRQ 交给本内核。离线通过仅证明构建、模型与集合结构，不能证明上述硬件条件已成立。

参考源码与哈希保存在 [`reference/sources.json`](reference/sources.json)。Apple API
语义参考 [IOHIDFamily](https://github.com/apple-oss-distributions/IOHIDFamily)，TLMM 与
PMIC 寄存器参考 [Linux v6.12 Qualcomm pinctrl](https://github.com/torvalds/linux/tree/v6.12/drivers/pinctrl/qcom)。
