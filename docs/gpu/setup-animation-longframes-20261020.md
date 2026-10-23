# Setup 唤醒与 HOME：批量长帧采集

本轮无需现场操作。通过测试专用的原生 HID 上报、独立的 80 ms 自动松开定时器，以及渲染服务器的独立 IOSurface 截图，重复验证唤醒到 Hello、HOME 到语言页。没有实现或使用触摸返回；锁屏仅用于重置测试场景。

## 可复核的基线

- GPU 1.157 / Metal 1.30，RAM 载荷 SHA-256：`3a547e0eb51cde138ac783aae14534b05fcb58113d2ccbef6894764c53a0ee0a`。
- 6 轮操作，19 次按键（含首次唤醒），按下和自动松开的 HID 报告均确认；18 个显示状态端点均符合关闭/开启预期，12 张截图成功保存。首轮图片确认了欢迎画面和语言选择页。
- 连续 30 个 5 秒窗口，每窗保存 power、timeline、operations、jobs、native、metal、frames、input、failures，270 次查询全部返回 0。
- 已完整导出 14,968,440 字节诊断文本；连接 4,703 个作业（序号 1–4703，无缺口）、3,230 条去重的慢操作、5,195 条 Metal 时序、1,488 个显示完成记录、54,035 条显示时间线事件。
- 数据：`build/setup-animation/vector-clear-headless-01-data-{00-09,10-19,20-29}/`；汇总：`build/setup-animation/clear-vector/headless-all-{joined,cycles}.json`。

| 轮次 | 唤醒后首个显示完成 ms | HOME 后首个显示完成 ms | HOME 起 400 ms 内显示完成数 | HOME 起 2 秒内最大完成间隔 ms |
|---|---:|---:|---:|---:|
| 0 | 1123.45 | 18.78 | 11 | 666.75 |
| 1 | 913.72 | 11.29 | 12 | 650.12 |
| 2 | 953.60 | 5.15 | 12 | 683.47 |
| 3 | 956.20 | 8.71 | 12 | 633.28 |
| 4 | 952.55 | 25.61 | 11 | 666.79 |
| 5 | 1144.44 | 15.51 | 12 | 600.32 |

这些是完整显示提交的完成计数，包含背景更新，不等同于光学视频或经过语义识别的动画帧。400 ms 从按键按下计时，包含约 80 ms 的按键保持时间。欢迎页默认淡出时长 0.4 秒，语言页的原生状态动画另有时长；两段不能混为同一动画。

## 固件路径与已定位成本

输入使用与物理按键相同的 IOHIDDevice 和描述符：Consumer Power `0x0c/0x30`、Menu `0x0c/0x40`，之后经过 IOHIDEventDriver 和原生 UI 策略。测试绕过 GPIO、PMIC 按键采样及指纹安全世界采样，不用它代替物理输入电气验证。此前 `SBSUndimScreen` 不能可靠完成关屏后的唤醒，旧软件注入记录不作为本轮唤醒对照。

欢迎页退出和语言页的固件分析见 [原生页面路径分析](setup-hello-language-transition-analysis-20261020.md)。主线是欢迎视图退出、CoverSheet 内容控制器切换、Buddy 语言页及其 CA 状态动画；Fluidity/Mercury 背景另行提交 GPU 作业。

1. **索引 CPU 提交成本已下降。** 旧记录中三绘制作业准备平均约 50.6 ms；缓存暂存后校验的实现已包含在本轮基线。同形态作业的新采集平均约 15–16 ms，索引快照校验和地址空间准备仍有成本。跨旧采集的均值是作业形态比较，不能代替完整场景 A/B。
2. **全屏清屏身份已直接确认。** 对齐的两笔清屏分别为 `blit_fill_rows_2d`（4320×1920，stride 4352，均值约 4.09 ms）和 `blit_fill_pairs_2d`（8640×1920，均值约 9.48 ms）。两笔合计约 13.6 ms；此前重复工作形态约 52.6 ms。带两像素边框的 4328×1922、stride 4352 附件仍落入旧逐 word 清屏，观察到 24.58 ms。
3. **相同不支持函数被反复同步编译。** SpringBoard 的 `VTMTSComputeFunction`，AIR 469,936 字节，SHA-256 `126ececb99aa35e19dccda5208abcf3ddd1eb094f6c55782a27a2f37b110b287`，202 个已解析常量，反复使用同一变体键。每轮锁屏/唤醒窗口出现约 1.48–1.55 秒和 0.87 秒的失败调用。新建库还重复做约 0.56 秒 AIR 反射解析。GPU 失败报告按内容去重，计数为 1 并不代表只执行过一次失败编译。具体不支持项仍是活动的 `texture2d<uint, write>`；本轮不把它伪装成浮点纹理。
4. **后半段切页还有重工作与排队。** Fluidity 三绘制 GPU 作业约 36 ms，其中主绘制常见约 28.34 ms，两笔 SDF 绘制约 3.96 ms/笔；这些是 CP 操作边界耗时。部分提交等待约 150–180 ms。多个 200–680 ms 显示间隔的主要时间发生在下一次显示 Prepare 之前，需结合 CA 编译、生产者与合成提交分析。GPU 核心计数测得约 596 MHz。

## 本轮候选实现

GPU 1.158 / Metal 1.31，RAM 载荷 SHA-256：`bf0893c7e64eb5679c0f55239119ab86d2a5a4c0359134dce3488c3e96b2eb93`。

- 行宽为完整 word、行首仍按 16 字节对齐的清屏增加二维尾部内核。每个线程处理最多四个活动 word，尾部不读改写相邻字，也不写行填充；完整 uint4 路径保留。内核重新构造规范参数，检查精确 dispatch 及对应的内核名称，再把参数冻结到只读 IB。
- 进程内复用不可变的 AIR 反射数据，最多 32 项，每项最多 512 个常量及 64 KiB 名称。每个库重新构造原生反射对象，避免跨库共享可被调用方接触的对象。
- 以完整编译上下文、库、函数、常量和线程上限的 SHA-256 键缓存确定的 `AIR JIT: unsupported …` 前端失败，最多 64 项、每项原因不超过 4096 字节。保留原错误和原生回退；后端、内存、注册、提交、硬件失败不缓存，不写磁盘。成功的二进制档案仍优先查找。
- Metal Function 时序 detail 新增：`0x20` 确定失败命中，`0x40` 进程反射命中。旧的成功档案位含义保留，可组合出现。
- 串口压缩导出改为压缩级别 6、1024 字符分块，降低采集结束后的传输成本；压缩前后长度、分块偏移、zlib 完整性和 SHA-256 仍核对。Bash 结束标记主动输出前导换行，防止紧贴未换行的内核日志导致误等超时。

## 验证与边界

ASan/UBSan 的提交和常量窗口检查通过；常量窗口覆盖 8,224 个边界组合。确定失败缓存覆盖完整键隔离、原错误保留、瞬态错误排除及固定容量淘汰。GPU 集成 5 项、输入集成 6 项通过。

真机清屏 52 项、103,205,833 字节逐字节匹配，包括 1082×1922 附件、尾部 word、行填充、前后保护区；索引绘制、常量窗口、字窗口检查通过。像素回读期间只重新连接过主机串口观察器，没有重新运行设备测试或重启设备。原生 `GPUStartTime/GPUEndTime` 此工具返回 0，故不把测试打印的 elapsed_ms 作为纯 GPU 时间。

对照性能采集已完成，像素校验通过不等于动画验收完成。DPU 单次传输结束后原 IOSurface 可能已被生产者复用，原来的截图所有权保护保持有效；页面图片取自渲染服务器的新目标，显示完成数据另由驱动记录。没有伪造 Scheduled、完成回调或 fence。

## 1.158 / 1.31 完整对照

同样执行 6 轮、19 次原生 HID 脉冲，按下/自动松开均确认，18 个显示状态端点、12 张页面图片均成功。30 个连续 5 秒窗口的 270 项查询全部返回 0，六批共 270 个诊断文件已完整导出。连接 4,983 个作业，无缺失序号；去重慢操作 3,212 条、Metal 时序 5,645 条、显示完成 1,020 条、显示时间线 54,039 条。六轮唤醒和 HOME 的前 2 秒及整个操作阶段均有采样覆盖。

| 轮次 | 唤醒后首个显示完成 ms | HOME 后首个显示完成 ms | HOME 起 400 ms 内显示完成数 | HOME 起 2 秒内最大完成间隔 ms |
|---|---:|---:|---:|---:|
| 0 | 1060.38 | 25.79 | 11 | 583.43 |
| 1 | 928.50 | 32.87 | 11 | 633.43 |
| 2 | 830.31 | 24.03 | 11 | 666.89 |
| 3 | 1005.86 | 9.58 | 12 | 617.18 |
| 4 | 956.07 | 0.00 | 12 | 633.41 |
| 5 | 916.54 | 42.91 | 11 | 650.07 |

首个完成可能是按键前已经提交的欢迎页/背景帧，尤其第 4 轮约 0 ms 的结果不是语言页瞬间到达。唤醒中位数由 954.90 ms 变为 942.29 ms，整体未出现足以宣布解决卡顿的改善。两轮比较没有光学视频，也不把这些完成计数称为实际动画帧率。

已直接验证的局部收益：相同行字节宽 4328、1922 行、stride 4352 的行清屏，GPU 耗时由 24.56 ms 降到 6.77 ms；相同尺寸成对清屏由 14.08 ms 降到 6.78 ms。`VTMTSComputeFunction` 第一次失败仍需约 1.52 秒，紧随其后的同键失败为 6.06 ms，detail 为 `0x20`。其余已覆盖唤醒阶段不再观察到该函数的重复编译调用，因此不额外宣称它的反射缓存命中收益。

数据目录为 `build/setup-animation/vector-clear-final-01-data-{00-04,05-09,10-14,15-19,20-24,25-29}/`，汇总为 `build/setup-animation/clear-vector-tail/all-{joined,cycles}.json`。

## 剩余瓶颈的边界

第 2 轮 HOME 有一个 666.89 ms 的显示完成间隔：下一次 Prepare 前占 574.42 ms，Prepare 到 Active 为 61.73 ms，实际屏幕传输为 30.54 ms。区间内 21 个 GPU 作业，其中完整观测到的 GPU 工作合计 154.23 ms，最长作业排队 30.79 ms。随后两个间隔为 533.29 / 433.41 ms。这里不能用“GPU 排了 600 ms”解释，也不能把加深队列作为修复。

同一 666.89 ms 区间内，硬件 IRQ/worker 各 41 条，原生 VBL 观测与发布各 40 条。VBL 观测最大间隔 16.99 ms、发布前时间点最大间隔 16.99 ms，节拍约 60 Hz；六轮 HOME 的前 2 秒分别都有 120 个原生 VBL 观测，最大间隔均低于 17.65 ms。因此该批长帧并非 TE/VBL 时钟停了数百毫秒。原始时间线已保存，补充结果为 `clear-vector-tail/display-cadence.json`；这也不等于客户端已处理了每一条通知或显示了新内容。

背景生产者仍有三绘制约 36.27 ms 的 GPU 工作，加上两笔约 13.7 ms 的清屏、CPU 准备和用户态提交等待，已经超过 16.7 ms 帧预算。合成器及生产者在其他时段也存在 CPU 工作和等待；清屏改快与整体过渡仍卡顿可以同时成立。全屏清屏、地址空间、实际纹理/绘制和回调各自计时，重叠区间不能相加成 CPU 忙碌时间。

另以同一 RAM 候选补采 2 轮原生唤醒/HOME 操作，顺序抓取 30 组 backboardd（PID 65）、SpringBoard（PID 32）、背景生产者（PID 249）等待栈及显示状态。这是单独的侵入性归因采集，不混进上面的性能对照。121 个文件（含操作标记）均已完整导出并校验，90 份选进程 stackshot 全部成功。导出脚本的混合文件网格循环缺少 `in` 已修复；设备采样无需重做。

用户帧使用捕获的加载基址/UUID，按原始 22H374 dyld cache、UUID 匹配的本地 Metal 驱动解析；私有符号来自同一缓存的 symbols 文件，其 UUID 与主缓存 header 的 `0x190` 字段匹配。内核及自编译 Kext 按该候选实际预链接段地址解析，不能把独立 Kext 的零基址直接套到运行地址。

- 显示主线程：`CA::WindowServer::IOMFBDisplay::finish_update` → `CA::OGL::MetalContext::flush` → `commitAndWaitUntilSubmitted` / `waitUntilScheduled`。可见在真实 native Submit 内构建地址空间，或等待同一进程的传输锁。
- 背景主线程：CA transaction → `commit` → `flushDirectDrawable`，另有 inline snapshot 分配等待传输锁。直接 drawable 的生产者必须在呈现前建立真实 IOSurface 生产者 fence，不能仅提前标记 Scheduled 来移除等待。
- 原生提交的内核栈出现 `QcomGPUMapper::createAddressSpace`、逐页映射及 `State::check`；另有 `QcomGPUPlatform::validate` → `GenericARMIO::clockRequest` 在反复创建匹配字典、枚举全体时钟控制器。
- 诊断上报线程和部分完成处理等待 GPU provider 的同一 command gate；它们的等待不能单独证明诊断上报持锁很久。独立栈点不是持续时长证据。
- SpringBoard 栈确认正在执行 CoverSheet 收尾和原生显示链接/手写路径更新。没有把不存在的触摸返回当成测试前提。

栈文件：`build/setup-animation/vector-clear-final-01-wait-stacks-data/`。解析脚本：`build/setup-animation/decode-wait-stacks.py`；用户态私有符号结果：`clear-vector-tail/wait-stacks-private.json`；包含内核/Kext 的结果：`clear-vector-tail/wait-stacks-symbolicated.json`。

## 基于路径的下一项修复

GPU 1.159 候选把 GPU 平台租约绑定到首次非阻塞、唯一匹配、同一总线下找到的 resident 时钟控制器。每次 Validate/Release 继续调用真实控制器，检查在线状态、provider 层级、controller kind、原协议及不变的 opaque token；不缓存硬件健康结果、不跳过 MMIO/租约校验、不转交仍存活的 token。这样去掉热提交及资源释放路径内重复的全局服务枚举。

候选仍使用完全相同的 Metal 1.31、AOT registry、清屏与索引实现。驱动构建、目标 Kext 校验、18 项 GPU 集成/串口检查通过；真机 info、索引绘制、常量窗口、字窗口四条检查均返回 0。

## 1.159 批量结果与当前载荷

`platform-routes-01` 从内存启动，载荷 SHA-256 为 `349cfc62c67d84c7a1128917ab2328674f050f59689eaee6a1a93989c9bc9e07`。30 个窗口的 270 项查询全部成功，270 个诊断文件完整导出；压缩前打包文本 17,535,039 字节，压缩后 3,413,440 字节。连接了作业序号 497–7422 共 6,926 条，区间内无缺口；前 496 条启动前史不在此次保留范围内。另有 4,272 条慢操作、8,677 条 Metal 时序、2,065 个显示完成、59,529 条显示事件。最终原生计数为 submitted/completed 7422/7422，failed=0。

首轮输入上报均成功，但“locked”端点仍为 `(1,1,1,1)`、“hello”端点为 `(0,0,0,0)`，所以第 0 轮无效，保留原始数据并剔除性能比较。其余五轮的 15 个状态端点全部正确。这种状态错误不能靠 HID 上报计数或成功保存 PNG 判作页面测试成功。

| 有效轮次 | 唤醒后首个显示完成 ms | HOME 起 400 ms 内显示完成数 | HOME 起 2 秒内最大完成间隔 ms |
|---|---:|---:|---:|
| 1 | 891.81 | 12 | 533.45 |
| 2 | 840.10 | 13 | 583.41 |
| 3 | 887.25 | 12 | 616.76 |
| 4 | 786.25 | 13 | 583.44 |
| 5 | 883.10 | 13 | 666.81 |

有效唤醒窗口的中位数为 883.10 ms；上一版同为第 1–5 轮的中位数是 928.50 ms。起始显示状态和启动后时间不同，不能把整个差值都归因于路由优化，也不能由显示完成数推断实际动画帧率。原生地址空间 BuildHealth 均值由 599.30 µs 降到 451.40 µs，RetireHealth 由 539.67 µs 降到 365.03 µs；逐页 BufferMap 仍为约 2.21 ms。背景三绘制仍约 36.3 ms，准备约 14–15 ms，持续长间隔尚未解决。

自动操作工具已补充状态检查：关屏不符合预期时最多再上报一次 Power 脉冲；仍不符合则输出 `SETUP_CYCLE_INVALID` 并停止，唤醒和下页也检查四个状态。主机采集脚本保留原生操作退出码，所有查询仍会完成并记录；测试桥进程退出 0 不再被当作设备命令成功。真实生成的 Bash 脚本验证了成功、按键操作失败、单项查询失败三个分支，失败会传播且不会退出父串口 Bash。

当前设备运行 `platform-routes-02`，载荷 SHA-256 为 `6a05e75efcca0b381745bb83bf1157cc45d7df340680847a317ea255f3a9bda9`。其内核与 01 逐字节相同，Metal 也相同，HFS 变化仅 `displayctl`。一次冷启动检查确实拦截了“报告已上报但屏幕状态未改变”的操作，未计入有效轮次；之后同一启动中完成两轮操作，7 次脉冲全部确认自动松开，6 个状态端点全部正确，4 张图片成功保存。证据为 `platform-routes/guard-native-validation.json` 和 `platform-routes-02-guard-actions-0/`。未刷写持久固件。

导出还覆盖了插进 base64 行内的精确内核权限日志，保留原始 UART、记录被剥离的日志，再检查完整偏移、长度、zlib CRC/EOF 及文件 SHA。新增 `--decode-only` 后从已保存串口恢复，无需重采；121 个旧等待栈文件重解码后的文件 SHA 全部相同。

完整对照位于 `build/setup-animation/platform-routes/all-{joined,cycles}.json`，其中 `cycle_state_valid=false` 的窗口必须排除。有效采集与导出摘要为 `platform-routes/capture-validation.json`。

## 仍未实现、但不能混同的项

在 1.159 阶段，`VTMTSComputeFunction` 所需的无符号纹理写入尚未实现；后文 1.160/1.161 已补齐并验证实际派发。失败缓存本身没有代替真正的 `texture2d<uint, write>` 支持。背景绘制及清屏仍有真实 GPU 成本，合成器仍存在用户态准备/提交等待，不能通过提前信号 fence 或伪造 Scheduled 消除。

`QcomMTLCommandBuffer` 的 `GPUStartTime/GPUEndTime/kernelStartTime/kernelEndTime` 仍返回 0。已追到 QuartzCore 的 Scheduling/Executing signpost 回调及 GPU duration 存储；现有证据没有证明这四个返回值是数百毫秒停顿的根因。相应原始反编译保存在 `platform-routes/QuartzCore-*.c`。真实 GPU 时钟尚未校准到主机时钟，不能拿 CPU 提交或回调时刻冒充 GPU 时间。

## 1.160 / Metal 1.32：补齐 VTMTS 的实际整数纹理路径

1.160 测试使用 RAM 候选 `uint-render-02`，载荷 SHA-256 `4370b68ecb0df718e1573321c68a8a462b122869539867dd0e68b0396d5cfdf2`，未刷写持久固件。Shader 协议为 44、JIT 为 39，描述 2468 字节、完整四变体镜像 83720 字节。AOT 保留原有 140 项的顺序，追加四个公开自编 AIR 校验例子；当前目录为 144 项。

已采集的 VTMTS 特化会把 half4 按 BGRA 顺序排列，再用 `air.pack.unorm4x8.v4f16` 打包一个 uint32，广播后写入 `texture2d<uint, write>`。这需要 R32Uint 原始字存储，并非把 uint 转成 float 的颜色写入。现在补齐了 AIR C++、设备 C API、MSL、描述字段 `imageUintMask`、内核类型校验、R32Uint 视图及 BGRA/RGBA IOSurface 的四字节原始映射。浮点着色器不能绑定 R32Uint；整数过滤、整数渲染目标和整数 MSAA 仍显式拒绝。

沿完整固件路径还补齐了：有界的线程私有 int32/float32 数组（单入口最多 1024 字节、最多八层数组）、half2/half4 与 uint 字的转换、half/float unorm4x8 打包。数组形状先验证，再计算布局；设备 LLVM 只导出旧的 32 位数组长度 API，因此还核对原始长度拼写，拒绝被截断的大数组。包括 0、65、257、4294967304 长度的拒绝用例。

原先 JIT 的分支栈上限 3 也会拒绝此着色器（实际需要 9）。Mesa A5xx 编译器配置 64 个重汇聚条目，SP 寄存器按成对条目编码。无线程组屏障的着色器现允许至 32；带屏障的着色器继续维持原有 3 的限制，保留最大线程组的驻留边界。没有跳过描述/类型/资源范围校验。

使用全部 202 项真实观测常量，通过生产 AIRRuntime + Mesa + JIT 验证的结果为：1952 个代码字、48 个 full 寄存器、25 个 half 寄存器、image/uint mask=2、sample mask=1、buffer mask=3。主机转换和编译合计约 125 ms；这不是设备首编译时间，也不是动画帧耗时。记录在 `uint-texture/vtmts-runtime-result.json`。

主机检查通过：完整内存编译器、新类型/超限数组/屏障栈拒绝边界、ASan/UBSan 的 GPU 提交/常量窗口/纹理布局，以及 18 项 GPU 集成和串口测试。真机已验证 AIR 和 MSL 整数复制、高位整数、uint swizzle、unorm4x8、BGRA 原始字视图、IOSurface 写入、2D/array/cube/cube-array 的 mip1 及越界调度、65,536 种 half 位模式（含 NaN 位模式）、256 个私有数组实例。相同 GPU/Metal 的索引绘制、常量窗口和 word-window 回归也通过。

第一份候选的小 IOSurface 请求了 1152 字节分配，被驱动的整页导入条件拒绝；第二份把测试分配扩大到 16384 字节后通过，内核、Metal 驱动和编译器动态库与第一份逐字节相同。随后实际 UI 采集证实，IOSurface 的逻辑分配长度可以不对齐整页，因此前一次把此问题归为测试错误的判断不成立。第二份只验证了整页测试形状，没有修复这条兼容性边界。完整原始记录保留在 `uint-texture/native-validation.json`、两个候选的启动记录及 `uint-render-01-unit-failures/`。

六轮自动唤醒和 HOME 进入下一级页面已全部采集和导出，18 个显示状态端点正确，270 个查询均成功。270 份文本合计 14,611,940 字节，压缩导出 2,923,571 字节，逐文件 SHA、偏移、zlib CRC/EOF 检查通过；另导出 12 张渲染服务器截图。共有 5403 个连续作业（序号 1–5403 无缺口）、3476 个慢操作、6185 个 Metal 事件、1247 个显示完成和 54445 个显示事件。摘要见 `uint-texture/batch-summary.json`，完整关联数据见 `uint-texture/all-joined.json` 与 `all-cycles.json`。

唤醒后的首个显示完成为 1687.89、901.48、1059.41、975.58、892.17、951.72 ms；HOME 后两秒内的最大显示间隔仍为 533.39–633.38 ms。显示完成不等于光学帧率或 Hello/语言页的语义动画帧，HOME 的首个完成也可能仍属于 Hello。语言页截图包含完整语言列表；逻辑 Hello 截图有明显色带/多边形，未见 Hello 字迹，图像正确性仍需复核。

实际 SpringBoard 首次 VTMTS 编译已成功：反射 564.50 ms、AIR 932.63 ms、IR3 637.47 ms、上传 14.53 ms，总计 2167.92 ms；后续正缓存调用约 7.30 ms。但其输出 IOSurface 仍被拒绝：810×1440、BGRA、行距 3264、逻辑分配 4700224 字节（64 字节前缀加完整图像）。操作记录中未见 VTMTS 的 GPU 程序/派发，不能据编译成功推断转换已运行。背景三绘制作业 317 个样本的 CPU 准备均值 15.08 ms、GPU 均值 36.62 ms，仍超过单帧预算。

只筛选完整包含于操作后两秒、且显示状态采样全开的间隔，六轮 HOME 的最大间隔中有 463.62–573.04 ms 发生在下一次显示 Prepare 之前，占间隔的 84.29%–91.70%；Prepare 到 Active 24.22–60.34 ms，启动到传输完成 14.40–29.26 ms。该区间仍有生产者和合成器工作，不能把它直接命名为 GPU 空闲或某个锁的持续等待。详见 `uint-texture/interval-attribution.json`；跨越锁屏或窗口边界的间隔已排除，后续需在同样边界下比较。

## 1.161 / Metal 1.33：IOSurface 尾页及 AIR→NIR 合并验证

「对比 NativeMetal GPU 驱动」已完成运行时接入并冻结编译器。合并候选 `nir-tail-02` 的载荷 SHA-256 为 `65fab30094e9b2503d4cd21449cd9a42e2c02ddd2dfeab8c1c6a92a54fbc10b0`；仅从内存启动。编译器动态库仍与其冻结版本逐字节相同（`c44c6d850648d064bfb323bae81f02701e14b42f38a0b8e23882a66b6656cac7`）。

IOSurface 导入现在分别验证逻辑图像范围、原始描述符长度、原生 DMA 长度和页映射跨度。保留原始父资源、描述符、物理页及所有权校验；仅 PTE 范围向上取整，纹理视图不暴露尾部填充。审计本地 Family 二进制确认：ResourceData.bytes 来自根资源保存的 DMA 长度，mappedBytes 来自映射长度减偏移；二者不能强行等同于 IOSurface 的逻辑分配长度。证据在 `surface-tail/native-resource-length-audit.{txt,json}`。

真机整数纹理测试通过三种 IOSurface：整页 16384 字节、逻辑 1152 字节，以及真实的 810×1440、pitch3264、offset64、allocation4700224；像素、填充及越界调度保护均通过。完整 UI 中已观测到 VTMTS 的 GPU 派发，实际特化为 1280 words、40 个常量单元、24 full / 15 half 寄存器，7 个慢操作样本均值 16.97 ms。它与前述 202 项常量的离线样本不是同一特化。

新一轮自动六次唤醒及 HOME 操作全部有效，18 个显示状态端点正确，270 个查询成功。导出完成 270 份诊断、12 张 PNG、三份冷启动/预热快照；6338 个 GPU 作业序号连续，无失败或恢复。图像导出曾因 base64 行内的 APFS 日志中止；扩展精确日志白名单后，从原始 UART 重解码并核验偏移、长度、CRC/EOF、SHA，没有重采。完成凭据为 `surface-tail/batch-summary.json`。

Hello 截图恢复了手写文字和“按下主屏幕按钮来打开”，语言页列表完整；背景仍有色带，不能据此宣布所有渲染正确。唤醒到首个显示完成为 883.78、1010.03、849.53、881.83、948.58、1263.14 ms。HOME 起 400 ms 内有 12–13 个显示完成，起两秒内最大间隔为 533.40–633.45 ms。显示完成并非语义动画帧，间隔可能包含 400 ms 动画结束后的静态期。

背景三绘制 324 个样本的准备均值 14.72 ms（其中地址空间 7.46 ms），GPU 均值 36.61 ms，与 1.160 大致相同。全屏清屏的 rows2d/pairs2d 各 320 个样本，均值分别 4.09 / 9.48 ms，合计 13.57 ms。缺失路径已补齐，持续卡顿仍未解决。

AIR→NIR 与原 GLSL 的冻结镜像在 A540 做了两次完整矩阵，每次 16 项计算及 324 项片元（9 个特化、两种常量窗口、1/2/4 MSAA、六组输入），检查原始样本、缓冲区保护和未改动常量。首次 339/340 通过：Fluidity0/base/单样本/seed6 的 GLSL 输出有 754 个异常 half 分量及两个 NaN，而 NIR 输出有限。第二次 340/340、3,098,051 个值全部通过；对应 NIR 与首次逐位相同，GLSL 则恢复为相同输出。CPU 全帧参考及冻结镜像一致性也通过，但 CPU/GPU SFU 并非逐位参考。初次异常原因尚未证明，不能称全部 680 项均通过，也不能归因于 NIR 前端。完整证据在 `air-nir-hardware/`、`nir-tail-02-hardware-data/` 和 `nir-tail-02-ui-hardware-repeat-1-data/`。

后续针对原生附件清屏及提交成本继续处理；总目标尚未完成。

补充路径归因：1.161 六轮最大 HOME 间隔的起点均晚于按键约 400 ms，不能将其全部称为默认淡出动画的长帧。`surface-tail/interval-attribution.json` 保存了每个间隔相对输入的起止时间。冷启动/预热快照与完整 UI 共记录 26 次 NIR、5 次 GLSL 回退和 12 次缓存/其他 Function 调用；detail bit16 表示临时缓存写入。首次观测 VTMTS 已命中持久二进制缓存，但反射仍用 493.49 ms，Function 总计 543.64 ms，后续调用约 7.82–8.73 ms。不能把首次 Function 总时间当作当次 AIR 编译。分段证据为 `surface-tail/cold-runtime-analysis.json`。

## Hello 上移卡片的后续定位

本轮继续只处理 Hello→语言页。400 ms 只描述已解析的默认淡出，不能用于宣布后续卡片上移动画已完成，也不能把其后的长间隔直接算作静止期。

采集改为先 HOME 唤醒，等待并保存 Hello 图像，再单独 HOME 触发转场。一次自动熄屏导致首个 HOME 仅唤醒的记录已排除；黑屏截图和错误的自动复位轮次均不进入对比。逻辑图像来自原版渲染服务器，不能等同于光学录像。

另修正诊断顺序：`gpuctl job-history` 只读最后发布的快照，必须先调用 `operation-profile` 更新快照，再读 job history。1.35 首次转场的旧快照已从同一窗口发布但尚未刷新的快照重新导出，没有重新执行动画。

### 提交与清屏

开始时运行的 Metal 1.34 候选把附件清屏改成普通 draw。实际背景两笔清屏 draw 合计约 27.5 ms，连同原有三笔 draw 约 64 ms，比 1.33 的向量 compute 清屏更慢。恢复默认向量清屏，保留原有诊断比较开关。

1.35 将每队列未退休 native submission 限为两笔，确实把一组转场中 compositor 最长准入排队由 584.28 ms 降到 5.74 ms，完成观测后到退休的最长额外等待由 411.80 ms 降到 2.64 ms；但热缓存下仍有约 0.5–0.65 秒的显示空档，因此没有把这项局部改善算作动画修复成功。

热缓存记录进一步显示，卡片阶段一个 compositor 命令缓冲会持续处理约 250–450 ms。以 HOME 后 1191–1646 ms 为例，22 个合成器作业包含 85 个操作，GPU 区间合计约 50.39 ms、CPU 准备合计约 93.76 ms；同期 17 个背景作业 GPU 区间合计约 295.65 ms。这些区间可能重叠，不能直接相加为一帧时长。小清屏和绘制分开准入，反复夹入背景工作，是本轮针对的路径。

1.36 把限流移到命令缓冲起点，允许同一缓冲继续提交全部 pass。热缓存目标窗口仍存在 583.79 / 466.37 ms 的显示间隔，没有单凭冷样本中较多的完成数宣布成功。冷样本仍包含新 shader 编译。

Metal 1.37 在该路径上继续做两项小改动：

- 附件清屏在编码时保存独立、不可变的参数快照，使用原有向量 compute shader；这些内部清屏可与相邻绘制一起提交。普通 blit 保留原有参数页，indexed draw、显式 fence/event/barrier 和内核物理别名校验保留边界。
- 新命令缓冲在本队列仍有 native 工作未退休时等待；已经开始的缓冲继续提交其 pass，避免把逐 pass 的限流重新引入一帧内部。原有内核容量限制、真实 Scheduled/Completed、IOSurface fence 和退休顺序均保留。

Metal 1.37 的热缓存转场在 HOME 后 400–2000 ms 内有 7 次显示完成，仍有约 517 / 383 ms 的间隔；针对混合清屏的 readonly-feedback 真机检查通过。Metal 1.38 将同队列 fence 的依赖交给已准入的 native FIFO；跨队列依赖及对外完成仍等待真实退休，错误路径仍终结全部待完成点。针对两队列、共享串行执行目标、复用 fence 和实际 GPU 回读的 fence-test 通过；热缓存窗口仍有约 767 ms 的显示间隔，不能认为已修复。

Metal 1.39 允许内部不可变清屏与 indexed draw 合批，仅当清屏目标与索引数据属于不同的驱动自有根分配，且两者都没有 IOSurface 或客户端内存别名。内核对实际物理别名和索引快照的检查保留。UInt16/UInt32 私有索引、baseVertex、instance/baseInstance 和叠加绘制的真机检查通过。

1.39 的转场起点和终点截图分别确认 Hello、完整语言列表；目标窗口没有 Function 编译事件、没有 GPU 作业失败。400–2000 ms 内有 8 次显示完成，主要时刻为 415、449、999、1365、1599、1732、1815、1916 ms；后续稳定段约 80–120 ms，但起步仍有 550 / 367 ms 的间隔。以上是驱动显示完成，含背景更新，不能作为卡片的光学帧率。GPU 1.161 / Display 1.0.23 在这些 Metal 对比中保持不变。

GPU 1.162 的 compositor 优先选择候选已拒绝并回退。预热出现 Metal timeout，转场起点实际为语言页上的选项菜单，不能计入 Hello 动画比较；没有据此认定超时根因。源码及设备恢复 GPU 1.161 / Metal 1.39。拒绝记录保存在 `hello-card/gpu-162-rejected.json`，候选源码单独保存；其大文件已删除，额外解除最后一个链接的数据为 543.1 MiB。

复核 [IOGPUFamily 调度分析](ios18-iogpu-scheduling-and-reuse.md) 后，补查原厂 WorkQueue / CommandDescriptor 的构造、提交和回收 ABI，证据保存于 `hello-card/native-workqueue-abi.json`。原厂 submitted 与 completed 不同，不能用提前完成替代排队；当前 A540 gate 与原厂 Scheduler 的锁关系也不允许直接替换工厂。本轮尚未启用原厂 WorkQueue，不将结构分析宣称为运行时复用。

单次定点栈采样的起止图片确认 Hello 与语言页。第一份采样脚本使用的 `/bin/sleep` 在设备上不存在，实际栈时间为 HOME 后 180 / 282 ms，不能用于解释后续长间隔。修正为 Bash 定时读取后，第二份采样分别位于 705 / 1001 ms：前者合成线程在 CFRunLoop 等待消息，后者在 `MetalContext::copy_image_to_texture` 中调用 `newBufferWithBytesNoCopy` 导入原生资源。该结果不支持把 400–850 ms 的整个空档归因于 GPU 排队。栈采样有侵入性，不能替代无采样时的帧间隔对照。

GPU 1.163 / Metal 1.40 候选将同一作业内的小型索引快照按 16 字节紧凑排布，只对总分配按页对齐；此前每个 draw 各占至少 4 KiB，16 个小 draw 合批仍需 64 KiB。预准入、Metal 批计划和内核分配使用同一布局函数，保留原索引范围、物理别名和逐索引校验，快照仍在实际完成及地址空间退休后复用。该候选的动画收益待真机对照。

1.163 / 1.40 已通过 gpu-submit 主机检查、kext / Metal 构建，以及真机 indexed-test。`hello-card-07` 的 RAM 载荷 SHA-256 为 `01448da7e5ffa1d9450db8773b3937d7c010f9dd6f0618ecae3165928965f5c1`。复核端点图片后，07 的终点是语言页加选项菜单，与 05 的普通语言列表不同，排除其动画帧间隔对比。该记录无 Function 编译事件或目标窗口 UI 作业失败；1100–2000 ms 窗口内，51 个 indexed 作业的快照中位值为 4 KiB、获取耗时中位值为 0.008 ms，05 对应为 20 KiB / 0.921 ms；两组工作量不同，不把准备耗时及显示完成差当成动画加速比。元数据保存在 `hello-card/index-pack-comparison.json`，已更正场景限制。当前保留紧凑索引分配，继续定位转场。

中间版本数据、逐窗口比较和清理清单位于 `build/setup-animation/hello-card/`。已清理本轮用完的打包中间件及被替代候选约 2656.7 MiB，保留原始诊断、源代码快照、哈希与当前候选。冷缓存样本另列，不与热缓存样本混用。

Metal 1.41 的同队列 fence 合批已在 `hello-card-08` 验证。仅把批内先出现的 update，或已准入本队列的 update，视为可继续提交的依赖；所有批内点在成功准入后绑定同一个真实结束 token。跨队列等待、显式 barrier/event、索引 CPU 快照约束和内核缓存刷新保留。16 段带 fence 的 GPU 依赖计算实际只产生一个原生作业，全部结果正确；两队列/共享串行 target/未来事件等待/回调顺序检查也通过。

08 载荷 SHA-256 为 `6ccb6230e65e6570007a6b50d7ac4d5f884636b578f936e0031f695cb4464102`，GPU 仍为 1.163。首次正确场景包含四次 Function 初始化，共约 1.31 秒，另列冷样本。第二次确认 Hello 起点，语言列表终点与已确认冷样本 PNG 哈希一致，且前两秒没有 Function 事件。400–2000 ms 有 8 次显示完成，主要时刻为 423、457、1023、1407、1640、1773、1873、1940 ms；仍有 567 / 383 ms 的起步停顿，不能认定动画已修复。准备区间内仍有许多 3–6 操作的小批，下一步用现有 QueueRun 记录统计拆批原因。详细记录为 `hello-card/fence-batch-08-{metrics,hot-metrics}.json`。

1.42 仅在已有 QueueRun 记录中添加拆批原因计数。09 的观测窗口内：49 次批提交尝试、20 次索引边界、6 次其他边界、0 次资源/字节容量边界；最多使用 11/64 个资源，16 次满 16 操作批，未跨越 fence。该轮仍有两个 Function 初始化，不能作热帧间隔对照。边界证据保存在 `hello-card/batch-boundaries-09.json`。因此 1.43 已撤回不作用于目标动画的 1.41 fence 合批逻辑，相应扩展测试也恢复原样，避免为本任务保留无收益的同步分支。

1.43 候选允许私有、非导入、无 CPU 外部存储的清屏根分配，与独立的 no-copy / IOSurface 索引存储合批。私有根的 `contents` 不对客户端暴露，同根 ID 仍拆开；共享/导入清屏、真正 GPU 索引生产者和内核物理别名检查保留。1.164 同时根据原厂 WorkQueue `+104` 的实际赋值，将 0/5、4、1、2 分为正确优先级，同等级稳定顺序和同队列 FIFO 保留。尚未接入原厂 WorkQueue/Scheduler 运行时。两项候选均待 10 的真机结果。

10 已从 RAM 启动，Metal 1.43、GPU 1.164 身份确认。新增的 private-clear/client-indices 检查返回 1：实际仍有 2 笔 native 作业，期望 1 笔；其余原有 mixed 阶段通过。未采集该版动画，不能计为合批生效或性能改善。定位到 retained command buffer 的 `copyLiveResourcesWithValidity:` 有意返回空数组（无需额外借用快照），但索引查找错误地仅遍历该数组，导致找不到已持有的索引资源。Metal 1.44 改为 retained 读取原操作资源、unretained 读取借用快照；生命周期和内核别名校验保持原样。11 复用同一 1.164 内核，待真机验证。

11（载荷 SHA-256 `d107111ddb648d3f6dce297728c1a798c5044f4e467bbd161f389b5c0e320c70`）已验证 private-clear/client-indices 只提交一笔真实 native 作业，包含 compute/render 两类操作。独立 GPU 回读的 4096 个非均匀像素和保护区正确；原有 GPU 索引生产者仍拆为两笔并保持结果正确。这是合批功能验证，动画采集尚未完成，凭据为 `hello-card/private-clear-index-11-validation.json`。

11 的无截图干扰转场窗口没有 Function 事件和失败作业，400–2000 ms 内显示完成为 10 次，起步间隔仍约 500 / 300 ms；不能称已修复。0–3 秒 QueueRun 中索引拆批为 0，134 次批尝试中 85 次达到 16 操作上限。用户随后人工确认真机画面正常、仅刚唤醒轻微卡顿，但明确反馈上移转场仍卡。两次逻辑截图缺字不能据此认定屏幕渲染回归；单独位置截图有 GPU 渲染干扰，不用于性能比较。10 的单点对照逻辑图有 Hello，也不能替代真机证据。

1.165 / 1.45 候选把 Native 批操作上限接到已有 64 命令预算，仍受原有 64 KiB 共享命令区和 64 资源限制，不增大在途作业数。Batch 协议版本升到 6，旧内核/客户端版本不匹配时保持单操作能力回退。绘制数量实际由打包字节数限制；满批 compute 可容纳 64 段。现有 gpu-submit 检查已通过，包括计算满批及绘制字节容量边界；真机只扩充原 mixed 的资源复用循环至满批，再验证目标转场。没有为此接入新的调度状态机。


12（GPU 1.165 / Metal 1.45）已通过真机 mixed：64 段计算一笔 native 作业，private-clear/client-indices 一笔，GPU 索引生产者两笔，结果和保护区正确。热缓存转场无 Function 事件、无失败作业；400–2000 ms 有 14 次驱动显示完成，较 11 的 10 次有所增加，但仍有约 367 ms 的间隔，不能当作流畅或光学帧率。详细数据在 `hello-card/hello-card-12-hot-transition-metrics.json`。

用户明确要求参考 A10 直接继承 IOGPU 调度框架，已停止仅调整本地优先级的方向。候选 13 接入真实 IOGPUScheduler / WorkQueue / CommandDescriptor，细节见调度复用文档。载荷 SHA-256 为 `375e0122f2e0f94bef733dec1a29d61388147bd8889121becec8337f8005094f`，待目标场景结果；打包大文件已清理约 448 MiB，保留当前和回退载荷。

13 的 mixed 全部通过，但预热中 ordinal 736 尚未提交，737 已硬件完成却因全局连续退休等待 736，约 5 秒后进入恢复。后续窗口 UI GPU 作业为零，显示完成增多不代表动画更快，13 整轮性能比较无效。原厂完成处理只有在资源进度字节置位或全部在途队列空闲时唤醒 prepare；A540 自有资源退休漏接了这个标记。1.167 接入 0xfffffff005f358d0 后再调用既有完成通知，候选 14 验证中。原始末尾作业在 `hello-card/tail-diagnosis-13.log`，ABI/分支证据在 `hello-card/native-integration/native-prepare-wakeup.json`。

14（GPU 1.167 / Metal 1.46，SHA-256 `98e8f87c45364d5d3b4c0c2ccf03afeb89692a87c91534f0ddd079d11cf955ae`）无 Function 事件、零 GPU 失败、零恢复。400–2000 ms 仅 8 次驱动显示完成，仍有约 500 ms 起步间隔。1100–2000 ms：36 个 UI 作业，CPU 准备合计 223.15 ms、GPU 区间 95.77 ms，观测完成后等待退休合计 84.4 ms（最长 40.15 ms）；22 个背景作业 GPU 区间合计 550.03 ms。这些重叠区间不能直接相加为帧时长。

1.168 按队列接入独立 native completion channel，保留同队列 FIFO，实际完成的独立 UI 作业不再被别的队列的未提交作业挡住。候选 15 SHA-256 `d462faf285f9229e78499bcc4af6c474c9604cb9d41222876c0e11deaf82153a`；只补两队列 fence 检查并继续同一转场。当前仍未证明动画已解决。

### hello-card-15 / GPU 1.168.0：原版提交通知与每队列完成通道

真机两队列 fence 测试通过，转场捕获无 GPU 失败、无恢复；不是 hello-card-13 那种无 GPU 工作的无效样本。每个 IOGPUWorkQueue 分配自己的原版 EventMachine 通道，去掉全局 submitted 前缀，恢复调用原版 IOGPUCommandDescriptor::didSubmit()。硬件完成按实际已结束且资源依赖允许的任务逐个退役，同一队列保持 FIFO。

1100–2000ms 前台 43 个任务、583 个操作，CPU 准备合计 225.87ms，GPU 合计 127.34ms；GPU 观察到完成后的退役等待最大 2.04ms（14 为 40.15ms）。原版继承接入的跨队列完成阻塞已消除。400–2000ms 显示完成 10 次，仍不代表光学动画帧率；开始阶段依然有 483ms 间隔，目标仍未完成。命令容量造成 39 次批次边界；下一版只扩展上传到 256KiB，使原有 64 条 Draw 预算能够真正装入。

### hello-card-16 / GPU 1.169、Metal 1.47：实际 64 Draw 容量

上传容量 64→256KiB，现有 64 条命令上限不变，批协议升至 7。原 gpu-submit 检查通过，真机目标转场无失败/恢复，容量边界 39→0。首次捕获有 335ms Function 创建，不能与无 Function 的 15 直接比较。一次热缓存复采无 Function，400–2000ms 显示完成 19 次，最大间隔约 267ms；这些含背景更新，不是光学动画帧率。1100–2000ms 前台 28 任务 / 646 操作，准备 217.90ms（其中编码 123.47ms，页表 54.50ms），GPU 104.92ms；后台 20 任务 / 50 操作，GPU 500.36ms。

端点：导出的起始逻辑截图只有背景（无法据此独立确认 Hello 叠层），终点是普通语言列表，没有选项菜单。用户随后人工确认“有改善，但仍明显卡顿”，因此目标继续保持未完成。16 EFI SHA-256 `bd342a98419bb8a578875f855ea32cf29b6c1203808e23d2c78fb6b90c98ca28`。

下一候选只改 IB 的 CPU 内存属性：PM4/常量/ISA 在 CPU copyback 内存生成，提交所有 IB 到硬件 ring 前经原 IOMemoryDescriptor 缓存操作发布到 PoC；ring、fence、GPU 写回 scratch 保持 Normal-NC。避免每条编码指令直接写非缓存内存。必须经真实 GPU 回读及目标转场确认，不能用主机 PM4 测试代替缓存可见性验证。

17（GPU 1.170）真实 mixed 回读和私有页表启动检查通过，零失败/恢复。但同类 54-op UI 作业编码仍为约 11.2–11.7ms；1100–3000ms 编码每操作 171.49µs，16 为 175.04µs，工作内容并非逐项相同，未得到有意义的编码收益。17 中另有 221/135ms Function 创建，不能用它的显示完成数作干净性能对照。CPU-cacheable IB 实验已撤回，未保留额外缓存模式。

18（GPU 1.171）以 16 为基础，只复用一批命令内已持有的 Family resource ID 解析结果，并对同一个索引 backing 的 CPU 可见性只做一次 synchronize。每 draw 的访问范围、GPU 写入依赖、物理别名和真实索引值校验仍保留。整批 CPU snapshot 在同一个 GPU gate 内，不会在两次 index read 之间插入新 GPU 提交；票据继续持有所有资源到真实退役。mixed 真机回读通过，目标场景零 GPU 失败/恢复。但同类 54-op 编码仍约 10.8–11.6ms，未见足够下降。转场另有 542.349ms Function，其中后端编译 507.793ms、AIR lowering 5.222ms、注册 1.527ms、编译锁等待约 0.001ms。故不能把这次长停顿归到原版调度锁，也不能用显示完成数与热缓存场景直接比较。

19（GPU 1.172 / Metal 1.47）仅补充 attachments / indices / bindings / physical aliases / PM4 emission 五段 CPU 计时，嵌套于现有 encoding 区间。只采目标转场；不重复 mixed 套件。

19 实测 54-op 编码 10.998–11.531ms，其中 indices 3.35–3.39ms、物理别名 2.96–3.35ms、PM4 emission 3.79–3.88ms。零失败/恢复。此轮没有预热，存在 220/518/294/253ms Function，故仅用于拆分 CPU 准备成本。20（GPU 1.173）针对前两部分共有的 pinned physical page 交集运算：先排除首尾物理地址不相交，再使用指数查找跨过不可能匹配的区间；每个匹配页的字节/跨层范围检查不变。

缓存路径只读核查确认：compositor 的临时函数缓存位于 `/private/var/mobile/tmp/com.apple.backboardd/org.darwin.QcomMTL/functions`（19 时 29 文件）；Mercury 扩展的 Library/Caches 有 117 文件，公共 mobile Caches 有 15 文件。此前只检查 tmp 根目录不足以发现进程子目录，不能据此声称缓存未落盘。跨启动临时目录持久性仍未证明，当前先修热转场的 CPU 扫描。

20（GPU 1.173 / Metal 1.47，EFI SHA-256 `a3f9e01b6176e8493b5e662e0c24d43431ce5f8738bd378a09fd1f7ea20ab418`）已完成目标热转场采集，无 Function 事件、零 GPU 失败/恢复。同类 54-op 作业 CPU 编码降到 5.15–6.09ms，通常约 5.5ms；indices 为 0.31–0.49ms，physical aliases 为 0.12–0.24ms，PM4 emission 仍为 3.77–4.05ms，页表准备约 2.7–2.83ms。现有 gpu-submit 检查通过，包括稀疏物理页及字节边界。用户尚未人工观测该版，已重启并要求优先接入原生缓存，因此这些 CPU 数据不作为流畅度结论。

21 保留 20 的内核和全部页范围校验，仅更新 Metal 1.48 的自动 shader 存储为原生 CoreFSCache。QMTLARC1、完整身份/摘要校验及内核 JIT 注册保留；候选及验证见 [缓存接入记录](ios18-shader-cache-reuse.md)。

23 的 Metal 1.48 CoreFSCache 已验证同启动跨进程的实际机器码命中；Apple 系统进程跨重启仍按原库 boot-session 策略冷编译。用户人工确认预热后的 Hello 上移转场“没有明显改善”。缓存接入与 CPU 编码下降均不能作为动画已解决的结论。

下一候选 GPU 1.174 把已固定资源的完整物理页/保留页表核对移到 `IODMACommand::prepare` 建立映射后、发布 buffer 前。私有作业持有该 buffer 直至完成与 ASID 退休，DMA complete/unmap 仅在其最终释放时发生；热提交因此可直接复用已验证且仍 wired 的物理索引。每个私有 PTE 的地址、权限、重复映射检查及硬件健康/TLB/缓存同步不变。待目标转场测量，尚无性能结论。

1.174 的现有 gpu-submit 检查、arm64 kext 构建与内核集合校验已通过；24 EFI SHA-256 `f9ee9fc34b236dd5040af18fec7f45b4df705aaab5754cb9e0d3802a37b96142`。准备启动时，23 的原生 orderly reboot 在 launchd shutdown 中触发 `initproc exited` panic 并进入 UEFI；此时 1.174 尚未运行，原始重启记录保留。


### hello-card-25 / GPU 1.175、Metal 1.49：离线减少重复哈希与清屏标量写入

用户说明设备电量耗尽，要求先做离线优化；本轮停止 UART、fastboot 和真机操作。最后有效人工反馈仍是 23 预热后“没有明显改善”。1.174 的映射核对前移保留在本候选中，尚无真机性能数据。

复查热转场 20 的编码路径发现：启用逐操作统计时，每个 draw 都对完整 vertex/fragment 元数据、ISA 和 immediates 做一次串行 FNV 哈希，该工作也落在 `drawEmitTicks` 内。因此之前约 3.8–4.0ms 的“PM4 emission”不能全部归因于写命令。1.175 只在单次同步编码内复用相同不可变 Program 对象的统计描述；键为实际对象地址，MSAA 等同 ID 的不同对象不会混用。GPU gate 保证该过程内对象存活且不被修改；描述借用本次固定大小 operation records，批次结束后索引即失效。原哈希算法和逐操作记录不变，不增加跨作业缓存或新调度机制。

另一处已确认的渲染编译缺失：`blit_fill_rows_2d` / `blit_fill_pairs_2d` 的源码是 `uint4`，但内存边界 pass 将其拆为四个独立分支与四条 `STGB.u32.1`。现在完整 16-byte 范围有效时保留向量写入；反汇编确认一条 `STGB.u32.4` 后直接跳过标量回退。仅匹配 compute 的四分量、全写掩码、word 对齐、非 volatile/coherent SSBO store；部分范围仍逐分量检查，shared/fragment/half/稀疏写掩码沿用原路径。两个清屏程序的 full/half 寄存器数仍为 6/4；保留回退使静态 ISA 从 288 增至 320 dword，不能据指令宽度推断 GPU 加速倍数。尾部清屏 kernel 未改。

离线检查仅围绕这些改动：现有 gpu-submit 通过；生产 NIR 边界 pass 的 336 个新增向量写入组合及原有内存边界检查通过，覆盖完整/部分/空 binding、uint32 尾部、稀疏写掩码和必须保持标量的访问。144 个 AOT 条目重建后仅 11 个 compute 程序变化，全部图形 shader 和 tail 清屏保持一致；主要输出、反汇编和差异记录在 `hello-card/vector-clear-report.json`、`vector-clear-guards.log` 与 `gpu-175-final-test.log`。AOT registry 更新为 `5929f35a948802cd`，内核及 Metal 必须配套。

Metal 1.49 保留 CoreFSCache、QMTLARC1 校验和内核 JIT 注册；实际编译器及 registry 进入原有 archive build identity，旧机器码缓存自然失效，失败仍正常编译。本候选的离线通过不代表 Hello 动画已解决，仍需有电后只验证目标转场。

25 已打包并完成内核/HFS 逐文件回读校验，EFI SHA-256 `8d6c4b19fdafc886937b2533caa489bb4e7c93a4614cbc2ad52e72984982a65a`。载荷与待执行采集入口位于 `build/setup-animation/hello-card-25/bridge/XnuBridge.efi`、`hello-card/boot-capture-25.py`，均未启动真机。被替代的 24 载荷及 25 打包中间文件已清理，独立文件约 588.0 MiB（含共享硬链接的逻辑总量约 656.5 MiB）；保留 25 候选与既有验证回退。清单见 `hello-card/cleanup.json`。


### hello-card-26 / Metal 1.50：其余重复哈希检查

用户要求继续检查重复哈希。本轮只改 Metal 层，GPU 保持 1.175，复用 25 已校验的内核；设备仍没电，未执行真机操作。项目自有 GPU、Metal 与编译器调用检查记录在 `hello-card/hash-audit-150.json`，差异在 `hello-card/metal-150.diff`。

1. 内置库当前为 63,606 bytes，之前每个 library wrapper 与 AOT function 构造都会重新做 SHA-256。现在由 `dispatch_once` 保存进程内不可变内容的 identity。
2. `QcomMTLFunction` 构造之前会对 JIT 元数据、ISA、immediates 和 MSAA 变体计算临时 key，调用方紧接着用已派生的最终 key 覆盖它。五个构造调用点现在直接传入最终 key；内部 depth-only AOT 的 key 仍按原算法派生。
3. `loadLibrary` 在复制外部 bytes 后已有完整 SHA-256。现在把该摘要随同一个不可变快照传给 MTLB library 初始化或 raw AIR 编译，诊断 origin 也复用它。直接 MSL 输入仍先取得独立快照再哈希。新文件的完整摘要、每个 AIR payload 校验和 QMTLARC1 完整文件校验均保留。
4. compute/render pipeline 没有 binary archives 时不再构造并哈希 archive key；仍先校验 archive 参数，空列表的 FailOnBinaryArchiveMiss 仍失败，真正查找和写档时照常派生 key。`addFunctions` 因此也避免了先创建 pipeline 再写档时的重复哈希。

核对了现有 CoreAnimation、Mercury、转场 metallib，分别有 178/22/9 个函数，其 AIR spans 全部互不重复，因此没有为它们添加 span 缓存。剩余 specialization 常量键必须反映可能改变的输入，semantic 名称仅在编译时哈希短字符串；shader capture/compute trace 是显式启用的诊断路径，后者仅限少量启动命令，均未增加持久缓存。1.175 已处理的内核每批统计描述复用保留。

本轮只执行 arm64 Metal 构建及载荷校验，不重复 GPU/编译器全套测试。哈希算法与 key 的组成规则不变；版本/构建 context 按原策略变化，所以不宣称跨版本 key 相同。收益目前仅能确认少做了重复计算，尚无真机耗时或动画改善数据。

26 打包完成，EFI SHA-256 `1dd9bd4a2be1de12aefe76b36000a24c2da0a70e675db057613ddfb07984555b`，HFS 逐文件回读校验通过，GPU 内核及 AOT registry 与 25 字节一致。25 候选载荷已被 26 替代并清理，本轮连同打包中间文件清理独立文件约 554.2 MiB；源快照、差异和构建记录保留。


### hello-card-27 / GPU 1.176：合并相邻 PM4 寄存器写包

继续离线处理 Hello 转场的 CPU 编码路径，Metal 保持 1.50，保留 1.174/1.175 的映射、统计描述复用和向量清屏改动。设备仍按没电状态处理，本轮没有 UART、fastboot 或真机操作。

现有状态编码大多以一个 Type4 包写一个寄存器，即每个值重复写入包头并检查命令空间。A5xx 本来支持一个 Type4 包按顺序写连续 DWORD 寄存器；项目固定版本 Mesa 的 `freedreno_pm4.h::pm4_pkt4_hdr` 和现有 AdrenoStartup 固件装载均采用该格式。1.176 为 Encoder 添加有界 `regs`，只把原来已经相邻、地址也连续的写入显式合并：restore 的零值范围、shader object 地址与 scratch 状态、VS 输出/VPC destination、FS output、VFD control、viewport/scissor、depth/LRZ 禁用值以及 MRT base/flag。不跨越其他寄存器写入或 Type7 指令，不改变缓存刷新、等待、页表切换和绘制次序，也不增加自动合并缓存。

离线差分使用更改前后的真实 PipelineEncoder，覆盖 direct/indirect shader 加载下的两次 compute、两次普通 draw、两次 depth/stencil/MRT draw，共六组。将 Type4 展开成有序寄存器写入后，逐条比较全部 Type7 包头/内容和私有尾部上传数据及偏移，结果完全一致。普通 draw 两次的寄存器包头由 720 降为 514，即每次少 103 个（约 28.6%）；direct shader 样例的命令总长度由 2546 降为 2340 DWORD（约 8.1%），compute 两次少 34 DWORD。这里只是命令数量差异，不代表相同幅度的 CPU 时间或动画帧率改善。报告：`hello-card/pm4-176-equivalence.json`，可复现脚本与源文件：`pm4-176-compare.py` / `pm4-176-capture.cpp`。

本轮仅运行现有 gpu-submit 及上述差分检查，新增测试覆盖 Type4 parity/计数、空间不足时不写入、保留尾部和 checkpoint 回退。gpu-submit 与 ARM64 QcomGPU 构建均通过；没有重跑 Metal 或 shader 编译器套件。当前仍缺有电后的目标 Hello 转场耗时与人工观感，不能声称卡顿已经解决。

27 已打包完成，EFI SHA-256 `d12a53fcf8b6e2d7494c7b0336b3d3ca3ffaa971112507c13dfcab71c4e30450`，内核集合和 HFS 逐文件回读校验通过，HFS 内容与 26 完全相同。载荷位于 `build/setup-animation/hello-card-27/bridge/XnuBridge.efi`；采集入口 `hello-card/boot-capture-27.py` 已准备但未执行。被替代的 26 载荷、27 临时集合及打包中间文件已清理，释放独立文件约 589.7 MiB；保留 27 候选和 23 已验证回退。结果与清理记录见 `hello-card/offline-176-result.json`、`hello-card/cleanup.json`。

### hello-card-28：离线改动的真机验证与热路径复查

用户接回设备后，通过 CH340 `/dev/ttyUSB0` 恢复无人值守验证，只用 HOME 唤醒。27 首次启动被 AMFI 拒绝 launchd 使用更新后的 SystemOS shared cache；同日系统 UI 补丁任务已替换该文件，其新的 trust cache 尚未进入 27。28 只追加该任务已验证的 `build/system-ui-patches-20261021/prepared/trust-cache.bin`，没有重写系统分区，kernel/HFS 与 27 完全相同。旧启动日志的 os.dmg ELOOP 也出现于成功启动，不将其当作本次失败原因。

28 EFI SHA-256：`e696416874d708539313b1138181d3367273019ed5d453b94a221f11b424f650`。GPU 1.176 / Metal 1.50 加载成功，两次目标转场均没有 GPU 作业失败或恢复。用户反馈为“有改善，但仍明显卡顿”，目标尚未完成。

与 20 的相同 54 操作合成作业比较，CPU 编码中位值由约 5.50 ms 降到 3.74 ms、地址空间由 2.76 ms 降到 0.86 ms、draw emit 由 3.83 ms 降到 2.12 ms。28 该形状只有两份样本，跨启动且系统 UI 模板已更新，不能视为隔离 A/B 或动画加速比。背景 Fluidity 主片元仍约 28.34 ms；两个全屏向量清屏约 3.77 / 8.92 ms。

首次目标采集仍包含一次 Function 200.13 ms（identity `234461aeb739725c`，AIRLower 约 170.57 ms、Backend 约 24.36 ms）；第二次同一启动下 `clean-transition` 没有 Function 事件，但有约 600 ms 的驱动显示完成空档，随后仍多次出现 100–200 ms 间隔。显示完成包含背景更新，不是卡片光学 FPS。无编译样本中，空档大部分发生在新 compositor commit 之前，不能全部归因于 GPU 排队。64 命令拆批上限命中较多，但尚未修改。

只补采一轮两个线程栈，另列侵入性诊断，不与无采样时长直接比较。HOME 后约 702 ms 的合成线程等待消息，约 1036 ms 的栈处于 CoreAnimation backdrop GaussianBlur 创建纹理：`newResource → IODMACommand::prepare → QcomGPUMapper::iovmMapMemory → RetainedPageTables::map`。与运行中 Kext UUID 匹配后，map 的返回 PC 位于 `page()` 调用之后，指向逐级保留页的重复线性查找；单点栈不能证明整个区间都耗在该函数。

原始记录：`hello-card-28-hot-transition-data/`、`hello-card-28-clean-transition-data/`、`hello-card-28-stack-data/`；汇总与符号解析：`hello-card/runtime-comparison-28.json`、`hello-card/hello-card-28-clean-transition-metrics.json`、`hello-card/stack-28-decoded.json`、`hello-card/stack-28-gpu-kernel.json`。28 已用完的桥接中间文件清理约 445.3 MiB，EFI 与可复现输入保留。

### hello-card-29 / GPU 1.177：保留页表查找复用

仅给 RetainedPageTables 的逐级 walk 增加四个最近命中的保留页索引。每次仍读取实际父 PTE，并核对索引指向的物理页；不匹配时走原有查找。权限、环路、真实叶项、DMA 生命周期和 TLB 同步均保持。索引不缓存 PA 翻译或旧描述符；destroy/re-adopt 后由 count 和物理地址检查排除失效索引。继续复用原生 IOGPUFamily、IOMemoryDescriptor、IODMACommand，仅优化 A540 硬件页表适配。

现有 gpu-submit 检查通过（包括 39/40 位树、跨两个叶表、父项替换/环路、叶项变化与销毁重建）。为隔离并行任务正在修改的冷编译器，本候选只重编 GPU mapper 和版本单元：原对象重新链接后除 LC_UUID 外与 1.176 字节一致，之后替换 mapper；Metal 1.50、HFS 和 AOT registry `5929f35a948802cd` 保持原样。ARM64 构建与内核集合校验通过。EFI SHA-256：`1cbb11b51f2c8e9d77394963900c8348678001bf8dc035b5dd410afb5a2cf5bc`。尚不声明动画改善。

用户授权与任务“优化 iOS 18 GPU 冷编译耗时”协调，已发送 200 ms 冷事件证据、冻结版本和共享目录边界。该任务继续离线编译器优化；本任务统一操作真机，待其交付一致的编译器、Metal、AOT 和缓存 context 后再集成。

29 已成功完成预热及目标转场采集：160 个目标窗口作业没有失败，没有 Function 事件。用户反馈“有进一步改善，但仍卡”。相同 Metal/HFS 下，HOME 后 400–2000 ms 的驱动显示完成由 28 无编译样本的 10 次变为 18 次，最大起步间隔由约 600 ms 变为约 467 ms；仍有约 217 ms 的后续间隔，后段多次 50–150 ms。工作形状和采样时序不同，不将这些数量当作光学帧率或精确因果加速比。背景两次清屏约 12.75 ms、三次 draw 约 36.45 ms，基本不变；背景索引快照仍约 3.86 ms。证据：`hello-card/comparison-29.json` 和 `hello-card/hello-card-29-hot-transition-metrics.json`。已清理 29 的桥接、临时集合及重复构建文件约 453.5 MiB；另保留 4.09 MiB 经字节等价核验的 1.176 对象输入，避免共享构建目录被冷编译任务更新后无法复现增量候选。

### 原版 A10 附件清屏分析（静态，无新候选）

按用户要求核对 iOS 18.7.10 AGXMetalA10：descriptor 的 load/store/clear 值进入 framebuffer config，再配置 BGO/EOT 程序并绑定到 render command。当前 QcomMTL 把 clear 展开为 compute fill，A540 渲染仍是 BYPASS，没有对应的 GMEM pass 生命周期。参考 Freedreno 找到独立于普通三角形绘制的硬件 FAST_CLEAR 路径；最小后续候选是验证实际两种全屏 rows/pairs fill，完整 GMEM 还必须处理 Fluidity 的附件输入。没有因此改动驱动或重启真机，不能宣称新的性能提升。详见 [iOS 18 原版附件清屏分析](ios18-native-attachment-clear.md)，反编译/调用点证据保存在 `hello-card/native-clear/`。冷编译任务的 Metal 1.51 冻结交付已收到，尚未部署。
