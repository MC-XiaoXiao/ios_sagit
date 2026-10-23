# GPU1.141：复用已完成的 framebuffer 发布

2026-10-19。继续离线优化；未访问设备或测量新帧率，60 FPS 尚未验收。配套 Metal1.4 / Display1.0.18，JIT ABI36。

## 同步覆盖证明

GPU1.138 加入常量窗口时，在每个有效窗口的完整 raster setup 中新增一次 `CACHE_FLUSH_TS` 和匹配时间戳等待，再执行 `CP_WAIT_MEM_WRITES`、HLSQ allocation update、普通 / 间接常量加载。普通绘制仍需要这次发布。

真实 Welcome 的 framebuffer 展开路径已有相同强度的发布：

1. 进入展开前执行 color flush / wait、UCHE flush / wait。
2. in-place 路径在首次加载前只配置状态；后续图元保留逐图元 color / UCHE flush / wait。
3. 快照回退在 GPU copy / compute 后重新执行 UCHE flush / wait，再进入 raster。
4. raster 的状态配置和 shader LOAD_STATE 不启动 shader，也不写 uniform 数据。

因此这条特定调用在常量读取前已经发布了全部先前 shader 写入。本轮仅为该私有调用传入本次调用的 `shaderWritesPublished` 证明，略过重复的全局 flush / wait。没有跨 draw、operation 或回滚缓存一个“干净”标志。普通绘制与显式 primitive-snapshot 直接 raster 仍执行完整发布；所有常量加载仍保留 `CP_WAIT_MEM_WRITES` 和 idle。

IB 切换前后保持原命令顺序及逐图元发布；未发布的诊断 / compact trial 回滚后重新按原入口生成证明。每图元反馈、color / UCHE 完成等待、最终 fence、ASID / 页表和显示规则保持。

依据：[现有展开与发布](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/AdrenoPipeline.h:775)、[实际 physical backing 别名拒绝](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/QcomGPUNative.cpp:1824)、[Mesa A5xx 间接常量编码](/home/xxiao/Projects/MI6_iOS/build/gpu-compiler/mesa/src/gallium/drivers/freedreno/a5xx/fd5_emit.c:59)。Apple AIR 的只读 Uniform 契约继续保留；没有把 Apple GPU 的一致性假设用于 A540。

## 验证和范围

- 8,224 个原窗口范围 / 对齐案例及 metadata、重绑定、packet 顺序检查通过 ASan / UBSan。
- 新增 in-place / snapshot、状态复用开关、显式 primitive-snapshot 八种组合。逐包跟踪实际 flush 事件、对应时间戳等待、GPU 写操作和间接常量加载：每次间接加载之前都必须有覆盖这些写入的完成发布。
- 混合 framebuffer → compute → 普通常量绘制保留新发布。384 个图元跨多个 IB，包含 profiling / compact trial 回滚，检查 385 次 UCHE 发布、每次窗口重装载的前置发布、计数恢复及分配失败。完整 GPU submit / MMIO / 私有页表 / fence / FIFO 模型通过。
- 常见双三角形 in-place、同 IB、有效窗口的命令序列少一次全局发布，总 flush 由 7 降到 6；这是编码序列变化，没有测得相应 GPU 时间或 FPS 收益。
- 完整编译器回归仍通过，含 2,700 / 832 / 1,768 组既有案例。135 个 AOT、registry `72b191f4da4d7b92`、编译器、Metal adapter 和 archive context 逐字节保持。真实热点继续为 `80bdefd21c667f9f`、672 / 704 words、6 full / 20 half registers。

## 配套原生正确性诊断

`metalctl --system constant-window-test` 现在有普通和 framebuffer 两种 consumer，各五个阶段。每阶段在同一 command buffer 内由 GPU 生成 half 数据，再进行绘制；检查 4,096 个像素、短绑定、部分 half 行、source offset 4、重绑定和 guard。

framebuffer 模式清为白色，再输出 half 矩阵和加四分之一 framebuffer 输入，期望值全部为精确的二进制分数，可直接计算 UNORM 像素。它使用工具内嵌的 authored AIR，经现有 `newLibraryWithData` 入口加载，不增加 AOT 项。主机检查读取工具里的同一段 AIR，验证真正的 base / MSAA ISA、framebufferMask1、buffer1 `[160,224)` 窗口和 wire 有效性；避免诊断意外绕开待测路径。

初始 MSL 诊断被当前入口类型限制拒绝，保留了失败日志；最终使用现有 AIR 接口，编译器实现没有改变。新原生诊断**仅完成构建和主机检查，未运行真机**。像素通过后还须结合逐操作计数确认路径命中，再用复杂 Welcome 验证实际时间和亮屏实显节奏；本诊断不提供动画帧率结论。

## 产物

GPU1.141 [匹配内核](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/kernelcache.ui-constant-publish-reuse)、仅更新 metalctl 的 Metal1.4 [覆盖镜像](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/drivers-ui-constant-publish-reuse.hfs)和 [trust cache](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-constant-publish-reuse.trustcache)已完成离线审计：74 个预期 bundle、23 个本地可执行文件及 HFS 的 13 个文件独立校验通过。没有完整 EFI 或根文件系统修改。原 GPU1.140 / Metal1.4 冻结组件及其他任务改动保留。

[审计](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-constant-publish-reuse/audit.json)、[提交模型](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-constant-publish-reuse/driver-checks.log)、[编译器检查](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-constant-publish-reuse/host-check-final.json)、[源码补丁](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-constant-publish-reuse/changes.patch)。
