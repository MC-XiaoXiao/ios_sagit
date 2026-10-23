# Metal 1.1 framebuffer shader 离线优化

2026-10-19。按用户要求只做离线实现与验证，未访问 UART / Fastboot，未启动或修改真机。

## 改动与依据

以 round 156 导出的真实 Apple `fixed_frag_lph_cpf` AIR、函数常量及 round 157 / 158 使用的 Metal 1.0 GLSL 为输入。原始 AIR 明确使用只读常量地址空间和 `<4 x half>` 读取；Uniform 参数声明大小为 224 字节、对齐为 16。实际热点矩阵读取五行，覆盖 byte offsets 176–215。

此前前端将每行拆成四个 half 读取，后端逐分量的安全检查使 20 条 scalar LDGB 保留到 A540 ISA。Metal 1.1 在边界检查前使用固定版本 Mesa 的 NIR 向量化器合并**常量偏移、只读、非 volatile 的 fragment half 读取**，保留其原有别名 / barrier 检查，并额外拒绝跨 uint32 地址回绕的合并。无此类输入的 shader 跳过新增预处理。

每组完整有效时执行一条 `LDGB u16.4`；绑定短于完整向量时，仍按原规则读取每个有效 half、对无效分量返回零。向量读取仅能在完整范围检查的 then 分支执行，移除其 `ACCESS_CAN_REORDER`，防止后续 peephole-select / GCM 将它投机执行到检查之前。不会将短绑定回退成在 byte 0 读取完整向量。

动态偏移、可写 / volatile 输入、compute / vertex 和 store 的处理保持原路径。没有常量 CPU 快照、扩大可读范围或修改原 half 算术；GPU buffer 数据及生产者 fence 仍由原路径管理。fragment discard 的状态变量在预处理之后建立，避免 SSA 转换使后续写入屏蔽读取未初始化变量。

## 真实热点的离线结果

新身份为 `6006761a9eb7f18e`。原 AIR + 原函数常量重放与从 Metal 1.0 GLSL 编译的新完整产物一致，前端 GLSL 和 reflection 逐字节未变；base / MSAA 都通过现有 JIT v35 描述验证。

| 项目 | Metal 1.0 | Metal 1.1 |
| --- | ---: | ---: |
| 矩阵五行完整有效时的读取指令 | 20 条 scalar | 5 条 half4 vector |
| 此路径全部 LDGB，含 bias / noise | 22 | 7 |
| 矩阵读取字节数 | 40 | 40 |
| code words，含尾部回退与 padding | 640 | 800 |
| full registers | 16 | 6 |
| half registers | 19 | 26 |
| branch stack | 1 | 1 |

总静态 ISA 实际包含 **27 条 LDGB**，其中 20 条属于保留的尾部标量回退。7 条只描述五行均完整有效时的路径，不能说总静态指令从 22 减到 7，也不能据此声称带宽或实际 GPU 耗时下降。寄存器分布和代码长度均有代价，尚未测得运行收益或 occupancy。

原有单次 framebuffer 输入物化保持；像素精度及 framebuffer / buffer / sampler 等接口没有改变。两个参考程序的完整 image 仍逐字节一致：普通纹理 `48fd06ac66534652` 和小 framebuffer 变体 `c98564e9d2e5945b`。133 个 AOT 程序、registry `064b848e6ad7be1c` 与既有 GPU1.137 内核参考保持一致。

## 验证与交付

生产 NIR 的 **2,700 个案例通过**，包含 190 次完整向量路径与 483 次部分路径。测试用真实向量化 / guard / 优化 pass，先验证每个残留读取都在逻辑绑定范围内，再注入按地址区分的 16 位值检查所有输出分量；覆盖 0 / 1 / 2 字节绑定、half2 / half3 / half4、奇数地址、尾部、uint32 回绕、可写和 volatile。还在 descriptor 长度未知时运行实际 select 优化，检查向量读取仍位于受保护的 then 分支。

完整 MSL / AIR / IR3 编译器回归、既有 atomic / discard / shared-memory guard 检查、arm64 iOS 原生编译器及适配器构建通过。初始较宽的预处理版本曾使完整回归失败；最终实现显式限制到目标输入，原失败 receipt 保留，只有最终 source / library hash 对应的通过结果用于验收。

已生成 [离线驱动覆盖镜像](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/drivers-ui-vector-buffer-loads.hfs) 和对应 [trust-cache module](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-vector-buffer-loads.trustcache)，**GPU1.137 / Metal1.1 / Display1.0.18**。12 个 HFS 文件均独立读回校验；只更新 Metal adapter、原生 compiler、版本 plist 和与实际二进制一致的构建 receipt。编译器 cache context 已变化，避免复用旧 ISA。

工作区另有 README、wallpaper 构建入口和 displayctl 的变更，已保留并单独记录来源差异；没有将新 displayctl 放进本候选，也没有修改根文件系统。交付为驱动覆盖镜像，**未生成新的完整 EFI 启动镜像**，更未真机测试。

后续真机只需对这批合并改动验证实际像素和复杂 Welcome 的 GPU 时间、实显节奏；用显示电源状态排除疑似熄屏窗口。当前不能声称提高帧率、无闪烁或达到 60 FPS。

证据入口：[实现审计](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-vector-buffer-loads/audit.json)、[base / MSAA 指令分析](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-vector-buffer-loads/shader-analysis.json)、[完整编译器检查](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-vector-buffer-loads/test-compiler-final.json)、[产物校验](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-vector-buffer-loads/artifact-verification.json)。
