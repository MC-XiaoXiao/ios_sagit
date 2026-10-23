# iOS 18 冷编译：随驱动分发精确的 A540 二进制

Metal 1.51.0 将已知系统 AIR 的反射和 A540 程序提前生成并嵌入插件。
匹配命中时不调用 LLVM、AIR lowering 或 IR3 编译器。此前已有的进程缓存、
CoreFSCache 和用户显式 BinaryArchive 继续服务其他请求。本次只进行了主机验证和
arm64 iOS 交叉构建，没有连接、刷写或测试设备。

## 为什么单纯加速编译器不够

历史记录 `build/setup-animation/hello-card-28-hot-transition-data/1-metal.txt` 中，
identity `234461aeb739725c`、argument `f1831668f182e7ba` 的一次 Function 为
200.134 ms；AIRLower 170.573 ms，Backend 24.356 ms，CompilerLock 等待约
0.000469 ms，ProgramRegister 约 1.520 ms。主要瓶颈在实际编译，不在锁等待。

本次先减少专门化后的无用 helper 优化，并在安全的 base/MSAA 配对内共享已完成
finalize 的 NIR。31 次独立新进程主机测量中，QuartzCore 样本编译 CPU 中位数
30.439 → 24.501 ms，下降约 19.5%；Fluidity 八种特化没有一致的额外收益。
测量包含第一次 AIR 解码与完整 JIT image 生成，不含进程/动态库启动、文件读取、
反射、Metal 对象创建、内核注册或 GPU 执行。不能把主机时间换算成 Snapdragon 时间。

因此，消除已知系统 shader 的现场编译才是本版本的主要手段。
如果上述 200 ms 请求精确命中目录，可移除约 194.929 ms 的 lowering/backend 工作，
即原 Function 的约 97.4%；剩余约 5.2 ms 是根据旧分段相减的估计，不是新版本实测。
现有短日志标识不足以证明该次请求的完整 AIR/常量已匹配，不能把估计当作已实现的真机结果。

## 对照原厂机制

分析本地 iOS 18.7.10 / 22H374 Metal.framework：

- `-[MTLCompiler compileRequestInternal:binaryArchives:failOnBinaryArchiveMiss:pipelineCache:sync:completionHandler:]`
  位于 `0x182436790`，创建多级二进制缓存、散列请求、查找已有代码；未命中才提交给
  compiler connection manager，且保留同步/异步参数。
- `MultiLevelCacheFactory::createBinaryCache` 位于 `0x18242e1b0`，区分归档、
  pipeline library 和文件缓存的路径。
- 反编译证据与输入哈希保存在 `build/gpu-cold-compile-1.51/native-compiler.json` 和
  `build/gpu-cold-compile-20261021/native-evidence.json`。反编译器有局部变量恢复警告，
  本结论基于符号及可见调用关系，未移植猜测出的私有数据结构。

Apple 官方也将提前生成 GPU 二进制、随软件分发以及异步准备作为减少运行期停顿的手段：
[WWDC22：Metal offline compilation](https://developer.apple.com/videos/play/wwdc2022/10102/)、
[Metal binary archives](https://developer.apple.com/documentation/metal/metal-binary-archives)。
AIR 中间码随 App 分发不等于 GPU 机器码已经生成；遇到新的代码/特化，原厂仍可能编译。
本工程使用 A540 IR3，不能直接执行 Apple GPU 二进制。

## 本版本实现与范围

`tools/gpu-compiler/SystemShaders.json` 固定原始输入的 SHA-256 和已知函数常量配置；
`system-shaders.py` 使用生产编译器离线生成 `GPUSystemShaders.h`。
目前有 **200 个去重后的反射入口、121 个可恢复程序变体**，零段压缩后的程序数据
784,032 bytes，约 766 KiB；这是数据体积，不是整个驱动增量大小。

输入为现有 QuartzCore/default.metallib、MercuryPosterExtension/default.metallib，
以及保存的 Fluidity 原始 AIR。覆盖 `fixed_frag_lph_cpf` 的四种已知配置、
`fixed_frag_lph_cph` 的三种配置和 Fluidity fragment 三个布尔常量的全部八种组合。
200 个反射入口不意味着 200 个入口的所有特化均有机器码；无法编译的自动候选在报告中
明确记录，显式要求的配置编译失败会使构建失败。相同 AIR/入口只保留一份。

查找规则是完整 AIR SHA-256、入口名、解析后的常量索引/类型/有效字节全部匹配。
原有 MTLB 校验先验证实际 AIR 内容；目录恢复后仍执行完整 JIT ABI、registry、stage、
名称校验，再走现有内核程序注册。没有按照函数名替换源码、猜测未提供常量或跳过 draw。
未匹配的程序走原有缓存/编译路径；显式 archive 的 fail-on-miss 语义保持原样。

常规命中路径还使用内置反射，避免为了读常量声明先启动 LLVM。
Function timing 的 detail 新增 `0x400`（内置反射）、`0x800`（内置程序）。
开启 shader capture 诊断时，沿用已有归档捕获逻辑重建 GLSL，可能额外运行 AIR lowering；
性能结论只适用于未开启该诊断的正常路径。

非目录请求也得到两个独立改进：

1. specialization 先传播常量并删除死计算，再丢弃已无 IR 引用的 helper body，之后才做
   InstCombine；保留选中入口、AIR 元数据声明、地址被引用的函数和递归环。
2. 无 sample shading 的 base/MSAA 对共享 finalize 后的 NIR，但仍独立生成需要不同
   key 的 ISA；不跨 full-window 配置共享。使用 IR3 的受所有权管理 variant API，
   使 variant 随 shader 正常释放。

## 离线验证

- 121 个目录程序与修改前编译器生成的完整 83,720-byte image **逐字节一致**。
- 另有 122 个 GLSL/AIR 回归样本的完整 image 一致，且双方编译必须成功。
- 生产编译器检查通过，覆盖内存边界、常量窗口、MSAA、函数常量、并发解码缓存及失败路径。
- AIR→NIR / AIR→GLSL→NIR 的 CPU quad 差分通过：3,095 quads、12,380 lanes，
  当前样本零误差，包括 base/MSAA/window/MSAA-window。没有执行 A540 ISA。
- 目录查找、解压、完整校验和 dead-helper/PHI/回边/元数据测试通过 ASan、UBSan。
- 生成器只有在所有压缩 image 解码后与原 image 的 SHA-256 一致时才发布新头文件。
  生成阶段的恢复测试不链接 LLVM 或 GPU 编译器。

主机首次查找、反射、常量解析、恢复与校验的中位数约 **20 µs**。
不包含 iOS 的库加载、ObjC 对象、源库散列、首次缺页、内核注册和 GPU 执行，
不能声称真机 Function 已从 200 ms 降到 20 µs。
主要证据目录为 `build/gpu-cold-compile-1.51/`，完整生成报告为
`build/gpu-system-shaders/report.json`；最终交付索引为 `handoff.json`。

## 构建与系统集成

执行 `make gpu-metal JOBS=3`。输入库须与 manifest 哈希一致；缺失或变化时直接失败，
必须明确更新输入与覆盖配置后重新生成，避免旧系统二进制误配。
生成物嵌在 `QcomMTLDriver.bundle/QcomMTLDriver`，随整个 bundle 部署到
`/System/Library/Extensions/QcomMTLDriver.bundle`；无需给每个 App 安装一个缓存文件。
`build/output/gpu-rootfs` 仅为离线 overlay，本任务没有安装它。

本候选的 AOT registry 为 **94ff5d0137498ac2**（144 个 AOT 入口），
与本任务开始时的 `GPUPrograms.h` 相同；它不同于先前设备日志中的
**5929f35a948802cd**。集成必须使用相同 registry 的 GPU Kext、默认程序库和本插件，
不能把此 bundle 单独配到旧设备 registry。归档 context 包含生成目录、编译器、
相关源码和 ABI 输入；最终值及哈希以冻结交付文件为准。

普通 UIKit/Core Animation 用户共享系统着色器，覆盖这些变体可以让多个 App 受益；
自定义 Metal、游戏、滤镜以及系统新增的常量组合仍可能在首次使用时编译。
之后通常由进程对象或缓存复用，并非每帧重编译。要覆盖更多首次请求，应把确切输入和
特化加入离线目录；同步 API 遇到未知代码必须生成正确程序，不能用延迟 draw 冒充无卡顿。
当前离线结果证明已覆盖路径能够跳过编译，尚不能证明所有 App、所有场景都不可感知。
热态 GPU 绘制、映射和显示提交的停顿也不属于本次冷编译收益。
