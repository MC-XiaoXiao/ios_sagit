# SDM835 时钟、电压与 CPU 性能管理

驱动使用 Apple Kext、IOService、AppleARMIODevice 匹配及 IOKit PM 框架。
本阶段完成驱动与离线验证；没有访问真机，也没有验证原生 iOS daemon 的运行行为。

## 分层与匹配

`GenericClockController` 提供版本化内核协议、IOWorkLoop / IOCommandGate、IOKit PM、
IOReport 和受限 IOUserClient。`QcomPowerController` 是共享 Qualcomm 后端库，封装
provider MMIO 映射及跨控制器请求；MMCC/GPUCC 共享数据驱动时钟实现。
各硬件 Kext 均通过 Info.plist / OSBundleLibraries / Apple 元类注册，
`IOProviderClass=AppleARMIODevice`，没有采用 Linux 驱动框架。

| Kext | IONameMatch | 实现 |
| --- | --- | --- |
| QcomGCC | qcom,gcc-msm8998 | 40 个 RCG、75 个分支、43 个复位、5 个 GDSC |
| QcomRPM | qcom,glink-rpm | GLINK-RPM intentless，PM8998 的 13 SMPS / 28 LDO 电压 vote；CX/MX/SSCCX/SSCMX level |
| QcomSPMI | qcom,spmi-pmic-arb | v2/v3 PMIC arbiter，PM8005 S1–S4 FTS426 电压、使能、HPM 和读回 |
| QcomMMCC | qcom,mmcc-msm8998 | 39 个根、91 个分支、43 个复位、9 个电源域、8 个固定 Fabia PLL |
| QcomGPUCC | qcom,msm8998-gpucc | 4 个根、5 个分支、4 个复位、2 个电源域、GPU Fabia 调频 |
| QcomOSM | qcom,cpu-clock-osm-msm8998-v1 / v2 | 两集群 OSM LUT、目标档位选择、频率上限和档位枚举 |

资源节点位于 `device_trees/msm8998.dtsi`；现有统一 FDT 转换器将总线转换为
`/arm-io`，由 AppleARMIO 工厂发布设备。所有后端校验完整的物理地址、资源数量和
长度，通过 IOMemoryDescriptor / IOMemoryMap 访问，并使用 ARM64 Device-memory 屏障。
MMCC/GPUCC 对 GCC 映射只读；共享 GPLL 门控通过 GCC 服务完成，避免多个控制器
对同一 vote 寄存器独立读改写。

`GenericARMIO` 根据控制器类型匹配同一总线下的已发布服务，不阻塞总线发布。
缺失返回 NotReady，重复实例返回 ExclusiveAccess。原有 getClockName / Frequency、
enableDeviceClock / Power 接口继续使用 GCC ID 空间。其他控制器使用 v2 定向协议。
Apple 私有批量 gate/pin 数组没有 SDM835 契约，继续拒绝未知格式。

## 电压与时钟事务

RPM 从共享 RAM TOC 发现环形缓冲区，完成 VERSION / OPEN 握手、收发及分片重组，
根据 RPM message ID 配对应答。跨环回绕、缓冲区长度及 cursor 均有边界检查。
迟到 ACK 不完成新请求；远端关闭、协议破坏或超时会隔离传输，不以重试猜测状态。
RPM 没有通用的既有 HLOS vote 查询命令；配置 floor 必须覆盖尚未迁移的消费者，
floor=0 代表调用者已确认可完整交接该资源。PM8005 配置不得包含 OSM/CPRh
仍控制的 CPU 供电轨。RPM 电压/level 在独立 active/sleep 集合内按最多 16 个内核消费者的最大请求聚合，
只有收到对应 ACK 才提交软件状态。RPM 返回的是已确认请求，并非实测电压。
PM8998 LVS1/2 保留名称/ID 描述，但本次未开放电压操作或 switch 控制。

SPMI 读取硬件 APID 映射及 EE ownership，拒绝对非 AP-owned 外设写入；命令最长
8 字节且不能跨 peripheral 边界。PM8005 在首次访问时验证类型/子类型、读取电压、
使能与 step-control，保留启动时已有电压为 consumer 0 的不可释放底线。
新电压向上取整至 4 mV，配置 HPM，按实际步进延迟计算爬升时间，然后检查
VREG_READY 和寄存器读回。部分提交后出错会隔离供电轨。
SPMI telemetry 是 selector 寄存器电压，也不是 ADC 测量。

MMCC/GPUCC 先计算所有已知根时钟与活动电源域对共享供电轨的最大需求，
先确认升压，再切换父源/PLL/RCG；降频 UPDATE 与读回确认后才降低 vote。
未知频率的活动根会阻止电压变更。未声明供电映射的资源不能凭固件当前状态调频。
每个 branch/GDSC 有共享引用计数；调频检查同源分支及强制开启的根。
电源域建立父子引用，处理 reset、clamp、retention 和 Qualcomm 默认状态机等待参数。
局部复位保留无关寄存器位，调用者仍须先停止 DMA 等外设事务。

GPU PLL 只在所有相关根空闲时改频；保持模拟校准配置，检查 RESET_N、L 和 FSM
所有权，完成 Fabia UPDATE/ACK 锁存及 LOCK 等待。RCG/PLL 失败恢复原配置并保留
已提高的电压；无法确认恢复时隔离控制器。MMCC 固定 PLL 不重新校准或任意改频。
MCLK 的 Linux v6.12 描述缺失 MND 位宽，且 33.333/66.667 MHz 比率有矛盾；
生成器明确采用固定 Qualcomm `clock-mmss-8998.c` 比率和 `clock-local2.c` 的
完整寄存器写入、8/16 位读回扩展规则，163 个频率表项经过模拟读回检查。
DSI/DP/HDMI 的外部 PHY 父源尚未接入；未知父源返回未知或 Unsupported。

GCC 保留原有 M/N/D、父频率校验、固件 vote 保留、retention 和回滚实现。
新增四个 MMSS/GPU GPLL 输入分支，统一管理共享 `0x5200c` 寄存器；PLL0 不任意改频。

## CPU 调频与固件边界

QcomOSM 读取 MSM8998 OSM v1/v2 的实际 40 行 LUT，验证索引、频率与电压范围，
排除 single-core turbo，仅发布 quad-core 档位。PLL 乘法先用 64 位计算并验证范围，
再转换成公开的 32 位 Hz，避免异常 L 值溢出后被误认作合法低频。
每次改频重新检查目标、当前 desired index 对应的 LUT 行及 OSM enable；
当前状态无法验证时锁存该集群故障，不发起可能回滚到非法档位的请求。
请求超时后再次校验旧行和 enable，只有旧状态仍合法才回写，并核对恢复结果；
表变化、控制器关闭或回滚失败均隔离该集群。上限向下选择可用档位；
设为 0 恢复硬件表的最高上限。
CPU 启停仍由现有 XNU/PSCI 路径负责，不注册第二套 IOPMGR CPU core 控制者。

OSM 负责 CPU PLL、电压、APM 和 CPRh/LMH 联动。本驱动管理 desired P-state，
读回 desired index 表示请求被接受，不能证明瞬时实测频率或绕过硬件热限制。
CPRh/ACD 的受保护 sequencer、熔丝校准和安全固件初始化仍是必要前提。
未初始化 OSM 时发布不可用状态，不写通用频率表、不通过直接 PMIC 写入绕过 CPRh。
驱动现在提供基于 XNU perfcontrol 的自动策略，也保留显式目标档位与用户态上限。
合法的重复 quad-core LUT 行可作为启动 desired index 的别名；动态读回会校验该行
以及 OSM enable，single-core turbo 行仍不会用于四核集群。

## XNU perfcontrol 自动策略

公共实现位于 `GenericClockController/CPUPerformanceAdapter.*`、
`XNUPerfControlCapture.*` 和 `include/CPUPerformancePolicy.h`。
适配器是 Apple OSObject，使用控制器现有的 IOWorkLoop / IOTimerEventSource；
QcomOSM 只提供 LUT、当前 desired 档位、上限和切换后端。
没有新增硬件匹配 personality 或仿冒 AppleCLPC 类名。

注册的是原生 `sched_perfcontrol_callbacks` v5 的 `csw` 与 `state_update`：
覆盖切换/idle、quantum、QoS/性能属性和 thread-group 更新。
QoS 取 XNU 归一化的 `perfctl_class`，不把不同 ABI 的用户 QoS 整数直接当作档位。
本版 SDK 的 table / thread-data 大小及回调偏移有独立 arm64 编译断言。

调度回调只向固定大小、每 CPU 独立的记录区发布时间、busy 积分、QoS 加权积分
和短时 boost 提示。最多一次弱 CAS；失败只发布一个 loss timestamp。
所有读写都是本机无锁原子操作，不分配内存，不访问 IOService、工作循环、PLL/
PMIC，不获取等待锁、唤醒线程或等待硬件；`WAKE_UNSAFE` 路径同样不唤醒。
arm64 对象检查还验证无阻塞/硬件导入，且回调可达指令图无调用或重试环。
XNU 的属性/分组事件可能使用较旧的 approximate timestamp，记录时将它夹到
上一条记录的时间边界，保留新的 QoS 类别而不产生负的时间积分。
事件可合并；GetPerformanceDroppedSamples 统计记录生产者冲突所影响的 CPU
采样次数，不承诺逐事件精确计数。读取快照时的瞬时竞争也会暂停该窗口决策。

工作线程每 5 ms 采样一次。这是正常调度条件下的目标周期，不是硬实时响应保证。
它通过本地 XNU CPU-state 快照初始化尚未产生回调的 CPU，并排除离线 CPU，
避免将启动时未观测到的 idle 核误当成满载，或永久保留离线核的 busy 需求。
快照读取涉及的调度锁仅在工作线程中获取。

| 策略项 | 当前实现 |
| --- | --- |
| 负载 | busy 时间积分；报告集群平均利用率，决策采用最忙核心的加权需求，保护串行任务 |
| QoS 权重 | background 600、utility 800、普通/内核 1000、user-initiated 1200、UI/above-UI 1400、realtime 1600 |
| 目标余量 | 以 750 为满需求阈值，将需求换算为硬件最高频率的比例，向上选择合法 LUT 档位 |
| 短时性能下限 | user-initiated 65%、UI 80%、above-UI 90%、realtime 100%；事件提示保持 20 ms |
| 持续后台负载 | 最忙核心利用率持续达到 95% 达 30 ms 后，可请求最高档；QoS 权重不是永久吞吐上限 |
| 降频 | EMA 平滑，低需求至少保持 30 ms，且上次自动切换后至少驻留 20 ms；升频可在下一采样执行 |
| 用户态上限 | 最终档位始终不高于 OSM 已确认的 ceiling，QoS/boost 不得越过它 |
| 不可靠样本 | 快照竞争、时间倒退、计数不一致或超过 100 ms 的采样间隙停止该窗口的自动决策，清除 EMA、连续降频与饱和计时，保留实际切换的驻留时间 |

CPU 离线后再次上线、丢失样本或策略重置时，会丢弃边界之前的采样记录和 boost
提示；新调度记录到来前使用 XNU 当前 CPU-state 快照，避免旧 busy/QoS 无限延续。
整个集群离线也会清除连续需求趋势；从启动快照切到首条调度记录时，同样检查
100 ms 的采样间隙，不能把未观测的时间计作持续低负载或持续饱和。

调频与用户请求共用同一 IOWorkLoop gate。工作线程在执行时重新读取当前 ceiling，
不把旧目标放进异步硬件队列，因此不会用过期决策覆盖新上限。
每个集群独立控制：成功的显式 `SetRate` 会进入持久手动模式；`auto 0` 保持当前
档位；`auto 1` 恢复自动。两种模式都接受 `limit`，降低上限会即时压低超限的
目标档位；提高/清除上限在自动模式下由后续负载决定是否升频。

低 IOKit 电源状态暂停调频并清除采样基线，恢复后重新采样。
自动硬件请求失败会隔离该集群的策略，停止反复重试；只有显式重新启用且硬件
仍有已知合法状态时才恢复。OSM 自己的故障状态和 CPRh/LMH 限制仍然有效。
定时器重新排程失败时取消待处理计时，给所有可用集群设置 `Fault` 并停止自动
请求；`Automatic` 位继续表示请求的模式，须结合 `Fault` 判断运行状态。
显式 `auto 1` 先核对回调所有权、控制器在线、合法当前档位与上限，再尝试恢复
定时器。恢复失败向调用方返回错误；成功也仅清除指定集群的故障。
不接管核迁移、CPU hotplug 决策、TSENS 温控或 work-interval deadline 策略；
这里实现的是负载/QoS 驱动的 CPU 频率策略，不是 Apple 闭源 CLPC 算法复刻。

### 回调注册所有权与内核要求

原生 `sched_perfcontrol_register_callbacks()` 没有“仅在空闲时注册”的返回值。
本地 XNU 增加以下私有扩展，补丁为 `tools/patches/xnu-perfcontrol-policy.patch`：

- `sched_perfcontrol_register_callbacks_exclusive`：配置线程中的互斥注册；仅接受
  v5 且所有原生 callback slot 均为默认值，不覆盖已安装的 Apple 性能策略。
- `sched_perfcontrol_callbacks_owned`：原子查询注册身份。
- `sched_perfcontrol_release_callbacks`：仅释放自己的注册，不清除后来接管者。
- `sched_perfcontrol_cpu_state`：工作线程中的初始化/在线状态快照。

这些是本项目的 XNU 扩展，不是声称 Apple 官方已提供的 KPI。
原生注册入口及 scheduler callback ABI 保留；显式原生注册可以接管，适配器在
检测到失去所有权后停止自动请求。注册互斥锁不出现在调度回调路径。
回调与记录区位于常驻 Kext 的静态存储中，停止服务后迟到的回调不会访问已经
释放的 IOService；epoch 区分前后两次适配器生命周期。

自动策略默认启动的前提是：OSM 已校准且当前 desired 档位可识别、XNU CPU
拓扑能映射到对应物理 OSM bank、回调注册空闲。bank 根据物理 MPIDR cluster
映射，不假设逻辑 CPU 连续编号或启动顺序。缺少条件时 QcomOSM 保持原有服务，
发布 `CPUAutomaticPerformanceAvailable=false` 和 `CPUAutomaticPerformanceStartStatus`；
不通过虚构表项或替换其他性能控制者强行启动。
`CPUAutomaticPerformanceAvailable` 记录初始化结果，运行中的故障/暂停/所有权
变化应通过 `GetPerformanceStatus` 查询。

使用这层需要同时使用含上述四个扩展的本地 XNU；旧内核缺少符号，离线链接会
明确失败。本轮修复版本为 GenericClockController 1.3.1 / QcomOSM 1.2.1，
后者明确依赖前者的 1.3.1，保证策略对象布局及恢复行为一致。

### 2026-10-20 CPU 频率管理离线审计

分层继续采用 Apple 的
[XNU 调度性能回调](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm/machine_routines_common.c)
和 [IOTimerEventSource](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/iokit/IOKit/IOTimerEventSource.h)，
通过现有 IOWorkLoop 串行访问 OSM。此次无需增加内核私有接口，也未增加调度
回调中的分配、等待或硬件访问。

修改前的离线用例分别复现了采样中断后立即降频、计时器排程错误未报告故障、
`L=255` 被 32 位乘法误解为约 601 MHz 三个问题。新增回归还覆盖 CPU
重新上线后的旧 QoS、首条回调跨越长采样间隙、暂停时定时器故障、首次排程
失败后的所有权释放、非法/单核 turbo 回滚索引、轮询期间 LUT/enable 变化、
回滚读回失败，以及集群之间的故障隔离。

本轮验证记录及源码/产物哈希位于
`build/cpu-frequency-audit/validation.json`，日志位于同目录。
所有验证仅运行于宿主模型和 arm64 交叉构建，没有连接设备、部署、刷写或
真机测试。温度传感器驱动与系统热策略接线、实际频率/电压/能耗观测以及整机
深度休眠恢复仍未完成验证；OSM desired index 读回不表示实测 CPU 频率。

## 设备树契约

这些是板级拓扑、安全限制与所有权数据，不要求 loader 预先写好每个数字寄存器。
默认节点不会虚构 regulator 接线、GPU speed-bin 电压表或资源所有权；未经交接的
资源保持只读。RPM 的普通初始化会重置本地主机 cursor 并发起 VERSION，因此要求 HLOS
独占 GLINK-RPM 通道。`QcomRPM 1.4.0` 另接受桥内冷启动的 policy version 2 和 48-byte
`qcom,rpm-transport-handoff`：核对 TOC、游标、通道和已排空 TX，沿用消息序号，
接收已停止的桥生产者通道并重新 ACK 资源基线，不重置 FIFO 或重新握手。
协议、启动顺序及边界见 [GPU 桥内电源启动](gpu/hardware-submission.md#桥内-rpm电源与时钟启动)。
两种路径都不能与另一个正在运行的 OS 共享主机端。

所有属性在 Apple DT 中为小端 uint32 数组；统一转换入口支持以下格式：

| 属性 | 格式 |
| --- | --- |
| GCC qcom,gcc-handoff-version | 单个 1；出现 GCC owned-* 时必须提供 |
| GCC qcom,owned-clocks | `{branch-id, fixed-parent-hz}`，有 RCG 的分支第二项为 0 |
| MMCC/GPUCC qcom,owned-clocks | branch ID 列表 |
| qcom,owned-roots | `{root-id, maximum-safe-hz}` |
| qcom,owned-domains / owned-resets | 对应控制器 ID 列表 |
| RPM qcom,regulator-limits | `{rail-id, minimum-uV, maximum-uV, nonzero-floor-uV-or-0}` |
| SPMI qcom,pm8005-regulators | `{S1-to-S4-id, SID, minimum-uV, maximum-uV, floor-uV-or-0}` |
| MMCC/GPUCC qcom,supply-points | `{root-id, upper-hz, controller-kind, supply-id, voltage-or-level}` |
| 电源域 supply-point | root 字段为 `0x80000000 | domain-id`，upper-hz 为 0，其余同上 |

supply controller 2 表示 RPM，3 表示 SPMI。RPM ID 1..13 为 SMPS，
`0x100+n` 为 LDO n，`0x200+n` 为 LVS n；`0x1000..0x1003` 为 CX/MX/SSCCX/SSCMX level。
level 取硬件 RPM 的离散枚举，不能传入 uV。MMCC/GPUCC/时钟 ID 保留 Qualcomm
binding 空间。MMCC 使用 consumer 4，GPUCC 使用 5；其他内核消费者应分配互不
冲突的 slot，0 留给继承状态。相同 rail/root 的供电点必须单调，重复 ID/无效长度/
越界/非法值导致 start 失败或请求拒绝。

## 用户态兼容

优先接入已知 Apple 接口，避免以同名服务伪装私有 PMGR：

- 原生 IOKit PM 提供设备电源状态。当前两个状态均保持上下文，低状态只阻止新
  配置事务；平台尚未实现系统 suspend/resume 与深度 idle。
- IOSimpleReporter / IOReportLegend 以原生 KPI 发布频率、uV、RPM level 和事务计数。
  未知值为 -1，并通过 PowerTelemetryUnavailableValue 明示；不伪造功耗、温度、
  CPU residency 或电池读数。各控制器属性区分 configured/desired/register/request
  读数；用户态不能把它们当作物理测量。
- OSM 将自己的频率上限换算为原生 `CPU_Speed_Limit` 百分比，通过 root domain
  `systemPowerEventOccurred(CPU_Power_Limits, dictionary)` 发布 Power Status。
  另一策略/温控 provider 已占有该字典时保留其状态。此项报告上限，不报告温度。
- PowerUserClient 使用原生 IOServiceOpen / IOConnectCallScalarMethod；不接受用户
  指针、结构体、异步请求、MMIO 映射或任意电压写入。读取使用操作白名单；CPU
  上限/目标频率修改要求 kIOClientPrivilegeAdministrator，且仅允许 OSM。

标量 ABI 在 `drivers/include/PowerUserProtocol.h`，协议版本 2：

| selector | 输入 scalar | 输出 scalar |
| --- | --- | --- |
| 0 GetInfo | 无 | `{version, controller-kind, capabilities}` |
| 1 ReadResource | `{operation, resource-id}` | `{value}` |
| 2 SetLimit | `{cluster 0/1, Hz-or-0}` | `{selected-ceiling-Hz}` |
| 3 SetCPUFrequency | `{cluster 0/1, exact-LUT-Hz}` | `{accepted-desired-Hz}`，同时进入手动模式 |
| 4 SetAutomatic | `{cluster 0/1, enabled 0/1}` | `{configured-automatic-mode}` |

操作编号由 ClockControllerProtocol.h 给出：GetRate=1，GetVoltage=11，
GetPerformanceLimit=18，GetResourceCount=20，GetResourceID=21，
GetPerformanceStateCount=24，GetPerformanceStateFrequency=25，
GetPerformanceStateVoltage=26。档位查询 ID 为 `(cluster << 16) | state-index`；
档位数量查询 ID 为 cluster。最大/最小频率查询分别为 16/15。
自动策略查询：GetAutomaticPerformance=37、GetPerformanceUtilization=38、
GetPerformanceDemand=39、GetPerformanceTransitions=40、GetPerformanceStatus=41、
GetPerformanceDroppedSamples=42，ID 都为 cluster。利用率单位为千分比，需求值
为 QoS 加权量；二者不是物理功耗。Status 位为 Automatic=1、Manual=2、Fault=4、
Paused=8、OwnershipLost=16、Unavailable=32。Automatic 表示配置模式，实际运行
还要求无 Fault/Paused/OwnershipLost。所有读数仍经只读白名单，切换模式同样需要管理员权限。
只读不等于无需系统沙箱/签名授权；真实 iOS 对服务访问的限制仍须之后验证。

`tools/powerctl/powerctl.cpp` 是内部诊断客户端，实际交叉链接 SDK IOKit、
CoreFoundation 和 libSystem 的 arm64 iOS Mach-O，示例：

```sh
powerctl list
powerctl read 6 24 0        # 集群 0 档位数量
powerctl read 6 25 0        # 集群 0、第 0 档频率
powerctl read 6 26 65536    # 集群 1、第 0 档 LUT 电压
powerctl read 6 1 0         # 集群 0 desired 频率
powerctl limit 0 0          # 清除集群 0 上限（管理员）
powerctl auto 0 1           # 集群 0 自动策略（管理员）
powerctl auto 0 0           # 暂停自动，保持当前档位（管理员）
powerctl read 6 41 0        # 自动策略状态位
powerctl read 6 38 0        # 上一个可靠窗口的平均利用率，千分比
# powerctl frequency <cluster> <已枚举的Hz>  （管理员）
```

已核对的是公开 IOKit/RootDomain/IOReport 契约与链接接口。原生 powerd、
thermalmonitord、CLPC/SOCTuner 的私有 PMGR selectors/共享内存 ABI 不能由这些接口
自动替代。`hw.cpufrequency*` 仍由 XNU 启动时的 gPEClockFrequencyInfo 提供，
本驱动不把 desired P-state 写成该全局值或宣称它是实时频率；当前用户态镜像未能提供可分析的完整 daemon，故未宣称其直接运行兼容。
AMFI 的 ApplePMGR 依赖继续按下面的证据保留。

## iOS 18 PMGR 的软件依赖

检查对象是项目已有 iPad7c arm64 缓存，SHA-256：
`aa65c36c486a8b5cb1d7a2d565d1d6b3cc228b3c172f8c69c5da664ee19832cf`。
参考项目的 QEMU `IOPMGR` 示例只用于理解分层，没有复制其 no-op CPU 方法。

Apple 的公开 `IOPMGR` 是 CPU core/cluster 和 idle 回调基类；它不是外设时钟的
通用 KPI。当前项目配置 `USE_APPLEARMSMP=0`，CPU_ON/OFF 和 WFI 已由 XNU/PSCI
负责。因此 `GenericClockController` 和 `QcomGCC` 继承 IOService，而不是为了
匹配名称继承 IOPMGR，避免注册两个实际 CPU 电源控制者。

本样本有四个直接声明 ApplePMGR 的客户端：AMFI、AppleT8010SOCTuner、
AppleT8010PMGR、AppleT8010CLPC。后面三个依赖 Apple SoC；AMFI 是软件驱动，
确实不能忽略其依赖。审计定位到 AMFI 的三个已解析指针：

| AMFI 地址 | 原生目标 |
| --- | --- |
| 0xfffffff006cf4bc0 | 0xfffffff0059dfaa4 |
| 0xfffffff006cf4bc8 | 0xfffffff0059ddfe8 |
| 0xfffffff006cf4bd0 | ApplePMGRNub::metaClass，0xfffffff006ce1048 |

前两个指针分别经 arm64 import stub 在 `0xfffffff005a74bc8` 和
`0xfffffff005a74bd8` 被调用。被调方法读取 nub 的 `+136` 字段，继而调用
ApplePMGR 虚表槽 323/324。此处是本缓存的具体反汇编观察，不据此猜测未导出的
方法名或策略字段含义。调用点还有对动态转换结果的空指针分支；这不能证明
完整 AMFI 启动、用户态或策略初始化可以无 PMGR 运行。

原生 `ApplePMGRNub` 实例大小为 1856 bytes，ApplePMGR 为 28344 bytes，
而公开 IOPMGR 为 136 bytes。仅继承 IOPMGR 或发布一个同名 IOService 无法满足
ApplePMGRNub 的类型转换及具体方法 ABI。

因此保留原生 `com.apple.driver.ApplePMGR` 软件库和真实依赖闭包，不改 AMFI
二进制、不删除依赖，也不发布假的 ApplePMGRNub 或构造虚假的策略结果。
`power` collection 显式包含原生库；`power-security` 加入 AMFI 的完整依赖闭包。
它们不会为高通 DT 添加 `pmgr1,t8010` / `pmgr1,bring-up`，原厂硬件驱动不会被
改名匹配 GCC。这里完成的是框架和链接依赖兼容，AMFI 的运行时行为仍须另行验证。

## 构建和验证

```sh
make build              # 本次增加 XNU 私有扩展，先更新本地内核
make test-perfcontrol
make collect-power      # 从固定、校验 SHA-256 的参考文件生成表
make drivers
make test-drivers TEST_FLAGS="--test gcc --test power --test power-clock"
make power-client
python3 -m unittest discover -s tests -p test_power_integration.py
python3 -m unittest discover -s tests -p test_boot_bridge.py
make power
make verify-power
make power-security
python3 scripts/toolchain.py verify-collection --collection power-security
```

构建、表生成、用户客户端、Kext collection 统一由 `scripts/toolchain.py` 管理，
配置在 `drivers/Build.json`；电源集合明确选择平台/GIC/UFS/GCC及电源测试，
命令行 `--test` 支持单独运行已有配置测试。没有新增一次性构建/部署脚本。
`power_test.cpp` 测试真实线格式、应答 ID、分片/回绕、权限、供电聚合及 OSM 状态；
`power_clock_test.cpp` 测试表项、供电顺序、GPU PLL、域引用、回滚及故障隔离。
结合 ASan/UBSan、FDT 转换、Apple ABI/import 检查及独立 KC verifier。
`test_perfcontrol_integration.py` 将实际调度回调与定时工作线程、实际 XNU 所有权包装
代码放入确定性的 IOKit/调度 harness，并连接实际 OSM 状态机；包括管理员接口
验证、手动/自动切换、用户上限、离线 CPU、PM 暂停、所有权竞争与硬件故障。
`cpu_performance_test.cpp` 还覆盖 10000 个随机负载窗口及并发原子记录。
打包器回归还覆盖 kmod 名称末尾的小整数不能被误当成重定位指针。
最终日志、哈希及离线验证结果见 `docs/power/validation.json`。

参考实现和来源哈希位于 `docs/power/reference/sources.json`。硬件协议/数据参考
Linux v6.12 与固定 Qualcomm Android commit；框架采用项目中的 Apple XNU/SDK。
通用协议/Apple glue 标注 BSD-2-Clause，参考硬件实现及派生表保留 GPL-2.0-only。
