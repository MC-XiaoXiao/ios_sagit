# Fluidity 全屏耗时与长显示间隔：离线修复候选 1.52.5

2026-10-22。已完成源代码修改、主机语义验证、iOS arm64 交叉编译和完整 Metal 候选打包。**本轮没有连接、运行、调试或部署到真机。新版本的 GPU 时间、显示间隔和像素结果尚未实测，650 ms 长显示间隔的上游原因仍未闭环。**

可审查候选位于 [final](../../build/fluidity-offline-20261022/final/)，核心差异见 [source-review.patch](../../build/fluidity-offline-20261022/final/source-review.patch)，机器可读结果见 [result.json](../../build/fluidity-offline-20261022/result.json)。原有启动产物和 generated/GPU 的 23 个文件已核对 SHA-256，内容保持原样。

## 证据与定位

项目已经接入 iOS 18 原生 IOGPU Scheduler、WorkQueue、CommandDescriptor 和真实完成 fence。固定原厂参照为 iOS 18.7.10 / 22H374，A10 AGXG9P 328.5、IOGPUFamily 104.6.4。这里继续修正 A540 后端和 Metal 提交语义。

历史捕获 `hello-card-44-transition-data` 中实际全屏热点是 Fluidity 常量组合 **100**（isPhone=true，其余 false）的四常量窗口变体，旧程序 hash 为 `a7aba4cb1419480f`，6 顶点、1080×1920、window_mask=0xf、800 code words、full6/half23。25 次操作的 GPU 时间中位数为 **28.3396 ms**。该捕获没有验证整套运行 payload 的身份；这里用操作 hash 和元数据确认着色器，不能据此确认所有部署文件的版本。

既有计数显示 SP/TP busy 各约 67.49M，ALU 18.70M，EFU 3.11M；TP L1 miss 约 5.53%，UCHE 主存等待较少。这些计数存在重叠，不能相加换算收益。热点已经使用 `sam (f16)`，八次读取使用不同坐标，不能通过删采样或再次宣称“改成半精度”解释优化。

清屏和 GMEM 的既有结果见 [附件清屏记录](ios18-native-attachment-clear.md)：CP_BLIT 清屏约 2.23 ms；27 tiles 的 GMEM 重放曾更慢，约 45.95 ms。本候选沿用当前 BYPASS 默认策略。

本轮对保存日志重新计算，脚本为 [analyze-saved-trace.py](../../build/fluidity-offline-20261022/analyze-saved-trace.py)，结果为 [saved-trace-analysis.json](../../build/fluidity-offline-20261022/saved-trace-analysis.json)。

| 历史区间 / 指标 | 结果 | 含义 |
| --- | ---: | --- |
| Home+466.981 → 1117.058 ms 的显示间隔 | 650.077 ms | 显示完成记录之间的间隔 |
| 其中 prepare → retire 区间并集 | 60.695 ms | 已记录作业的保守 CPU 覆盖范围 |
| 其中 submitted → observed 区间并集 | 50.451 ms | 包含观察延迟，不能当作精确 GPU busy |
| 其中 Commit / QueueRun 并集 | 33.330 / 42.048 ms | 两者嵌套、重叠，不能相加 |
| 最长相邻 prepare 起点间隔 | 631.102 ms | 单独缺少 prepare 记录不能证明 GPU 完全空闲 |
| Home 后 3 秒内按 retire_end 选取的作业 | 143，失败 0 | 与按其他端点统计的 144 项可有边界差异 |

另一次 Home+614 ms 左右的[保存栈采样](../../build/setup-animation/hello-card/gap-41-stacks-summary.txt)位于 SpringBoard 的 `SBCoverSheetPresentationManager::_prepareForDismissalTransition` → `UIWindow setWindowLevel` → `_UISceneLayerHostContainerView::_rebuildLayersForNormalPresentationWithReason:withFence:`。这是继续检查场景切换与 fence 的线索；单次栈采样不能解释整个 650 ms。

## 修改内容

[Interface.h](../../tools/gpu-compiler/Interface.h) 在两条前端及 AOT 共用的采样映射中，对满足以下条件的 literal sampler 将隐式 `tex` 改为 `txl(0)`：2D 或 2D array、非 shadow、min/mag 过滤相同、mip filter 为 none、anisotropy=1，并且只有原坐标及可选 offset 源。外部可变 sampler、cube、比较采样、显式 LOD、bias、gradient 和依赖不同 min/mag 的情况保持原路径。

八次读取、坐标、offset、RGBA16Unorm 输入、输出精度及原硬件 sampler 槽位、LOD clamp 均保留。采样不再需要推导隐式 LOD 后，调用 Mesa 的分析重新计算 `needs_quad_helper_invocations`，仅更新该派生标记，保留已降低的 IO 和资源元数据；shader 里其他导数或 quad 操作仍会保留辅助线程需求。[Backend.c](../../tools/gpu-compiler/Backend.c) 新增 18 组资格、拒绝和导数保留检查。

| 实际热点 full-window 变体 | 修改前 | 候选 |
| --- | --- | --- |
| 采样 ISA | 8× `sam (f16)` | 8× `saml (f16)` |
| need_pixlod | true | false |
| code words | 800 | 800 |
| full / half registers | 6 / 23 | 6 / 23 |
| program hash | a7aba4cb1419480f | 201610c3ea3c3ca8 |

base、MSAA、window、MSAA-window 的完整记录及反汇编位于 [hot-machine-comparison.json](../../build/fluidity-offline-20261022/hot-machine-comparison.json) 和 [machine-code](../../build/fluidity-offline-20261022/machine-code/)。这些结果证明发出的指令和标记发生变化，不能据此承诺毫秒数或帧率。

[QcomMTL.mm](../../drivers/gpu/QcomMTL/QcomMTL.mm) 修正 `commitAndWaitUntilSubmitted`：前序 enqueue 尚未 commit 时返回 NO；可执行时使用普通 FIFO executor，只处理到当前 buffer；删除私有 helper 的推测性 marker 提交与最长 3 秒容量等待。私有 BOOL 路径也绕过 public commit 为 Mercury 立即 raw present 设置的专用等待。正常 public commit 的 drawable admission 仍走原路径。

原厂依据来自[已保存的 Metal 反编译](../../build/setup-animation/hello-card/native-metal-submission.json)：`_MTLCommandBuffer::commitAndWaitUntilSubmitted` @0x182420d30 调用提交函数；`_MTLCommandQueue::_submitAvailableCommandBuffers` @0x18242106c 只取已 commit 的队列前缀，没有可提交前缀时返回 false。若私有 helper 占据 QuartzCore 的串行提交目标，等待调用者稍后才会 commit 的前序会阻止进展。返回 NO 后由 Scheduled 回调执行资源清理。

YES 仍要求完整真实操作列表已 Scheduled，完成仍依赖真实 end token；可用操作的编码和正常 transport 调用仍可能花费 CPU 时间，这不是对 helper 耗时上限的保证。[metalctl.mm](../../tools/gpuctl/metalctl.mm) 增加同串行目标上的“前序只 enqueue、后序先请求提交、再 commit 前序”回归，并检查真实写回的 FIFO 结果；本轮仅编译了该程序。

## 离线验证与产物一致性

- 主机编译库和 iOS arm64 编译库均重新编译两份 adapter translation unit，链接固定 Mesa 静态库；iOS 的 171 个 LLVM C 导入已与 SDK 核对。命令和依赖 SHA-256 见 [build-receipt.json](../../build/fluidity-offline-20261022/build-receipt.json)。完整 Metal 和 metalctl 使用 iOS 18.6 SDK 交叉链接，版本为 1.52.5。
- 修改前 1.52.4 与候选分别编译并捕获最终 NIR，执行 Compute、Welcome、Fluidity 全八种常量组合及四种变体、常量窗口与短 buffer、1/2/4 samples。**3,095 组 quad，两条路径完整颜色输出及全部 compute buffers，共 16,770,508 字节逐位一致**。两边被解释的编译结果又分别与其实际主机 JIT 库逐字节核对。输出 +0.25 的负控能检测差异。见 [semantic-comparison.json](../../build/fluidity-offline-20261022/semantic-comparison.json)。
- 候选另通过 54 个功能条目 / 3,456 quads 和 16 个整数、控制流条目 / 4,096 quads；NIR validator、内存边界及 18 组采样检查均通过。这是 CPU NIR 解释，不是 A540 ISA 或纹理硬件仿真。
- 完整 `gpu_compiler_test`、`gpu-submit`（包含 MetalSync）、`gpu-constant-window`（8,224 组边界/对齐检查）、`metal-retirement` 主机回归通过。见 [host-validation.json](../../build/fluidity-offline-20261022/host-validation.json)。8 项缓存身份测试和 3 项相关集成测试通过；集成使用独立路径视图和已经通过的完整编译器结果。
- 初次整套集成检查还触及未重建的历史 Kext / gpuctl：默认 Kext 是 1.158、当前内核源码为 1.189，旧 gpuctl 的输入 hash 也对应历史 toolchain。初次结果保存在 `integration-initial.log/json`。本轮没有将这些旧产物声明为新版本；最终相关集成范围为 Metal 候选、原生 ABI 模型和硬件参照。
- AOT 的 144 个条目、系统目录的 200 个反射条目及 148 个精确 profile 均按新编译器重新生成。所有目录 image 通过独立解码、JIT 结构校验和逐字节 roundtrip；27 个历史 miss 通过不链接 shader compiler 的精确查找与恢复。通用合法化影响了 37 个 profile（包括全部八个 Fluidity profile），另外 111 个只变更 registry；目录中其他原生 shader 没有逐一进行像素执行验证。见 [catalog-comparison.json](../../build/fluidity-offline-20261022/catalog-comparison.json)。
- AOT registry 更新为 `5410fe4884682433`；archive context 更新为 `ec2aa4684cd9ae4c85baa043793fd4c8ef7419fcb0d1f0f0c68c4bae0e6dc79c`。构建 context 已重新计算核对；仅撤销提交实现修改不会改变 shader context。kernel shader ABI=44、JIT ABI=39、schema=2 保持原有机制，运行时仍添加 OS build。新库、AOT、目录和 context 必须作为匹配的一组使用。

## 尚未完成的验证

没有运行 iOS 上新增的队列回归，也没有测量候选的 GPU 操作时间、真实纹理输出或显示间隔。全屏采样的有效性与 helper 的进展仍需要后续设备验证才能形成性能结论；本轮按用户要求停留在离线构建与检查。

历史长间隔还包含大量 GPU 作业区间之外的时间，需要把场景重建、CA transaction、提交与 presentation fence 的连续区间对应起来。现有证据支持本轮两项具体修正，但不足以宣布 Fluidity 动画和 650 ms 长帧已全部解决。
