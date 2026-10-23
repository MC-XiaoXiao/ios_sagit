# iOS 18 逻辑唤醒首帧时序与 Display 1.0.23

2026-10-20。本轮只读取本地固件、驱动和已有采集，进行主机测试及交叉构建；没有连接设备、发送 USB/UART 命令、部署或真机测试。

## 结论与修复范围

已修复一个确定的显示时序缺陷：当一次 workloop 回调同时处理 PP_DONE 和 VBL 时，原驱动先执行 surface 退休、原生完成回调及成功日志，再调用 `note_vbl_ISR`。这个原生函数读取调用当下的 continuous clock，并直接用两次调用的时间差更新刷新间隔；因此完成路径开销的变化会污染提交给 CoreAnimation 的 VBL 时间和间隔。

Display 1.0.23 改为先记录原生 VBL，再完成旧传输，最后通知原生 VBL 订阅者。完成回调可能重入提交，通知使用记录时保存的 count/time/delta。成功传输日志改为显式采集时最多四条，失败日志保留。新增离线回归中，恢复旧顺序会触发时间戳断言失败，新顺序通过。

**这修复了可复现的时序缺陷，尚不能证明当前观察到的亮屏卡顿已全部消除。** 旧 round166 中存在 261 次合并处理，较新的 round168 环形采集里没有合并处理；它们也没有覆盖本次反馈对应的完整唤醒窗口。不能把历史触发条件或主机模型当作当前真机收益。

## 固件中的逻辑关屏与重新渲染

依据项目当前使用的 iPad arm64 iOS 18.7.10 / 22H374 固件。共享缓存读取通过现有映射、原始 cache UUID 和连续文件 extent 校验；分析副本没有用于安装。以下地址均为原始未 slide 地址。

| 环节 | 原版地址 | 核对结果 |
| --- | --- | --- |
| `CSCoverSheetViewController setInScreenOffMode:forAutoUnlock:fromUnlockSource:` | `0x1d22cce64` | 先更新 screen-off、外观及行为；在 `0x1d22cd33c` / `0x1d22cd350` 分别构造 type 25 / 24，再经 `_handleEvent:` 分发。因此即使 DPU/DSI 保留供电，UI 仍会进入独立的逻辑关屏状态。 |
| `SBDashBoardSetupViewController handleEvent:` | `0x1d2681be0` | type 25 停止组件轮播并隐藏按钮；type 24 调用 `_startCyclingComponents:withDelay:`，延迟参数 **0.5 秒**，按钮和儿童图标也使用相同延迟。 |
| `_startCyclingComponents:withDelay:` | `0x1d2682b40` | 已有有效启动 timer 或已经轮播时避免重复启动；否则重置显示字符串，再创建原生 timer。0.5 秒属于固件策略，不能直接计为驱动掉帧。 |
| `_animateComponentsForNewCycle` | `0x1d2682cec` | 要求 CoverSheet 非 screen-off、view 有 window、appear state 为 2；随后更新字符串并启动组件动画。逻辑唤醒包含重新进入动画和更新图层。 |
| `SBDashBoardSetupView _createDisplayLink` | `0x1d2bf318c` | 创建 CADisplayLink，请求范围 80/120/120，加入主 runloop。请求值不代表本机 60 Hz 面板实际达到 120 Hz。 |
| `_animateCursiveForTimePassed` | `0x1d2bf32b0` | 按经过时间调用文字视图 `setTime:`，到期移除 display link。CPU 生成路径，CAShapeLayer 的 path/shadowPath 更新后进入 CA 合成。 |
| `SBDisplayLinkController` 的 unblank/blank 回调 | `0x1d281a5f8` / `0x1d281a6b0` | 更新 `_displayLinkWouldBeOff` 后重新计算 `setForceFixedRateLinksEnabled:`；没有证据支持通过驱动强制维持 Hello 动画或改其请求帧率。 |

开机多语言 Hello 属于 **SpringBoard 的 SBDashBoardSetupView**。Setup.app 结束向导的 `BuddyFinishedCursiveChildWelcomeView` 是另一套实现，不能混为同一个首屏：

- `BuddyDisplayMonitor wakeDisplay:`（`0x1000c7fec`）检查 `BLSBacklight.backlightState`；需要唤醒时构造原生 `BLSBacklightChangeRequest`，时间戳来自 `mach_continuous_time()`。请求被接受不等于首帧已显示。
- `BuddySceneDelegate sceneDidBecomeActive:`（`0x1000ec520`）进入 `SetupController didBecomeActive`（`0x100044df4`），处理锁定时间、inactivity timer 和导航统计；这段代码没有同步证明渲染已完成。
- Setup 的欢迎动画也创建 CADisplayLink（`0x100129564`）；`_animateCursiveForTimePassed`（`0x10012975c`）使用 `CFAbsoluteTimeGetCurrent()` 减起始值，再调用 `BuddyCursiveTextView setTime:`（`0x1000820ac`），生成和变换 CGPath、更新 path/shadowPath。

路径轮廓、渐变 mask、透明度和壁纸扩展会产生不同的渲染工作。已有 Mercury 首次初始化和着色器磁盘缓存属于其他已实施的改动；本轮没有把每次唤醒都当成冷编译，也没有修改这些代码。更完整的笔画与动画分析见 [Welcome 静态分析](../gpu/welcome-static-analysis.md) 和 [Mercury 生命周期记录](../gpu/hello-mercury-lifecycle-review.md)。

原生绘制链为：

```text
逻辑背光 / CoverSheet 事件
  → Hello timer / CADisplayLink / CPU 路径与 CA 事务
  → backboardd 的 QuartzCore / Metal 合成
  → QcomMTL / IOGPUFamily / QcomGPU
  → GPU 真正完成与 IOSurface 就绪
  → IOMobileFramebuffer / QcomDisplay
  → DPU 源地址锁存 / PP_DONE / read fence 退休
```

CADisplayLink 的真实帧间隔和提交期限依赖 timestamp / targetTimestamp，声明刷新率不是实测帧率，参见 [Apple CADisplayLink 文档](https://developer.apple.com/documentation/QuartzCore/CADisplayLink)。

## 为什么完成路径会污染时序

原版 IOMobileGraphicsFamily 的 `note_vbl_ISR`（`0xfffffff005c23a74`）调用 slot 244 获取 continuous time，增加计数，将 `now - previous` 写入 `+0x110`，将 now 写入 `+0x108`。只有没有 previous 时才将传入的纳秒周期转换为 fallback；它不会把所有间隔强制钳成 16.67 ms。`nativeNotifyVBL`（`0xfffffff005c23b18`）把 count/time/delta 传给原生订阅者。

设 workloop 观察到 VBL 的时刻为 `T[n]`，完成路径耗时为 `C[n]`。旧顺序输出的间隔是：

```text
(T[n] + C[n]) - (T[n-1] + C[n-1])
= 实际观察间隔 + C[n] - C[n-1]
```

例如仅在模型中注入 3.5 ms / 0 ms 的交替完成成本，16.67 ms 的输入会变成约 20.17 / 13.17 ms。新顺序排除这个成本差；这不是新测得的设备耗时。旧映射清理、原生 surface/reference 释放、回调重入准备和逐帧日志都位于这段可变成本中。

历史采集的只读复算结果：

| 已有采集 | worker 事件数 | 同时含 PP_DONE 与 VBL 的 worker |
| --- | ---: | ---: |
| round166 | 755 | 261 |
| round167 | 1299 | 0 |
| round168 | 1005 | 0 |

round166 主 IRQ 中只有 4 次同时包含两位，但 worker 中有 261 次，说明部分状态在 filter 与 worker 之间合并。后两轮使用更新的 tearcheck 参数，round168 还包含映射复用，不能将三轮当作同配置 A/B。各段均为已有环形历史，包含静态停留或逻辑关屏的可能，不是本次唤醒的帧率验收。

## 实现与不变条件

- 新增 `DisplayInterruptDispatch.h`，生产中断路径与主机回归共用顺序：observe → complete → notify。只有捕获到真实 VBL 位才记录/通知，done-only 和无关中断不产生 VBL。
- 记录和通知均在原生 workloop 上；硬 IRQ filter 没有新增原生调用、锁、分配或清中断操作。采样仍是 worker 入口附近的时间，**没有修复进入 worker 之前的中断延迟或被合并掉的硬件边沿**。
- 命令完成仍要求 PP_DONE、源地址/pitch 锁存和 single-transfer idle。原生 fence、FIFO、失败隔离及 100 ms 超时保持；没有提前 signal 或以关背光证明 DMA 已停止。
- `onVBL` 使用记录时的本地 observation，通知仍在旧传输完成以后。没有清零唤醒时钟、虚构计数、调整 DSI/tearcheck/频率或接入硬件断电。
- 显式 `display-scanout-capture=1` 时，原 timeline 的 kind 1–10 保持，追加 11=`VBLObserved`、12=`PowerRequested`、13=`PowerApplied`。11 的 aux 为原生 continuous timestamp；12/13 的 aux 高/低 32 位为 controller/display 状态。timeline 的主 ticks 仍是 absolute clock，不能跨真实系统休眠直接混用两种 epoch。

## 离线验证与交付

通过：显示和 MIPI DSI 主机测试、显示 ASan/UBSan、19 项驱动构建回归、3 项显示引导桥测试。构建回归的 DTS 检查保留已有 simple_bus_reg / unique_unit_address 警告，测试退出为 0。

新增回归覆盖 9 ms / 0 / 3.5 ms / 0 的可变完成成本、完成回调重入下一笔、VBL-only、done-only、无关中断和 video 模式不消费 command-done。独立顺序变异检查恢复旧实现后断言失败（SIGABRT），新实现退出 0。

`QcomDisplay.kext` 1.0.23 已交叉构建并通过现有 arm64 Mach-O、导入与虚表检查；本轮没有重新打包完整 kernelcache/EFI。候选及证据保存在：

- `build/gpu-runtime/ui-logical-wake/drivers/QcomDisplay.kext/`
- `verification-drivers.json`、`ordering-regression.json`、各 `test-*.log`
- `historical-irq-audit.json`、`firmware-code-receipt.json`、原版反编译和 annotated `.asm`
- `baseline.json`、`source-before/`、`source-after/`、`changes.patch`、`review.json`

后续获准真机测试时，应在相同 GPU/Metal/固件配置下比较 1.0.22 与 1.0.23，并覆盖真正的逻辑熄屏→唤醒首批帧。按 power 事件、VBLObserved→Complete→VBL、GPU 编译/提交时间和实际画面关联，分别检查固件预定 0.5 秒、上游渲染空档和显示完成成本。若没有合并中断仍然卡顿，本修复不能解释该次现象，应沿 CPU 路径/合成资源准备继续定位，不能以降低动画请求或放松 fence 掩盖。
