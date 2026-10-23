# Hello 淡入淡出和 HOME 提示的驱动优化优先级

## 当前进度：Display1.0.22 / round168

一次合并采集定位到两项具体成本：冷启动25次函数专用化平均约534ms，后端约392ms、AIR lowering约133ms；嵌套时间不能相加，编译锁等待约1µs。每轮相同Metal二进制均重复编译，持久化专用程序缓存尚未实现。

显示映射复用已经真机命中：显示177→245，命中175→242，创建保持3、淘汰0。准备中位数由3.773ms降至0.084ms，旧映射处理由3.485ms降至0.006ms；原生读访问仍每帧同步、PP_DONE后才退休。ASan/UBSan通过，四核、无运行panic；未改GPU/Metal，沿用166原生fence和CA八帧像素证据。

实显间隔中位数仍33.35ms：稳定帧常在读计数7–9启动，等待约16.5ms到TE，再传输约14.3ms。需要结合Apple原生队列/VBL的启动时机处理，不能靠映射收益宣称60FPS。4.10秒窗口145GPU完成、0失败，GPU约596MHz；最后约0.44秒逻辑熄屏，采集状态1保留，不作完整亮屏FPS验收。运行时CPU频率仍未知，不再重复失败的powerctl查询。

新增后续范围：按壁纸参考任务修复Mercury的原生Metal设备/JIT准入，保留CursiveHello策略和已安装PosterBoard数据仓库补丁。GPU146/Metal1.8已通过原生设备与队列初始化；当前阻塞转为错误默认Metal库和缺失Fluidity shader/双附件格式。GPU147/Metal1.9已合并原生库加载、实际AIR lowering与RG32Float清屏/MRT/fetch，集中真机验收待进行，尚未确认背景出图。详见[完整计时报告](hello-display-timing-review-1.022.md)。

后续背景进度：round174的RG32和memoryless双在途pass像素通过，用户仍见纯黑；round175已通过Apple CARenderer八帧，但真实MetalKit加载maps.tif明确被RGBA16Unorm格式110拒绝。GPU148/Metal1.11补齐原始16位归一化采样，集中验证maps/filmic_lut上传与GPU采样，尚未确认背景已修复。详见 [原生GPU进度](native-iogpu.md)。

## 首次启动卡顿与回退：GPU1.143 分析进度

更新：round163 正确性套件通过，但仍有长 prepare 前空档；round164 已补入 OSM，真机显示 secure 初始化及自动策略均不可用，没有改变 CPU 频率。powerctl 打包权限错误已在单独候选修正为 0755，未再次重启。当前仍未解决首次卡顿或语义帧回退，详见本轮报告及其硬件限制。

round162 已保留首次亮屏历史：743 笔作业成功、264 次显示 token 单调；长间隔有大段发生在下一笔 GPU prepare 前，GPU 实测约 596 MHz。发现 shader 抓取错误地随 Metal 错误诊断自动开启，会在编译锁内重建 AIR／写文件；GPU1.143 / Metal1.5 已将其改为独立显式开关，保留错误上报。当前集合缺少 QcomOSM，源码 CPU 自动策略未随镜像加载，实际 CPU 频率仍未测得。一次合并验证正在进行，先冷启动采集再运行正确性套件；未宣称解决内容回退或达到 60 FPS。详见 [本轮分析](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-cold-start-review-1.143.md)。


## 最新真机：GPU1.141 / Metal1.4，60 FPS 尚未达到

已恢复真机调试并合并当前设备壁纸补丁的配套 trust cache，GPU kernel / HFS / DT / boot args 保持。十阶段 GPU 常量窗口、render-state、跨队列 fence 和 Apple CARenderer 八帧像素检查均通过。12.0876 秒真实系统合成窗口完成 817 笔作业、0 失败，GPU 实测 595.970 MHz；GPU 作业均值 3.470 ms，p95 17.231 ms，最大 19.393 ms。

新 framebuffer 程序 `80bdefd21c667f9f` 实际执行 310 次，平均 1.824 ms，copy bytes 0；当前最大逐操作累计项是普通 fragment `8e20a5b0ebb569fd`（平均 2.887 ms）。CPU prepare→admission 均值 4.640 ms，address-space 2.126 ms／次；显示 Active→Started 中位 3.654 ms、Started→观察 PP_DONE 26.128 ms，需继续分解映射、cache maintenance、staging 与 TE / 完成衔接，保持真实读 fence 退休。

实际显示更新首末速率 25.640 次／秒，间隔中位 33.423 ms，最大 166.889 ms，包含内容停留，不是连续运动光学 FPS。预先指定窗口被 118 条逻辑 on 样本包围，最大采样间隙 106.195 ms；完整 60 秒 holder 后段存在 off，因此返回 1，未记作保亮全程通过。原始 off、长间隔与采集失败均保留。CPU 实际频率仍未测得，旧不同身份 / 场景数据不作隔离变量 A/B。

详见 [GPU1.141 真机分析](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-runtime-review-1.141.md)。以下离线记录描述各自构建时状态，现以本节合并真机证据为准。

## 当前合并启动候选：GPU 1.141 / Metal 1.4 与有效采集窗口（未上机）

合并 EFI 已完成离线构建，含既有 GPU1.141 / Metal1.4 / Display1.0.18 和更新的 displayctl，保留设备身份诊断。新增限时 native display-on / 10 Hz 状态记录命令，用 absolute / continuous Mach 时钟帮助识别熄屏或状态未知区间；采集窗口内不逐样本打印。它不改变显示驱动提交或退休。

13 HFS 文件读回一致，并独立对照 ELF payload symbols，确认实际 kernel / driver-image / trust-cache blob 在 raw Image 与 EFI 中一致。现有 60 Hz 声明不支持“固定 30 Hz 配置”的解释，旧 23.4 ms Started→观察 PP_DONE 不能当成纯 DMA；没有改写显示时钟。

已准备一次合并正确性检查和一个 compositor-activity-gated 12 秒窗口，尚未执行设备 runner。采样逻辑 on、作业增长都不单独证明光学亮屏或 Welcome 持续运动；须保留原始长间隔并检查覆盖。设备可用性问题仍在等待用户回复。详见 [合并候选与采集条件](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-hardware-ready-1.141.md)。

## GPU 1.141 发布复用实现（合并候选内，未上机）

已核对实际展开调用链：进入 framebuffer 展开前及快照复制后都有真实 UCHE flush / 匹配时间戳等待。常量加载现在仅在这条具有本次调用证明的路径上略过重复发布；普通 / 显式 primitive-snapshot raster 仍自行发布，`CP_WAIT_MEM_WRITES`、逐图元反馈和完成 fence 保留。没有跨 draw / operation 缓存发布状态。常见有效窗口双三角形序列少一次全局 flush，模型总数 7→6，尚无 GPU 时间收益。

8,224 个窗口范围案例、新增八种 framebuffer 组合、mixed compute、384 图元跨 IB / profiling / compact 回滚和分配失败，以及完整 submit 模型通过 ASan / UBSan。原生 `constant-window-test` 增加 GPU producer→常量窗口 + framebuffer consumer，普通 / framebuffer 各五阶段，每阶段核对 4,096 像素；工具已构建且同一段 authored AIR 的 base / MSAA ISA、窗口与 framebuffer metadata 已通过主机检查，尚未真机执行。

135 个 AOT、registry `72b191f4da4d7b92`、编译器、Metal adapter 和 archive context 保持逐字节一致，覆盖镜像只更新 metalctl。GPU1.141 / Metal1.4 / Display1.0.18 组件通过 74 kext / 23 本地 bundle 和 13 HFS 文件审计，未操作设备。复杂 Welcome 的亮屏时间线和 60 FPS 仍待验证。详见 [实现与证据](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-constant-publication-reuse-1.141.md)。

## 前一离线候选：GPU 1.140 / Metal 1.4 静态读取边界简化（未上机）

针对只读、非 volatile / coherent 的 fragment SSBO 常量偏移，在编译时用 64 位整数证明分量末端，生成单次 `size >= end`；回绕、末端超出 uint32 可表示长度或奇数 half 地址为 false。保留原逻辑长度、逐分量零填充及完整向量的不可推测 guard。动态 / shared / store / atomic 路径和 GPU 发布 / fence 不变。

真实 Apple 热点 `80bdefd21c667f9f` 的 base / MSAA 指令字数从 864 / 864 降到 672 / 704，half registers 从 26 降到 20，full 6 保持。五行冷回退各删除一条必定无效的末分量读取，静态 LDGB 从 27 降到 22。1,768 个实际 NIR literal / symbolic 差分案例、原 2,700 / 832 边界案例、完整编译器和当前 AOT 的提交 / 窗口 ASan / UBSan 检查通过；74 kext / 23 本地 bundle 和 13 HFS 文件校验通过。

135 个 AOT 中 104 个 ISA / immediates 保持、31 个 fragment 改变，compute / vertex 指令保持；registry 为 `72b191f4da4d7b92`。部分 AOT 有 full / half 分配取舍，不能概括为全部提速。匹配 GPU1.140 / Metal1.4 / Display1.0.18 组件已打包，ABI36，未访问设备或生成完整 EFI。真实像素、净收益和 60 FPS 仍待验证。详见 [实现与证据](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-static-read-bounds-1.4.md)。

## 前一离线候选：GPU 1.139 / Metal 1.3 常量窗口判断合并（未上机）

真实 Apple framebuffer 热点的五次相同窗口判断已合并为一次：在同一基本块内集中同一窗口的独立只读 half 加载，只跨纯 SSA 指令；保留内存、volatile / coherent、不同 buffer、barrier、纹理、输出及控制流边界。原始 AIR / 常量与前端 GLSL / reflection 保持，half 算术和 kernel 发布 / fence 实现不变。

热点 `85db5ac1599a4f7a` 的 base / MSAA 指令字数从 896 / 928 降为 864 / 864，half registers 从 27 降为 26。2,700 个原 half 边界案例、832 个带交错 ALU 的窗口位值案例、调度边界 / SSA 支配检查及完整编译器回归通过；8,224 个生产窗口 ASan / UBSan 案例和 74 kext / 23 本地 bundle、13 HFS 文件校验通过。原 133 个 AOT 和 GPU 生产者诊断指令保持，仅常量消费者诊断 ISA 改变，registry 为 `465b990a90071c98`；匹配 GPU1.139 / Metal1.3 / Display1.0.18 组件已打包，ABI 仍为 36。

没有启动真机、生成完整 EFI 或测得新帧率。加载集中可能改变值的活跃区间，静态代码减少不能代替真实复杂 Welcome 的 GPU 时间验证。前一 Metal1.2 组件保留；后续硬件阶段使用合并候选做一次正确性和性能采集，并关联真实屏幕电源状态。详见 [实现与证据](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-window-guard-coalescing-1.3.md)。

## 前一离线候选：GPU 1.138 / Metal 1.2 有界 GPU 常量窗口（未上机）

已完成生产接入：根据真实 Apple AIR 的静态只读 half 访问，收集至多四个 64 字节常量窗口；由 GPU 在执行时加载，未增加 CPU 数据快照。真实热点使用 buffer 1 的 `[160,224)`，覆盖矩阵 `[176,216)`。内核逐 draw 核对实际范围、source 对齐、地址溢出和写入限制；短或不对齐绑定保留原逐分量边界回退。发布顺序包含真实 cache flush / wait、HLSQ 更新和间接常量加载。ABI36、编译器、AOT、kernel identity、native adapter 一起更新，必须成对使用新内核和覆盖镜像。

原有 2,700 个 half 向量案例、832 个生产窗口 NIR 案例（128 个启用路径没有 SSBO 读取）、8,224 个生产 packet 范围 / 对齐及状态重绑定案例、完整编译器和原生构建通过。74 kext / 23 本地 bundle 校验通过，覆盖镜像 13 个文件独立读回；原 133 个 AOT code / immediates（含 MSAA）逐字节不变，新增两个诊断入口。真实热点 `6c69f37293d6f5a7`，base / MSAA 896 / 928 words，6 full / 27 half registers，constlen 40 vec4。

新的同 command buffer GPU 生产者→常量消费者诊断已编译，尚未执行。该路径增加冷回退代码与一次条件性缓存发布，收益需在真实复杂 Welcome 上计时；当前没有新的像素、同步、帧率或 60 FPS 验收。本轮只完成离线匹配组件，没有操作设备或生成完整 EFI。详见 [实现与证据](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-constant-bank-1.2.md)。

## 前一离线候选：Metal 1.1 framebuffer 常量向量读取（未上机）

按用户要求，本轮只离线优化真实 Apple framebuffer shader。对只读、非 volatile、常量偏移的 fragment half 读取先按原 NIR 别名规则合并，完整范围走 half4 向量，短绑定仍保留逐分量零填充和溢出检查；禁止优化器将向量读取提前到边界检查外。原始 Apple AIR / 常量重放与新产物对上，GLSL / reflection 和两个参考 shader 的完整二进制不变。

热点身份 `6006761a9eb7f18e`，矩阵完整时 20 条 scalar 读取变 5 条 `u16.4`，含 bias / noise 的有效路径 LDGB 为 22→7。静态 ISA 仍有 27 条 LDGB（含 20 条冷尾部回退），读取字节数不变；代码 640→800 words、full registers 16→6、half registers 19→26，不能据此宣称提速。2,700 个生产 NIR 边界 / 位值案例、完整编译器回归、原生 arm64 编译器 / 适配器和 12 个 HFS 文件读回均通过。

已生成 GPU1.137 / Metal1.1 / Display1.0.18 的离线驱动覆盖镜像和 trust cache，保留 133 AOT 程序、registry、内核参考及显示 / fence 实现。未生成完整 EFI，未操作真机或修改根文件系统。工作区已有其他显示 / 壁纸变更已保留；本候选继续使用旧 displayctl / gpuctl。后续只做这一批合并候选的真机验证，并关联屏幕状态。详见 [实现与证据](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-framebuffer-shader-offline-1.1.md)。

## 历史常量缓存可行性研究（已由上方生产实现接续）

隔离编译器原型验证了 A540 的 GPU 间接常量加载方向：热点矩阵可由 `[160,224)` 的 64 字节窗口覆盖，运行时必须验证真实绑定和 source 对齐。520 个 NIR 模型案例中，80 个启用路径没有矩阵 SSBO 读取；包 / 长度 / 对齐的 ASan / UBSan 模型检查及 raw half 位值检查通过。base / MSAA 为 896 / 928 words、6 full / 27 half registers。它缺少生产元数据和 GPU 生产者可见性接入，明确不可部署；Metal1.1 候选保持原样。下一批应完整实现这些接口与依赖，再做真机复杂 UI 验证，不能只部署 shader。见 [可行性与接入条件](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-constant-bank-feasibility.md)。

## 2026-10-19 真机更新（157 正确性 / 158 性能采集完成）

GPU 1.136 的 WB 私有作业页表已通过真机像素、mixed batch 与复杂 UI 验证。round 154 的 12.103 s 窗口完成 619 个 UI 作业，失败 0；GPU 实测约 596 MHz，实显间隔中位数 33.518 ms，首尾约 22.050 次/秒（包含动画停留），尚未达到 60 FPS。CPU 地址空间平均 2.214 ms/作业；热点 framebuffer shader 仍占 GPU 作业时长之和约 66%。详见 `hello-runtime-review-1.136.md`。

此前原生 shader 文件缺失的原因已定位到采集脚本漏查 Foundation 实际目录 `/private/var/mobile/tmp/com.apple.backboardd`。round 156 已导出原生热点及两个参考变体，验证 SHA、base/MSAA 身份；热点 `bc8b834de5755555` 的完整 41,840 字节原生/主机重编译结果逐字节一致。它是单采样变体，不能将此前未匹配的原因归因于 MSAA。诊断开关在 backboardd/SpringBoard 的 registry 和原有设备连接上都为 1。

基于真实 Apple AIR 的单个 framebuffer 值参数语义，Metal 1.0 在 AIR runtime/host 和 MSL 中各只物化一次输入，保留原始 FP16 转换及后续算术。热点从 704 降至 640 words，实际取数路径从三组变一组，但 full register 占用 12→16，需要实测收益。133 个 AOT 程序、backend、registry、GPU 1.137 内核与 Display 1.0.18 均保持逐字节一致。round 157 的四个 framebuffer 像素/边界阶段及原生 CARenderer 八帧像素均通过；同次启动的 round 158 完成 12.097 s 配对采集，586 个 UI 作业完成、失败 0。热点仍占 GPU 作业累计时长约 68.0%，copy_bytes=0；GPU 实测 595.988 MHz，实显间隔中位 33.446 ms。尚未证明此次优化提高帧率，也未达到 60 FPS。

用户指出长间隔可能是熄屏：158 最长 1.300 s 先标记为疑似熄屏，不作为动画卡顿或回退证据。原始首尾 19.028 次/秒包含这段未知屏幕状态，不能当成亮屏运动 FPS，也不通过按阈值删点宣称更高帧率。下一步关联显示电源状态，再按实际 Apple AIR 优化 22 条 scalar buffer load / color-matrix 路径及寄存器占用。当前无 UART reader，不重复已有通过检查或要求主观比较。详见 [本轮分析](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-runtime-review-1.137.md)。

以下保留之前各阶段的记录；旧的“充电离线、未启动”描述属于历史状态，当前以本节和 `ui-render-handoff.json` 为准。


## 离线候选 GPU1.136：可缓存私有页表，设备充电期间未启动

用户要求先离线优化。当前已完成并打包 **GPU1.136／Metal0.96／Display1.0.18**，尚未真机启动。主修改针对 CPU 建表成本：私有作业页表由单一 CPU 可缓存映射构建，所有页在发布前通过原始、已 prepare 的 IOBuffer 描述符执行 `kIOMemoryIncoherentIOStore`，子表先于父表清理到 PoC，再封存 root 和执行原 ASID TLB 同步。原有 retained kernel 页表和数据页属性保持原实现。

核对了 Apple XNU 的非分页 IOBuffer 自动 prepare、IOMemoryDescriptor 缓存维护契约及 ARM64 clean 路径，并对**本候选实际复用的冻结内核**反汇编：`dcache_incoherent_io_store64` 调用真正的 `CleanPoC_DcacheRegion`，后者执行 64 字节步进 `dc cvac` 及前后 `dsb sy`。没有把 Apple 的一致性 GPU 条件当成 SDM835 的硬件保证。[原始来源与二进制证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-cacheable-job-tables/apple-xnu-cache-audit.json)

ASan/UBSan 的非一致性设备模型检查了跨 2 MiB／1 GiB 边界的 514 个只读／XN 叶项、清理前不可见、逐子表至 root 的发布次序、每个清理位置失败时不封存／不释放，以及退休页重用后旧地址不再映射。完整 GPU submit 主机检查、arm64 驱动／ABI 和 74 kext／23 本地 bundle 校验通过；沿用 Metal0.96 的 12 个 HFS 文件及既有 GPU／显示策略。实现和校验见 [审计](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-cacheable-job-tables/audit.json) 与 [补丁](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-cacheable-job-tables/changes.patch)。**尚无真机建表耗时或帧率收益，未达到 60 FPS 验收。**

Metal0.95 的 round151 已通过四 CPU 和像素检查，但没有导出 shader 文件，不能记为导出成功。Metal0.96 补齐直接 AIR／MSL 与延迟 metallib 编译入口、冷编译尝试及写入结果日志，并放宽有界文件长度；仍未上机。下一次恢复设备后直接使用合并候选，先验证真正的冷编译导出，再按 SHA／FNV 对接实际热点和完整编译重放，避免继续凭同名示例 shader 推测。充电期间没有继续操作 UART／Fastboot。

## round150：95% 操作计时覆盖确认主要 framebuffer shader 热点

有效性能依据仍是 **GPU1.135／Metal0.94／Display1.0.18** 的真实 Welcome 窗口：12.114 秒内 590 笔 GPU 作业完成、0 失败。选中窗口 GPU 作业区间合计 4726.166 ms，逐操作配对覆盖 4497.575 ms，约 **95.2%**；它是累计执行区间覆盖率，不是 GPU 墙钟利用率。4188 个操作无 bucket 溢出，主要热点桶没有未采样项。

`bc8b834de5755555`（`fixed_frag_lph_cpf`，704 code words、12 full／21 half registers、framebuffer mask 1）251 次共 **3126.413 ms**，约占该窗口 GPU 作业区间合计的 **66%**。常见操作为两个三角形，已经命中原位免拷贝和同 draw 状态复用，复制字节为 0；普通采样 shader `48fd06ac66534652` 另占 713.695 ms。主要 shader 的确切 GLSL／函数常量仍缺少成功导出，尚不能精确分为 ALU、纹理、带宽或排空成本。

该窗口显示间隔中位 **33.485 ms**、p95 **83.385 ms**、最大 **300.271 ms**，7 个超过 100 ms。首末帧区间约 20.715 次／秒，包含正常停留，不能当成纯运动动画帧率。累计 largest 列表里的约 80 ms 操作发生在所选窗口之前；窗口内 GPU 作业最大 62.663 ms。当前 GPU 约 596 MHz，CPU 有效频率仍未知。[完整真机证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-operation-coverage/hardware-evidence.json)

## round149：具体 shader 与显示启动时间已分开，继续补齐重操作的采样

真机当前运行 **GPU1.134／Metal0.94／Display1.0.18、fetch2／pipe6**。本轮是诊断构建：`gpu-op-timing=1` 在已有私有进度页128字节起放置每操作一对 CP 19.2MHz 时间戳，记录可信编译产物身份、几何范围、实际 raster 调用及 flush 次数；Display1.0.18 仅增加成功 CTL_START 后的 Started=9 采样点。没有修改原生 ready、GPU 一致性发布、`PP_DONE + latch + idle` 后退休显示读 fence 的条件。重新核对了项目固定的 Apple 完成清理、IOMFB 层所有权／准备交换反编译，以及 DPU tearcheck 原实现。Apple 框架证据不等于 A540 缓存／GMEM 语义的证明。

精确载荷经 Fastboot stage／CRC 启动并验证四核；1040×67 的四阶段像素／padding／guard 和实际混批检查均返回0。随后冻结的 **12.100799 秒**窗口提交532、完成534、失败0，开始已有2笔在途工作。GPU实测 **595.988 MHz**，CPU有效频率仍未核实。534笔作业 GPU 区间中位2.247ms、p95 53.452ms、最大62.199ms；这一次内容／诊断窗口不能与上一轮178ms上限作严格A/B改善判断。

显示238条窗口记录形成237个间隔，中位 **33.467ms**、p95 83.449ms、6个超过100ms、最大1000.200ms，首末显示跨12.0519秒、含正常停留约19.665FPS。尚无逐动画活动标签，不能把1秒停留直接当作GPU执行或卡顿；**没有达到约60FPS，也没有本轮新的光学无闪烁验收**。

| 同窗口诊断 | 已确认的开销 | 解释与限制 |
| --- | --- | --- |
| 34组完整 Active→Started→CompleteBefore | 显示准备至CTL_START中位3.761ms；启动至观察到PP_DONE中位25.505ms | 后半段含TE、实际传输及中断工作循环处理；不是纯DMA硬件耗时 |
| 33组 recorded ready→Active | 中位0.0075ms，最大0.0099ms | 已记录就绪后没有额外等待整轮VBL；仍不等于真实fence首次就绪的硬件时间 |
| `fixed_frag_lph_cpf`，身份 `48fd06ac66534652` | 238次已采样操作共715.845ms；无framebuffer fetch | 普通纹理合成也占明显GPU时间，不能只优化fetch |
| 同名fragment，身份 `bc8b834de5755555` | 167次已采样共514.878ms，另71次未采样；334次raster调用、1002次flush | framebuffer-fetch变体；已保留样例中，两三角形、约960×1056 scissor 的一次操作耗25.846ms |

3915个私有页可诊断操作没有bucket溢出、无效计时或失败作业，但已采样GPU区间只覆盖约1840.193ms，作业区间总和为4765.410ms。当前实现只尝试现有IB；未加时间戳的操作本身需要换IB时，也没有在新IB重新采样，因此可能漏掉重操作。另外本窗口366笔慢操作覆盖了128笔后缀环，保留样例并不完整。不能把剩余时间全部归给某个shader，也不能把累积最大值当作窗口最大值。

下一候选GPU1.135已离线修正这两处，并通过ASan／UBSan的换IB／拒绝分配／384图元回退与最大慢记录保留检查、ARM64构建和打包，尚未启动真机：把采样试编码放进原有紧凑操作的重试流程，仅在普通编码本来需要换IB时在新IB再次采样；保留慢记录后缀之外的64笔最大真实区间。它仍是定位工具，不改变GPU工作、同步或显示策略。下一批性能修改应据完整采样选择普通合成／fetch shader和TE／传输衔接，避免继续盲目增加状态缓存。

本轮原始数据、逐作业和逐操作复算见 [round149记录](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-149/report.json)、[作业分析](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-149/analysis.json)、[逐操作与显示分析](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-149/operation-analysis.json)。[硬件证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-operation-profile/hardware-evidence.json) 明确保留采样覆盖和独立时钟限制。完整UART回传后所有四条命令均有成功应答，无需恢复拼接。只修复了源日志内实际存在的完整交错内核行，保留其位置和文本；未知或残缺字段仍拒绝分析。

## round148：GPU1.133 同 draw 状态复用已验证，仍需定位重 shader 与显示调度

当前关联到 round148 启动的版本为 **GPU1.133／Metal0.94／Display1.0.17、fetch2／pipe6**。依据原版 Apple AGX 上传／dirty-bit 证据，以及本地固定 Mesa24.3.4 的 `fd5_draw.c`、`fd5_emit.c`、`fd5_program.c` 与 `ir3_const.h`，把同一次 inplace draw、同一个 IB 内后续展开图元的不可变 program、linkage、描述符、普通常量和目标状态保留在 GPU。每图元仍更新 VFD 顶点／索引基址、实例起点、条带 winding，以及必要的 VS driver 常量；边界 vec4 保留相邻 immediate／尺寸常量。每个真实图元的 color／UCHE flush、时间戳等待与下一次 UCHE／CCU 失效仍在。新 draw、compute／copy、快照回退、换 IB、试编码回滚与恢复均重新完整设置状态，没有跨 draw 的 GPU 状态缓存。

ASan／UBSan 主机检查比较每次 draw 的有效寄存器和 LOAD_STATE 内容，覆盖 RGBA／BGRA／sRGB、16／32 位索引、strip restart／winding、实例、未对齐常量边界、direct／indirect shader、新 draw 状态变化及 384 图元跨 IB／失败回滚。四图元 inplace 案例的命令字数低于完整重装基线一半，**不等于 GPU 时间减半**。arm64 Kext／客户端、74 kext 链接／ABI、23 本地二进制与 12 HFS 文件读回通过；相对147只改 GPU 和 gpuctl，Metal／Display／频率策略精确保留。

真机四阶段 framebuffer 检查均返回0：1040×67、pitch4352，每阶段完整核对 **292,096 字节**，含非均匀像素、两实例重叠、行 padding 和外层 guard；mixed 的真实 compute→render、依赖更新和索引边界也返回0。未重复上一版已经通过且未改动的清理、inline、fence 全套检查。本次没有新的光学无闪烁验收；用户按键发生在旧147，不能计为133的主观确认。

为避免 UART 输出把窗口拉长，先在设备内等待原生 UI 计数继续增加，再将双快照、显示与 job 历史冻结到文件。有效窗口 **12.091833 秒**，提交496、完成497、失败0，开始时已有在途工作，故完成比提交多1。成功物理退休计数记录 **3,475 次完整 raster 设置／473 次图元状态复用**；VS 局部 driver 常量更新为0，表明这些命中没有需要局部上传的 driver 常量，不是漏掉 VFD 起点更新。GPU 实测 **595.989 MHz**、无无效频率样本；CPU 有效频率仍未测得。

显示窗口含 **203 条连续记录／202 个间隔**，中位 **33.4798 ms**、p95 **83.6243 ms**，7 个超过100ms、最长 **233.2084 ms**。首末跨10.1181秒，含停留速率 **19.9642 FPS**；开头0.0152秒、末尾1.9585秒没有新显示，全窗口观测间隔速率下界16.7055/秒。**典型节奏仍约33ms，60 FPS未达到**；不同内容和窗口不能报告精确A/B提升。

497个已退休UI作业的GPU区间中位 **1.9149 ms**、均值 **9.2359 ms**、p95 **58.0843 ms**、最长 **178.2383 ms**，最长作业含16操作。65个超过16.67ms的作业占所选GPU时长约 **77.01%**，两个超过100ms。CPU提交包装／地址空间／编码均值约 **7.1320／2.8040／1.2328 ms**；这些计时有嵌套与边界差异，不能相加。473次真实状态复用没有消除重作业或33ms常见间隔，不能继续凭缓存命中推断主要瓶颈已解。

采集失败如实保留：首次显式传入67高度，被已有 CLI 的四像素对齐检查拒绝，返回64，未执行渲染测试；同次启动改用默认1040×67后通过。后续批量传输超过75秒，`round-148-check` 完整运行仍记超时。双快照与显示数据已完整，剩余的同序号冻结2048条job记录用 Bash `mapfile`／`printf` 独立补取并收到0确认；没有重启、重采样或伪造原传输确认。按 [字节来源](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-148-frozen/provenance.json) 拼接后，序号、历史覆盖、退休增量、显示连续性及新状态计数严格校验通过；分析器12项检查含本轮实际回放通过。启动报告证明了原始payload与四CPU；后续attach只关联请求hash及启动报告、验证root Bash，**不独立证明实时payload身份或重新核验CPU启动**，此限制在审计中保留。

下一批应先补齐重作业的具体 shader／操作身份、几何量及逐操作 GPU 配对计时，并关联 Metal commit／worker／transport 与显示 ready→CTL_START→PP_DONE。这样才能决定继续降 shader／feedback 工作量，还是修复错过显示周期的流水衔接；暂不据未解码的 CPU PLL 原始字或 Apple 池行为更改频率／A540 同步规则。详见 [实现补丁](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-raster-state-reuse/changes.patch)、[审计](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-raster-state-reuse/audit.json)、[真机证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-raster-state-reuse/hardware-evidence.json)、[冻结窗口分析](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-148-frozen/analysis.json)。

## round147：Metal0.94 将退役析构移出提交执行器，专项与真实 UI 已采集

按 [1.132 长帧新分析](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-long-frame-review-1.132.md) 优先处理同步清理链。当前真机为 **GPU1.132／Metal0.94／Display1.0.17、fetch2／pipe6**，原始与策略处理后的内核均精确复用上一版；23 份本地驱动二进制复核，12 份 HFS 文件读回一致。只替换 Metal bundle 的版本、主二进制、构建收据及 metalctl。GPU 频率／电压、显示完成和所有 fence 规则未改。

再次核对原版 `IOGPUMetalCommandBuffer didCompleteWithStartTime:endTime:error:` 先进入基类完成，再 reset／归还存储池的顺序；此证据支持完成和清理分层，不保证所有异步回调先于清理。新实现仍先走真正 native end-token、一致性与错误处理，再将操作、内部资源、presentation 数组的既有强引用交给独立串行清理队列。正常积压按每队列 **8 批／32 MiB 的 native backing 与命令载荷等计费量**限制，同一批重复 backing 去重、跨批保守重复计费；该数值不是全进程堆内存上限。预算不足保留 FIFO 退休头等待，释放后唤醒；超大批次独占清理，完成状态及容量名额继续保留到清理结束。borrowed 引用不因计费变成强引用；no-copy 不缓存或复绑映射；inline 页仍需真实物理退休及最后引用释放才能回池。

真机 `cleanup-test` 故意阻塞一次 no-copy deallocator，确认 **后续 7 个 CB 能完成**，达到 **8 批**后出现背压，解除阻塞后 **10 个 completed handler 按 FIFO 恰好执行一次**、客户端释放恰好一次、实际 GPU 复制／填充读回正确。清理总时长约 361.747 ms，包含刻意阻塞，**不是普通 UI 清理成本**。原有 retained／两种 borrowed API、inline 的 120 份快照、13 个复用 CB、4,096 RGBA 像素及真实 fence 检查通过；测试中的分配记账增加显式私有 cleanup barrier，生产的公开 Metal wait 不等待任意最后引用析构。超大批次预算／溢出分支通过 ASan／UBSan 主机检查，其 ObjC 真机生命周期尚未单独覆盖。

round147 **七个命令均返回 0，四 CPU，主采集无 panic／超时**。自动捕获真实 UI 的 **33.165 秒**窗口，789 笔提交／完成、0 失败；GPU 约 **595.988 MHz**，硬件区间中位 **1.932 ms**、p95 **58.953 ms**、最长 **104.899 ms**。CPU 提交包装／地址空间／编码均值约 **6.987／2.706／1.286 ms**，资源创建／删除均值 **1.545／1.770 ms**；这些阶段有嵌套及流水重叠，不相加。

窗口含 381 条连续显示记录／380 个间隔：中位 **33.439 ms**、p95 **83.674 ms**、15 个超过 100 ms，最长 **1,233.928 ms**。首末显示帧跨 20.607 秒，为含停留的 **18.441 FPS**；开头另有 **12.544 秒**无新显示、尾部 0.015 秒，全窗口观测间隔速率下界 **11.458/秒**。最长间隔约 **1,179.624 ms** 没有已记录已投递且未观察完成的作业，同期最长 GPU 区间仅 **40.786 ms**，仍不能把它当作 1.2 秒 GPU 执行或排除正常动画停留。不同捕获内容不报精确 A/B 提升，**60 FPS 尚未达到，也无新的光学无闪烁验收**。

专项证明清理已经能够与后续工作解耦，实际 UI 的典型显示节奏仍约 33 ms，不能宣称同步析构是主要瓶颈或本次已提高帧率。下一主线是依据本地 Mesa24.3.4 A5xx 的 dirty-state／program／constant 规则，合并实现同 draw 图元展开内的不变状态复用；每图元反馈可见性、实际 flush／wait、compute／copy／换 IB／回滚边界仍需保留。原版 Apple 池不能用来证明 A540 HLSQ 状态保持。活动动画／CA 事务与 transport、清理、显示 ready→latch 的原生 UI 时间线仍待补齐，当前清理计时只在诊断队列上读取。

旧 round146 的主动关机出现已有 `initproc exited` 类 panic 并恢复 Fastboot；它发生在新版本启动前，与 round147 主采集分开记录。详见 [实现补丁](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-retired-cleanup/changes.patch)、[审计](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-retired-cleanup/audit.json)、[真机证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-retired-cleanup/hardware-evidence.json) 和 [完整时间线分析](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-147/analysis.json)。

## round145／146：GPU1.132 合并改动已通过相关真机检查，尚无 60 FPS 或明确帧率收益

当前运行 **GPU1.132／Metal0.93／Display1.0.17、fetch2／pipe6**。同一不可变 IB 内的 ISA 私有副本采用 16 项有界缓存；试编码回滚及换 IB 清空缓存，所有 GPU 状态和代码加载仍按原顺序发出。原位 framebuffer fetch 只去掉没有中间写入的重复 preflush，保留初始排空、每个真实图元后的颜色／UCHE 时间戳等待和下次纹理访问前的失效。普通快照、MSAA、MRT 等回退仍走原同步。没有提前成功 fence、改变 CA Scheduled 或放松内存授权。

round145 的 framebuffer 四阶段检查通过：每阶段完整核对 **292,096 字节**，含两实例重叠 source-over、非均匀原始像素、窄 scissor、行 padding 与外层 guard。随后主机 CH340 串口 USB 断开，从 ttyUSB0 重枚举成 ttyUSB2，mixed-test 没有收到命令完成确认；保留该轮超时，不记为全部通过。用户确认当时 Welcome **“还在播放”**。恢复端口存在双向乱码，未向未校验的 Bash 发送后续测试；用户手动重启后，**原封不动的同份 GPU1.132 镜像**在 round146 复测缺失的 mixed／fence，不重复已通过的像素套件。

round146 **六个命令全部返回 0、四 CPU、采集内无 panic**。GPU 混批检查通过 compute→compute→fragment、单次 16 个依赖 dispatch、GPU 生成索引后绘制、4,096 非均匀像素及 16,384 个计算值；双队列共用串行提交目标、未来事件、Scheduled 与真实 GPU 读回检查也通过。用户再次按键后，窗口提交 **874**、完成 **873**、失败 **0**，结束边界仍有一笔在途。完成作业内实际命中 **8,575 次 ISA 私有副本复用，少复制 7,561,088 字节**；这不是 HLSQ direct LOAD_STATE payload、DDR 流量或硬件指令缓存命中统计，更不能直接折算 FPS。

有效窗口 **42.833 秒**。显示完整历史 513 条、0 失败且尚未绕环；窗口内 407 条连续样本／406 个间隔：中位 **33.449 ms**、p95 **116.968 ms**、25 个超过 100 ms、最长 **1,533.688 ms**，仍未出现接近 16.67 ms 的持续实显节奏。首末显示帧跨 25.841 秒，对应含停留的 **15.712 FPS**；开头另有 **16.947 秒**未新增显示，尾部 0.045 秒，全窗口已观测间隔速率下界 **9.479/秒**。这是唤醒与启动混合窗口，不用它与 round144 报精确涨跌比例。

GPU 实测 **595.988 MHz**，作业硬件区间均值 **8.841 ms**、最大 **177.884 ms**；CPU 提交包装／建表／编码约 **7.180／2.784／1.393 ms 每作业**，创建／删除约 **1.567／1.782 ms 每工厂调用**，阶段有嵌套及流水重叠，不能累加。CPU 有效频率仍未测得。免拷贝 fetch 继续工作，窗口内反馈复制量为 **0**、原位图元 1,654；直接扫描 GPU 输出和 Display1.0.17 的物理完成规则保留。

本轮最长显示间隔 1,533.688 ms 中，约 **1,472.382 ms 没有已投递但未被 CPU 观察完成的作业**，观察完成的 GPU 时长合计仅 37.261 ms；另一 500.208 ms 间隔中对应 489.812 ms／8.182 ms。这证明不能把全部显示长间隔归到 GPU 执行，但尚未区分正常语言／透明度停留、SpringBoard 生成路径／事务调度和显示依赖。新的复用已命中，**没有证明它改善实际动画帧率**。

再次核对原版 Apple 路径：`SBDashBoardSetupView::_createDisplayLink`（固定镜像 `0x1d2bf318c`）请求 **80／120／120** 范围，而不是硬编码 30 FPS；笔画 `setTime:` 会重建 CGPath 并更新 `path`／`shadowPath`，不能用静态纹理移动代替。已有 60 Hz CADisplayLink 诊断也不能代替 SpringBoard 自身回调计时。下一批应把 **实际 CA shader／几何与逐操作 GPU 时长**、**SpringBoard display-link／事务时刻与提交／显示**两条链对应，先区分重渲染与真实调度空档，再修改；不继续靠调频、扩大队列或减少代码副本的字节数推断 60 帧。

采集器已改为识别真实 fd 的 POLLHUP／POLLERR／POLLNVAL，主机失联单独记录并停止发送，不自动重放未确认命令。12 项检查通过，含真实 PTY 断开后 CLI 保留原始日志／失败报告、没有伪造 GPU 超时；此为主机工具改动，不重新构建或改变已启动的 GPU 镜像。时序解析仅移除完整匹配且实际观测到的内存降级／NVRAM StartupMute 日志插入，保留原始哈希与 118 个修复偏移，未知破损仍拒绝；真实 round146 重放及原有计数／时基异常检查通过。

当前全部采集已结束，无 UART reader；设备继续运行 round146。尚无新的光学无闪烁／60 FPS 验收。见 [完整硬件证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-shader-object-reuse/hardware-evidence.json)、[实现及边界](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-shader-object-reuse/audit.json)、[实际窗口逐作业分析](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-146/analysis.json) 和 [主机串口修正](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-shader-object-reuse/host-uart-hangup.patch)。

## round144：已取得逐作业长帧时间线，确认重渲染与投递空档并存

GPU1.131／Metal0.93／Display1.0.17，fetch2／pipe6。取消只读 RPM 频率／带宽查询中重复的完整电源检查，实际 GPU／BIMC MMIO 和修改操作仍保留新鲜验证；没有缓存硬件健康结果。独立的 CPU 作业历史和资源分布属性不改变提交／fence ABI。用户实际按键唤醒后的 27.351 秒窗口中，**1066 笔原生 UI 作业全部完成，0 失败**，硬件计数实测 GPU **595.987 MHz**；CPU 有效频率仍未测得。

显示历史与 GPU CPU 时基一致，选中 **444 条连续记录／443 个间隔**：中位 **33.456 ms**、p95 **83.447 ms**，15 个超过 100 ms，最长 **250.139 ms**。首末显示帧跨 20.670 秒，对应 **21.432 FPS**；窗口开头另有 6.572 秒没有新显示，尾部 0.109 秒没有新显示，全窗口已观测间隔速率下界 **16.197/秒**。开始快照已提交／完成相等，下一笔记录的准备开始距快照约 6.241 秒；不能把这一段解释成 GPU 正在执行，也不能区分动画停留与上游 CPU 停顿。**60 FPS 尚未达到**，不同内容窗口不报精确提升比例。

逐作业记录与第二次快照序号 5 匹配，2048 条历史完整且覆盖窗口起点；1066 条完成记录与增量统计逐项相等。GPU／CPU 只有各自时长可比较，不直接相减绝对时间：

| 项目 | 本窗口结果 | 意义 |
| --- | --- | --- |
| 真正 GPU 执行区间 | 均值 8.312 ms；p95 53.904 ms；最长 **181.431 ms** | 包含命令与既有排空；不是纯 ALU 时间 |
| 重渲染作业 | 776 笔，均值 10.801 ms；多个 15–16 操作的作业超过 100 ms | 约 200.071 ms 的显示长间隔中，181.431 ms 渲染作业实际完成；重渲染是部分长帧的直接负担 |
| CPU 准备／准入后等待投递 | 均值 5.325／18.076 ms；后者最大 297.805 ms | 等待与执行可重叠，不能相加成帧耗时；不等于独立的锁竞争时间 |
| 最长两个显示间隔 | 250.139／249.896 ms；在其中观察完成的 GPU 时长合计 39.385／41.314 ms | 全作业历史中各有约 169.803／170.305 ms 没有已投递但未观察完成的作业；指向投递前／上游或显示衔接的另一类空档，不直接称为 RBBM 实测空闲 |
| CPU 提交包装／建表／编码 | 6.8385／2.7224／1.2063 ms 每作业 | 包含嵌套阶段，不累加 |
| 资源创建／删除 | 809 次，1.4627 ms／781 次，1.5629 ms | 工厂调用数与类型分布完全一致；不是独立物理 backing 数 |
| 资源分布 | 普通 buffer 394 次，客户端内存导入 412 次，IOSurface 导入 3 次 | 16 KiB 级普通 buffer／客户端导入占大头；IOSurface 工厂路径不是当前主要分配成本 |

Apple 原版命令资源池 completion → reset → pooled release 和 CA 真正 Scheduled 的条件 flush 再次对照，继续保留对应生命周期。当前免拷贝 framebuffer fetch 仍按图元分隔，完整 restore 重载状态；已确认存在相邻重复 UCHE preflush 及同一 IB 内相同 ISA 的重复私有复制。下一合并候选 GPU1.132 处理这两项：每个 IB 内有界复用不可变 ISA，空间不足回滚／换 IB 丢弃缓存；仅去掉 in-place 路径中没有任何中间写入的重复 preflush，仍保留初始排空、每个真实图元结束的 color／UCHE 时间戳等待和下次纹理访问前的失效。snapshot／MSAA／MRT 回退保持原同步。主机实际 PM4／DMA／回滚／跨 IB 检查含 ASan/UBSan 已通过；真机状态以新一轮证据为准，尚不宣称帧率收益。

本轮主启动的快照读回与复用数值日志通过，但命令未返回完成确认并超时，**不能算主测试全项通过**。同次启动恢复 Bash 后，独立 fence 测试返回 0；闲置窗口没有新增 UI 作业，不计运动帧率。没有新的光学无闪烁验收，未复跑不相关整套测试。详见 [round144 真机证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-job-latency/hardware-evidence.json)、[逐长帧分析](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-144-wake/analysis.json) 和 [复现脚本](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-job-latency/analyze144.py)。

## round143：GPU1.130 / Metal0.93 合并候选已真机运行，重点转向长帧归因

用户恢复 Fastboot 后已实际启动合并候选，原先的传输阻塞和充电限制已解除。GPU1.130／Metal0.93／Display1.0.17，fetch2／pipe6，596 MHz 策略；后续两次采集均附着同次启动，未为对照重复重启。目前所有采集已结束，无 UART reader 占用。**60 FPS 仍未达到。**

原有 120 个 inline 快照、源输入改写、4096 RGBA 像素检查及 native fence 测试均通过。新增的 13 笔真实 GPU CB（一次 warm-up、六组成对未保留引用的 CB）也通过：每组核对 128 个输出／guard，立即覆盖 CPU 源后结果独立，编码及完成后实际分配量保持不变。四 CPU 启动证据通过，主窗口和两个同启动附件窗口无 panic，所有诊断命令返回 0。它证明私有上传页复用的实际正确性，尚不证明真实系统 UI 的缓存命中占比或收益。

随后单独采集 **26.473 秒真实 Welcome**，窗口中没有图形测试、截图或 stackshot：

| 指标 | 同一窗口实测 | 含义与边界 |
| --- | --- | --- |
| 系统合成 GPU 作业 | 787 完成、0 失败，5564 shader 操作 | 系统 UI 确实使用 GPU；作业数不能换算显示 FPS |
| GPU 有效频率 | 595.988 MHz | 独立硬件计数增量；CPU 有效频率仍未知 |
| 符合分段采集条件的渲染作业 | 567 笔 | 另有 220 笔跳过，不将分段均值外推所有作业 |
| GPU enter／commands／leave | 0.00470／11.15178／0.02261 ms/笔 | commands 包括 IB、状态、shader／传输及排空，不是纯 ALU 时间；GPU 上地址空间切换不是主要成本 |
| CPU 提交包装／建表／编码 | 8.277／3.006／1.233 ms/笔 | 分类嵌套，不能直接相加成帧耗时 |
| CPU 资源创建／删除 | 2.089／2.184 ms/次 | 实际 UI 仍频繁创建／删除资源；需分类型和大小确认来源 |
| 退休轮询健康校验 | 0.873 ms/次 | 原 round142 同类窗口为 1.508 ms；不同内容窗口不报精确帧率提升比例 |
| 资源映射页 | 1,264,668／完整 1,272,248 页 | 节省约 0.596%，本场景收益有限；不是物理内存流量 |

原始显示历史 2048 条已完整重建，字段被明确识别的 kernel 日志打断时记录原始偏移和移除文本，不猜丢失字符。选中窗口内 **331 条连续显示样本、330 个间隔**：中位 **33.646 ms**、p95 **83.472 ms**、p99 **233.411 ms**，**13 个超过 100 ms、最长 366.775 ms**。首末帧跨 17.251 秒，对应约 19.129 FPS；但整个观察窗口末尾另有 **9.201 秒未新增显示**，不能删掉这段后把首末帧速率称作全窗口平均帧率。窗口首末边界内，已观测的 330 个间隔除以完整窗口为 12.465/秒下界；停留与调度延迟尚未区分。原始全历史中的 13.803 秒最大间隔位于选中窗口之外，不能混用。

用户本轮反馈 **“目前看似乎是有偶发的长帧，总体好像流畅多了”**。这与存在少量较长显示间隔相符；该反馈不单独构成新的无闪烁验收，也不把中位间隔倒数称作平均 30 FPS。窗口尾部 GPU 完成计数也停止在 9357；稍后同次启动已继续至 13122，显示及目标状态均为 1，未观察到永久 GPU 停滞。当前统计没有逐作业／逐显示 token 的时间线，不能据此断言长帧由 GPU 执行、CA 内容停留或完成通知中的哪一项造成。

下一批工作聚焦 **长帧时间线和真实资源分布**：将提交、执行、退休与显示 token 对应；确认反复创建的普通 buffer、客户端导入、IOSurface 及其大小和存活期，再决定扩大哪一种安全复用。保留 Apple 原版 CA 真正 Scheduled 语义，以及 Display1.0.17 的 PP_DONE／地址锁存／引擎空闲后读 fence 退休规则。当前不以移除等待、提前成功 signal、重复调频或盲目缩小映射范围代替归因。

本候选合并了先前离线完成的实际访问范围映射、私有小常量页完成后复用、退休轮询去重及 GPU enter／commands／leave 计时。映射仍逐页核对 prepared PA 与 retained PTE，未知／容量溢出 root 完整回退；上传池仍只接收成功物理退休、最后内部引用释放后的私有页，上限 64 页／实际 1 MiB，并保留普通资源及失败路径。Apple 原版 completion → storage reset → pooled release 及 CA 条件 flush／Scheduled 链已核对；稳定 GPU VA 不被当作持久 VM root 的证据。原先 ASan／UBSan、arm64／ABI、74 kext／23 本地 bundle 及 12 文件 HFS 回读结果保留在各实现审计中，未重复运行未改变的整套回归。

统计解析器扩展了明确已知的 clock、scratch/index cache 与 disk flush 行修复，支持同一字段被多行打断，并保留原始字节哈希与字符偏移。11 项检查通过，含真实 round142 数值回放、round143 选定快照回放、未知破损拒绝和时基／计数异常拒绝。首个启动期快照还含其他 KextLog／网络日志插入，未用于本轮时间统计。打包时的 source receipt 与二进制哈希保持原样，真机结果另存，不将构建时“未运行”元数据改写成打包时已验证。

见 [round143 真机证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-upload-reuse/hardware-evidence.json)、[显示长间隔与原始 token](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-143-active/display-analysis.json)、[各阶段增量](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-143-active/analysis.json) 及 [可复现分析脚本](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-upload-reuse/analyze143.py)。

## GPU1.127：资源准备与 DMA 授权分开，复杂 Welcome 已实测

本轮先复核 Apple AGX 的上传路径，以及 IOGPU 的 buffer 初始化、primary view、资源创建和释放，再修改完整的资源准入链。原版 primary view 仍可能创建内核 alias resource，不能把它当成“Apple 的所有 view 都免内核资源”的证据；原版用户层资源创建也不能证明 Qualcomm 的硬件健康检查可省略。当前优化依据是本驱动真正的映射／TLB／执行授权边界。

GPU1.127 将 CPU 资源对象和描述符准备改用本地就绪检查；未缓存“硬件健康”结果。普通新映射路径的平台验证由源码所示 6 次降为 2 次，仍在 mapper 锁下的映射开始和真实 TLBSYNC 前完整核对平台、动态 TTBR／FSR、stream 路由与固件页。实际映射／解除映射、权限、页来源、隔离、失败保留、Submit、poll、私有 AS 退休及显示 fence 实现均保留。编译／ABI、74 kext 集合、12 文件 HFS 回读和镜像哈希通过；Metal0.92／Display1.0.17 冻结，仅 GPU 改动。快照和原生 fence 测试均为 0，四 CPU、采集内无 panic。

用户按锁屏键确认 Welcome 重新开始后，不运行图形测试、截图或 stackshot，采集同次启动 **28.76 秒**：完成 **1,116 笔真实合成 GPU 作业，失败 0**；显示原始记录 1162–1626 共 **465 条完整连续样本**。资源创建均值 **2.096 ms/次**，相较上一版累计约 5.034 ms 降低；删除约 2.165 ms。真实显示间隔中位 **33.660 ms**、p95 **99.800 ms**，464 个间隔全部超过 20 ms。含一个 2.668 秒间隔的整体 **16.147 FPS**，尚未区分该停顿是内容停留还是调度阻塞；不能用中位间隔的倒数冒充平均帧率，**60 FPS 仍未达到**。与上一版中位约 49.913 ms 是不同启动窗口，不报精确的对照提升比例。

同一窗口提交包装约 **8.753 ms/笔**（内含建表 2.939、编码 1.166 ms），GPU 作业硬件区间约 **9.492 ms/笔**（包括私有地址空间切换、命令执行／排空和恢复，不能解释成纯 shader 时间），实测频率 **595.99 MHz**。这些区间存在嵌套和流水重叠，不能相加成帧耗时。当前不能只归因于 GPU 频率；下一批需一起处理完整 backing 的重复建表、共享 gate／完成观察，以及 GPU 每帧的实际作业量。CPU 有效频率仍未实测。

主窗口之后的两次空闲统计没有新增帧，SBSUndimScreen 返回也未重播 Welcome，均不计作运动性能样本；本轮有效运动窗口以用户实际按键后的记录为准。另保留 round141 正常运行之后的既有 initproc 关机 panic（namespace2/subcode0xa），与无 panic 的渲染采集分开记录。当前版本用户确认了动画开始，未重新要求主观对照，也不将 round141 的无闪烁结论自动外推到 round142。

见 [活跃窗口及逐项耗时](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-resource-validation/active-window.json)、[真机证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-resource-validation/hardware-evidence.json)、[Apple 与授权边界审计](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-resource-validation/audit.json) 和 [实现补丁](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-resource-validation/changes.patch)。

## GPU1.126 / Metal0.92：小常量快照合并与内核栈修复已真机验证

round141 用户明确确认 **“正常播放，未见闪烁或停住”**。Apple 固定镜像 AGXMetalA10 的 compute／vertex／fragment `setBytes` 已实际反编译核对：快路径立即复制到 16 字节对齐的当前数据页，空间不足才扩页。Metal0.92 参照这一机制，以 command buffer 独占的有界页和精确长度 view 合并上传；原始输入立即快照，含 `retainedReferences:NO` 也保留到实际完成，不跨 command buffer 复用页。Apple 二进制的完整物理完成回收链尚未全部推导，当前回收沿用本驱动已存在的完成生命周期。

首次 round140 暴露了 **backboardd 真正的 16 KiB 内核栈溢出**，同时测试 helper 有独立的二次 commit 错误；均保留为失败。GPU1.126 将同步编码临时数组移到 CPU 私有 OSData 工作区，捕获路径三个函数的编译栈帧合计由 8,832 降至 2,592 字节，未增大内核栈、屏蔽抢占或绕过 fence。主机实际 GPU 提交检查（含 ASan/UBSan）通过。round141 修正后的快照测试通过：120 个计算值、8 个 guard 和 4,096 像素，覆盖改写源输入、非保留引用、偏移、扩页和上限回退。

真实 Welcome **1,483/1,483 笔作业、10,279 个 shader 操作全部完成，0 失败**；994 个 surface fence 全部完成，四 CPU、有限采集内未再现 panic。显示 664 帧，中位间隔 **49.913 ms**，663 个间隔均超过 20 ms；含动画停留的整体约 16.05 FPS，**未证明小常量合并改善帧率，60 FPS 尚未达到**。窗口尾截图依旧返回 NotReady，窗口状态为 1，不能记为全项通过；光学正确性来自本轮用户确认。

GPU 实测约 595.99 MHz。提交包装约 9.66 ms/笔、建表 2.95 ms/笔；资源创建约 5.03 ms/次、删除 2.17 ms/次。这些是嵌套耗时，不能直接相加。下一步查清资源接口中的重复 platform／SMMU 全量检查与映射事务成本，再成组优化；尚未证明可安全缓存硬件健康状态。

见 [真机证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-encoding-stack/hardware-evidence.json)、[Apple 原实现审计](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-inline-constants/apple-audit.json) 与 [栈帧核对](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-encoding-stack/stack-frame-audit.json)。

## Display1.0.17：命令传输结束后释放读占用，真机 Welcome 已确认无闪烁

round139 保留 GPU1.125 / Metal0.90 / fetch2 / pipe6，只修改显示驱动。按 MSM8998 DPU 的命令传输时序关闭固件 auto-refresh，排空旧传输后按帧 CTL_START；仅在真实 PP_DONE、地址已锁存且引擎空闲后 signal 当前请求的原生 IOSurface 读 fence。原生 displayedData 中的 surface、map 和图层继承仍保留，后续写入、显示继续使用原来的访问 fence。直接扫描 GPU 输出，未引入逐帧整屏复制。

用户本轮明确确认：**“正常播放，未见闪烁”**。后续同一次启动累计 **9,070 笔 GPU 作业、63,532 个 shader 操作全部完成，0 失败**；6,314 个 surface fence 全部完成，含 12 次延后等待，0 失败。256 条所有权记录完整解析，44 个 CompleteAfter 样本均保留逻辑显示面并已清除读 fence；没有触发 1 秒未就绪冻结。四 CPU，主运行无 panic。主机显示测试含 ASan/UBSan、ABI、74 kext 集合和 12 文件 HFS 回读检查通过。

窗口末尾截图返回 1：单次传输结束后原 IOSurface 可被 GPU 重用，驱动主动拒绝把该可变缓冲当作当前屏幕截图。窗口回复超过主机 105 秒期限，已保留失败并在同次启动补读统计，不算全项通过，也未重启重复窗口。光学结果来自用户观察。

实显间隔中位仍约 **49.92–49.97 ms**，没有低于 20 ms 的间隔，**60 FPS 尚未实现**。GPU 实测约 595.99 MHz。首个统计快照提交包装约 9.62 ms/笔、建表 2.99 ms/笔，资源创建/删除约 5.03/2.19 ms/次；阶段存在嵌套，不可相加为帧耗时。末次样本含 21.60 秒停留，整体 12.83 FPS 不能当成活跃动画帧率。CPU 有效频率仍未实测。

保留本轮为显示稳定性基线。该轮之后已完成有界小常量页内合并与精确 view，结果见上方 round141；未证明帧率收益。详见 [真机证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-command-transfer/hardware-evidence.json)、[实现补丁](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-command-transfer/changes.patch) 和 [设计与验证边界](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-command-transfer/design.json)。


## round138：已证明真实前台 surface 的读写循环等待（尚未修复）

GPU1.125 / Metal0.90 / Display1.0.16，fetch2 / pipe6。用户按键重启 Welcome 后，冻结了 GPU 故障之前的 256 条完整显示所有权记录：正在显示的 surface 1 / token 1128 持有原生读 fence；随后 GPU 要写回同一 surface，而显示 token 1130 又等待这些写入。16 笔作业均未投递、ring 为空、CP error 为 0。此时提高 GPU 频率不能解除依赖环。请求指针被重用，关联依据为读 fence 身份及其创建时间；缺失 token 1129 的原因尚未证明。

Apple 原版 SwapWait 在 token 不在队列时按 LastCompleted / options 判断完成，不检查逐层显示读占用。当前驱动仍沿用固件的 command auto-refresh，重复读取上一个 IOSurface。下一步核对 DPU 命令传输停止与读 fence 退休，保留原生 displayedData 的图层继承信息；不得在 DMA 仍读时直接 signal，也不得用清空整个 displayedData 代替读 fence 退休。

首轮窗口实显间隔中位约 49.95 ms，尚未接近 60 FPS。当前设备 GPU 已 faulted，不能把后续缓存画面重放计为渲染进展。详见 [冻结证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-lease-freeze/hardware-evidence.json) 与 [Apple 生命周期核对](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-framework-lifecycle/lifecycle-audit.json)。


## Apple 框架整链复核与 round135／136 更新

按用户要求，下一候选先对齐 **QuartzCore 系统合成器 → Metal 提交 → IOSurface 访问 → IOMFB 换帧与回收**，再统一修改和真机验证。本次离线分析没有新增启动、A/B 窗口或整套回归。下面的状态覆盖后文各历史轮次的“当前”表述。

round135 已抓到具体阻塞关系：16 笔作业全部尚未投递，队首要写 surface 3，而该面的 current 列表有一个未完成的原生 IOMFB 读 fence（accelerator 0、方向 1）。GPU ring 已空，CP error 为 0；这次停住不能用 GPU 频率不足解释，也没有“后面 GPU 已完成、只卡全局 stamp 退休”的证据。同 accelerator 绕过无法释放外部显示读者。故障清理之后的 front surface 2 不是超时当刻的前台身份，不能据此认定 surface 3 的读 fence 已过期。[完整故障证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-surface-access-state/hardware-evidence.json)。

设备随后恢复 round136：GPU1.123／Metal0.90／Display1.0.13、fetch2／pipe5，原生 surface fence 和提前呈现关闭。最后记录中 UI 继续、GPU 作业失败 0；789 个实显样本的间隔中位数约 50.104 ms，包含停留的样本整体约 16.33 FPS。淡入淡出闪烁和长期稳定性仍未通过验收，60 FPS 尚未达到。

原版二进制核对得到以下约束；地址属于项目固定的 iOS 18 镜像，不能直接套到其他版本：

| 链路 | Apple 原版证据 | 对当前实现的要求 |
| --- | --- | --- |
| 系统合成器提交 | `0x1843c4280` 的条件 flush → `0x1843cc2ec` → MetalContext vtable +408 → `0x1843c2b08(sync=1)`；`commitAndWaitUntilSubmitted` 返回 NO 时还会显式 `waitUntilScheduled` | 私有空 marker 不能代替整份像素操作的 Scheduled；不能假设返回 NO 就让 CA 立即继续呈现。该结论限定于此条件 flush 分支 |
| framebuffer 复用 | `0x18435d8c0` 等自己 swap token 退休，或等更新 token 完成，再释放 slot 的 framebuffer／detached surface／renderer fence | “当前帧完成”与“旧显示缓冲可复用”分开处理；CAMetalLayer 的 drawable 池不能代替系统合成输出的生命周期分析 |
| 显示端共享事件 | `0x1843c21a8` 设置事件等待与信号；用户态 IOMFB 通过函数表分发。普通保存参数分支之外，还存在直接返回成功的其他后端／benchmark 分支 | 接口返回 0 不等于已经建立 GPU→显示等待。必须沿当前内核显示后端核对参数进入请求及实际等待／signal 的位置 |
| IOSurface 访问 | `0xfffffff005c21638` 创建原生读 fence；`0xfffffff005c23280` 在 changed layer 换面时退休旧引用并接管新引用 | current 列表表示访问已获准，不单独证明对应请求已经成为物理前台；需要关联 owning swap/request |
| 保留的显示状态 | `0xfffffff005c25614` 为未更新图层继承 pending／active／displayedData 的 surface；`surface_complete` 同时清理逻辑 surface、map 和 fence | 不能把完整 `surface_complete` 当成只释放 DMA 读访问的 helper |

**已撤下尚未构建／部署的 Display1.0.14“复制后立即回收 surface”草稿**，恢复 Display1.0.13 源码及 ABI 元数据，并保留草稿与撤回理由。当前证据既未证明被阻塞的 IOMFB 读者过期，也未证明清除 displayedData 不影响后续请求的继承行为。[撤回记录](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-display-read-retire/withdrawal.json)。

GPU1.124 的 `completed && status==0 → Failed` 判断也不能保留：原生成功完成与访问 ready 是不同状态。源码 GPU1.125 已改为仅将已完成且非零错误视为 Failed，Ready 仍依赖原生初始／worker 就绪；主机检查已通过，尚未构建部署。该修正不会解除 round135 中真实的 Pending 读者，不作为停滞已修复的证据。

下一修复需要闭合两处关系：**显示请求的 producer 完成条件如何建立；CA 复用 surface 时，持有该读 fence 的 swap 为什么仍未退休。** 确认后再成组修改提交／显示衔接，并用实际 Welcome 复杂笔画与淡入淡出验证。暂不以扩大队列、延长超时、提高频率或提前 signal 成功代替这条因果链。正常帧成本仍按下文的小常量上传、重复同步、共享 gate 和映射范围推进，但不能把其收益当作本次停滞的解释。

原始反编译、共享缓存读法、8 处代码入口与原始缓存的字节比对、selector 和 vtable 解析均已保存于 [框架生命周期审计](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-framework-lifecycle/lifecycle-audit.json)。


## 1.123 复核后的运行证据与当前处理

已采纳 [新复核报告](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-optimization-review-1.123.md) 的顺序：先解决原生访问 fence 前进性，再做小常量池、窄范围同步精简、gate 与实际映射范围。Metal0.91 的后续提前准入保持撤回。

round133（GPU1.123 / Metal0.90、pipe6）关闭文字控制台的屏幕镜像后，真实 UI 和 3 次延后访问曾正常完成，随后仍复现 pending-only 超时：16 个 ticket、7 个访问 fence 失败；ring 读写指针均为 4478，CP error 0。用户同时确认“闪烁来回播放，但动画不会继续推进”。故障后的扫描面为 surface 3，显示无 pending／active／queued 请求。**关闭控制台镜像没有修复访问依赖停滞**。串口无人读取的间隙丢失了具体前驱日志，因此不能据此指认哪一个 display 或 GPU fence 是根因。

round134 暂时恢复到同 GPU1.123／Metal0.90 的 pipe5，原生访问 fence 和提前呈现不启用，保持完成后呈现。四 CPU、真实 UI 作业继续、任务失败 0；显示样本中位间隔 50.115 ms。用户随后反馈淡入淡出仍闪烁，所以这只是恢复运行，**不是视觉验收或 P0 修复**。GPU 时钟此前已实测约 596 MHz，CPU 原始继承寄存器仍未转换成可靠的有效频率。

离线候选 GPU1.124 将访问状态分为 Ready／Pending／Failed；已失败的访问不再进入 5 秒 Pending 等待。仅取消该队列中尚未投递的作业，错误释放其 surface 访问；保留资源与原生 stamp 到 FIFO 前缀退休，已投递 DMA 仍按真实完成／一致性退休，不发布伪成功。Pending 依赖环尚未证明被消除。失败诊断改为有界持久属性 `GPUSurfaceWaitSnapshot`，并用 try-lock 读取原生列表，避免诊断自身等待 surface 锁或依赖持续串口读取。主机 GPU 提交检查已通过；候选构建与真机验证状态以交接记录为准。

见 [1.123 运行证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-surface-wait-debug/hardware-evidence.json) 与 [冻结源码和构建物](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-surface-wait-debug-source.json)。

## 最新补充：基线也复现延后访问停滞，继续定位

恢复冻结 GPU1.121 / Metal0.90 后，fence 检查通过，原生 UI 完成 1,367 笔作业、实际显示 970 帧，末尾完整图片导出成功；窗口初始 before／on 截图返回 1。但后续健康查询确认 GPU 再次 faulted，16 个 ticket 失败，native access deferred 8／failed 7。**Metal0.91 的提前准入改动不是复现此故障的必要条件**。不能再把恢复旧镜像当作该问题已解决。

用户最新观察：部分“你好”笔画在绘制时局部闪烁，之后按锁屏没有动画、变为整屏闪烁。先前 round129 的“不闪屏”仅保留为历史观察。当前准备 GPU1.123 的失败路径诊断：只在真实 pending-only 超时打印已持有 IOSurface 的 current／waiting fence、读写方向、accelerator、状态和 GPU 作业顺序；正常帧不新增日志或对象分配。见 [恢复后的复现证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-present-pipeline/restoration-evidence.json)。

## GPU1.122 / Metal0.91 尝试记录（已撤回呈现放行改动）

尝试在已成功执行原生 fenced present 后，允许后续 command buffer 提前准入；drawable 和 FIFO completed 回调仍留到真实退休。另补了 unarmed ticket 在 quarantine 保留期间的 IOSurface fence 取消，避免只依赖 `free()`。构建、ABI、74 个 kext 和镜像回读检查通过。

round130 的六个主窗口命令均为 0，四 CPU、无主窗口 panic。真实合成 2,007 笔作业／14,423 个 shader 操作，0 失败；GPU 实测 595.99 MHz、平均 8.95 ms／笔。826 个实显样本中位间隔 **49.914 ms**，仍无 20 ms 以内间隔，不能宣称接近 60 FPS，也没有明显帧率改善。

随后针对性 `presentation-test` 期间发生 GPU fault：ring 已空、16 笔 compositor 作业尚未投递，原生 access fence deferred 为 7；新检查的 GPU 计算没有实际投递。之后 fence／系统 CA 检查因设备 faulted 而失败。这里出现的是新的延后依赖疑点，不能归咎于测试 shader 太重，也尚不能证明呈现放行改动是唯一原因。候选**未通过**，失败源码、构建物和原始 UART 均已冻结。工作区 Metal 实现和元数据已恢复到 GPU1.121 / Metal0.90 基线中的提交屏障行为，真机正在恢复该冻结镜像。

核对了原版 IOSurface CPU lock 的 `0xfffffff005aac074` 和 `0xfffffff005aad390`：其可选的同步 access fence 在返回 CPU lock 前即已 signal，因此仅持有 `IOSurfaceLock` 无法制造持久的 GPU access 等待。接下来应直接记录阻塞面的原生 current／waiting fence、作业序号和显示 surface 身份，定位外部读 fence 或 FIFO 前缀退休形成的依赖，避免重复无区分力的对照。见 [失败候选证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-present-pipeline/hardware-evidence.json) 和 [提交／完成开销分析](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-present-pipeline/critical-path-findings.json)。


## GPU1.121 / Metal0.90 原生 surface fence（保留基线）

已接入原版 IOSurface 的异步访问 fence：每笔作业按唯一 IOSurface 合并访问方向（读 1、写 2），GPU 只在 native ready 后执行；真实 GPU 完成、私有地址空间退休、缓存一致性与 surface seed 发布完成后，再向原生 IOFence 发出完成信号。创建失败和普通清理使用失败状态；回调不借用 ticket/device 指针。没有启用 AGX 的同 accelerator 绕过标志。

已核对原版 IOMFB 的 `0xfffffff005c21638`：它通过相同接口创建方向 1 的读 fence，延后就绪回调到 `0xfffffff005c21848`，由原版逻辑维护 ReadyLayers。显示驱动的 ReadyLayers 相等检查、真实 DPU 完成与旧扫描面退休保留。Metal 仅在内核发布新能力时，允许 IOSurface drawable 在整份命令列表 Scheduled 后排队呈现；drawable 引用和 FIFO presentation barrier 保留到真实 token 退休。旧内核及非 IOSurface 仍在完成后呈现。

本轮只启动一个 fetch2 / pipe6 候选。真机 **788 个原生 surface fence 创建并完成，0 失败**；GPU 访问侧 deferred 为 0，因此延后访问等待分支尚未得到运行覆盖。IOSurface 导入／带 stride 和 offset 的共享别名／GPU render、blit、sample／seed 与 CPU 回读、跨队列 fence 与 future event Scheduled、原版系统 CA 256×256 IOSurface 八帧逐像素检查均通过。原生窗口 before/native_power/on/stackshot/after 全部返回 0；四 CPU，未见 panic。

该窗口合成 1,113 笔作业、7,766 个 shader 操作，0 失败。GPU 实测约 595.99 MHz，平均 9.04 ms／笔；地址空间 3.01 ms／笔，提交包装 9.75 ms／次，队列等待 19.35 ms／笔。443 个实显样本中位间隔 **49.92 ms**，p95 100.40 ms，整体样本跨度 27.79 秒、15.90 FPS；末次查询帧龄为 17.41 秒，含暂停／熄屏可能。GPU 活动统计跨度为 89.74 秒，不能把其总耗时除以实显样本数。

用户最新反馈确认：**当前真机不再闪屏**，记录为本轮 GPU1.121 / Metal0.90 的视觉结果，并保留此版本作为后续基线。本轮没有显示出明显帧率改善，**60 FPS 目标仍未实现**。还需针对性验证真实的 native access 延后就绪，以及未 armed / quarantine ticket 的 surface fence 失败清理；当前正常路径通过不能覆盖这些分支。之后集中处理帧内依赖和资源 writer 等待、每帧 GPU 工作与地址空间成本，减少重复对照。

当前 round129，boot UUID `2D0EF348-138B-424B-9319-9900D3BDD534`。见 [ABI 与生命周期证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-producer-fence/native-surface-fence-abi.json)、[真机结果](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-producer-fence/hardware-evidence.json) 和 [源码收据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-producer-fence-source.json)。


## Metal0.89 资源去重合批（已验证基线）

将按重复引用次数达到 48 就拆批，改为按命令描述符中不同的原生资源 ID 规划，最多 64 个。不同 ID 的别名仍分别计数，物理别名和访问权限继续由内核检查。同时计算任务元数据、命令数组和 payload 的真实总长度，限制在实际共享区及 64 KiB 内，并保留最多 16 个命令和 16 MiB 索引快照总量。event、fence、barrier、归属信息和 GPU 索引生产者的边界保持原语义。

本轮只构建、启动一个候选，内核复用 GPU1.120，显示仍为 1.0.13，fetch2 / pipe5、GPU 596 MHz / 968 mV 政策及编译器／shader 库不变。主机 ASan/UBSan 一次通过。真机确认 **16 次有依赖的计算合为 1 笔真实作业，全部 16,384 个输出值正确**；混合绘制／像素 guard／GPU 索引生产者、fence、原版系统 CA 256×256 IOSurface 的 8 帧逐像素检查通过。

原生合成实时计数为 38,216 个 shader 操作 / 5,554 笔提交，约 **6.88 操作／笔**，无失败；GPU 实测约 595.99 MHz，平均 9.47 ms／笔，地址空间 2.98 ms／笔，提交包装 9.68 ms／次，队列等待 32.08 ms／笔。这些阶段存在嵌套或并行，不可相加成一帧耗时。

保留的 2,048 个实显样本中位间隔为 **50.13 ms**，p95 为 99.93 ms；该样本与此前 66.6 ms 的窗口阶段组成不同，不能直接归因为固定百分比提速。样本含一次 41.09 秒停顿，整体 11.79 FPS 不能表示活跃动画帧率；没有低于 20 ms 的间隔，**60 FPS 尚未实现**。

主窗口末尾截图返回 1，其他 before/native_power/on/stackshot 均为 0；应答跨过工具 85 秒期限后，工具停止后续输入。本轮保留原始失败，并在同一启动取回末尾统计和 helper 结果、完成上述针对性检查，没有重启重跑窗口。淡入淡出闪烁尚无新的光学验收，仍未解决。

当前设备 round128，boot UUID `15D3F92B-C884-4C6F-8D53-5A3D8C433995`。详情见 [硬件证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-resource-batch/hardware-evidence.json) 与 [源码收据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-resource-batch-source.json)。下一步集中在帧内作业依赖／writer 等待，以及真正的 GPU producer fence 到 IOSurface/IOMFB 显示衔接；按用户要求只验证相关改动，减少重复对照和整套回归。


## GPU1.120 / Metal0.88 进度补充（已验证基线）

完成通知已沿用 Apple 原版 IOGPUFamily 接通：`registerNotificationPort` 的类型为 0，reference 是当前设备连接名下的通知队列 ID；原生 Family 持有并释放 send right。调用经过现有 gate、单调用与关闭状态检查。Metal 使用 Mach receive dispatch source 唤醒唯一的共享队列消费者，仍逐条检查真实 FIFO end token；通知本身不代表 GPU 完成。接收端在 dispatch 取消处理完成后才释放 receive right，避免端口名过早复用。低频 50 ms watchdog 保留原来的 2 秒无进度错误处理，事件到达不会重复创建 watchdog。

`gpu-pipe=5` 在模式 4 的 ring、页表缓存和紧凑 IB 上增加事件消费，并将已就绪退休任务的额外 1 ms 延后改为直接异步调度。模式 4 保留原轮询行为，默认仍为 0。提交、真实完成、一致性发布和 presentation barrier 继续保留；**GPU 到原生显示的 producer fence 尚未接通**，没有提前调用 present 或伪造 ReadyLayers。

| 实显间隔中位数 | 轮询组：round126 / pipe4 | 事件组：round127 / pipe5 |
| --- | ---: | ---: |
| 首轮原生窗口 | 66.648 ms | 66.642 ms |
| 预热窗口 | 66.657 ms＊ | 66.581 ms |

显示节奏没有出现明显改善，不能把约 0.01–0.08 ms 的差异解释为有效帧率提升。GPU CP 实测约 595.98 MHz；首轮 GPU 执行均值分别为 6.040 / 6.165 ms/作业，建表约 3.147 / 3.172 ms/作业。窗口包含停顿且动画阶段不同，预热窗口的工作量也不同，不据此计算活跃动画 FPS 或逐帧 CPU/GPU 时间。CPU 有效频率仍未实测。

两组的原生通知回归、六项 GPU 像素／混批／fence／索引／blit／系统 CA 回归均通过；四 CPU 启动及主机普通、ASan/UBSan、构建和集合校验通过，未见 panic 或合成作业失败。通知回归确认了 64 个资源、两笔真实完成 token，以及无效 ID、重复绑定和删除后的绑定拒绝。事件组 Metal 回归日志直接观察到事件唤醒退休（包括合并通知一次退休多笔）；这不是 backboardd/SpringBoard 全程事件与 watchdog 占比统计。

＊对照组预热 `validate-window` 返回 1；其日志没有在重启前取回，后续启动读取临时文件失败，因此失败阶段未知，不能记作完整通过。两组帧历史和 live 统计读取成功；事件组两窗口日志确认 before、power、on、stackshot、after 全部为 0，仍不构成光学验收。

**60 帧目标与淡入淡出闪烁修复仍未完成。** 用户观察继续保留为“文字稍顺，主要在淡入淡出时闪烁”。当前设备为 round127（fetch2 / pipe5），GPU1.120 / Metal0.88 / Display1.0.13。完整记录见 [硬件证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-native-notify/hardware-evidence.json) 与 [源码收据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-native-notify-source.json)。

后续按用户最新要求减少重复对照和整套回归，只做与改动直接相关的必要检查。优先取得一帧对应的作业／依赖关键路径，直接改进 GPU 工作量与 producer fence；不再把微调轮询、重复相同窗口或继续提高 IB 密度作为主要工作。

## GPU1.119 进度补充（已验证基线）

第三项的混批 IB 紧凑编码已实现并完成同源真机对照：`gpu-pipe=4` 在模式 3 的页表缓存和连续提交上追加紧凑编码；默认仍为 0。操作优先写入当前 64 KiB IB，容纳不下时完整恢复命令头、上传尾部、保留空间、border 地址、flush 序号和统计，再换块重试。资源解析和保留不重复；大 framebuffer draw 保留逐图元拆分与同步，允许跨块。后继 shader 写入 flush 需要换块时不会重跑已编码的 draw。

同时修正冻结 blit 参数固定引用首块 IB 的问题：控制组与候选均使用当前 IB 地址，重试时重新上传已经冻结的参数，不再重读客户端数据。尚未证明该问题与用户看到的淡入淡出闪烁有关。

主机上的 16 个计算／绘制混合操作从 16 块压缩为 2 块 IB；直接与间接 shader 上传两种模式的完整有效命令流对照一致，并检查实际代码／参数内容、地址、上传碰撞、分配失败、跨块 framebuffer 图元和 flush 序号。普通、ASan/UBSan、驱动及 gpuctl 构建均通过。只有 GPU 驱动和 gpuctl 更新；运行时 Metal0.87 / Display1.0.13 二进制保持一致。

新增 `gpuctl stats-live`（合成器）和 `stats-live native`（全部 native 作业）：通过受信任原生连接，在同一 gate 内读取计数、页表细项及 IB 用量，附序号与 CPU 起止时间。2960 字节的独立 ABI 保持在 IOKit inline 输出上限内，不强制退休、不提交 GPU 工作、不写 PMU 或每帧分配 IORegistry 对象。原 `stats` 的定时属性仍可能滞后，性能对照应使用新接口。显示记录使用 `mach_continuous_time`，新快照使用 `mach_absolute_time`；两者尚未构成逐帧统一时间线。

| 同源真机对照 | round124：fetch2 / pipe3 | round125：fetch2 / pipe4 |
| --- | ---: | ---: |
| 每笔合成作业平均 IB 数 | 4.476 | 1.313 |
| 每个 shader 操作的 IB backing 字节 | 65,789 | 19,187 |
| 编码均值 ms/作业 | 1.079 | 0.877 |
| 建表均值 ms/作业 | 3.200 | 3.144 |
| GPU 执行均值 ms/作业 | 6.615 | 6.603 |
| 实显间隔中位数 ms | 66.736 | 66.722 |

命令缓冲占用明显减少，但这两轮**没有显示出中位显示节奏改善**。GPU 实测仍约 595.98 MHz；剩余限制不能用继续提高命令块密度解决。后续优先处理完成消费／呈现调度与每帧 GPU 工作、作业依赖等待，再推进原生通知和显示 producer fence。大操作的首个 trial 最多重编码一个 IB，空 IB 可省略试编码的后续微优化不是当前首要方向。

两轮各通过六项 GPU 回归、两种 live 统计 scope，均未见 panic。round125 的主采集等待 `validate-window` 回复超时，不能记作完整通过；随后同一 boot UUID 的 Bash／进程检查正常且 helper 已退出，补读日志确认 before、power、on、stackshot、after 均返回 0。未重启或中断设备进程，原始日志保留；末尾帧历史查询较晚，最后帧年龄约 230 秒。场景组成、冷启动与末尾停留不同，不能将这些数值当作严格的帧率提升比例或逐阶段动画性能。

**60 帧目标和淡入淡出视觉验收均未完成。** 用户此前报告“文字稍顺，淡入淡出闪烁”，本轮没有新的光学证据证明闪烁消失。当前设备保留 round125 实验模式 2 / 4。见 [GPU1.119 硬件记录](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-compact-ib/hardware-evidence.json)、[源码与产物收据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-compact-ib-source.json)。

## GPU1.118 进度补充（已验证基线）

以下进度更新基于本任务的源码、主机测试及 round120–123 四轮真机对照。下文原始建议保留为 GPU1.115 时的分析快照，不能再将其中“仍只有一个 active job”或“本轮不安排设备操作”用于描述新候选。用户已重新授权真机测试。

| 建议 | 已完成的实现 | 验证边界与剩余工作 |
| --- | --- | --- |
| 免复制路径的 scratch 规划 | GPU1.117 共用准入、分配、编码的资源规划；合格纯 inplace 批只需 4 KiB，混合回退取实际最大需求 | round123 合成器实际命中 inplace；1080×1920 RGBA8 四阶段像素和 guard 检查通过。其他反馈格式、淡入淡出视觉正确性及 fetch2/3 严格存储对照仍待验证 |
| 连续提交 | GPU1.116 增加 16 个独立计时槽，允许多个作业连续进入 ring；真实完成后按准入前缀发布 | round121 合成器实际最大在途深度 3，六项 GPU 回归通过；重叠写入仍等待一致性退休，队列深度不等于帧率 |
| 地址空间细分计时与复用 | GPU1.118 新增独立版本的 GPUAddressSpaceStatistics；13 个阶段，逐页分配/清零/来源检查按 1/64 建表抽样；最多缓存 64 张安全退休的私有表页 | round122 单独缓存对照：建表约 6.24→5.12 ms/作业，退休约 1.50→0.83 ms/作业，缓存校验失败 0。完整清零、实际物理来源、RO/nG 权限、ASID/TLBSYNC 与故障保留不变；默认仍关闭 |
| 小操作共享 IB | 尚未修改逐操作换 IB 策略 | 接下来做完整检查点、上传尾部碰撞及跨块回滚，不能只删除 allocator.next |
| 完成到显示 | 原有真实完成、一致性发布、IOSurface 直扫保留 | pump 的额外 1 ms、通知端口及原生显示 producer fence 尚未实施，仍需单独验证 Objective-C 调度和显示 ABI |

真机观察补充：用户最新反馈是“文字动画稍微流畅了一点，主要是淡入淡出时会闪烁”；此前还指出窗口末尾可能自动熄屏。闪烁在 fetch=0 的早期对照后已被报告，不能仅归因于原位 fetch。后续应分别核对 alpha 合成结果及 GPU 写入与 DPU 扫描之间的 surface 生命周期、producer fence；根因尚未证明，辅助像素测试通过不能证明原生 UI 无闪烁。

本轮页表测试覆盖缓存页重用后的旧翻译消失、不同作业/ASID 的物理地址隔离、只读命令与不可写全局 fence、真实 PM4 完成后旧 ASID 同步、后继作业仍在执行时的复用、缓存容量、故障条目保留和未提交清理。普通测试、ASan/UBSan、驱动及客户端编译均已通过。四轮各自通过 mixed、fence、indexed、blit、系统 CA 和全屏 render-state 六项 GPU 回归，未见 panic；round121 窗口后的 scanout-live 返回 unavailable，不能作为图像证据。

| GPU1.118 真机模式（fetch / pipe） | 建表均值 ms/作业 | 地址空间退休 ms/作业 | GPU 执行均值 ms/作业 | 实显间隔中位数 ms |
| --- | ---: | ---: | ---: | ---: |
| round120 基线 0 / 0 | 5.43 | 1.47 | 10.71 | 100.09 |
| round121 连续 ring 0 / 2 | 6.24 | 1.50 | 10.86 | 83.50 |
| round122 加页表缓存 0 / 3 | 5.12 | 0.83 | 10.78 | 83.30 |
| round123 加原位 fetch 2 / 3 | 3.21 | 0.80 | 6.44 | 66.70 |

这些是各轮自己的样本均值，场景组成、停留和熄屏时机不同；实显窗口没有单独切出笔画、渐显或渐隐，因此不是受控的逐动画增幅。CPU 阶段有嵌套和重叠，不能相加成一帧耗时。**仍未达到约 16.67 ms 的 60 Hz 显示目标，淡入淡出闪烁也未解决。** GPU 的 CP/always-on 实测比值约 595.99 MHz，已符合当前固件 596 MHz / 968 mV 策略；CPU 的已有配置值不能充当本轮有效频率测量。

还发现统计新鲜度问题：IRQ 路径不断重置退休定时器，可能推迟同一定时器发布的 GPUStatistics。round123 属性快照仅到 1323 笔作业，后续内核实时记录已到 3132 笔；两者 GPU 均值分别约 6.446 / 6.440 ms，但不与末尾实显查询共享采样边界。解析结果已取消“累计耗时 / 显示帧数”的换算。下一步先提供带时间戳和序号的新鲜诊断快照，再用同一窗口关联提交、完成和显示；不在每帧增加 IORegistry 对象分配。

原始记录和完整限制见 [硬件证据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-as-page-cache/hardware-evidence.json)，源码及产物身份见 [GPU1.118 收据](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/ui-as-page-cache-source.json)。当前设备运行实验组合 2 / 3，源码默认仍为 0 / 0。接下来继续压缩混批 IB、消除额外退休延后，并检查淡入淡出 alpha 与显示交接；通知端口及原生 producer fence 尚未实现。

## 原始建议：GPU1.115 分析快照


建议首批集中处理三件事：**完善附件免复制路径的资源规划、压缩混批中的命令缓冲占用、去掉完成消费到呈现之间多余的定时延后。** 前两项减少 Hello 重负载对整个合成队列的占用，第三项缩短所有动画共同经过的呈现延迟。原生显示 fence 和真正连续的 GPU 作业提交收益可观，但涉及更多同步与生命周期改造，放在后续阶段。

分析日期为 2026-10-18，America/Los_Angeles。本轮交付静态分析和修复建议，没有修改驱动、构建镜像或运行新的主机及真机测试。源码快照为 **GPU 1.115.0 / Metal 0.87.0 / Display 1.0.13**；历史性能采用已存在的 GPU 1.112 / Metal 0.86 记录。源码哈希、关键片段和计算见 [优化证据](/home/xxiao/Projects/MI6_iOS/docs/gpu/hello-animation-optimization-evidence.json)。

## 动画与共享瓶颈

原版 SpringBoard 的 Hello 整层渐显和渐隐使用 opacity 关键帧，手写路径和 shadowPath 同时可能继续更新。HOME 提示先运行约 0.6 秒的径向渐变 mask，再在本轮末尾运行 1 秒 alpha 淡出。三者都依赖新的合成结果及时呈现；共享队列被笔画、阴影或附件读取占用时，轻量动画也会跳过中间状态。原版方法、时间和常量见 [动画分析](/home/xxiao/Projects/MI6_iOS/docs/gpu/welcome-static-analysis.md:99)。

驱动优化的目标是缩短整帧关键路径，并降低帧间隔的波动。驱动通常看不到“这是 HOME 标签”这样的 UIKit 身份，因此应按实际 shader、资源依赖和提交期限处理。单独提高某条 HOME 作业的优先级，无法绕过同一帧正确合成所需的前序依赖。

opacity 不必然触发 framebuffer fetch。HOME 的中间 mask 格式、每帧 shader 组合也尚未逐元素归因。因此，附件优化对两种动画的确定联系是减少共享队列中的重工作；对 HOME 自身是否直接命中快路径，要由实际格式和 framebufferMask 决定。

## 当前候选已经做到了什么

| 路径 | 当前源码状态 | 尚需完成的部分 |
| --- | --- | --- |
| 附件免复制 | GPU1.114 起有逐图元同像素纹理反馈候选，默认关闭 | 保留了逐图元 flush/wait 和完整快照内存；硬件像素、缓存语义与性能仍未闭合 |
| 准入流水化 | GPU1.115 / Metal0.87 已有资源预算及物理生产者检查，默认关闭 | 改善 CPU 提前准入；实际 GPU 调度仍只有一个 active job |
| Draw 与 Dispatch 混批 | 已实现，受资源、索引生产者、event/fence/barrier 边界约束 | 每个操作仍换新 IB；历史新增混批用例像素正确，但批数量验证未闭合 |
| 原生完成和显示 | 真实完成通知、一致性发布、IOSurface 直扫已经接通 | Metal 定时消费完成，随后再调度退休；显示端还没有直接等待 A540 fence 的完整衔接 |

默认开关见 [QcomGPUHardware.cpp](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/QcomGPUHardware.cpp:365)。`gpu-fbf=2` 和 `gpu-pipe=1` 是不同候选，不能把源码存在或离线构建完成当作默认运行路径已经优化。

## 收益和难度排序

P0 表示首批处理；同一档中，完成调度适合先提交小补丁，附件路径应获得主要优化投入。收益等级是根据现有工作量和代码路径判断的潜力，不是帧率预测。

| 优先级 | 修复方向 | 预期收益及覆盖 | 实现难度与主要风险 |
| --- | --- | --- | --- |
| P0 | 附件免复制及相应 scratch 规划 | 潜力最高；减少实际命中快路径的逐图元复制、清零和映射，减轻整个 Welcome 队列 | 中高；当前候选仍需证明硬件反馈语义，必须保留格式和多重采样回退 |
| P0 | 混批内按容量连续编码 IB | 中到高；普通透明合成、渐变及路径混批均可能减少命令内存和页表工作 | 中；需要完整的操作检查点、空间不足回滚及跨 IB 地址修正 |
| P0 小补丁 | 去掉退休执行器额外的 1 ms 延后 | 小到中；直接减少 GPU 完成后到 present 的人为延迟，覆盖两种动画 | 低到中；保留异步执行、锁顺序、FIFO 和唤醒合并，不能同步递归执行 |
| P1 | 完善生产者相关准入，再减少 GPU 作业间 CPU 空档 | 中到高；减少无关索引等待和队列饥饿 | 中高到高；1.115 已实现前半部分，后半部分涉及完成、地址空间和计时槽生命周期 |
| P1 | 降低建表成本及合理扩大混批 | 中到高；每个小作业也要支付固定成本，直接影响 opacity 和 mask | 中高；先细分健康检查、分配、逐页核对和 TLBSYNC，不能把总成本都归给内存分配 |
| P2 | 原生通知驱动完成消费 | 中；减少轮询抖动及跨队列无效唤醒 | 中高；当前通知端口注册返回 Unsupported，需要实现实际通知传输 |
| P2 | GPU 到原生显示 fence | 中到高；减少 CPU 退休和呈现串行，降低错过显示机会的概率 | 高；需要原版 IOSurface/IOMFB ABI、生产者序号、错误传播及扫描生命周期证明 |

历史记录支持这个排序：GPU 执行区间约 **10.939 ms/作业**，建表约 **5.200 ms/作业**，完整 submit wrapper 约 **11.620 ms/调用**；索引内容快照仅约 **0.0129 ms/次**。轮询验证约 **1.580 ms/次**，退休约 **1.630 ms/作业**，其中地址空间退休约 **1.437 ms**。这些阶段存在嵌套、不同采样次数和 CPU/GPU 重叠，不能相加成为一帧耗时。[历史原始统计](/home/xxiao/Projects/MI6_iOS/build/gpu-runtime/round-119-metrics/performance.json)

这也意味着优化索引 memcpy、只后移少量对象清理、继续微调拷贝 texel 宽度，都不应占据第一优先级。现有记录里横向裁剪节约约 4.72% 的 payload，仍保留约 6.56 GB；附件读取的工作量与逐批固定成本更值得投入。GPU 区间包含复制、同步和状态切换，不能将其全部归为着色器算术成本。

## 首批补丁的具体边界

### 完成消费后立即异步安排退休

[QcomMTL 的 pump](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomMTL/QcomMTL.mm:2756) 在退休队首已经可运行时，仍用 `dispatch_after(...1 ms...)` 调用 `finishRetirements`。此前 [armRetirement](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomMTL/QcomMTL.mm:2819) 已经通过另一条约 1 ms 的定时链消费完成通知。因而这里存在额外的调度延后；实际延迟还取决于 GCD 调度，不能简单等同于固定 2 ms。

最小修复是将 **pump 中的退休调度**改为独立执行器上的 `dispatch_async`，保留 `_finishScheduled` 合并、`_executionLock`、`_orderLock`、blocked 检查和 FIFO。首次轮询暂时保留，背压的 2 ms 重试也保留为兜底。`tryLock` 失败时继续由持锁执行器解锁后的 pump 唤醒，避免自旋或丢失工作。

这个补丁可以提前处理，改动范围小，且不改变 GPU 命令和显示 ABI。它缩短的是已就绪工作的执行等待，不能单独解释或消除历史约 100 ms 的 Welcome 呈现间隔。原生 present 仍只能在真实 end token 与资源一致性发布后发生；Scheduled 的公开时机维持完整操作列表准入之后。

主机侧应验证：GPU 先完成或后完成、执行器被占用、连续唤醒、错误退休、带 presentation 的 FIFO 队首及回调恰好一次。普通 C++ CompletionWindow 测试不能替代 Objective-C 调度和锁的验证。

### 让免复制路径同时免除无用的背景空间

当前 [作业内存规划](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/QcomGPUNative.cpp:1237) 先按 framebufferLayout 申请完整 scratch；[编码器](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/AdrenoPipeline.h:605) 也先要求完整 scratch 足够，再判断 `context.inplace`。即使之后不发复制命令，完整背景区仍被分配、缓存命中后清零，并进入逐作业页表。

建议引入统一的内部 `DrawMemoryPlan`，区分普通绘制、inplace 和 snapshot 回退，供准入预检、实际分配、编码检查共同使用。格式有效性检查始终保留；inplace 判断应使用同一份特化后的 fragment program，以及已经核实的模式和权限。

对于整批都不需要背景快照的情况，保留独立的 **4 KiB 进度和 flush timestamp 页**即可。批中只要有回退操作，就按实际回退操作的最大需求保留背景区；不能看到一个 inplace draw 就把整批 scratch 缩小。`framebufferLayout` 的附件合法性校验与实际所需分配大小要分开，避免通过改变通用布局函数破坏回退路径。

以一个 pitch=4352、高度=1920 的合格单颜色附件为例，当前 scratch 为 **8,359,936 字节**；inplace-only 可缩至 **4,096 字节**，少清零和映射 **8,355,840 字节、2040 个 4 KiB 页**。这是该示例资源的确定差额，不是帧率增幅；实际 HOME 中间纹理也未确定为这个尺寸或格式。[完整 backing 映射](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/QcomGPUMapper.cpp:314) [scratch 清零](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/QcomGPUHardware.cpp:526)

当前 inplace 仅接受限定的单采样、单颜色 RGBA/BGRA8 UNorm/sRGB 三角形路径。R8、RG8、RGBA16Float、MSAA、MRT 等仍需回退；尤其不能因为 HOME 是小文字，就推断其 mask 已全部命中候选。该优化应沿用默认关闭的模式，离线先完成资源规划与命令验证，硬件反馈语义未闭合前不升级默认路径。

离线检查应覆盖：纯 inplace 只映射 4 KiB、inplace 与回退混批、所有不合格格式、溢出/配额、预检与实际需求一致、所有 flush 地址均落在保留页内、跨 IB 进度单调、错误回滚，以及回退仍按每图元保留先前结果。不能把逐图元复制直接改成每 draw 一次，也不能删除真实 flush/wait。

### 混批中的小操作共用 IB

[QcomGPUNative.cpp](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/QcomGPUNative.cpp:1307) 对 `i > 0 && job.commandBuffers` 无条件调用 `allocator.next`。每块 IB 为 **64 KiB**；16 个操作即使很小，也至少占用 **1 MiB** IB backing，并为它们建表。已有混批减少了 Family 作业数，但这部分内存成本仍按操作增加。

建议让已经解析和保留资源的操作优先追加到当前 IB，空间不足时才封口和换块。需要同时计算命令头与尾部上传的代码、常量、纹理描述符及对齐空间；不能仅按 PM4 指令字数判断剩余容量。适合先支持能完整放入一块 IB 的小操作，再沿用既有逐图元跨 IB 路径处理大 draw。

操作失败重试必须恢复尚未提交的命令、上传空间、状态和统计；只执行一次资源准入，避免重试增加 pending 引用或重复写入。换块后重建实际地址和边框表，flush sequence 跨块保持单调。`beginOperation` 目前只更新地址并清除 borderAddress，它本身不能替代完整的回滚接口。

16 个操作不一定能压进一块 IB，收益应按实际编码字节计算。准入预检目前对相关请求估计最多 **16 MiB IB**；live 计数使用实际分配量，所以这不代表固定只允许 4 个在途作业。第一步可以保留保守上界，等实际编码规划可证明后再收紧预算，防止在 Family 已消费 token 后才发现容量不足。

离线检查应把相同操作列表分别按旧分块和紧凑分块编码，比较命令的有效语义、资源地址、执行顺序和输出模型；覆盖恰好满块、代码和常量碰撞、跨块回滚、compute→draw 数据依赖、索引生产者、最大块数及错误传播。已有的简单同 IB 双 dispatch 测试提供局部依据，尚未覆盖这个完整改造。

## 后续流水化需要补齐的部分

GPU1.115 的生产者检查已经替代了候选模式下的无条件索引排空，不能重复当作全新待办。应先验证跨客户端的真实物理别名、只读共享、GPU 生成索引、CPU 冻结的 blit 参数、准入拒绝不消耗 token，以及失败和 close 路径。继续把整个 buffer 的缓存操作范围纳入依赖判断，不能只比较索引的逻辑字节区间。

真正的 GPU 执行仍受 [fActiveJob](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/QcomGPUHardware.cpp:950) 限制，且 [retireJobs](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/QcomGPUHardware.cpp:1062) 完成页表退休、资源一致性、释放及原生完成之后，才启动下一作业。因此“允许更多作业准入”与“消除 GPU 作业之间的 CPU 空档”是两项不同改造。

不能简单提前调用 dispatchNextJob 或删掉 active 检查。当前 [postBatch](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/AdrenoSubmit.h:299) 的 profiling 只有一组计时槽，要求 issued==completed；多作业提交需要按 sequence 保留独立计时和完成状态。私有 ASID 退休也须在自己的 GPU 访问结束后进行，不能清理仍在使用的树或在下一作业写同一 backing 时执行未经证明的 CPU 缓存处理。

建议先在保持真实完成和发布顺序的前提下，缩短准入持锁时间、减少不必要的作业数量，并评估可延后的纯 CPU 清理。历史退休成本约 88% 落在地址空间退休；仅后移对象记账和池操作的上限有限。随后再设计持续 ring 提交、按作业完成的发布队列及物理依赖调度，保持同队列顺序和原生时间线的退休前缀。

## 地址空间与完成通知的下一步

地址空间阶段应补充健康检查、表页分配/清零、固件映射、实际物理页核对、PTE 写入、发布与 TLBSYNC 的子阶段计时。现有 5.2 ms 总量不足以确定页分配是最大项；轮询验证本身约 1.58 ms 也说明硬件状态核对不可忽略。优先通过更少批次、少映射无用 scratch/IB 降低重复成本，再考虑安全退休表页的有界复用。表页复用不能省略逐页物理来源检查、清零、权限、ASID 隔离和 TLBSYNC。

完整事件唤醒也需要额外实现：[registerNotificationPort](/home/xxiao/Projects/MI6_iOS/drivers/gpu/QcomGPU/QcomGPUNative.cpp:735) 目前直接返回 Unsupported；[pollCompletions](/home/xxiao/Projects/MI6_iOS/tools/gpuctl/GPUConnection.cpp:185) 是读共享 IODataQueue。因此仅在 Metal 端换一个 dispatch source 不会自动获得内核完成事件。正确路线是建立真实通知端口与队列绑定、处理关闭和错误，再以通知唤醒唯一消费者，保留低频 watchdog 兜底及精确 FIFO end token 验证。通知只是唤醒信号，不能直接当作 GPU 完成。

显示 fence 最后衔接：允许提前排队的前提，是原生显示确实等待对应 surface 的生产者 fence，并在真实 GPU 完成和一致性发布后解除等待。[QcomDisplay](/home/xxiao/Projects/MI6_iOS/drivers/display/QcomDisplay/QcomDisplay.cpp:822) 当前等待 ReadyLayers==ChangedLayers，旧扫描面只在真实 DPU 完成后退休；这两项语义需要保留。仅提前调用 present 或清除 presentation barrier，可能使合成器看到未完成图像，也不能作为性能修复。

## 离线交付与流畅度判据

首批补丁可以分别保留开关和独立源文件哈希，先完成主机命令回放、边界测试、ASan/UBSan 及构建检查。一次只改变一个性能路径，保持默认、候选和回退可比较。硬件缓存、真实像素和呈现节奏由后续单独阶段验证，本轮不安排设备操作。

用于后续判断的记录应能连接 `提交等待 → GPU 执行 → 原生完成发布 → 用户态完成消费 → present → DPU 实显`，并区分 Hello 渐显、保持、渐隐、HOME 径向展开和淡出。分别记录实际命中的 fetch/回退格式、复制字节、IB 数和映射页数，才能判断收益来自哪里。只看平均作业数或包含长停留的整段更新率，会掩盖 0.6 秒 HOME 展开与 1 秒淡出里的跳帧。

60 Hz 下每次显示机会约相隔 16.67 ms。以上优化分别减少工作量、CPU 固定成本和呈现抖动；当前静态证据足以确定实施顺序，尚不能承诺其中任一补丁单独达到 60 fps。


