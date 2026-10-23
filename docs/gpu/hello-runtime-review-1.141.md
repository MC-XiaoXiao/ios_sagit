# GPU1.141 / Metal1.4 真机验证与剩余开销

2026-10-19，用户恢复真机调试授权后完成。GPU1.141 / Metal1.4 / Display1.0.18 已实际启动；60 FPS 尚未达到或验收。

## 结果

原生 Metal 的普通与 framebuffer 常量窗口共十阶段全部通过，每阶段检查 4,096 像素，覆盖短绑定、部分行、不对齐 source offset、重绑定和同 command buffer GPU producer。render-state、跨队列 fence、Apple CARenderer 八帧检查也返回 0，后者每帧 4,096 像素正确。四核启动验证通过，正常运行采集没有 panic。

预先指定的 12.087614 秒系统合成窗口内，提交 816、完成 817、失败 0、shader operations 5,769。窗口开始已有未完成作业，因此完成数可以比提交数多一笔。硬件 core / always-on 计数器对这 817 笔作业测得 GPU **595.970 MHz**，不是只读取请求频率。

| 项目 | 同窗口结果 |
| --- | ---: |
| GPU 作业 | 均值 3.470 ms，中位 1.561 ms，p95 17.231 ms，最大 19.393 ms |
| CPU prepare → admission | 均值 4.640 ms，p95 7.126 ms |
| CPU address-space 阶段 | 2.126 ms／次，816 次 |
| CPU encoding | 1.234 ms／次，816 次 |
| wrapper resource create / delete | 1.476 / 1.578 ms／次，532 / 533 次 |
| admission → doorbell | 均值 3.056 ms，中位 0.455 ms，p95 12.229 ms，包含等待与调度 |
| CPU retirement | 均值 0.742 ms |
| 显示 Active → Started | 41 组配对，中位 3.654 ms |
| 显示 Started → 观察到 PP_DONE | 同组中位 26.128 ms，含 TE、传输和 CPU 观察 |
| 实际完成显示记录 | 310 条、309 间隔，首末跨度 12.051526 秒 |
| 显示更新间隔 | 中位 33.423 ms，p95 50.078 ms，最大 166.889 ms |

首末实际显示记录的更新速率是 **25.640 次／秒**，包含内容停留；它不是已经确认连续运动的 Welcome 光学 FPS。没有删除长间隔，也没有以中位数倒数宣称 30 或 60 FPS。CPU 各项有嵌套和重叠，不能相加作为一帧成本；GPU、CPU epoch 不直接对齐。

## 新 shader 确实在系统 UI 中使用

真实 compositor 的 `fixed_frag_lph_cpf` / `80bdefd21c667f9f` 有 310 次操作，共 565.359 ms，平均 **1.824 ms／操作**。运行记录为 672 words、6 full / 20 half registers、framebuffer mask 1，与合并候选一致；620 次 raster、1,860 次 flush、copy bytes **0**，仍走原地零拷贝反馈。

这证明新程序用于真实合成器及其逐操作时间；不能单凭像素通过或 flush 数证明每次 draw 的间接常量窗口均命中。窗口绑定不足时仍允许正确的边界回退。旧 GPU137 数据的设备身份与当前场景不同，没有把旧 15 ms 与新 1.824 ms 当成隔离变量的 A/B 提速倍数。

当前最大逐操作累计项变为无 framebuffer fetch 的 `fixed_frag_lph_cpf` / `8e20a5b0ebb569fd`：310 次、894.946 ms，平均 2.887 ms。逐操作边界共覆盖 2,435.887 ms，约为所选作业 GPU 区间总和 2,834.972 ms 的 85.9%；其余区间未强行归给某个 shader，也不把该比例解释为墙钟 GPU 利用率。60 笔作业 GPU 区间仍超过 16.667 ms。

## 屏幕状态与测量有效范围

60 秒原生 on 请求期间保留了 578 条状态样本。整个 holder 返回 **1**：后段确实出现 display / power off 与切换状态，原生请求不能保证压过系统熄屏策略；没有将此命令记为全程保亮通过。

预先指定的 GPU 窗口被 118 条样本包围，实际和 target 的 display / power 四状态均为 1，窗口最大采样间隙 106.195 ms。采样 absolute 与 continuous 时钟差为 1–18 ticks；显示记录使用 continuous，GPU / lease 使用 absolute，分析按观测偏差对应边界。窗口外 off 状态和原始长显示间隔全部保留。逻辑采样仍不是连续光学证明。

## 启动兼容修正与保留失败

首次 round159 在 GPU 检查前失败：设备最近安装的 PosterBoard 补丁修改了共享缓存，原 GPU 启动候选遗漏其配套 trust cache，launchd 的 libSystem 加载被拒绝。`os.dmg` 的 ELOOP 在早先成功启动中也存在，不能单独归因于它。新启动包只追加当前设备已验证的 `poster-store-policy/trust-cache.bin`，kernel、GPU / Metal HFS、DT 和 boot args 逐字节保持；没有文件系统写入。

round160 成功启动，但首个组合采集函数触发交互式 Bash 历史展开错误，未执行测试链；同一启动内停止 runner 后重接。后续采集准备还发现设备没有外部 mkdir，已改为现存目录下的文件前缀。失败输出保留，不当成性能或像素验收。原离线计划恢复为构建时的原字节，实际执行计划另存。

`gpuctl info` 返回旧接口错误 `0xe00002e2`，其与后接的 Metal info 的组合返回 1；这项不记为成功。后续使用原生 `metalctl --system info` 确认实际 Metal1.4 bundle UUID，四项原生正确性检查分别返回 0。runner 的进程退出 0 仅表示完成采集，诊断状态单独检查。

## 下一阶段依据

CPU 准备、资源生命周期和显示 staging / 完成衔接现在应与剩余重 GPU 作业一起处理；不能继续假设旧 framebuffer shader 独占主要成本。Active → Started 尚含约 3.65 ms，需分出 DMA prepare / mapping、整幅 cache maintenance、寄存器 staging，而非直接删除 flush。Started → PP_DONE 约 26 ms 也不能当成实际 DSI 传输时长或固定 30 Hz 上限。

再次核对项目内的 Apple 原版 CA / IOMFB ABI 证据：Scheduled、当前像素完成和旧显示读访问退休是不同边界。当前 `QcomDisplaySurface` 同时持有 IOSurface use count、prepared descriptor 和 DMA mapping，直接缓存整个对象可能长期阻止 CA 缓冲复用；需先明确生命周期再设计复用。保持真实 producer fence、PP_DONE、latch、idle 后退休规则。未改显示 PLL 或提前 signal。

CPU 固件查询仅给 cluster1 configured 300 MHz、`measured=0`，不能当成当前四核实际频率；cluster0 查询不支持。没有据此宣称 CPU 已测为 300 MHz，也没有盲目改写 CPU PLL / 电压。

证据：[启动包兼容校验](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-141-runtime-compatible/verification.json)、[四项原生检查](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-160-verify/report.json)、[原始窗口及解析](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-160-window/analysis.json)、[解析脚本](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-141-runtime-compatible/analyze-window.py)、[Apple 生命周期证据](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-long-frame-review-1.132.md)、[当前 EFI](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-141-runtime-compatible/bridge/XnuBridge.efi)。
