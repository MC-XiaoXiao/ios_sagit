# 真实 Welcome 矩阵的 GPU 常量缓存可行性

2026-10-19。继续约 60 FPS 的目标，遵循当前只做离线优化的约束。本轮得到编译和语义证据，**没有完成驱动接入，也没有新的帧率结果**。

## 为什么继续这一条路径

Metal 1.0 的实测热点约占 GPU 作业累计时长的 68%。Metal 1.1 已将完整矩阵的每 fragment 全局读取从 20 条 scalar 合并为 5 条 half4，但这些仍是每 fragment 的 SSBO 读取。Apple 原始 AIR 使用只读 constant Uniform，矩阵可以研究由 GPU 在 draw 前搬入常量寄存器，不需要 CPU 读取 / 固化数据。

固定版本 Mesa 的 `fd5_emit_const_bo` 确实使用 A5xx `CP_LOAD_STATE4`、`SS4_INDIRECT`、`ST4_CONSTANTS`；其目的偏移和长度检查要求 64 字节组织。通用常量提交原实现还检查 source offset 的 16 字节对齐。不能直接把 40 字节矩阵当作合法的间接加载长度，也不能假设 native buffer 的起点满足所需对齐。

原始 Apple Uniform 声明为 224 字节，热点矩阵范围为 `[176,216)`。候选窗口 `[160,224)` 恰好为 64 字节且 source offset 对齐 16；运行时仍必须检查**实际绑定长度和 GPU 地址**，不能把 AIR 声明当作实际授权长度。短绑定或不支持的对齐应关闭缓存路径，继续 Metal 1.1 的安全回退。

原始依据：[A5xx 常量包](/home/xxiao/Projects/MI6_iOS/build/gpu-compiler/mesa/src/gallium/drivers/freedreno/a5xx/fd5_emit.c:59)、[原生 source offset 检查](/home/xxiao/Projects/MI6_iOS/build/gpu-compiler/mesa/src/gallium/drivers/freedreno/ir3/ir3_const.h:153)、[PM4 字段](/home/xxiao/Projects/MI6_iOS/build/gpu-compiler/mesa/src/freedreno/registers/adreno/adreno_pm4.xml:706)、[原始 AIR 解码](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-vector-buffer-loads/apple-hot.ll:8813)。

## 隔离原型结果

仅在 `build/gpu-runtime/ui-constant-bank-study/` 构建独立 host library，使用固定 Mesa 静态依赖；生产源文件、host library、Metal 1.1 覆盖镜像和原生 bundle 均未替换。

原型用一个假设的 kernel-owned 有效标志选择常量路径 / 原路径，按整数位操作将两个 32 位常量字拆成 half4，没有 float32 往返转换。研究模型固定映射原生 slot 9、source `[160,224)`、constant words `[128,144)`、flag word 144，**还不是通用、可部署的元数据协议**。

| 实际热点重编译 | base | MSAA |
| --- | ---: | ---: |
| code words，含完整回退 | 896 | 928 |
| full / half registers | 6 / 27 | 6 / 27 |
| constlen，vec4 单位 | 44 | 44 |
| branch stack | 1 | 1 |

base 身份 `2a8173811f5b8343`。实际反汇编有基于常量 flag 的分支、常量字的整数移动 / 位移及跳过矩阵 LDGB 回退的跳转。整个静态程序仍有 27 条 LDGB；不能说静态 LDGB 已被消除。

生产 NIR 变换加模型数据检查的 **520 个案例通过**，其中 **80 个启用路径在优化后没有矩阵 SSBO 读取**。短绑定走原逐分量回退；启用时结果与按源地址区分的 half 原始位值一致，含负数 / NaN 位模式。另以 ASan / UBSan 检查 **8,224 个长度 / 对齐组合**、PM4 golden、溢出 / 范围拒绝和 **65,536 个 half 位值的整数 pack / unpack**。这些分别是 NIR 模型和包 / 算术模型检查，不是 GPU 执行、原生驱动或物理缓存一致性验收。

## 生产接入还缺什么

下一批应一起完成以下内容，不能只把 shader 换成原型：

1. 增加明确的 program window 元数据及新 wire version；包含 buffer 绑定和 source offset，目的常量槽由可信规则推导，禁止复用旧字段。同步 JIT / AOT / kernel / archive / capture / profiler hash，拒绝旧、新格式混用。
2. 编译器按真实只读常量访问收集有限数量的窗口，只有 metadata 覆盖的读取才能使用常量，保留所有其他读取和逐分量回退。所有 constant words 与 immediates / driver constants 必须互不覆盖。
3. kernel 根据当前作业实际绑定验证长度、完整映射、对齐和地址范围，并排除同 draw 写别名。每次新 setup 初始化所有有效标志为 0，只有合法且成功编码的间接加载才置 1，不能沿用旧 draw 的有效标志。
4. 在实际 GPU 生产者完成、写缓存发布之后，按 HLSQ allocation update→普通常量→间接常量→draw 的顺序加载。不能仅凭只读限定符跳过真实依赖；CP 读取与此前 GPU 写入之间的可见性仍需核对。
5. 基于真实原生产生数据验证 compute / render→constant window→fragment、短绑定 / 对齐回退、重绑与状态复用，最后采集复杂 Welcome 的 GPU 和亮屏实显时间。直到这些完成，原型不能部署。

建议 wire 扩展：`count` 加至多四个 `{buffer, byteOffset}`，新增 36 字节；每窗口固定 64 字节。若采用这个布局，description 为 2,464 字节、image 为 41,912 字节，需从 v35 升到新版本。后续核对确认现有元数据前缀实际为 112 words；本轮隔离原型额外保留 16-word 间隙，使用 word 128–143 / flag 144。生产接入应从宏推导布局，不复制原型的数值；每窗口 16 words，flags 使用一个完整 vec4，prefix 为 `112 + 16*count + 4`。本报告中的原型测量仍对应上述隔离布局；生产 ABI 接入另行记录。

Metal 1.1 仍是可供后续测试的离线候选；原型 library / image 全部标记 research-only、不可部署，未生成 HFS 或 trust cache。未操作设备，未验证 60 FPS。

证据：[研究审计](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-constant-bank-study/audit.json)、[NIR 模型检查](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-constant-bank-study/model-check.log)、[包 / 位值模型检查](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-constant-bank-study/window-contract-check.log)、[原型 base ISA](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-constant-bank-study/base.research.ir3.txt)。
