# GPU 1.137 / Metal 1.0：原生 shader 优化与真机结果

2026-10-19，Display 1.0.18，fetch2 / pipe6。round 157 为启动和正确性检查，round 158 为同一次启动、用户按键后的性能窗口。

## 本次实现与 Apple 原始输入

round 156 已找到 Foundation 为 backboardd 选择的实际目录 `/private/var/mobile/tmp/com.apple.backboardd`。此前的文件检查遗漏了这个目录，不能据此认定诊断未启用、权限失败或缓存命中造成漏导出。backboardd / SpringBoard 的 registry 与原有设备连接查询均为 enabled=1。实际冷编译记录为 `metallib-air`；archive-hit 导出入口虽已实现，尚未单独验证。

已导出热点 `bc8b834de5755555` 的原始 AIR、函数常量、GLSL、reflection 和 base / MSAA ISA。AIR SHA256 为 `bc740d8121dbe021a7699afe9af691b2c6ad64cf17c124f743af54ff7a6cf261`；完整 41,840 字节原生产物与主机重放逐字节一致。实际热点是单采样变体。

Apple AIR 的 framebuffer 输入是当前 fragment 的值参数。原翻译将同一参数的多次使用展开为多次纹理读取。Metal 1.0 在入口只物化一次对应值，保留原始 swizzle、FP16 转换及后续算术；AIR runtime、host AIR 和 MSL 路径均采用此语义。没有以硬件 blend 替代 Apple 运算，也未放松 NaN、half 精度或 fence 规则。

热点变为 `a5f84e06617f6984`，704→640 code words，framebuffer 读取从三组变一组；full registers 12→16、half registers 21→19，仍有 22 条 scalar buffer load。代码减少不能直接推断运行加速。普通不含 fetch 的 `48fd06ac66534652` 完整程序保持一致。133 个 AOT 程序、registry、backend、GPU 1.137 内核和显示实现逐字节保持原样；HFS 只更新 Metal 适配器与版本 plist。

## 验证

- 主机生产编译器与 guard 检查通过，新增跨分支 / PHI、多 framebuffer 输入及 MSAA 用例。首次 MSL 测试夹具语法错误已修正，原失败记录保留。
- round 157 四个 framebuffer 阶段逐个核对 292,096 字节，含像素、padding、外层 guard，全部通过。
- 原生 Apple CARenderer 的八帧 64×64 IOSurface 检查，每帧 4,096 个像素正确，覆盖透明度、移动、隐藏、局部损伤和透明 CGImage。
- round 157 和 158 各四个命令均返回 0，未捕获 panic 或命令超时。启动验证四 CPU；这不代表测得 CPU 核心频率。
- round 157 的性能窗口结束时 UI 已停止更新，只有两条显示记录，不能用于持续动画 FPS。round 158 预先在 Bash 定义采样函数，实际 UI 完成数增长后仅调用短函数，冻结 12 秒数据，再批量导出。

## round 158 同窗口实测

| 指标 | 结果 |
| --- | ---: |
| 配对统计窗口 | 12.097 s |
| UI 提交 / 完成 / 失败 | 586 / 586 / 0 |
| GPU 核心计数测得平均频率 | 595.988 MHz |
| GPU 作业均值 / p50 / p95 / 最大 | 8.674 / 1.483 / 51.301 / 62.813 ms |
| CPU address-space 平均 | 2.092 ms / 作业 |
| CPU wrapper-submit 平均 | 6.455 ms / 作业 |
| CPU queue-wait 平均 | 20.499 ms / 作业 |
| 实显间隔 p50 / p95 | 33.446 / 83.422 ms |
| 已保留的显示记录 / 间隔 | 230 / 229 |
| 首尾显示跨度 | 12.035 s |

同窗口 13 个程序桶累计覆盖 4,861.337 ms，约为 GPU 作业累计时长 5,083.257 ms 的 95.6%。这不是墙钟 GPU 利用率。主热点 `a5f84e06617f6984` 的 231 次操作累计 3,457.774 ms，约占 GPU 作业累计时长的 **68.0%**；普通采样变体为 775.785 ms。热点实际命中两次 raster、六次 flush 的原位路径，**copy_bytes=0**。系统 UI 确实在 GPU 上渲染，免拷贝也已启用。

显示尾部保留的 34 组完整 lease：Active→Started 中位 3.966 ms；Started→观察 PP_DONE 中位 23.419 ms，含 TE、传输和 IRQ workloop 服务，不能当作纯 DMA 时间。33 组已记录 ready→Active 中位约 0.008 ms；pending→Active 则可能包含 GPU 依赖等待，不能直接归为显示额外延迟。

本轮没有证明 framebuffer 单次物化提高真实帧率。不同动画阶段及寄存器变化使 round 154 与 158 不构成严格 A/B；不以指令数或单个均值宣称收益。常见实显节奏仍约 33 ms，**尚未达到 60 FPS**。CPU 阶段存在嵌套和流水重叠，不能相加为帧时间；CPU 实际频率仍未知。

## 长间隔与屏幕状态

用户反馈“长间隔应该是熄屏了”。因此最长 1,300.165 ms 先标记为**疑似熄屏污染**，不作为动画卡顿或版本回退证据。此间约 1,209.015 ms 没有已投递但未被 CPU 观察完成的作业，已观察完成作业的 GPU 区间累计仅 56.379 ms；这些数据只排除了整段都在执行 GPU 的解释，不能区分熄屏、动画停留或上游调度。

原始窗口包含该间隔，保留的首尾平均为 19.028 次/秒、六个间隔超过 100 ms。这是含未知屏幕状态 / 停留的原始统计，**不能作为亮屏运动动画的有效帧率或卡顿数量**。没有屏幕状态时间戳，不按时长阈值删点后重新声称有效 FPS，也不将剩余间隔自动归为持续亮屏动画。

## 下一步依据

1. 在同一时间线上记录显示电源 / 可见状态和 Welcome 实际提交，按状态划定亮屏窗口，保留原始记录，避免再次把熄屏当长帧。
2. GPU 优先研究真实 Apple AIR 的 color-matrix / 常量读取路径：22 条 scalar buffer load、half 数据布局、只读 buffer 绑定与寄存器占用。按原参数与边界语义设计整批优化，再验证实际程序及像素；不继续仅凭减少指令或调频推断收益。
3. 显示准备和实际传输链仍有明显时间，继续对照原 Apple IOMFB / CA 所有权和物理完成规则；保持真实 GPU 完成、缓存发布、单次扫描和显示读 fence，不提前完成以制造帧率。

没有本版本新的光学无闪烁验收。设备保留当前启动，不为重复对照重启或重新索取主观比较。

## 复算与证据

- [硬件证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-framebuffer-input-snapshot/hardware-evidence.json)
- [实现审计](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-framebuffer-input-snapshot/audit.json)
- [同窗口作业 / 显示分析](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-158/analysis.json)
- [逐程序与显示阶段分析](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-158/operation-analysis.json)

复算命令：`python3 build/gpu-runtime/ui-job-latency/analyze144.py round-158` 与 `python3 build/gpu-runtime/ui-operation-coverage/analyze150.py round-158`。原分析器未经修改；验证两个 snapshot 的顺序、2048 条历史覆盖、计数、时间和全部命令状态。已知完整内核日志插入的修复偏移及原始 UART SHA 均记录在分析产物，未知破损仍拒绝。
