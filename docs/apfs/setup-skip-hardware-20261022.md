# Sagit 跳过 Setup 与增量写入记录

设备为开发用 Mi 6 / sagit，系统为 iOS 18.7.10 / 22H374。
本轮没有生成完整 System 或 APFS 容器镜像。Data 和 System 文件均以 APFS
增量补丁经 USB 存储导出模式写入；启动测试使用 Fastboot stage + run-efi。

## Setup

参照 [ios-sweet](https://github.com/dipa-studio/ios-sweet) 的开发设备配置，
在 `/chosen` 写入以下 32 位小端单元：

| 属性 | 值 |
| --- | --- |
| certificate-production-status | 0 |
| effective-production-status-ap | 0 |
| certificate-security-mode | 1 |
| effective-security-mode-sep | 1 |

Data 的 `mobile/Library/Preferences/com.apple.purplebuddy.plist` 合并
`SetupDone=true`、`SetupFinishedAllSteps=true`、`SetupVersion=11`、
`daemonizeNeedsToRun=false`，另一个 `com.apple.purplebuddy.notbackedup.plist`
关闭配置的 MiniBuddy/Silent 标记。原有其他偏好及 mobile 所有权保留。

Data 补丁 `build/ufs-mount/setup-complete-device.delta` 写入 375 块，
提交 XID 为 `112386 → 112387`。用户确认真机显示“应用图标和 Dock 桌面”，
运行时偏好导出与预期一致。启动报告为
`build/setup-skip/boot-test/hardware-current/report.json`，当时 EFI 的 SHA-256 为
`dac5b2b6543af472addf0e9c53f25854dea87e627551d925bbfc9f06df0e17b8`。

渲染服务导出的逻辑 PNG 全黑，直接扫描输出截图请求返回 busy/not-ready；
该 PNG 不作为物理屏幕证据。桌面结果来自用户观察。

## EventTimingProfile

在 System 增加
`/System/Library/EventTimingProfiles/SDM835.Default.plist`，内容为有效的 XML
空字典 plist（`{}`）。这是 UIKit 所提示的临时缺省文件处理，没有校准时序参数。
源文件位于 `tools/overlays/event-timing/`。

USB 导出模式下从当前设备直接规划：

```sh
python3 scripts/apfs-cow.py plan-system-device \
  --device /dev/disk/by-id/usb-Qualcomm_sda_1234567890-0:5 \
  --disk-guid 98101B32-BBE2-4BF2-A06E-2BB33D000C20 \
  --partition-guid 1AA2F04D-E440-4633-B455-CEB454F3431E \
  --overlay tools/overlays/event-timing \
  --output build/setup-skip/boot-test/event-timing.delta
```

补丁写入 389 块 / 1,593,344 字节；独立 apfsck、变更块直接读回及文件读回通过。
退出导出模式前完成主机和设备刷新。后续真机启动再次读取到该文件及 `<dict/>`
内容，SpringBoard 和 backboardd 正常运行，捕获的 UART 没有该缺失文件提示。
记录位于 `build/setup-skip/boot-test/event-timing-result.json` 与
`hardware-final/report.json`。只根据本次捕获范围描述日志结果。

补丁绑定规划时的设备结构；后续 iOS 写入 Data 后，再次使用应重新规划。

## 触摸回归与合并

用户报告桌面物理触摸无响应。检查发现用于 Setup 启动的 `hello-card-44`
基线仍使用 SynapticsTouch 1.0.1、QcomDisplay 1.0.23；旧版 inputctl 只有
registry 命令。运行时 framebuffer 仍发布
`IOMFBUUID=D60C6B4E-4EDA-4456-9C57-960E7509100D`，遗漏了此前已验证的
内置屏幕关联修复，详见
[主屏触摸投递修复](../touch/main-display-routing-fix-20261022.md)。

`build/setup-skip/boot-test/repack-touch-efi.py` 合并已验证候选中的
QcomDisplay 1.0.24 与 SynapticsTouch 1.0.2，前者将板级 UUID 保留为
`QcomPanelUUID`，后者修正 HID 宽高字段及抬起坐标。两份内核的源摘要比较确认
只有这两个本地 Kext 改变；GPU 保持 1.189.0，Metal 保持 1.52.4。

EFI 保留 Setup 设备树及当前 SystemOS 所需的全部 trust cache。内存中的辅助
Drivers 卷仅更新 inputctl，并追加对应开发签名缓存，提供真实 HID 事件监视。
该辅助卷随 EFI 装载，不写入设备 System 分区。

合并产物为 `build/setup-skip/boot-test/bridge-touch/XnuBridge.efi`，SHA-256：
`5ffc0819ca6190a860d6cf23e46e309218b8cf264908d6b7ceac3e1a34fe148e`。
该包捕获到 482 帧原生 digitizer 事件、6 次物理按下与抬起，RMI 错误为零，
但用户报告滑动导致卡死，因此该包验收失败。不能用事件到达代替界面可用结论。

## 核对最新会话后的基线纠正

用户指定“优化 GPU 驱动启动长帧”会话
`01a16324-b14d-7470-baaf-cad788d328a4`。核对后确认最新已测基线为
`build/setup-startup-20261022/work-interval-fix/bridge/XnuBridge.efi`，SHA-256
`bf612d9f05f99c9886052df46f2ed366210cd9ca8eb74ec99f9be49986d11d84`。
该会话已修复 UIKit 开始滑动时 CA_CLIENT 创建返回 EPERM 的内核兼容问题，
并有用户“滑动正常，已进入地区页”的验收。前述两个 Setup 候选错误地遗漏此项。

最终采用 `repack-latest-efi.py` 直接从该完整已测 EFI 提取全部载荷，仅增加
Setup 的四个 `/chosen` 属性。内核、Drivers 辅助卷、GPU 固件与 50 个 trust
cache 模块逐字节保留，不再从旧基线拼接。System 上的 EventTimingProfile
补丁已安装，保持有效。

当前候选 `build/setup-skip/boot-test/bridge-latest/XnuBridge.efi` 的 SHA-256 为
`9e79e824ba2995c808b837ca7926f855445e0cd639922f969ed62b7726300127`；
内核 SHA-256 与该会话一致：
`5e0f3d11db0a222ddf77da5a57aec6506e26554b3033b4e9f46c9764f3c77450`。
该包已重新启动真机。SpringBoard（PID 32）记录
`WI_CREATE2 stage=create flags=0x30000000 errno=0 kret=0`，用户随后确认
“滑动点按正常”。本次同时验证桌面物理滑动与点按，旧包的失败记录保留。
物理验收为 `build/setup-skip/boot-test/latest-physical-observation.json`；
完整启动和输入采集位于 `hardware-latest/`。EventTimingProfile 在此次启动也可读。

最终采集正常结束：120 秒监视记录 1,351 帧 digitizer 事件，物理按下/抬起
计数各从 2 增至 32，触摸结束时 contacts 为 0，`RMILastError=0`、
`HIDInputOffline=0`；本轮没有注入合成触摸。启动捕获无内核 panic，
SpringBoard 的 CA_CLIENT 创建成功。最终机器可读结果为
`build/setup-skip/boot-test/result.json`，输入明细为
`build/setup-skip/boot-test/hardware-latest/touch-monitor.txt`。
