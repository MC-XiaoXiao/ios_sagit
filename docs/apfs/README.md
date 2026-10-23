# userdata APFS 安装与启动配置

使用本项目的 iPad 10.2、iOS 18.7.10（22H374，j171ap）固件。Apple 对 iOS 的
布局定义是至少两个 APFS 卷：System 和 Data；同一容器共享空闲空间。
这里创建一个 GPT `ios-apfs` 分区，内部是区分大小写的 System（角色 1）和
Data（角色 64）。最初安装时使用了共同的 volume group UUID；后续原生 ignition
查询证明这不符合当前 iOS 固件的布局。XID 3 已按原厂 System 的零卷组 UUID 修正，
并补齐 Preboot、xART、Update、Hardware 角色卷，见 [原生启动记录](bringup-2026-10-15.md)。

System 的根目录挂载为 `/`，Data 的根目录就是原固件 `/private/var` 的内容，
挂载为 `/private/var`。保留原来的 `/var -> private/var` 链接。fstab 通过卷 UUID
引用，不固定 `disk0s1s1` 之类会随 LUN 枚举顺序改变的名字。

## 本次设备结果

已写入设备并刷新主机分区节点，当前布局如下；主机 sd 字母仅供本次会话识别。

| 项目 | 结果 |
| --- | --- |
| `ios-apfs`（当前 `/dev/sdf18`） | 12020 MiB，约 11.74 GiB |
| Android `userdata`（当前 `/dev/sdf17`） | 43,842,011,136 字节，约 40.83 GiB；尚未格式化 |
| APFS 实际共享空闲量（checkpoint 修复 XID 42） | 2,090,414,080 字节；没有固定 Data quota |
| System 卷 UUID | `C1FFCBC9-9565-4E1F-8686-2000A9397B25` |
| Data 卷 UUID | `33888466-84EE-4238-A837-230EA4F61F00` |
| 原 GPT 备份 | `build/apfs/device-gpt-backup-final/` |
| 当前调试内核与启动桥 | `build/output/kernelcache.sdm835.apfs`（40 个 Kext）、`build/ufs-mount/preboot-bridge40/XnuBridge.efi` |

完整验证记录见 [validation.json](validation.json)。启动桥已按 System UUID 配置根卷，
尚未刷入 boot；完整 iOS 启动及 Android 双系统仍需下文列出的引导工作。
最初全量安装（XID 1）的直接 I/O 读回 SHA-256 与原镜像一致。当前 XID 3 的
主/备 GPT、卷 UUID/角色、零卷组、fstab、实际空闲量和全部增量块已另行验证，
组合文件系统通过独立 apfsck；旧完整镜像哈希不能代表当前分区。

## 可重复构建

```sh
python3 scripts/build-apfs.py bootstrap
python3 scripts/build-apfs.py firmware firmware/iPad_10.2_18.7.10_22H374_Restore.ipsw
python3 scripts/build-apfs.py image
make apfs-kernelcache
python3 scripts/build-bridge.py --kernel build/output/kernelcache.sdm835.apfs \
  --apfs-report build/apfs/ios.apfs.json --output build/apfs/bridge --watchdog-seconds 31
make test-apfs
```

bootstrap 固定源代码提交和 Go/ipsw 归档哈希，按补丁的正反向检查决定是否应用；
构建不使用参考项目的路径。第三方许可与版本记录在 `tools/apfs-tools.json`。
本项目扩展是 `tools/apfs-image`，第三方修改在 `tools/patches/go-apfs-*.patch`。

writer 从固件 APFS 直接流式复制数据，不先完整解包到主机目录；保留 uid/gid、
权限、set-ID/sticky 位、四种时间、BSD flags、硬链接、符号链接和扩展属性。
压缩文件复制原始 data fork 和 decmpfs/resource fork，避免解压数据后继续标为压缩。
不支持的特殊 inode 会失败，不静默变成普通文件。

SystemOS/AppOS Cryptex 原始封印镜像保存在 System 的
`/usr/standalone/firmware/Cryptex1/{SystemOS,AppOS}.dmg`，计入容量，保留原始哈希。
这是恢复载荷的存放路径，不是已经激活的 Cryptex 挂载点；激活仍需对应 iOS
恢复用户态和引导策略交接。没有声称把一个解包目录等同于经过认证的 Cryptex。

## 最小容量

读取 BuildManifest 的 `MinimumSystemPartition`（该固件为 6528 MiB），以它为下限；
用写入器的实际 APFS space-manager/B-tree 布局按 1 MiB 二分搜索首个能容纳全部
文件、元数据及 Cryptex 镜像的容量，再增加可配置的 Data 空闲余量（按本次用户要求，默认 2048 MiB）。
随后检查新增容器元数据后的实际 shared free count，不足则按 1 MiB 扩大；本次最终
容量为 12020 MiB，避免把扩容元数据占用误计入承诺的 2 GiB 空闲量。
两卷没有固定 quota/reserve，空闲空间共享。该余量是 bring-up 的安装余量，不是
对日常运行、升级或照片存储容量的承诺。完整输出、容量、UUID 和 SHA-256 写入
`build/apfs/ios.apfs.json`。

## 通过设备 EDK2 导出存储

无需打开菜单，可以从 EDK2 Fastboot 直接启动固件自带的 LinuxSimpleMassStorage：

```sh
fastboot -s edk2-fastboot1 stage \
  boot/edk2-msm8998/Platforms/Msm8998FamilyPkg/LinuxSimpleMassStorage/LinuxSimpleMassStorage.efi
fastboot -s edk2-fastboot1 oem run-efi
lsblk -o NAME,SIZE,MODEL,PARTLABEL,PARTUUID
```

本次设备导出了六个 UFS LUN。带 userdata 的是固件端 sda，主机端枚举为 sdf；
不要依据主机 sd 字母选择磁盘。稳定链接是
`/dev/disk/by-id/usb-Qualcomm_sda_1234567890-0:5`，逻辑扇区 4096 字节，
GPT UUID 为 `98101B32-BBE2-4BF2-A06E-2BB33D000C20`。
参考实际设备的新枚举结果；工具还会验证 GPT CRC、双表一致性、分区重叠和规划哈希。

项目现提供可重建的串口控制版本，无需长按电源退出导出模式：

```sh
make usb-storage
python3 scripts/usb-storage.py start
python3 scripts/usb-storage.py status
python3 scripts/usb-storage.py reboot
```

串口为 115200 8N1，命令为 `status`、`help` 和 `reboot`，以回车或换行结束。
主机工具先确认对端确为 UFS 导出程序，重启前检查主机挂载状态、刷新主机缓存；
设备收到命令后断开 USB 导出、逐个刷新 UFS 后端，再调用 Linux reboot 返回 EDK2。
USB 断开或刷新失败时不执行重启。手工串口操作也应先结束主机写入并刷新缓存。

源码和许可版本固定在 `tools/usb-storage.json`，串口代码位于 `boot/storage/`；
`build/usb-storage/` 保存构建、UART 和自动重启验证结果。内核增加 MSM UART/TTY
与 devtmpfs，按设备树的物理端口 `0x0c1b0000` 识别串口，避免误用蓝牙 UART。
保留原导出器的倒序 LUN 布局，使 userdata 继续对应 `...sda_1234567890-0:5`；
实际磁盘仍以 GPT GUID 核验。

`python3 scripts/build-usb-storage.py install-source` 将已构建载荷装入 EDK2 源码的
原 `LinuxSimpleMassStorage.efi` 路径，并保留原文件备份。Fastboot stage/run-efi
可立即使用新版本；设备固件菜单内嵌的版本随下一次 EDK2 固件更新生效。

```sh
python3 scripts/apfs-storage.py plan \
  --device /dev/disk/by-id/usb-Qualcomm_sda_1234567890-0:5 \
  --image-report build/apfs/ios.apfs.json --output build/apfs/device-plan.json
python3 scripts/apfs-storage.py install \
  --device /dev/disk/by-id/usb-Qualcomm_sda_1234567890-0:5 \
  --plan build/apfs/device-plan.json --image build/apfs/ios.apfs \
  --backup build/apfs/device-gpt-backup --erase-userdata
```

用户明确选择了清空 userdata。安装器从原 userdata 的尾部划出最小 APFS 范围，
保留其他 GPT 分区的 entry 字节；原 userdata entry 缩小，主签名区清零，剩余空间
不格式化。不支持保留 Android 数据的在线缩容。

写入前检查独立的 apfsck、镜像 SHA-256、当前 GPT 哈希、未挂载/未作为 swap、
没有 device-mapper holder，并获得独占设备句柄。备份主/备 GPT 元数据；镜像写入并
flush、完整读回 SHA-256 一致后，才写备 GPT 和主 GPT。GPT 备份不能恢复已经被
覆盖的 Android 数据，断电也可能使主/备 GPT 短暂不同，恢复需要人工核查备份。

仅有设备 ACL 时，Linux `BLKRRPART` 可能需要额外的 CAP_SYS_ADMIN。此时磁盘上的
双 GPT 已校验，安装记录会标记需要重新连接 UAS，主机旧分区节点暂时不代表新布局。

安装后还可使用直接 I/O 绕过主机缓存检查物理读回：

```sh
python3 scripts/apfs-storage.py verify-device-hash \
  --device /dev/disk/by-id/usb-Qualcomm_sda_1234567890-0:5 \
  --plan build/apfs/device-plan.json --output build/apfs/direct-readback.json
```

`apfs-image verify-device` 可按 GPT 提供的字节偏移读取真实设备上的两个卷，核对 UUID、
volume group、fstab 和实际可用余量，不依赖尚未刷新的主机分区节点。

## 当前设备的持久权限规则

只匹配 USB 磁盘及本设备 GPT UUID，避免授予所有磁盘访问权：

```sh
sudo install -m 0644 docs/apfs/99-mi6-ios-storage.rules /etc/udev/rules.d/99-mi6-ios-storage.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --action=change /sys/class/block/sdf
```

该规则使用户 xxiao 拥有这个 LUN，模式为 0600；不为其他用户或其他磁盘开放权限。
换设备、GPT UUID 或用户名时修改规则。重插 UAS 后仍会应用，不需要长期使用 root
运行 Codex，也不需要将用户加入可访问全部磁盘的 disk 组。

## APFS Kext 和启动选择

`apfs` collection 使用原厂 APFS、IOStorageFamily、corecrypto、AppleImage4、
AppleEffaceableStorage、AppleARMWatchdogTimer、CoreAnalyticsFamily、AMFI、Sandbox、
AppleBSDKextStarter 等依赖闭包，加上项目平台/GIC/GCC/SPMI/RTC/UFS 驱动。
依赖来自实际固件 plist；不是仅把 APFS 二进制拼接到内核尾部。

bridge 的 `--apfs-report` 设置 `/chosen/boot-uuid` 和
`rd=uuid boot-uuid=<System 卷 UUID>`，移除旧的 root-matching/rd/rootdev 配置。
没有把 GPT 分区 GUID 或容器 UUID误当 System UUID。build.json 记录实际选择。

2026-10-22 的 Sagit 真机记录已完成原生 UFS / APFS 启动、Data 读写和
SpringBoard 启动，用户确认显示桌面。使用显式 `--ufs-handoff` 的开发配置，
存储交接在 EFI 退出前后完成；启动后的块设备由 QcomUFS 提供。
启动测试采用临时 EFI，持久 boot 分区未改写。具体启动包、Data 补丁及
验证边界见 [Setup 真机记录](setup-skip-hardware-20261022.md)。

## 开发镜像跳过首次 Setup

参照 `ios-sweet`，Setup 完成状态写入 Data 卷的
`/mobile/Library/Preferences/com.apple.purplebuddy.plist`：设置
`SetupDone`、`SetupFinishedAllSteps`、`SetupVersion=11` 和
`daemonizeNeedsToRun=false`，并关闭 `com.apple.purplebuddy.notbackedup` 中的
mini-buddy 标记。只合并这些键，保留既有偏好。该步骤不会改动云配置/MDM 状态。

已运行过 iOS 的设备通过 USB 存储导出模式生成 Data 增量补丁：

```sh
python3 scripts/apfs-cow.py plan-data-device \
  --device /dev/disk/by-id/usb-Qualcomm_sda_1234567890-0:5 \
  --disk-guid 98101B32-BBE2-4BF2-A06E-2BB33D000C20 \
  --partition-guid 1AA2F04D-E440-4633-B455-CEB454F3431E \
  --development-setup-complete \
  --output build/ufs-mount/setup-complete-device.delta
```

随后使用本文的 `validate`、`apply` 和读回流程写入补丁，不生成完整 APFS 镜像。
设备树沿用已通过启动测试的 EFI 载荷，在既有 `build-bridge.py` 参数上添加
`--development-hactivation`。当前基线为用户指定会话中的
`build/setup-startup-20261022/work-interval-fix/bridge/XnuBridge.efi`，包含主屏
触摸投递、HID 报告及 UIKit CA_CLIENT 内核兼容修复。运行
`python3 build/setup-skip/boot-test/repack-latest-efi.py` 生成
`build/setup-skip/boot-test/bridge-latest/XnuBridge.efi`；只增加四个 Setup
设备树属性，其余内核、驱动卷、trust cache 及固件保持原包字节。

早期的 `bridge-current` 和 `bridge-touch` 留作排错记录，均遗漏 CA_CLIENT
内核兼容修复，用户反馈滑动会卡死，不作为当前启动候选。

`--development-hactivation` 仅接受本项目 `iPhoneMi6,1` / Sagit 设备树，并在
`/chosen` 写入参考项目使用的四个 production/security status 单元，以选择原生
Hactivation 分支。它必须与明确的 no-SEP、legacy Data 和 unsealed-root 开发模式同时
启用。当前数据卷没有预建的 `mobileactivationd` system container，因此不生成额外
`data_ark.plist`；设备树状态与 PurpleBuddy 偏好分别由两个显式选项控制。

2026-10-22 已通过 USB 导出写入 Data 补丁，并通过 Fastboot stage + run-efi
临时启动真机。用户确认屏幕为“应用图标和 Dock 桌面”；运行时读取的 Setup 完成
偏好与预期一致。成功启动报告位于 `build/setup-skip/boot-test/hardware-current/`，
汇总为 `build/setup-skip/boot-test/result-before-timing-profile.json`。
渲染服务导出的 PNG 全黑，不能作为实际屏幕证据。触摸是否可用另行记录，
不能从主屏出现或驱动匹配推断。

对齐 `work-interval-fix` 后的 `bridge-latest` 已重新启动真机，用户确认
“滑动点按正常”；SpringBoard 的 CA_CLIENT 创建返回 `errno=0`。
本次 Setup、EventTimingProfile 和触摸回归的完整记录见
[Setup 真机记录](setup-skip-hardware-20261022.md)。

## Restore 与双系统手动安装的区别

官方 Restore 的流程包含创建分区/文件系统、ASR 恢复映像、检查与挂载、修复 var、
安装 kernelcache、封印 System，以及固件/启动策略等阶段。可以借鉴其中的 APFS
恢复顺序，但 Mi 6 不能直接当成 iPad 运行 idevicerestore，也不能把本次离线重建
冒充官方 Restore 或已个性化的系统。原厂签名、SEP 与 Apple 硬件服务不会因写入
APFS 而自动具备。

手动双系统安装的步骤是：

1. 备份真实 GPT，并保留所有 Android 非 userdata 分区；确认 Android boot.img 的来源。
2. 从 userdata 划出独立 ios-apfs（本次含全部载荷与至少 2 GiB 实际余量为 12020 MiB）。
3. 生成并校验 System/Data、fstab、Cryptex 载荷，写入后完整读回核对。
4. 将缩小后的 Android userdata 重新格式化为设备 ROM 要求的 ext4，并保留其标签、
   加密 footer/metadata 预留及 Android fstab 约束。原 Android 数据已经选择清空。
   此步骤尚未执行；不能只缩 GPT entry 就把旧 ext4 当作已经缩容的文件系统。
   必须先让主机/恢复环境重新读取 GPT，再按新 userdata 的范围格式化。旧的主机
   sdf17 节点仍可能描述原来 52.6 GiB 的范围，不能对它直接运行 mkfs。可在主机执行
   `sudo blockdev --rereadpt /dev/disk/by-id/usb-Qualcomm_sda_1234567890-0:5` 后核对
   `lsblk`，或在 Android 恢复环境重新枚举分区后使用其 Format Data 功能。
5. 在 EDK2 中配置两个启动入口。iOS 入口启动本项目的 XnuBridge.efi，根卷按 UUID
   选择；Android 入口加载该 ROM 的原始 boot.img，不能加载现在已经替换为 EDK2
   的 boot 分区，否则会再次进入 EDK2。
6. 分别验证 Android 挂载新 userdata、XNU 找到 System/Data，再设置默认启动项。

SimpleInit 的示例支持从独立分区或文件启动 Android 镜像，EFI 镜像用另一种入口。
本机固件树包含 SimpleInit 相关代码，但当前构建默认进入 Fastboot/Shell；上游
SDM845 的成功示例不等于本机 SDM835 已验证。

当前 EDK2 没有 APFS EFI 文件系统驱动，不能假设它能从 System 卷读取 EFI 文件。
自动双启动需要独立的小 FAT ESP，或把引导桥打包进固件 FV；两者都还需要实际配置
和验证。本次先保留 EDK2 调试启动，通过 Fastboot stage/run-efi 使用自包含桥，
不把尚未完成的 iOS 引导替换成唯一的 boot 入口。

可复核的参考：

- [idevicerestore restore.c](https://github.com/libimobiledevice/idevicerestore/blob/master/src/restore.c)
- [EDK2-porting SimpleInit 多系统示例](https://github.com/edk2-porting/renegade-project.org/blob/master/en/multiboot.md)

## 保留原生卷的 System 文件补丁

`scripts/apfs-cow.py plan-system-device` / `apfs-image system-update-plan` 直接对
System 的目录树、extent reference 和 object map 做路径复制。Data、Update、Preboot
等卷保持原树，不再导入重建，因而不会因为这些卷内的多 extent、稀疏文件或 socket
而拒绝 System 配置更新。仅支持本项目创建、未加密、未密封、无快照的区分大小写
System，覆盖已有父目录下的 root 所有常规文件或添加常规文件；当前不创建目录。
替换支持空文件、单 extent 普通文件及 `com.apple.decmpfs` 内联压缩文件。补丁中的
压缩文件会删除压缩属性并清除 UF_COMPRESSED，添加普通数据流，保留其他 inode
扩展、所有权与模式；共享流、硬链接、resource fork 和复杂替换目标明确拒绝。

```sh
python3 scripts/apfs-cow.py plan-system-device \
  --device /dev/disk/by-id/usb-Qualcomm_sda_1234567890-0:5 \
  --disk-guid 98101B32-BBE2-4BF2-A06E-2BB33D000C20 \
  --partition-guid 1AA2F04D-E440-4633-B455-CEB454F3431E \
  --overlay build/wallpaper/usb-system-update-20261019/final-overlay \
  --output build/wallpaper/new-system-update.delta
```

继续使用下文 `validate`、`apply`、`verify-blocks` 和 `verify-update` 流程。
规划仅输出 delta；独立 apfsck 在只读 FUSE 组合视图中验证，不生成完整容器。
原生 crash 标志与 pending free queues 保留，因此检查使用 `apfsck -w`，不要求
已有卷带 clean-unmount 标志。来源身份绑定所读结构块的 SHA-256 清单，每次提交
重新核对，不依赖旧基线镜像或整分区哈希。规划至提交期间必须保持 USB 离线导出。

新节点只占用原位图的空闲块；旧 System 块在新位图中退休，不用作提交前写入目标。
描述环也整体迁入原空闲区，防止 APFS 扫描到新 checkpoint 提前提交；checkpoint
数据和内部池元数据只写入原状态未使用的槽。全部非零块 fsync/读回后才发布块零。
备份的旧块零只适用于同一离线事务的提交前恢复；系统启动后旧块可能已被重用，
不能只恢复旧超级块。后续修改须基于当前 USB 导出状态重新规划、完整校验。
Go 回归覆盖内联压缩替换、无关 xattr 保留、450 / 1,800 文件树分裂和后续批量替换、两次
提交前旧文件系统可读，以及提交后独立 apfsck；Python 回归覆盖来源结构变化拒写。

大文件的同尺寸小改动可改用 `--file-block-patch <descriptor.json>`，不必提供 overlay。
描述文件包含 System 相对 `path`、`delta`、完整 `source_sha256` / `derived_sha256`。
delta 为 `APFSPAT1`：8 字节 magic、两个 LE uint64（文件大小、记录数），随后每条
记录为 LE uint64 块号、4096 字节旧数据、4096 字节新数据。块号严格递增，文件大小
必须按 4096 对齐；旧数据和来源完整哈希都必须匹配。当前限定未压缩、未加密、无稀疏洞、
独占的普通 System 数据流；支持多 extent 以及对上次补丁结果继续增量更新。
只为修改块分配新空间，保留未修改数据，并拆分相应的文件 extent 和物理引用记录。
规划后的完整派生文件哈希必须匹配，仍须完整独立 apfsck、USB 提交和绕过缓存的变更块
读回。回归覆盖首次和再次补丁、相邻块跨新旧 extent 边界、提交前旧版本仍可读、
Data 不变，以及来源哈希/旧字节错误时拒绝生成可提交结果。

检查器另有 `tools/patches/apfsprogs-ios18-native-metadata.patch`，bootstrap 自动重放。
这是依据本设备 iOS 18 原始 Data 的 199 条记录新增的限定支持：只有 synthetic
purgeable 目录（parent inode 7）中的精确 `0xc004` 编码被识别为 purgeable 标记。
其他保留标志仍拒绝；索引成员必须设置 IS_PURGEABLE，分配大小构成的名称、所有权、链接
和 extent 校验仍保留。回归夹具验证这种编码通过、`0xc008` 拒绝。这是本项目基于
原生磁盘记录的兼容扩展，不代表上游检查器已支持任意较新 APFS 特性。
另有 52 个原始 Data/Update inode 标记为可清理但未进入 synthetic 索引；检查器
不再强制“可清理”必然意味着索引成员，仍要求每个索引成员具备可清理标志。
同一补丁支持普通文件继承目录统计键，将每个硬链接的实际分配字节（扣除稀疏洞）汇入实际统计值，避免将文件的
链接计数误算为子目录数量；回归同时检查错误的统计大小仍会被拒绝。
同时修正了两个克隆/数据流假设：WAS_CLONED 表示该 inode 来源于克隆，
WAS_EVER_CLONED 表示该 inode 曾被用作克隆源，不能强制前者蕴含后者；
private_id 是独立的数据流标识，带数据的文件也不能强制其等于 inode 编号。
流引用计数、物理 extent 所有者和共享标志的独立交叉检查继续保留。语义参考
[Apple File System Reference](https://developer.apple.com/support/apple-file-system/Apple-File-System-Reference.pdf)，
另有数据流编号与 inode 不同的完整 APFS 回归夹具。
诊断 `validate --metadata-only` 跳过旧压缩文件的载荷检查，报告明确不能用于 apply；
正式写入仍须通过不带该参数的完整检查。

## 本机增量写入

`apfs-image update-plan` 已补充离线 COW 更新路径。它导入现有卷的元数据，保留
原文件和流式 xattr 的物理 extent，仅将新文件及重建的元数据放入已核验的空闲尾部。
输出为 `APFSCOW1` 块补丁，不生成另一份完整固件镜像。System/Data 的 UUID、
卷组、硬链接、权限、时间、压缩属性及原始数据叉保留。当前仅支持本工具创建的
4096 字节块、未加密、未封印、无快照的两卷容器；拒绝克隆/重叠、多 extent 和
未知特性。它是离线更新工具，尚不是通用的 Linux APFS 读写挂载驱动。

```sh
python3 scripts/build-apfs.py bootstrap
python3 scripts/prepare-serial-bash.py
build/apfs/apfs-image update-plan \
  --source build/apfs/ios.apfs \
  --overlay build/ufs-mount/serial-shell/overlay \
  --output build/ufs-mount/serial-shell.delta
python3 scripts/apfs-cow.py validate \
  --delta build/ufs-mount/serial-shell.delta \
  --output build/ufs-mount/cow-validation.json
```

开发移植需要直接跳过 Setup Assistant 时，可仅修改 Data 卷中的 PurpleBuddy 完成偏好，
不传 `--overlay` 也不会生成完整容器镜像：

```sh
build/apfs/apfs-image update-plan \
  --source build/apfs/ios.apfs \
  --development-setup-complete \
  --output build/ufs-mount/setup-complete.delta
```

`scripts/apfs-cow.py plan-device` 同样接受 `--development-setup-complete`，可从设备当前
APFS 分区直接规划 Data 补丁。设备内容变化后应重新规划；补丁规划本身不写设备。

设备已运行过 iOS 时，使用原生 Data 更新路径，以保留多 extent 文件及其他卷的原生状态。
它只合并两个 PurpleBuddy 偏好文件，保留其他键与 `mobile` 的 `501:501` 归属，
要求 `mobile/Library/Preferences` 已存在：

```sh
python3 scripts/apfs-cow.py plan-data-device \
  --device /dev/disk/by-id/usb-Qualcomm_sda_1234567890-0:5 \
  --disk-guid 98101B32-BBE2-4BF2-A06E-2BB33D000C20 \
  --partition-guid 1AA2F04D-E440-4633-B455-CEB454F3431E \
  --development-setup-complete \
  --output build/ufs-mount/setup-complete-device.delta
```

对应底层命令是 `apfs-image data-update-plan`。后续沿用 `validate`、`apply`、
`verify-blocks` 和 `verify-update`；来源身份绑定规划读取的结构块，避免重复读取整分区。

本次通过 USB 导出的实际设备已提交 Setup Data 补丁：XID `112386 → 112387`，
375 块，补丁 1,539,024 字节。独立 apfsck 通过；O_DIRECT 回读全部变更块一致，
设备文件读取确认两个偏好文件及其余五个卷的超级块，GPT 未改动。
补丁和报告位于 `build/ufs-mount/setup-complete-device.*`，旧主块及提交记录位于
`build/ufs-mount/setup-complete-device-backup/`。后续已退出 USB 存储模式并完成
iOS 真机启动，用户确认进入有应用图标和 Dock 的桌面，见上文启动记录。

后续设备文件发生过修改时，应直接从 UAS 分区规划，不能再把旧的 `ios.apfs`
当成当前设备内容。下面的命令只读整个 LUN 的 GPT，再限定读取 ios-apfs 范围：

```sh
python3 scripts/apfs-cow.py plan-device \
  --device /dev/disk/by-id/usb-Qualcomm_sda_1234567890-0:5 \
  --disk-guid 98101B32-BBE2-4BF2-A06E-2BB33D000C20 \
  --partition-guid 1AA2F04D-E440-4633-B455-CEB454F3431E \
  --overlay /absolute/path/to/system-overlay \
  --output build/ufs-mount/next-update.delta
```

需要修正卷组或添加角色卷时，可显式传入 `--volume-group UUID`（零 UUID 移除卷组）
和 `--add-volumes volumes.json`。后者是包含 `name`、`role`、`uuid` 和可选 `overlay`
目录的对象数组；工具拒绝重复角色和 UUID，保留既有卷的身份与数据 extent。
缺少这些参数时保留现有布局。

再对新补丁执行 `validate`、`apply` 与 `verify-update`。目前更新元数据使用连续
空闲尾部；连续尾部不够时会拒绝，不会冒险覆盖已有数据。即使空间管理器还有其他
空洞，也可能需要后续补齐空洞分配功能。

只读 `apfs-cow-view` 通过 libfuse3 将原镜像与补丁组合成虚拟文件，供独立 apfsck
检查；磁盘上不会出现另一份 12 GB 镜像。bootstrap 需要主机已有 libfuse3 开发头文件
和 pkg-config。生成补丁时原容器只读打开。

设备进入 EDK2 UAS 并导出整个 UFS LUN 后，可以安装已验证的补丁：

```sh
python3 scripts/apfs-cow.py apply \
  --delta build/ufs-mount/serial-shell.delta \
  --validation build/ufs-mount/cow-validation.json \
  --device /dev/disk/by-id/usb-Qualcomm_sda_1234567890-0:5 \
  --disk-guid 98101B32-BBE2-4BF2-A06E-2BB33D000C20 \
  --partition-guid 1AA2F04D-E440-4633-B455-CEB454F3431E \
  --backup build/ufs-mount/cow-device-backup
python3 scripts/apfs-cow.py verify-blocks \
  --delta build/ufs-mount/serial-shell.delta \
  --device /dev/disk/by-id/usb-Qualcomm_sda_1234567890-0:5 \
  --backup build/ufs-mount/cow-device-backup \
  --output build/ufs-mount/cow-device-direct-verified.json
build/apfs/apfs-image verify-update \
  --device /dev/disk/by-id/usb-Qualcomm_sda_1234567890-0:5 \
  --offset 44311773184 \
  --report build/ufs-mount/serial-shell.delta.json \
  --output build/ufs-mount/cow-device-files-verified.json
```

安装器校验主/备 GPT、磁盘与分区 GUID、分区范围、整个原容器 SHA256 和补丁
校验值，拒绝正在挂载或有 holder 的设备。先持久保存旧块零，再写空闲块，fsync
并逐块读回；最后写块零提交新 checkpoint，再次 fsync 和读回。GPT 不变。
提交前中断时旧文件系统仍可用。旧超级块备份只用于本次写入失败后、设备再次启动
或发生其他写入之前恢复，不能当作之后任意时刻的文件系统回滚备份。

本次 Bash 补丁为 42902 个块，归档 176069832 字节，设备写入 175726592 字节；
重用全部原文件数据，净分配增加 3436544 字节。更新后共享余量为 2144940032 字节
（约 2045.57 MiB），Bash 及配置消耗了原先预留空间中的约 3.28 MiB。
`verify-blocks` 对真实块设备使用 O_DIRECT 检查所有改动块，避免只验证主机缓存。

串口配置位于 `/System/Library/LaunchDaemons/org.mi6.debug.serial-bash.plist`，
以 root 运行 `/usr/bin/bash`，标准输入输出连接 `/dev/console`。Bash 为校验过的
Procursus iOS arm64/ALL 包，配套 readline、ncurses 和 libiosexec。文件写入验证与
运行验证分别记录：启动 Bash 还需要根卷、launchd、dyld 和代码签名策略实际工作。

向固件缓存增加服务时，使用 `scripts/prepare-launchd-overlay.py --cache <原始缓存>
--overlay <overlay>` 同时生成 `System/Library/xpc/launchd.plist` 和对应 `.sig`。
修改缓存后不能沿用原厂 detached signature。生成器为实际内容生成 SHA-256
CodeDirectory，并校验所有页面；构建桥时通过 `--trust-file <overlay>` 将其 cdhash
与 Bash 可执行文件一起加入开发信任缓存。信任缓存生成器会再次核对签名与伴随文件。

当前无 SEP、共享 Data 的开发布局使用 `--firmware-report <report>
--development-no-sep --development-legacy-data`：前者增加原生
`filesystems/DoNotUseSEP` 属性，后者移除 `e-apfs` 并传入
`-apfs_shared_datavolume`。真机已验证 keybag 和 usermanagerd 初始化完成，
不需要修改它们的二进制文件。该模式用于本项目的未加密开发 Data 卷。

MSM8998 看门狗阈值为 20 位，最大可配置 31 秒；先前 90/120 秒配置会溢出。
`--watchdog-seconds 0` 关闭开发恢复计时器，适合后续服务启动和交互测试。
`handoff-73` 已验证修复的缓存通过原生签名检查，且 launchd 报告 early boot 完成；
Bash 的运行与交互证据继续记录在启动日志中。

2026-10-15 原生 XNU 已通过 System UUID 挂载根卷，原厂 launchd 和 dyld 通过
启动信任缓存并进入 Darwin Ignition。先前 zone 字段被破坏的问题已定位为链接器
重定位错误并修正；随后缺少 FSCompression 的根卷压缩读取和缺少 AES provider
的 AppleImage4 等待也已修正。当前已通过关联卷挂载、共享缓存、fsck、keybag、
usermanagerd 和 launchd 缓存签名检查，launchd 已进入普通服务启动。
`handoff-75` 的后续阻塞为原厂 watchdogd 退出引发 panic，尚无交互 Bash 证据。
新构建的 `build/bash-bringup/watchdog-bridge/XnuBridge.efi` 包含 Qualcomm
内核看门狗及显式开发适配，已通过 41-kext 离线检查，等待设备电源重启后真机验证。
原生驱动同时发现块计数警告：`apfs_total_blocks_alloced` 不能使用输出区间长度，
应统计数据流块（包括增量更新保留的文件及 xattr 流，不重复计算硬链接）。
`go-apfs-native-accounting.patch` 已修正工具并通过原始/COW 容器测试和 apfsck；
手机当前 XID 3 已随 Preboot 布局增量写入记账修正，并通过独立 apfsck 和全部
改动块的直接读回；原生驱动对本次记账及 Preboot 查询的运行时验证尚待启动。
验证记录见 [原生启动记录](bringup-2026-10-15.md)。

## 验证

### 原生 checkpoint ENOSPC

原生启动日志曾在事务 42 报
`checkpoint area out of space: stable: 2 0 - next 4 4 available 6 4 tx[42] 2 5`。
这里耗尽的是 checkpoint 数据环：原先仅预留 8 块，稳定事务占 4 块，后续
free-queue B-tree 增长后新事务需要 5 块。用户文件共享空间当时仍有约 2 GiB。

`go-apfs-writable-checkpoints.patch` 同时修正新镜像及离线 COW 更新的布局：
按容器大小和 free-queue 节点上限预留可写 checkpoint 容量，并从可用空间中扣除。
小至 512 KiB 的测试容器仍支持，当前 12020 MiB 容器预留 256 个描述块和
12,024 个数据块（约 47 MiB）。具体几何由工具计算，不把这组数写死在驱动中。

已有原生写入的容器使用专用修复，不通过文件导入重建卷：

```sh
python3 scripts/apfs-cow.py plan-checkpoints \
  --device /dev/disk/by-id/usb-Qualcomm_sda_1234567890-0:5 \
  --disk-guid 98101B32-BBE2-4BF2-A06E-2BB33D000C20 \
  --partition-guid 1AA2F04D-E440-4633-B455-CEB454F3431E \
  --output build/apfs-checkpoint-fix/device.delta
```

随后沿用上文的 `validate`、`apply` 和 `verify-blocks`，分别传入该补丁及新的
验证/备份目录。规划与提交均只校验涉及的超级块、checkpoint、spaceman、CIB
和分配位图；结构清单和身份摘要记录在补丁 JSON 中，不读取整个分区计算哈希。
提交前重查所有结构，先写原先空闲的主分配块和内部池槽位，读回后最后提交块零。
卷的对象映射、文件树、数据 extent 和原生延迟释放队列均保留。

独立 apfsck 检查组合视图的结构和分配一致性。该修复保留原生卷的未正常卸载标记，
因此此路径不使用 apfsck 的 `-c` 选项；没有伪造干净卸载，后续由原生启动处理。
2026-10-15 已提交 XID 42，全部 12,382 个改动块通过 O_DIRECT 读回，GPT 未变。
原生 checkpoint 后续已推进至 XID 81，Bash 在 Data 卷创建文件并重启读回成功，
未再出现 checkpoint ENOSPC。完整证据与保留的 watchdog 限制见
[启动记录](bringup-2026-10-15.md) 和 `build/apfs-checkpoint-fix/verification.json`。

格式字段依据 [Apple File System Reference](https://developer.apple.com/support/apple-file-system/Apple-File-System-Reference.pdf)
的 Container、Checkpoint 和 Space Manager 定义；容量策略为本项目实现。

Python 回归覆盖 512/4096 扇区、主/备 GPT CRC、仅 userdata 范围修改、镜像读回、
篡改规划/镜像拒绝，以及 UUID 根卷选择。Go 回归覆盖元数据/硬链接保留、fstab
幂等性及目录名哈希碰撞，并使用独立 apfsck 验证碰撞样本。

此次全量固件暴露了上游 dentry 排序错误：`name_len_and_hash` 的低 10 位是名称
长度，不能参与哈希碰撞时的排序。补丁屏蔽低 10 位后，再按名称排序；第一版镜像
曾被独立 apfsck 拒绝，修复后的主体系统镜像通过了全量检查。

来源：

- [Apple：APFS 的作用](https://support.apple.com/en-asia/guide/security/seca6147599e/web)
- [Apple File System Reference](https://developer.apple.com/support/downloads/Apple-File-System-Reference.pdf)
- [go-apfs-v2 v0.18.0](https://github.com/deploymenttheory/go-apfs-v2/tree/v0.18.0)
- [apfsprogs](https://github.com/linux-apfs/apfsprogs)
- 本项目真实 IPSW 的 BuildManifest 和 prelink plist，以及参考项目的存储/启动分析。


### 同一离线会话中的小增量

`apfs-cow.py validate` 和 `apply` 的 `--same-offline-session` 可复用刚完成规划时
计算的完整源哈希，并核对源超级块。调用者须确认规划后没有启动设备、写入源或
更换存储源；通过旧计划缓存源哈希的规划不能使用此选项。默认仍执行完整源哈希。
这可减少小改动的重复读盘时间，APFS 检查、分区身份检查、提交顺序和变更块读回
保持原有行为。

2026-10-15 的缓存路径增量已提交 XID 5，详情见
[当日记录](bringup-2026-10-15.md)。42,134 个变更块已直接读回通过；
原始 SystemOS/AppOS 镜像数据块继续复用。启动验证尚未完成。
