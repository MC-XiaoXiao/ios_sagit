# Hello 与 HOME 动画驱动优化复核

本轮结论是：**先解决原生 IOSurface 访问 fence 的等待与调度问题，再优化正常帧的提交成本和逐图元工作。** 显示 producer fence、资源去重合批已经推进，不能继续把它们列成完全缺失的功能；但“正常路径不闪屏”也不能代替延后访问路径的稳定性。

分析日期：2026-10-18，America/Los_Angeles。范围为当前源码和项目已经保存的记录。本轮没有连接设备、运行真机或主机测试、构建镜像、改动驱动。源码与数据哈希、关键定位及统计口径见 [本轮证据](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-optimization-review-1.123-evidence.json)。共享工作区正在继续推进，以下结论对应证据文件中的快照。

## 当前进度与版本边界

| 项目 | 当前证据 | 对后续优化的影响 |
| --- | --- | --- |
| 工作区 | GPU1.123 / Metal0.90 / Display1.0.13；Metal 源码与冻结的 round129 版本相同 | GPU 保留未 armed ticket 的 fence 取消修正，1.123 新增失败时的 surface 等待诊断；工作区不是完整的 GPU1.121 快照，诊断也不是修复 |
| Metal0.89 资源去重合批 | 按不同原生资源 ID 计算容量，最多 64 个资源、16 个操作，保留真实 payload、索引及依赖边界 | “重复绑定把批次提前拆开”已修，不应继续重复实施 |
| GPU1.121 / Metal0.90 | pipe6 接入原生 IOSurface 访问 fence；GPU ready 后执行，真实完成和一致性发布后 signal；原生显示依赖这些 fence | producer fence 已有正常路径；Metal 允许整份命令列表 Scheduled 后呈现，但仍保留后续准入屏障 |
| round129 视觉反馈 | 已有记录明确写入用户确认“不再闪屏” | 仅保留为该轮历史观察；后续已再次报告局部笔画和整屏闪烁，不能继续称为当前验收结果 |
| GPU1.122 / Metal0.91 尝试 | 放开 fenced present 后的后续准入；round130 后续检查发生 GPU fault，候选撤回 | 不应恢复这项改动来追求帧率，直到访问依赖的前进条件明确 |
| 更晚的恢复记录 | round131-health 报告 `GPUStage=faulted`、16 个 ticket 失败、8 次访问 fence deferred、7 个失败 fence | 不能宣称恢复后已稳定；故障也不能只归因于 Metal0.91 |

最新恢复证据将 round131 绑定到冻结的 GPU1.121 / Metal0.90，boot UUID 为 `0D13F968-C105-4BB4-A4BD-CC43B6D6380D`，并记录该基线再次停滞。用户最新反馈为部分“你好”笔画局部闪烁，随后按锁屏没有动画而整屏闪烁；所以 Metal0.91 的提前准入不是项目现有记录中复现故障的必要条件。单独 health attach 的 `running_payload_identity_verified=false`，身份关联依赖恢复启动及后续交接记录，而非该查询独立核验。见 [恢复复现证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-present-pipeline/restoration-evidence.json)。[恢复健康日志](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-131-health/uart.txt)、[撤回记录](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-present-pipeline/withdrawal.json)。

## 正常窗口还慢在哪里

采用已有 round129 的正常窗口作为成本参考；round130 只用于比较观察值及定位失败，不能作为通过验收的候选。恢复 round131 的独立窗口曾完成 1367 笔 UI 作业、显示 970 帧，窗口头部 p50 约 33.45 ms，但随后 GPU fault；这不是新的稳定性能基线，也不能用来推翻故障记录。

| 指标 | round129 | round130 |
| --- | ---: | ---: |
| 实显间隔中位数 | 49.923 ms | 49.914 ms |
| 实显间隔 p95 | 100.400 ms | 100.103 ms |
| GPU 执行均值，每作业 | 9.039 ms | 8.951 ms |
| CPU 观察到执行完成，每作业 | 16.179 ms | 16.520 ms |
| 提交包装耗时，每次提交 | 9.746 ms | 9.697 ms |
| 地址空间建立，每作业 | 3.010 ms | 2.992 ms |
| 排队到投递，每作业 | 19.351 ms | 17.289 ms |
| shader 操作数 / 作业数 | 7766 / 1113，约 6.98 | 14423 / 2007，约 7.19 |
| framebuffer 背景复制字节 | 0 | 0 |

这些窗口的显示节奏仍远离每 16.67 ms 更新一次。较早窗口约 66.6 ms，较新窗口约 50 ms，是观察到的进展；动画阶段和停留构成不同，不能把差值全部归给某项补丁。合批后每笔 GPU 时间由约 6 ms 变为约 9 ms，也不能直接判定为退化，因为每笔承载的操作数增加了。

round129 的 GPU 统计跨度 89.74 秒，显示样本跨度 27.79 秒；两者不能相除得到每显示帧的作业数。CPU 阶段存在嵌套，不能把上表相加成帧耗时。16.179−9.039≈7.14 ms 是“CPU 观察区间与 GPU 计时区间之差”，其中可能含命令设置、调度和完成处理延迟，尚不是测得的锁竞争时间。[正常窗口统计](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-129/performance.json)。

## 优化优先级

| 顺序 | 改动方向 | 预期收益与证据 | 难度 |
| --- | --- | --- | --- |
| P0 | 原生访问 fence 等待状态与调度前进性 | 已出现整队列停止；必须先避免卡死和全设备失败 | 中高 |
| P1 | 有界小常量上传复用 | `setBytes` 系列仍逐次 new/delete 原生资源；资源创建约 5.09 ms/调用，删除约 2.19 ms/调用；真实 Welcome 命中比例未分离 | 中 |
| P1 | 免复制路径的重复同步、同一 draw 内的重复状态 | 已实际命中 inplace；仍逐图元 flush、wait、restore 和重发程序状态 | 小改动低至中，状态复用中高 |
| P1 | 缩短提交占用共享 gate 的时间 | 提交约 9.75 ms，完成观察存在约 7.14 ms 差值；源码确认提交与退休共用串行路径 | 中高，收益需细分验证 |
| P2 | 缩小每作业实际映射范围与重复健康检查 | 建表约 3.01 ms，其中资源映射约 2.08 ms、健康检查约 0.87 ms | 中高 |
| P2 | 同队列 Metal fence 的 GPU 依赖快路径 | 当前 updateFence 暂停后续编码，等待 CPU 消费真实完成；对多 pass 有潜在收益 | 高，依赖 P0 的正确模型 |
| 暂缓 | 再放开 presentation barrier、扩大队列、继续压缩 IB、优化背景复制 | 前者已有失败候选；后几项缺少当前瓶颈收益证据，复制量已为 0 | 不作为下一补丁 |

## 首先补齐访问 fence 的前进条件

当前有四道顺序约束同时生效：

1. 每笔作业在准入时为所有唯一 IOSurface 创建访问 fence，写访问合并为方向 2；只有所有 surface ready 才允许投递。
2. 同一队列里，较早尚未投递的作业阻止后续作业越过它。
3. 任一写入涉及重叠 backing 时，后继作业等前驱完成 CPU 一致性退休。
4. 全局 native stamp 按准入前缀退休；后面的作业即使 GPU 完成，也不能直接越过队首发布成功。

单项约束各有正确性理由，但它们与原生显示持有的读 fence 组合后，需要证明整个依赖图可以前进。源码位置为 [访问 fence 创建及就绪判断](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/QcomGPUNative.cpp:323)、[投递筛选](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/QcomGPUHardware.cpp:1032)、[全局前缀退休](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/QcomGPUHardware.cpp:1180)。

round130 的记录中 ring 读写指针相等，CP error 为 0；全部已投递的 2009 笔作业完成，仍有 16 个 ticket 在故障清理中失败。`active=16` 是无 active job 的哨兵值，**不是 16 个作业正在 GPU 上执行**。7 是 deferred fence 的数量，也不等于恰好 7 笔被直接阻塞的作业。其余作业可能受到同队列顺序、资源依赖或容量限制。[故障原始记录](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-130-targeted/uart.txt:25)。

需要区分三个候选原因：外部 IOMFB 读者尚未释放；GPU 自身的访问 fence 与准入/退休顺序相互等待；原生 fence 已进入完成或失败状态，但 Qcom 仅观察 ready 位而继续等待。尚无逐 surface 前驱关系证明其中哪一个是根因。全局前缀退休是潜在放大器，不足以单独解释本次全部未投递的队列。

分析期间源码已经补入 `logSurfaceFenceWait`：持有原版 surface 锁读取 current/waiting 列表，解锁后输出，有界记录各 8 项。应利用这项新增诊断继续判别，避免重复开发同样日志。它尚不等于修复，也没有在本轮产生新的运行证据。还需把 fence 身份对应到 GPU job、队列、原生 stamp，以及显示的 front/queued surface 身份，才能确定谁在等待谁。

修复设计应先把 Ready、Pending、Failed 分开，并明确以下情形的状态迁移：外部读 fence 释放后，没有 GPU IRQ 时能否继续投递；较早作业等待、较后独立作业完成时如何保持合法发布顺序；多 surface 访问能否形成持有一面、等待另一面的环；未 armed、取消、关闭与晚回调如何清理。原版等待 worker 在部分失败路径不会执行 ready callback，不能仅依赖回调是否到达判定存活。

当前 stateless callback 不触碰 ticket，生命周期安全但不唤醒调度；pending-only 情况每 1 ms 检查，约 5 秒未 ready 后触发全设备 fault。后续可增加有取消保证的工作循环唤醒，保留低频 watchdog。**更快轮询或单纯延长 5 秒期限不能消除依赖环。**

原版 AGX 的 transaction 路径和非零同 accelerator bypass 可作为静态对照，但不能直接给 flags 加 4：必须证明 GPU 内部先后关系已由自己的调度完整维护，并继续等待外部显示读者。也不能提前 signal 成功来解除等待，那会破坏已经取得的显示一致性。相关原版证据见 [接口与生命周期](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-producer-fence/native-surface-fence-abi.json) 和 [等待 worker](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-present-pipeline/0xfffffff005a9e1dc.json)。

## 正常帧成本的可执行优化

**小常量上传池是比较独立的性能补丁。** [compute setBytes](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomMTL/QcomMTL.mm:3740)、[vertex setBytes](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomMTL/QcomMTL.mm:4107)、[fragment setBytes](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomMTL/QcomMTL.mm:4139) 每次都创建独立 shared buffer，继而进入原生资源创建；对象释放再次进入删除接口。即使只改一个 opacity 常量，也走同一机制。已有资源计时包含所有 NewResource 调用，尚不能认定其中全部来自 Hello 的 setBytes。

建议使用每 command buffer 的有界小页分配，提供精确长度的内部 view；每次 setBytes 当场复制，后续覆盖绑定不能改变此前快照。提交后冻结页面，真实 GPU 消费结束后才回池，保持错误、取消和资源权限语义。现有 `_nativeOffset` 与精确 view length 可作为复用基础。避免巨型全局 arena：当前 mapper 会映射整个 backing，过大的池反而增加建表及别名等待。先记录调用数和长度分布即可判断真实收益，不需要另做一整套泛化 profiling。

**逐图元同步可以从一个很窄的改动开始。** inplace 分支跳过了背景复制，但仍在 [每个图元 raster 前](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/AdrenoPipeline.h:699) 无条件 `flush(4,2)`；进入循环前以及上一图元结束时已有相应 flush。对于两次 flush 之间没有 GPU 写入的 inplace 路径，这是可证明、可回放检查的冗余候选。每次 helper 含两个 idle、一个带时间戳的事件和对应 wait。只在证明成立的分支移除前置重复同步，保留图元写回、UCHE 可见性、失效以及 fallback 复制后的同步。

随后再考虑同一个 draw 内提取不变状态，减少 [每图元 restore](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/AdrenoPipeline.h:736) 和程序、常量的重复上传。跨 IB 重试、compute 切换、实例和 winding 变化以及掉电恢复都要使相应状态重新有效。不能把有 framebuffer feedback 的重叠图元直接合并成一个无同步 draw。现有计数证明这条路径在用，但没有给出删一条同步可节省多少毫秒。

**共享 gate 的优化应先减少里面的工作，再考虑拆锁。** `nativeQueryAction` 的完整 Family 调用、`submitNative` 的编码和建表占用共享 gate，IRQ/workloop 上的退休也走串行处理。小资源池、减少作业固定成本和减少重复编码可以先降低占用。进一步将纯 CPU 准备移出 gate 时，必须持有资源引用和不可变快照，并在最终准入重新核验关闭、故障、容量、权限及 generation；不能把 Family 调用或 AS 发布直接移出保护范围。应补充 gate 等待、持有和 GPU 完成至首次观察的独立时间，避免把 7.14 ms 差值误认为可全部消除。

**建表优化重点已不在页表分配。** round129 的资源映射约占建表 69%，健康检查约占 29%。[mapper](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/QcomGPUMapper.cpp:364) 把 view 转为 root 后映射整个 backing。下一步应按经过验证的 buffer 范围、texture 子资源和实际资源访问取页对齐并集，保留物理来源和权限检查；未知访问范围回退整块映射。不能仅按屏幕 scissor 裁掉任意 shader 可能采样的纹理页。IB 的命令头和代码/常量尾都必须包含。

健康检查可研究同一受保护事务内的重复合并，前提是硬件状态不变量及失效条件明确；实际 FSR、当前 root、ASID 和 TLB 同步仍须保留。build TLB 约 5 μs、retire TLB 约 2.35 μs，删除这些检查的收益远小于风险。

**Metal fence 仍有下一阶段空间。** `updateFence` 分支在 [run](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomMTL/QcomMTL.mm:3431) 调用 `waitNative`，异步暂停后续编码，等 CPU 消费前序完成后才发布 fence point；这不是 CPU 忙等，但会打断多 pass 提交链。先限定同 command buffer、同队列的可证明依赖，建立 GPU 屏障快路径；保留跨队列、事件和失败回退。当前混批只接受 draw/dispatch，且 kernel 写入重叠仍等 CPU 退休，所以仅去掉上层 wait 不会自动形成正确流水线。还需区分“已知 GPU 依赖”与“已完成”，并处理 native fence、缓存所有权和批次边界。

## 对 Hello 与 HOME 的意义

Hello 的层 opacity 与动态笔画、shadowPath 可以同时更新；HOME 是约 0.6 秒径向 mask 展开和末尾 1 秒 alpha 淡出。它们共同依赖 backboardd 输出的新合成结果及时通过 GPU 和显示。资源分配或依赖等待占住共享路径时，HOME 即便字形稳定，也会跳过中间状态；修复提交和访问等待能同时帮助两者。[原版动画路径](/home/xxiao/Projects/MI6_iOS/docs/gpu/welcome-static-analysis.md:99)。

因此当前最值得做的下一步是：完成已经开始的 surface 等待诊断与离线状态模型，先修复前进性；独立性能补丁优先小常量池和窄范围同步精简，再推进 gate、映射范围和 GPU fence 依赖。完整 GMEM/tile renderer、泛化 shader 优化或继续扩大合批不是这轮收益最明确的首选。现有静态证据不能承诺其中一项单独达到 60 FPS。
