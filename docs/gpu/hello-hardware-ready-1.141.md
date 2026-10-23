# GPU1.141 合并启动候选与有效采集窗口

2026-10-19。按“先离线”要求继续准备；本轮未访问 UART、USB、Fastboot 或设备，没有新的实际帧率、像素或光学验收。60 FPS 目标仍未证明。上一轮已完成 GPU1.141 发布复用，本轮完成合并 EFI 和采集前置条件，没有继续叠加 shader 或显示驱动行为修改。

## 帧预算核对

- 当前交接配置及 QcomDisplay 的 native timing attributes 声明 **60 Hz**，周期约 16.7 ms。旧原生 `layer-animation` 使用 CABasicAnimation 和 run loop，没有自行设置 30 FPS 上限。
- round158 的 Started→观察到 PP_DONE 中位约 **23.419 ms**包含 TE 等待、传输及 IRQ workloop 服务；不能视为纯 DMA 时间或 30 Hz 的硬上限。
- 旧日志只记录 DSI clock-control 位值，没有实际 DSI byte / bit-clock 频率，因此不能据此计算物理链路带宽或改写 PLL。
- 现有启动 serialmode **0x13** 含 `SERIALMODE_NO_IOLOG`；没有把普通 IOLog 直接当成 UART 阻塞的主要原因。驱动和串口策略保持。

这些检查没有证明显示通道能在每种负载下达到 60 FPS，只排除了“当前配置明确声明为 30 Hz”的解释。后续按同一时间窗口关联 GPU 作业、逐操作热点、实际完成帧和屏幕状态，不把不配对的平均耗时相加当成整帧时间。

依据：[native timing attributes](/home/xxiao/Projects/MI6_iOS/drivers/display/QcomDisplay/QcomDisplay.cpp:1100)、[原生动画诊断](/home/xxiao/Projects/MI6_iOS/tools/displayctl/displayctl.mm:671)、[旧实测限制](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-runtime-review-1.137.md)、[Apple XNU IOLog 路径](/home/xxiao/Projects/MI6_iOS/kernel/iokit/Kernel/IOLib.cpp:1477)、[serialmode 位定义](/home/xxiao/Projects/MI6_iOS/kernel/osfmk/console/serial_protos.h:52)。

## 原生 display-on 与状态记录

新增 `displayctl hold-display-on SECONDS`，参数限 1–60 秒。复用已有、经过 loaded-image UUID / selector ABI 验证的 CADisplayStateControl 入口，请求 on 并持有控制对象，按 10 Hz 记录实际 / target display、power 四个原生状态，同时记录 absolute 与 continuous Mach 时钟。

采集期间不逐样本打印；结束后批量输出所有状态和实际跨度。整体有超时，提前结束、跨度异常或采样中出现非 on 状态返回失败。设备身份诊断和原有命令保留，内核显示提交 / 退休规则不变。

**这是采样的逻辑状态，不是连续光学亮屏证明。**分析必须检查时间覆盖、采样间隙和 frame flags；原始 off / unknown 样本、长间隔和正常动画停留全部保留，不通过删除长间隔或取中位数倒数宣称运动 FPS。新命令仅通过 arm64 构建和导出校验，尚未真机执行。

## 合并候选

[XnuBridge.efi](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-141-hardware-ready/bridge/XnuBridge.efi) 包含 GPU1.141 / Metal1.4 / Display1.0.18，JIT ABI36、registry `72b191f4da4d7b92`。GPU 内核及先前审核的 Metal 组件保持，覆盖镜像仅再更新 displayctl；设备身份 DT 源、原启动参数及 APFS UUID 与已验证的身份任务保持。没有修改根文件系统。

13 个 HFS 文件读回一致。独立解析 ELF64 symbols / load segments，对照最终 kernel、driver-image 和 trust-cache 文件，确认实际 blob 在 raw Image 和 EFI 中均存在；两个 loader 格式有各自的入口代码，不要求整个 raw Image 嵌入 EFI。前一未启动的组件候选保留。

## 一次合并验证计划

已保存[采集计划](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-141-hardware-ready/capture-plan.json)，**未执行设备 runner**。先完成普通 / framebuffer 共十阶段的 GPU 常量像素诊断、render-state、fence 和原生 CARenderer 八帧检查；任一步失败不进入性能窗口。

随后一次原生 wake 请求、限时 on / 采样进程和 compositor 作业增长 gate，再取一个 12 秒窗口。开始 / 结束的实际帧计数、GPU live / operation / job 记录、lease 与全部状态样本在窗口后导出，避免 UART 批量输出进入测量期间。不重复旧开关对照；gate 仅证明新增 compositor 作业，Welcome 内容及持续运动仍需另行确认，不能把 idle 判作 GPU hang。

设备可用性仍在等待用户回复；在此之前保持离线。证据：[离线审计](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-141-hardware-ready/audit.json)、[构建输入](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-141-hardware-ready/source.json)、[启动参数](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-141-hardware-ready/bridge-invocation.json)、[采集函数](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-141-hardware-ready/capture-functions.sh)。
