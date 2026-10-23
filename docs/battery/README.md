# PMI8998 电池数据与 Apple 电源框架

本阶段实现 **SPMI → PMI8998 FG Gen3 / SMB2 → IOPMPowerSource** 的只读数据通路、
IOKit 生命周期和诊断工具。宿主测试、真实 XNU SDK 交叉编译及离线链接验证已通过。
**2026-10-22 已在 Mi 6 真机取得有效采样；用户已确认界面出现充电连接提示**。
状态栏电量百分比、系统 IOPS 消费和充电净增量仍需分别验证。

USB调试时发现当前运行的98-Kext集合未包含 QcomBattery，注册表也没有
IOPMPowerSource。将已有驱动合入99-Kext候选后，实际读到电量47%、电压3750mV、
温度41.10°C，`ExternalConnected=true`、`IsCharging=true`；当前净电流为-298mA，
表示这个采样时电池仍在净放电，不能把充电器状态开启等同于电量正在增加。
该驱动只读取固件保留的充电器和FG状态，没有改写充电参数。
`batteryctl --system` 仍报告系统电源源不可用，需要继续核对 powerd 的发现与消费。
UART原始记录和结构化结果保存在 `build/usb-bringup-20261022/power-status-host.log`
及 `charging-hardware-validation.json`。

## 项目与框架分析

这不是 UIKit 应用项目，而是在 SDM835 上运行 iOS 18.6 的 XNU 移植。
应用层 `UIDevice.batteryLevel` 只能消费系统已有的电池信息，不能为系统提供电池硬件后端。
本项目已有 `QcomSPMI`（总线串行访问）、`QcomPMIC`（按键／连接器）和
`PMI8998WLED`（背光），但此前没有 `IOPMPowerSource` 电池生产者。

Apple XNU 的 `IOPMPowerSource` 在其 settings dictionary 中保存属性，
`updateStatus()` 将变化发布至 IORegistry 并发送
`kIOPMMessageBatteryStatusHasChanged`。根电源域按 `IOPMPowerSource` 类匹配；
对应源码在 `kernel/iokit/Kernel/IOPMPowerSource.cpp` 和
`IOPMrootDomain.cpp`。本次直接使用本项目构建的 XNU 类及导出符号，
不移植 Apple 电量计的硬件后端，也不伪装成 `AppleSmartBattery`。

`QcomBattery` 通过独立 `IOMatchCategory=BatteryObserver` 匹配 `QcomSPMI`，
不占用背光的独占 retainBus 租约。成功取得第一组完整数据后，显式调用
`QcomBatteryPowerSource::init(void)` 并发布继承自 `IOPMPowerSource` 的子服务。
这样既正确初始化 Apple 电源属性字典，也避免启动时将未读到的数据当作 0%。

系统侧还需要 powerd 接受此电源服务，之后才可能到达 IOPS、BatteryCenter 和 UIKit。
诊断工具包含独立的 `--system` 检查路径；目前仅验证该路径可以链接原生 IOKit API，
尚未验证 iOS 18 powerd 的实际发现、缓存和 UI 消费行为。

## 硬件依据与数据转换

小米官方 `sagit-n-oss` 提交
[`3fb35ef1f0fe1f714be3df39724cbc8eb217181e`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/3fb35ef1f0fe1f714be3df39724cbc8eb217181e)
的 `msm-pmi8998.dtsi`、`qpnp-fg-gen3.c`、`fg-reg.h`、`smb-reg.h` 和 `smb-lib.c`
确定了以下布局。参考文件保存在 `reference/`，`sources.json` 固定 URL 与 SHA-256，
并纳入现有 Kext 构建校验。补充的 Qualcomm `qcom-fg-reg.h` 定义 SOC_READY 位。

| 项目 | SID 2 寄存器 | 发布语义 |
| --- | --- | --- |
| 芯片身份／版本 | `0x103..0x105`，`0x4005`，`0x4105` | 限 PMI8998 数字主版本 1/2、FG SOC/INFO subtype |
| 电池存在 | `0x4107` | missing bits 为零才认为电池存在 |
| FG 可读条件 | `0x4046`、`0x4010`、`0x4048` | 算法启用、SOC ready、无 restart |
| 电量 | `0x4009/0x400a` | 影子值一致后，`round(raw × 100 / 255)` |
| 电压 | `0x41a0/a1`，副本 `a6/a7` | `raw × 122070 / 1000000` mV |
| 电流 | `0x41a2/a3`，副本 `a8/a9` | 有符号 16 位；`−raw × 488281 / 1000000` mA |
| 温度 | `0x4106`、`0x4150/51` | 有效位成立时，四分之一 K 转为 0.01 °C |
| 输入电源 | `0x160b` | 有效输入且实际使用 USBIN/DCIN |
| 充电状态 | `0x1006/07/0d` | 充电阶段、充电使能及故障状态共同判断 |

电压／电流在 PMI8998 v1 上交换字节序；温度不交换。电流转换为 Apple 的
正值充电、负值放电约定。温度发布单位参考 Apple 官方
[`AppleSmartBattery.cpp`](https://github.com/apple-oss-distributions/PowerManagement/blob/d415e45501842834a280930c3eed9186544a67f0/AppleSmartBatteryManager/AppleSmartBattery.cpp)
中非 macOS 分支的 centi-C 语义；这份 OSS 实现用于字段依据，不代表已经验证 iOS 18 私有消费者。

`CurrentCapacity/MaxCapacity` 发布为百分比／100，额外标记
`BatteryCapacityUnits=Percent`；这是相对容量，**不是 mAh 或健康度**。
`Voltage` 为 mV，`Amperage` 和 `InstantAmperage` 都来自本次瞬时采样，
`Temperature` 为有符号 32 位、0.01 °C。`TimeRemaining=-1` 表示未知。
没有可用温度时只撤销温度字段，仍可发布其他完整读数。

不发布设计容量、实际满充容量、循环次数、序列号、厂商、健康度和充电剩余时间；
不根据板级 3350/3440 mAh 电池配置猜测当前电池。`ExternalChargeCapable` 也暂不发布，
因为当前没有适配器协商／预算依据。终止充电且电量至少 99% 才报告 FullyCharged，
不把 inhibit 当作充满，也不强制将电量改为 100%。

## 一致性、失败与电源管理

每 5 秒采样一次，板级配置允许 1–60 秒。SOC 影子一致性最多读 5 次；
整组采样最多尝试 3 次，并复验电池存在、供电路径、充电状态、SOC、
FG ready／enable／restart。电压／电流还核对硬件副本。失败时不提交部分数据；
有效采样必须通过 1500–5000 mV 的单节电池合理性检查。温度有效范围为 −40–100 °C。
这些检查避免明显无效读数，不能证明固件使用了正确的电池校准配置。

采样、定时器、PM 与清理都经 IOWorkLoop 串行化。读取失败后，
原生电源服务撤销旧的容量、电压、电流、温度和供电／充电状态，
设置 `BatteryTelemetryValid=false` 与 `ErrorCondition`，通过原生通知报告失效；
后续成功会清除错误并恢复字段。没有电池与读取失败采用不同状态，前者才发布
`BatteryInstalled=false`。暂停取消定时器并使数据失效，恢复立即重新读取。
这是驱动轮询的 PM 支持，不等于已经验证整机深度休眠或电池 IRQ 唤醒。

控制器保留 `BatteryReadError`、`BatteryLastIOError`、成功／失败次数、
`BatteryLastAttemptUptimeNS` 和 `BatteryLastSuccessUptimeNS`，便于区分未配置、
驱动未匹配、SPMI 拒绝、FG 未就绪和读数不一致。时间戳使用 XNU uptime。

| BatteryReadError | 含义 |
| --- | --- |
| 0 | 成功 |
| 1 | 无效板级配置（启动时拒绝） |
| 2 | I/O 或发布资源分配失败，另看 BatteryLastIOError |
| 3 | 芯片／版本／外设类型不支持 |
| 4 | FG 未就绪、算法未启用或正在 restart |
| 5 | 重试后采样仍不一致 |
| 6 | 电压超出支持范围 |
| 7 | 驱动暂停 |

没有 PMIC 写入接口，不修改 FG profile、SRAM、充电电压／电流、JEITA、
充电开关、OTG、ADC 触发或中断屏蔽，也不实现软件强制关机策略。
充电及其保护仍依赖固件／硬件原有配置；若 EDK2 启动链没有保留可用 FG，
驱动将持续报告未就绪。本次没有确认固件 profile 与实装电池的匹配，
因此始终显式标记 `BatteryProfileVerified=false`。

## 构建与后续检查入口

```sh
make battery-driver
make battery-client
make test-battery
make test-battery-sanitized
make battery
make verify-battery
```

`make battery` 构建独立的 `build/output/kernelcache.battery`，包含平台及电池依赖，
不含完整图形用户环境。需要完整 iOS 图形／输入依赖时，可离线运行：

```sh
make kernelcache COLLECTION=battery-runtime
```

`battery-runtime` 继承 `graphics-runtime` 和 `battery`；本阶段没有构建或部署该完整集合。
现有其他 collection 的默认装载列表不变。设备树添加
`qcom,battery-observer=<1 2 0x4000 0x4100 0x1000 0x1600 5000>`，
启动桥已把这些 DTS 数值转换为 Apple DT 小端 cells，并有真实 DTS→DTB→Apple DT 测试。

生成的 `build/output/batteryctl` 是 arm64 iOS 可执行文件；本阶段没有执行它。
后续允许真机测试后，可用默认模式检查驱动／电源服务的完整 IORegistry 属性，
用 `batteryctl --system` 独立检查 IOPS 原生电源列表。
没有有效数据时返回非零状态，不提供任何充电控制命令。

离线测试包含实际寄存器读取算法的故障注入，以及直接编译执行生产
`QcomBattery.cpp` 的宿主 IOKit harness。后者验证首次失败、成功发布、
原生通知去重、数据失效、故障恢复、无电池、暂停／恢复和 start 失败回滚；
它不模拟真实 iOS powerd，不替代真机行为验证。
构建／验证日志及可机器读取的结果见 `build/battery/` 和本目录 `validation.json`。
