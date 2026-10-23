# iOS 18 显示着色器补齐与离线验证

本轮更新 Metal bundle 为 **1.31.0**，补齐原版 QuartzCore AIR 中已定位的编译缺口。
Shader43 / JIT38 的结构与准入规则不变。没有连接、启动或写入设备，没有进行真机测试。
离线目录为 `build/gpu-ios18-shaders/`。

## 分析依据和范围

沿用项目的 iOS18.7 / 22H374 QuartzCore `default.metallib` 原始提取件：
SHA-256 `8ca2bef447981ae26acbe3c5023043ad9dcc312e893f44781d7373b1e27adfa7`。
通过生产 MTLB 解析器核验每个 AIR 模块的哈希，逐入口检查库中的178个函数。
不修改 Apple AIR，不按函数名替换实现，不为缺少的必需函数常量猜值。

修改前92个入口能直接降低，修改后100个；原有92个的完整降低结果逐项一致。
这是给定空函数常量集合下的入口调查，**不代表178种运行时管线的兼容率**。
记录见 `index.json`、`audit-before.json`、`audit-after.json`。

## 新增实现

- 两条 AIR 前端均支持运行时 `air.any/air.all` 的2..4分量布尔向量，不再依赖函数常量特化时的预处理。
- 支持标量及2..4分量 FP16/FP32 的 `exp`、`log`、`atan2`、`powr`、`fmin3`、`fmax3`，包括相应 `air.fast_` 名称；严格检查参数个数、精度、向量宽度和名称一致性。
- `atan2(y,x)` 保持参数顺序；三参数极值保持 `op(x,op(y,z))` 顺序。普通极值处理单个 NaN，fast intrinsic 或函数明确声明 `no-nans-fp-math=true` 时省去额外选择。
- 2D/2DArray 的常量纹素偏移 `int2(-8..7)` 接入隐式 LOD、显式 LOD、梯度采样。偏移直接进入 `textureOffset/textureLodOffset/textureGradOffset`，保留纹理过滤、mip、数组层及镜像坐标规则。
- `read` 的平面偏移与 `sample` 的纹素偏移分别检查。非零偏移须有对应标志；动态偏移、范围外偏移、非零 read 平面偏移继续明确失败。

上述数学操作和纹素偏移的语义依据 [Apple Metal Shading Language Specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf)，
AIR 签名以同版原始模块为准。本轮没有采用新系统专有的 Metal 4 接口。

实现集中在 `tools/gpu-compiler/AIR.cpp`、`AIRRuntime.cpp`、`Semantic.h`；
两条 LLVM API 适配分别验证输入，共用表达式生成。目标使用的新增 LLVM C 符号已由本地 iPhoneOS18.6 `libLLVM.tbd` 及交叉链接核对。

## 性能处理

没有新增 CPU 像素处理、GPU 提交、资源上传或逐纹素软件采样循环。
零偏移保持原来的 GLSL 路径；常量采样器且无需镜像时保留直接硬件纹理指令。
FP16 超越函数沿用 f32 数学降低后收窄一次的明确精度路径；极值保留 half 运算。
不依据 FP16 类型名称推断执行更快，也不对未测量的目标 GPU 宣称精度或吞吐量符合完整 Metal family。

现有138个 AOT 程序的元数据、代码及立即数全部逐项一致，registry 保持
`0fb81c61d72aab78`。因此这些已有程序没有引入机器码、寄存器或常量量的变化。
完整比较见 `aot-regression.json`。

新增通过的原版函数在生产 A540 编译器中的静态占用如下。codeWords 包括硬件对齐，
寄存器数为编译器描述字段；它们是静态指标，不能换算为实际帧率或 GPU 耗时。

| 函数族 | FP32 codeWords / fullRegs / halfRegs | FP16 codeWords / fullRegs / halfRegs |
| --- | --- | --- |
| resolve_blur_weight_frag | 288 / 6 / 4 | 288 / 6 / 7 |
| variable_blur_downsample_frag | 96 / 6 / 0 | 96 / 4 / 5 |
| conic_shading_frag | 288 / 8 / 2 | 320 / 8 / 5 |
| chromatic_aberration | 576 / 6 / 8 | 608 / 6 / 15 |

普通和 MSAA 变体均通过生产 JIT profile。两条前端的 GLSL 存在等价常量拼写差异，
8个函数的全部代码变体和立即数逐字节一致，见 `native-compilation.json`。

## 已完成的离线检查

- `test_gpu_compiler()`：现有生产内存编译、资源边界检查和新增数学/布尔/偏移用例通过；包括 FP16、FP32、NaN 快路径、梯度采样及非法偏移拒绝。
- 主机 ASan/UBSan 的 `gpu-submit`、`gpu-constant-window` 检查通过。
- `make gpu-metal` 对应的 iOS arm64 bundle、运行时编译器和 metalctl 构建通过；`make gpu-driver gpu-client` 通过。
- `test_gpu_integration.py` 的5项检查通过。
- CPU llvmpipe 执行生产生成的 FP32 数学 GLSL，6组输入的最大绝对误差约 `1.69e-6`；抽取生成的三参数极值表达式，6组 NaN 位置检查与独立模型一致；梯度偏移的3组纹素、边界和镜像坐标检查逐像素一致，见 `math-semantics.json`。这不运行 iOS、IOGPU 或 A540。

生产检查报告为 `build/gpu-compiler/jit-check.json`，构建和集成日志保存在本轮离线目录。

## 仍需实现或另行分析

本轮调查仍有78个入口未直接降低，其中6个明确缺少调用方必需的函数常量；其余涉及额外入口语义、
GammaLUTs buffer 元数据、动态采样偏移、整数颜色输出、其他纹理维度/访问类型、较高 buffer 槽位、
raster-order group，以及一个主机 LLVM InstCombine 不收敛错误。具体入口及原始诊断保留在 `audit-after.json`。
其中 `simple_rim_lpf/lph` 使用动态偏移，本轮未把它展开成256路采样或软件纹理循环。

完整 argument buffer、通用指针/内存模型、动态链接、Tile/Mesh 和完整 Metal family 仍不在当前实现范围。
本轮不把离线编译成功等同于显示效果或动画性能通过。
