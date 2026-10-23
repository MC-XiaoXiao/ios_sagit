# iOS 18 着色器冷编译优化：MSAA 无关变体复用

2026-10-21，Metal **1.48.0**。对不依赖采样模式的片元函数，只执行一次对应的
NIR/IR3 后端编译，再生成完整的普通和 MSAA 程序记录。普通常量窗口与 full-window
特化分别判断。AIR→NIR 直编和 AIR/MSL→GLSL 回退共用此实现。

本轮未连接、部署、重启或测试设备。下列数据是 Linux x86_64 主机的无 shader 缓存
编译结果；不是 Snapdragon 835 时长，也不是首次亮屏或帧率测量。

## 原版驱动与框架依据

复核本地 iOS **18.7.10 / 22H374** 样本及既有反编译记录，重新计算原始
kernelcache Mach-O、IOGPU.framework 和 Metal.framework 的 SHA-256，均与已审计样本一致。
SDK 仍为 iPhoneOS 18.6。身份核验与所用缓存函数见
[native-evidence.json](../../build/gpu-cold-compile-20261021/native-evidence.json)。

- 原版 `IOGPUFamily 104.6.4` / `AGXG9P 328.5` 的调度层负责工作队列、依赖事件、
  提交与完成回收。已有[分层证据](ios18-iogpu-scheduling-and-reuse.md)支持将本轮改动放在
  用户态编译器；没有改变 GPU 完成、资源退休或调度优先级。
- `MTLCompilerFSCache::getElement`（`0x182452118`）和 `addElement`
  （`0x182451080`）位于原版 Metal 用户态编译基础设施，使用完整键与载荷摘要。
  QcomMTL 已有自身的函数磁盘缓存。替换为 CoreFSCache 存储后端本身不会减少真正
  未命中时的 AIR 特化或 IR3 工作，故本轮直接消除后端的重复编译。
- Apple 的[编译工作流说明](https://developer.apple.com/videos/play/wwdc2021/10229/)
  将 AIR、GPU 二进制与 BinaryArchive 区分处理；归档可复用已编译结果。
  本轮继续生成 A540 机器码，并使用现有 A540 归档与 JIT 校验，不复用 AGX 指令。
  没有引入 Metal 4 API。

原版编译器不公开其优化实现，以上证据用于确定分层与复用边界；具体 MSAA 等价条件
由项目固定版本的 Mesa/IR3 源码与离线对照验证。

## 实现与边界

修改集中在 [Backend.c](../../tools/gpu-compiler/Backend.c) 的 `compile_variant` 和
`compile_variants`。原来每个 fragment 总会编译 base/MSAA，存在常量窗口时再编译
window/MSAA-window。现在对每一对分别判定是否可以复用。

判定同时检查：

1. 原始准备态 NIR 未声明 `uses_sample_shading`，避免跳过 A540 的像素中心修正。
2. IR3 `key_mask.msaa` 未标记采样依赖。
3. 插值声明没有 sample/centroid 模式，实际最终 NIR 没有 sample、centroid 或
   at-sample 重心坐标指令。

不能仅依赖第 2 项：新增的 centroid 反例发现，standalone 路径在创建 key mask 前
可能没有刷新全部 `shader_info`。保留实际指令和声明检查后，该反例按原路径独立编译。
此限制来自本轮主机用例，不推断原版 Apple 编译器有同类问题。

符合条件时复制完整代码、立即数、寄存器和资源元数据，并设置原有
`QGPU_SAMPLE_MSAA` 标志；两个记录仍独立存在于完整 JIT image 中。不符合条件时
执行原来的独立 MSAA 编译。常量窗口失败后的回退、GLSL 回退和内存边界检查均保留。
没有新增持久编译器对象、后台任务或共享的可变 NIR。

Shader44 / JIT39 的结构及内核准入不变。构建仍通过运行时编译器及 adapter 的内容
生成 archive context，使旧构建缓存自然失效。Metal bundle 版本为 1.48.0。

## 无缓存编译对照

[benchmark-cold.py](../../tools/gpu-compiler/benchmark-cold.py) 冻结优化前后的主机库和输入，
两个库分别运行在独立进程，同绑 CPU 8，交替调用；每个输入先运行一次，再各测量
31 次。每次 AIR 调用重新解析、特化并生成全部 JIT 变体，IR3 磁盘缓存关闭。
计时包括编译器调用，排除进程/库加载、文件读取、IPC、结果哈希、Metal 反射、GPU
上传和 pipeline 对象创建。下表为线程 CPU 时间中位数。

| 样本 | 优化前 ms | 优化后 ms | 减少 |
| --- | ---: | ---: | ---: |
| Fluidity fragment，profile 4 | 32.542 | 18.658 | 42.7% |
| Fluidity fragment，profile 5 | 31.772 | 18.057 | 43.2% |
| Fluidity fragment，profile 6 | 46.947 | 26.147 | 44.3% |
| Fluidity fragment，profile 7 | 43.211 | 23.737 | 45.1% |
| 生成的显示数学 fragment，GLSL 输入 | 7.176 | 5.437 | 24.2% |
| 常量采样 fragment，GLSL 输入 | 6.271 | 5.455 | 13.0% |
| QuartzCore `fixed_frag_lph_cpf`，已捕获特化 | 22.297 | 22.180 | 0.5% |

Fluidity 的 profile 是三个布尔常量按索引 0/1/2 构成的位掩码。覆盖全部八种组合；
profile 0..3、Compute 和上述 QuartzCore 样本没有显著改善，约 −2.7% 至 +1.0%
的变化不作为收益。这里没有证明优化能消除 Welcome 首次停顿。

122 个输入均编译成功，优化前后的 **83,720 字节完整程序映像逐字节一致**，包含
代码、立即数、所有变体与元数据。31 次计时调用也逐次核对完整映像哈希。
全部样本、CPU/墙钟原始数据、库和输入 SHA-256 见
[comparison/report.json](../../build/gpu-cold-compile-20261021/comparison/report.json)。

复算示例（输出目录必须为新目录）：

```sh
python3 tools/gpu-compiler/benchmark-cold.py \
  --baseline build/gpu-cold-compile-20261021/before/build/gpu-compiler/host/src/freedreno/QcomGPUCompiler.so \
  --candidate build/gpu-cold-compile-20261021/comparison/candidate.so \
  --output build/gpu-cold-compile-recheck --repeats 31 --cpu 8
```

## 离线正确性与构建

- 生产编译器检查通过：原有内存边界、半精度、常量窗口、控制流、函数常量、并发
  串行化和拒绝路径，以及新增的普通片元、像素中心、sample/centroid、显式 centroid
  插值和 full-window 的独立编译对照。见
  [jit-check.json](../../build/gpu-compiler/jit-check.json)。
- AIR→NIR / GLSL→NIR 语义差分：Compute、QuartzCore 和 Fluidity 八种特化共
  **3,095 quads / 12,380 lanes**，零差异。抓取时强制独立编译每个变体，并与开启
  复用的生产库逐字节核对完整 JIT image。保留误差注入检查、buffer guards、1/2/4
  sample framebuffer 和常量窗口状态覆盖。见
  [DIFFERENTIAL.md](../../build/gpu-cold-compile-20261021/differential/DIFFERENTIAL.md)。
- `make gpu-metal` 完成 arm64 iOS 18.6 编译和链接；144 个已有 AOT 程序所在的
  `GPUPrograms.h` 与 `default.qmetallib` 均保持字节一致。
- 5 项 GPU 集成检查全部通过，见
  [integration-final.log](../../build/gpu-cold-compile-20261021/integration-final.log)。默认输出目录的
  GPU Kext 仍是历史 1.158，而当前源码为 1.173；本轮将当前 Kext 单独构建到本次
  `Extensions/` 后通过 `QCOM_GPU_KEXT_BUNDLE` 指定验证。第一次检查还遇到两个测试
  同时重建 `jit-test` 的文件忙冲突；最终按顺序运行后通过，首次日志保留。

生成的 Metal bundle 位于 `build/output/QcomMTLDriver.bundle`；冻结构建回执、源码与
产物哈希见 [final-receipt.json](../../build/gpu-cold-compile-20261021/final-receipt.json)。

历史 `hot-constants.txt` 原路径已不存在；本次语义测试恢复了之前冻结的同一份
常量输入，来源与 SHA-256 单独记录在
[restored-fixture.json](../../build/gpu-cold-compile-20261021/differential/restored-fixture.json)。

这些检查不执行 A540 ISA 或目标纹理硬件，也不覆盖真实沙盒缓存命中率。后续若继续
优化 QuartzCore，应单独分析其 AIR 特化/内联和采样相关后端耗时；不能用本轮部分
Fluidity 变体的收益替代整机启动或 UI 流畅度结论。
