# 首次亮屏、显示提前准备与时钟核对

2026-10-19，round165：GPU1.143 / Metal1.5 / Display1.0.19。一次合并启动先采真实系统合成，再检查正确性；没有切换 shader、GPU 频率或队列深度做小步对照。**尚未解决首次卡顿或语义帧回退，也未达到约 60 FPS 的目标。**

## 本轮实现与验证

对照固定镜像 AppleDisplayPipe `prepare_swap`、原生请求准备及 IOSurface 访问 fence 的分析，将下一笔已 ready 的普通全屏请求的映射／cache maintenance 提前到上一笔 DPU 传输期间。只保留一个尚未暴露给硬件的映射；请求、surface、原始 read fence 和 token 必须全部一致，且 fence ready、未完成，才在实际出队后接用。保留 fence 引用以避免请求地址／token 复用；旧 front 与新准备即使来自同一 IOSurface，也拥有独立 DMA preparation。取消、换队首和清理失败回退或隔离，实际 MMIO stage、PP_DONE、latch、idle 及读 fence 退休顺序保持。

没有把提前准备放进原生 `prepare_swap`，避免在尚未证明可重入的 surface 访问锁内映射；仅在已有活动传输的 gate 和 VBL 回调中尝试。Apple 原分析支持准备与实际呈现分离的生命周期，不能据此断言两种 GPU／DPU 的硬件细节相同。

ASan/UBSan 显示检查覆盖身份变化、未 ready／已完成 fence、取消、引用 ABA、token 回绕及失败清理，arm64 驱动和 displayctl 构建通过。75 bundle／24 本地 bundle 集合仅替换 Display；15 个 HFS 文件读回和 EFI 实际嵌入核对通过。保留 GPU／Metal／135 AOT、设备身份、壁纸补丁 trust 及现有 GPU 策略。

真机 runner 返回 **0**，四核 bring-up 验证通过，四条控制台命令均返回 0，无采集内 panic／超时。跨队列 fence、未来 event 恢复与真实 GPU 读回通过；Apple CARenderer 八帧各 **4096 像素正确、0 unchanged**。这些是正确性检查，不是 Welcome 运动／回退的光学验收。

Apple 原分析：[prepare_swap](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-framework-lifecycle/appledisplaypipe-prepare-swap.txt)、[原生 surface 准备](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-framework-lifecycle/native-prepare-surface.txt)。实现：[DisplayPrepareSlot.h](/home/xxiao/Projects/MI6_iOS/drivers/display/QcomDisplay/DisplayPrepareSlot.h)、[QcomDisplay.cpp](/home/xxiao/Projects/MI6_iOS/drivers/display/QcomDisplay/QcomDisplay.cpp)。构建及真机证据：[build-checks.json](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-display-prepare-ahead/build-checks.json)、[hardware-evidence.json](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-display-prepare-ahead/hardware-evidence.json)。

## 已经量到的显示开销

完整启动累计提前准备 **190 次、命中 190 次、丢弃 0**。两次配对快照之间为 507 次显示、171 次提前命中；提前准备均值 **3.718 ms**，平均提前 **4.462 ms**；其余 late 准备 336 次、均值 **3.773 ms**。这些值说明实际工作被提前，不表示消除了全部准备工作，也不能折算成 FPS 收益。

后段独立 lease 环中 42 组 Active→Started→CompleteBefore，Active→CTL_START 中位 **0.018 ms**，p95 **3.818 ms**；Started→观察到完成中位 **29.727 ms**。该环晚于 GPU 窗口，且没有逐请求标记提前命中，不能称为同一 GPU 窗口的完整耗时分解或逐请求性能对照。

配对快照间硬件 VBL 回调约 **59.995/s**，显示更新间隔中位仍约 **33.4 ms**。这证明存在约 60/s 的观察回调，不能据此认定运动达 60 FPS。接下来应同时量 CTL_START 相对 TE 的相位、原始 PP_DONE 与工作循环服务时刻，区分等待下一 TE、实际传输和中断延迟。当前没有这三者的独立时间戳，不能把约 30 ms 全算作 DSI DMA，也不能未经验证改 tearcheck／链路时钟或提前结束 read fence。

高通原显示代码明确按 VSYNC／vtotal 配 tearcheck，并区分 start、continue 阈值及 rdptr；本机固件为内部计数使能、外部 TE 未使能（SYNC_CONFIG_VSYNC=0x800a6）、height=1927、rdptr=1921，原先自动刷新配置已由既有稳定实现关闭。应按这些实际参数分析，不把模式声明 60 Hz 当作链路实测。参考：[原始 tearcheck 配置](/home/xxiao/Projects/MI6_iOS/docs/display/reference/dpu_encoder_phys_cmd.c:332)。

## 冷启动长帧与熄屏处理

12.777 秒 GPU 窗口完成 **746 笔、0 失败**，GPU 实测 **595.971 MHz**；作业均值 **3.609 ms**、p95 **18.357 ms**、最大 **77.762 ms**。CPU prepare→admission 均值 4.746 ms，admission→doorbell 3.715 ms，退休 0.777 ms；阶段存在嵌套与流水重叠，不能相加为一帧。

首次亮屏 token66→67 的 **1167.024 ms** 间隔中，下一笔 prepare 前约 **1090.237 ms** 没有新作业进入，已有准备作业为 0，同期最长 GPU 作业 **13.015 ms**。token131→132 的 433.588 ms 中，prepare 前空档约 350.426 ms。秒级长帧主要空档仍位于当前记录的 GPU prepare 入口之前；这里还包含 CA 编码、编译、调度、gate 等待和动画停留，尚不能指定某一项为根因。本轮 CPU 只读诊断也产生了多次进程启动，尤其两边窗口之外存在扰动，不能当干净的冷暖性能对照。

GPU 窗口末端附近出现逻辑 off，原有全 on guard **失败并保留**。完整显示历史又跨过一次熄屏／唤醒；6484.524 ms 最大间隔已明确归为电源转换，原始数据不删除。首次熄屏前 342 个原始间隔中位 33.480 ms；首次完全唤醒后的 226 个中位 33.441 ms。两段语言／内容阶段不同，含自然停留及最大约 1.53 秒采样空档，不能据此证实或否定用户看到的冷暖差异。

重要覆盖限制：GPU job-history 的时间范围早于后段唤醒显示历史；后段缺少作业关联的值记为 **null／未覆盖**，不能把它写成 GPU 耗时 0。显示 token 无倒退也不能排除像素内容回退；仍需同一内容代次与 surface 生产／扫描的关联。

[GPU 窗口与完整修复记录](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-165/analysis.json)、[电源分段／lease／提前准备](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-165/phase-analysis.json)、[复算脚本](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-display-prepare-ahead/analyze-phases.py)。仅移除明确完整的已观察日志插入，保留原 UART 哈希；未丢弃损坏记录或猜测缺字节。

## CPU 时钟：排除错误的低频判断

UEFI 公共 Clock 协议 CalcClockFreqHz 在 cluster0 返回 **388796000 Hz**，启动 MPIDR=0x80000000。固定高通原 `cpu_dbg_mux_get_rate` 明确 pwrcl 到 debug mux **除以 4**、perfcl 除以 8；分析的 ClockDxe HAL 对 GCC debug 分频做补偿，但未再补偿这段固定 CPU 分频。因此 cluster0 的 UEFI 核心频率估计约 **1.555184 GHz**，接近相应 BSP 的 1555.2 MHz 档，不能称其只有 389 MHz。该结果是 **UEFI 阶段估计，不是系统动画运行时 CPU 频率实测**；cluster1 返回 0 也不证明运行核心停在 0 Hz。

固件源及官方 MIUI 提取的 ClockDxe 已按 SHA 分别核验；工程版本在 CPU 初始化段有五个 NOP，不能据此假设当前运行模块就是该版本或 CPU 实际低频。协议前缀布局已核对，未调用私有地址或 CPU 设置接口。原始 vendor Android DTB 的 OSM compatible 为 **v2**，本轮 DT 已纠正；高通 v2 路径需要完整 LUT／PLL／CPRh／ACD 初始化，当前只读观察驱动拒绝未就绪 bank 的行为保留，不能靠设置 enable 位补齐。

修正执行权限后，36 次 powerctl 查询实际执行但均未打开控制器；一次无重启的 `powerctl list` 得到五个 **0xe00002e2 / kIOReturnNotPermitted**，具体拒绝策略未单独 trace。registry 仍为 QcomOSM=1、SecureOSMInitialized=false、CPUAutomaticPerformanceAvailable=false。没有发生 CPU 频率／电压／自动策略写请求；当前运行时 CPU 频率仍未知。不再反复启动这批已知失败的 CPU 查询进程干扰后续采集。

分频原实现：[固定提交 clock-osm.c](https://android.googlesource.com/kernel/msm/+/e60e904d4e7457386748f5e80fe8b56da7a79bd1/drivers/clk/msm/clock-osm.c)、[本地对应函数](/home/xxiao/Projects/MI6_iOS/docs/power/reference/clock-osm.c:814)、[ClockDxe 证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-cpu-firmware-path/evidence.json)。

下一步集中补齐上游 prepare 前耗时和显示 TE／真实完成／工作循环服务三段时间线，结合原始框架定位冷路径和偶发内容回退。当前提前准备可以保留为已通过正确性的实现；其独立帧率收益尚未证明。
