# GPU 1.138 / Metal 1.2：有界 GPU 常量窗口

2026-10-19。按当前要求继续离线实现；本轮未访问 UART / Fastboot、未启动真机、未修改根文件系统。约 60 FPS 的目标仍未验证。

## 生产接入

此前隔离原型已经验证真实 Apple framebuffer shader 的矩阵可由常量寄存器读取。本轮完成通用编译器、wire、内核编码和原生诊断接入：

- 编译器从静态、只读、非 volatile / coherent 的 fragment half 读取收集至多四个 64 字节窗口，每个 buffer 至多一个；没有固定 shader 名称或 slot。含实际 SSBO 写入的 shader 不收集窗口。
- 新 JIT ABI **v36**：description **2,464 字节**，image **41,912 字节**，包含 count 和四个 `{buffer, byteOffset}`；闲置记录为零，绑定顺序规范化，旧 v35 拒绝。AOT 导出、native loader、archive context 和 kernel profiler identity 一起更新。
- 常量布局从真实的 **112-word** 元数据末尾推导；每窗口 16 words，所有数据后放一个完整 vec4 的有效标志。prefix 为 `112 + 16*count + 4`，立即数 / driver constants 不得覆盖窗口。原型曾额外留了 16-word 间隙，生产代码没有复制该硬编码。
- 内核使用当前 draw 已解析并持有的 GPU binding 规划源地址，检查实际逻辑长度、地址溢出、设备范围、16 字节 source 对齐及 64 字节 destination / count 组织。有任何可写 SSBO 时保守回退。已有 native admission 对真实物理 backing 的附件 / buffer 写别名检查保持。
- 每次新 setup 清零有效标志。有效源先经真实 `CACHE_FLUSH_TS`、对应时间戳等待和 CP memory-write wait，再按 **HLSQ allocation update → 普通常量 → GPU 间接常量 → draw** 编码。数据在 GPU 执行时读取，不做 CPU 常量快照；同一 draw 的扩展图元可以保留其常量状态。
- 无效、短或不对齐的绑定保留原逐分量边界处理。half 数据用整数移位 / 位截取进入原 half 运算，保留位值语义。新增更宽 NIR 向量的拒绝检查。native 编译若因新增冷回退超过代码 / 寄存器 profile，会清理两个 variant 并重试未提升版本。

本轮没有修改原 half 算术、framebuffer 输入单次物化、原位免拷贝、ASID / 页表、GPU 调频、显示扫描或完成 fence 规则。

## 真实 Apple shader

使用此前导出的原始 AIR 与原函数常量重放，不用同名简化 shader 代替性能目标。前端 GLSL 逐字节不变，原 reflection 的 2,428 字节前缀也不变。

| 项目 | 生产候选 |
| --- | ---: |
| 真实热点身份 | `6c69f37293d6f5a7` |
| 窗口 | buffer 1，`[160,224)` |
| 真实矩阵使用范围 | `[176,216)` |
| base / MSAA code words | 896 / 928 |
| full / half registers | 6 / 27 |
| constlen | 40 vec4 |

窗口有效时，矩阵读取走 GPU 加载的常量字；原 LDGB 路径作为回退保留。代码和 half 寄存器有增长，还新增一次条件性的真实全局缓存发布，实际 GPU 时间需要测量，不能据此换算 FPS。

原有 **133 个 AOT 程序的 code / immediates，包括 MSAA，逐字节保持一致**。新增两个原生诊断程序，总数 135；registry 为 `a52d1cc3dce54a71`。GPU 内核与 Metal 覆盖镜像必须成对使用，不能搭配旧 GPU1.137 / ABI35 内核。

## 离线验证

- 原有 **2,700** 个 half 向量边界案例通过。
- 生产常量窗口 **832** 个 NIR 案例通过，覆盖全部八个 buffer 绑定；其中 **128** 个启用路径没有 SSBO 读取。还检查 unsupported 宽向量拒绝。
- 实际编译了一至四窗口的 fragment base / MSAA ISA，并检查 volatile、coherent、SSBO 写入的排除。完整 MSL / AIR / IR3 编译器回归通过。
- **8,224** 个生产规划器范围 / 对齐案例，以及 wire v35 拒绝、metadata 拒绝、kernel / wire 布局、kernel / capture hash 一致性、flush / wait / HLSQ / load / draw 顺序和有效标志清零 / 重绑定检查通过 ASan / UBSan。
- 原有 GPU submit、物理别名 dispatch、页表 / 上下文、硬件 fence 模型、FIFO / epoch 和 job-activity 检查通过。以上 host fabric 不执行实际 GPU shader。
- 原生 arm64 compiler / adapter / gpuctl / metalctl、GPU1.138 Kext 构建通过；完整内核校验 **74 kext**，其余 local bundles 保持父候选。Metal adapter 集成检查更新了旧的版本断言，验证 v36 / shader command v38 与构建声明。
- 驱动覆盖镜像 **13 个文件**独立读回校验。更新 adapter、compiler、两个工具、版本 / receipt / default library，并新增 `ConstantWindows.air`；沿用 displayctl。新的 cache context 防止复用旧 ISA。

校验过程中发现并纠正的是：模型使用了原型的 128-word 地址，实际公共元数据末尾为 112 words。生产接口始终从公共宏推导；最终模型、packet golden 和真实 shader 重放均对应同一布局。原失败日志保留，未将其当作硬件故障。

## 待运行的原生诊断

`metalctl --system constant-window-test` 已构建，**尚未运行**。它在同一个 command buffer 内由 GPU 生成 half 数据，再绘制读取窗口的 fragment，核对每阶段 4,096 个真实像素：

1. 完整 224 字节绑定。
2. 216 字节，矩阵完整但 CP 窗口不足，走原读取。
3. 212 字节，末行部分 half 分量越界返回零。
4. source offset 4，关闭不满足对齐的间接加载，并检查前后 guard。
5. 复用原完整 buffer、由 GPU 写新 phase，检查旧常量没有残留。

后续应结合已有逐操作 flush 计数和当前编译产物，确认有窗口时新增发布 / 加载路径确实命中；像素正确本身不能证明选中了快路径。然后验证 framebuffer 像素 / fence 回归，再采集复杂 Welcome 的 GPU 时间与**实际亮屏**实显节奏，不把疑似熄屏长间隔算作动画卡顿。此诊断不是简化动画的性能验收。

## 交付

已生成匹配的 [内核](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/kernelcache.ui-constant-bank)、[驱动覆盖镜像](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/drivers-ui-constant-bank.hfs) 和 [trust-cache module](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-constant-bank.trustcache)。未生成完整 EFI；其他任务的根文件系统 / 壁纸 / displayctl 变更保持原样。

证据：[实现审计](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-constant-bank/audit.json)、[编译器检查](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-constant-bank/host-check-final.json)、[产物校验](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-constant-bank/artifact-verification.json)、[原 AOT 保持检查](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-constant-bank/AOT-preservation.json)、[源码补丁](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-constant-bank/changes.patch)。
