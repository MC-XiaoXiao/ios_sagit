# iOS 18 IOGPUFamily 调度与复用边界

IOGPUFamily 提供任务、资源、事件、软件调度和完成回收框架。QcomGPU 1.168 起已在真机验证 `IOGPUScheduler`、`IOGPUWorkQueue`、`IOGPUCommandDescriptor`：原版框架选择 Metal 作业并回收描述符，A540 后端提供实际硬件执行与缓存/页表退休。命令队列使用独立原版完成通道；1.169 扩充现有命令上传容量。动画已改善但仍卡顿，不能把继承接入成功当作目标完成，验证状态见文末。

这项收敛能减少自己维护的排队、依赖与生命周期代码。A540 的着色器、命令编码、页表、缓存、固件和电源控制仍需要高通后端实现。

## 样本与证据

分析日期为 2026 年 10 月 20 日。原版样本来自本地 `iPad_10.2_18.7.10_22H374_Restore.ipsw`，BuildManifest 标明 iOS 18.7.10、22H374，产品为 iPad7,11 和 iPad7,12，GPU 为 A10 的 AGXG9P。

IPSW 内的 `kernelcache.release.ipad7c` 与本地同名文件逐字节一致，SHA-256 为 `e66f26ecc0fd7e5d9f8afd5343c81d7603738553727768f9688ad28d3afef358`。解包后的 Mach-O SHA-256 为 `aa65c36c486a8b5cb1d7a2d565d1d6b3cc228b3c172f8c69c5da664ee19832cf`。驱动版本为 IOGPUFamily 104.6.4、AGXG9P 328.5。驱动的构建 SDK 标签 `iphoneos18.7.internal / 22H368` 与固件产品版本是不同字段。

34 个原版函数的反编译、关键比较指令及虚表原始数据保存在 [调度证据](../../build/gpu-scheduling-analysis/20261020/evidence.json)。当前源码的 SHA-256 和复用评估保存在 [源码审计](../../build/gpu-scheduling-analysis/20261020/reuse-audit.json)。未恢复符号的函数按行为命名，分析别名不代表 Apple 原始导出名。

以下结论针对该 A10 样本的主机驱动。固件内部的时间片、公平性、老化和抢占算法尚未恢复，不能据此推定后续 Apple GPU 的实现。

## 原版调度分层

```text
Metal 和 IOGPU.framework
        │ 命令参数、共享内存、通知 token
IOGPUCommandQueue
        │ segment 解析和厂商命令描述符
IOGPUWorkQueue + IOGPUCommandDescriptor
        │
IOGPUScheduler
        │ 依赖可提交性、优先级列表、提交协调、完成回收
AGX3DWorkQueue / AGXCLWorkQueue
        │ AGX 命令、固件通道和硬件状态
Apple GPU 固件与执行引擎
```

`AGXScheduler` 的 MetaClass 明确以 `IOGPUScheduler` 为父类；该样本中两者实例均为 552 字节。AGX 派生类的虚表保留通用实现，并覆盖部分硬件操作。因此，`IOGPUScheduler` 是 Family 层的通用框架；硬件命令、执行通道和复位语义通过后端接入。

### 提交和完成各有工作循环

`0xfffffff005f34398` 的初始化创建两个 `IOWorkLoop`，线程分别命名为 `IOGPU Submission` 和 `IOGPU Completion`，各有 command gate。初始化还创建完成事件源、硬件错误事件源、进度定时器和已注册命令队列表。

提交协调函数 `0xfffffff005f358dc` 也会在调用线程上尝试准备命令、处理背压并睡眠等待条件变化。因此，两个工作循环不等于所有提交准备都自动转到独立后台线程，也不等于提交路径永不阻塞。

### 可提交与已完成使用不同的事件进度

`IOGPUWorkQueue` 的普通分类路径 `0xfffffff005f32c7c` 查看队首描述符的依赖事件，调用 `0xfffffff005f26db8`，与 EventMachine 的每通道 **submitted stamp** 比较。

回收路径 `0xfffffff005f32a54` 调用另一函数 `0xfffffff005f258ec`，与实际的 **completed stamp** 比较；完成条件满足后才调用描述符回收钩子、释放引用并推进环形队列的已完成前缀。

| 进度 | 此样本中的存储 | 用途 |
| --- | --- | --- |
| issued | EventMachine `+64 + channel × 16 + 8` | 已分配的原生事件序号 |
| submitted | EventMachine `+64 + channel × 16 + 12` | 依赖是否已进入可继续提交的阶段 |
| completed | EventMachine `+32` 所指的每通道 stamp 指针表 | GPU 完成、事件满足与回收判断 |

描述符函数 `0xfffffff005f3f2c4` 会通过 `0xfffffff005f2562c` 发布 submitted 进度。这种设计允许依赖链继续向硬件提交，硬件后端仍必须正确实施相应等待。把 submitted 和 completed 合并，会改变调度与资源生命周期语义。

### 工作队列按优先级稳定插入

`0xfffffff005f35d30` 把工作队列重新分类到等待列表或就绪列表，并根据工作队列 `+104` 的优先级键升序插入。`0xfffffff005f35eb0` 的 `CMP` 后接 `B.LT`：只有新键更小时才插到已有队列之前，相同键继续向后遍历。因此，相同优先级在这次插入中保持先后顺序。

被排序的对象是工作队列，其内部描述符仍通过环形队列维护顺序。这不是把所有进程的 draw 混成一个可以任意越序的全局数组。原版也有队列满时的背压：`0xfffffff005f333e0` 转入 `0xfffffff005f33220`，等待已完成计数释放空间，并检查停滞超时。

### 优先级由 Family 策略传入 AGX

用户态 `IOGPUCommandQueueSetPriorityAndBackground` 位于 `0x214546278`，通过 selector 27 传入队列 ID、12 字节的前后台优先级结构，并取得实际应用的前台等级。

内核 `0xfffffff005f44a20` 接受前台数值 0、1、2、4、5，后台数值 2 或 3。请求 5 时检查 `com.apple.private.agx.rt-priority-spi`；无权限时降为 0，原版日志将两者分别称为 RealTime 和 High。

`0xfffffff005f4470c` 根据任务的 foreground 权限选用前台或后台值，并在有效等级改变时调用队列虚表槽 22。后台值 3 会折为有效值 2，同时记录队列错误 6；不宜把它直接当成普通低优先级值。

AGX 的槽 22 实现 `0xfffffff005f67c40` 再把等级传到所属工作队列与固件对象。其转换表位于 `0xfffffff00530f5d8`：

| Family 有效等级 | AGX 固件等级 |
| --- | --- |
| 0 | 0 |
| 1 | 2 |
| 2 | 3 |
| 4 | 1 |
| 5 | 0 |

等级 3 在这个转换入口无效。数值 4 的官方枚举名本次未恢复；这张表只证明转换关系，不能证明实时等级具有怎样的时间片或抢占保证。

后续补查 `0xfffffff005fc00d4`，它直接跳转 `0xfffffff005f32d10`，后者把第二个参数写入 WorkQueue `+104`。因此上表的转换也确实用于 Family 的队列排序键，并非只传给固件。证据在 [排序字段补查](../../build/setup-animation/hello-card/native-priority-key.json)。

### AGX 实施硬件提交

补查默认值：`IOGPUMetalCommandQueue initWithDevice:descriptor:`（`0x214545520`）将 `_priority` 初始化为 1、`_backgroundPriority` 初始化为 3，创建时没有额外调用 priority setter。AGXA10 派生初始化（`0x23565219c`）没有改成高优先级；仅在内部安装分支显式调用 `setGPUPriority:1`。当前 QcomMTL 则显式提交 `{0,2,0}`，使所有新队列默认请求 High，存在策略差异。用户态证据在 [默认队列参数](../../build/setup-animation/hello-card/native-metal-queue-defaults.json)。后台 3 的原生特殊语义仍需保留其上下文，不能只复制 getter 数值而假定它等同于低优先级。

`AGX3DWorkQueue` 的提交入口为 `0xfffffff005fbe8fc`，`AGXCLWorkQueue` 为 `0xfffffff005fbf208`。它们处理对应描述符并进入 AGX gate；共同提交函数 `0xfffffff005fbedbc` 更新固件队列状态，经通道函数表调用 AGX 固件接口。

完成工作循环中的 `0xfffffff005f35614` 遍历在途工作队列，调用完成检查并移除已经排空的队列。进度定时器 `0xfffffff005f37690` 检查实际进度，在超时恢复前再次检查命令是否已经 GPU 完成，以避免不必要的重启。

## 当前项目已经复用的部分

这里的“已复用”表示当前源码确实调用原版实现或派生原版类，不单凭接口命名判断。

| 组件 | 当前接入 | 可保留的原生职责 |
| --- | --- | --- |
| `IOGPU`、`IOGPUDevice`、`IOGPUTask`、namespace | 原生服务和 UserClient 基类调用；`QcomGPUTask` 调用原生初始化 | 任务归属、句柄命名空间和对象基础生命周期 |
| `IOGPUResource`、`IOGPUSysMemory`、`IOGPUDeviceShmem` | 原生资源工厂和 backing；厂商资源派生类补充 A540 映射 | 资源 ID、共享内存和 CPU 侧内存对象管理 |
| `IOGPUCommandQueue` 优先级策略 | selector 27 调用原生 Family，槽 22 发布有效等级 | 前后台选择与实时 entitlement 检查 |
| NotificationQueue、BlockFence、FenceMachine | 原生通知端口注册、提交接口和 fence 退休 | 向用户态交付原生 token、状态与通知 |
| IOSurface 与 IOFence | 原生任务授权查找和 access fence 函数 | 跨生产者和消费者的表面访问协调；该部分属于 IOSurface 层 |

主要入口见 [QcomGPUNative.cpp](../../drivers/gpu/QcomGPU/QcomGPUNative.cpp) 的 `createGPUTask`、`allocSystemMemory`、`allocResource`、`prepareSurfaceFences`、`completeNative`，以及 UserClient 对 `IOGPUDeviceUserClient::externalMethod` 的调用。

通知和 fence 已经使用原生实现，但 ticket 创建、stamp 预留、何时发布完成、A540 DMA 资源的保持与释放仍由厂商桥接代码决定。原生通知本身不能替代正确的完成源。

## 仍由项目自行维护的关键路径

1.165 及更早版本的 [QcomGPUNative.cpp](../../drivers/gpu/QcomGPU/QcomGPUNative.cpp) 明确写道：

```cpp
// The vendor descriptor hook submits directly to the A540 ring and binds a
// Family event fence. Apple's generic segment/work-queue encoder is not used.
OSObject *QcomGPU::allocWorkQueue() { return nullptr; }
```

当时 `QcomGPUCommandQueue::submitCommandBuffer` 调用 `QcomGPU::submitNative`，绕过原版 segment/work-queue 编码链。硬件作业的选择由 [QcomGPUHardware.cpp](../../drivers/gpu/QcomGPU/QcomGPUHardware.cpp) 的 `dispatchNextJob` 和 [AdrenoSubmit.h](../../drivers/gpu/QcomGPU/AdrenoSubmit.h) 的 `nextAdmission` 完成。

首次审计时 `priorityRank` 将 0 与 4 合为一档，1.162 实验还加入了 compositor 选择规则；1.162 已拒绝并回退。后续 1.164 候选按已核实的 WorkQueue 排序键区分 0/5、4、1、2，同等级保持稳定顺序，同队列 FIFO 和物理别名依赖不变；此处不声称已接入原厂 WorkQueue/Scheduler 的运行时。

当前 [NativeTimeline.h](../../drivers/gpu/QcomGPU/NativeTimeline.h) 自行维护单通道 stamp 序列；`prepareNativeCompletion`、`armNativeCompletion` 和 `completeNative` 桥接原生 fence。`QcomGPUEventMachine` 只配置一个通道，`supportsEvents()` 返回 false，尚无可直接启用的完整原生 GPU 等待编码。

用户态还存在另一层自有状态机：[QcomMTL.mm](../../drivers/gpu/QcomMTL/QcomMTL.mm) 中的 CommandQueue 和 CommandBuffer 继承项目的 `QcomMTLObject`，通过协议实现提交与回调；[MetalSync.h](../../drivers/gpu/QcomMTL/MetalSync.h) 管理自己的 fence point。这里与内核 IOGPUFamily 分属不同层，不能仅靠替换内核调度器消除这些状态。

## 后续复用顺序

### 首先接入原生工作队列和命令描述符

优先补齐 `IOGPUWorkQueue` 与 `IOGPUCommandDescriptor` 的私有 ABI、构造、锁和回收契约，为 A540 实现描述符准备、实际提交、完成检查和失败处理钩子，再让原生 Scheduler 接管等待、就绪和在途列表。

以下是首次审计时的适配要求；1.166 的实现和验证状态见本文末尾。仅让 `allocWorkQueue()` 返回一个对象不够；必须把原生依赖、submitted/completed 事件与 A540 硬件进度接通。原版软件队列和厂商选择器不应同时决定同一批作业的执行顺序。

可以先让原有的 fill/copy 或简单 compute 通过这条路径，核对同队列顺序、跨队列依赖、异步完成和队列满时的行为，再迁移 render。close、客户端死亡、错误和 GPU reset 都需要继续保持 DMA 资源到实际停止或完成。

### 随后接入资源列表和原生命令生命周期

`IOGPUSegmentResourceList`、`IOGPUResourceSet` 和原生描述符可以承接一部分资源保留、命令归属与退休工作，从而减少厂商 ticket 中重复维护的资源集合和引用关系。资源能否使用、页表如何建立、哪些访问冲突以及 A540 命令是否合法仍需后端判断。

这些对象与原生 segment/descriptor 路径关联，应随执行队列一起适配。不能仅删除当前资源表、配额或冲突检查，就认为 Family 已承接对应职责。

### 再评估用户态对象与原生事件

`IOGPU.framework` 的 CommandQueue、CommandBuffer、存储池与事件对象也值得评估。它们是用户态的另一套可复用实现，需要核对具体类的初始化和厂商覆盖方法，保留自己的 AIR 编译与 Adreno 编码器。原版 `IOGPUMetalCommandQueue` 的 `_submitCommandBuffers:count:` 只负责组装参数并调用 IOGPU 提交接口，不能自动生成 A540 命令。

原生 MTLFence/MTLEvent 接入还需要正确的事件等待与信号编码。当前 `supportsEvents()==false` 表明这部分应作为后续适配，不能用提前发布完成的方式替代。

### 最后收敛恢复协调和统计

Family 的进度检查、错误传播、已注册队列和统计框架可继续复用。A540 停止 DMA、CP 重启、固件装载、SMMU/TLB 退休及电源恢复仍由硬件后端完成；通用恢复协调需要调用这些钩子。

## 保留在高通后端的职责

| 职责 | 原因 |
| --- | --- |
| MSL/AIR 到 Adreno shader 的编译 | Apple GPU 机器码与 A540 ISA、寄存器和资源绑定不同 |
| PM4、绘制与计算状态、GMEM 和纹理布局 | 由 Adreno 的硬件协议决定 |
| GPU VA、SMMU、ASID、TLB 与缓存同步 | Family 的 CPU 内存对象不等于 A540 的设备页表实现 |
| IRQ、实际完成时间戳、固件、时钟和复位 | 必须来自 A540 的真实硬件状态 |

建议的目标结构是：让 Apple Family 管理任务、资源和通用命令生命周期，让 Qcom 后端提供经过验证的编译、内存映射、命令生成与硬件执行。调度复用主要减少排队和生命周期问题的维护面；像素错误、shader 错译与硬件状态错误仍应按各自路径定位。

Apple 的公开 [命令组织与执行模型](https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Cmd-Submiss/Cmd-Submiss.html) 可用于核对同队列顺序、Scheduled 与 Completed 的外部语义；本文的私有调度细节来自上述固定二进制，不由公开 API 文档推导。

## 1.166 原生运行时接入候选

按用户要求改为实际继承：`QcomGPUScheduler : IOGPUScheduler`、`QcomGPUWorkQueue : IOGPUWorkQueue`、`QcomGPUNativeSubmission : IOGPUCommandDescriptor`。ABI 以同一 iOS 18 / A10 provider 的实例大小、虚表槽与函数字节校验绑定。原版 IOGPU 初始化 Scheduler，Qcom 命令队列创建原版 WorkQueue；原生 append、准备、硬件提交及完成回收链已接通。

Metal 作业不再进入本地 `nextAdmission`。Family 的等待、就绪、在途列表负责选择和描述符退休，WorkQueue 的 submit 钩子调用 A540 PM4 后端。硬件进度、物理别名、IOSurface fence、页表与缓存退休仍保留在后端；单通道事件只发布已实际提交的连续前缀，完成仍由真实硬件退休推进。

`nativeAppend` 在退出 A540 gate 后调用，避免原生调度等待阻塞 IRQ 完成；prepare 与 submit 之间保留一个短暂硬件准入预约。故障会撤销预约，事件 epoch 重置还要求原生 WorkQueue 已回收。CPU 命令准备目前仍在已有提交入口完成，本候选不声称已迁移原生 segment 资源列表或用户态 Metal 队列。

Metal 1.46 将普通队列前台优先级默认值设为已核实的 1；AGX A10 普通队列没有默认强制最高优先级。后台值仍沿用当前已支持的 2。GPU 1.166、Metal 1.46 已编译和打包，原有 gpu-submit 检查通过；候选 13 真机 mixed 全部通过，但动画预热触发一次恢复；后续无 GPU 作业的显示完成记录已排除。1.167 补上原厂 `IOGPUScheduler` 资源可重试标记（0xfffffff005f358d0），并通过既有完成通知触发 prepare 重试。原厂内存 unprepare 在 0xfffffff005f20adc 调用此入口；仅调用完成 IRQ、未设置该标记时，只要还有在途队列，资源等待线程可能不会醒来。候选 14 正在上机。

1.167（候选 14）目标转场有持续 GPU 作业，零失败、零恢复、无 Function 事件，证明资源可重试通知修正生效；但动画仍卡，不能宣告任务完成。其 1100–2000 ms 内 UI 完成后的额外退休等待合计约 84.4 ms、最大约 40.15 ms。

1.168 将事件通道按命令队列分配（现有 8 客户端 × 8 队列，共 64 个槽，原版 EventMachine 支持最多 128；stampOffset 为 channel × 4）。队列释放后可复用通道，序号继续单调推进。硬件完成的作业可以独立退休，同队列仍 FIFO；物理别名、真实 GPU timestamp、页表与缓存完成条件保留。删除全局 submitted 前缀和自定义 didSubmit，恢复原版描述符提交记账。候选 15 已编译/打包，跨队列 fence 和目标转场验证待完成。

1.168 真机结果：原版 WorkQueue + Scheduler + CommandDescriptor::didSubmit 已运行；每队列 EventMachine 通道与原版完成状态判断配合，通过两队列实际 GPU readback/fence 测试。目标转场无失败/恢复，前台完成后等待最大 2.04ms。动画整体仍慢，不能把继承接入成功写成动画已修复。
