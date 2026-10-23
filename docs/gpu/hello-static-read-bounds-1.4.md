# Metal 1.4：静态只读 fragment 边界判断

2026-10-19，继续离线优化 framebuffer shader。本轮未访问设备，没有新的 GPU 时间、像素或 FPS 验收，约 60 FPS 的目标仍未证明。

## 实现与等价范围

Metal 1.3 的静态读取仍生成 `size >= span && size - span >= offset` 及分量回绕检查。即使地址是常量，后端也保留了部分减法、布尔临时值和尾部分量读取。

本轮对**只读、非 volatile / coherent 的 fragment SSBO 常量 32 位偏移读取**，在编译时用 64 位整数算 `end = offset + relative + span`。只有 `end <= UINT32_MAX` 时生成 `size >= end`；越过该上限或 half 地址为奇数时结果为 false。没有扩大合法范围，也没有假设绑定一定完整。

等价性来自原有无符号 guard：未回绕时，`size >= span && size - span >= at` 等价于 `size >= at + span`；分量地址回绕或末端超过 32 位可表示长度时，原判定也不可能接受该分量。`end == 2^32` 同样不可接受，因为逻辑长度是 uint32。动态偏移、shared、store、atomic、volatile 和 coherent 保持原路径；32 位读取的原有对齐行为也保持。

完整 half 向量仍在其范围判断内加载，禁止向外推测执行；部分向量仍逐分量读取并将无效分量置零。向量完整条件为 false 时，末分量必定无效，现在 Mesa 可以证明这一点并删除对应读取。half 算术、原始 AIR、函数常量、前端 GLSL / reflection、GPU 常量窗口及其 cache 发布 / fence 均保持。

Apple 原始 AIR 的 `CA::OGL::Metal::Uniform` 是 224 字节、16 字节对齐、只读 address-space 2，矩阵区域 `[176,216)`。仍用其完整捕获和原函数常量重放。[原始元数据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-vector-buffer-loads/apple-hot.ll:8813)

## 真实热点

| 项目 | Metal 1.3 | Metal 1.4 |
| --- | ---: | ---: |
| base / MSAA code words | 864 / 864 | 672 / 704 |
| full / half registers | 6 / 26 | 6 / 20 |
| 窗口有效标志判断 | 1 | 1 |
| 静态 LDGB，含冷回退 | 27 | 22 |
| constlen | 40 vec4 | 40 vec4 |
| base hash | `85db5ac1599a4f7a` | `80bdefd21c667f9f` |

静态 LDGB 的减少来自五行向量各少一条必定无效的尾部标量读取；完整 uniform 窗口的常量路径仍跳过整个矩阵 SSBO 回退。新增改动也简化了 bias / noise 的静态读取边界。指令和寄存器减少是编译事实，不能换算 GPU 用时或实际帧率，也不能据此推断占用率。

## 离线验证

- 新增 **1,768** 个真实生产 NIR 差分案例：同一输入分别使用 literal 偏移与保留原 guard 的 symbolic 偏移，检查每次 surviving load 的范围及逐分量原始位值。覆盖 16/32 位、1–4 分量、短绑定、奇数 half、接近 UINT32_MAX 的地址、分量回绕和 `end == 2^32`。
- 原 **2,700** 个 half 向量案例、**832** 个窗口案例（128 个启用路径没有 SSBO 读取）、窗口调度 / SSA 边界及完整 MSL / AIR / IR3 回归通过。
- 135 个 AOT 的名称、顺序和 ID 保持；**104 个 code / immediates（含 MSAA）逐字节不变，31 个 fragment 程序改变**。其中原 133 个内改变 30 个，另一个是已有常量消费者诊断；compute / vertex 指令和 GPU 生产者保持。registry 为 `72b191f4da4d7b92`。
- AOT 的取舍逐项记录：[变化清单](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-static-read-bounds/AOT-preservation.json)。不能概括为所有寄存器都下降，例如 `buffer_color` 的 full 6→8、half 5→3；真实热点为 full 6 保持、half 26→20。
- 原生 arm64 编译器、Metal1.4 适配器 / 工具与 GPU1.140 Kext 已构建。ABI 仍为 v36，description 2,464 字节、image 41,912 字节。Display1.0.18 保持。
- 当前 AOT 的 GPU submit 编码 / MMIO / fence / 私有页表 / FIFO 模型及 8,224 个窗口范围 / packet 案例通过 ASan / UBSan；这些模型不执行实际 ISA。完整内核 74 个预期 bundle、23 个本地可执行文件和 HFS 的 13 个文件读回校验通过。另有 145,900 个独立整数恒等式案例通过。

配套[内核](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/kernelcache.ui-static-read-bounds)、[覆盖镜像](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/drivers-ui-static-read-bounds.hfs)和 [trust cache](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-static-read-bounds.trustcache)已完成离线审计，未生成完整 EFI。前一 Metal1.3 组件及其他任务的设备身份 / 壁纸 / displayctl 修改保留。

当前阶段没有运行新 GPU 生产者→常量消费者诊断，也没有对这 31 个 fragment 的实际 GPU 像素作验收。恢复硬件阶段后，应对合并候选运行窗口路径 / 像素检查、受影响的 render-state / framebuffer / fence 回归，再采集复杂 Welcome，计时关联真实屏幕电源状态。

证据：[审计](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-static-read-bounds/audit.json)、[编译器检查](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-static-read-bounds/host-check-final.json)、[驱动模型检查](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-static-read-bounds/driver-checks.log)、[源码补丁](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-static-read-bounds/changes.patch)。
