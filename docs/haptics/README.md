# 小米 6 / PMI8998 振动马达驱动

2026-10-22：新增 `PMI8998Haptics.kext`、内部诊断客户端 `hapticsctl`、独立
`haptics` collection 和离线测试。实现的是 PMI8998 direct-mode LRA 硬件驱动。
**未连接、刷写、部署或进行真机测试；默认板级配置不授予驱动写权限。**
UIKit、Core Haptics、系统来电与键盘反馈尚未桥接，不能据此宣称系统振动已可用。

## 项目与 iOS 18 原厂路径

项目是 Linux 上交叉编译的 XNU 11417 / iPhoneOS arm64 移植，现有
`QcomSPMI` 提供串行 PMIC 访问，`PMI8998WLED` 与电池服务已共享这条总线。
本次使用同一控制器、原生 IOService / IOUserClient / IOWorkLoop / PM；没有
复制 Apple AOP 寄存器，也没有将 Android timed_output 接口当成 iOS 接口。

原厂分析使用本地固件，而非根据类名猜测 ABI：

| 提供映像 | 实际发现与处理 |
| --- | --- |
| 项目运行时 iPad7c arm64 kernelcache | 不含触觉硬件 Kext；继续作为当前运行时 provider |
| iPhone14,6 iOS 18.6.2 / 22G100，arm64e | 含 AppleAOPHaptics 840.26、AppleHapticsSupportLEAP/NVM 9.21；仅用于路径分析，不装入本项目 arm64 集合 |
| AppleAOPHaptics | 匹配 `AppleAOPAudioDeviceNode` 的 `aop-audio-haptic`，查找 `AppleAOPAudioDeviceProvider`，依赖 AOP 音频设备链 |
| AppleHapticsSupportLEAP | `AppleARMIODevice` / `haptics-support,leap`，有固件、校准、电源预算组件；用户客户端要求 `com.apple.ahs.userclientaccess` |
| AppleHapticsSupportNVM | 匹配 `AppleSPUAppInterface` / `arc-eeprom-i2c`，不是通用马达输出端 |

IDA 字符串交叉引用与反编译确认：`function-haptics_session` 是会话开始/结束通知，
参数不能解释成“持续时间、强度”。仅发布相同 IORegistry 名字或该属性不能接通
播放路径。本驱动明确发布 `HapticsNativeAOPBridge=false`，不冒充这些原厂类。

`ios18-model.json` 记录映像哈希、原厂 personality 和依赖；
`native-interface.json`、`native-decompile.txt` 记录已检查的地址与语义。
提取 Kext 的 PAC/导入和部分反编译 helper 未恢复，未将 arm64e 偏移投影成
本项目 arm64 子类 ABI。驱动的 168-slot IOService 与 186-slot IOUserClient
由当前 XNU SDK 编译器虚表验证，直接链接本地 XNU 的实际导出。

重新生成固件清单：

```sh
make analyze-haptics
```

它只读取本地 IPSW，提取/解压比较内核，并扫描本地两个 provider；不访问设备。
人工反编译证据不会由清单命令重新生成。

## 板级依据与实现

依据小米官方 `sagit-n-oss` 固定提交
[`3fb35ef1f0fe1f714be3df39724cbc8eb217181e`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/3fb35ef1f0fe1f714be3df39724cbc8eb217181e)：
实际手机的 [`sagit-msm8998-mtp.dtsi`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/3fb35ef1f0fe1f714be3df39724cbc8eb217181e/arch/arm/boot/dts/qcom/sagit-msm8998-mtp.dtsi)、
`msm-pmi8998.dtsi`、`drivers/soc/qcom/qpnp-haptic.c` 和绑定文档。
参考副本、来源 URL、SHA-256 见 `reference/sources.json`，构建校验会核对它们。
通用 `msm8998-mtp.dtsi` 不能替代手机专用 sagit 配置：通用默认 6667 µs，
sagit 实际覆盖为 5000 µs。

| 项目 | 设置 |
| --- | --- |
| SPMI 路由 | SID 3，peripheral `0xc000` |
| 芯片确认 | SID 2 的 `0x103..0x105`，PMI8998 type/subtype `0x51/0x15`，主版本 1/2 |
| 驱动方式 | LRA、direct、正弦，周期 5000 µs（200 Hz） |
| 幅度 | 1–1000 归一化电压指令；116 mV 步进，最高 3248 mV；不代表线性加速度 |
| 电流限制 | 800 mA，板级可收紧到 400 mA |
| 保护 | 八周期短路去抖；运行期间每 5 ms 读取状态；短路或 QWD 错误锁定故障 |
| 自动谐振 | QWD、高阻 opt1、四校准周期；先关闭 TEST2 auto-res，超过 20 ms 的脉冲在启动 20 ms 后启用 |
| 停止/制动 | 清 PLAY，保留静态 `[03 02 00 00]`，等五周期（25 ms）后关闭 MODULE_EN |
| 时间范围 | 5–15000 ms；内核按单调时间检查，不由客户端计数决定停止 |

RATE_CFG 编码为 `5000 / 5 = 1000`，低字节 `0xe8`，高四位 `0x03`。
VMAX 为 `floor(28 × strength / 1000) × 116 mV`，非零最低一级。
大于 3248 mV 的请求被拒绝；本阶段没有开启原厂的 3596 mV 动态 overbrake、
overdrive、按持续时间变化的制动表、频率追踪或不同波形上传。

启动只接受明确的 AP 独占交接；先读取身份、MODULE_EN、PLAY 和 BUSY。
身份未知或检测到既有播放时，拒绝接管且不写寄存器。不会在加载时发出测试振动。
同一 SPMI 工作循环保护整个读改写和 `SEC_ACCESS=0xa5` / TEST2 对，
20 ms 的反电动势等待和制动用定时器完成，不阻塞共享总线。
不用背光的独占 `retainBus` 租约；本服务作为 SPMI 的 PM 子节点提出正常电源要求。

客户端同一时刻只有一个播放所有者。其他连接可读状态，不能改变在播输出。
正常结束确认关闭后释放所有权；关闭、进程死亡、PM-off、清理会独立尝试清 PLAY
和 MODULE_EN。PM-on 的重复通知不会取消正在运行的期限；恢复不会自动重放。

传输失败、读回不符、短路、谐振错误或定时器设置失败后，尝试切断输出并锁定故障。
PLAY 清除失败也会单独尝试 MODULE_EN；SPMI 完全不可用时，无法保证物理停机，
此时 `HapticsOutputKnownOff=false`，不能把软件状态当作硬件已停止。
输出未知时保留客户端所有权，使它关闭时仍可重试切断；确认关闭不会解除故障锁定。
故障恢复需要新的驱动生命周期和可信交接，没有无条件清错或重播接口。
5 ms 是软件轮询间隔，未证明实时期限或短路 IRQ 响应，也没有验证整机深度休眠。

## 设备树与授权

`device_trees/xiaomi-sagit.dts` 在 SPMI 节点声明：

```dts
qcom,pmi8998-haptics = <1 0 3 0xc000 5000 3248 800 15000>;
```

八个 cells 依次为：协议版本、独占交接标记、SID、base、周期 µs、最高 mV、
电流上限 mA、最长 ms。当前第二个 cell 是 **0**，驱动不绑定、不写马达寄存器。
标记 1 表示启动链已确保 AP 独占并可访问该外设；本次没有实现或验证该交接生产端，
不能仅修改数值就声称资源已交接。SPMI arbiter 的真实 APID 访问权限仍由硬件检查。

启动桥将此属性从 FDT 大端 cells 转为 Apple DT 小端 cells，有实际 DTS→DTB→
Apple DT 回归测试。Kext 通过独立 `IOMatchCategory=HapticsController` 匹配
`QcomSPMI`，不占用背光/电池匹配类别，也不修改原有默认 collection roots。

## 构建、接口与离线验证

```sh
make haptics-driver
make haptics-client
make test-haptics
make test-haptics-sanitized
make haptics
make verify-haptics
```

产物：

- `build/output/Extensions/PMI8998Haptics.kext`
- `build/output/hapticsctl` 与 `.json` 交叉链接报告
- `build/output/kernelcache.haptics`、`.manifest.json`、`.verification.json`

`haptics` 是平台加马达的独立离线集合。`haptics-runtime` 另继承
`graphics-runtime`，保留完整图形环境的构建入口；本次未构建或部署该完整集合。

`hapticsctl info` / `status` / `stop` / `pulse <ms> <strength>` 使用本项目
`HapticsProtocol.h`，仅管理员可打开，非 Apple 私有 ABI。`pulse` 保持连接至
输出与制动结束；诊断进程退出会取消自身输出。没有执行此 iOS 可执行文件。
`stop` 不能取消另一个仍持有租约的客户端播放。

selector 0 / 1 分别读取 8 / 12 个 uint64，selector 2 输入 duration / strength，
selector 3 停止。拒绝异步端口、结构体/OOL 输入输出、错误计数、64 位越界值和未知 selector。
状态的 12 项依次为 version、state、initialized、output-known-off、strength、
applied-mV、remaining-ms、result、last-IO-error、generation、awake、owner-active。
state 的 0–5 为 Uninitialized / Idle / Playing / Braking / Suspended / Fault；
result 定义见协议头。IORegistry 的 `Haptics*` 字段提供同样的关键诊断。

离线测试覆盖寄存器保留位、身份/权限拒绝、时序、短脉冲、制动、迟到回调、
逐项初始化/播放写失败（含实际已写入却报错）、初始化读失败、输出卡位、
总线丢失与故障锁定。另在宿主执行真实 IOKit 适配器和用户客户端源码，验证
工作循环、两客户端所有权、关闭清理、重复 PM、定时器失败、参数验证和对象回收。
宿主 shim 不模拟真实 IOKit 电源调度，单独用实际 XNU SDK 编译及虚表/导入检查补充。
最终验证结果见 `validation.json`，日志位于 `build/haptics/`。

后续系统接入需要恢复当前用户态触觉消费者的发现、PCM/控制与校准约定，
为 PMI8998 提供实际播放桥并验证 Core Haptics / UIKit。仅有 AOP 会话通知或
将驱动改名为 AppleHapticsSupport 都不足以完成这一步。
