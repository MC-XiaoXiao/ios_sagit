# iOS 18 的 iPod 回退与 iPhone 分类

2026-10-18 完成固件静态分析、原版 AArch64 分类函数的 QEMU 验证和候选引导桥构建。
2026-10-19 基于最新 GPU 1.137 / Metal 1.0 / Display 1.0.18 完成真机启动、
原生 `MGCopyAnswer`、重启及重新生成的持久缓存验证，设备已识别为 iPhone。

## 根因

本项目使用 iPad7,11/12、iOS 18.7.10、22H374 固件。
原版 `/usr/lib/libMobileGestalt.dylib` 的 UUID 为
`BEED77DE-987A-347E-ADA9-88634A6BE56A`，加载地址为 `0x19ceeb000`。
分析直接读取本地 SystemOS Cryptex `build/apfs/firmware/094-33089-038.dmg`。

```text
设备树根节点 model
  -> IODTPlatformExpert::getMachineName / getProductName
  -> PEGetProductName -> sysctl hw.product
  -> MobileGestalt 按名称前缀分类
  -> DeviceClassNumber / DeviceClass
```

XNU 实现位于 `kernel/iokit/Kernel/IOPlatformExpert.cpp` 和
`kernel/bsd/kern/kern_mib.c`。`product/product-name` 是独立描述属性，
修改它不能改变此处的 `hw.product`。

原版固件的关键位置如下，地址均为未滑动 VA：

| 位置 | 含义 |
| --- | --- |
| `0x19cef071c` | once 回调，将 `hw.product` 传入 sysctl 字符串读取器 |
| `0x19cef0638` | sysctl 字符串读取器，128 字节缓冲区 |
| `0x19cef0608` | 返回原生 once 缓存中的产品字符串 |
| `0x19cef0394` | 设备分类初始化函数 |
| `0x19cef03d8` | 比较产品前六字节与 `iPhone` |
| `0x19cef0590` | iPhone 分支设置类别 1 |
| `0x19cef0588` | iPod 或未知产品的回退分支，设置类别 2 |
| `0x19ceecc7c` | 获取原生分类枚举 |
| `0x19cef5654` | DeviceClass 回调，根据枚举选择 CFString |
| `0x19cef7784` | DeviceClassNumber 回调，尾调用枚举读取器 |
| `0x19cef6f0c` | ProductType 回调，使用相同的原生产品字符串 |
| `0x1e8ea78c8` | 类别名称表：1→iPhone，2→iPod，3→iPad |

回调绑定通过原版问题表核对。问题键为
`Base64(MD5("MGCopyAnswer" + key))`，去掉末尾 `=`：
DeviceClass 对应 `+3Uf0Pm5F8Xy7Onyvko0vA`，
DeviceClassNumber 对应 `mtrAoWJ3gsq+I90ZnQ0vQw`，
ProductType 对应 `h9jDsbgj7xIVeIQ8S3/X3Q`。
IDA 将部分相邻函数边界合并，以上入口以实际指令和表内地址为准。

原先根节点 `model = "Xiaomi Mi 6 (sagit)"` 没有命中任何已知前缀，
原版分类函数因而回退到 2。这解释了先前记录中的
`ProductType=Xiaomi Mi 6 (sagit)` 和 `DeviceClass=iPod`。

## 修改

`device_trees/xiaomi-sagit.dts` 根节点改为 `model = "iPhoneMi6,1"`。
这是移植项目自定义产品标识，前六个字节进入原版 iPhone 分支；不对应量产 iPhone。
`compatible` 仍为 `xiaomi,sagit`、`qcom,msm8998`，`target-type` 与
`platform-name` 仍是 SDM835，描述名称仍为 Xiaomi Mi 6。MobileGestalt 没有打补丁。

原生重新计算时的预期答案：

```text
hw.product / hw.machine = iPhoneMi6,1
ProductType              = iPhoneMi6,1
DeviceClass              = iPhone
DeviceClassNumber        = 1
```

类别判断与显示几何、`main-screen-class` 精确匹配表、固件中的 iPad UI 资源、
基带/电话能力分别有自己的实现，不能由类别 1 推断这些功能已适配。

## 离线验证

```sh
make analyze-device-identity
python3 -m unittest discover -s tests -p test_boot_bridge.py -v
```

`scripts/analyze-device-identity.py` 只读 Cryptex，提取并校验原版分类代码和
字符串的固定 SHA-256，编译真实板级 DTS，并检查 Apple DT 转换后的 `model`。
随后在 QEMU 中于原 VA 执行未改动的原版分类函数。
环境替身提供 `strncmp`、关闭的日志、栈保护值，以及已初始化的原生
`hw.product` once 缓存。这不是完整 iOS 或公开 MGCopyAnswer 的端到端测试。

| 产品输入 | 实际分类枚举 |
| --- | --- |
| Xiaomi Mi 6 (sagit) | 2 |
| 从本次 DTB 读取的 iPhoneMi6,1 | 1 |
| iPhone9,1 | 1 |
| iPad7,11 | 3 |
| iPod9,1 | 2 |
| Unrecognized | 2 |
| iphoneMi6,1 | 2，大小写敏感 |

七组原版代码验证通过；49 项引导桥主回归通过。
完整 `make test-bridge` 的 57 项测试全部通过，包含 NVRAM、QEMU 原始入口和 panic 后端检查。
代码范围和哈希、输入与验证边界见 `build/device-identity/verification.json`，
IDA 分析结果见 `build/device-identity/ida-evidence.json`。

候选产物在 `build/device-identity/bridge/`，包含 `XnuBridge.efi`、
`boot-bridge.img` 和 Apple DT。从现有 `initialization-memory-bridge` 配置重建，
内核与 trust cache 字节和该基线一致，保留其背光禁用参数。
设备树差异仅为根节点 `model` 和正常更新的 `random-seed`。
`build-command.json` 保存实际构建参数，`tree-diff.json` 保存逐属性差异。
针对最终候选 DTB 的原版分类复验结果在
`build/device-identity/candidate-check/verification.json`。

## 后续真机验证边界

2026-10-18 的离线分析未读取或修改真机 MobileGestalt 缓存。运行新配置时，应同时读回
`hw.product`、`hw.machine`、`ProductType`、`DeviceClass` 与 `DeviceClassNumber`。
如果前两项已更新而 MG 答案仍旧，应先备份并核对旧缓存，再由原生服务重新生成。
现有 `displayctl invalidate-gestalt-cache` 专门要求旧的 scale=0 屏幕记录，
不能当作通用机型缓存刷新命令，也不应直接手填机型缓存答案。

## 2026-10-19：最新项目合入与真机验证

目标聊天 `codex://threads/01a14f77-3391-7a93-bd3c-1b60c349ed8f` 的源码修改
已经在共享工作目录中。当前目录没有顶层 Git 仓库，因此这里按实际文件和产物
核对合入状态。最新 `ui-framebuffer-input-snapshot-bridge` 也已包含
`model=iPhoneMi6,1`；没有回退使用 10 月 18 日的旧候选。
最新 DTB 的七组原版分类函数验证和完整 57 项引导桥回归均通过。

直接从 EDK2 Fastboot stage / CRC / run-efi 启动最新镜像后，四核、Bash 正常，
但 `displayctl screen-info` 的原生查询仍返回 iPod、2、旧产品名。
新增 `displayctl device-identity` 直接通过 `sysctlbyname` 读取内核值，
并独立读取公开 `MGCopyAnswer` 和磁盘缓存，得到：

| 项目 | 旧缓存仍在时 | 处理缓存并再次启动后 |
| --- | --- | --- |
| `hw.product` | `iPhoneMi6,1` | `iPhoneMi6,1` |
| `hw.machine` | `iPhoneMi6,1` | `iPhoneMi6,1` |
| `ProductType` | `Xiaomi Mi 6 (sagit)` | `iPhoneMi6,1` |
| `DeviceClass` | `iPod` | `iPhone` |
| `DeviceClassNumber` | 2 | 1 |

根因是持久 MobileGestalt 缓存覆盖了已生效的新内核产品名。
旧文件版本为 `22H374`，7,717 字节，`CacheExtra` 中的 ProductType 和
DeviceClass 分别仍是旧产品名和 iPod。

新增显式命令 `displayctl invalidate-device-identity-cache`，仅在内核两项均为
`iPhoneMi6,1`、原生答案仍是旧产品名/iPod/2、缓存为普通文件且版本及旧产品字段
匹配时执行。它先复制完整原文件，逐字节比对备份与当前源文件并 sync，再移除旧缓存。
不写入任何机型答案，也不改变原有零 scale 缓存命令的条件。

本次真机备份路径：

```text
/private/var/device-identity-gestalt-8653386C-BAF8-4CA3-A6CA-5B0422B9282A.plist
```

新的原生进程立即返回 iPhone/1/iPhoneMi6,1。再次启动后同样返回上述值；
稍后的复查确认系统已自行写回 7,676 字节的 `22H374` 缓存，持久 ProductType
和 DeviceClass 也分别是 `iPhoneMi6,1` 与 `iPhone`。
原版 MobileGestalt 和 UIKit/SpringBoard 未打机型补丁。

诊断镜像从最新启动参数和驱动盘重建，内核 SHA-256 与最新基线相同：
`1d98b0a66397ee65022bfe63a6a427fa9f671aba87758e6cdfd173245cab9f5e`。
驱动盘 12 个文件读回核对，只有 `displayctl` 改变；GPU、Metal、Display 驱动及
133 个 AOT 程序保持原样。补充了诊断工具对应的 trust cache。
最终 EFI SHA-256 为 `58a99e3d44707a6cfdaff7ce8d0705b44b4ed8bcdd856a530fe15709f33f9e5d`。
全程通过内存 stage 启动，没有刷写固件分区；缓存备份与重新生成写入 Data。

最终启动确认四核，EFI 至交互 Bash 约 17.4 秒，机型查询成功且没有运行时 panic。
启动早期 `screen-info` 返回过 `SCREEN_INFO_PENDING`（状态 1），保留该失败记录；
同次启动稍后复查三个命令均返回 0，BKS 和 MobileGestalt 的屏幕值为
1080×1920、scale 3，backboardd 和 SpringBoard job 的 LastExitStatus 均为 0。
早期 GPU 快照提交数为零，本轮不据此宣称 UI 动画、帧率或电话功能已验证。
两次主动重启均复现已有的 `initproc exited namespace 2 subcode 0xa`，发生在
launchd 关机退出阶段，随后恢复 Fastboot；与最终启动期间没有 panic 分开记录。

所有命令、构建参数、原始 UART 和汇总位于
`build/device-identity/runtime-20261019/`：

- `verification.json`：重启前后、实时与持久缓存答案、证据哈希。
- `boot/`：未经缓存处理的最新原始镜像启动，MG 仍是 iPod。
- `diagnostics-boot/`：内核为新产品名、缓存为旧答案的直接证据。
- `cache-refresh/`：备份、比对、移除旧缓存及新进程查询。
- `final-boot/report.json`：当前实际启动的主报告；后续 attach 应引用此报告。
- `final-warm/`：同次启动稍后复查，含已写回的新缓存和屏幕结果。
- `reboot-diagnostics/`、`reboot-final/`：关机阶段 panic 的独立记录。
- `bridge/XnuBridge.efi`、`bridge-invocation.json`、`packaging-verification.json`：最终产物与构建核对。
- `changes.patch`：诊断与缓存迁移工具的源码差异。

可用 `python3 build/device-identity/runtime-20261019/verify.py` 复核证据。
设备保留在最终已验证的启动状态，所有 UART 采集工具已退出。
