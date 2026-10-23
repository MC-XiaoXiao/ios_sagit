# iOS 18 启动 Apple Logo 缺失：固件静态分析

分析日期：2026-10-19。对象是本项目实际采用的 **iPad7,11/12、iOS 18.7.10、22H374**，
不是工作区内另一个 iPhone 18.6.2 IPSW。本轮只读本地固件与源码，未连接、启动、刷写或调试真机，
未修改启动配置、系统镜像或驱动。分析副本、反汇编和核对结果保存在
`build/apple-logo-static/`。

## 结论

本项目有一个足以解释用户态启动 Logo 缺失的确定性资源不匹配：

* backboardd 冷启动的 Apple Logo overlay 通过 **ProgressUI** 加载图片。
* ProgressUI 根据 **DeviceClass** 选择资源后缀：iPhone=1 和 iPod=2 都选择 `iphone`。
* 22H374 的 ProgressUI 资源目录只有六张 `@2x~ipad.png`，没有 `~iphone` 或通用图片。
* 在当前 3 倍 scale 下，原生加载器的所有候选路径均不存在，返回空 CGImage。

屏幕参数 `1080×1920 / scale 3 / 429 PPI` 无法命中原生屏幕分类表，
是另一个已确认的问题，但 **`unsupported screen class` 本身不会停止窗口创建或图片查找**。
不能把这条错误直接等同于“backboardd 不创建启动画面”或“GPU 没有送屏”。

当前 DTS 已把产品名改成 `iPhoneMi6,1`。从旧 iPod 分类修正为 iPhone 后，
ProgressUI 仍然选择 `iphone` 后缀，因此这项身份修正不会自动修复 Logo。
本轮没有读取真机的当前 MG 缓存或实际执行分支。

## 原始输入与分析边界

| 输入 | 内容 |
| --- | --- |
| `build/apfs/firmware/094-32804-038.dmg` | 原始 System，提取 `/usr/libexec/backboardd` 和 ProgressUI 资源 |
| `build/apfs/firmware/094-33089-038.dmg` | 原始 SystemOS Cryptex，提取共享缓存中的 ProgressUI 等代码 |
| `build/activation-analysis/SpringBoard.analysis.macho` | 已有的同版原始 SpringBoard.framework 分段分析副本 |
| `build/device-identity/libMobileGestalt.analysis.macho` | 已有的同版原始 MobileGestalt 分段分析副本 |

ProgressUI UUID：`eaad30b2-5553-385f-b9c3-4451355bd10e`，原始基址 `0x20e8db000`。
所有下述地址都是未滑动 VA。分析用 Mach-O 保留原始映射字节，去除不适用的 LINKEDIT 等加载命令，
不能作为可安装、可执行框架；提取读取区间与 SHA-256 见 `ProgressUI.extraction.json`。
backboardd、SpringBoard UUID 和分析副本 SHA-256 见 `offline-verification.json`。

**反编译类型注意事项：** IDA 重建的 PUIProgressWindow 类型在部分成员上偏移八字节，
会把 `_deviceClass` 错标成 `_screenClass`，把 `_screenClass` 错标成 `_isSecurityResearchDevice`。
本结论以原始 ivar 元数据和实际 LDR/STR 偏移为准，不采用这些错误字段名。
`ProgressUI.ivars.json` 核对出的关键偏移为：

| 偏移 | 真正的成员 |
| --- | --- |
| `+40` / `+0x28` | `_deviceClass` |
| `+44` / `+0x2c` | `_screenClass` |
| `+104` / `+0x68` | `_displayScale` |
| `+136` / `+0x88` | `_appleLogo` |
| `+176/+177/+178` | `_forceInverted` / `_white` / `_showsProgressBar` |

## backboardd 实际负责创建用户态启动 Logo

```text
BKBootUIPresenter 初始化
  → _queue_updateOverlayForReason: ("init 2")
  → _queue_addOverlayForReason:
  → 获取 system activity assertion
  → _queue_continueAddingOverlayForReason:generation:
  → BKBootUIOverlayVendor currentOverlayWithLevel:
      isFirstBoot = YES → BKDisplayRenderOverlayAppleLogo
      isFirstBoot = NO  → BKDisplayRenderOverlaySpinny
  → BKDisplayRenderOverlayManager applyOverlay:withAnimationSettings:
  → BKDisplayBootUIRenderOverlay _presentWithAnimationSettings:
  → AppleLogo 子类 _prepareContentLayerForPresentation:
  → PUIProgressWindow (不显示进度条，不自行创建 CAContext)
  → 取其 CALayer，挂到 overlay 自己的 secure/displayable CAContext
```

| 地址 | 原始函数及作用 |
| --- | --- |
| `0x100092fd8` | Presenter 初始化 block，添加 underlay 并更新 overlay |
| `0x100091650` | 更新 overlay，处理 screen owner、启动完成和 sustain 状态 |
| `0x1000920ec` | 创建前检查 `BKSDefaults.hideAppleLogoOnLaunch` |
| `0x100091cd0` | 获取 vendor 返回的 overlay，交给 manager |
| `0x10008d7a4` | vendor 在 Apple Logo 和 spinny 之间选择 |
| `0x1000823e8` | AppleLogo 子类创建 PUIProgressWindow，`setVisible:YES` 并返回其 layer |
| `0x10002cb74` | 父类创建 secure/displayable CAContext、背景层及内容层 |

AppleLogo 子类调用 `initWithProgressBarVisibility:createContext:contextLevel:appearance:`，
两个 Boolean 实参均为 0、appearance 为 0。`0x20e8dc03c` 的实际指令把
`createContext` 转换成 options 的 bit 2；该调用不设置它。
所以不能把 ProgressUI 自己没有创建 remote context 误判为失败，context 由 overlay 父类创建。

原生背景与图片内容是独立的：PNG 查找返回空时，背景层和窗口依然可能存在。
这能解释黑色启动画面，或迁移期间出现进度条而缺少 Logo；静态分析不能证明某次实际帧已送屏。

## 图片为何查找失败

`-[PUIProgressWindow _productSuffix]` 位于 `0x20e8dd924`。
第一条指令是 `LDR W8, [X0,#0x28]`，明确读取 **deviceClass**：

| DeviceClass | suffix |
| --- | --- |
| 1，iPhone | `iphone` |
| 2，iPod | `iphone` |
| 3，iPad | `ipad` |

`_layoutScreen`（`0x20e8dda50`）选择 `apple-logo` 或黑色变体，
部分原生屏幕枚举选择 d3x/d5x 等专用图片。对于这里未识别的屏幕枚举，
记录错误后仍继续用通用 stem 调用 `_createImageWithName:scale:displayHeight:`。

加载器位于 `0x20e8de53c`。对普通 iPhone/iPod、3 倍 scale、`apple-logo`，依次查找：

```text
ProgressUI.framework/apple-logo@3x~iphone.png
ProgressUI.framework/apple-logo@3x.png
ProgressUI.framework/apple-logo~iphone.png
```

三个文件都不存在时，在 `0x20e8de6f4` 进入失败分支，追加
`no apple logo found`，随后返回空图像。黑色变体使用相同规则。
它没有降级扫描其他 scale，也没有跨设备后缀搜索 iPad 图片。

从原始 System 直接提取出的图片全集为：

```text
apple-logo@2x~ipad.png
apple-logo-black@2x~ipad.png
apple-logo-j317@2x~ipad.png
apple-logo-black-j317@2x~ipad.png
apple-logo-j320@2x~ipad.png
apple-logo-black-j320@2x~ipad.png
```

六张均为 600×600 的 CgBI PNG。原生路径查找使用确定的文件名，不会因为目录中有 Logo
图片就自动选中。离线核对还覆盖 iPhone 的 2 倍与 3 倍 scale，两种 stem 的候选路径均无匹配；
iPad 的通用 stem、2 倍 scale 可以匹配。详情见 `offline-verification.json`。

## 为什么同时报 unsupported screen class

`PUIEnvironment mainScreenClass`（`0x20e8de934`）查询 `main-screen-class`，
默认值为 -1。MobileGestalt 原始计算入口 `0x19cefaed0` 读取：

* chosen 的 `display-scale`、`display-rotation`；
* IOMobileFramebuffer 的实际显示尺寸、DotPitch。

它把短边、长边、scale、PPI 编码为一个整数后进行精确匹配。
在本版原生代码的 37 个已识别参数组合中：

| 参数 | 匹配情况 |
| --- | --- |
| `1080×1920 / 3 / 429`，key `0x43807800301ad` | 未匹配，screen class 为 -1 |
| `1080×1920 / 3 / 326`，key `0x4380780030146` | 原生表内组合，返回枚举 9 |

因此，不能把缺失的 `main-screen-class` 答案简单归因为少了同名设备树属性；
原始实现会从几何参数计算它。也不应为命中表而把真实 429 PPI 伪报为 326。
**即便屏幕分类已修复，现有 iPad 资源与 `iphone` 路径的冲突仍然存在。**

## SpringBoard 的角色与其他隐藏条件

SpringBoard 的 `SBUIStartupFromOverlayAnimationController _startAnimation`
（`0x1d2647dd0`）对已有 overlay 调用 `dismissWithAnimation:` 或 `dismiss`。
selector 已通过原始共享缓存 selref 核对，见 `selector-verification.json`。
这条已追到的启动过渡链负责退出 overlay；Logo PNG 的装载发生在 backboardd/ProgressUI。

另外有三个原生条件会影响观察到的启动画面，但本轮没有运行态证据证明它们是当前原因：

1. **同一次内核启动中的再次启动。** `BKFirstBootKernelSemaphoreToken init`
   （`0x1000315a0`）检查 `backboardd.firstboot_check`，`isTokenSet`
   （`0x100031684`）返回“该 semaphore 不存在”。结束 first boot 后创建它。
   这是本次内核启动周期的标记，不是 DataMigration/Setup 的首次安装标记；后续启动可选择 spinny。
2. **明确要求隐藏。** `hideAppleLogoOnLaunch` 为真时，`0x1000920ec` 直接跳过 boot UI。
   系统外壳 relaunch options bit 0 可将它设为真（`0x100074598`）；正常主外壳完成启动时清零
   （`0x100074fac`）。不能仅凭存在此分支断言配置已启用。
3. **正常退出。** `_queue_updateOverlayForReason:` 在 owner 显式 dismiss 或外壳完成启动后移除
   overlay；sustain 状态可以延后退出。activity assertion 未取得等分支也可取消添加。

## 修复方向与本轮交付

对“用户态 Logo 图片为空”，应先解决 **ProgressUI 的资源选择适配**：提供与真实设备类别、
scale 和原生 stem 对应的资源，或设计显式的通用资源回退。不能只把 `DeviceClass` 改为 iPhone，
也不能只改 screen class。未知屏幕的布局兼容是独立工作，应保持真实分辨率、scale 和 PPI。

这是一份静态根因分析，没有应用资源替换或二进制补丁。未分析 iBoot 阶段的 Logo 绘制；
不能由用户态 PNG 失败反推更早的引导器是否曾绘制 Logo，也不能断言当前显示硬件存在新故障。

重跑本轮离线核对：

```sh
python3 build/apple-logo-static/verify.py
```

该脚本核对原始反编译常量表、提取资源的路径与 PNG 尺寸，并记录输入哈希，
不执行 iOS 程序，不提供真机像素可见性结论。所有核对已通过。
反汇编主证据为 `ProgressUI.disassembly.txt`、`backboard-logo.disassembly.txt`，
各函数反编译 JSON 与提取脚本均保留在同一目录。
