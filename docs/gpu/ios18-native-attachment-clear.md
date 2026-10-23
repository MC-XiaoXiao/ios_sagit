# iOS 18 / A10 附件清屏与 A540 的对应缺失

2026-10-21。范围仅为 Setup Hello 卡片上移转场的清屏、附件读写；以下原厂结论来自静态分析；之后进行了范围限定的硬件清屏候选验证，见文末记录。目标动画的最后通过版本仍为 GPU 1.177 / Metal 1.50，动画仍卡顿。

## 结论

原版 A10 驱动把 clear 值、load/store 动作保留为 render pass 的附件状态，配置 BackgroundObject（BGO）和 EndOfTile（EOT）程序，在渲染开始/结束时处理附件。Apple 官方说明：clear 与后续绘制在片上 tile memory 内进行，最后才按 store action 写回；独立全屏清屏会增加本可避免的系统内存流量。[Apple WWDC20，load/store 与 clear 说明](https://developer.apple.com/videos/play/wwdc2020/10632/)

QcomMTL 已复用系统 Metal descriptor、IOGPU 调度和 CoreFSCache，但当前渲染后端没有相应的 GMEM render-pass 路径：`loadActionClear` 在编码器初始化时变为全屏 compute fill，A540 绘制处于 BYPASS 模式。合并 native 作业减少了提交开销，却没有合并为片上附件生命周期。这是实际渲染实现的缺口，不能仅靠继续调整 IOGPU 调度或 shader 缓存解决。

最小的下一步候选是 A540 专用硬件 fast clear，先降低现有全屏 compute fill 的执行成本；完整 tile clear/draw/store 需要后续保留 pass 信息并实现 GMEM，不能只翻转一个寄存器。

## 原版二进制证据

来源：iOS 18.7.10 / 22H374 arm64，见 [提取记录](../../build/drivers/metal-plugin-analysis/extraction.json)。分析的是从 dyld cache 重建的分析 Mach-O，不是可直接部署的独立 bundle。原始输入与证据摘要见 [receipt.json](../../build/setup-animation/hello-card/native-clear/receipt.json)。

| 环节 | 原版函数与地址 | 已直接确认的处理 |
|---|---|---|
| 读取 API 附件 | Metal `MTLRenderPassColorAttachmentDescriptorInternal::_descriptorPrivate`，`0x18247294c` | 返回内部附件结构；`loadAction` 在 +32，`storeAction` 在 +40，`clearColor` 在 +56。已与 Metal IDB 的结构类型及 getter 对照。 |
| 整理附件状态 | AGX `FramebufferDriverConfigGen2(...MTLRenderPassDescriptor...)`，`0x2356916b4` | 逐附件读取上述结构，分别建立 load、clear、store、resolve 掩码；按格式处理 clear 值。clear 值保存在 driver config 中。 |
| 准备起始程序 | AGX `FramebufferGen2` 构造函数内，调用点 `0x2356953ec` | 调用 `findOrCreateBackgroundObjectProgramVariant`，将结果保存到 framebuffer。 |
| 准备结束程序 | 同一构造函数，调用点 `0x235694c7c`、`0x235694e60` | 调用 `populateEndOfTileProgramVariants`；不是为每个像素在 CPU 上产生独立命令。 |
| 开始 render pass | AGX `RenderContext::beginRenderPass`，`0x235700ab0` | 获取 BGO 变体，发出 BGO 参数；同时发出 EOT 参数，将程序/参数地址写入 render command。 |
| store 状态收敛 | AGX `FramebufferGen2::finalizeStoreActions`，`0x2356962a0` | 按最终附件动作合并掩码、更新条件和编码状态。 |

颜色附件掩码可进一步核对：driver config +1384 对应 `loadAction == 2`（Clear），+1388 对应 `loadAction == 1`（Load）。该判断来自比较常量和 Metal 私有结构布局，不是仅凭符号名推测。+1380 对应 store DontCare，+1392 对应 Store/StoreAndMultisampleResolve；其他 resolve/unknown 动作另有字段。

BGO 是驱动内部的附件初始化/加载程序，不能与 Setup 的 Fluidity 背景绘制混为一谈。也不能把这条路径说成“完全不用 shader”：原厂确实使用设备相关的程序变体和参数编码；节省的关键是片上附件处理及避免中间内存往返。

保存了完整的小函数反编译和大构造函数的调用点汇编摘录；被工具截断的大函数输出已删除，不作为完整证据。资料位于 [native-clear](../../build/setup-animation/hello-card/native-clear/)。没有复制固件大文件。

## 与当前实现逐项对照

| 行为 | 当前代码 | 差别及影响 |
|---|---|---|
| API load/store | [QcomMTL.mm](../../drivers/gpu/QcomMTL/QcomMTL.mm) `QcomMTLRenderEncoder::initWithBuffer:descriptor:rejection:` | 使用系统 descriptor；但 clear 在初始化时立即记录为独立 compute 操作，后续提交不再保有完整 pass 附件 load/store 状态。 |
| 清屏 | 同函数 `fillRowsPlan` / `fillPairRowsPlan` | 使用冻结参数和原有边界校验，直接写系统内存。内部 clear 可与 draw 合批，但仍实际执行全屏 fill。 |
| 普通绘制 | [AdrenoPipeline.h](../../drivers/gpu/QcomGPU/AdrenoPipeline.h) `restore()` | `CP_SET_RENDER_MODE` 选择 BYPASS，`RB_CNTL` 也开启 BYPASS，附件指向系统内存。没有 GMEM tile 初始化、逐 tile 绘制和 store。 |
| memoryless | QcomMTL `newPassBackingForQueue:` | 当前使用内部 pass backing 实现生命周期与可见性，尚没有获得只驻留片上存储的性能收益。 |
| framebuffer 输入 | `framebufferReadOnlyEligible` / `framebufferStoragePlan` | Fluidity 读取 attachment1、写 attachment0，当前用内存纹理视图及同步。切换 GMEM 时必须同时处理该输入，不能让 shader 继续读取尚未写回的旧内存。 |

29 的无编译目标采集已经区分出两笔清屏：

| 操作 | 参数 | 目标窗口样本数 |
|---|---|---|
| rows fill | 行字节数 4320，高 1920，pitch 4352，word 值 4278190080（不透明黑） | 27 |
| pairs fill | 行字节数 8640，高 1920，pitch 8640，两个 word 均为 0 | 27 |

两笔清屏合计约 12.75 ms，后续三笔 draw 约 36.45 ms，其中 Fluidity 主片元约 28.34 ms。它们是重复 GPU 工作成本，不能直接相加当成整段动画的延迟或光学帧率。即使清屏变快，也不能据此保证消除全部卡顿。原始数据：[1-operations.txt](../../build/setup-animation/hello-card-29-hot-transition-data/1-operations.txt)、[comparison-29.json](../../build/setup-animation/hello-card/comparison-29.json)。

此前 Metal 1.34 的普通三角形清屏，两笔合计约 27.5 ms，比向量 compute 更慢；这不等于已经验证过 A540 硬件 FAST_CLEAR。

## A540 可落地的范围

本地固定版本 Mesa Freedreno 提供两个可直接核对的参考：

- [fd5_draw.c](../../build/gpu-compiler/mesa/src/gallium/drivers/freedreno/a5xx/fd5_draw.c) `fd5_clear`：按颜色格式打包值，设置 `RB_BLIT_CNTL`、`RB_CLEAR_CNTL.FAST_CLEAR`、`RB_CLEAR_COLOR_DW0..3`，通过 BLIT event 执行；结束后关闭 FAST_CLEAR。
- 同文件 `fd5_clear_lrz`：在线性 R16_UNORM 目标、BYPASS 模式中也使用硬件 fast clear。这证明 fast clear 不应简单等同于“必须先实现完整 GMEM”，但并未替本项目证明所有颜色格式、pitch、边缘尺寸都可直接照搬。
- [fd5_gmem.c](../../build/gpu-compiler/mesa/src/gallium/drivers/freedreno/a5xx/fd5_gmem.c) `fd5_emit_tile_mem2gmem`、`fd5_emit_tile_renderprep`、`fd5_emit_tile_gmem2mem`：对应 restore、片上 draw、按附件需要写回；[freedreno_gmem.c](../../build/gpu-compiler/mesa/src/gallium/drivers/freedreno/freedreno_gmem.c) 管理 tile 循环，clear 覆盖的内容可以免除 restore。

建议先做一个范围明确的硬件 clear 候选：只替换已经在内核核对并冻结参数的内置 rows/pairs fill；直接复用现有资源解析、物理别名检查、IOGPU 准入、缓存可见性及真实退休。先覆盖上述两种实际全屏布局，非适用布局保留 compute。不要识别进程名或把某个 shader hash 当作跳过 clear 的许可。

必要验证只需现有清屏像素/行填充/保护区检查，以及一次预热后的 Hello 目标转场；需确认后续 draw 能读取正确结果。通过后再判断是否值得实现 GMEM。完整 GMEM 的最小边界是同一 pass 内 `clear → draw → store`，包括 attachment1 输入的正确性；不宜先建立通用 pass 合并器、重新实现调度器或跨帧猜测附件内容。

这里可以复用系统的 Metal 描述符、IOGPU 调度及现有缓存机制，AGX 的 G9 BGO/EOT 二进制、PDS 编码和硬件命令则不能直接给 A540 执行。当前已接入的框架保留，硬件清屏与 GMEM 后端由 QcomGPU 实现。

## 协作状态

冷编译任务已交付 [Metal 1.51 冻结清单](../../build/gpu-cold-compile-1.51/handoff.json)，要求 GPU registry `94ff5d0137498ac2`；已在后续候选 1.183 中配套部署。已同步本次渲染缺口，与冷编译独立处理。本文没有把预编译目录的离线收益当作热动画的真机收益。

## 硬件清屏候选验证（2026-10-21）

在现有内核 builtin 校验和参数冻结之后，用硬件命令替换适用的 rows/pairs fill，未跳过资源范围、物理别名、IOGPU 或真实完成检查。配套使用冷编译任务冻结的 Metal 1.51；设备信息查询通过，但没有据此宣称编译性能改善。

- GPU 1.178（hello-card-30）使用 `RB_CLEAR_CNTL.FAST_CLEAR` / BLIT event，在首个适用的 1040 字节 × 17 行、pitch 1088 布局失败。停止后续附件读取和目标转场验证。
- GPU 1.179（hello-card-31）改为参考实现的 BLIT 发出顺序，以正常颜色 flush 确认完成，仍在同一处失败。完整 registry 给出 `GPUMapperFaultReason=context-interrupt`、FSR `0x80000402`、FAR `0x10875100`、syndrome `19`。精简 `gpuctl failures` 不包含 mapper 故障项，不能据其缺失推断没有 SMMU 故障。证据：[clear.txt](../../build/setup-animation/hello-card-31-clear-data/clear.txt)、[registry.txt](../../build/setup-animation/hello-card-31-failure-data/registry.txt)。

这排除了“只修 BLIT 完成等待即可”的假设；尚未精确证明 FAST_CLEAR 的实际访问范围。不能扩大映射、放松保护区检查或把 Linux LRZ 示例直接等同于本项目任意颜色布局支持。两版均不作为可用优化保留。下一候选使用现有线性 2D 管线的 `CP_BLIT` solid fill，验证同样的任意 bit pattern、padding 与前后保护区；本段不预设该候选成功。

后续 2D 候选 1.180/1.181 也在该布局发生 SMMU 翻译故障。1.181 的临时地址记录把目标 `0x11398040`、长度 `0x4810`、scratch `0x109dc000` 与 FAR `0x11798480` 对上：FAR 是目标 + `0x400440`，即 4 MiB + 1088 字节。该差值对应第二行错误行距，不是正常的边缘访问。当前 `RB_2D_DST_SIZE` 用只按 64 字节对齐的 `pitch*height` 作为层间距，奇数个 64 字节单元会使打包寄存器 bit16 置位；待验证修正为 128 字节对齐。这个证据针对 CP_BLIT，不足以宣称 FAST_CLEAR 同因或硬件不支持。证据：[1.181 registry](../../build/setup-animation/hello-card-33-clear-data/registry.txt)。

### 已验证的 2D 修正与 FAST_CLEAR 适用范围

GPU 1.182 将 2D 寄存器层间距按 128 字节对齐后，同一 1040 × 17、pitch 1088 布局通过，并通过两种 Hello 全屏布局：合计 12 项、99,854,528 字节逐字节检查，行填充及首尾保护区完全一致；CPU 图案输入和同 command buffer GPU clear 后的 attachment1 读取共 3 阶段通过。native/compositor ticket failures 均为 0，无 mapper 故障。启动/诊断并行期间仍有一条 backboardd stage6 提交超时，不能称整个启动无失败；随后使用无诊断负载的目标转场启动单独核对。证据：[清屏](../../build/setup-animation/hello-card-34-clear-data/clear.txt)、[附件读取](../../build/setup-animation/hello-card-34-clear-data/feedback.txt)、[registry](../../build/setup-animation/hello-card-34-clear-data/registry.txt)。

这证实了 CP_BLIT 的 packed pitch 编码问题。只调整寄存器层间距，不扩大映射，也不改变可写矩形。临时目标地址属性在 1.183 中移除。

另外，[Mesa fd5_layout.c](../../build/gpu-compiler/mesa/src/freedreno/fdl/fd5_layout.c) 明确说明 mem↔GMEM blit 的 32×32 访问粒度；线性末级高度被补齐到 32，pitch 和层分配也按布局规则对齐。将相同 BLIT/FAST_CLEAR 机制直接用于仅授权 17 行、必须保留 padding/guards 的 builtin buffer fill，缺少这样的布局前提。此项是对原接入适用范围的否定，并非证明 A540 不支持 FAST_CLEAR；1.179 尚无目标 VA 对照，不能把它的每个越界细节当作已精确定位。当前保留能通过精确矩形验证的 CP_BLIT solid fill，FAST_CLEAR 若后续启用，必须先证明块访问范围属于目标自身。

### 1.183 目标采集的限制

去掉临时地址日志后，hello-card-35 记录到 37 个目标窗口作业，无失败 ticket、无新 Function 事件，但此前每帧出现的两笔后台 clear 与三笔后台 draw 均未出现。该样本不能用于证明清屏提速或动画改善；其 HOME 唤醒后首个状态样本已是 display=1，仍需核实目标画面与自动熄屏过程。

35 存在一条早于目标 HOME 165.929 秒的 backboardd CommandBuffer timeout。重新核对 29 原始记录，基线同样已有一条相同错误特征，早于 HOME 167.063 秒；此前“零 GPU 失败”仅指 GPU ticket，不能扩展成零 Metal failure。冷编译任务的[只读核查](../../build/gpu-cold-compile-1.51/startup-timeout-review-35.md)确认 35 超时前尚未命中新接入的预编译目录，且相关提交与退休实现未变化。现有证据不足以归因于 Metal 1.51，也不足以断言两个超时根因相同。

补充：随后取回 35 的 idle 文件，75 秒内 719 个采样全部为 display=1、power=1，证明该轮未按预期先熄屏。因此原先的目标窗口不满足 Hello 唤醒前提，后台组缺失不能用于认定清屏或 Metal 1.51 使渲染停止。后续应先确认实际熄屏，再 HOME 唤醒与采集。证据：[idle.txt](../../build/setup-animation/hello-card-35-idle-data/idle.txt)。

### 确认熄屏后的有效目标采集

用户人工确认当前为 Hello，随后确认已熄屏。`hello-card-35-confirmed2-transition-data/before.txt` 四个 display/power 当前与目标值均为 0；HOME 唤醒、稳定 6 秒，再 HOME 采集。144 个目标窗口作业无失败，Metal failure 仍是原来的 1 条，没有新增。用户反馈：**进一步改善，但仍卡**。

同形状的两笔后台清屏作业，29 基线 27 组中位 12.746 ms，35 新采集 24 组中位 **2.273 ms**，约减少 82.2%。RGBA16F 对应 pairs fill 的单操作中位由 8.916 ms 降到 1.484 ms，记录明确为 blit_kind=8；另一笔已低于慢操作记录阈值，不将缺项当作零耗时。三笔后台 draw 中位 36.462 ms（原 36.453），其中主 Fluidity 片元 28.341 ms。结果证明硬件清屏减少了该项 GPU 工作，不能作为整段动画帧率改善百分比。

目标窗口还有一笔 3.917 ms 的 Function，detail=c00，需按事件类型区分目录命中与编译，不使用“零 Function”描述。[比较结果](../../build/setup-animation/hello-card/comparison-35-confirmed2.json)、[作业](../../build/setup-animation/hello-card-35-confirmed2-transition-data/1-jobs.txt)、[操作](../../build/setup-animation/hello-card-35-confirmed2-transition-data/1-operations.txt)。

### GMEM 接入候选（1.184 / 1.52，功能验证中）

依据原厂 BGO/EOT 的完整附件生命周期，将符合条件的整个 Metal encoder 保留为一个 native pass：load/clear、连续 draws、store。仍使用 IOGPU 的队列、资源、页表和完成通知。新增 version 8 携带 608 字节附件描述，旧版 4/7 提交保持可用；不符合单采样颜色附件、只读 framebuffer 输入和命令容量条件的 pass 使用原路径。

A540 布局和寄存器顺序参考固定 Mesa 24.3.4 `fd5_gmem.c`。Hello 的 BGRA8 + transient RGBA16F 使用 256×320、5×6 tiles、983040 字节 GMEM。绘制 IB 只编码一次，由根 IB 重放；保留全局 viewport/FragCoord，私有附件 fetch 才减去当前 tile 原点。1.52 编译器保留 constants 80–95，用原生间接常量加载更新，BYPASS 保持零原点。普通采样、片元坐标及全局附件边界检查不变。

重放的 flush 使用独立 scratch 64–87，根 IB 每 tile 重置；原 watchdog 的 offset16 只随真实颜色 flush 完成单调递增。原点位于 offset2048，避开操作时间戳区。宿主命令检查确认 Hello 根 IB 9174 words、90 次子 IB 调用、60 次递增完成等待；GPU/Metal 交叉构建通过，不能据此宣称 GMEM 正确或动画提速。

候选 hello-card-36 默认关闭 GMEM，仅 `metalctl gmem-test` 在该诊断 device 上启用。该测试将多 tile BGRA8 写回、RGBA16F load/clear/只读附件 fetch、前序 producer draw、全局坐标及存储边界与 BYPASS 逐字节对照。真机结果待补；尚未用于 Hello。

功能验证结果：1.184 的 GMEM pass 已实际执行完成，但首个差异位于 offset512 + 1080×4，即 BGRA 行尾 padding；被 clear 的蓝色159覆盖，原保护字节为165。没有把这次执行成功当成功能正确。1.185 对最右侧非32像素对齐的 store 使用私有线性 tile 中转，再 CP_BLIT 精确矩形写回；不扩大目标范围。Hello 额外临时空间为327680字节，根 IB 命令检查为10014 words、72次单调完成等待。

`hello-card-37` 真机的3阶段对照全部通过：CPU图案 load、GPU clear后读取、同一pass的producer draw后读取，BGRA8输出与RGBA16F附件的有效像素、padding及首尾保护区与BYPASS完全一致，`GMEM_EXIT=0`。证据：[像素结果](../../build/setup-animation/hello-card/gmem-dev/hardware-185-pixels.txt)、[原始启动日志](../../build/setup-animation/hello-card-37-boot/uart.txt)。结果证明该颜色附件路径功能正确，不代表Hello已流畅。下一候选38使用相同GPU1.185，Metal1.52.1只为含只读附件消费者的完整pass默认选择GMEM，目标转场采集待补。


### 1.185 的 Hello 实测与下一步

38 已确认 display/power=0 后 HOME 唤醒，稳定6秒，再 HOME 采目标转场。59个目标窗口作业无失败，Metal failure 仍为启动时的1条。用户反馈“更顺，但仍卡”。后台两笔单独clear已并入GMEM三draw pass；18个该类pass的GPU中位为47.684ms，较35的两clear约2.273ms加三draw约36.462ms更慢，因此不能把GMEM功能通过当作性能成功。

本窗口另有4次不同请求键的现场shader编译，分别约170.6、215.9、628.8、179.6ms，已交冷编译任务核对。它们也意味着本轮不是纯热缓存样本。对照证据：[比较](../../build/setup-animation/hello-card/comparison-38-gmem.json)、[原始作业](../../build/setup-animation/hello-card-38-gmem-transition-data/1-jobs.txt)、[Metal事件](../../build/setup-animation/hello-card-38-gmem-transition-data/1-metal.txt)。之后尝试采热转场时检测到display仍为1，脚本未按HOME，也未把该轮当转场样本。

1.186候选先缩减完整只读GMEM pass中的重复同步：每draw结尾真实颜色flush与下一draw的UCHE invalidate保留；不再重复入口颜色flush和没有shader writes需要排空的UCHE flush。IOGPU真实退休、根IB单调progress及外部资源依赖保持原流程。同时选择满足1MiB与64×32/16KiB对齐要求、tile数最少的布局，Hello改为128×672、9×3=27tiles（原30），精确右边缘写回从6次减为3次。加上根IB的load/draw/store阶段时间戳，后续以真机结果判断收益。宿主命令检查：9114words、81个示例child调用、60次单调flush、108个阶段时间戳通过；不替代真机像素与目标验证。


### 1.186 的分阶段结果（2026-10-21）

39 真机通过1080×1920全高度的3阶段像素对照：CPU图案load、GPU clear、producer draw后读取，所有有效像素、padding和首尾保护区与BYPASS一致。目标HOME前display/power及目标值均为0，唤醒稳定6秒后采3秒；69个作业无失败，启动已有的backboardd超时未新增。

22个后台GMEM三draw pass中位45.933ms，相比38的47.684ms降低约3.7%。其中load/clear为0.194ms、draw为44.808ms、store为0.879ms，每pass27个tile的时间戳均完整。CPU编码中位4.646ms、地址空间准备1.479ms。结果把瓶颈定位到draw，不能继续以清屏耗时解释剩余卡顿。该路径目前每tile重放完整几何，尚未使用A5xx VSC硬件binning。

同一窗口4次实际编译合计约1209.6ms，其中一次NIR失败后GLSL路径626.6ms。显示驱动在实际PP_DONE及读访问退休后记录的23次完成中，5段间隔超过100ms，最长683.651ms；这些是显示完成间隔，不是光学FPS。若干长间隔与编译事件重叠，仍需精确变体补齐和热转场验证，不能把整段动画的变化全归因于GMEM。用户不在设备旁，后续按这些实际记录判断，不等待人工观感。

证据：[像素对照](../../build/setup-animation/hello-card/gmem-dev/hardware-186-pixels.txt)、[分阶段比较](../../build/setup-animation/hello-card/comparison-39-gmem.json)、[长帧](../../build/setup-animation/hello-card/long-frames-39.json)。40保留同一GPU，已加载Metal1.52.2的缓存身份/未命中诊断；第一次采集因屏幕仍亮而跳过，不作为目标样本。


### GMEM默认策略依据热转场回退

40同一启动的热转场无JIT，121个作业无失败，但21个后台GMEM pass中位45.949ms，draw44.824ms；显示完成最长间隔450.070ms，超过100ms有4段。另有UI自身的64操作作业75.988ms，其中全屏片元hash2f70b41ee30a6eb1单draw约12.838ms中位。剩余耗时不是单一清屏或冷编译问题。

在未实现A5xx VSC几何分箱前，现有GMEM每tile重放完整几何，已测得比原BYPASS两clear2.273ms加三draw36.462ms更慢。因此源代码恢复默认BYPASS，保留已通过像素验证的GMEM完整实现与诊断开关，不强制默认使用较慢路径；下一次配套目录候选进行目标验证，尚未把该源代码调整当作真机收益。


### 1.187 前景尾宽清屏候选

40热转场的前景还存在compute尾宽清屏：widthBytes/rows/pitch为4328/1922/4352、2168/962/2176及2184/744/2240，分别约6.771、1.835、1.423ms。它们与后台GMEM内0.194ms的load/clear是不同操作。原CP_BLIT资格检查沿用了compute uint4的16字节行宽限制；普通R32 solid fill实际用像素端点描述矩形。1.187仅将R32行宽条件放宽到4字节，8字节pair格式保持原限制，起址、pitch、范围、格式及冻结参数验证不变。

新增clear-tail-target-test仅比较上述3种实际布局和邻近4332/17/4352，共8个scalar/hardware完整字节检查，包含padding和首尾guards。配Metal1.52.3精确目录及默认BYPASS的41已打包，像素与转场结果待补；没有把候选当作已验证收益。


### 1.187 / 1.52.3 已部署结果

41的尾宽清屏8项真机检查全部通过，共24,398,120字节，有效像素、padding、首尾guard精确一致。启动前保存的完整job-history中，测试pid150恰有8个单操作作业；按原测试顺序匹配，3个实际布局的CP_BLIT GPU时间为0.8531、0.2345、0.1597ms，邻近4332字节行宽为0.0490ms。对照scalar分别24.6539、6.2407、4.8935、0.2786ms；scalar不是40的向量实现，不能混作同一基线。40目标中的向量尾清屏分别约6.771/1.835/1.423ms，41目标中这些已低于慢操作阈值。

41确认熄屏后的首次转场138jobs、热转场141jobs均无失败ticket，无GPU恢复；启动已有的1条backboardd超时未新增。热转场没有Function/JIT事件。22组后台两clear中位2.233ms、三draw36.504ms；两项中位之和38.737ms，40GMEM完整pass中位45.949ms，约减少7.2ms的GPU工作。两版作业分组不同，不能将此百分比当作整段动画流畅度。

热转场50次显示完成，最长间隔483.395ms，另有250.160、133.225、116.680、116.741ms，目标尚未解决。作业prepare在HOME+403.775ms后直到849.809ms才出现下一项，约446ms没有新作业进入准备阶段；下一步定位CPU侧资源/场景准备与等待，而不是继续用冷编译解释该空档。主背景片元和UI重绘也仍有独立GPU成本。

证据：[清屏字节检查](../../build/setup-animation/hello-card/gmem-dev/hardware-187-tail-pixels.txt)、[8个GPU作业耗时](../../build/setup-animation/hello-card/tail-clear-187-gpu.json)、[首次转场](../../build/setup-animation/hello-card/target-41.json)、[热转场](../../build/setup-animation/hello-card/warm-41.json)、[部署凭据](../../build/setup-animation/hello-card-41/receipt.json)。41打包中间文件已清理，保留当前EFI/内核及必要回退和证据。

### 候选 41：提交前空档的原生线程栈归因

同一 GPU 1.187 / Metal 1.52.3 启动，仅追加一次带栈的目标转场，先确认熄屏，再 HOME 唤醒、稳定 6 秒、HOME 进入转场。原始证据在 `build/setup-animation/hello-card-41-gap-stacks-data/`，解析在 `hello-card/gap-41-stacks.json`。此采样具有侵入性，不混入无栈的性能基线。

HOME +614.814 ms 的全系统原生 stackshot 覆盖 290 个任务，内核记录的 capture duration 为 174951 个 mach ticks，即 9.112 ms（不是 174951 微秒）。SpringBoard 主线程在 `SBCoverSheetPresentationManager::_prepareForDismissalTransition` → `UIWindow::setWindowLevel:` → `_UISceneLayerHostContainerView::_rebuildLayersForNormalPresentationWithReason:withFence:` 调整窗口层级；上游调用是原版 HOME 单击处理。backboardd 的主显示线程、Setup 主线程和 Mercury 主线程在 Mach 消息/RunLoop 等待，没有看到这一时刻卡在 QcomMTL 提交锁或 shader 编译。

一个采样点不证明整段约 440 ms 都由场景层级调整耗费；不能据此修改原版按钮识别时限或伪造 GPU Scheduled。全系统文本输出推迟了后续选进程采样：它们实际位于 HOME +2034/+2114/+2198 ms，不能冒充 +0.7 秒附近。后续 backboardd 栈落在原版 `CA::OGL::Shape::FillRenderer::render_lines`，背景主线程落在 `flushDirectDrawable` 等待；这两条路径与动画进行期间的 CPU 几何工作和 GPU 背压相符，但栈点本身不是持续时间。

无栈热基线的 22 笔背景三绘制作业中，CPU prepare 中位 7.217 ms，其中 index snapshot/校验中位 3.728 ms、编码 4.548 ms；计时相互嵌套，不累加。两次 SDF 绘制各 29184 个索引。相同内核 flags 的 ARM64 汇编确认 `GPUShaderProtocol.h::indexFootprint` 为每个索引执行动态逐字节拼装内循环。候选 GPU 1.188 将 16/32 位扫描拆为编译期固定宽度，生成单次 LDRH/LDR W，保留原本的逐索引 baseVertex/范围检查、strip restart 和失败返回语义；仅扫描不可变内核副本。候选尚待真机验证，不把指令减少直接称为帧率改善。

### GPU 1.188：固定宽度索引扫描的真机结果

候选 42 保持原版 Metal 1.52.3 二进制、编译器、148 变体目录、GMEM 默认关闭及清屏实现，仅优化内核不可变索引副本扫描。主机 GPU submit 检查（ASan/UBSan）通过；真机 `indexed-test` 的 UInt16/UInt32、baseVertex、instance_id/baseInstance、additive instances 像素检查通过。

首次目标转场 130 个作业，失败 0，无新 JIT；全部失败报告中的单条 stage6 是 HOME 前启动记录（timestamp 1065470859，HOME 3586504166），不能报作本次转场新失败。匹配的 22 笔背景三绘制作业：

| 阶段中位耗时 | 41 热基线 ms | 42 首次转场 ms |
|---|---:|---:|
| 索引快照/完整校验 | 3.728 | 2.777 |
| CPU 编码（含上述阶段） | 4.548 | 3.563 |
| CPU prepare（含上述阶段） | 7.217 | 6.786 |
| 地址空间准备 | 1.461 | 1.455 |
| 实际 GPU 工作 | 36.504 | 36.352 |

这是相同作业形状、不同启动和预热状态的 CPU 阶段比较，不能把中位时长差相加或换算实际动画 FPS。索引扫描改进约 0.95 ms 已在真机出现；首次转场显示完成仍有 616.806 ms 的长间隔，问题未解决。证据为 `hello-card/index-scan-188/hardware-comparison.json` 和 `hello-card/target-42.json`。

候选 42 的 EFI、kernel、验证记录保留；其已使用的 collection、HFS、image-root、桥接大中间产物已清理，完成的 17.98 MB 主机测试程序也已删除。当前保留 41 EFI 作为上一已验证版本。一次热采集因开始时屏幕仍亮被状态检查跳过，没有额外 HOME 输入；该次缺失文件导出失败不计作有效采样或 GPU 失败。

候选 42 的有效热窗口随后完成：154 个作业、失败 0、Function/JIT 事件 0。28 笔背景三绘制的索引快照中位 2.784 ms、CPU 编码 3.546 ms、prepare 6.379 ms、GPU 36.450 ms；显示完成仍有 466.733/300.029/183.509/116.677/116.721 ms 间隔。已确认 CPU 局部收益，仍未证明动画流畅。有效证据为 `hello-card-42-warm2-transition-data/`、`hello-card/warm-42.json`；前一次被亮屏状态拒绝的窗口继续保留并排除。

### GPU 1.189：目标片元的 A540 计数采样（2026-10-21）

候选 `hello-card-44` 保持 Metal 1.52.4，增加仅由 `gpu-oc=3` 启用的诊断：沿 pinned Mesa `fd5_query.c`，对每批第 3 条、完整计时且不使用 GMEM 的 draw，读取 8 个 SP、4 个 TP、4 个 UCHE 计数。使用一计数器一条 64-bit `CP_REG_TO_MEM`，私有进度页 1152..1407 字节保存前后值；只在物理完成、地址空间退休和一致性处理后写入 64 条 CPU 环形记录。动画期间不打印，`gpuctl shader-counters` 在既有 statistics 发布后读取。默认关闭，不改变渲染同步或 shader ABI。

确认熄屏后 HOME 唤醒、稳定 6 秒再 HOME，3 秒窗口中 144 作业、0 失败、0 JIT。实际 profile100 / `a7aba4cb1419480f` 获得 25 条计数，操作中位耗时 28.339635 ms，与上一版一致。SP busy 67,493,707，ALU 18,696,438，EFU 3,114,720；TP busy 67,491,072，TP 等待 UCHE 50,238；L1 请求 18,828,584、miss 1,041,237（5.53%）；UCHE 等待 VBIF 6,852。主绘制 SP 等待 UCHE 仅 2、等待 RB 为 0。计数会重叠、包含多个 SP 实例，不能相加当作墙钟时间，尤其不能把 LONG_SYNC 的比值当作可节省的时间。

这些结果将后续审查重点转到纹理采样和吞吐；没有证据支持继续以少量 ALU 精简、扩大主存缓存或删同步作为当前解决方案。实际 8 次 SAM 坐标不同，静态 constant sampler 是 normalized / min-mag linear / mip none / clamp edge / anisotropy1，与 `fd5_texture.c` 对照未见状态遗漏；不能删采样或近似转换 RGBA16Unorm。全窗口仍有 650.077 ms 的显示完成间隔，与此前 native CoverSheet 交接期间未提交绘制的证据分别保留。

证据：`build/setup-animation/hello-card/counters-189/{source-receipt,hardware}.json`、`hello-card-44-transition-data/1-counters.txt`、`hello-card/target-44.json`。这是诊断定位，尚未获得新的流畅度改善；目标仍未完成。已删除使用完的候选镜像/内核中间文件，保留可启动 EFI、kernelcache、回退版本和小型日志。
