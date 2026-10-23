# 首次亮屏卡顿与帧回退分析

用户观察：启动后的第一轮 Welcome 较卡、偶有画面回退；熄屏再点亮明显改善。这是本轮优先调查的现象，不能用不同语言、不同阶段的更新率当严格性能对照。

## 已定位的开销与证据

round162 使用 GPU1.142 / Metal1.4 / Display1.0.18，直接观察首次启动，没有先运行像素测试预热。完整历史包含 743 笔成功作业、0 失败和 264 次显示；所选 13.3713 秒内 496 笔完成、0 失败。GPU 实测 **595.966 MHz**，作业均值 3.088 ms、p95 16.132 ms、最大 19.226 ms。CPU prepare→admission 均值 4.230 ms，退休 0.712 ms；类别有嵌套和流水重叠，不能相加为帧耗时。

| 显示 token | 显示间隔 | 下一笔 prepare 前空档 | 同期最长 GPU 作业 |
| --- | ---: | ---: | ---: |
| 8→9 | 1983.921 ms | 1130.289 ms | 15.559 ms |
| 27→28 | 1150.042 ms | 1084.811 ms | 9.783 ms |
| 34→35 | 1100.621 ms | 1055.260 ms | 2.789 ms |
| 136→137 | 850.436 ms | 789.813 ms | 11.523 ms |

这些例子的大段空档发生在已记录的 GPU 准备入口之前，不能解释为 GPU 自己执行了一秒。prepare 位于内核 gate 内，范围还涵盖上游 CA 编码、Metal 编译／调度、资源管理、gate 等待及动画正常停留；没有线程运行与编译计时，不能断言是纯 CPU 忙或某个 shader 编译。前 40 次显示的 39 个间隔中位 49.983 ms、最大 1983.921 ms；这也不是经内容标记后的运动帧率。

被动状态采样没有发送 CADisplayStateControl transition IPC。选定窗口 117 个边界及内部样本均为逻辑 on，但最大采样空档 1398.671 ms，**不足以证明连续亮屏**。顺序读取 absolute / continuous 时有一次 0.631 ms 的差值异常，之后回到低差值；解析器保留旧精确时差 guard 失败记录，以完整观测范围收缩时间窗口，未把它静默改成精确时间对齐。完整 momentsd 日志插入仅移除明确匹配的整行，保留修复记录及原始字节。

复算：[窗口脚本](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-passive-power-observer/analyze-window.py)、[冷启动关联脚本](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-cold-start-analysis/analyze-cold.py)、[窗口结果](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-162/analysis.json)、[长间隔结果](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-162/cold-start-analysis.json)。

## 冷启动路径的具体问题

当前 `captureNativeShader` 随 `_metalDiagnosticsEnabled` 开启；首次 fragment 在 `_compilerLock` 内计算身份、序列化含 GLSL／反射／程序映像的 JSON、生成十六进制分块文件并原子写入。每设备最多 32 文件、8 MiB；失败写入仍计预算。`captureArchivedNativeShader` 甚至在实际二进制已命中 archive 时，为抓取源码重新 `lowerAIR`，仍持有相同全局编译锁。

这是确定存在的额外冷路径工作，首次 shader 较集中、已抓取程序以后跳过，符合冷暖差异的方向；**还没有每次抓取耗时，不能认定它解释了全部卡顿或画面回退**。窗口开始前已有 25 次程序注册，窗口内新增为 0；这一指标计内核注册，不包含用户态编译时长，后段停顿仍需调查上游调度／动画阶段。

Apple 原版 QuartzCore 的 Metal context 构造函数 `0x1844ac088` 所在分析明确区分预编译管线、调试选项、GPU frame capture 与不支持硬件；原版 `CAMetalDrawable` present core `0x184459948` 保留目标时间和 deadline。它们支持将重型抓取与正常渲染分开、保留时间和呈现契约；不能据此给 A540 套用 Apple GPU 二进制或假定本机命中了原版预编译缓存。原始分析见 [Metal context](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-framework-lifecycle/metal-context-constructor.json)、[present core](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-framework-lifecycle/present-core.json)。

## CPU 与回退的调查边界

当前 74 bundle 内核集合中没有 `org.darwin.driver.QcomOSM`，23 个本地 bundle 也没有它。源码的 XNU perfcontrol→OSM 自动策略因此没有随这套集合加载。完整设备树已通过 `msm8998.dtsi` 声明该 OSM provider，实际 Apple DT 也包含 v1 compatible；缺口在内核集合打包。当前 GenericClockController 二进制和 XNU 内核已有自动策略及所需 exclusive perfcontrol KPI，但仍须核验固件 OSM/LUT 是否可用，不能仅凭驱动加载认为调频已生效。固件仅报告 cluster1 configured=300MHz、measured=0；这不是运行频率。先核对 secure 初始化、8998 v1/v2 布局、四核 LUT、物理 cluster 及实际设备接口，再决定独立的 CPU 平台接入，禁止猜电压／PLL。

264 个显示 token 全部单调，未发现旧请求 token 重新扫描。该结果**不能排除像素内容回退**：缺少 CA 内容代次、surface seed 与显示关联，后续需沿同一内容帧追踪，不修改 FIFO 或提前 signal fence 来掩盖现象。现有真正完成、IOSurface producer fence、PP_DONE＋latch＋idle 及读访问退休规则保持。

## 本轮合并实现

GPU1.143 / Metal1.5 将 shader 抓取独立为 `gpu-metal-shader-capture=1`，默认关闭，且仍要求 Metal 错误诊断和现有连接授权查询成功。缺失属性、类型错误和不匹配版本均关闭。保留 `gpu-metal-diagnostics=1` 的错误上报；默认路径不再为 shader 抓取重新 lowerAIR 或生成／写入文件。gpuctl 只读查询新增抓取版本与 QcomOSM 初始化／策略状态。

arm64 adapter、GPU kext 和 gpuctl 构建通过；135 个 AOT 与原 registry 字节一致，未改变 GPU 提交／调度、shader、显示或 fence。编译 archive context 随 adapter 源码改变，以拒绝旧缓存。候选独立完成 74 bundle／23 本地 bundle 核对和 13 HFS 文件读回；设备身份、当前壁纸 trust、启动参数保持，DT 仅新随机 seed 改变。正在进行一次合并真机验证，先采首次动画再运行正确性套件；尚无冷启动改善或 60 FPS 验收。

GPU1.142 的 SMR/S2CR 批读取实验通过正确性检查，但现有不同场景数据没有证明 CPU health 开销下降；不将其计为已验证性能收益。

## 合并候选 round163 结果

GPU1.143 / Metal1.5 已启动，四核验证通过，四条控制台命令均返回 0。真机 `GPUMetalDiagnosticVersion=1`、`GPUMetalShaderCaptureVersion=0`，`QcomOSM count=0`。常量窗口十阶段、render-state、fence 和 CARenderer 八帧各 4096 像素均通过。实际 CA 动画先采、正确性测试随后运行。

13.5122 秒窗口内 741 笔完成、0 失败，GPU 595.967 MHz，GPU 作业均值 3.224 ms、p95 17.483 ms、最大 37.205 ms。窗口 277 次显示、276 个原始间隔，中位 33.423 ms、最大 1766.629 ms，首末更新速率 21.362/s；内容保持、采样空档与场景差异存在，不能作为独立性能收益或 60 FPS 验收。

完整历史 874 笔成功、0 失败；显示 token 仍无倒退。首次 40 次显示最大间隔 2750.352 ms，其中下一笔 prepare 前空档 2242.432 ms、同期最长 GPU 作业 19.679 ms。**长停顿仍存在，关闭 shader 抓取并未解决根因；未证明修复内容回退。** CPU 策略缺失与上游时间线需要继续处理，不能继续把这些秒级停顿归入 framebuffer shader。

[新窗口](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-163/analysis.json)、[冷启动关联](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-163/cold-start-analysis.json)、[硬件控制台结果](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-163/report.json)。

## CPU 接入与 round164

完整设备树已经有 OSM 节点；实际内核也有 exclusive perfcontrol KPI、GenericClockController 和 QcomPowerController。进一步核对后，只向集合补入 **QcomOSM 一个 bundle**，原有 23 个本地驱动保持字节一致，成为 75 bundle／24 本地 bundle；未替换时钟基类或 GPU。对实际保留的两个 provider 重新核对所有导入，通过 CPU 策略／OSM 校准 LUT 和错误分支的 ASan/UBSan 主机检查。

真机 `QcomOSM count=1`，但 **SecureOSMInitialized=false、CPUAutomaticPerformanceAvailable=false**，自动策略启动被拒绝。按照当前 v1 布局和这份实现的分支，初始化结果为 NotReady，没有获得可用的已启用 bank；下一步需要对照原始设备／固件确认布局、受保护 OSM/CPRh/ACD 与 LUT 初始化，不能强行 enable 或猜测电压。补齐打包没有改变 CPU 频率，也不能解释本轮时间线差异为 CPU 提速。

CPU 原生查询工具本轮全部返回 126：新加入 HFS 时未保留执行权限，落成 0644。工具签名有效，但没有实际执行，**这些行不构成频率／策略切换证据**。已修正 `build-power-client` 的 ad-hoc 签名以及打包使用 `copy2` 保留 0755；另存修正版候选，未再次重启设备。现有只读 registry 已足以证明策略未启用。

本轮启动四条控制台命令均返回 0、无 panic，常量窗口、render-state、fence 与 CA 八帧像素检查通过。runner 总退出 1：为新增 powerctl 的 sandbox 路径仍满足原生 NVRAM boot-args 小于 1024 字节，移除了无用 Bash IOKit 路径及 `-enable_kprintf_spam`，因而没有四核 bring-up 日志，runner 无法独立验收四核。不能称本轮四核验收通过，也没有证据显示实际少了 CPU。

12.5859 秒窗口完成 836 笔、0 失败，GPU 595.967 MHz，作业均值 3.153 ms、p95 15.797 ms、最大 19.462 ms；CPU prepare→admission 均值 4.596 ms。显示中位间隔 33.465 ms，最大 333.252 ms；完整历史 1218 笔成功、0 失败，显示 token 仍单调。启动第一个 1270.514 ms 间隔里，下笔 prepare 前有 1139.745 ms 空档。采集脚本预置、调试日志设置与动画场景均有差异，**没有严格性能收益归因，首次卡顿与语义帧回退未解决**。

[CPU 接入硬件证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-cpu-policy-integration/hardware-evidence.json)、[round164 窗口](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-164/analysis.json)、[冷启动关联](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-164/cold-start-analysis.json)。
