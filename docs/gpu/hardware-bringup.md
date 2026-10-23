# A540 真机启动与原生接管

## 当前离线进度与欢迎页停帧

当前实际镜像为Display1.0.11/GPU1.88/Metal0.77。round73已通过真实VBIF halt后建立
显示MMU上下文、GOP映射保留和内核IOMapper交接。原版CARenderer/Metal绘制的全屏
IOSurface以4352行跨度映射到新DPU IOVA `0xf0004000`，SwapWait返回0、direct=1、
mapper_available=1、quarantined=0；2,073,600个参考像素全部一致。直接呈现不复制像素。
当前仅验证该受限直接扫描路径，原生Welcome和动画流畅度仍未通过。

round70同启动的9份backboardd异常明确报告四参数newLinearTexture SPI缺失；Metal0.77
补齐原buffer视图后，round71完成22笔原生合成GPU作业且未发现同启动UI进程崩溃。
round73的早期快照记录SpringBoard主线程等待backboardd，后者RenderServer等待显示
线程、显示线程等待持有锁的EDR通知发送。较晚同启动快照中这条等待链已解除，不能
称为永久死锁；保持原生屏幕唤醒时合成器仍停在9笔已完成作业，两次实际扫描回读均为
黑帧、硬件VBL继续增长、原生队列为空，须继续追查Welcome动画激活和后续调度。
六段原生唤醒窗口的首尾相隔约387秒，VBL从10420增至33362，completed/displayed
仍为10/7；Apple RenderServer自己记录的frame/dirty frame均为7。keybag原生状态为
Disabled、无密码且已解锁；同启动未匹配到新的UI进程崩溃。已排除本时段GPU队列积压，
但尚不能区分场景/动画未激活与Metal在进入内核前失败，具体根因未确定。
原版CADisplayLink独立探针的fixed60和欢迎页80/120/120请求均获得60 Hz有效回调，
这不能替代SpringBoard自己的动画回调证据。原始记录在`round-73*`，镜像及源快照
记录为`build/gpu-runtime/ui-display-context-wb-source.json`。
详见[显示映射与生命周期](../display/README.md#共享缓冲区与直接扫描的边界)。

2026-10-18。以下为round69历史记录：GPU1.87 / Metal0.75 / Display1.0.7。
其后同一次启动的三个 DPU 抓帧均为 SHA256
`6e6a2b4e695d9df02a559ed667b4b2ca9a47aceab94610fc27fe14edd2e19ce6`，
1080×1920、黑底，只有右下角信息图标的1354个非黑像素；未捕获欢迎文字或彩色模糊背景。
硬件 VBL 从40624增至43825再至61910，原生 completed/displayed 保持17且队列空。
两次 GPU 统计均为 native37、compositor34笔已准入且完成，期间没有新提交。
这将“显示器仍刷新”和“系统继续生成动画帧”区分开，尚不能确定停止调度的原因。

完整原生 stackshot 已连续校验并解码：695728 bytes、278个任务、808条线程。
Setup、SpringBoard 和 backboardd 的主线程及渲染/显示线程在普通 Mach/CFRunLoop 等待；
ButtonHaptics 线程仍等锁，但快照没有证明其造成全部停帧。显示与目标电源状态均为1，
LCD preferredScale/pointScale3、1080×1920、60 Hz。原生 keybag disabled、无密码且
unlockedSinceBoot 为真；失败的屏锁 MIG 查询不用于推断锁屏状态。

只读提取的原版 SpringBoard 资源含55份本地化 CursiveHello.plist；英文有2组笔画，
简体中文有9组。原版 `SBDashBoardSetupView::_animateCursiveForTimePassed` 驱动
`SBCursiveTextView` 的 loadText:pointSize:/setTime:/duration 路径。欢迎页属于 SpringBoard，
不能用 Setup 进程已启动代替该动画链已工作，也没有用自绘欢迎图替换它。
静态入口和资源可读取不证明运行时已经成功加载或执行它们。
原版手写层实际为 CAShapeLayer；_createDisplayLink 请求80/120/120的帧率范围。
当前60 Hz输出是否正确满足原版高帧率请求仍未知，displayctl display-link 同时采集
fixed60和原版80/120/120两组回调及实际频率，不改变显示模式或注入软件时钟。

有序重启发生在用户要求离线之前，完整收到 initproc-exit panic 及20帧回溯，随后到固件
BDS；未证实 Fastboot 重新出现。电量耗尽后没有访问设备。
GPU1.88 / Metal0.76 / Display1.0.8 候选补齐原版 IOGPU 的用户整页共享导入及 Metal
bytesNoCopy，加入真实 CADisplayLink/原版 VBL 订阅者诊断；DPU仍使用CPU staging复制。
Kext、Metal 和74项集合已离线构建；新的用户分配进口与帧回调尚未真机验证。
记录为 `build/gpu-runtime/round-69-welcome-evidence.json` 与
`build/gpu-runtime/ui-shared-resources-source.json`。

## UI 接口审计的剩余项

| 通路 | 当前证据与缺口 |
| --- | --- |
| IOSurface → Metal/IOGPU → A540 | 原有 descriptor 共享与真实 fence/像素回读已通过；无需 CPU 颜色上传 |
| 外部用户分配 → MTLBuffer | bytesNoCopy已接入；round70原版QuartzCore已走到该buffer上的四参数线性纹理SPI，缺失异常由Metal0.77修复，round71同启动合成作业及崩溃回归通过 |
| A540 输出 → DPU | round73通过显示MMU初始化和原生IOSurface直接扫描；覆盖受限全屏线性BGRA布局，保留实际fence和front生命周期 |
| 原生动画时钟 | round70原版CADisplayLink两种请求均返回真实回调；系统欢迎动画仍未验证，无软件tick伪造 |
| 欢迎页效果 | 手写资源和原版 view/动画入口已定位；完整运行时 shader/纹理参数及效果链仍未捕获 |
| FP16 颜色中间纹理 | RGBA16Float/R16Float/RG16Float 尚未实现；可能影响部分效果，但未证实本帧请求这些格式 |
| Family/可选 SPI | 未实现的完整 Metal family 不声明支持；原生 frame-info/CRC 等扩展也未开放，不能仅凭 stub 判定为本次停帧原因 |

以下章节保留各轮历史结果；不能把旧轮次的黑帧、故障或成功结果移作当前轮次结论。

2026-10-17。round-55 已通过原版 Metal/IOGPU 的设备端JIT Compute、4096像素纯红Render、AOT Compute/颜色绘制/私有纹理上传与采样，以及原生IOSurface导入、GPU绘制和回读。系统合成器GPU作业仍为0，原生图层呈现等待未通过。USB修改Bash的XML与launchd缓存后，实际优先级由4升至31，连续两次重启EFI至可用Bash为24.032及24.213秒，此前约6分钟。**系统UI加速与动画流畅度仍未通过。**

round-45/46 的首次故障为 `FSR=0x80000402`（转换故障及 multiple），不含 permission fault；round-46 的64位 FAR 与分开的高低32位读回一致。最小 AOT `add` 的运行时代码哈希为 `0x331e8096`，与生成库完全一致；460个命令字、135个包和资源地址均已解码。Compute 仍未成功，Render/合成器 GPU 完成数为0。round-46 实际屏幕仍为文字控制台，不能把历史 round-41 的 Welcome 页面当成本轮加速证明。

GPU1.77 增加 `gpu-shader-direct=1`：沿用 Freedreno 的 SS4_DIRECT 命令，直接上传同一份 ISA，其他资源、地址空间和硬件执行仍沿用原生后端。round-48 已实测：内联128字机器码与 AOT 逐字一致，运行时 word-FNV 同为 `0x331e8096`，仍触发 `FSR=0x80000402`、`FAR=0x1f9d405523e00`。所以不能仅归因于间接代码上传。一次UART PM4包头损坏，完整包解码未成立；屏幕串流另缺两个base64字符，未生成或修补图像。`round-48-evidence.json` 区分这些证据边界。GPU1.78 增加 `gpu-fault-snapshot=1`：启动时准备内核专用8 KiB映射，仅在故障时 CP 空闲、非 stalled、实际 TTBR0 仍为内核根且平台租约有效时，经 CP crashdumper 读取六个固定 SP/HLSQ bank；CPU不直接读受保护的上下文寄存器。状态版本1的顺序为 `(0x35,0xe00,0x32)`、`(0x3f,0xec0,0x40)`、`(0x32,0xe780,0x62)`、`(0x34,0xef80,0x62)`、`(0x3e,0xe580,0x170)`、`(0x3c,0xed80,0x170)`，每项为 bank/基址/寄存器数。完整字流保存在 `GPUFaultHLSQSnapshot`，关键Compute寄存器同时发到UART。完成轮询上限10 ms，不改 TTBR、不清故障、不重新开放提交，失败时保留映射。该诊断已交叉编译，round-49 正在真机采集。原生 `gpuctl diagnose` 逐块输出完整字流和 FNV，供主机核对传输完整性。

1.75 镜像在测试前中止，1.76 从未启动；二者含不适合该安全平台的名义 SP/HLSQ CPU 读回，已标记为禁止重放。1.77起移除这些读回。原始镜像仅保留证据。

## 已取得的硬件证据

`build/gpu-runtime/round-33/` 保留本阶段首次完整通过的不可变 EFI 哈希、Fastboot 校验、原始串口和报告。`round-39/` 进一步记录原生驱动接管。

| 阶段 | 实际结果 |
| --- | --- |
| RPM | 独占通道接续，资源请求收到真实 ACK |
| 供电、时钟 | PM8005 GFX 896 mV 读回及 READY、CX/GX、Fabia/GCC/GPUCC、CRC、RBCCU/SP 通过 |
| SMMU | 非安全 bank 0 / SID 0 初始化、寄存器读回及上下文 TLB 同步通过 |
| 固件 | CP `ME_INIT` 完成；GPMU 返回 `0xbabeface` |
| ZAP | 原始签名镜像 PAS INIT、MEM_SETUP、AUTH 成功；CP 切换到非安全模式 |
| 提交 | GPU 写回真实 timestamp；Fill32、Copy32、Barrier、数据读回及整页边界检查通过 |
| 交接 | CP 停止、VBIF 排空和 TLB 同步完成后才发布交接属性 |
| 原生接管 | RPM/GCC/GPUCC/SPMI 租约、NoC、SCM 恢复、IOMapper 和固件固定通过；新内核 ring 初始化 Complete/Success |
| 私有地址空间 | 启动时真实 GPU 写入及读回自检通过；不代表非特权着色器隔离已验证 |
| 用户态 | round-41 原生设备查询、单次及两笔异步提交、真实 fence 和数据回读通过；累计 5 笔完成、5 次 IRQ、0 失败 |
| 系统 UI | 原生 QuartzCore 点亮显示，实际扫描输出为系统欢迎界面；合成器 GPU 提交仍为 0 |
| Metal | round-43 原版 `MTLCreateSystemDefaultDevice` 与显式工厂均返回 Adreno540；round-44 首个计算 Blit 进入硬件提交后触发故障，尚无成功像素回读 |

`hardware-debug.json`、各轮 `report.json` 和 `uart.bin` 位于上述调试目录。旧轮次中仅寄存器或时钟检查成功，不能替代最后的 DMA 证据。

## 本轮修正

- CPU 必须显式建立 GPU/SMMU/BIMC 的精确 MMIO 映射，固件原有映射并不覆盖整个 GPU。
- PM8005 GFX 供电须与 CX/MX 分开处理，交接后的原生 SPMI 保留并检查电压、模式和 READY，拒绝冲突写入。
- A540 的完整停止控制状态使用低位 `0x303`；旧 A530v1 snapshot 的高位掩码不会在这颗 GPU 上置起。停止检查同时要求 VBIF 的实际排空确认。
- `CB_FSR=0x400` 是 AArch64 FORMAT，实际 fault 位仍为 `0xc00001fe`。格式变化不再被误判为转换故障。
- 已停止的 CP 可置起 BUSY 汇总位。仅在停止和总线排空均已证明时允许这些汇总位，独立流水线忙状态仍阻止启动。
- 重新设置 `RB_CNTL` 会清零读指针，却留下已初始化的 PFP 取指状态，导致越过写指针。运行引擎现只接续同一已初始化队列；换到内核队列须经过共同的复位、固件初始化、SCM 恢复和新栅栏验证。
- 首个完整命令流先写入并发布，再释放 CP。直接检查 CP 错误状态，避免仅依赖被屏蔽的汇总中断。
- 本地 Kext 常量表原先受到链接器指针猜测扫描破坏，例如 GPUCC 的 `0x1030` 变成 `0x0674ea70`，导致全部根时钟读为 0。本地 Kext 现仅按显式 Mach-O 重定位记录修正指针；发布前逐字节核对其余常量。23 个 Kext、677,278 字节的核对通过，旧镜像会被拒绝。

PAS 的支持查询在本固件返回 0，但接口可用性查询及实际签名认证操作成功。与上游 Adreno MDT 装载路径一致，以真实 INIT/SETUP/AUTH 的结果为准；未直接写安全控制寄存器。

## Apple 框架与调试工具

保持 Apple 的 IOService 匹配、IOGPUFamily、IOMapper、IOMemoryDescriptor、IOSurface 和 Metal 插件接口。只读插件映像由原版 DiskImages、HFS 和 VFS 挂到 `/System/Library/Extensions`。原版 APFS 在这里的容器组合下拒绝第三个容器，因此辅助驱动映像使用 HFS；不修改 APFS 的检查逻辑，也不写 System 卷。

工具仍归入现有入口：`scripts/toolchain.py` 构建驱动与客户端，`tools/apfs-image` 的 `create-volume` 制作并逐文件读回验证驱动映像，`scripts/test-bridge.py` 统一启动、身份/CRC 检查和采集。`gpuctl diagnose` 在同一进程读取注册表和原生设备信息；`displayctl scanout-live` 导出真实扫描输出。

已有 DEVELOPMENT/debugger/明确可执行文件路径调试策略增加 GPU 的 type 1 和 memory-info type 33 类匹配，保留驱动的管理员或原始 backboardd entitlement 检查及其他 MACF 策略。此配置用于工程启动，不代表通用应用权限已完成。调试保留 GPU 的 kprintf 阶段日志，使用原生 SERIALMODE_NO_IOLOG 降低重复 IOLog 串口输出；系统日志仍保留。

后续交互命令使用 `scripts/uart-command.py --require-bash`，必须取得实际 root Bash 回复后才发送命令批次。启动采集在 panic 或固件恢复后停止继续输入。

round-41 为有时限的诊断启动：`wdt=-1 watchdog-capture-seconds=1800` 关闭原生用户态检查，由 Qualcomm 适配层单独保留真实 31 秒硬件 watchdog，窗口到期停止喂狗。默认不开启此模式。它只用于采集，不证明正常看门狗策略下的系统健康。正常策略镜像仍使用 `wdt=0x238`。

原先 PID 31 的拒绝已查明来自 `/usr/libexec/runningboardd`。不能将它归为 backboardd。原生用户态提交实测使用 root 工具；非管理员合成器角色尚待验证。

## 当前剩余工作

GPUCC 阻塞已由常量表重定位修正，原生用户态资源/队列已实测。单次提交到完成为 498.292 ms，两笔异步提交为 1030.698 ms，空闲后重测为 866.498 ms；这不是帧率。后续工具分开记录提交调用和通知等待时间，再定位延迟。

最新截图为 `build/gpu-runtime/round-41-screen.png`，1080×1920，系统欢迎界面；未将静态画面视为 GPU 或动画性能证据。Metal 0.55 在确认框架版本、limits 及 initializer ABI 后，让具体设备采用原生注册的 `MTLDeviceSPI`，并明确声明公共 `MTLDevice`，不再要求没有协议表的 `_MTLDevice` 提供继承。

round-41 有序重启日志确认 APFS 卸载及 PSCI reset 请求；随后 UART 停在固件交接，Fastboot 未重新枚举，原因未确定。用户确认没有远程电源/按键接口，并要求继续离线开发。GPU 1.65 增加 `systemWillShutdown`，复用已有 CP 停止、VBIF 排空及 SMMU 恢复路径，保留无法停止的 DMA 图；该新路径尚未真机验证。

GPU1.66 / Metal0.56 继续优化 blit：小块按实际 PM4容量批量提交，大块填充及带行跨度的对齐拷贝使用真实 A540计算着色器，等待原生结束事件后才复用参数。1 MiB连续拷贝的离线规划从1024笔缩为1笔；这不代表实测加速比，也未解决或重新测量单笔498.292 ms的延迟。详见 [批量拷贝与填充](metal.md#批量拷贝与填充)。

GPU1.67 / Metal0.57 继续移除非对齐 blit 的 CPU 像素读取和逐字往返，支持没有 CPU 映射的私有 IOSurface。内核冻结布局参数并按实际物理字节范围检查拷贝，Metal 与内核共用跨度判断。132个着色器及插件已交叉编译，生产 GLSL 的29组 CPU 执行检查通过；新代码未触碰真机，硬件实测结论仍以上述 round-41 为准。详见 [字节拷贝与参数冻结](metal.md#字节拷贝与参数冻结)。

Metal0.58 增加原版 MTLFence 接口、实际退休点与多个生产者等待，以及可暂停的队列执行；新增 metalctl fence-test 供设备恢复后一次核对共享串行目标、事件和 Blit/Compute/Render clear。该命令未执行。commitAndWaitUntilSubmitted 使用真实无资源 Barrier 标记，可能使合成器作业总数增长；这些标记不渲染系统像素，不能单独作为 UI 加速证据。已测硬件状态及本轮未访问设备的约束不变。

调试中临时替换的 Data 诊断工具仍待恢复，原始备份 `build/gpu-runtime/displayctl-device-original` 的 SHA-256 保持 `c193a3d42999f51673e33e077c9723a1084fbc89d015661ddc4e407ac78e1ba2`。设备恢复后需重新核验文件及 extent 所有权再回写；没有刷写固件或修改 GPT。


## 提交缓存与统计

GPU1.68 在原生提交及现有诊断提交中复用内核私有64 KiB IB，缓存上限16个（1 MiB），每次取用先清零。缓存继续持有原版 IODMACommand/IOMemoryDescriptor 的已准备映射；只有真实 timestamp 或已证明的 CP/VBIF 停止与私有地址空间退休后才归还，隔离失败保留整个 DMA 图。暖恢复保留缓存，正常关闭在 mapper.shutdown 前释放。额外的多 IB draw 缓冲区仍沿用原有释放路径。映射生命周期参考 [Apple IODMACommand 实现](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/Kernel/IODMACommand.cpp)。

只读统计通过标准 IORegistry 数据属性 GPUStatistics 发布，版本1、1424字节，不增加 UserClient selector，也不暴露地址。现有 gpuctl stats 单独解码，gpuctl registry/diagnose 同时显示。正常更新约每秒一次，停机/故障/隔离在停止定时器后即时发布最后的退休或失败计数；旧版内核没有此属性，工具不据此判定硬件成功。

native 与 compositor 分别记录 marker、transfer、compute、render 的真实投递/完成/失败次数。marker 只包含无像素工作；不可变内建 blit 同时属于 transfer 和 compute，类别不能相加当作作业总数。被准入但未投递的作业也可能失败，所以失败数不必小于投递数。compositor 仍根据实际任务权限判断；在角色识别前拒绝的请求仅计入 native 准入统计。渲染完成计数仍须结合实际 IOSurface/scanout 和画面核验，不能单独证明 UI 加速。

分段记录 preflight、IB 获取、编码、cache prepare、私有地址空间、原生 fence 准备、准入分发、准入总计、队列等待、硬件投递、轮询校验、观察到的执行区间、退休清理和 Family 完成发布。每段有样本数、最近/最长时间、原始总 tick 和反向时钟计数；tick 转换来自原生 absolutetime_to_nanoseconds。它们是 CPU mach_absolute_time 的观察，执行区间包含 IRQ/定时器与健康检查延迟，各段也有嵌套，不能相加或当成 GPU 硬件计时。准入总计覆盖 submitNative 函数内的记录区间，不等同于整个系统调用耗时。

缓存命中/分配次数和缓存大小供下一轮在单次启动中区分冷启动映射、热提交、页表/验证及原生通知开销；保留平台/SMMU 检查。现有 gpu-submit 检查只补充这份生产头文件的边界与计数逻辑，无新增测试文件或独立脚本。本阶段未访问真机，不能解释或宣称已降低 round-41 的498.292 ms延迟。构建记录见 build/gpu-runtime/submit-cache-offline.json。


GPU1.69 / Metal0.59 继续将普通渲染、计算及 blit 接到有界异步准入，四种内建 blit 参数全部冻结。通知消费者按原版 Family 的实际 end token 退休，完成缓冲区按 FIFO 发布；私有提交标记与公开 Scheduled 分离，容量/快照背压在 Family token/fence 前处理。索引读取、fence 更新、事件信号和显示保留真实退休条件。详见 [有界异步提交](metal.md#有界异步提交)与 build/gpu-runtime/async-submit-offline.json。此版本仍为离线开发，未访问设备，也没有新的延迟、UI 合成或动画数据。


GPU1.70 / Metal0.60 扩大原生资源命名空间，单作业64项引用与64位表面写标记，预算按实际IOVA窗口计算；4096项DMA账本改为有序扫描，仍保留guard和原有退休/隔离条件。重复IOSurface导入在私有树中按backing去重。现有gpuctl stats显示内存计账与映射，native-async-test改为64资源实测探针并显式打印规模；新探针未执行，旧轮次仍是单资源样本。详见 [资源预算](native-iogpu.md#原生资源规模与计账)及 build/gpu-runtime/native-memory-offline.json。系统UI GPU加速和动画状态不变，仍未证明。


### Buffer纹理视图的离线进展

Metal0.61 补上原生buffer到线性2D纹理的零副本视图和对齐查询，共用现有IOGPU资源，保留逻辑长度、offset、pitch和来源生命周期。已有blit-test追加CPU共享写入、GPU拷贝、Compute、采样渲染及guard回读，未在设备执行。GPU1.70、Shader36/JIT33和132入口registry保持不变；仍未证明系统Metal加载、UI GPU合成或动画改善。构建记录为 build/gpu-runtime/buffer-textures-offline.json。


### Heap子分配的离线进展

Metal0.62 接入automatic/placement资源heap，重用原生IOGPU backing及已有布局，贯通子资源的blit、Compute、索引、Render和纹理偏移。资源别名保留独立token，防止旧对象销毁释放新分配；现有blit-test追加完整backing guard回读及alias生命周期检查。GPU1.70、Shader36/JIT33和registry不变。该路径尚未真机运行，系统UI GPU合成、动画及提交延迟仍无新证明。记录见 build/gpu-runtime/heaps-offline.json。


### 有效像素行清屏

GPU1.71 / Metal0.63 为load-clear新增独立GPU行填充，保留padding和边缘字节，参数由内核校验并冻结在只读IB。新增133号AOT入口，blit layout能力版本2，需配套registry与插件。既有blit-test新增clear-only及packed格式guard回读，未在设备执行。真机结论仍限于round41原生CP提交，UI加速和动画均未证明。记录见 build/gpu-runtime/strided-clear-offline.json。


Metal0.64 离线补齐无保留引用的原生命令缓冲及 descriptor 入口，外部资源弱引用、内部上传保活和锁外释放已接入既有异步准入/退休。没有修改 GPU1.71 内核或 AOT registry；继续使用经过完整 collection 校验的冻结 kernelcache。现有 blit-test 编入普通/无保留生命周期及 heap 负载，未执行。构建及镜像记录为 build/gpu-runtime/references-offline.json。设备仍无远程电源接口，本轮未访问 UART、Fastboot 或其他设备；恢复 displayctl 的待办和 round41 结论继续保留。


Metal0.65 离线接入原版 MTLBinaryArchive 的 A540 Compute/Render 函数存储、原子序列化、载入及精确函数/管线查找。编译器/系统指纹和文件校验失配会拒绝，原生 JIT 注册及硬件作业生命周期不变。既有 gpu-submit 主机 codec 检查通过；既有 pipeline-test/jit-test 的重载与恢复后 GPU 回读仅编译。继续复用 GPU1.71 的完整校验 kernelcache，未访问设备、未恢复 displayctl，也无系统 UI/动画或编译耗时的新证据。记录为 build/gpu-runtime/archive-offline.json。


## 恢复真机调试：round42–44

用户重新连接设备后，round42 的原版 Metal 加载进入原生连接初始化，但在 client buffer 的缓存属性切换中触发 `phystokv_range` panic。回溯和反汇编确认 `pmap_sync_page_attributes_phys` 在32位页号移位后才转换到64位，截断4 GiB以上物理地址。源码改为先扩宽再移位，generic 与 SPTM 实现同步修正；目标只重编译 generic pmap.o。新内核的实际指令使用 `lsl x0`，完整74 Kext collection 校验通过。round43 随后完成系统 Metal 设备发现，未再 panic。证据为 `build/gpu-runtime/round-42-panic.json` 和 `round-43-evidence.json`。

round43 的两笔原生作业各引用64个资源，真实结束通知和回读通过，提交至退休675.710 ms，其中提交调用599.253 ms、通知等待76.443 ms；不能与round41的单资源样本直接作为性能改善比较，也不是帧率。此启动累计4笔完成、0失败，包括1笔标记和3笔transfer，Compute/Render及compositor均为0。新版IB缓存与统计接口已在实际设备读取。两次扫描输出均经PNG逐块CRC校验，内容是未变化的内核文本控制台；不是系统UI或动画证据。原始串口、命令退出状态及截图位于 `round-43/` 和 `round-43-screens.json`。

Metal0.66 修正原版 Compute 描述符惰性创建的空stage-input被误拒绝、Blit参数未按原生资源页对齐、JIT完整程序超过原8 KiB共享上传缓冲这三处实际阻塞。上传按4 KiB对齐至45,056字节，GPU1.72 的原生shmem单块上限64 KiB、数量上限仍32；能力通过只读IORegistry属性发布。空输入描述符按Apple原生 `isEqual:` 比较，非默认stage-in仍拒绝。nil函数在写入原版描述符之前返回错误。round44 已确认 Blit 越过参数检查并进入 Compute 硬件提交，但随后故障，累计2笔提交、1笔完成、1笔失败；此后AIR/JIT/Render探针均因设备不可用而退出，不能据此确认各管线修复的像素结果。

round43 有序重启已成功回到Fastboot。round44 使用相同已修复内核、GPU1.72 / Metal0.66 和相同有界诊断策略，已经记录SpringBoard通过实际签名角色检查并打开原生GPU连接。可选 `gpu-platform-timing=1` 仅打印首次平台检查的分段耗时；启动采样中secure约11.7 ms，仍需后续作业数据，不能当作稳定性能结论。

统一采集入口现逐条等待实际 `DEV_CONSOLE_RESULT` 回复并保存退出状态；`--stop-after-console` 在全部命令返回后结束，单命令超时或panic停止后续输入。新增 `gpuctl restore-bootstrap` 仅在原始备份、当前已安装内容、root所有权/普通文件/大小/模式等全部匹配后，通过原生VFS临时文件和原子rename恢复固定诊断路径，再读回校验；不使用旧物理extent。备份作为只读数据随HFS驱动映像携带，未额外信任为可执行文件。round44 已实际输出 `GPU_BOOTSTRAP_RESTORE vfs=1 bytes=131072 original readback verified`，原文件SHA256为 `c193a3d42999f51673e33e077c9723a1084fbc89d015661ddc4e407ac78e1ba2`，恢复待办已清除。旧APFS inode/extent记录仅作历史证据，不能再次用于写盘。

GPU1.73 在首次失败时记录具体SMMU不变量、FSR/FAR/FSYNR及上下文寄存器，先于中断状态清除和GPU停止；健康检查仍执行。`gpuctl kernel-log` 使用Apple原生libproc的非消费式 `proc_kmsgbuf` 快照，只导出GPU/SMMU行，不占用 `/dev/klog`。round45用于定位首次Compute故障；尚未放宽页表保护或宣称硬件修复。

round45 的 GPU1.73 / Metal0.66 复现首个 Blit 故障，首次快照为 `FSR=0x80000402`、`FAR=0x152c217b35ac0`、`FSYNR0=0`、`FSYNR1=0x1030001`、`TTBR0=0xffd18000`。FSR 表示多次转换故障，未置权限故障位；尚不能将捕获的异常地址归因为执行权限。GPU再次停止并保留相关映射，系统未panic。native CP回读仍通过，单次提交至退休429.936 ms；compositor作业仍0。原生libproc快照成功返回131,073字节，但没有GPU日志行，故障证据来自UART中的首次kprintf快照。此次显示关闭，scanout返回0xe00002d8，没有新增有效截图。证据见 `round-45-evidence.json`。

## 串口 Bash 启动时间

实际读回的 launchd 配置没有显式 Nice、LowPriorityIO 或后台 ProcessType；rc 只设置环境和提示。round-46 主机UART到达时间：26.606秒启动串口服务的xpcproxy，166.534秒同一PID完成setexec，227.959秒出现Bash提示，234.291秒验证命令返回。另一个已启动系统上的非交互Bash约0.807623秒启动。这些时间说明启动链存在较长等待，尚不能区分CPU调度竞争、缺页或IPC阻塞。`gpuctl shell-info` 使用实际getpriority和PROC_PIDTASKINFO只读查询父Bash的当前状态，输出Mach时间基准并明确 `startup_sample=0`；它不替代冷启动线程采样，也不修改调度策略。

## 后续硬件诊断与运行时供电

round-49 / GPU1.78 的 CP crashdumper 在100微秒内完成，1046字/4184字节通过目标与主机的FNV核对。HLSQ两套Compute上下文与PM4一致；SP三个bank全部返回0xffffffff，必须标为不可用，不能当作真实对象地址。

round-50 / GPU1.79 在首个原生CP传输前读到SP/RBCCU状态均为1（PWR_ON未置位）；直接唤醒后状态均为0x140000、控制0x778000，但该传输未完成，约20.375秒后收到固件重启日志。没有内核panic记录。1.79以及从未启动但继承该热切换代码的1.80镜像均禁止重放。当前1.81仅在可信CP状态中重申SP/RB/TPL1的64位地址模式，不默认进行运行时GPMU访问；round-51已完成对照，见下文。

可选 `gpu-domain-resume=1` 仍属未验证工程诊断：必须先证实ring空闲、旧fence完成、CP停止和VBIF排空，才进入供电回调；结束后接续同一ring，不重写RB_CNTL或重置fence序号。失败保留所有权并走停止/隔离。现有gpu-submit检查只追加了这组顺序、连续性及未收到排空ACK时不调用供电回调的检查，通过不代表真实供电模型已通过。

Bash的实际PROC_PIDTASKINFO已读到nice=0、任务优先级4、timeshare策略1；当前任务CPU用时约0.684秒，而本次内核到提示约353秒。4对应XNU的MAXPRI_THROTTLE，单纯nice读数不足以判断是否被后台策略限制。已在串口作业生成器和暂存plist中明确ProcessType=Interactive；目标尚未安装这一修改，完整慢启动原因仍待复测。

串口采集沿用现有test-bridge.py：安静UART也按截止时间重试root Bash探测；`--attach-running --running-report`只用于同一已启动系统的后续采集，不操作Fastboot，也不独立证明镜像身份。前轮若有panic、固件恢复或不同payload哈希，打开UART前即拒绝；检测到UEFI/Shell时停止输入。


## round-51：CP状态恢复对照与串口优先级复核

GPU1.81 / Metal0.66 实际启动，14条诊断命令全部返回，无panic或固件恢复。默认关闭运行时手动供电；仅通过可信CP重申SP/RB/TPL1的64位地址模式。原生CP填充/回读通过，CPU观察的提交至退休为370.975 ms；首个Compute仍出现FSR=0x80000402、FAR=0xf9dc05523e00、FSYNR1=0x2030001。4184字节的CP快照通过FNV核对，逐字节与round49相同，SP aperture仍全部为0xffffffff。地址模式恢复尚未解决着色器故障；UI合成GPU作业仍0，失败后的Blit、layer与像素检查明确跳过，不能记为通过。

本轮内核至Bash提示335.564秒，实际task priority仍为4、nice为0；提示之后采样的任务CPU累计约1.04548秒。低优先级已经复现，但尚无冷启动调度轨迹，不能把全部延迟归为调度。Apple公开的[launchd.plist手册](https://github.com/apple-oss-distributions/launchd/blob/main/man/launchd.plist.5)说明未设置ProcessType的作业受到CPU与I/O资源限制；交互式串口作业已在生成器和暂存plist设为Interactive。原生mount确认System卷只读，command -v与路径检查未发现launchctl/plutil/cp/mv；因此目标作业尚未替换，启动改善尚未验证。证据见shell-startup-evidence.json与round-51-shell-policy/uart.txt。


供电状态解释补充：KGSL `a5xx_is_sptp_idle()` 在启用SPTP自动掉电时，正是以SP/RBCCU的PWR_ON位清零作为空闲条件。因此round50提交前状态为1只能证明当时域已掉电，不能单独证明GPMU没有正确自动唤醒，也不能据此继续手动写供电寄存器。手动恢复仍默认关闭。下一步先由空Compute与单SSBO写入分离指令启动和资源访问，再核对CS shader装载与HLSQ更新顺序；审计见 `ui-compute-state-audit.json`。


## round-52：无资源Compute与用户态完成通知

GPU1.82 / Metal0.66 实际运行，内核至Bash提示379.2秒，nice=0、task priority=4仍复现。原生CP回读通过。设备端MSL JIT编译的probe_empty为32字、word-FNV262ae445，零SSBO、零常量、1个线程；内核统计在该探针后为Compute submitted=1/completed=1/failed=0，原生总作业2/2、failed tickets=0，硬件仍健康。Metal命令缓冲区却返回0xe00002bc，探针因此没有继续执行store。随后独立进程的AOT add再触发FSR80000402、FAR1f9d505523e00。两次Compute的310字与594字PM4均完整解码；不得把空内核的用户态错误误记为shader硬件失败，也不能据此宣称有像素回读。

1.83将Compute的HLSQ分配更新置于代码/描述符装载前，并移除装载后的重复更新，对齐fd5_compute.c；现有gpu-submit检查通过。Metal0.67在原生通知出队或CompletionWindow格式检查失败时打印实际条目字段，保留失败处理；shader-store-probe允许在新进程单独核对一个SSBO的写入和其余保护字。

## 19.2MHz计时转换

round52出现APFS负耗时与无符号下溢记录。源码与实际内核反汇编确认absolutetime_to_microtime先将19.2MHz整数除为19 tick/us：秒内余数最大会转换成1,010,526微秒，在秒边界倒退10,526微秒。现改为余数乘USEC_PER_SEC再除完整秒频率；保留RTClockData布局及之前的64位pmap物理地址修正。主机直接编译生产函数体，验证19.2/24/100MHz、低频、秒边界与最大输入。新内核f2b2d9ffe9bc9aa729340570d3a9b0b834c8f109e06a6a98d1aac4e2ef10369b已在round53启动，采集窗口未再观察到上述负耗时或无符号下溢记录，但Bash仍等待363.433秒。该缺陷已证实，不能据此宣称全部启动延迟或APFS问题已解决。

## round-53：Compute完成与原生通知区间

GPU1.83 / Metal0.67 的12条采集命令全部返回，无panic。JIT空Compute、独立单SSBO常量写入及AOT add在内核端均完成；最终Compute为3/3、failed=0，传输为1/1，Render与compositor仍0。原生CP回读通过。三个Metal进程都收到合法的开始token1，例如start=558067922968、end=558639014114、status=0、reserved=0；旧解析器却要求开始通知的endTime为0，返回0xe00002bc。原版Family反汇编确认提交包装器在调用硬件提交hook前后各取一次CPU时间，并把这个区间放入开始通知。不能将这个区间当作GPU结束，也不能将解析失败当作硬件shader故障。

Metal0.68按已准入token对区分开始与结束；开始记录不退休作业，只有合法FIFO结束记录才能退休。同一通知窗口贯穿异步与同步等待，避免消费后续记录时丢失状态。现有gpu-submit状态机检查增加两个实际非零开始区间断言并通过。GPU1.84/Metal0.68集合、驱动HFS与EFI已构建，真机验证待完成。另在原有metalctl中加入无SSBO的64×64纯红全屏三角形探针，真实Render完成后才校验4096个像素。round53截图205434字节，base64偏移连续且全部PNG CRC通过，画面仍为文字日志；不是系统UI加速证据。详见`round-53-evidence.json`、`native-start-notification-evidence.json`。

## USB安装串口Interactive策略

round53结束后有序重启至EDK2，使用现有USB存储导出器核验磁盘GUID和ios-apfs分区，再按当前APFS路径备份三个目标文件。XML及609个缓存作业中仅串口Bash增加ProcessType=Interactive；其他608个作业及缓存顶层配置保持语义一致。离线COW写入器因运行后产生的多段文件拒绝更新，未放宽限制；本次改为对重新解析并核验SHA的三个既有单段目标文件作等长数据更新，保留inode、大小、权限、归属、extent、GPT及APFS元数据。压缩缓存对象表的重复容器并保留合法padding，签名保留原分配长度；更新后通过O_DIRECT整段回读与重新打开APFS的逐文件回读。临时布局读取源码已删除，没有添加独立脚本。

设备缓存SHA256为91703643839a35f9a0cf7be28e1f12146beecdf5b8508cea717258113a624a25，detached CodeDirectory hash为651691f82cc08293dbcaddf08c9fa5431f6e8b62。新EFI显式信任该签名；旧镜像的信任缓存不能作为本次启动依据。USB导出器已完成两侧flush并返回固件。实际Bash任务优先级及提速效果待下一轮复测，证据位于`serial-interactive/transaction.json`。


## round-54：真实Compute回读及串口提速

GPU1.84 / Metal0.68，16条采集命令全部返回，无panic、超时或SMMU故障。64资源两笔异步CP提交与回读通过；两笔共用通知窗口，实际结束token均被消费。JIT空内核、JIT单SSBO常量0x1234abcd及其余15个保护字、AOT add的64个结果均通过。完成通知解析错误为0。最终native作业按重叠类别统计：transfer4/4、compute6/6、render2/2，失败均为0。不能把硬件Render完成数等同于像素正确：纯红探针首个像素仍为165,165,165,165，AOT颜色绘制也在pixel0失败；尚未证明整幅纹理都未改变。原生LCD帧计数为0，compositor所有类别为0。205302字节截图在移除明确插入的UART日志后，base64偏移及全部PNG CRC通过，画面仍为文字控制台。

新launchd缓存的cdhash被原版loader确认trusted并成功交给launchd。Bash实际nice0、priority31；主机UART时间测得内核至提示21.544秒、EFI至提示23.867秒、EFI至可用命令24.032秒。上一轮同一修正计时内核、旧作业策略时内核至提示363.433秒，优先级4。该对照证明本次策略修改已生效且本次等待显著缩短；启动耗时仍需考虑轮次负载差异。

GPU1.85的后续对照只改变图形FS/HLSQ装载顺序和有界诊断；保留已通过的Compute顺序、Metal0.68通知窗口与时钟修正。单IB Render纳入gpu-submit-trace的三次全局上限，输出实际PM4、图元、目标、viewport与寄存器编号；多IB附件读取不会输出错误的字流。临时缺失的构建信任输入已从实际启动过的ui-notify镜像handoff逐模块提取，并逐项核对build.json中的SHA；新镜像复用完全相同的已信任依赖，仅更新驱动工具模块。没有扩大信任范围。证据见round-54-evidence.json和ui-notify-trust-snapshot/receipt.json。


## round-55：Render、纹理和IOSurface真实回读

GPU1.85 / Metal0.68，保留已通过的Compute、通知解析与计时修正。12条采集命令均返回，无panic或命令超时；其中render-info第一次返回pending，工具按原有行为记录自身stackshot，不能算成功。JIT纯红三角形4096个像素全部为255,0,0,255，初始化值与其他像素均为0。AOT pipeline-test通过64值Compute、纯色sysmem绘制、私有纹理上传/采样和共享纹理回读；原生二进制归档恢复后的Compute与Render也实际执行。首笔Render的900字、378个PM4包完整解码，无缺口。证据保存在round-55-evidence.json及round-55-render-pm4.json。

成功路径使用gpu-shader-direct=1，未据此声称间接上传或生产默认策略已验证。手动GPMU域恢复仍关闭。本轮Bash优先级31、nice0，内核至提示21.713秒、EFI至可用命令24.213秒，与round54约24秒一致。

同一次启动在display-on之后追加采集，未重新引导：surface-test通过原生IOSurface导入、带偏移/行跨度的共享别名、GPU render/blit/sample、seed及CPU回读。backboardd PID65、SpringBoard PID32均运行，LastExitStatus为0；原生stackshot成功捕获十个目标，并由Apple源码kcdata.py解析。backboardd渲染服务线程优先级55、主显示线程54，SpringBoard主线程47；这些线程在Mach消息/RunLoop等待中，没有着色器故障或低优先级4的渲染线程证据。不能把普通后台工作线程的priority4当作UI主线程优先级。

首次图层上下文valid=1、displayId1、commitId1，但waitForRendering返回0；合成器提交仍0。主轮截图148353字节、全部PNG CRC通过，内容为诊断文字。追加采集末尾scanout返回0xe00002d8，未生成或补造截图。另安排屏幕on与原生图层有效期重叠的即时采集，继续区分显示调度与硬件后端。主轮图片在图层销毁后读取，不能用于证明图层有效时的显示内容。原始证据和栈位于round-55-ui/，不增加独立脚本。


同次启动的round-55-present再次请求屏幕on，并在原生context valid=1、commitId1已记录之后获取scanout。硬件VBL计数7779，源地址始终0x9d400000，native completed/displayed/pending/active/queued均0；图层waitForRendering仍返回0，合成器GPU提交仍0。148353字节PNG通过全部CRC，SHA692b997d86ecf3b7e14cbe891f0af3b89dc38c549f5baeba175dc31e85ab3420，与先前截图逐字相同，仍为诊断文字。该证据证明硬件VBL在运行，尚未证明系统渲染请求或swap已进入后端。记录见round-55-present-evidence.json。

round-56 沿用GPU1.85/Metal0.68，增加QcomDisplay1.0.1的display-submit-trace=1：已有takeDiagnostic上限内的原生prepare/验证/开始/完成日志可走kprintf，默认仍为IOLog；必须同时启用display-scanout-capture。持续VBL及逐帧日志不改为UART输出。gpuctl kernel-log display保留管理员和非消费式libproc读取，检查整个返回缓冲区的NUL跨度，限定显示相关行并输出命中/非零字节统计；默认GPU过滤和旧bytes汇总保留。交叉编译与Kext ABI检查通过，实际设备已返回6行原生显示启动日志。4CPU证据通过，Bash优先级31，EFI至可用命令24.330秒。主机重启后采集进程已不在，只有前两条命令返回的原始前缀，未生成完整report；display-on之后的swap-test和UI呈现不能算执行或通过。原始前缀没有panic(cpu标记，也不能据此推断采集中断后的设备状态，见round-56-evidence.json。

现有metalctl新增ca-render-probe，通过公开CARenderer.rendererWithMTLTexture:options:及kCARendererMetalCommandQueue直接使用原版QuartzCore的Metal渲染器和着色器库。它绘制64×64的红色根CALayer及32×32绿色子层，经同一Metal队列GPU拷贝到共享buffer后检查全部4096个BGRA像素，并观察原生Render计数。IORegistry统计约每秒发布，探针有界等待采样刷新；这些计数属于设备全局，可能包含并发客户端。没有调用项目自己的合成入口，也未增加独立测试脚本或文件。创建、begin、render、end及回读前均输出阶段，以便定位原版框架失败的位置。即使离屏探针通过，也不表示backboardd、系统屏幕呈现或动画已通过。

ui-native-renderer-bridge已完成arm64交叉编译和打包；只替换上一版卷中的metalctl，内核、GPU/Metal插件、显示驱动和Shader registry保持同一份已记录输入。已有gpu-platform-timing=1诊断在下一版启用，用于一次性拆分平台控制器、NoC和安全状态检查的耗时。round-55-ui的最后一笔CPU观测显示准入约299ms、poll校验约50ms、回收约78ms；这些区间包含软件检查和调度，不能当作A540着色器执行时间或帧率。见ui-cpu-timing-evidence.json。镜像与原版框架引用、卷内容、5个可执行文件的信任缓存已核对，见ui-native-renderer-verification.json。当时UART探测仍收到乱码/大量NUL；用户随后重启，通信恢复，后续实际运行结果见下文。

## round-57：串口恢复、Setup和原生显示提交

用户重启后，CH340的115200串口、EDK2 Fastboot及产品身份重新验证，GPU1.85 / Metal0.68 / QcomDisplay1.0.1成功启动，4CPU证据通过。EFI至可用Bash为24.587秒，实际priority31、nice0。追加同次启动采样确认Setup PID250、budd PID251；SpringBoard和backboardd均运行。16条主轮命令均返回，CARenderer探针状态1，其余15条状态0；无panic或命令超时。

原版CARenderer创建阶段向QcomMTLTexture发送protectionOptions时抛出未识别selector异常，尚未进入GPU绘制。依据原版Metal的无符号64位资源、Heap和命令缓冲接口，Metal0.69补齐无保护资源的getter及命令缓冲setter，拒绝非零保护请求，保留真实未保护内存语义。没有宣称实现受保护内容或完整Apple GPU family。

displayctl的原生IOMFB swap-test实际通过验证、prepare、queue、硬件完成与SwapWait，sequence9 completed/displayed均为9。DPU选择的scanout回读已变为红底白条，PNG全部CRC通过，SHA652e256f981af2bfd3b1643d32c28117f6ac0cc422b4b20d91e5ffbcc360d6ef。图案由CPU CoreGraphics绘制，只证明普通线性BGRA的原生显示提交通路；不能作为UI GPU加速或光学拍摄证据。系统另请求0x26424741，即SDK定义的Apple lossless BGRA格式，row1088；驱动明确拒绝，不能将其解释为普通BGRA。见round-57-evidence.json、round-57-screens.json。

## round-58：Metal保护接口通过，原版渲染仍未绘制

GPU1.85 / Metal0.69 / QcomDisplay1.0.1启动，EFI至可用Bash24.148秒、priority31、4CPU验证通过。十条主轮命令及三条追加日志命令均返回，无panic或命令超时；两次CARenderer探针均状态1，不能将采集脚本退出0等同于全部探针通过。原版renderer已依次经过create/begin/render/end/readback，未再发生保护接口异常，但4096个像素全部保持0xa5，Render仍0。原生GPU纹理回读拷贝完成，未伪造绘制结果。

使用原版QuartzCore的QUARTZCORE_LOG_FILE=stderr获取框架日志，记录Apple原生二进制归档不兼容；尚未证明此可回退路径就是跳过绘制的原因。后续探针将图层关联置于显式CATransaction提交内，并在无应用RunLoop的命令行环境中flush，额外记录updateBounds。该调整只影响现有诊断工具。

原生屏幕状态达到on，远端context valid=1，waitForRendering返回1；DPU帧仍是文字日志，PNG217439字节、2265个base64分块、全部CRC通过，SHA dd35c5845bd3425f4b13cc99a914a192797da5e227359fe37bdd82ff27c9604d。只移除23段语法明确的交错UART日志，原始字节完整保留。Apple压缩BGRA的swap仍在stage304被拒绝，合成器GPU提交仍0。当前两次ps采样有SpringBoard32、backboardd65，没有Setup，不能把round57的PID当作本轮状态，也不能据此推断Setup从未启动。见round-58-evidence.json及round-58-ca-log/。

原版QuartzCore在0x1844b8b70读取IOMFBMaxSrcPixels字典，并从该字典读取IOMFBCompressionSupport和IOMFBMaxCompressedSizeInBytes；缺失前者时保留默认能力。QcomDisplay1.0.2将两者明确置0并放入正确字典，保留对压缩布局的严格拒绝。已有单Kext交叉编译和ABI核对通过；新集合复用其余全部冻结Kext、同一内核和provider，74项集合验证通过。硬件效果待下一轮验证，不能预先宣称已解决UI呈现。审计见ui-compression-negotiation-native.json。

## round-59：原版图层提交生效，继续补齐任务归属接口

GPU1.85 / Metal0.69 / Display1.0.2实际启动，4CPU和priority31通过，EFI至可用Bash24.157秒。将根图层关联放入显式CATransaction并flush后，原版CARenderer的updateBounds达到0,0,64,64，进入render时明确抛出setResponsibleTaskIDs:count:缺失接口异常。原生调度marker实际完成1笔，Render仍0；不再将未提交图层的空帧当作驱动绘制结果。

本轮日志未观察到此前的压缩BGRA表面拒绝，但不能据此宣称所有显示格式已适配。另观察到kind6/7、changed0、零矩形的原生显示请求被严格拒绝，其语义尚待核对。原生on状态达到1，DPU帧仍是文字日志。PNG202315字节、2108个base64分块、全部CRC通过；SHA5ee781379e50fc5903741bad26a10b3c4725dfc4f89d40fbd15db4a91ea12273，仅移除13段语法明确的UART日志。截图命令在240秒期限之后才返回0，脚本已按原有策略停止后续输入；主机在完整回包和Bash提示出现后中断采集，剩余统计及ps在同一次启动追加取得。不能将本轮记为没有命令超时。无内核panic或固件恢复。

后续ps记录backboardd365、SpringBoard367、budd276，显示服务PID已改变，没有Setup进程。历史CrashReporter中有WATCHDOG监控超时记录；设备各轮墙钟不一致，尚未把这些文件与本次启动关联，不能直接归因当前重启。详细证据见round-59-evidence.json。

原版Metal的setResponsibleTaskIDs:count:使用const uint32_t*和uint32_t，要求无活动encoder，按流顺序复制DWORD列表；不是64位task指针。GPU1.86 / Metal0.70把这类提示作为有界顺序操作，复制到私有命令版本4的对齐前缀，再冻结到内核作业ticket。最多64项，null清空，不改变原生设备任务或资源权限。成功像素作业退休后才发布GPUCompletedResponsibleTaskIDs，保留原版Family通知、真实硬件fence及作业隔离；系统能耗计费仍未接入，并明确发布false。新集合只更新冻结基线中的QcomGPU和既有Display1.0.2，74项检查通过，Shader36/JIT33及registry a74b60151e63e259保持一致。新Metal归档context为e1d63b6aa30d47e9de220d19342d193d6aa41e6bface22fcc992b692d623dbf5，按实际新源码重新生成。见ui-task-attribution-native.json及ui-task-attribution-source.json，硬件验证进行中。

## round-60：私有命令版本4及基础Render通过，原版AIR特化失败

GPU1.86 / Metal0.70 / Display1.0.2真机启动，4CPU、priority31、EFI至可用Bash24.784秒。主轮9条命令均返回，无内核panic、命令超时或固件恢复；CARenderer探针状态134，其余8条状态0。原版渲染不再因任务归属selector缺失而异常，updateBounds为0,0,64,64；随后fixed_frag_lph_cpf、spec=Xfc两次报告AIR specialization: is_function_constant_defined does not name a function constant，QuartzCore终止进程。此阶段没有GPU Render提交，不能将主采集退出0记为原版渲染通过。未重复截取文字日志；最近完整DPU图像仍为round59。

追加同次启动复用两个现有后端探针，三条命令全部状态0。64资源两笔异步提交、不可变命令快照及两个真实硬件结束token通过，CPU观察的提交至退休753.139ms，其中submit调用676.272ms、通知等待76.850ms；这些数值不是A540着色器执行时间。独立纯红Render探针4096像素全为255,0,0,255，未变及其他像素均0。最终native transfer2/2、render1/1、failed0；compositor仍全0。新私有版本4的空任务前缀传输及真实像素退休已验证，非空任务列表的硬件传输尚未验证，系统能耗计费仍为false。来源见round-60-evidence.json。

离线从原版QuartzCore库提取同名96048字节AIR，SHA bc740d8121dbe021a7699afe9af691b2c6ad64cf17c124f743af54ff7a6cf261。反射59项函数常量，其中23项required；现有主机前端用明确的最小值可成功特化。因此尚不能把真机报错归为已经离线复现的通用编译器缺陷，也未修改编译器掩盖未知predicate。下一步需取得目标实际AIR及常量变体、失败operand的符号/类型和定义列表，再比较目标LLVM行为。证据位于ui-air-specialization/evidence.json。系统UI加速与动画流畅度均未通过。

## round-61：定位原版AIR的typed-pointer常量别名

GPU1.86 / Metal0.71 / Display1.0.2的诊断镜像真机启动，4CPU、priority31通过，EFI至可用Bash24.313秒。9条命令全部返回，无内核panic、命令超时或固件恢复；CARenderer仍为134，其余8条为0。首次引导尝试在固件初始化时过早发送RETURN，未取得Shell提示；该次尚未执行新EFI。随后从已确认的Fastboot传输加载并核对CRC，失败尝试单独保留。

实际失败参数为`getelementptr (<4 x i8>, <4 x i8> addrspace(2)* @_Z17fc_color_opcodes0.MTL_FC_INIT_57_Dv4_h, i64 0, i64 0)`。目标typed-pointer LLVM保留向量首元素的地址，主机opaque-pointer LLVM将其折叠为全局变量；原先root()只消除cast，导致同一个常量的零偏移别名被拒绝。失败日志的定义表确认index57确实存在。原版日志限制了长错误长度，末尾AIR哈希和实际常量值未被完整保留，不能宣称已取得同一变体的全部输入。见round-61-evidence.json及ui-air-specialization/native-intrinsic-failure.txt。

Metal0.72新增独立predicateRoot()，仅消除有界的常量全零GEP，再按实际LLVM声明对象匹配；非零内部地址、未知对象及过深指针图仍报错。原版AIR最小值特化通过，刻意传入index1内部地址的查询被拒绝；此主机检查不能代替目标typed-pointer路径验证。失败诊断将AIR SHA256、大小及实际常量值置于错误前部，避免再次因原版日志截断而丢失。现有displayctl layer-test增加180秒进程期限，包括60秒图层保留窗口，超时明确退出124。没有新增独立测试脚本。新镜像复用同一GPU集合、Shader registry和Mesa后端，按实际新源码重新生成归档context，硬件验证进行中。

本轮远端图层waitForRendering返回0，随后正常结束。最终ps有backboardd354、SpringBoard356和budd280，没有Setup；不据此推断Setup从未启动。系统UI加速仍未验证，因此跳过重复的文字屏截图。

为验证Metal0.72而进行后续有序重启时，固件输出停在BDS的`Battery prfile BATTERY.PROVISION1, platform type 30`。追加20秒被动UART采集为0字节，单独发送RETURN后15秒无回应；指定edk2-fastboot1的product查询12秒超时，对应USB serial未枚举。此时新EFI尚未执行，不能把固件停顿归因于新编译器，也未确定电池或其他固件原因。设备没有已知远程电源/按键接口，已请求手动重启。0.72源码、HFS文件、5个受信任可执行文件、信任缓存与EFI的SHA均重新核对；新EFI SHA d905c5a121580d854bf13c710089a37f8b7065e8b490a4274566a59013500074。round62仅准备命令，未执行。详见hardware-debug.json的post_round61_recovery。

随后离线开发期间再次查询，edk2-fastboot1返回product sagit，UART收到固件自报`UEFI Total : 475620 ms`，不再需要等待手动重启。该时长属于本次固件初始化自报，不能归为Bash或XNU用户态启动耗时；停顿原因仍未确定。恢复证据为ui-default-templates-recovery-check.json及ui-default-templates-passive-uart.bin。round62随后开始加载下述配套镜像，尚待实际结果。

## 原生默认事务初始化

原版IOMobileFramebufferLegacy的slot307、0xfffffff005c22304，先初始化对象+0x130处的1300字节事务模板，将changed/ready置为0xffffffff，再复制到+0x644。之后才根据+0x1898标志与非零尺寸决定是否分配Apple的PurpleGfxMem。另查明请求+0x32c是原版Swap Intent，由用户提交+0x17c复制；内部函数0xfffffff005c268f4在没有显式事务时，为intent6/7选择这两份模板。Qcom此前覆盖了slot307而只创建自己的默认IOSurface，未执行父类的模板初始化；这与日志中的零掩码相符，具体轮次归因仍待硬件确认。

Display1.0.3在framebuffer设置尚未就绪时调用原版父类初始化，核对两份模板的四个掩码，再继续普通IOSurface、IRQ和PM注册；成功后才置原生framebuffer标志。没有本地重写事务payload，也未进入Apple硬件内存区分配分支。原版初始化的104字节入口、模板写入及分支保护已加入apple-mobile-graphics-abi.json的构建核对。单Kext交叉编译、368槽ABI和74项独立集合检查均通过。GPU1.86、Metal0.72、Shader registry及用户卷保持同一份冻结输入，只替换显示Kext。

该修正尚未改变非零intent的严格拒绝，也不证明UI已呈现或动画性能通过。原版父类会根据intent处理排队顺序、KeepOnScreen、有效IOSurface与完成状态，不能把6/7直接当成已显示的空操作。完整原生分析记录位于ui-display-default-audit/native-functions.json，新镜像和来源见ui-default-templates-source.json。

## round-62：进入XNU前的GLINK握手拒绝

新EFI实际执行并通过传输CRC，显示交接也通过；GPU桥接在resource-acks阶段返回Invalid1，外层返回-11，尚未进入XNU。因此Metal0.72、Display1.0.3、原版CARenderer及新事务初始化均未完成硬件验证，9条Bash采集命令未执行。31秒独立watchdog随后将设备带回固件，串口记录36次SBL Charging in progress、SDP源、Vbatt原始值范围3042至3299，随后进入UEFI/BDS。电池及充电信息不证明它导致了GLINK解析错误。前轮中断后主机采集进程已不存在，原始UART与部分命令日志保留，最终report未生成。

37字RPM诊断明确显示localAck未建立、remoteID3、messageID/expectedID0、replyBytes0；最后header0x00010004、param12，即发给本地channel1的OPEN_ACK，尚无资源投票消息。旧解析器在case4同时要求channel1及param0，由此提前拒绝握手。现有固定Linux v6.12 qcom_glink_native.c的OPEN_ACK接收分派只传本地channel ID，未解释尾部param2；本地参考SHA0238242a1620174c083aceefa45f165913214ae3be84476e45e5caed47239466已核对。参见[上游源码](https://github.com/torvalds/linux/blob/v6.12/drivers/rpmsg/qcom_glink_native.c)及round-62-rpm-failure.json。

RPM1.14在内核和桥接共用解析器中按本地channel识别OPEN_ACK，保留真实收到的尾部字供本次启动诊断。继承已连接通道的内核对象不会声称自己见过此帧；原有Debug记录及handoff布局未改变。错误channel、资源ACK消息ID、长度、拒绝和超时校验保留。在原有power_test.cpp中回放word12和错误channel，既有分片、陈旧消息及真实资源ACK模型检查通过，没有新增测试文件。RPM Kext已交叉编译，并连同两份逐项核对的冻结本地父依赖通过导入/ABI检查；新74项集合通过，用户卷、Metal及Shader registry保持相同输入。配套ui-rpm-open-ack桥接镜像已构建，尚未启动。用户随后要求设备充电期间先离线开发，本阶段未再查询或操作设备。

## 离线原生控制事务桥接

Display1.0.4继续使用原版事务初始化、IOSurface解析与引用管理、pending队列、准备栅栏和完成函数。固定默认IOSurface分配后，按原版swap_submit读取的字段，为默认事务模板填入其ID及物理全屏源/目标矩形。intent6仅接受这份默认surface、线性BGRA及全屏几何；intent7仅接受四层全清除、无raw/effective surface且显式背景为零的黑帧。0xffffffff掩码只对这两个已审计内部intent解释，普通intent0的未知位拒绝保持不变。

黑帧实际写入DPU扫描缓冲区，再执行现有地址切换与flush/命令传输；普通与默认surface仍沿现有CPU复制到扫描区的后端。只有真实传输中断并确认地址锁存，或视频模式真实VBL后，才调用原版swap_complete_gated；超时仍为未显示。原版对intent6/7不更新普通LastCompleted/LastDisplayed序号的行为保持原样，原生状态、引用释放及CommandWake由Apple完成函数处理。

现有display主机套件、单Kext arm64编译、原生368槽ABI及74项集合检查通过，只在原测试文件补充必要的接纳边界检查。未新增独立脚本。冻结候选为ui-control-swaps，包含GPU1.86、Metal0.72、Display1.0.4和RPM1.14。默认surface的原生映射前提、系统产生这些控制事务的实际路径、显示切换和UI合成均未做目标验证；其他非零intent和空帧非零背景色仍不支持。源码及原版指令证据见docs/display/native-queue-audit.json与build/gpu-runtime/ui-control-swaps-audit/native-functions.json。

## round63：原生渲染通过，UI纹理9失败与输入锁竞争

实际版本GPU1.86/Metal0.73/Display1.0.4/RPM1.14进入四核XNU，EFI到Bash24.243秒；主采集中未见panic或SMMU故障。原版CARenderer红底/绿色子图层4096像素通过，系统合成器5笔Render和2笔transfer也完成。IOMFB实际完成至swap7；intent6/7已沿硬件通路完成。

1080×1920、scale3同时由原版BKS和MobileGestalt读回，逻辑尺寸按比例为360×640。最终扫描PNG只有中央20×20像素的灰色转圈；用户未确认实屏显示。行跨度为扫描4320、IOSurface4352，读取时按真实各自行跨度处理。不得将64×64离屏探针或缓冲区PNG解释为物理显示尺寸错误或面板已经亮屏。

同次开机继续采集得到backboardd PID630、bootSessionUUID7250550C-B293-42E8-98FF-A13647CCD54B的COREANIMATION/SIGABRT，原版fixed_frag_lph_cpf报unsupported texture metadata，含原始AIR哈希与59项常量。该变体需要噪声纹理槽9。新GPU1.87/Metal0.74的逻辑绑定映射及两个数值修复已通过生产编译检查和普通/MSAA后端比对；74项集合结构检查通过。新候选`ui-air-bindings-bridge`临时不启用可选Home固件，保留Retina配置。

原生系统stackshot共711280字节；导出的42240字节前缀含完整kernel_task容器及93个线程，使用Apple kcdata解析器的--incomplete选项解码，未伪造结尾或其他任务。FPC线程408累计约1092秒CPU时间，在SCM action中运行；GPU线程3234等待其SCM锁，较早backboardd主线程5849等待其HID工作队列锁。单次采样不证明永久死锁。

原有display-state诊断因原生IPC等待而由本任务中断，Bash恢复；该次跟进的原始report缺失，保留console-progress和中断收据，不能将未完成命令标为通过。新客户端对display-state增加10秒期限，并增加原生IORegistry背光状态读取。layer-test实际超时124，原生渲染等待为0；主脚本index7的0只代表扫描读取成功。完整证据见`build/gpu-runtime/round-63-evidence.json`。

## round64：原生资源回收调用导致合成服务退出

GPU1.87/Metal0.74进入四核XNU，EFI到Bash22.564秒，到已验证Bash22.707秒。
原版CARenderer的4096像素再次通过；系统合成器1笔Render、1笔transfer和1笔marker
完成，transfer同时计入compute，不重复相加。普通显示完成/显示序号最终均为11。
三个完整PNG为启动日志、黑帧和原生唤醒后的黑帧；后两者逐字节一致，RGB非零像素
为0。不能将swap完成或驱动背光开启状态视为物理UI显示证据。

原版BKS、MobileGestalt和CADisplay仍为1080×1920、scale3，CADisplay另报60Hz。
第一次display-on在读回on后触发客户端10秒期限，exit124；跟进的同类请求exit0。
中间一次扫描返回0xe00002d8，后续原生唤醒和扫描成功。每条命令的真实结果都保留，
采集脚本exit0不代表所有诊断都通过。没有调整Retina参数。

完整backboardd PID300崩溃属于bootSessionUUID E77DC3A4-3246-48DB-9345-38E923C4BDA3，
加载的Metal插件和编译器UUID与冻结0.74镜像及稍后的原生任务栈一致。
lastExceptionBacktrace从Core Animation空闲回收定时器进入
`QcomMTLBuffer.setPurgeableState:`；旧代码对合法Volatile/Empty请求主动抛异常，
随后SIGABRT退出。另一条primary线程停在GLSL/NIR编译过程，这是同时采样到的
线程，不能归为编译失败或malloc故障。按文件名末尾选出的两份旧watchdog前缀
已经从本轮结论排除，后续使用完整元数据及镜像UUID匹配。

Metal0.75已补齐保留DMA backing的原子资源状态转换、heap/IOSurface所有权语义
和提交状态检查，现有ca-render-probe增加对应调用。新诊断工具在shell-info中直接
读取kern.bootsessionuuid。交叉构建和新HFS/信任缓存已完成，内核仍为冻结GPU1.87、
SCM1.0.1，暂不携带Home固件。另行编译的SCM1.0.2锁拆分未混入此对照镜像。
第64轮有序重启约12.5秒已看到UEFI banner，随后启动第65轮0.75候选；本轮无需
再次手动复位。完整证据见`build/gpu-runtime/round-64-evidence.json`。

## round65：回收异常修复通过，GPU图层仍输出黑帧

Metal0.75实际通过新增的资源状态调用及原版CARenderer的4096像素回读，四核与
定时/IPI正常，EFI到Bash22.331秒。直接读回的本次启动UUID为
19CBABF0-5E68-4A09-8DC2-789CC17ADF2D；据此筛选backboardd崩溃报告，观察窗口内
为0，原生job仍为PID65。不能仅凭脚本exit0判断图层展示：首次layer-test的渲染等待
为0，缓存预热后的同一原生图层等待为1，两次进程均exit0。实际系统合成器累计
5笔Render、3笔transfer和3笔marker完成，失败为0，compute中的3笔与transfer重叠。

原生统计主LCD已产生6个dirty frame，但四份UI扫描PNG都是同一张全黑图，所有
PNG分块CRC均验证。随后通过原版IOMFB提交CoreGraphics绘制的1080×1920参考帧，
普通token11完成并显示；实际DPU扫描PNG含2052000个RGB(204,13,13)红像素和
21600个白像素，白条位置x270..809/y940..979完全符合预期。它证明当前显示传输能
覆盖完整物理raster；该参考图由CPU绘制，不能算GPU/UI渲染通过或光学输出确认。
scale仍为3，源IOSurface与扫描stride分别为4352、4320。

既有metalctl ca-render-probe现在可传width/height和可选iosurface模式，用同一
原生CARenderer在全屏尺寸比较普通backing与IOSurface；使用堆上sentinel、真实
padding行跨度及首个差异像素的直接纹理读取，没有新增测试文件或独立脚本。
第66轮镜像只有metalctl二进制与第65轮不同，GPU/Metal/内核逐字节沿用。
第65轮有序重启出现initproc退出的panic开头，原报告因提前停止未包含完整panic；
补采确认initproc exited，USB已恢复edk2-fastboot1/product sagit，随后直接启动第66轮。
完整证据见`build/gpu-runtime/round-65-evidence.json`。

## round66：全屏原生CARenderer与IOSurface回读通过

沿用GPU1.87/Metal0.75/Display1.0.4/SCM1.0.1，仅更新metalctl尺寸诊断。
四核定时/IPI通过，EFI到Bash22.614秒；直接查询的启动UUID为
89B33F9B-BAAD-4FC3-B8AD-85429D47EDA0。普通纹理与导入IOSurface均采用
1080×1920、row4352，原版CARenderer绘制红底与绿色子图层，每种模式的
2073600个像素全部通过GPU blit回读，各完成两笔Render，无未改写像素。
两种模式均执行对应的资源状态调用；IOSurface状态由原版IOSurface拥有。

这些结果确认全尺寸离屏GPU输出，包括源纹理的行跨度；尚未独立比较显示端CPU
读取的同一表面。本轮主采集scanout-live返回0xe00002d8、命令exit1；补采原生唤醒
成功后，实际扫描PNG仍为与第65轮一致的全黑帧，completed/displayed均为6。
不能归因scale或声称UI已显示。未在此采集执行当前启动的崩溃报告筛选。
Display1.0.5补上CPU复制源表面前的原版IOMemoryDescriptor缓存同步，交叉构建通过，
尚未打入该镜像或真机验证，不能将黑帧原因归于缓存。
完整证据见`build/gpu-runtime/round-66-evidence.json`。当前scale3及1080×1920保持；
小米6与iPhone7/8的官方屏幕参数、逻辑点和缩放区别见`../display/README.md`。

## round67：源缓存同步对照与稳定黑帧

本轮只把Display1.0.4换成1.0.5，GPU1.87、Metal0.75、用户镜像及scale3保持不变。
四核定时/IPI通过，EFI到Bash22.417秒，启动UUID为
4F1CBDD9-2B0A-4236-9216-D0EE5F523CDE。首次原生图层等待为0，扫描帧含695210个
RGB(204,13,13)红像素，仅位于y1275..1919；其余像素全黑，没有预期白条。
该帧completed2/displayed1，不能证明完整图层或判定局部红色的原因。

预热重放的原生等待为1，实际扫描completed/displayed17仍全黑。最终合成器接受19笔、
拒绝0，9笔Render均完成、失败0。按直接读取的启动UUID筛选backboardd报告为0，
原生job仍为PID65。缓存同步未解决稳定UI黑帧，不能把等待返回或作业完成当作展示。
主采集一条诊断误用了目标不存在的/bin/cat、/bin/sleep，exit127；随后通过Bash内建
命令完整导出旧日志，再进行预热重放，补采各命令均exit0。原始错误记录保留。
完整证据见`build/gpu-runtime/round-67-evidence.json`。

后续Display1.0.6保留缓存同步，仅增加显式诊断启动下最多16次的复制源ID、采样像素
和耗时记录；常规日志仍限4次。现有displayctl增加swap-gpu：原版CARenderer在Metal
IOSurface上画红底白条，真实队列完成后交给原版IOMFB并读取DPU扫描，保留原版
显示控制对象及表面直到采集/等待结束，不以CPU填充或离屏回读替代显示结果。
本段只记录已编译的诊断能力，目标验证待进行；不新增测试脚本。

统一test-bridge.py在已附着的系统出现panic时立即停止输入，并继续被动采集15秒，
避免在UART半行处丢失panic主体。第66轮有序重启因此取得initproc exited的完整
原生报告及20个回溯帧，随后验证Fastboot/product sagit恢复；该异常属于请求重启
阶段，与此前运行期GPU结果分别记录。

## round68：原生GPU到DPU的完整图案通过

GPU1.87/Metal0.75/Display1.0.6进入四核XNU，EFI到Bash22.097秒。
启动UUID为C2A81A5A-B252-48DE-924E-D413C37C9EF8。现有displayctl swap-gpu调用
原版CARenderer/Metal，两个真实Render在IOSurface2上绘制红底白条；没有CPU颜色
填充。原版IOMFB token1等待0，扫描completed/displayed均为1，源row4352正确
转换为扫描row4320。DPU PNG含2052000个RGB(204,13,13)像素及21600个白像素，
与第65轮CPU参考图逐字节相同，完整2073600像素及全部PNG分块CRC均通过。
这证明原生框架的GPU表面到显示传输路径，仍非光学确认或完整系统UI通过。

系统合成器累计25笔Render、4笔transfer、3笔marker完成，拒绝/失败为0；compute
计数中的4笔与transfer重叠。随后远端图层等待0，没有新增GPU绘制，实际扫描为
全黑帧。复制源采样显示原生UI表面RGB为0、部分alpha非零，而GPU参考帧的源
采样含正确红白值。不能把这一观察扩大为所有像素或特定着色器错误的证明。

同轮直接GPU表面复制总耗时约31.7ms，原生UI表面部分约316～337ms，普通默认
表面约31ms；原生timebase625/12用于换算。这些值含准备、缓存同步及逐行描述符
读取/完成，且处于显式诊断启动，不能当作单独memcpy耗时或正常策略帧率。
但它们已超过60Hz的16.7ms预算，需要继续优化显示传输。

完整原生backboardd/SpringBoard stackshot分别11360/13008字节并由Apple kcdata
解析器解码；主线程、render-server及primary display线程采样时均等待Mach消息，
不能据此证明所有锁/事件均健康。按实际启动UUID筛选backboardd崩溃报告为0。
主采集与补采所有命令exit0，无观察到运行期内核panic。原始数据及解释范围见
`build/gpu-runtime/round-68-evidence.json`。

Display1.0.7在原版内存描述符prepare/缓存同步后创建只读内核映射，以原有缓存
属性逐行复制可见像素，并保持不同行跨度和padding；无法映射时仍检查readBytes
返回值。映射在complete之前释放，不保留额外IOSurface或常驻映射。显式诊断中
进一步拆分prepare、sync、map、read时间。交叉构建和74项集合校验通过；后续
对照复用相同GPU、Metal、用户镜像及scale3，关闭已采完的GPU PM4输出。

## round69：映射读取通过，CPU传输仍超出帧预算

Display1.0.7使用原版描述符的只读映射，实际采到的各次source copy均为read=map，
未触发描述符回退；GPU/Metal、用户镜像及scale3与第68轮相同。四核定时/IPI通过，
EFI到Bash22.552秒，启动UUID D236E210-5C0F-4176-9DF5-78734D570925。
swap-gpu再次取得完整红底白条DPU帧，2073600像素与已验证参考逐字节相同，
token1完成/显示。主采集18条命令均exit0，没有观察到运行期内核panic。

直接GPU图案的复制总计24.577ms，其中read23.499ms；原生UI表面的总计为
307.087～333.305ms，中位313.398ms。prepare、sync、map各阶段均低于1ms，
主要耗时在实际CPU读取/复制，而非本次采样中的建图或缓存同步。计时包含抢占，
且是一次显式诊断启动，不据此声明稳定帧率或受控性能提升。

原生合成器34笔接受、0拒绝；27笔Render、4笔transfer及3笔marker完成，compute
与transfer重叠。远端图层将CA_SYSTEM_SESSION_CLIENT提前到exec环境后，等待仍为0，
扫描completed16/displayed13为黑帧，图层并未通过。没有在本轮重新采集崩溃报告，
第68轮的0报告结论不能移用为本轮结论。见`build/gpu-runtime/round-69-evidence.json`。

后续应把全帧传输交给GPU/DMA或直接扫描，保留原版IOGPU资源生命周期、真实完成
栅栏与IOMFB队列；在复制完成前不得触发DPU flush/显示完成，错误或未退休DMA不得
释放引用。CPU映射优化未达到60Hz预算，不能作为最终显示后端。原生UI黑色内容
需继续核对真实UIKit点几何、原生上下文提交及实际绘制资源，不能以已通过的独立
GPU图案替代系统UI和动画验证。
