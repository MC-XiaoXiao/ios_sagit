# 当前 Welcome 长帧与 CPU 提交链路分析

当前长帧同时包含提交前的长空档和 GPU 内部重作业。CPU 侧资源管理、串行执行与提交链路值得优先优化；现有数据尚不能把它归结为 CPU 频率或算力不足。对另一类长帧，GPU 自己的执行区间已达 100–178 ms，CPU 优化无法替代 GPU 后端优化。

源码和最新完整采集 round146 均为 GPU 1.132.0、Metal 0.93.0、Display 1.0.17。分析期间其他工作新增了 round146 结果，本文已纳入；原 round144 的 GPU 1.131.0 只保留为历史观察。本文只读取源码、Apple 原版反编译和已有采集，未操作设备、修改驱动或构建镜像。复算与源码哈希见 [最新证据](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-long-frame-review-1.132-evidence.json) 和 [历史证据](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-long-frame-review-1.132-round144-evidence.json)，可用 [分析脚本](/home/xxiao/Projects/MI6_iOS/docs/gpu/analyze-long-frames-1.132.py) 重现。

**长帧的位置已经可以分开。**

最新 42.833 秒窗口内，874 笔合成作业提交、873 笔完成、0 失败；407 条连续显示记录形成 406 个间隔，中位 33.449 ms、p95 116.968 ms、25 个超过 100 ms、最大 1533.688 ms。首末显示帧区间速率为 15.712 FPS，另有开头 16.947 秒和尾部 0.045 秒未新增显示，不能把首末区间速率当作整个窗口平均帧率。窗口含按键唤醒和内容停留，静态停留与动画卡顿尚未逐阶段标记，不能根据长间隔分布断言 1.132 比 1.131 回退。

| 显示 token 区间 | 显示间隔 | 同期关键证据 | 优先调查的位置 |
| --- | ---: | --- | --- |
| 324 → 325 | 183.387 ms | 观察完成的一个 15 操作渲染作业，GPU 区间达 177.884 ms | 重渲染及逐图元反馈实现 |
| 189 → 190 | 250.133 ms | 前 152.318 ms 未进入下一笔已记录作业的 submitNative；间隔开始没有准备中且未退休的历史作业 | 准备入口之前的 CPU、框架、资源和调度链路 |
| 111 → 112 | 1533.688 ms | 同类前段空档 1455.898 ms；同期观察完成的最长 GPU 作业仅 35.215 ms | 先区分正常停留与上游延迟，不能称为 1.5 秒 GPU 执行 |
| 174 → 175 | 500.208 ms | 同类前段空档 454.120 ms；同期观察完成的最长 GPU 作业约 4.562 ms | 轻 GPU 工作也伴随长显示间隔，仍需活动帧标签 |

submitNative 的 prepare 时间戳位于共享内核 gate 以内。因此“尚未进入准备”涵盖 CA 编码、Metal 队列、资源创建、内核 gate 等待等可能性，也可能包含动画自身停留；它不等于 CPU 忙了相同的时间。历史只记录已准入并退休的作业，不能排除准备入口前的调用或未准入尝试。GPU 与 CPU 绝对时钟未对齐，只比较各自持续时间及 CPU 侧事件关联；表中的 GPU 作业不必完整落在该显示间隔内。

历史 round144 中两个约 250 ms 的间隔，前段未进入下一笔 submitNative 的时间分别为 152.613、153.251 ms；最新 round146 仍出现同类约 152 ms 的空档。这个重复现象值得优先关联上游时间线，但不能把内容不同的捕获当成严格 A/B 对照。无已投递且未观察完成作业的时间也不是直接读取硬件 idle 得到的利用率。

**CPU 是瓶颈之一，但尚未证明 CPU 算力不足。**

| 阶段 | 同窗口结果 | 实际含义 |
| --- | --- | --- |
| CPU 准备到准入 | 中位 5.860 ms，均值 5.650 ms，最大 34.303 ms | 小作业也支付明显固定成本 |
| 提交包装 | 均值 7.180 ms/作业 | 包含其他阶段，不额外累加 |
| 私有地址空间准备 | 均值 2.784 ms/作业 | CPU 映射与核验路径，区别于 GPU 上下文切换 |
| 命令编码 | 均值 1.393 ms/作业 | 可从减少重复状态和上传入手 |
| 资源创建和删除 | 811 次创建，1.567 ms/次；755 次删除，1.782 ms/次 | 普通 buffer 与客户端导入是主要对象 |
| CPU 退休 | 中位 0.714 ms，最大 5.961 ms | 这一内核阶段本身不足以解释约 152 ms 的前段空档 |
| GPU 执行 | 中位 2.005 ms，均值 8.841 ms，p95 58.985 ms | 分布极不均匀，平均值掩盖重作业 |

106 笔 GPU 区间超过 16.67 ms 的作业，占作业数约 12.1%，却占所选作业 GPU 区间总时长约 75.5%；8 笔超过 100 ms。优化应覆盖小作业的 CPU 固定成本，并集中削减重作业的 GPU 工作量。表中计时有嵌套、等待与流水重叠，不能相加成一帧，也不能换算 CPU 利用率。GPU 硬件计数对应约 595.988 MHz；同窗口没有 CPU 有效频率、逐线程运行时间和调度等待证据。

**Apple 完成处理与当前 Metal 资源清理的差异值得优先修。**

原版 `IOGPUMetalCommandBuffer didCompleteWithStartTime:endTime:error:` 先调用基类完成处理，再调用 `IOGPUMetalCommandBufferStorageDealloc`。后者 reset 存储并尝试归还 pool，资源池优先取已缓存对象、没有可用对象才创建。原版 AGX 的 setBytes 路径也使用按游标推进的上传池。这些证据支持完成后的资源复用与清理分层；不能据此断言全部异步回调一定在清理前执行。

当前 [finishRetirements](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomMTL/QcomMTL.mm:2878) 持有 `_executionLock` 调用 command buffer 的 `run`；[run 的末尾](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomMTL/QcomMTL.mm:3681) 在设置 Completed、唤醒等待者之前，同步清空 `_operations` 和 `_ownedResources`。数组最后引用释放可进入 [buffer dealloc](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomMTL/QcomMTL.mm:1786)，再持 `_transportLock` 调用内核 deleteResource。该路径可阻塞下一轮提交；当前私有 inline 页可以回池，普通 buffer 和客户端导入不享有同样的快路径。

建议在真实 native end token、资源一致性和错误状态都确认后，分离完成状态发布与耗时资源清理：把待释放数组的强引用交给有界清理队列，使提交执行器不被一串析构占用。保留 FIFO 完成语义、回调恰好一次、client deallocator 的安全时机、最终引用与物理退休约束，并对保留内存设上限。需要记录清理耗时和锁等待以确认收益；当前不能认定约 152 ms 空档全部来自这里。

该问题即使没有 drawable presentation 也可出现。`_presentationBarrier` 只在相应 presentation 分支生效，不能把 CAMetalLayer 的路径默认套到原生 CA server 的 IOMFB 输出。

原版证据：[完成处理](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-hardware-phases/apple-command-did-complete.json)、[存储 reset 和回池](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-hardware-phases/apple-command-storage-release.json)、[池内优先取用](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-hardware-phases/apple-resource-pool-create.json)、[AGX 上传游标](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-inline-constants/apple-audit.json)。

**GPU 重作业仍受逐图元反馈路径放大。**

当前 [framebuffer fetch 展开](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/AdrenoPipeline.h:690) 即使命中 inplace，也对每个实例、每个图元分别调用 raster。raster 仍恢复状态、失效缓存、重发大量寄存器及 shader LOAD_STATE，并在每个图元之后执行 color/UCHE flush 和等待。一个作业中的 15 个操作不等于 15 个图元；缺少逐作业图元数、像素覆盖、程序身份和 flush 次数，尚不能把 177.884 ms 精确分给 shader 算术、显存或排空。

1.132 已增加单个 IB 内不可变 ISA 副本复用，并去掉 inplace 相邻图元之间的一次冗余 UCHE preflush。最新窗口记录 8575 次副本复用、避免复制 7,561,088 字节 ISA，证明路径实际命中；它保留 shader 状态加载及真正的每图元完成等待，direct LOAD_STATE 仍携带指令负载，因此避免上传的字节数不代表 GPU 指令缓存命中或等量显存带宽收益。beginOperation、IB 切换和回滚还会清除缓存，不能把它描述为跨作业 shader cache。当前仍有 177.884 ms 重作业，不能以命中次数代替长帧改善。

下一步可在同一 draw 的图元展开内区分不变状态和图元变化状态，减少重复的 linkage、shader 配置、纹理描述符、常量准备及合法范围内的重装载。保留实际 framebuffer 可见性、图元顺序和必要失效；compute、copy、IB 切换、回滚和恢复边界必须使状态缓存失效。Apple 的资源池证据不证明 A540 的缓存语义，真正减少 HLSQ 重装载仍需依据该后端的状态保持约束。

更大的优化是把严格可证明等价的 framebuffer-fetch source-over 表达式降为硬件 blending，或实现正确的 tile/GMEM 反馈路径。它可能减少每图元串行排空，但难度更高；mask、gamma、非标准混合和任意 shader 读取不能套用普通 alpha blending。当前不建议简单取消全部 flush/wait，也不建议把 16 操作批统一切小：后者增加准入、建表和通知成本，并不能减少整帧总渲染工作。

**按收益与难度安排下一批工作。**

| 优先级 | 工作 | 收益依据 | 难度 |
| --- | --- | --- | --- |
| P0 小范围定位 | 补齐活动中的 display-link/CA transaction，以及 CB commit、worker 开始、transport/gate 等待、清理开始结束、Scheduled、present、显示 ready 与 latch 的关联时间线 | 区分正常停留与提交前长空档；已有 GPU 作业历史无法覆盖这些边界 | 低到中 |
| P1 首选 CPU 改动 | 完成状态与耗时析构分离，缩短提交执行器和 transport 锁的占用 | 有明确同步清理链及每次资源删除约 1.78 ms 的证据 | 中 |
| P1 CPU 后续 | 扩展普通私有临时 buffer 的有界复用；按用途区分客户端导入 | 811 次创建中，272 次普通 buffer 和 304 次客户端导入落在大于 4 KiB、不超过 16 KiB 的分组 | 中到高 |
| P1 GPU 主线 | 完成 1.132 的效果归因，再做同 draw 不变状态复用和重复 LOAD_STATE 削减 | 少数重作业占 GPU 区间总时长多数；现有逐图元完整 raster 明确存在 | 中到高 |
| P2 | 准确识别可降为硬件 blending 的 fetch，或设计 tile/GMEM 路径 | 有机会去除大量逐图元串行代价 | 高 |

客户端 no-copy 导入不能仅凭相同地址和大小直接命中旧映射；必须验证 backing 身份、存活期、权限和回调契约。普通资源池也必须在最终引用和真实 GPU 使用结束后复用。原版 CA 的条件 flush 使用 `commitAndWaitUntilSubmitted`，必要时等待真正 Scheduled；不能把私有 marker 当作整份操作列表已提交来消除 CPU 等待。

显示 1.0.17 的单次传输读 fence 退休已解决此前捕获的持久 front 读依赖，当前完整窗口没有该类作业失败。应保留 `PP_DONE + latch + idle` 后退休读访问的规则。约 33 ms 的常见间隔可能包含错过 60 Hz 显示机会，但当前 timing attributes 并未固定为 30 Hz，不能先归因于“显示锁 30 帧”。需用 ready → start → PP_DONE 时间线判断是否额外等待了显示周期。

Hello 的 path/shadowPath 生成与语言切换发生在上游；淡入淡出和 HOME 渐变仍共享最终合成与呈现。已有样本没有逐元素动画标签，不能把每个长间隔都指定为 Hello 淡出或 HOME mask。优化共享提交链路与重渲染路径能够覆盖二者；原版动画正常停留应与真正延迟分开。[原版动画路径](/home/xxiao/Projects/MI6_iOS/docs/gpu/welcome-static-analysis.md:99)
