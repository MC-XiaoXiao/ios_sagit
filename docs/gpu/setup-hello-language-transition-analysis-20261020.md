# Setup Hello → 语言页：固件与驱动离线分析

日期：2026-10-20。仅读取本地固件、源码和此前保存的采集；本次没有连接设备、采集真机、刷写、修改驱动或修改固件。

## 结论与改动优先级

现有证据支持：短转场遇到了 GPU 重负载、CPU 提交准备和跨队列等待，导致可见更新十分稀疏。没有发现固件把这段转场限定成“一两帧”。

最快值得投入的方向是**公共提交路径和清屏路径**。只继续优化 Hello 笔迹或 Fluidity 最后的一个 fragment shader，无法解除其余开销。这里的优先级针对动画卡顿，不把所有界面功能故障归为同一原因。

1. **先收敛 CPU 索引校验成本和共享 gate 占用。**既有采集中，每组重绘制作业的 `prepare→admitted` 平均约 50.52 ms；这段准备发生在共享 GPU gate 内。1.155 会把索引复制到 GPU 的非缓存快照，再逐字节扫描快照。分析期间工作区已更新为 1.156，加入缓存暂存后校验、再复制至 GPU 快照，以及 PID/分阶段诊断字段。方向与此瓶颈吻合，但旧采集不能证明 1.156 的实际改善，不能把它当作本次已验证修复。
2. **下一项优先做全屏附件 clear 的快速路径。**在 29 组可关联到 Fluidity 的重复作业中，两笔 Transfer|Compute 的硬件区间平均合计 52.62 ms，后面的三次 draw 合计约 36.15 ms。前者约占该组 GPU 时间的 59%。它们的形态与当前独立 compute 清附件路径高度吻合，但日志没保存这两笔的具体 shader ID，所以 clear 身份仍是有源码支持的推断。建议先优化通用对齐清屏：二维索引、成对/向量存储，消除当前每个 word 的动态行号除法/取余；保留边界、padding、格式和同步语义。不必一开始重做完整 tile renderer。
3. **随后处理合成输出的排队延迟。**窗口内 compositor 作业准入后最长等待约 401 ms 才提交到硬件，而有些作业自身只需小于 1 ms。先缩短前两项的重工作，再评估调度公平性、已入 ring 的工作量、每队列 presentation barrier 和全局退休前缀的影响。直接加深队列可能只增加延迟；改变 fence 发布规则涉及正确性，优先级应在低风险的成本削减之后。
4. **再针对剩余 shader 和显示时序优化。**Fluidity 最后一个 fragment 的既有优化确实有效，但它不是总帧成本。仅由 88.77 ms 的重复 GPU 工作组估算，其吞吐量已经约为每秒 11.3 组，还未计入 compositor、CPU 与显示等待；这不是实测 FPS，也不是完整帧身份判定。目标应是整条可见输出链落入 33.3/16.7 ms 预算，而不是某一个 shader 排行下降。

## 证据范围与版本

固件分析对象：

- 原版 Setup：`build/display-runtime/setup-software/Setup.original`。
- 原版 dyld cache：`build/wifi/reference/dyld/dyld_shared_cache_arm64`。
- SpringBoard、CoverSheet、OnBoardingKit 的 Objective-C 元数据、反编译和 selector 解析结果位于 `build/setup-transition/20261020/`。

性能依据：

- `build/setup-animation/window-specialization/review.json` 明确记录候选 `window-fast-home-01`，GPU 1.155.0 / Metal 1.29.0，以及用户反馈 **“Still only one or two transition frames”**。
- 本次重新解析的是 `build/setup-animation/window-fast-home-01-data/` 中已有的 `5-jobs.txt`、`5-frames.txt`、`5-operations.txt`、`5-timeline.txt`。没有运行该目录的设备采集程序。
- 启动参数已有 `gpu-fbf=2 gpu-pipe=6 display-direct-scanout=1 gpu-fb2d=2 gpu-op-timing=1 cpus=4`。
- 最后一次源码核对：GPU **1.156.0**、Metal **1.29.0**、Display **1.0.23**。1.155 的对应源码快照在 `build/setup-animation/index-staging/source-before/`。源码版本和采集版本必须分开。

复算入口：`python3 build/setup-transition/20261020/analyze_offline.py`。结果 `offline-findings.json` 保存时间换算、分组、原始文件及源码 SHA-256。

## 固件实际执行了哪些动画

这里至少存在三个不同对象，不能用一个对象的性能替代整个转场。

| 对象 | 固件路径 | 分析结果 |
|---|---|---|
| 开机 Hello 覆盖层 | SpringBoard `SBDashBoardSetupController` / `SBDashBoardSetupViewController` | 清除 Hello 时调用 CoverSheet 的内容控制器 dismissal |
| 语言页头部插图 | Setup `BuddyLanguageController` → OnBoardingKit | 加载 `Language.ca`，通过 CA state controller 运行状态动画 |
| 全屏动态背景 | 既有日志里的 Fluidity / Mercury 渲染链 | 单独的 GPU producer；其重负载可以与 compositor 竞争，不能称为语言页头部动画 |

### Hello 退出

`-[SBDashBoardSetupController _clearSetupViewIfNecessaryAnimated:]`，地址 `0x1d29acd3c`，向 `mainPagePresentationViewController` 发送：

```text
dismissContentViewController:animated:completion:
```

CoverSheet 的 `-[CSPresentationViewController _updateContentViewControllersAnimated:completion:]`，`0x1d23704bc`，在 animated 分支构造 duration **0.4 秒**的 settings。默认 dismissal block 位于 `0x1d2371010`，使用 `BSUIAnimationFactory animateWithSettings:actions:completion:`。动作 block `0x1d2371220` 把 view 的 **alpha 设为 0**，并禁用交互。基类 `presentationTransition`，`0x1d2399bac`，返回 0，对应默认动画分支。

这证明默认 Hello 内容退出有连续淡出语义，不是只提交两张固定图片。实际转场还可能叠加 cover sheet、scene 和背景变化；不能把这个 0.4 秒当成所有组件的总时长。

另外存在 `SBUIStartupToUserSetupAppZoomInAnimationController` 的 1 秒启动动画，但尚未证明 HOME→语言页必经该路径，不以该类名字直接套用到本问题。

### 语言页

- `BuddyLanguageController viewDidLoad`：`0x10009881c`，构造 table view，并取得 `buddy_animationController:@"Language"`。
- `viewWillAppear:`：`0x1000989dc`，首次进入父控制器时准备语言数据、`reloadData`、布局。
- `_didBecomeActive`：`0x100098b30`，调用 `OBAnimationController startAnimation`。
- `OBWelcomeController buddy_animationController:`：`0x100112ec4`，创建两个状态：
  - `State 1 / Dark 1`：transitionDuration `0.01`，transitionSpeed `1.0`。
  - `State 2 / Dark 2`：transitionDuration `1.0`，transitionSpeed `0.5`。
- `bundleUrlForPackageName:`：`0x100112ff0`，从 main bundle 查找 `.ca` 资源。
- OnBoardingKit `_stepAnimationStatesForLayer:` 的 block `0x19b9413a0` 调用 `setState:ofLayer:transitionSpeed:`，并以 transitionDuration 安排下一步。

**两个状态是 CA 动画状态节点，不是两帧。**也不能只把这些 transitionDuration 相加，就宣称得到了 `.ca` 包内所有动画的精确总时长。此次没有解析 `Language.ca` 的全部 layer 内容。

## 既有采集中的卡顿证据

HOME 输入被已有轮询限定在约 **332.310～343.096 秒**之间，没有精确的动画启动标记。下面是该窗口内相邻 display completion 的间隔：

| 相对启动时间，秒 | 显示完成间隔 | 与区间重叠的 GPU 作业最大准入排队 |
|---|---:|---:|
| 338.461 → 339.161 | 700.14 ms | 62.79 ms |
| 339.161 → 340.512 | 1350.17 ms | 270.56 ms |
| 340.512 → 341.545 | 1033.47 ms | 401.20 ms |
| 341.545 → 342.262 | 716.87 ms | 303.57 ms |
| 342.262 → 342.545 | 283.28 ms | 273.83 ms |

339、340、341 三个自然秒各只有一次显示完成。相比 0.4 秒的默认淡出，这种更新间隔足以让中间动画状态不可见，产生“只有一两帧”的观感。

这不是精确逐帧归因：frame token 没有 scene/layer 身份，长间隔也可能含静态内容、暗屏或应用没有提交。本报告不把更晚约 5.93 秒的静止间隔算成转场卡顿。338.461～339.161 的前约 453.17 ms 没有记录到新的 prepare 且没有跨入作业，但 prepare 在进入共享 gate 后才计时，因此不能单凭此区间区分用户态停顿和 gate 入口等待。

### GPU 重复工作组

按非 compositor 作业的 prepare 顺序，在 338～343 秒内找到 29 组以下形态：

| 作业 | GPU 硬件区间均值 | CPU prepare→admitted 均值 |
|---|---:|---:|
| Transfer\|Compute，1 operation | 24.58 ms | 5.02 ms |
| Transfer\|Compute，1 operation | 28.04 ms | 7.44 ms |
| Render，3 operations | 36.15 ms | 50.52 ms |
| 组内合计 | **88.77 ms** | **62.98 ms** |

29 组最后一笔 Render 的 sequence 全部能关联到慢操作记录中的 `fluidity::fragmentShader`。三次 draw 与两次 SDF draw 加最后的全屏 fragment 对应；最后一个 fragment 既有统计约 28.34 ms，剩余时间还包括两个 SDF pass 等工作。

这里的“组”是日志序列分组，不是已证明的 command buffer 或显示帧。GPU 与 CPU 可以重叠，**不能把表中两列直接相加成端到端帧耗时**。GPU 硬件区间也包括作业内的等待/同步，不能称为纯 ALU 执行时间。

例如在 339.161～340.512 秒，prepare 落入该区间的非 compositor 作业有 24 笔 Compute 与 12 笔 Render，GPU 区间分别累计约 631.80、435.00 ms。到下一段 1.033 秒间隔，仍有 16 笔 Compute 和 8 笔 Render。GPU 一直忙，并非仅仅没有动画事件。

全量统计 native/compositor 分别完成 7450/6701 笔，failed 均为 0。正常完成只说明没有作业失败，不代表低延迟。核心频率统计约 596 MHz；没有证据把问题首先归为低频或全局 GPU 挂死。

### 为什么之前的 shader 排行容易漏掉约 53 ms

`QcomGPUNative.cpp` 的 scratch 分配起点是：

```cpp
UInt32 scratchBytes = header.count > 1 ? GPU::PageBytes : 0;
```

单条 compute 没有 draw framebuffer storage，也就通常没有该 scratch 页。后续 operation profiling 只有在 `job.scratch` 存在且至少一页时才记录，否则累加 `scratchSkips`。

本采集 `GPU_OPERATIONS` 的 `scratch_skips` 从 1967 增加至 2421。独立 job 级 GPU 时间仍可见，但这些作业缺少具体 operation/program 记录。**只看 fragment 的优化结果，会漏掉这部分真实 GPU 时间。**

当前 loadActionClear 的路径：

```text
QcomMTL render encoder
  → 每个 Clear 附件记录独立 QcomMTLOperation
  → fillRowsPlan / fillPairRowsPlan
  → blit_fill_rows / blit_fill_pairs
  → ShaderDispatch，单独 GPU job
```

相关代码：

- `drivers/gpu/QcomMTL/QcomMTL.mm:3453`：builtin blit dispatch。
- `drivers/gpu/QcomMTL/QcomMTL.mm:3496`：clear operation 转 compute plan。
- `drivers/gpu/QcomMTL/QcomMTL.mm:4664`：颜色 clear 打包与附件 clear 记录。
- `drivers/gpu/QcomMTL/Blit.metal:103`：pair fill，逐 word 计算行号与列号。
- `drivers/gpu/QcomMTL/QcomMTL.mm:2255`：memoryless 在此实现中使用队列私有 backing，并非原生 tile 内零成本附件。

既有 Mercury 分析也记录了 1080×1920 的 BGRA8/RG32Float 附件与 clear pass。这增加了“两笔 compute 是附件清屏”的可信度，但不能代替缺失的逐笔 program ID。

## 驱动如何放大延迟

### CPU 提交准备

1.155 的 `submitNativeGated` 在共享 gate 下做快照、索引校验、PM4 编码、地址空间和 fence 准备。索引从资源复制到 `job.indices` 后，`GPU::Shader::indexFootprint` 逐 index、逐字节读取它。后者来自 GPU 非缓存快照；大的 SDF indexed draw 比 compositor 的小矩形更易暴露代价。

已有较晚的 native 汇总记录中，`index_snapshot` 最大约 25.13 ms、encoding 最大约 101.22 ms，但它们是累计汇总，不能证明二者就是同一笔 HOME 作业，也不能把最大值相加。HOME job 记录 v1 没有 PID 和阶段细分，50.52 ms 的组成尚不能全归为索引扫描。

工作区的 1.156 已添加 cached `OSData` 暂存供校验和编码读取，再将相同字节复制到 GPU snapshot；并增加 `preflight/indexAcquire/indexSnapshot/encoding/addressSpace/fence` 与 PID。该变更并未消除 GPU clear 和 fragment 时间。

### 排队与退休

- `QcomGPUHardware.cpp::dispatchNextJob` 会依据 ready、priority、surface access 和资源依赖选择作业。存在优先级选择，不是完全无区分 FIFO 执行；但是已进入 ring 的重工作不会因后来 compositor 到达而自动消失。
- `QcomGPUHardware.cpp::pollHardware` 明确保留**全局 admission 前缀退休**：即便 GPU 执行可按优先级变化，共用 native stamp timeline 的 fence、IOSurface 发布与资源释放仍等前缀完成。
- `QcomMTLQueue::drainThrough/finishRetirements` 中，带 presentation 的 command buffer 进入 retirement 后设置 `_presentationBarrier`，直到真实退休才允许本队列继续。该机制可能进一步限制产出节奏；现有日志未单独量化它在这次转场中的比例。
- Mercury 直接 drawable 路径的 `commit` 可同步推进至完整 Scheduled。这是为确保原版随后直接 present 能看到 producer fence，并不是无条件等 GPU Completed；但 CPU 准入/排队仍可阻塞事务线程。

这些机制解释了重 producer 不仅自己慢，还能影响轻量合成输出。应先减小重作业和 gate 占用，再评估更复杂的调度及退休规则。

### 显示

`QcomDisplay::swap_begin_gated` 先处理 pending/coalescing，只有 `ChangedLayers == ReadyLayers` 才能进入实际显示。生产者或合成结果尚未 ready 时不能正常扫描新内容。后续依赖真实 TE/VBL、PP_DONE；Display 1.0.23 在真实完成后退休 display read access。

原生 pending 队列还会处理 redundant/completed/superseded swaps。在生产和显示速度落后时，用户未必能看到每个中间状态；本次缺少对应时间段的完整 display event ring，不能报出实际合并了多少帧。

保存的最后一个 `5-timeline` 只覆盖 **347.132～364.151 秒**，晚于上述主要卡顿段。所以不能拿该段正常 DPU 事件排除 338～343 秒的所有 fence 或显示等待。历史约 33 ms 的显示节奏也只能解释基线约 30 Hz，不能单独解释本次 0.7～1.35 秒间隔。

## 不应沿用的旧判断

- **“每次都等 GPU 完成后才能 present”**：当前 native IOSurface 路径在整份命令准入、Scheduled 后已提交 presentation，由 producer fence 保护；保留的是退休和队列 barrier。
- **“索引绘制一律 drain 全队列”**：相关等待有 `!_pipelinedAdmission` 条件；采集启用了 pipe6。索引 CPU 校验的代价仍存在，是不同问题。
- **“还在逐三角形拷贝整个 framebuffer”**：本次相关记录 snapshots/copy 为 0，整体 copy_bytes 亦为 0；不能把已绕过的旧瓶颈继续列为本次主因。
- **“都怪 shader 首次编译”**：当前已有进程内和持久缓存。冷编译历史上确实造成过长延迟，但这个 HOME 窗口没有匹配的 Metal 编译计时或栈，不能确认为此次首因。
- **“HOME 驱动 QSEE 阻塞了整个图形流程”**：FingerprintHID 的 secureWorker 在 nativeHIDgate 外调用 QSEE。`HomeMaxSecureUS≈506 ms` 是累计最大值，输入采集前就已存在，不能作为这次转场停顿的精确时间证据。

## 后续的离线工作边界

最小有效优化单元应围绕上述一、二项：检查 cached index 校验的数据一致性与边界；设计对齐 clear 的二维/向量实现并保留回退；补齐单条 compute 的离线可审查诊断覆盖。它们都可以先做源码分析与主机侧语义验证。

一次真实转场的端到端改善仍需最终由同版本、同窗口的输入/scene、CPU 阶段、GPU job 和 display completion 对齐来确认。本次遵守“不进行真机测试”，不宣称验证了新版本帧率或唯一根因。
