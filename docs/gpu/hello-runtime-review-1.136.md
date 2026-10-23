# GPU 1.136 真机记录：页表开销与真实 UI 热点

2026-10-19。GPU 1.136 / Metal 0.96 / Display 1.0.18，round 154。

本文为 round 154 及当时导出排查的历史记录。后续 round 156 已确认文件位于 Foundation 实际目录 `/private/var/mobile/tmp/com.apple.backboardd`，此前采集脚本漏查，不能归因于权限或诊断开关。已导出原生热点并完成 41,840 字节逐字节编译重放。GPU1.137 / Metal1.0 的实现和157/158结果见 [后续分析](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-runtime-review-1.137.md)。

## 已验证的结果

私有作业页表改用 CPU WB 映射，并在发布根表前通过原始、已 prepare 的 descriptor 按子表到父表顺序清理到 PoC。真机 render-state、四个 framebuffer snapshot 阶段的像素与边界检查、mixed batch 均返回 0；四 CPU 的 timer/IPI 证据齐全。真实 UI 窗口完成 619 个作业，失败为 0，无 panic。

| 指标 | round 154 实测 |
| --- | ---: |
| 配对统计窗口 | 12.103 s |
| UI 作业提交 / 完成 / 失败 | 619 / 619 / 0 |
| GPU 核心计数器测得平均频率 | 595.987 MHz |
| CPU 地址空间阶段平均 | 2.214 ms / 作业 |
| CPU wrapper submit 平均 | 6.611 ms / 作业 |
| GPU 作业 p50 / p95 / 最大 | 1.962 / 49.342 / 62.605 ms |
| 实显间隔 p50 / p95 / 最大 | 33.518 / 83.267 / 216.270 ms |
| 实显首尾 265 个间隔 / 12.018 s | 22.050 次 / 秒 |
| 大于 100 ms 的实显间隔 | 4 |

实显统计保留动画停留，不能称为持续动画阶段的瞬时 FPS，也不能声称达到 60 FPS。CPU 阶段彼此嵌套、可以与 GPU 重叠，不能相加为帧时间。没有测得 CPU 的实际运行频率；四 CPU 在线和固定计时器频率不等于 CPU 核心频率。

旧 round 150 的地址空间平均为 2.784 ms / 作业，wrapper submit 为 7.118 ms / 作业。这是不同动画阶段的观测差异，不构成严格 A/B 性能增益。后续累计页表统计的 sampled table-zero 平均为 1.356 µs，publish 为 33.705 µs；这些数值与窗口平均不能混用。

## 仍占主要开销的路径

`fixed_frag_lph_cpf`，操作身份 `bc8b834de5755555`，266 次采样共 3176.652 ms，占窗口 GPU 作业时长总和约 66%。它使用 framebuffer fetch，拷贝计数为 0；现有免拷贝路径确实命中。普通同名变体 `48fd06ac66534652` 为 724.969 ms。这里的比例是 GPU 作业时长之和的占比，不是墙钟利用率。

显示 lease 的完整配对只保留了窗口尾部 34 组：Active 到实际启动中位数 3.711 ms；启动到观测 PP_DONE 中位数 21.748 ms，包含 TE、传输和 IRQ 服务延迟。不能把它全部解释为 DMA 耗时，也不能据此证明固定 30 FPS 上限。

## Apple 原实现与导出核对

从原始 iPadOS 18.7.10 系统镜像读取的 `com.apple.backboardd.plist` 明确以 `mobile` 启动；原始签名携带 `seatbelt-profiles=backboardd` 与 system-container entitlement。固定使用 root 的全局临时目录不足以代表该进程的文件访问能力，但尚未证实这是导出缺失的原因。

对运行中的 backboardd PID 65 获取了完整 11728 字节原生 stackshot。其 dyld 映像 UUID `4c4c443d55553144a1229bcb9ceae8fa` 与本轮 HFS 中的 Metal 0.96 二进制一致，与 0.94、0.95 不同。因而已经排除本轮 backboardd 加载旧适配器的解释。

root 的 mixed-test 曾成功写出一个当前上下文的 MSL fragment 文件；原生 UI 热点文件仍缺失。Metal 0.97 改用 `NSTemporaryDirectory()` 并通过已有、默认关闭的诊断通道报告真实写入错误。round 155 核对了实际加载 UUID、0.97 版本及 UI 作业增长，但热点文件检查仍返回 1，且未见写入错误报告，不能将目录调整描述为已解决问题。

后续 GPU 1.137 / Metal 0.98 补充同一 native device connection 上的只读诊断配置查询，以及 binary archive 命中时的源重建导出。缓存命中保留实际已验证 ISA，仅从原始 AIR 与已解析 constants 重建 GLSL / reflection；不重编译或替换 GPU 程序。是否解决导出缺失须由下一轮真机结果确认。

## 采集完整性

round 153 的冷 JIT 检查超时，保留原始失败记录，未执行后续正确性与性能检查。round 154 的整批串口导出超过 90 秒，原始 report 同样保留超时。之后只重传同一冻结 `jobs154.txt`，943 条记录的 sequence 为 6，先前完整收到的 767 条逐条一致。两个 snapshot、全部显示记录、完整 job history 的 sequence、计数与时间约束仍经过原分析器验证。

性能分析不把失败的热点导出检查当作成功。证据位于 `build/gpu-runtime/round-154-analysis/provenance.json`、`analysis.json`、`operation-analysis.json`；复算入口为 `build/gpu-runtime/ui-cacheable-job-tables/analyze154.py`。硬件结论汇总在同目录 `hardware-evidence.json`。
