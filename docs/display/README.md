# SDM835 LCD / MIPI DSI

启动 Logo 资源与 OnBoardingKit 未知屏幕模板回退的设备补丁、签名及启动验证见
[2026-10-21 真机记录](system-ui-device-patches-20261021.md)。

逻辑熄屏与唤醒首帧的固件链路及 Display 1.0.23 候选修复见
[逻辑唤醒时序分析](logical-wake-review-1.0.23.md)。已离线修复合并 PP_DONE/VBL
回调中完成路径污染刷新时间戳的问题，主机回归和 arm64 构建通过；尚未进行本轮真机验收。

## Setup 语言页顶部地球偏小：离线定位（2026-10-20）

用户确认范围为首次设置语言选择页顶部的地球。原版 `BuddyLanguageController`
在 `viewDidLoad` 的 `0x100098898` 创建 `Language.ca` 动画，经 OnBoardingKit
`OBAnimationView` 显示。
该资源使用 CAShapeLayer 矢量路径，根画布 864×300，地球外径约 74×3=222 个
画布单位。此次未修改固件、显示驱动、资源或设备状态，未做真机测试。

已找到能够解释小地球的完整原版代码路径：

1. 当前移植配置为 1080×1920、scale 3、429 PPI。它未命中 MobileGestalt 的
   固定屏幕分类表；10 月 19 日两份已保存记录均为
   `main-screen-class=(none)`、`DeviceClass=iPhone`。
2. `OBDevice` 的 once 初始化 `0x19b94f3ac` 按 `DeviceClass` 选择手机，再按
   `main-screen-class` 设置设备模板。缺失答案的默认 0 和不支持的 -1 均跳过
   模板赋值，保留零初始化的 `templateType=0`。
3. 语言控制器的父类实现 `OBSetupAssistantSupport`；原版
   `OBBaseWelcomeController.viewDidLoad` 将控制器模板设为 2，该值传入 header。
   此控制器模板与上一步全局 `OBDevice.templateType` 是两种不同的模板。
4. header 模板 1～3 的 `iconHeight` 会调用 `_setupAssistantIconHeight`
   (`0x19b956930`)。该函数读取全局设备模板；有效模板的高度是 60/80/85 点，
   模板 0 则返回 **0**。`animationView` 以此高度建立 `preferredFrame`。
5. `OBAnimationView.defaultScale` 用 `preferredFrame.height / rootLayer.size.height`
   计算基础缩放。Setup 的 `setStandardBuddyScaleForAnimation` 随后加上 `1/15`；
   未分类屏幕的 `BYDevice.size` 为 0，不进入额外增量减半的 size=2 分支。

由原资源和原版公式得到的几何预测如下，不是实际屏幕测量：

| 设备模板提供的高度 | 最终动画缩放 | 地球外径（逻辑点） |
| --- | --- | --- |
| 0 | 0/300 + 1/15 | 约 14.8 |
| 60 | 60/300 + 1/15 | 约 59.2 |
| 80 | 80/300 + 1/15 | 约 74.0 |

因此分类缺失时，地球仍可能可见，但只剩正常 80 点模板下约 20% 的外径。
这条定位优先指向**屏幕分类到 Setup 设备模板的适配缺口**。同一模板函数也被
其他 Setup 页使用，但此次没有测量其他页面或把桌面 56/60 点图标规格套用于地球。

已在无网络、无设备透传的 QEMU 中执行未经修改的原版 OBDevice 初始化、
图标高度函数及原表，9 组输入全部通过，覆盖缺失/未知分类、正常手机分类、
宽窄屏与 iPad 分支。外部 MG 答案、UIScreen bounds 和 Objective-C 对象操作
由明确的环境替身提供；这验证给定输入下的原版决策，不是完整 Setup 渲染验证。
原版 Setup 文件与本地 System 固件逐字节一致，OnBoardingKit 原始段哈希也已核对。

复现：`python3 build/ui-icon-scale/20261020/globe_native_check.py`。
汇总、函数地址与哈希见 `build/ui-icon-scale/20261020/globe-analysis.json`，
原版代码执行记录见同目录 `globe-native-verification.json`、`globe-qemu.log`。
本次对当前设备原因的归因仍依据历史分类记录与离线链路；未新读 Setup 进程的
全局模板、preferredFrame 或扫描像素，也未实施修复。

### 不改框架指令的替代方案

本版 Setup 的 Info.plist 与已追踪的 `OBDevice` 初始化路径没有提供设备模板或
图标高度偏好开关。另有两种离线候选，均未写入固件或设备：

1. **MobileGestalt 缓存分类覆盖**。原版从
   `/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist`
   加载缓存。`CacheExtra["oBbtJ8x+s1q0OkaiocPuog"]` 是 24 字节 Data；
   末尾 offset 20 的 little-endian int32 是屏幕分类，前 20 字节包含实际宽、高、
   PPI、scale 和方向。覆盖分类为 9，原版 OBDevice 会选择模板 4、图标高度 80 pt；
   BYDevice.size 为 1，按本页公式预测地球外径约 74 pt。此分类是兼容枚举，
   并不意味着实际 429 PPI 屏幕命中了原生分类表。

   修改时还要保持缓存一致：此 **22H374 / MG UUID
   beed77de987a347eada988634a6be56a** 的 `CacheData` 长度为 5922 字节，
   分类问题 `fdh+s6j3VijuyrK7xLjd7g` 在索引 429，值区 offset 3432
   存放 int32，validity 区对应 offset 5693。需要更新该缓存值或使其失效后
   从屏幕记录重新取值，不能只在 plist 顶层添加 `main-screen-class`，也不能
   把数值答案随意放进对象类型的 CacheExtra。保留 CacheVersion、CacheUUID、
   其他条目和类型；该布局不能跨版本套用。覆盖会影响所有读取该分类的进程，
   缓存重建也会丢失修改，已初始化的进程不会自动刷新。当前只静态核对了原版
   加载器和 getter，未执行完整缓存加载或生成实际缓存补丁。

2. **仅放大 Language.ca 图形**。`main.caml` 的 `Globe`、`Globe 1` 两层
   初始变换均为 `scale(3, 3, 1)`；改为 `scale(15, 15, 1)` 会将图形放大五倍。
   在当前推断的高度为 0 路径下，几何预测由 14.8 pt 变为 74 pt，但 header
   仍不预留正常高度，可能重叠或裁切，也不修复其他 Setup 图标；分类恢复后
   该资源会过大。只调整 index.xml 的 documentHeight 无法消除零高度来源。

缓存字段、原版字节哈希与反汇编见
`build/ui-icon-scale/20261020/mobilegestalt-config-evidence.json` 和同目录
`config-*.asm`。以上为配置/资源替代方案的离线分析，不代表已验证画面结果。

## iPhone 壁纸配置补丁（2026-10-19）

已通过 USB UFS 存储导出将 9 份配置补丁写入真机，并同步本地
`build/apfs/ios.apfs`。启动后，原版 WallpaperKit 实际枚举出 4 个集合、18 张唯一
壁纸：iPadOS 17（7570）、Lake（7560）、Stripe（7 张）和 Bokeh（9 张）；
默认选择 7570，不再回退到 400 Solid Black。已验证资源选择，尚未验证面板上的
壁纸裁剪和动态渲染。

`scripts/prepare-wallpaper-overlay.py` 从原始 plist 提取目录生成配置：

- 新增 `DefaultWallpapers~iphone.plist`，同时提供通用默认项和 `iPhoneMi6,1` 机型项。
- 新增 `Collections~iphone.plist`，仅列出现有 4 个唯一集合 ID，移除 3 个无资源引用。
- 将 Lake / iPadOS 17 的 `logicalScreenClass` 由 `810w-1080h@2x~ipad` 改为原生
  通用匹配值 `any`，同时限定 `productTypes = [iPhoneMi6,1]`；对应集合限定为 iPhone。
- 清空旧的 iPad 默认、集合顺序及旧版壁纸查找配置，保留原集合、壁纸 ID 和路径。

机型覆盖选择、屏幕类别和缩放模式偏移由原生代码分别处理。已核对
`WKDefaultWallpaperManager.deviceProductType`、`deviceColor/deviceEnclosureColor`、
`WKWallpaperBundle.shouldLoadWallpaperBundleAtURL:` 的产品/屏幕类别过滤，以及
`WKWallpaperAdjustmentTraitOffset.offsetForCurrentScreenScaleModeWithInterfaceOrientation:`。
`screenScaleModeZoomed/Standard/MoreSpace` 不是全局显示 scale；补丁保留已有偏移，
不添加缩放系数。原动画 3176×3176 画布、状态变换、HEIC/CAML 和素材引用均未修改。

同轮原生 BKS 返回 1080×1920、scale 3；360×640 pt 只是据此换算的参考几何。
独立 `wallpaperctl` 中的 UIKit `UIScreen` 仍返回 0×0、scale 1，对应
`0w-0h@1x~iphone`。暖机、切换 mobile UID 以及调用原生 `UIApplicationInitialize`
均未消除该现象。尚未定位具体缺失的显示配置环节，也未测量 SpringBoard 自身的
UIScreen，不能将此工具的结果直接外推到所有 UI 进程。
首轮严格匹配 `360w-640h@3x~iphone` 时，原生筛选只留下 16 张，默认回退为 400；
后续使用 `any` 加精确产品限制后，原生返回全部 18 张和默认 7570。此兼容配置没有
修复 UIKit 显示尺寸，不作为视觉缩放通过的依据。只读工具支持 `--mobile` 和
`--require-screen`；后者在屏幕无效时返回 8，普通枚举仍输出显式屏幕不可用标记。
构建入口为 `python3 scripts/toolchain.py build-wallpaper-client`。

增量工具已增加保留其他原生卷树的 System 路径复制，见
[APFS System 补丁流程](../apfs/README.md#保留原生卷的-system-文件补丁)。旧全卷导入器
因 Data/Update 的多 extent 和稀疏数据流拒绝当前真机；新路径不导入重建这些卷。
两次真机事务分别修改 400、382 个块，均通过带本项目原生元数据兼容补丁的完整
独立 apfsck。经 USB 提交后绕过主机缓存读回所有修改块，最终 9 份配置与预期
SHA-256 相符，其余 88 个原始壁纸文件全部匹配；五个非 System 卷超级块保持原样。
System/Data UUID、卷组和 fstab 已核对。未生成完整容器副本，设备写入均通过 USB。
配置回归 5 项、增量工具 Python 回归 9 项及 Go 回归全部通过。

USB 退出前主机和设备均确认刷盘；后续原生四核启动及 WallpaperKit 枚举通过，
采集期间未检测到 panic。该轮 `screen-info` 的 BKS 查询成功，但后续 MobileGestalt
查询超出 worker 期限，整条命令返回 1；`wallpaperctl --mobile` 返回 0，完整结果
按此记录，没有将 screen-info 整体记为通过。设备当前运行原生 XNU。

当前证据汇总为 `build/wallpaper/usb-system-update-20261019/verification.json`，
包括完整校验、USB 事务、九份文件和 88 份素材读回、原始 UART 与原生枚举。
最终配置为同目录 `final-overlay/`；最初分析、原文件及本地旧事务位于
`build/wallpaper/runtime-20261019/`，其汇总已标为历史记录。
旧基线 delta 不能用于当前真机；每次后续补丁都要从当前离线导出重新规划。
系统启动后不能只恢复旧块零来回滚，因为旧块可能已被重新分配。

## 最初 Hello 页的背景选择（2026-10-19）

后续用户指出 Hello 页本应有背景：下述“隐藏普通壁纸”仅涉及一个组件，不能作为整个
Hello 背景缺失的解释。上一轮收尾结论已撤回，正在继续核对真正的背景提供者。

本轮限定为最初“你好 / Hello”页，用户要求保留原生策略，仅修复实际故障；渲染问题
不在修复范围。已确认普通默认壁纸与该页的背景显示不是同一个选择条件。
真机原生 `_os_feature_enabled_impl("SpringBoard", "CursiveHello")` 返回 true。
原版 `SBDashBoardSetupViewController aggregateAppearance:` 首先生成
`CSComponent.wallpaper.hidden(YES)`；对应指令 `0x1d26826f0` 为 `MOV W2,#1`，
随后调用原生 `hidden:`。CursiveHello 分支另外添加背景样式 5，保留原有文字与图层流程。
`CSCoverSheetViewController _updateWallpaperEffectView` 对隐藏组件执行
`setWallpaperEffectView:nil`，可见分支才调用 `createCoverSheetWallpaperView`。
因此，Hello 页没有显示此前已识别的默认 7570，不能作为壁纸配置识别失败的证据。
本轮没有修改这些原生分支、设备类别、屏幕尺寸或显示格式。

原生文字路径读取 SpringBoard 框架本地化目录中的 `CursiveHello.plist`，System 中
相应资源存在。另行检查 HelloPoster：框架和 26,088 字节的 `default.metallib` 可读，
原生 Metal 库加载成功，包含 background/text 的四个 vertex/fragment 函数；其扩展
`UIDeviceFamily` 同时包含 1、2。它们的存在不能证明 HelloPoster 正在承担当前页面。
root 进程的 LSPlugInKitProxy 查询没有返回该扩展，也不能据此认定 mobile 注册表损坏。

一次诊断曾错误地把 252 当作颜色格式；SDK 实际定义为 `MTLPixelFormatDepth32Float`，
HelloPoster 初始化设置的是深度附件。诊断用共享存储纹理的拒绝由探针自身产生，
不等同于 Hello 进程的请求，已明确排除出故障判断。没有据此修改 QcomMTL，
也没有将“存在渲染故障”作为本轮已证实的结论。两次实时扫描捕获返回 unavailable，
本轮没有取得新的 Hello 画面；CA 帧计数也不能替代背景呈现验证。

当前已定位非渲染初始化故障：原生统一日志记录 PosterBoard 从数据仓库版本 59 迁移至
61 失败，随后无法创建数据仓库；SpringBoard 反复取得 nil poster style state。
空的 v59 目录不能直接视为旧数据损坏，因为原生新建流程本来就从最低版本 59 开始。
迁移校验要求每个目录存在、类型正确，并满足可读和 `NSURLFileProtectionNone`。
当前未启用内容保护的 Data 卷不返回 `NSURLFileProtectionKey`；真机直接调用
PosterBoard 的 `pbf_URLIsReachableAndConformToResourceValues:error:`，在 root 和
mobile 身份下均复现：原生两项要求返回 false，只保留可读要求返回 true，错误对象为空。
这与迁移最终给出的未知原因错误一致。只读 SQLite 测试成功与该目录属性校验是不同阶段。

已通过 USB 安装明确限定无 SEP、legacy 未保护 Data 开发环境的兼容补丁：
`scripts/poster_store_policy.py` 修改固定 22H374 PosterFoundation 中 None 资源期望字典，
保留 `NSURLIsReadableKey=YES`，省略不被该卷提供的保护属性要求。目录存在、类型、
创建权限与保护参数、其他受保护策略均不修改，不伪造加密状态。补丁重签缓存 `.54` 并更新
嵌套 Cryptex 的文件哈希和密封树；既有 keybag 缓存改动以设备现状为基线保留。
该策略不能用于启用内容保护的正式系统。原生 Hello 策略、显示尺寸和渲染器保持原样。

USB 事务 XID 51836 → 51837 修改外层容器 390 个块（1,597,440 字节），
其中嵌套 Cryptex 修改 10 个块。完整独立 apfsck 通过；390 个块经 O_DIRECT 读回全部
匹配，主机和设备均确认刷盘，GPT 未修改，非 System 卷超级块保留。来源为 USB 当次
设备镜像，不使用本地旧 Cryptex 覆盖既有 keybag 改动。新原生启动四核验证通过，
采集期间无 panic；WallpaperKit 仍返回 4 个集合、18 张唯一壁纸、默认 7570。
原生日志证实版本 59 → 60 → 61 迁移成功，数据库、WAL 建立，扩展环境继续初始化。

用户确认安装后、原生唤醒和电源键重新唤醒后，Hello 背景仍纯黑。本轮另已定位实际
绘制故障：原生 MercuryPosterExtension 报
`MercuryPosterExtension.WallpaperCoordinatorError.unavailable`，并反复报告缺少绘制协调器。
原版扩展 `DrawingController` 在 `0x100026a58` 先设置黑色底层背景，再创建绘制协调器。
`WallpaperRenderingCoordinator` 的 `0x10002f070` 检查共享 Resources；为空时进入
`unavailable` 分支（`0x10002f32c`），不会建立绘制层。Resources 初始化函数
`0x10003a230` 只有在 `MTLCreateSystemDefaultDevice`（`0x10003a334`）或
`newCommandQueue`（`0x10003a348`）未取得对象时返回 nil；该返回值被 once 单例缓存。
因此剩余故障属于 Metal 绘制资源初始化，当前未细分设备与队列两者。该分支早于
WallpaperMetalView 的建立，不将独立工具的 UIScreen 0×0 外推为此处故障原因。
遵照用户明确范围，本轮不修复渲染器、Metal/GPU 驱动或这一失败分支。
Hello 策略保留，不能宣称黑底已修复。

后续用户另行要求只分析 Mercury 绘制初始化的修复方案、不做真机测试。
静态核对发现实际打包 GPU 驱动把系统显示客户端权限组合用于原生 Metal/JIT 准入，
固件内 Mercury 不具备该组合；非管理员连接若到达该检查必被拒绝。
修复方案和证据界限见 [Mercury 静态复核](../gpu/native-iogpu.md#mercury-初始化失败的静态复核2026-10-19)。
此后续分析没有修改驱动或固件，也没有连接设备，尚不能宣称首次运行时失败点或完整画面已修复。

原生错误行的 Unicode 标点在 UART 中含控制字节；证据保留原始字节、行偏移与 SHA-256，
只使用完整未变的 ASCII 错误枚举判定，不将该行计为标准有效 JSON 或重建标点。
详见 `mercury-render-failure-evidence.json`；这来自实际扩展，区别于前述错误格式的旧探针。
原生 `SBSUndimScreen` 请求返回 0 仅表示请求已发出；另一个 hold-display-on 诊断因
原打包 displayctl 不支持该命令返回 2，没有建立保亮租约，也不作为系统故障。

当前配套启动包为
`build/setup-background/20261019/poster-store-runtime-v2/bridge/XnuBridge.efi`，设备已启动原生
XNU；相对首个兼容包，仅增加只读日志筛选能力，所有原有驱动、工具与内核保持原样。
后续重建须保留原 trust cache，并追加
`--trust-cache build/setup-background/20261019/poster-store-policy/trust-cache.bin`；
旧壁纸任务 final-runtime 不含新增缓存签名，不能直接作为修改后设备的启动包。
`build/apfs/ios.apfs` 内 Cryptex 不是本轮设备来源基线，后续增量必须重新读取 USB 当前状态。
回退也须使用反向文件块补丁基于当前离线状态重新规划，不能在原生运行后只恢复旧块零。

完整汇总为 `build/setup-background/20261019/followup-verification.json`。其中
`poster-store-failure-evidence.json`、`resource-validation-evidence.json` 和
`post-patch-log-followup-evidence.json` 分别保存原故障、属性对照与修复后的迁移证据。
较早 `verification.json` 已标记被替代；全部物理补丁写入经 USB 导出，临时诊断装入 RAM。

## 启动 Logo 静态分析

2026-10-19 固件静态分析：[启动 Apple Logo 缺失](apple-logo-static-analysis.md)。
原版 backboardd 的 Logo overlay 复用 ProgressUI；本项目 iPhone/iPod 类别选择的
`~iphone` / 通用资源路径，与 22H374 固件仅有的 `@2x~ipad.png` 不匹配。
未知 screen class 的错误与 PNG 查找失败是两个分支；仅修正设备类别或 screen class
不能补齐缺失资源。本轮未进行真机操作，也未修改系统或驱动。

当前调试按用户要求暂时禁用背光接管，优先原生 UI 渲染、合成和显示。统一构建入口增加
`scripts/build-bridge.py --disable-display-backlight`：不发布 `display-backlight` 和 WLED
交接记录，相关背光驱动不匹配，保留固件照明；原始背光配置和实现保留供后续恢复。
Display1.0.12 已验证关背光时扫描中断和 native VBL 持续，但 Welcome 仍未恢复。
GPU1.89/Metal0.78 增加有界提交前错误诊断和原生帧采样，禁用背光的候选已构建，尚未启动。
新任务完整交接见 [原生 UI 调试交接](../gpu/ui-render-handoff.json)，包含实际设备状态、
候选镜像和串口排他要求。不能把对照轮的硬件按键唤醒与仅申请显示 on 的结果直接归因于背光。

2026-10-18 更新：round73 已启动 Display1.0.11 / GPU1.88 / Metal0.77，建立显示 MMU
上下文并通过 GPU 表面的直接扫描。原生 CARenderer/Metal 输出经 IOSurface、IOMapper 和
IODMACommand 映射到 DPU IOVA `0xf0004000`，保留原行跨度4352；SwapWait返回0，
实际扫描地址、传输完成及全部2,073,600个参考像素一致，没有呈现路径的 CPU 像素复制。
这是受限全屏线性 BGRA 路径的硬件证据，尚未取得原生 Welcome 动画和流畅度证据。

round70 原版 QuartzCore 调用缺失的四参数线性纹理 SPI，造成当前启动的9份 backboardd
异常。Metal0.77已补齐原 buffer 视图；round71 同启动回归完成22笔合成器 GPU 作业，
未匹配到新的 backboardd/SpringBoard 崩溃。round73 保持原生屏幕唤醒后，合成器仍会
停止新增提交；该问题须在动画和 RenderServer 通路继续定位，不能归因于 CPU 复制。
当前实际镜像、源快照及原始串口见 `build/gpu-runtime/ui-display-context-wb-source.json`、
`round-73/` 和同启动的 `round-73-native-window/`。

## 共享缓冲区与直接扫描的边界

Apple 的 [IOSurface](https://developer.apple.com/documentation/iosurface) 用于共享硬件缓冲区；
当前 GPU 进口已经复用原始 descriptor。原版 AppleH7DisplayPipe 的硬件提交路径取得
原生 surface 并配置显示硬件。合成会写入输出表面，但不要求 CPU 每帧搬运该表面的像素。

此前 QcomDisplay 将该表面逐行复制到固件保留的扫描区：原生源 stride4352、扫描
stride4320。round69 原生 UI 的实测复制约307～333 ms，远超60 Hz帧预算。
要让 DPU 直接读取同一张 IOSurface，需要显示设备专属 IOMapper/MMSS SMMU 的实际映射、
32-bit 可达的扫描地址及真实源 pitch，不能将 GPU IOVA 当作 DPU 地址。round73已通过
受限 IOSurface 的 DPU 专属映射及直接扫描；不适用的布局仍使用原复制路径。

直接扫描还必须保持原生 ready fence、当前 front surface 的 retain/use count 和 DMA
映射，直到真正换出且显示硬件不再读取；超时或无法证明停止时不能释放 backing。
双缓冲交换本身不代表已经免复制。GPU1.88/Metal0.76 的 bytesNoCopy 进口是另一条资源
接口，不能将它的完成状态写成 DPU 直接扫描或欢迎动画已通过。

Display1.0.9 已离线接入这部分显示桥接：`QcomDisplaySurface` 持有原版 IOSurface 及其
IOMemoryDescriptor，经显示 provider 的 IOMapper 和原版 IODMACommand 建立单段32-bit
IOVA。要求全屏非平面线性 BGRA、64-byte pitch/base 对齐、完整分配和可达范围；
prepare、gen64IOVMSegments 前后均确认实际使用原描述符，不接纳 bounce backing。
沿用原始 cache 属性和显式非一致性 cache 同步，不建立颜色复制或新的 CPU alias。

`swap_begin_gated` 仍先等待原版 changed/ready mask，再设置实际源地址与源 pitch；
只有真实 transfer-done/VBL 且 latch 地址与 pitch 一致，才切换 front mapping 并释放旧映射，
随后进入原版 swap_complete_gated。黑帧或复制回退会将 pitch 恢复为保留扫描区的4320。
同一个 front surface 可复用已准备的 DMA 映射。超时或部分提交且无法确认 DMA 已停时，
相应映射保持隔离，禁止继续提交；resident teardown 同样保留仍被扫描的 DMA 图。
Display1.0.12 的 WLED blanking 只改变可见性，保留实际扫描、中断和已接受换帧的真实完成，
不会把关背光当作 DMA 停止证明或主动取消活动事务。
旧帧 DMA unmap/descriptor complete 失败也会保留该失败图和当前 front，停止新的准入，
不重复解除同一映射，也不会逐帧积累失败映射。
这个失败路径保留内存直到复位，正常电源管理与可恢复的显示停止仍需进一步实现。

该分支需要 `display-direct-scanout=1`，以及 provider 已发布的真实 `iommu-parent` IOMapper
对象。可选路径不等待未解析的 phandle，不使用 GPU/system mapper，也没有物理地址回退。
Display1.0.10 已实现专用 `QcomDisplayMapper` 和 EFI MMSS 交接。`--display-smmu-handoff`
在 GPU/内核分配前读取真实 SID0 路由、上下文和页表，保留原根页表，验证32-bit空闲窗口；
固件页表按完整16 KiB CPU页保留并从全内存白名单排除，EFI退出后重验并提交原生记录。
只支持独占的 AArch64/4K stage-1、MAIR[0] Normal-NC及已验证WB的固件页表；
其他活跃上下文、共享/掩码SID、bypass、AArch32或格式不符均不授予DMA映射。
继承分支不重置SMMU、不重写TTBR/SMR/S2CR，不将仍在刷新的显示器宣称为已隔离。

Display1.0.11 对上述已观察到的禁用 stage-1 bank 增加初始化分支。只接受原有精确SID、
bank和VMID，且其他上下文全部禁用；单个CTL0/LM0/VIG0或DMA0、单个命令模式DSI，
其他取数管线、mixer路由和CTL必须空闲。EFI期间在独立WB的16 KiB页中建立4 KiB粒度页表，
只映射原始GOP carveout，保留高端256 MiB的空闲32-bit窗口；页表和GOP从XNU内存白名单排除。
退出EFI后请求并等待真实VBIF XIN HALT ACK，再由公共HardwareContext封锁已拥有的流、
安装根页表/格式并完成TLBI/TLBSYNC及读回校验，最后恢复同一取数通路。
没有ACK时不改MMU；部分上下文更新失败时保留halt和页表，禁止回退到physical bypass。
EFI明确采用轮询故障模式、CFIE关闭；内核安装原生上下文IRQ后才启用CFIE。
硬件定义依据Linux msm DPU的[VBIF实现](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/msm/disp/dpu1/dpu_hw_vbif.c)
和[msm8998拓扑](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/msm/disp/dpu1/catalog/dpu_3_0_msm8998.h)。

内核通过原版 IOMapper 接口将同一个已准备的 IOSurface descriptor/UPL 页面映射为
DPU只读32-bit IOVA，沿用公共 MappingLedger、PhysicalPages 和 RetainedPageTables。
CPU页表保持WB并在发布前显式清理到PoC；真实上下文TLBSYNC完成后才返回映射。
当前front保持到硬件真实换出，故障IRQ、所有权变化或TLB/清理失败均保留DMA图并停止准入。
原GPU停机接管继续要求硬件isolation；新增live接管单独校验所有权，未削弱该条件。
round70已证明固件的bank未启用；round73收到真实VBIF halt ACK，安装6页WB页表、
TTBR0=`0xfff84000`、SCTLR=`0x1027`并完成上下文TLB同步，恢复GOP扫描后交接内核IRQ。
内核随后通过新IOVA直接扫描GPU表面，direct_frames增长且quarantined=0。
串口中的mapper_capture=0来自状态写到了重定位前的设备树，不能据此推断MMU禁用；
源码已将finish阶段的状态/寄存器诊断重定向到交给XNU的最终树，round74/75已读回mapper_capture=6。
扫描回读属于硬件源地址和保留映射的验证，不等同于光学拍摄或完整欢迎动画验证。

诊断现在根据真实 source address 选择保留扫描区或已确认的 direct front；direct 快照
不将 IOVA 当作物理地址，PhysicalAddressValid为0，并记录 surface ID、transport、实际
direct帧计数与隔离状态。显式抓帧仍会将数据复制到诊断输出，但不参与呈现路径。
现有 display 检查仅补充地址范围和源 stride/latch 边界，无新增测试文件或辅助脚本。
Display1.0.9的离线记录为 `build/gpu-runtime/ui-direct-scanout-source.json`；
Display1.0.10的构建及本轮真机证据分别写入 `ui-mmss-mapper*` 和新的 round 目录。

驱动使用参考 iPad 的 iOS 18 原生显示框架。当前交付范围是固定模式、固件交接的
LCD 扫描输出、MIPI DSI 主机和通用面板服务。已进入真机阶段：iOS 18 原生
`IOMobileFramebufferLegacy::start()` 成功，1080×1920、60 Hz，并收到真实
TE/read-pointer 中断。已通过原生前台用户会话枚举到 `CADisplay:LCD primary`；
原生 MG/BKS/CADisplay 的 scale 已为 3，原显示模式断言未再出现。
第 50 轮原生画布已为 1080×1920，第 51 轮原生窗口提交已通过两道校验，
并在真实传输中断后更新原生 completed/displayed 序号。第 53 轮原生完成等待与
两帧退出等待均已通过；第 54 轮原始迁移进度条已进入实际扫描区，首次启动
与 Setup 流程尚未完成。

第 48 轮已经读回实际 DPU 扫描区中的红底白条：1080×1920 的全部
2,073,600 个 BGRA 像素与软件 IOSurface 一致，源 stride 4352 与扫描
stride 4320 的 padding 已逐行排除。原生 `SwapEnd` 成功；当轮 `SwapWait` 返回 `e00002eb`，驱动在首个传输
中断过早报告了 displayed=0。第 49、50 轮均已完整导出，真实中断报告
displayed=1，两轮的 2,073,600 个 BGRA 像素仍全部匹配。但两轮原生
`SwapWait` 都返回 `e00002eb`（`kIOReturnAborted`）；第 50 轮原生诊断明确
报告 `CommandWake didn't happen even after 5 seconds`，随后取消交换等待。
这些是历史测试的实际返回值；它们的等待语义已在第 53 轮纠正，不能继续据此判断驱动未唤醒。
第 51 轮再次逐像素验证测试图案，但 token 7 的原生完成序号虽已为 7，
`SwapWait` 仍超时返回相同错误。当时的 flags=0 会等待该 token 退出当前扫描，
单帧持续保留时仍会等待，并不只等待显示完成。

第 53 轮核对原始 `swap_wait_gated` 分支后，使用 flags=1 等待 queued/active
请求完成，返回 0。再由线程用 flags=0 等待 token 7 退出，主线程在约 100 ms
后提交另一个实际 IOSurface：token 8 完成等待返回 0，token 7 的退出等待在
151.510 ms 返回 0，没有原生 CommandWake 超时。after 扫描区的 2,073,600
个 BGRA 像素全部匹配，原生 completed/displayed 序号为 8，active/queued 均空。
没有修改原生 wait/wake 实现。此前将单帧超时归因于原生唤醒故障不准确。
扫描内存读回仍不能测量面板发光，也不能代替原生应用界面验证。

第 54 轮已读回原始 datamigrator 的 ProgressUI：黑底进度条，无诊断测试
surface。三次采集的白色进度终点分别为 x=236、242、321，原生 displayed
序号为 191、230、786；用户也确认面板上的进度条在动。未捕获 Apple Logo
或 Setup。原生迁移首帧及持续更新已验证，完整首次启动仍在跟进。

设备端统一使用 `displayctl validate-scanout /private/var/tmp/PREFIX`。
将其生成的 validation、source 及 before/on/after scanout 文件导出后，执行：

```sh
python3 scripts/toolchain.py verify-display-readback --capture-prefix build/display-runtime/displayctl-53
```

该命令只使用 Python 标准库，生成忠实还原的 PNG 和逐像素比较 JSON，
分别保留扫描内容、原生提交/等待结果与光学验证状态。
最新软件提交证据为 `build/display-runtime/displayctl-53-readback.json`，还原图像为
`build/display-runtime/displayctl-53-after.png`。`validate-scanout` 使用 flags=1
等待完成；`validate-retirement` 在同一工具中增加两帧并发退出等待测试。

原生画布审计又定位到父节点的 `canvas-width`、`canvas-height` 数据属性。
原始 iOS 18 `IOMobileFramebufferOpen` 用它们初始化首选画布；两者缺失时，
`GetCanvasSizes` 才在 1080×1920 上追加 1242×2208 兼容尺寸。Loader 现已
预留这两个原生属性，并在验证实际扫描时写入真实宽高，准确描述当前固定
1:1 输出能力；保留原物理尺寸、PPI、scale 和所有原始框架。27 项 bridge
测试通过，包含属性预留及原生 UInt32 字节序检查。第 50 轮真机已验证：
原生 CADisplay、immutableCopy、模式及实际提交的 IOSurface 均为
1080×1920，pointScale/preferredScale 仍为 3，刷新率 60 Hz。

请求 mask 的含义也已纠正为“本次修改哪些图层”，其中空 surface 可表示
清除操作，`0x80000007` 本身不能证明同时有三个可见图层。新诊断分别在
原生 304、352 钩子记录 raw/effective IOSurface 的实际尺寸、格式、保护、
tiling 和矩形。第 50 轮 `0x80000007` 请求只有 layer 0 非空，尺寸为
1080×1920、stride 4352、线性 BGRA；layer 1、2 均为 null，矩形为零，
表示清除更新。第 50 轮仍在第一道校验因 `mask != 1` 拒绝原生窗口请求。
原始用户框架的 `0x1b6fe5b9c..0x1b6fe5c18` 将 RGB 打包为不透明颜色，
写入提交颜色字段，并用 NEON `orr.2s` 同时设置 changed/ready 的 bit 31；
因此这一位是颜色更新元数据，不能当成第 31 个图层。第 51 轮按实际
raw/effective surface 校验后已接受这些请求：必须更新唯一的非空 layer 0，
尺寸和矩形均为物理全屏，其他实际 surface 必须为空；低四位可清除空层，
bit 31 可随该不透明全屏 surface 更新背景色元数据。仍拒绝未知位、额外
surface、缩放、保护或 tiling，以及尚未实现的独立空主层/纯背景帧。

第 49–53 轮完整原生全系统 stackshot 已解码，分别包含 194、194、195、195、195 个任务。
等待链仍为 backboardd migration → datamigrator XPC client → migration 队列
的 `waitUntilPreboardDidFinishUnlock` 信号量；PreBoard 主线程处于 UIKit/CFRunLoop。
这能确定当次等待位置，尚不能证明 PreBoard 主线程死锁或迁移进度条已显示。

用户曾提到 `Press home to upgrade`，随后说明可能混淆，应按新机向导路线调试；
这次目视观察暂未确认。这条文案确实存在于
本份 iOS 18 原始 `PreBoard.app/PreBoard.loctable`，键为
`PREBOARD_PRESS_FOR_UPGRADE`。原始 `PBADevicePasscodeViewController`
按 `SBFEffectiveHomeButtonType` 选择它或 `Swipe up to upgrade`。
datamigrator 在 keybag 尚需解锁时主动启动 `com.apple.PreBoard`，然后才等待
`waitUntilPreboardDidFinishUnlock`，所以该提示可以出现在迁移完成之前。
它不是迁移完成的标志，也不能据此确认这次显示过该页面。第 51 轮运行中的 PreBoard UUID 与原固件一致，
但已保存的 before/on 扫描区只有白场，没有捕获提示文字；目前对内容来源的
判断依据是原始资源、调用路径及运行进程，尚未独立追踪该帧的 context 归属。
证据见 `build/display-runtime/migration-reference/preboard-upgrade-ui-analysis.json`。
`displayctl validate-window /private/var/tmp/PREFIX` 可保持原生 on 状态，
等待实际窗口提交后采集三份扫描区及全系统线程栈，全程不提交软件测试图案；
同一 `verify-display-readback` 命令负责还原 PNG，原生模式不推断软件像素匹配成功。

第 52 轮只保留原生窗口约一分钟，三份读回均为不透明白场，没有捕获提示、
Logo、进度条或 Setup 界面。新机目标是原始 `/Applications/Setup.app`
（`com.apple.purplebuddy`）。当轮没有 Setup 任务；SpringBoard 主线程在等
BackBoard check-in，其 turnstile 连到 backboardd migration 等待线程。
Data 中没有 purplebuddy 完成记录，`DMUserDataDisposition.SyntheticDidUpgrade`
为 false，不能按旧数据升级完成页面解释这次白场。

启动日志明确报告 `systembag.kb` 不存在且未加载系统 keybag。原始 keybagd 的
`load_devicebag` 只查询已加载 bag 或加载已有文件，不创建缺失的系统 bag。
它的 enhanced-APFS 判定也接受现有 `-apfs_shared_datavolume` 参数；该模式下
记录未加载 bag 后仍继续 allowlist 初始化，所以 boot task 正常退出并不代表
keybag 已可用。用户已授权对阻塞服务做开发补丁。第 54–69 轮修改原始 datamigrator
的两条解锁分支：保留 MKB 调用及真实返回值，负的 lock-state 错误不再启动
PreBoard 等待；正常状态 0、3 及正的已锁定状态仍沿原路径处理。迁移插件和
Setup 完成条件未跳过，这也没有实现无 SEP 密钥存储。

统一通过 `scripts/toolchain.py prepare-display-services` 生成固定长度的服务，
保留原 requirements、XML/DER entitlement 字节，使用通用开发标识重签并加入
本轮 trust cache。可同时输入设备原来的 resource fork 与 decmpfs 属性，只重压缩
发生变化的两个逻辑块；大小、块偏移和资源表均保持不变。已验证该服务压缩
extent 的引用数为 1，备份后仅替换其中 12 个数据块，物理读回与独立 APFS
解压读回均匹配，未改 GPT、文件权限或 APFS 元数据。原数据及事务位于
`build/display-runtime/migration-policy/usb-backup/`；补丁和存储清单位于
`build/display-runtime/migration-policy/manifest.json`。第 54 轮没有 PreBoard 任务，
等待已前进到迁移插件：SpringBoard → backboardd → datamigrator →
SystemAppMigrator 的 App Store 请求取消回调；该原生等待自带 45 秒超时。
appstored 的多个线程等待 accountsd，后者又等待迁移完成；appstored 自身也
有迁移等待线程。单次采样不能证明整个服务永久死锁，第 55 轮保留更长时间
并比较前后状态。第 55 轮在约 123 秒仍采样到取消等待，约 490 秒已前进到
系统应用安装的 dispatch group；原生超时公式是待安装数量 × 40 秒。进度条
白色终点继续从 x=237 前进到 x=627，原生 displayed 序号最终为 3862，尚无
Setup 任务。不能把动画前进当作所有安装任务已完成。首次启动密钥路径分析见
`build/display-runtime/migration-reference/first-boot-keybag-analysis.json`。

继续核对 appstored 的请求监听线程后，原始 AppleMediaServices 的错误路径将
同步 Mach 请求定位为 `AMSFairPlayGetDeviceType`，不是普通下载进度等待。
请求编号为 1203，服务名为 `com.apple.fairplayd.versioned`，回退为
`com.apple.fairplayd`。账号服务等待迁移完成属于另一个已观测到的分支。
设备原生 `fairplayd.A2` 与启动 plist 均与原固件字节一致，cdhash 已在活动
trust cache 中。第 57、58 轮 launchd 查询中该任务没有 PID、LastExitStatus=0；
第 56 轮原生诊断报告的 launchd 线程名为 `fairplayd.A2 throttled after exit()`。
原 58 Kext 集合没有 `com_apple_driver_FairPlayIOKit`。第 64 轮已加入原始
iOS 18 FairPlayIOKit：原生 Apple 匹配、UserClient 打开均成功，返回 0。
重定位器通过完整只读页的等距映射证明及有界双目标跳转表识别解决原先 30 处
拒绝；未放宽未知分支检查。59 Kext 集合通过独立结构、依赖及指令检查，
5 项链接器测试通过。驱动及 FairPlay 返回值没有改成成功。

第 65 轮公共 GenericAES 诊断确认，FairPlay 初始化请求的参数是软件提供者
不支持的密钥选择器 2201、128 位、32 字节、原地 CBC，返回
`kIOReturnUnsupported`；普通软件密钥的 AES-CBC 加解密自测成功。原生
FairPlayCryptoUtilities 将非零结果转换成 -22 并打印第 233 行失败信息。
因此不能把该信息解释成“应该返回 -22”，也不能通过伪造设备密钥解决。
诊断由 `aes-compat-trace=1` 显式启用，限量记录参数形状，不输出密钥或数据。
64、65 轮进度显示仍工作，定位仍等 FairPlay；手动请求原生 reboot 后，
launchd 在 Cryptex 卸载阶段因 SIGBUS 退出并导致 panic，已保留失败结果，
不能将这两轮记为无 panic 的正常重启。
第 56 轮增加显式开发选项 `--development-fail-fast-system-app-installs`，通过
同一 `prepare-display-services --system-app-migrator ORIGINAL --output DIR`
命令只修改 SystemAppMigrator 的一条超时计算指令。原生 group wait、超时日志、
安装失败结果和其他迁移插件均继续执行，不伪造应用安装或 Setup 完成。
该选项仅用于首次显示与启动调试，未完成的异步安装可能仍然挂起。

模块为原始 MH_BUNDLE，保留 requirements，使用通用开发签名并加入本轮
trust cache；未将 bundle 声明为主程序。已备份其独占的压缩 extent，仅更新
9 个数据块，物理读回与独立 APFS 解压读取一致，未修改文件系统元数据。
补丁、安装事务和回滚原数据分别在
`build/display-runtime/system-app-policy/manifest.json`、
`build/display-runtime/system-app-policy/usb-backup/`。显示驱动及内核保持第 55 轮基线。

第 56 轮已验证 SystemAppMigrator 沿原生失败路径返回 NO，后续进入
`SpringBoard.migrator`。其 SplashBoard 工作线程通过 QuartzCore 同步请求生成
系统应用启动画面快照；迁移线程最多等待截图回调 300 秒，再写入
`XBCaptureLaunchImagesPostMigration` 以便启动后补做。第 57 轮还捕获到更早的
截图应用列表查询：UIKit/FrontBoard 同步进入 SpringBoard 的应用信息处理线程。
因此只缩短最后的信号量期限不能覆盖整个阻塞路径。

统一命令 `prepare-display-services --springboard-migrator ORIGINAL
--development-defer-launch-images --output DIR` 现将这项预生成延后：只让截图
专用的应用列表方法返回 nil，并从截图方法的原生栈帧进入既有 defaults 代码。
原生迁移后截图标记和其余 SpringBoard 数据迁移均保留，不写 Setup 完成状态。
24 字节代码修改、requirements、原生栈恢复及三个释放对象的 nil 初始化均已验证；
已备份独占压缩 extent，仅替换 11 个数据块并完成两种独立读回。
第 58 轮的期限修正已恢复为原始文件，最新延后策略及回滚数据位于
`build/display-runtime/springboard-deferred-policy/`。第 60 轮已从 Data 卷独立读回
`com.apple.springboard.plist`，确认原生延后标记为 true，并进入后续迁移插件。

第 59 轮在 158、221 秒仍处于 SystemAppMigrator 的缺失应用占位包创建，
尚未到达 SpringBoard 插件，不能据此宣称截图延后策略已通过。服务端为
installcoordinationd，具体调用是 `IXCreateDirectoryAtURL` 中的 `opendir`；
内核栈进入 APFS 查找。该线程保持运行，系统调用数从 794 增至 31,153，
写请求从 22 增至 786，正在逐一生成 `.lproj` 等目录，不是睡眠死锁。
最新同一开发选项还让 `installDemotedPlaceholderWithBundleID:installType:`
在创建安装协调器之前立即返回 false；原生调用者保留缺失应用、失败日志和
App Store 请求。仍不报告安装成功，不清除应用缺失状态。它与原生 group
立即超时共同构成 12 字节代码修正，准备与回滚数据位于
`build/display-runtime/system-app-fast-policy/`。第 60 轮已从 SystemApp 包装进程
转入包含 SpringBoard、CoreLocation 和 MobileSlideShow 的后续包装进程。

第 60 轮后续的关键等待链为 `CoreLocationMigrator → locationd → timed`。
`timed` 的 `-[TMCache hostUUID]` 调用 `gethostuuid()`，零超时参数在原生内核中
表示无限等待；实际内核栈停在 `IOBSDGetPlatformUUID → waitForService`。
同一资源还阻塞 configd 和 rapportd。设备树此前缺少 `unique-chip-id` 和
`chip-id`，原生 `IOPlatformExpertDevice::generatePlatformUUID` 因而无法生成
并发布 `IOPlatformUUID`。这条等待链没有截图或软件栅格化工作。

桥接器现在从固件 ChipInfo 公共协议读取实际硬件序列号和 SoC ID，再填入
预留的 8 字节和 4 字节 DT 属性；UUID 的派生和资源发布继续使用原生内核。
32 位 Qualcomm 序列号零扩展为 64 位，不使用固定 ECID，不提供 UID/GID 密钥。
未知标识或属性布局错误会在进入 XNU 前终止。第 61、62 轮发现头文件中的未用
GUID 与实际固件不同，已按驱动安装代码及 SimpleInit.dec 修正为
`b0760469-970c-487a-a4b5-28db7b45cef1`。第 63 轮固件标识校验通过，原生内核
及服务补丁保持不变；相隔 63 秒的两次完整栈均不再出现平台 UUID 等待，
定位服务已进入后续 `CLHarvestControllerSilo` 初始化。实际 scanout 中的白色
进度条端点从 x=378 推进到 x=463，原生 displayed swap 从 1145 增至 1716。
Setup 尚未启动，仍不能把进度动画当作迁移完成。

后续定位线程停在同步 Mach 请求 1203：发送 24 字节、接收上限 60 字节、
回复编号 1303，timeout=0，与此前 AMS 的 FairPlay 请求 ABI 一致。同一原始
locationd 调用者含减去 75/90 的服务名解码，已通过间接表分别连接到
`com.apple.fairplayd.versioned`、`com.apple.fairplayd` 及 `_bootstrap_look_up`
导入；请求函数表项也已验证，服务归属已确认。原始指令和分析位于
`build/display-runtime/handoff-63/locationd-mach-request.json`。
并行的 MobileSlideShow 线程仍等待 assetsd 的迁移清理请求；全量共享缓存
镜像解析将服务端进一步定位到 PhotoLibraryServices 的 NSCondition 等待
和 PLFirstUnlockProtection 路径，初始化条件仍需追踪。
以上等待没有建立软件栅格化耗时过长的证据；300 秒是原生截图回调期限。


第 42 轮已增加默认关闭的只读 scanout 采集：启动参数
`display-scanout-capture=1` 下，管理员可通过原生 IORegistry 的
`setProperties` 请求快照；`displayctl scanout /private/var/tmp/PREFIX`
导出 BGRA 与寄存器元数据。它读取 DPU 当前源地址所对应的保留映射，
不写像素或显示状态。1080×1920、stride 4320、地址 `0x9d400000` 的
交换前后两份 8,294,400 字节完全一致；还原图像仍是内核文字日志。
测试 IOSurface 中的红底白条未进入扫描区。证据及 PNG 位于
`build/display-runtime/scanout-comparison-42.json` 和
`build/display-runtime/displayctl-42-after.png`。此方式可自行验证内存和
寄存器结果，不能测量面板是否发光。

同轮原生队列审计发现子类尚缺 pending TAILQ 摘链：只写 active 指针会
让原生完成函数释放后仍留有 pending 引用。修正按原生 iOS 18 子类模型
调用原生队列整理助手、检查 IOSurface 就绪 mask，并转移摘下的队列引用；
只在真实 VBL 上重试已有待处理请求。助手均作为哈希及入口字节校验的
重定位别名绑定原始实现，详见 [native-queue-audit.json](native-queue-audit.json)。

第 43 轮原生 `CADisplayStateControl` 已可创建并接受 on 请求：显示状态从 0
变成 1，但电源当前状态仍为 0、目标为 1；前、点亮请求后、软件交换后的三份
scanout 完全相同，原生控制器请求及当前电源字段仍为零。对照最终原生显示子类
补上硬件子类负责的 `PMinit/joinPMtree/registerPowerDriver`，并维护原生
`+0x8c` PM 初始化标记。两种状态均保留平台电源和时钟，低状态只撤回设备可用性，
不声称支持断电冷恢复。另纠正 slot 355 的早期命名推断：原生诊断明确将其返回值
标为 `TimingEnable`，现在按已验证的持续扫描条件返回；没有改写原生电源结果字段。
第 44、45 轮已确认 PM 标记生效，但硬件当前电源字段仍为零。
原生请求校验仍返回 `e00002c2`；三份扫描区保持相同的文字日志。
进一步审计纠正了早期 slot 300/301 的命名推断：原生先调用 slot 304 的
Boolean `verify_swap(void *)`，再调用 slot 305；进入 command gate 后还会
调用第二道 Boolean 钩子 slot 352。第 45–47 轮只实现了后一道检查，
前一道继承的默认 false 导致所有提交提前被拒绝。现有严格校验现已同时
接入 304 和 352，保留原生 305 及其余检查，第 48 轮已通过物理尺寸的测试提交。

最终原生显示子类还负责 slot 306 的硬件电源状态回调，并通过原生通知
报告当前控制器和显示状态。新增 SDM835 回调只在真实 TE、扫描区地址、
格式、尺寸、stride 和 DSI 时序均验证后报告固件保留的 on 状态；
不支持的 off 操作仍返回错误，不伪造断电。第 46 轮已验证当前控制器、
显示电源和原生 state-control 实际/目标值均为 1；扫描像素仍未更新。
`displayctl validate-scanout /private/var/tmp/PREFIX` 将原生 on 请求、
软件 IOSurface 交换、扫描区前后读回及原生全系统线程栈统一到一次执行中，
无需现场观察即可检查图案是否进入实际扫描内存。

第 34 轮已用三份原生全系统 stackshot 连上迁移等待链：
`backboardd → com.apple.datamigrator → waitUntilPreboardDidFinishUnlock`。
约 79 秒的两端采样停在同一等待；PreBoard 进程存在，主线程在 UIKit 事件循环，
尚无证据证明 PreBoard 自身死锁。动态启动的 PreBoard 不出现在同名 launchd
`GetJob` 查询中，不能据此判断应用未启动。SpringBoard 的普通 check-in
则等待 backboardd 的迁移阶段完成。

同轮原生 ProgressUI 已创建窗口和图层，但返回
`unsupported screen class, no apple logo found`；用户确认没有看到 Logo/进度条。
MG 的 `main-screen-class` 使用固定尺寸/scale/PPI 表，真实
`1080×1920 / scale 3 / 429 PPI` 不在表中，且这份 iPad 固件只包含 iPad Logo
资源。没有为迎合分类表修改物理 PPI 或伪造屏幕缓存。根诊断进程的两个只读
MKB 查询均返回 `-13`；服务端确实等待解锁，但该采样没有读取服务端自身的
MKB 返回值，也没有让无 SEP 的 AKS 返回虚假成功。完整证据见
[runtime-validation.json](runtime-validation.json) 的第 34 轮记录。

## 无 SEP 的公共 MKB 查询

通用 ARM 开发系统统一在 MobileKeyBag 处理无 SEP 情况：显式构建选项
`--development-no-sep` 选定该平台策略，`MKBGetDeviceLockState` 返回 3
（未设置口令），`MKBDeviceUnlockedSinceBoot` 和 `MKBUserUnlockedSinceBoot`
返回 1。用户查询同样覆盖非零用户 ID，不再给调用它们的服务分别加首次解锁补丁。
三个查询直接返回，不在服务初始化期间再次调用 MobileGestalt 或 AKS。
这里不要求特定挂载点、文件系统名称或机型。AKS 密钥操作和原始错误码保持原样。

统一构建入口如下，输入为本项目 22H374 的原始 raw SystemOS Cryptex：

```sh
python3 scripts/toolchain.py prepare-keybag-policy \
  --development-no-sep \
  --system-cryptex build/apfs/firmware/094-33089-038.dmg \
  --output build/display-runtime/keybag-common-policy/direct
```

输出包含公共查询代码、共享缓存的开发签名、对应 trust cache 和同尺寸块差分。
`scripts/apfs_sealed.py` 更新变更文件的哈希、B-tree 祖先及 seal root；原始镜像保持不变。
bridge 同时加入输出目录的 `trust-cache.bin`。新标识统一使用
`org.darwin.development.shared-cache`，没有机型前后缀。

第 70 轮前已恢复 datamigrator 原始文件，独立 APFS 解压读回 SHA-256 为
`4275137397d7df5d4c0f46d90eb5141ed3bb7f6759c41cc5c5ca019d5f14ee92`。
第 72 轮真机公共查询已返回设备锁状态 3、首次解锁状态 1。三个入口的 ARM64
执行测试覆盖共享缓存 slide 和不同用户 ID；用户查询没有单独取得真机返回值。

第 76–77 轮的原生看门狗报告保留了多份完整系统堆栈：原先的 Photos
`waitUntilFirstUnlock` 等待帧不再出现，backboardd 正常签到，datamigrator
回到 XPC/CFRunLoop，Setup 已启动。当前阻塞点为 Setup 同步等待
SpringBoard 返回应用初始化上下文，随后触发启动看门狗。
**这不等于所有迁移项均成功**：`com.apple.migration.plist` 的
`DMLastMigrationResults.success=false` 记录早于这两轮且没有更新。
汇总证据位于 `build/display-runtime/keybag-common-policy/hardware-result.json`。

后续调试先导出同一轮 `/private/var/mobile/Library/Logs/CrashReporter` 中的
原生报告和迁移偏好文件，核对 boot session、进程等待链与记录更新时间；
不要把旧的迁移结果当作本轮完成状态。串口不响应也应先检查这些落盘证据。
第 76–77 轮试制的常驻采集命令未实际运行，已恢复原先实测的诊断程序。
当前可复用的公共策略启动镜像为 `build/display-runtime/keybag-direct-bridge/XnuBridge.efi`；
第 77 轮镜像对应已回滚的诊断程序，不能混用其诊断签名。
安装事务和回滚块位于 `build/display-runtime/keybag-common-policy/`。

## 应用初始化等待与启动诊断开销

继续分析第 74、76、77 轮原生全系统报告后，SpringBoard 的主线程分别位于
SwiftUI、图标、材质和控件初始化，并有缺页读取等待；Setup 的同步初始化请求
只是下游等待。第 76 轮两个晚期采样分别有 238、239 个线程经过
`throttle_lowpri_io`，第 77 轮有 116 个。串口 Bash 也在其中。

`build-bridge.py --development-unthrottled-io` 在开发启动镜像中将 XNU 原有
`lowpri_throttle_enabled` 初值设为 0，通过符号定位，不依赖机型地址。它保留
`debug.lowpri_throttle_enabled` 运行时设置接口；不选此选项时维持原行为。
第 78 轮三份原生报告中该等待计数均为 0，但 SpringBoard 仍未正常签到。
同轮早期采样另有 34 个线程堵在详细 exec 跟踪的串口输出锁上：
`exec_image_trace → printf → console_ring_try_empty → lck_mtx_lock_contended`。

`--serial-no-iolog` 使用 XNU 原生 `SERIALMODE_NO_IOLOG` 位，保留串口输入、
控制台输出及 `os_log`，仅关闭 IOLog 的串口副本。结合关闭 exec、IOKit、AES
详细跟踪使用，避免诊断日志拖慢服务启动。第 79 轮验证镜像为
`build/display-runtime/initialization-quiet-bridge/XnuBridge.efi`。
同轮完整报告内 I/O 限流等待与 exec 串口锁等待均为 0；SpringBoard 主线程已在
主循环的字体／图层提交中缺页，Setup 的早期请求仍未返回。后期未写完的报告
还能确认 SpringBoard、backboardd 已换 PID，不能将“进入主循环”当作稳定完成。

安静串口暴露了另一项公共层缺省：`kern.maxvnodes` 未设置，嵌入式 xsmall
配置只给 1024 个 vnode，持续触发 `vnode-limit` jetsam。APFS 系统启动现在通过
`/defaults/kern.maxvnodes` 提供 16384，可用 `--max-vnodes` 调整，无需逐服务补丁。
第 79 轮最后还因缺少平台 suspend 后端，在系统休眠入口 panic。
`--disable-system-sleep` 写入原生 `/defaults/sleep-disabled=1`，由 IOPMrootDomain
提前拒绝休眠。两项修正的第 80 轮候选为
`build/display-runtime/initialization-platform-bridge/XnuBridge.efi`。第 80 轮 vnode
jetsam 为 0，原生报告中的首次解锁与 I/O 限流等待也为 0。datamigrator 主线程
处于正常事件循环、工作线程空闲，backboardd 没有 DataMigration 等待帧；Setup
仍在等待初始化回复，随后系统发生用户态 watchdog panic，已自动恢复固件。

第 80 轮全系统报告另有 303 个 runnable 线程超过 20 秒未获调度，其中 297 个
后台线程的 base/scheduled priority 均为 4。控制台启动进程 PID 94 在签名检查
时被抢占，此后 85.6 秒未运行，不能将这一栈位置误读为签名检查死锁。
本版 dualq 使用非 Clutch 的 iOS timeshare 规则，默认 priority decay limit=18，
使 base 31/37/47 的线程最低分别只能降到 13/19/29，高于后台优先级 4。
第 81 轮候选用原生 `sched_pri_decay_limit=63` 允许 CPU 密集型工作继续降低
动态优先级；不修改服务 QoS、固定／实时优先级或 watchdog。但第 81 轮仍有
294 个后台 runnable 线程超过 20 秒未调度，SpringBoard 仍触发 watchdog，未作为
最终修复采纳。

当前候选启用 XNU 原生 Clutch/Edge：平台声明 AMP，移除旧 opt-out，设备树
以原生 `cluster-type` 标记 E/P，现有 PSCI/GIC 后端保持。第 82 轮先使用与前轮
相同的四个 E 核比较防饥饿行为；八核 E/P 分配仍需独立验证。原生 Edge 内核、
collection、EFI 已构建，调度入口静态解析指向 `sched_edge_dispatch`。候选为
`build/display-runtime/initialization-edge-bridge/XnuBridge.efi`。第 82 轮实际内核 UUID
匹配 Edge 构建；完整报告中，排除暂停线程并取 lastRunTime 与
lastMadeRunnableTime 的较小值后，持续 runnable 超过 20 秒的线程为 92 个，
其中后台 58 个；按相同口径，第 81 轮为 310／290。Bash 已完成 exec，但未执行
采集命令。SpringBoard 仍在 SwiftUI／UIKit 初始化的缺页等待，Setup 仍等回复，
最终 SpringBoard watchdog 恢复固件；其他受监控服务保持签到。

第 82 轮仅有 1 GiB 物理内存，报告空闲 479 个 16 KiB 页（7.48 MiB），
throttled 队列有 38565 页（602.58 MiB）。后者是 VM 页面队列，不能与已解除的
lowpri I/O 等待混为一谈。按用户要求，下一轮使用 `--full-memory`：UEFI 最终
内存图中的 ConventionalMemory 加内核预留窗口形成 RAM 白名单，保留固件、
设备和仍在使用的引导资源；XNU 的页分配和 managed-page 判断跳过空洞，线性
vm_page 元数据仍按物理页号索引。早期汇编仍只映射连续启动窗口；`arm_vm_init` 读取区间后才将 `memSize` 扩展为
物理跨度，避免固定 bootstrap 页表越界。`memSizeActual` 和 `max_mem` 表示
实际可用总量。该选项需要包含通用 RAM 区间支持的新内核。第 84 轮已进入用户空间：
350145 个空闲页加 3944 个已占用页，按 16 KiB 计与 5.40297 GiB 交接量一致；
178.319 秒取得真实 Bash 命令响应。

第 84 轮已解除 Setup 等待 SpringBoard 初始化回复的卡点。单次全系统快照覆盖
264 个进程、850 个线程，核对内核与缓存 UUID 后，SpringBoard PID 32、Setup
PID 256、backboardd PID 65 的主线程均为正常 Mach 消息／CFRunLoop 等待，
Setup 的 dispatch_once／初始化上下文同步请求链已不存在。原版 datamigrator
PID 398 主线程正常事件循环、工作线程空闲；无首次解锁或低优先级 I/O 等待。
排除暂停线程后，持续 runnable 超过 20 秒的线程为 0。服务快照记录的原始
SpringBoard PID 未变、LastExitStatus=0，采集后主动执行 native orderly reboot，
全过程没有 panic。原始结果与判断见
`build/display-runtime/handoff-84/initialization-verdict.json`；对应镜像为
`build/display-runtime/initialization-memory-bridge/XnuBridge.efi`。

迁移偏好文件与标记再次独立导出：同版本 22H374、migrate 标记不存在、旧
`success=false` 时间戳仍为 08:37:27，没有伪造成功标记。本轮已证明迁移服务
不阻塞、Setup 初始化完成，不能据此声称历史每个迁移插件都成功。另有一份
softwareupdated 的空指针 SIGSEGV 报告；未将系统整体宣称为无故障。


## 无 SEP 环境的密码误判

第 85 轮真实扫描区已经能够显示原生文字和输入框，但内容是错误的
`Enter Passcode`，尚未显示首次设置向导。第 87 轮扩展 MobileKeyBag 的
状态信息查询后，MCProfileConnection 的 `isPasscodeSet` 已为 0；三次晚期
扫描区只有状态栏，用户仍报告看见密码框。因此这一中间版本没有作为修复结果。

根因是 iOS 18 的 `SBFMobileKeyBag::_queue_aksDeviceState` 直接调用原生
`aks_get_extended_device_state(0, &record)`，没有经过此前补过的 MKB 公共查询。
调用者先清空 66 字节记录，AKS endpoint 不可用时继续使用零值；
`_queue_keyBagInfo` 从记录偏移 4 取得锁状态，而
`_queue_setHasPasscodeIfNecessary:` 把非 3 状态视为已有密码。
这解释了公共 MKB 返回“无密码”，SpringBoard 仍要求密码的分歧。
原生 AppleKeyStore 服务已经存在，缺少的是无 SEP 开发环境的一致状态来源。

`prepare-keybag-policy --development-no-sep` 现在同时生成两个固定 22H374
子缓存的开发差分：保留原先 MKB 的三个可用性查询，并在 AKS 的默认设备 bag
只读查询中提供原生 66 字节状态（flags=2、lockState=3、bagState=4）。
普通与扩展查询共用同一记录；其他 bag 和空输出仍转入原始 wrapper/backend。
没有提供密钥创建、加密或密码解锁操作。第 87 轮试验的 MKB 间接查询与状态
信息 helper 改写已经撤回，状态字典仍由原生 MKB 构建。

状态实现位于 `tools/displayctl/keybag_aks_policy.S`，统一构建入口和
原始 Cryptex 输入不变。构建会分别验证两个原始缓存 SHA-256、刷新代码签名、
文件哈希与所有 seal 祖先，并生成含两个 cdhash 的 trust cache。
ARM64 执行回归覆盖共享缓存 slide、记录边界与哨兵、以及原始 backend 参数保留；
三个回归测试通过。安装对实际 GPT、无加密/无快照卷和独占物理 extent 做检查，
保留可回滚原块，并使用直接 I/O 和独立 APFS 逻辑读取验证诊断程序。

第 88 轮用户确认界面成功，同时该轮在控制台可用前记录了独立的调度诊断
panic。原生关中断检查会在 BSD 初始化结束以 `-b` 后缀重新初始化，只有早期
`ml-timeout-interrupt-masked-scale=8` 并不能保留后期阈值。最终候选同时提供
`ml-timeout-interrupt-masked-b-scale=8`，将 SDM835 上的两阶段阈值设为 5 ms，
保留原生检查与 panic 行为；内核调度器未为此修改。移除未使用的 loadable
诊断 mmap 例外，以保持原生 NVRAM boot-args 小于 1024 字节。

最新镜像为 `build/display-runtime/setup-software/bridge-aks-timing/XnuBridge.efi`，
SHA-256 为 `8d4764e77133603c0b06ca128fa149ccb5856e5d65efa9593a6e2b0d07a543e6`。
构建参数、测试、安装回滚块和根因记录统一保存在
`build/display-runtime/setup-software/`。第 89 轮在 132.116 秒取得真实 Bash，
同一次启动完成锁状态、原生帧计数、三份扫描区和全系统线程栈采集。MC 与原生
SBFMobileKeyBag 的 hasPasscodeSet 均为 0；普通及扩展 SBF 状态均为
Disabled（3）、isEffectivelyLocked=0，首次解锁为 1。使用不含 GPU 驱动的
59-Kext 内核集合，用户已确认真机界面出现。

第 89 轮窗口验证开始时，原生 displayState、powerState 与两个目标状态均为 0；
原生请求 on 后四者均为 1，completed/displayed 从 128 增至 131。晚期扫描区是
没有锁图标的状态栏画面，pending/active/queued 均为 0。SpringBoard PID 32、
backboardd PID 65 与 Setup PID 268 保持运行，服务 LastExitStatus=0。
用户随后观察到运行一段时间后疑似自动熄屏而面板仍亮、画面停住。

第 89 轮使用的 `QcomDisplay::do_power_state_change_gated` 只支持固件保留上电的扫描输出，
对真实 panel-off 请求返回 kIOReturnUnsupported；当时 IOPM 的两个状态也都保留电源
与时钟。原生软件状态与面板实际状态尚未形成完整熄屏联动，和上述观察吻合。
该轮的背光与原生唤醒联动尚未实现；不能仅凭停帧认定渲染服务死锁。
另一个原生锁屏 MIG 查询返回 0x10000003，没有把其无效输出解释为“已解锁”。
不宣称持续显示与自动熄屏/唤醒已经通过验证。

`displayctl scanout-live` 可将原始扫描区编码为 PNG 后经串口完整分块导出，
避免为每张截图重启到存储模式。本轮收尾后仅停止主机采集器，真机保留 XNU 运行。
10 个本目标遗留的一次性安装/导出/分析脚本已删除，临时 Go 元数据读取脚本随执行
删除；复用诊断工具、既有回归测试和回滚块保留。迁移历史中的旧 `success=false`
和 Setup 完成标记均未改写，不能据此声称历史每个迁移项都成功。

## PMI8998WLED 背光、关屏与唤醒

2026-10-17 新增独立 `PMI8998WLED.kext`，类、目录和 bundle 均按背光芯片型号命名。
本节包含离线实现和后续小米 6 真机验证；上面的轮次记录保留各轮当时的结果。
板级依据为固定提交的
[小米原厂配置](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/3fb35ef1f0fe1f714be3df39724cbc8eb217181e/arch/arm/boot/dts/qcom/sagit-msm8998-mtp.dtsi)：
主屏使用 `bl_ctrl_wled`，亮度范围 1–4095，WLED 使用 string 0/1。
SID 3、control `0xd800`、sink `0xd900` 和亮度锁存/软启动时序分别见
[PMI8998 DTS](reference/pmi8998.dtsi) 与 [原厂 WLED 驱动](reference/pmi8998-wled.c)；
完整来源、SHA-256 和文件长度已加入 [来源清单](reference/sources.json)。

`PMI8998WLED` 匹配现有 `QcomSPMI`，共享它的工作循环和总线串行化。
初始化只读：验证 PMI8998 的真实 type/subtype、AP 对两个外设的所有权、
实际启用的 strings、12-bit 模式、故障状态和各 string 一致的固件亮度。
不设置默认亮度，不重配 boost、电流限制、CABC、面板供电、reset 或 DSI PHY。
只有这些检查成功后才发布可匹配服务；缺失、错误或尚未提交的交接不能点灯。
专用的 retained-bus 租约使背光在 SPMI 软件低 PM 状态下仍可操作，不开启任何电源轨。

后续真机确认了一条连续故障链：固件保留的 `SID 3:0xd947=0x03` 被误判为
未完成同步，WLED 拒绝接管；依赖背光的 `QcomDisplay` 因此未调用原版
`IOMobileFramebufferLegacy::start`，framebuffer 数量为 0，SpringBoard 在
`SBMainDisplayConfigurationTransformer` 的显示配置断言中退出。此前同一
QcomSPMI 默认匹配类别的冲突也已通过独立 `DisplayBacklight` 类别修正。

SYNC 是软件亮度锁存位，并非忙标志；[Linux WLED 驱动](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/video/backlight/qcom-wled.c)
的 WLED3/4 共用操作同样先清零再置位，结束时保留高电平。接管现在接受固件保留
的 SYNC 电平，其他硬件检查保持有效。每次调光写入亮度影子寄存器后，清除本驱动
strings 的 SYNC 位，再等待并置位，保证存在实际锁存沿，同时保留其他位。
只删除接管检查不能解决原更新路径在继承 `0x03` 时连续写入两次 `0x03` 的问题。

`QcomDisplay` 通过带标识的通用 `DisplayBacklight` 平台接口独占该背光服务，
原生 `IOMobileFramebufferLegacy` 电源回调负责联动：

1. controller 或 display 请求关闭时，关闭真实 WLED module 并读回确认，
   然后停用本驱动拥有的 TE/transfer 中断、暂停新提交，取消定时器，并通过
   原生完成函数以未显示结果结束在途 swap。
2. 再次请求开启时，复核保留 raster 的尺寸、格式、地址与时序；双缓冲还核对
   已锁存地址。先恢复保存的亮度及 WLED module，再清掉休眠前的中断状态、
   恢复中断和提交，后续 swap 仍须等待新的真实硬件完成。
3. 硬件 ACK 和读回成功后才更新两个 native current 字段并发送原生电源通知。
   写入失败、失去资源或读回不符会保留失败状态，背光禁止继续写入，需重启重新交接。
   IOPM 回调单独处理错误，避免把 `IOReturn` 错误值解释为超长确认等待；
   真实失败保存在 `DisplayPowerTransitionError`，直接显示调用继续收到原错误。

这里的关屏关闭发光并暂停软件显示，**保留 LCD/DPU/DSI 的固件扫描、电源和时钟**。
尚未实现面板冷关机/冷启动与整机 suspend；开发引导继续使用既有
`--disable-system-sleep`。锁屏/唤醒按键沿用 `QcomButtons → IOHIDDevice /
IOHIDEventDriver → 原生系统显示策略`，没有在按键驱动中增加第二套亮灭屏开关。
`input-runtime` 集合包含这条原生按键链路及背光依赖。故障期间已捕获 FPC Home
按下/松开并确认原版 IOHIDEventDriver 打开传感器；无界面响应不能归因于传感器
未识别。修复后的启动再次捕获 Home 和侧键 HID 输入。

管理员可向 framebuffer 写入单独的 `DisplayBrightness` 整数属性（0–4095）。
关屏期间只保存期望亮度，唤醒后才写入；0 保持背光关闭。
`DisplayAppliedBrightness`、`DisplayBacklightPower`、`BacklightStateValid` 与
`BacklightLastError` 用于区分期望值、已确认值及失败。

另按项目内原版 22H374 `BrightnessControl.framework` 和 AppleARMBacklight 实现，
在接管成功后发布 `backlight-control=true` 与
`IODisplayParameters.brightness={min:0,max:4095,value:当前期望值}`。原版
`BCAppleBacklightBrtControl` 通过属性匹配发现服务；顶层 `brightness` 数值写入
经原生 `com.apple.private.dfr.brightness-access` entitlement 或管理员权限验证，
与 framebuffer 电源请求在同一 SPMI 工作循环串行执行。关屏时仅保存亮度，唤醒时
再锁存。范围使用真实 12-bit DAC，不提供虚构的 nits、电流或面板标定值。
这完成了原生发现和属性协议接入；Settings 滑块、自动亮度和私有亮度曲线的整机
端到端行为尚未验证。反汇编地址、原版映像标识与验证边界见
[原生背光审计](native-backlight-audit.json)。

`tools/display-handoff.json` 提供型号和资源，`build-bridge.py` 预留
SPMI 的 `qcom,pmi8998-wled-handoff`：8 个 U32 为 version、flags、identifier、
SID、control base、sink base、strings mask、sync delay（微秒）。构建时 version/flags
均为 0；EFI 完成 ExitBootServices 并再次核对 raster 后才提交为 1/3。
display 的 `display-backlight` 与 identifier 均取 SPMI phandle。
旧 profile 省略 `backlight` 时保留原来的上电扫描路径。

```sh
make display-drivers test-display
make display verify-display
# 包含原生锁屏按键的完整运行集合，沿用已验证的四核/全内存内核：
python3 scripts/toolchain.py pack-drivers --target SDM835 --collection input-runtime -j 3 \
  --kernelcache build/ios18-kext-analysis/kernelcache.release.ipad7c.macho \
  --local-kernel build/display-runtime/edge-kernel-build/memory-output/mach.development.sdm835.unstripped
```

现有 `display_test.cpp` 覆盖真实生产后端的固件亮度接管、重复关屏、关屏调光、
恢复顺序、零亮度、范围校验、错误资源、全部 off/on/调光交易步骤的故障注入，
以及 ACK 丢失、读回不符和故障后禁止重试。新增的锁存模型区分影子寄存器与真实
输出，覆盖实机 `SYNC=0x03` 及保留位组合，避免仅凭读回亮度误判成功。
桥接回归检查型号、资源校验和 Apple DT
小端字节序。交叉构建、私有 ABI、集合链接和 ASan/UBSan 均属于离线验证。
构建产物与日志见 `build/display-backlight/` 和 `build/display-backlight-*.log`；
临时测试输出在本轮结束时清理，保留源码中的既有回归测试。
本轮的源文件/产物哈希、校验范围和清理记录见
[背光离线验证记录](backlight-validation.json)，后续修复见
[原生接入与真机审计](native-backlight-audit.json)。

修复后的真机采集确认（必要原始日志行与哈希已整理至审计记录）：WLED 接管成功，
framebuffer 和 IOSurfaceRoot 均为 1，原生显示自行请求开启；SpringBoard PID 32、
backboardd PID 65 均运行且 LastExitStatus 为 0，真实 TE/transfer 完成已推进
displayed 计数，用户确认屏幕显示界面。280 秒采集未出现 panic。独立
AppleARMMIPIDSIController 数量仍为 0，属于保留 raster 路径，不再据此判定显示服务
缺失。测试中同时收到严重操作延迟反馈；完整 IOLog 下串口约 9.5 KB/s，且存在
两万余行 SEP 失败。相同内核及驱动增加 `--serial-no-iolog` 后重复串口日志消失，
但用户反馈动画没有改善，不能将全部延迟归因于串口。该选项保留 UART 输入、
控制台和 os_log。

后续临时帧诊断测得：连续 TE 约 60 Hz；一个真实帧的 CPU 拷贝为 80.600 ms，
拷贝后的硬件完成等待为 23.065 ms，完成时没有 timeout 或 pipeline fault。
当前逐行 `readBytes` 拷贝有明显开销，但单帧样本不能解释全部长时间产帧间隔，
也不能证明背光寄存器操作造成帧率下降。GPU 后续接入需要同时考虑合成与提交，
当前显示后端仍只接受全屏、线性 BGRA 的单一主 surface。

按用户要求，本轮至此收尾；帧率问题尚未修复。临时帧计数/耗时代码已从驱动移除，
大体积和不完整诊断输出已清理，测量摘要保存在[审计记录](native-backlight-audit.json)。
最终低日志镜像为 `build/input/native-backlight-final-bridge/XnuBridge.efi`；
重建参数保存在 `build/input/native-backlight-final-invocation.json`。



离线分析命令可从一份原生报告保留全部进程、用户／内核帧、等待和继承关系：

```sh
python3 scripts/analyze-runtime-report.py REPORT.ips \
  --kernel build/display-runtime/kernelcache.sdm835.aes-compat-diagnostic \
  --cache-inventory build/display-runtime/cache-image-inventory.json \
  --output analyzed.json
```

工具核对报告与本地内核、共享缓存的 UUID，防止错误符号产生误导。
运行时采样使用一次全系统 stackshot；延时用 Bash 的 `read -t`，设备没有
`sleep` 或 `sysctl` 命令。原生崩溃报告仍是采集命令未完成时的证据来源。

原版 datamigrator 的同版本判断还要求 `/private/var/datamigrator/migrate`
不存在；满足时立即回调结果 2（无需迁移），不会更新旧结果。实际迁移完成时，
即使插件结果为 false，也会删除标记并更新版本及结果。因此旧 `success=false`
与本轮服务阻塞、是否执行了新一轮迁移必须分别判断。对应代码分析保存在
`build/display-runtime/initialization-io-policy/migration-semantics.json`。

## 原生框架与 ABI

参考样本为本项目的 `build/ios18-kext-analysis/kernelcache.release.ipad7c.macho`，
SHA-256 为 `aa65c36c486a8b5cb1d7a2d565d1d6b3cc228b3c172f8c69c5da664ee19832cf`。
从该映像恢复的继承链为：

```text
AppleH7DisplayPipe → AppleDisplayPipe → IOMobileFramebufferLegacy
 → IOMobileFramebufferService → IOMobileFramebuffer → IOService

QcomDisplay → IOMobileFramebufferLegacy
QcomMIPIDSI → AppleARMMIPIDSIController → IOService
QcomPanel → IOService       provider: AppleARMMIPIDSIDevice
PMI8998WLED → IOService     provider: QcomSPMI
```

所以，这一代参考 iPad 确实使用 `IOMobileFramebufferLegacy`。名称含 Legacy
不意味着这里沿用 iOS 12 的类声明或驱动模型。

| 原生类 | 实例大小 | 虚表槽数 |
| --- | ---: | ---: |
| IOMobileFramebuffer | 144 | 266 |
| IOMobileFramebufferService | 144 | 266 |
| IOMobileFramebufferLegacy | 6592 | 368 |
| AppleDisplayPipe | 40560 | 429 |
| AppleH7DisplayPipe | 41216 | 435 |
| AppleARMMIPIDSIController | 144 | 181 |
| AppleARMMIPIDSIDevice | 144 | 177 |

槽编号不含 Itanium 虚表头。完整证据见 [ios18-layout.json](ios18-layout.json)。
私有头和契约位于 `drivers/include` 及 `apple-*-abi.json`；原生类的
metaclass、实现、用户客户端和提交队列保留在 Apple Kext 中，没有定义同名替身。
未命名的 `nativeSlotNN` 只维持 ABI 槽位和重定位，不作为可调用接口。
`IOSurface::native*` 是明确标注的审计别名，不声称是 Apple 导出的原始符号名。

构建检查样本哈希、父类、实例大小、虚表布局、函数入口字节与依赖库，并检查
Clang 实际生成的子类虚表。更换 iOS 映像必须重新审计；相同版本字符串不能代替验证。
特别核对了 MIPI 收发槽 171–174、无 provider 参数的 `start_hardware()`、
原生默认 IOSurface 及 swap 请求中的 surface/矩形字段。

## 驱动职责

`QcomDisplay` 使用 Apple `IOProviderClass=AppleARMIODevice` 和
`IONameMatch=qcom,msm8998-dpu` 匹配。继承原生 `init`、`getWorkLoop`、
`newUserClient` 和队列管理，只实现硬件钩子。当前支持 INTF1/2、CTL0 和
VIG0/DMA0 的单层全屏线性 BGRA 输出；拒绝缩放、裁剪及 IOSurface tiling。
扫描模式必须与固件现有寄存器一致。UUID、分辨率、刷新周期及限制在原生
`start()` 发布服务前填入，避免首次匹配观察到不完整显示身份。

提交时将 IOSurface 逐行复制到保留的物理 scanout 区。单缓冲路径可能撕裂；
若 loader 明确交接两个已映射缓冲，则复制到后台缓冲并写 SSPP 地址和 CTL flush。
视频模式在真实 VBL 中断且锁存检查通过后向原生队列报告 displayed completion。
命令模式使用 ping-pong transfer-done 完成提交，TE/read-pointer 只提供 VBL 通知；
先处理已捕获的 transfer-done，再通知 VBL，避免新提交误用上一帧的完成中断。
交接的自动刷新已启用时保留该模式；其他命令模式提交使用 CTL flush/start。
100 ms 超时报告失败；双缓冲锁存超时会封锁后续提交，不伪造 VBL。
没有无 pending swap 时的周期性全屏 fallback 复制。

`QcomMIPIDSI` 匹配 `qcom,msm8998-dsi-ctrl`，使用原生 Apple 控制器的工厂和
`publishBelow` 发布真实 `AppleARMMIPIDSIDevice`。它实现接口/视频/高速模式控制、
通用及 DCS 短包、长包和读取；长写最大 4092 字节，读取最多 10 字节。
命令 DMA 使用 `IOBufferMemoryDescriptor`、`IODMACommand` 和标准 `IOMapper`。
没有 mapper 时必须明确声明硬件处于 DMA bypass。DMA 地址必须满足 32-bit 范围，
超时后若硬件仍访问命令区，保留映射和服务直到重启，避免释放正在使用的内存。

`QcomPanel` 在 Apple MIPI device 上匹配 `qcom,dsi-panel`，校验并执行 DT 中的
通用初始化序列，或接管已点亮面板。通过 `MIPIPanelPrepare` 和
`MIPIPanelSetBrightness` 平台函数提供准备状态及可选 8/16-bit DCS 亮度控制。
具体面板参数来自 loader/设备树，不写入具体机型的类名、文件名或 bundle ID。

时钟、供电、PHY、reset 目前要求固件已配置并保持。驱动没有完成冷启动的
MMCC/PMIC/PHY 时序、完整休眠恢复、任意 IOSurface 的 SMMU 映射、GPU 合成、
多层硬件合成、DSC、命令模式的冷启动、热插拔和原生系统亮度曲线。
PMI8998 WLED 的背光、保留电源的关屏/唤醒联动见上节。
统计、CRC、工厂接口返回不支持，不以空实现冒充这些功能。

## 设备树交接

`device_trees/msm8998.dtsi` 提供默认 disabled 的 display、dsi0/dsi1 和 panel 节点。
只有 loader 确实交接资源后才能启用。Apple 展平设备树数值为小端 OSData；
现有 `scripts/build-bridge.py` 统一完成 FDT 数值转换，不新增专用转换脚本。
iOS 18 用户态 IOMobileFramebuffer 先计数 `IODeviceTree:/arm-io` 的直接子节点，
因此物理 DPU 节点必须带 `device_type="display-subsystem"`；只发布 IOService
不足以完成枚举。DSI 和 panel 不重复声明为 display-subsystem。原生库及 QuartzCore
方法地址、字节哈希记录在 [ios18-discovery.json](ios18-discovery.json)。
以下 U32/U64 属性分别必须恰好 4/8 字节。

| display 属性 | 类型与含义 |
| --- | --- |
| qcom,display-handoff-version | U32，1 |
| qcom,display-handoff-flags | U32，必需位 0–4：时钟保持、供电保持、链路就绪、固件停止修改、scanout 保留；位 5 表示两个缓冲均已映射；位 6 表示命令模式 |
| qcom,display-interface | U32，1 或 2 |
| qcom,display-control | U32，0 |
| qcom,display-source | U32，0 为 VIG0，8 为 DMA0 |
| qcom,display-vsync-bit | U32，视频模式 INTF1 为 27、INTF2 为 29；命令模式为 12+ping-pong 编号 |
| qcom,display-pingpong | 命令模式必需的 U32，0–3；当前固件交接已验证 0 |
| display-width / display-height | U32，1–4096 |
| display-row-bytes | U32，至少 width×4，4 字节对齐，最多 65536 |
| display-refresh-hz | U32，24–120，必须与实际扫描时序相符 |
| dot-pitch | U32，原生接口使用的 pixels-per-inch，20–1000 |
| framebuffer-address | U64，CPU 物理保留区起点，4 KiB 对齐，40-bit 范围 |
| framebuffer-size | U64，保留区大小，最多 256 MiB |
| qcom,scanout-device-address | U64，硬件现有扫描地址，可为 IOVA，4 KiB 对齐且在 32-bit 范围 |
| display-uuid | 36 字符 UUID 加 NUL，共 37 字节，按真实输出提供 |
| display-panel | 可选 U32，指向 panel phandle；提供时会等待对应 QcomPanel 并检查准备状态 |

双缓冲的第二个地址为起点加 `align_up(rowBytes×height, 4096)`，两个缓冲必须
同时保留且已有连续设备地址映射。CPU 物理地址和设备 IOVA 不可混用。
display MMIO 为 MDP 基址 `0x0c901000`，范围至少 `0x8f000`，使用 SPI 83。

真机入口使用 `scripts/build-bridge.py --display-handoff-profile tools/display-handoff.json`。
构建时 version/flags 为零，EFI 在保留所需时钟、检查 GOP carveout、源管线、CTL、
DSI 版本与工作模式后，且在 ExitBootServices 后复查通过，才发布有效交接。
当前固件实际使用 CTL0 / VIG0 / INTF1 / ping-pong0 的命令自动刷新，GOP 扫描
地址为 `0x9d400000`、pitch 4320。默认 IOSurface 的 pitch 为 4352；逐行复制时
分别使用两者，不能把 IOSurface 的对齐要求当作固件扫描 pitch。

此次真机修正了 IOSurface 行跨度 getter：iOS 18 样本的 `IOSurfaceBytesPerRow`
保存于对象 `+0x68`，getter 为 `0xfffffff005a9ad4c`。旧别名读取 `+0x78`，
使原生创建的 surface 被错误判为 row=0。证据记录在 `ios18-layout.json`。

| dsi 属性 | 类型与含义 |
| --- | --- |
| qcom,dsi-handoff-version | U32，1 |
| qcom,dsi-handoff-flags | U32，必需位 0–4：时钟保持、供电保持、PHY 就绪、reset 已释放、固件停止修改；位 5 显式声明 DMA bypass |
| dsi-lanes | U32，1–4 |
| qcom,dsi-byte-clock-hz | U32，1–250 MHz |
| qcom,dsi-escape-clock-hz | U32，1–20 MHz |

DSI0/1 MMIO 分别为 `0x0c994000` / `0x0c996000`，节点范围 `0x400`。
主机校验 6G 控制器版本。驱动不会因提供属性而替 loader 配置 PHY 或 SMMU。

| panel 属性 | 类型与含义 |
| --- | --- |
| AAPL,phandle | U32，非零，Apple device nub 的节点标识 |
| panel-handoff-version | U32，1 |
| panel-handoff-flags | U32，3=资源保持且固件已点亮；5=资源保持且执行初始化序列 |
| panel-init-sequence | flags=5 时必需的二进制数据 |
| panel-brightness-bits | 可选 U32，8 或 16，未提供则不支持亮度函数 |

序列每条命令为 8 字节头加 payload：`type, reserved=0, length:LE16,
delay_ms:LE16, reserved:LE16, payload`。支持 0x05/0x13 的一字节短写、
0x15/0x23 的两字节短写及 0x29/0x39 长写。整个序列先校验再执行，
最多 64 KiB、256 条命令、每条等待 500 ms、总等待 5 s。

## 硬件资料与工具

寄存器模型对照 Linux v6.12 的 MSM8998 DPU、DSI host、6G 寄存器及 PHY 描述。
固定来源、哈希和文件长度见 [reference/sources.json](reference/sources.json)。
这些资料用于硬件寄存器核对；驱动匹配和生命周期仍使用 Apple IOKit。
参考项目只用于阅读旧实现与日志，构建没有对其目录的运行时依赖。

显示 collection 引入原生 Libm。该 Kext 在可执行段混放完整浮点常量表，
其中部分数值会被误识别为 ADRP 或扩展指令。打包器和 ISA 校验器共享
`AppleMathTables.inc` 中 13 个完整字节模板，并限制为 Libm；不跳过任意未知指令。
验证器另外检查原始常量字节保留。SDM835 ARMv8 指令兼容处理使用项目统一管线。

```sh
make drivers JOBS=3
make test-drivers
make verify-drivers
make kernelcache COLLECTION=display JOBS=3
make verify-kernelcache COLLECTION=display
python3 -m unittest discover -s tests -p 'test_driver_toolchain.py'
python3 -m unittest discover -s tests -p 'test_boot_bridge.py'
```

构建入口仍为 `scripts/toolchain.py`。产物是
`build/output/Extensions/QcomDisplay.kext`、`QcomMIPIDSI.kext` 和
`build/output/kernelcache.sdm835.display`。原生依赖闭包自动解析；独立输入快照、
manifest 和 verification 保存在产物目录。结果见 [validation.json](validation.json)。
离线检查验证结构和实现约束，不能证明私有 ABI 所有语义或真实 LCD 已运行。

带完整 APFS、Cryptex 和用户态依赖的真机集合为 `display-runtime`：
`python3 scripts/toolchain.py pack-drivers --collection display-runtime`。
诊断工具统一由 `python3 scripts/toolchain.py build-display-client` 构建，提供
standalone 可执行文件及开发 Bash 的 loadable builtin；使用系统的 IOKit、
QuartzCore 和 liblaunch 查询服务、CADisplay、进程与 job 状态。测试命令使用
现有 `scripts/test-bridge.py --console-command`，工具签名通过 bridge 的
`--trust-file` 加入本次启动信任缓存。

权限诊断使用临时 boot-args。原生 iOS 18 AMFI 已在真机确认接受
`cs_enforcement_disable=1`，条件是 `/chosen/debug-enabled=1`；该参数本身
没有解除这轮诊断库的 Sandbox mmap 拒绝。
`development-sandbox-mmap-path=/private/var/displayctl.dylib` 是本项目新增的
调试选项，仅在非 SECURE_KERNEL 的 DEVELOPMENT/DEBUG 内核且允许调试时生效。
它按 vnode 的完整路径只跳过该文件的 Sandbox mmap hook，其他 MAC hook 继续执行；
默认不启用。第 8 轮真机已验证诊断库加载成功并枚举到一个原生 framebuffer。
`process-exec-trace=2` 额外记录原生 exec 阶段和进程转换等待；值 3 继续记录
MAC 凭据转换回调，便于定位尚未完成启动的用户态服务。第 10 轮确认
backboardd 卡在 Sandbox 的 exec label update，AMFI 回调已经成功。
`development-sandbox-exec-path=/usr/libexec/backboardd` 在同样的开发条件下，
只跳过该路径的 Sandbox exec label update，其他策略及凭据转换仍执行。
该选项也接受以冒号分隔的多个完整绝对路径，每项独立精确匹配；
诊断原生显示初始化时，可将同样停在 exec 的 `/usr/sbin/cfprefsd` 加入列表。

第 11 轮起已记录完整 exec 返回。该轮结束来自用户手动重启，并非 native panic。
`cs_relax_platform_task_ports=1` 仍未让诊断 Bash 取得 backboardd task port。
工具的 `framebuffer` 命令使用原生 IOMobileFramebuffer C API；调用等待超过
3 秒时保留 worker、返回控制台并读取本进程线程栈，不中止系统库。

第 14 轮抓到初始化 worker 等待 CoreFoundation preferences XPC，且 cfprefsd
尚未完成 exec。第 15 轮加入该精确路径后，cfprefsd 完整 exec，原生 framebuffer
用户客户端开始创建；backboardd 对该客户端的打开随后被 MAC 策略拒绝。
该轮 256 MiB 内存出现持续回收，未捕获到诊断命令的完整返回。
统一 bridge 入口新增 `--arena-mib`，默认仍为 256；显示运行时使用 1024。
EFI 只从完整容纳请求的 ConventionalMemory 区间分配并保留全部页面，
分配失败就停止启动；boot_args 和设备树只声明实际保留的容量。
`iokit-wait-trace=1` 还会按原顺序记录拒绝 IOKit open/task port 的 MAC 策略及 errno，
保留原生策略回调、DTrace 和返回值选择。
`development-sandbox-iokit-path=/usr/libexec/backboardd:/usr/bin/bash` 是本项目
开发选项：只对这些完整 vnode 路径、connect type 0、
`IOMobileFramebufferUserClient` / `RootDomainUserClient` 生效。
它跳过 Sandbox 的 open hook 和同一客户端的 selector filter resolver；
其他 MAC 策略、原生 entitlement 检查、客户端创建和 native externalMethod 继续执行。
同样要求 DEVELOPMENT/DEBUG、非 SECURE_KERNEL 和 debugger enabled，默认关闭。
第 16 轮区分了 Sandbox 的 IOKit 拒绝和 AMFI 的 task port 拒绝；
第 17 轮确认仅放行 open hook 仍会被 selector filter resolver 拒绝。

Sandbox 拒绝需要与最初的 exec 等待分开看。原版 backboardd 的签名包含
`seatbelt-profiles=["backboardd"]` 和 system-container entitlement；cfprefsd
也有自己的 profile 和 system-group entitlement。临时 exec 放行跳过的是
安装新 Sandbox 标签的回调，保留继承标签，因此后续 IOKit 拒绝不能用来判定
原生 backboardd profile 缺少显示权限。诊断 Bash 也不能代表 backboardd 的权限。
AMFI 的签名放宽、task port 权限和 Sandbox 的 IOKit 策略是不同检查。

同版原生 Sandbox 的 exec 回调会在容器路径未缓存时，经
`HOST_CONTAINERD_PORT`（25）同步调用 containermanagerd，再安装 profile。
exec hook 的槽位、参数和 ucred.cr_label 偏移已与源码核对一致；静态证据、
原版 entitlement 和同轮日志定位见 [ios18-sandbox.json](ios18-sandbox.json)。

`display-runtime` 已加入原生 `AppleSEPKeyStore`，依赖闭包为 58 个 Kext。
保留 Apple 的 IOResources 匹配、AppleKeyStore 服务和原生 user client。
SDM835 没有 SEP，开发启动使用 `--development-no-sep`，同时配置 DT 的
`DoNotUseSEP` 与原生 `aks-endpoint=0`。第 18 轮证明该参数单独仍会卡住：
原版驱动先无限等待 AppleSEPManager，随后才检查实例的 endpoint-disabled 字段。
统一 bridge 对这版固件的精确指令模式重排，使该字段先于 manager 查询；
分支目标和正常 endpoint 流程保留，不伪造 SEP 或密钥操作成功。
该修正仅在 `--development-no-sep` 和 `--development-unsealed-root` 同时指定时应用。

第 19 轮以 1 GiB 内存、原生 Sandbox exec/IOKit 检查启动，成功进入串口 Bash。
两个 containermanagerd 均完成原生 exec；system daemon 打开原版 AppleKeyStore，
selector 17 返回原生 `kIOReturnNotReady`（`0xe00002d8`），不再等待 AppleSEPManager。
但 backboardd 和 cfprefsd 仍停在原生 Sandbox exec label update，
`+[CADisplay displays]` 仍为 nil。缺失 AppleKeyStore 与错误的 SEPManager 等待
是已确认的问题，但不能据此认定剩余标签等待也由密钥状态引起。
第 18 轮由用户手动复位；第 19 轮的 180 秒采集中没有 panic 或自动恢复。
随后准备新轮次时设备已在 UEFI，未捕获这次状态转换。

`process-exec-trace=3` 现在还记录原生 Sandbox 的容器请求
（Mach message ID `0x13392fd4`）进入与返回，保留原始同步调用和返回值。
第 20 轮捕获到 26 个请求进入、零返回，包含 backboardd PID 65 和
cfprefsd PID 99/100；因此已确认标签更新等待的边界是容器 RPC。
containermanagerd agent PID 73 与 system daemon PID 102 均完成原生 exec。
但尚未证明消息已送到 agent，或具体哪条 server 线程停止处理。
普通 task port 与原生只读 `task_read_for_pid` 都未获授权，未得到容器线程栈。
该轮主采集没有 panic；后续串口捕获了返回 UEFI 的日志，未捕获转换原因。

诊断工具的 `stacks` 命令调用原生 libsyscall stackshot 接口，按 launchd
job 的实际 PID 抓取 kcdata，包含加载地址、线程等待和队列信息。
使用这版 XNU 原有的 root 权限检查，不增加 task-port 放行。
数据通过串口输出并在主机重组，限制每个进程 256 KiB；只有所有块齐全才解码。

第 21 轮成功抓到容器 agent 的完整 5792 字节 stackshot：
`com.apple.containermanagerd.listener` 队列的线程 694，经
`MCMUserIdentityCache` 调用 `personaGenerationIdentifierWithError:`，
同步等待 `com.apple.mobile.usermanagerd.xpc`。主线程在正常 CFRunLoop 等待。
同轮 system daemon 的首个串口块被其他启动日志插入，未作为完整快照使用。
证据和去除共享缓存 slide 后的地址见
`build/display-runtime/handoff-21/container-73.json` 与
`container-73-frames.json`。这进一步定位了容器侧等待，但尚未定位 usermanagerd
不回复的原因。第 22 轮扩展到 usermanagerd、keybagd、cfprefsd 和 backboardd，
保留原生权限检查，仅关闭大量 IOKit 查询追踪以减少串口交错。
backboardd 的完整内核栈再次证实它在 Sandbox 容器 RPC 的 Mach receive 中等待；
keybagd 和容器 system daemon 的主线程则在正常 CFRunLoop 等待。

同轮只读 USB 导出取得 `/private/var/logs/usermanagerd.log.0`：原生 usermanagerd
反复启动后报告 persona manifest 的 `ENOENT`，尚未建立 XPC listener。
缺失文件为 `/private/var/keybags/persona.kb`，Data 上连 `keybags` 目录都没有。
早期 PID 17 的 `usermanagerd --init` 已打印完成；共享 Data/无 session 的路径
没有为这份重建的初始 Data 生成 persona 状态，不能把补跑 init 当作已验证修复。
该轮最终捕获原生 backboardd watchdog panic。

`displayctl initialize-persona` 仅用于这份空的、未加密的开发 Data：使用原生
Foundation `NSPropertyListSerialization` 生成已核对的 iOS 18 manifest v1，
初始 generation 为 1，用户字典为空。通过 root 权限、目录所有者/模式校验、
`O_NOFOLLOW` 和 `O_EXCL` 创建缺失的 `persona.kb`，不覆盖既有文件。
文件为 root 0600，写后 fsync；写入失败会移除本次创建的不完整文件。
这是用户/persona 元数据，不包含 SEP 密钥或伪造的解锁结果。
第 23 轮已验证：原生 Foundation 生成 133 字节的初始 manifest 后，容器 RPC
开始返回成功，本轮记录 59 次原生 RPC 返回，backboardd PID 65 和 cfprefsd
PID 99/100 均完成原生 Sandbox 标签更新与完整 exec，没有启用 exec/IOKit 放行。
usermanagerd PID 109 得以保持运行。诊断 Bash 打开 framebuffer 仍被其 Sandbox
权限拒绝，返回 `kIOReturnNotPermitted`；其 CADisplay 列表仍为 nil。
backboardd 已进入显示用户客户端路径，后续显示初始化需要继续检查。

完整的后续线程栈在设备 `/private/var/tmp/displayctl-stacks.txt` 保存，避免串口
启动日志插入快照字节。用户复位后已通过 USB 读回全部 583 行，6 份快照均满足
连续偏移、声明长度及 kcdata 头校验并由原生 XNU 解码器解析；没有补齐缺失字节。
backboardd PID 151 的 13376 字节快照包含 16 个线程，已有原生 CoreAnimation
render-server 和 `display.primary` 线程；主线程从 Accessibility 启动调用链
同步等待 LaunchServices。4 份 lsd 崩溃报告均为原生 `_LSDefaults userContainerURL`
在容器查询失败后主动 abort。具体查询错误仍需系统日志，不能仅凭退出点归因
到 SEP 或文件权限。显示线程存在也不等同于远端 CADisplay 已发布。
USB 还验证 `persona.kb` 仍为 133 字节，root:root 0600，父目录 root:root 0700，
原生 manifest 三个字段完整。第 23 轮 240 秒主采集没有 panic 或恢复。

新建未加密的开发 Data 镜像可统一使用
`python3 scripts/build-apfs.py image --initialize-persona`，镜像构建器将补齐缺失的
初始 manifest 和 root 目录权限，已有 persona 状态保持原样。默认不启用；
镜像报告记录请求与实际补齐结果。原生 manifest 合约、保留已有状态和目录检查
的 3 项测试通过；统一 bridge 的 26 项测试也通过。

第 24 轮验证了跨重启持久化：原生 usermanagerd 将初始 manifest 扩展为
1514 字节、generation 1 的用户/persona 记录，之后不再写入初始空字典。
第 27 轮 USB 读回 18 份完整快照；usermanagerd、两个容器服务和 lsd 保持运行。
backboardd 首次启动仍被 watchdog 终止，采样时处于原生 QuartzCore 的
`IOAcceleratorES` 初始化等待；第二次启动的主线程已进入正常 CFRunLoop。
迁移线程仍等待原生 DataMigration XPC，不能将进程存活视为显示初始化完成。

诊断 root Bash 的 class-10 primary-persona 容器查询实际返回
`2→(51) XPC_INVALID_REPLY`；该调用与 lsd 的用户身份不同。
同轮 CARenderServer lookup 返回 `0x44e`（`BOOTSTRAP_UNKNOWN_SERVICE`），
framebuffer 主显示查询返回 `kIOReturnNotPermitted`，CADisplay 为 nil。
工具已增加原生前台用户/按用户 bootstrap 查询，以及在首次显示查询前使用
本版 QuartzCore 的 `CA_SYSTEM_SESSION_CLIENT=1` 入口；仍保留原生授权。
`displayctl reboot` 已通过真机验证，执行原生 sync/reboot 并自动回到 UEFI。

第 28 轮确认 user sessions 已启用、前台 UID 为 501，按用户 lookup 成功。
独立原生 Bash 在首次查询前设置 `CA_SYSTEM_SESSION_CLIENT=1` 后，原生 QuartzCore
返回 6 个 CADisplay 对象，主显示为 `LCD primary`，另有 5 个未连接的 wireless 对象。
主模式刷新率为 60 Hz，尺寸仍为 1242×2208、preferredScale 为 0；硬件 registry
仍为 1080×1920。先前 root Bash 的 nil 不能作为前台显示服务不存在的证据。
结果干净读回于 `build/display-runtime/handoff-28/displays-clean.txt` 与
`bootstrap-clean.txt`，同轮 9 份原生 stackshot 均完整解码。

SpringBoard/PreBoard 的 62 份原始崩溃均停在本版 `FBSDisplayMode` 参数校验，
保留的尺寸为 1242×2208，preferredScale/gamut/HDR 均为 0，与实际硬件尺寸不同。
崩溃报告没有保存刷新率和 scale override 所在的浮点寄存器。
原生校验在 preferredScale 为 0 时使用 `BSFloatIsZero` 拒绝近似零的 scale override；
这不是通用浮点 finiteness 检查。后续读取原生 BackBoardServices 的主屏信息。
原生 Legacy timing getter 确实使用 Q32.32 秒，60 Hz 值为 71582788；当前驱动表示
与原生相符。不能依据显示模式断言去更换成 IEEE double。

第 29–30 轮原生 IOMFB C API 返回 1080×1920、429 PPI；原生 CADisplay/BKS
仍使用 1242×2208。BKS 全局 scale 为 1，第 30 轮直接读取的主 LCD 与
`immutableCopy` 的 `pointScale` 均为 0，preferredScale 也为 0，刷新率 60、方向
`rot0`。原生 BKS helper 对不同于显示数组首对象的副本读取 pointScale，
这个零值符合 FBS 的拒绝条件；不能用 BKS 全局 scale 1 排除该路径。
两轮各 9 份原生 stackshot 已完整导出、解码。

当前 bridge 已按原生 MobileGestalt 的读取规则发布 `/chosen/display-scale=3`
与 `display-rotation=0`，数值均为 Apple DT 的 LE32。第 30 轮仍读取到 scale 0；
USB 保存的旧 MobileGestalt 缓存中确有 24 字节屏幕记录
`1080,1920,429,scale=0,orientation=-1,screenClass=-1`。下一轮先读取运行时 chosen，
第 31 轮证实运行时 chosen 的 scale 为 3、rotation 为 0；`_MGRebuildCache`
未通过导出入口校验，工具保留了缓存，也没有调用未经确认的私有函数。
后续改为核对原生缓存版本及零 scale 记录，备份、逐字节比对并 sync 后，
仅移除这一份旧缓存。下一次启动由原生库自行重算。

第 32 轮已成功备份、比对并移除旧缓存；USB 验证备份 SHA-256 与删除前完全一致，
原文件不存在。随后第 33 轮使用相同 EFI、内核、客户端和启动属性，原生
MobileGestalt 返回 scale 3、orientation 0；BKS 主屏 scale 3，CADisplay 及
immutableCopy 的 pointScale 3，模式 preferredScale 3、60 Hz。仍为 6 个原生
显示对象，主显示 `LCD primary`。300 秒采集中没有 user breakpoint、panic 或
硬件 watchdog 恢复；此前 FBSDisplayMode 断言未再出现。backboardd job 快照为
PID 64、LastExitStatus 0。原生 swap 完成和物理首帧仍未得到证明。
本轮 USB 导出时磁盘 MG 缓存尚未生成，已验证的是原生库重新计算的实时答案。
9 份完整线程栈已解码：backboardd 主线程处于 CFRunLoop，已有 render-server
和 display.primary 线程；migration 线程采样时仍等待原生 DataMigration XPC 回复。
这一个采样尚不能确定服务端原因或完整等待时长。

scale 表示渲染像素与逻辑点的比例，分辨率不能单独决定 scale。
1080×1920 在 @2 下为 540×960 点，在 @3 下为 360×640 点；1242×2208 若采用
@3 则为 414×736 点。后者与物理 raster 不同本身不能证明驱动错误，
还需核对原生画布选择、合成缩放和真正提交的 IOSurface。

小米官方标称面板为 5.15 英寸、1080×1920、428 PPI；iPhone 7/8 为 4.7 英寸、
750×1334、326 PPI，Apple 公布的 UIKit 几何为 375×667 点、scale 2。
若保留当前 1080×1920 画布，直接将 scale 3 改为 2，会把同一点数控件的物理尺寸
缩小到原来的 2/3，并不会得到 iPhone 7/8 的逻辑尺寸。按两者标称 PPI 计算，
要取得与 iPhone 7/8 相同的每点物理大小，原生像素与点的比约为 2.63，
因此当前整数配置先保留 3。这里的 360×640 是画布换算值，尚不能代替运行时
`UIScreen.bounds` 的读取。未来若采用 375×667 点、@2 的 750×1334 渲染画布，
还须明确适配到物理 1080×1920 的合成缩放，并分别核对 `scale` 与 `nativeScale`。
参数来源：[小米官方](https://www.mi.com/mi6/specs)、
[iPhone 7](https://support.apple.com/en-us/111943)、
[iPhone 8](https://support.apple.com/en-us/111976)、
[Apple 显示几何与缩放说明](https://developer.apple.com/library/archive/documentation/DeviceInformation/Reference/iOSDeviceCompatibility/Displays/Displays.html)。

原生GPU表面提交可使用现有`displayctl swap-gpu`诊断：由系统Metal发现设备，原版
CARenderer直接渲染全屏IOSurface，等待同一原生队列完成，再经已核对ABI的原版
IOMobileFramebuffer提交并流式导出DPU PNG。它与`swap-test`共用提交和等待实现，
其中`swap-test`仍为CPU CoreGraphics参考帧。新增GPU模式不写CPU颜色像素，保留
原版显示控制对象及表面通过真实等待和抓帧；需要同时核对GPU作业计数、token及
实际PNG，不能从命令exit0推断系统UI或光学输出已通过。round68的GPU模式已取得
完整1080×1920红底白条扫描图，所有2073600像素与CPU参考帧逐字节相同；
同轮系统合成图层仍为黑帧，两种结果分别记录。

第 36 轮通过原生 SecTask 读取确认，诊断 Bash 没有
`com.apple.QuartzCore.displayable-context` 和 `com.apple.QuartzCore.secure-mode`。
原生 CAContext 服务端同时检查请求选项和进程 entitlement；创建有效 context
不能证明它取得了直接显示权限。原固件 PreBoard、backboardd 和 datamigrator
自身均有这些权限，不能把诊断 Bash 的缺项归因于原生应用。
工具构建现在只为独立 `org.darwin.development.displayctl` 签入这两项布尔权限，
由现有 bridge 信任缓存加载，仍由原生 QuartzCore 执行授权。

第 37 轮独立程序已在设备上读到两项真实 entitlement；原生 audit-token 查询
也确认上述四个系统进程具备这两项权限。测试 context 的 `isSecure` 已为 1，
红底白条保留 60 秒，用户确认屏幕仍无变化。第 38 轮加入限次驱动日志，
没有捕获 `verify_swap` 或 `verify_swap_surfaces`；`swap_begin_gated` 只在原生
关机时进入一次，active/queued 均为空。当前应继续追原生显示状态和提交入口，
不能在没有实际 request 的情况下断定 IOSurface 尺寸或缩放导致拒绝。

同轮已用设备原生文件系统将诊断文件从 `/private/var/displayctl.dylib`
改名为 `/private/var/displayctl` 并设为 0700；USB 独立检查确认 inode、大小和权限。
后续在此持久路径安装 standalone 可执行文件，避免用 Bash 宿主代表诊断程序的权限。
`/var/tmp` 会在启动时清理，只用于本轮结果文件。
既有 `tasks`/`processes` 命令统一为原生 stackshot 查询。
串口忙时使用现有测试工具的 `--console-byte-delay 0.15`，不另建发送脚本。
此设备的 Fastboot 操作与恢复检测全部指定 `-s edk2-fastboot1` 并核对 product；
USB 读写另核对设备路径及 GPT GUID，避免与其他项目设备混用。


## 旧 Window 错误

阅读了参考项目
`qemu_test/SPRINGBOARD_FIRST_FRAME_2026-08-07.md`，尤其小点 122 的纠正结论。
有效轮次已有 `BKDisplayStartWindowServer: main display info` 和
`CADisplay:LCD primary`；随后 SpringBoard 查询 `com.apple.CARenderServer`
返回 `0x44c`、端口为零，最后抛出 `failed to find the main CADisplay`。
Apple 的 bootstrap 定义中 `0x44c` 为 `BOOTSTRAP_NOT_PRIVILEGED`，与本轮
`0x44e` 的未知服务错误不同；不沿用旧项目对错误码的解释。
其他轮次的 backboardd 还出现 `cannot update immutable CADisplay` 和
`No window server display found`。这些来自不同初始化分支，不能跨轮拼接因果链。

新驱动补齐原生显示身份、默认 IOSurface、提交与 VBL 路径。当前真机日志已确认
原生 framebuffer 启动与 TE 中断；同轮 watchdog 仍报告 backboardd 未成功 check-in。
通过路径限定的调试选项加载诊断库后，原生 IOKit 枚举到一个 framebuffer；
第 9–13 轮 `+[CADisplay displays]` 返回 nil。第 12 轮原生
`IOMobileFramebufferGetMainDisplay` 直接返回 `kIOReturnNoDevice`；分析同版
Cryptex 的用户库后，补齐 DPU 的 `device_type`。第 13 轮该调用变为等待，
但尚未返回主显示，也未记录 framebuffer user-client open。backboardd 已完成 exec，
watchdog 仍报告没有成功 check-in，并曾诱发两次重启进程。
后续需在同一轮次核对原生库的等待位置、主显示创建、CARenderServer 有效端口、
真实提交完成和 LCD 画面。完整轮次证据见
[runtime-validation.json](runtime-validation.json)。
