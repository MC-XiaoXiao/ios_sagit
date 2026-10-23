# Metal 1.3：合并 framebuffer shader 的常量窗口判断

2026-10-19，继续按用户要求离线优化；没有访问 UART、USB、Fastboot 或真机。约 60 FPS 的目标尚未实现或验证。

## 发现和实现

Metal 1.2 的真实 Apple 热点 `fixed_frag_lph_cpf` 对同一矩阵窗口生成了五次 `c32.x != 0` 判断，各自包含完整的 SSBO 冷回退。核对 Mesa 的 `nir_opt_if.c::opt_if_merge`：已有优化只在两个相同条件之间仅剩 phi 时才合并。原矩阵读取与 half 运算交错，使它没有机会合并全部判断。

生产编译器现在在已有窗口收集之后，将**同一基本块、同一窗口**的独立静态只读 half 加载集中到第一次加载处，仅跨越纯 SSA ALU、literal 和 deref 指令。后续加载的 literal binding / offset 在新位置重建，保持定义支配使用。移动保持加载原有顺序，一组最多 32 个 half 分量；其余代码仍由原有 NIR guard 和 Mesa if 合并处理。

遇到其他内存读取、volatile / coherent、不同 buffer、纹理、barrier、输出或控制流边界即停止。这没有提前读取另一个分支中的数据，也没有去掉原逐分量越界回退。half 算术、精度属性、运算顺序、窗口有效条件、内核 GPU 发布及 fence 均保持原实现。

核对的 Apple 原始 AIR 将 `CA::OGL::Metal::Uniform` 声明为只读 address-space 2、224 字节、16 字节对齐；矩阵区域为 `[176,216)`，五行 half4。本轮仍使用此前导出的完整 AIR 和原函数常量重放，前端 GLSL 和原 reflection 前缀逐字节一致，没有修改原矩阵数学表达式。

原始依据：[Apple AIR 元数据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-vector-buffer-loads/apple-hot.ll:8813)、[Mesa 相同条件合并](/home/xxiao/Projects/MI6_iOS/build/gpu-compiler/mesa/src/compiler/nir/nir_opt_if.c:1227)。

## 真实 shader 的生成结果

| 项目 | Metal 1.2 | Metal 1.3 |
| --- | ---: | ---: |
| 同一窗口有效标志判断 | 5 | 1 |
| base code words | 896 | 864 |
| MSAA code words | 928 | 864 |
| full / half registers | 6 / 27 | 6 / 26 |
| constlen | 40 vec4 | 40 vec4 |
| 静态 LDGB（包含冷回退） | 27 | 27 |
| base program hash | `6c69f37293d6f5a7` | `85db5ac1599a4f7a` |

仍是 buffer 1 的 `[160,224)` GPU 常量窗口；完整窗口由 GPU 执行时加载，CPU 不读取 uniform 数据。代码和判断减少是离线事实，不能据此推导缓存命中、占用率、GPU 时间或 FPS。集中加载也可能改变活跃值区间，必须在真实复杂 Welcome 中计时确认净收益。

## 验证与产物

- 完整 MSL / AIR / IR3 编译器回归通过，含原有 2,700 个 half 向量边界案例。
- 832 个生产窗口案例改为将读取和真实 ALU 消费交错，核对原始 half 位值及其计算输出；128 个启用路径没有 SSBO 读取。覆盖八个 buffer、短绑定、半行尾部、符号零、NaN / Inf 位型等原有输入。
- 新增纯 ALU 可移动、volatile / coherent / 其他 buffer / barrier / 输出不可跨越的 NIR 调度检查，并显式检查移动后的 SSA 源先于使用，包含 NDEBUG 构建。
- 原 133 个 AOT 程序和 GPU 生产者诊断的 code / immediates（含 MSAA）逐字节不变。只有新增的 `constant_window_half` 消费者指令改变，条目总数仍为 135；registry 更新为 `465b990a90071c98`。
- ABI 仍为 v36：description 2,464 字节，image 41,912 字节。原生 arm64 编译器、适配器、工具和配套 GPU1.139 Kext 已构建。组件版本为 **GPU1.139 / Metal1.3 / Display1.0.18**。
- 8,224 个生产窗口范围 / 对齐、metadata、真实 packet 顺序及清零 / 重绑定案例通过 ASan / UBSan。完整内核的 74 个预期 bundle 和 23 个本地可执行文件校验通过；HFS 的 13 个文件独立读回。覆盖镜像只改变五个 bundle 文件，gpuctl、metalctl 和 displayctl 二进制沿用原字节。

匹配的[内核](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/kernelcache.ui-window-guard-coalescing)、[HFS 覆盖镜像](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/drivers-ui-window-guard-coalescing.hfs)与 [trust cache](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-window-guard-coalescing.trustcache)已完成离线审计；没有生成完整 EFI。前一 GPU1.138 / Metal1.2 冻结组件保留。当前工作区独立的设备身份、壁纸、displayctl 等修改保持原样。

待硬件阶段恢复后，将这一批合并候选一次验证：运行已编译的 GPU 生产者→常量消费者像素及路径命中诊断、framebuffer / fence 回归，再采集实际复杂 Welcome。计时须关联真实屏幕电源状态，不把疑似熄屏间隔当成动画长帧，也不通过删除这些点宣称更高帧率。现有诊断尚未在真机执行。

证据：[审计](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-window-guard-coalescing/audit.json)、[编译器检查](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-window-guard-coalescing/host-check-final.json)、[AOT 保持检查](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-window-guard-coalescing/AOT-preservation.json)、[源码补丁](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-window-guard-coalescing/changes.patch)。
