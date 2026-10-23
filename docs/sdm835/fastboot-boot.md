# Mi 6 无串口烧录与自动启动

入口为 `scripts/build-boot.py`、`scripts/flash.py`，以及 EDK2 的
`PlatformBootManagerLib/XnuBoot.c`。不需要设备串口，也不绑定开发机的手机序列号。

## 启动方式

持久启动：

```sh
make edk2-sagit
python3 scripts/build-boot.py --bridge PATH/XnuBridge.efi
python3 scripts/flash.py --image build/boot/boot-sagit.img --boot
```

每次上电由原厂 bootloader 进入 EDK2。EDK2 优先读取 bootloader 通过 FDT
传入的 ramdisk；没有传入时，从 GPT 名称为 `boot` 的分区读取。
加载器验证并解压 EFI，通过 `LoadImage` / `StartImage` 执行。
桥继续完成米 6 的硬件交接并进入内核，固件区仍为 6 MiB。

自动启动默认开启，`--boot-delay` 为 0–30 秒，默认 0，不等待倒计时。
EDK2 在菜单和 BootOrder 枚举前启动桥，不依赖串口编译开关。
读取载荷前和解压后都读取现有 UEFI 按键输入协议；按住音量键可取消启动并
进入 USB Fastboot。`--boot-delay 5` 可恢复五秒倒计时。
显式指定 `--no-autoboot`、没有载荷、校验失败或 EFI 返回时也进入 Fastboot。
内核进入后若挂起，恢复入口仍是原厂 Fastboot。

从原厂 Fastboot 临时启动整个组合镜像，不改写分区：

```sh
fastboot boot build/boot/boot-sagit.img
```

此路径要求 `sagit-mi6boot3` 固件。PEI 阶段解析 FDT `/chosen` 的
`linux,initrd-start` / `linux,initrd-end`，兼容 32 位和 64 位大端地址。
校验地址属于有效 RAM、避开固件与分配器后，将载荷复制到已登记分配 HOB 的
独立页面。BDS 使用 GUID HOB 取得副本，沿用相同的头部、LZMA 和 CRC32 校验。
临时载荷关闭自动启动、损坏或分配失败时进入 Fastboot，不回退执行磁盘载荷。

打包器只封装传入的 EFI，不会给旧桥补上驱动或用户态配置。启动完整系统时，
应传入包含 UFS 交接、驱动镜像、APFS 根卷和信任缓存配置的完整 XnuBridge。

从 EDK2 Fastboot 一次性启动 EFI：

```sh
python3 scripts/flash.py --payload PATH/XnuBridge.efi --boot
```

要求当前设备已在 EDK2 USB Fastboot。工具检查 `product=sagit` 和
`version-bootloader=edk2-fastboot-1`，发送 EFI，核对设备端 CRC32，再执行
`oem run-efi`。EFI 下载仍受 64 MiB 限制；更大的桥应使用压缩后的持久镜像。

本机调试继续使用此临时 EFI 路径。`scripts/build-bridge.py`、原始入口产物和
设备测试脚本保持原有行为。构建或提交默认启动代码不会改写设备 boot 分区。
纯 EDK2 固件没有自动启动载荷，仍进入 Fastboot；也可通过
`scripts/build-boot.py --no-autoboot` 生成无载荷的恢复镜像。

工具不会把 Fastboot 的命令确认当作 XNU 已启动。

## 镜像格式

外层保留已使用的 Android boot header v0、EDK2 gzip 和附加 Android DTB。
镜像 `name` 为 `sagit-mi6boot3`，表示固件支持传入 ramdisk 和零倒计时。
打包报告的 `temporary_boot_supported` 标记此能力。
新加载器仍可读取旧标记 `sagit-mi6boot2` / `sagit-mi6boot1` 的磁盘镜像。
修订 2 支持零倒计时；修订 1 仅支持恢复和带倒计时的自动启动。
打包器及烧录器会拒绝将零倒计时自动启动交给修订 1。
它是兼容性标记，不是数字签名。

ramdisk 是 64 字节 `MI6BOOT1` 头和 EFI 载荷，均为小端：

| 偏移 | 类型 | 内容 |
| --- | --- | --- |
| 0 | byte[8] | `MI6BOOT1` |
| 8 | u32 | 版本 1 |
| 12 | u32 | 头长 64 |
| 16 | u32 | bit 0 自动启动，bit 1 LZMA，其余为零 |
| 20 | u32 | 倒计时秒数，0–30，默认 0 |
| 24 | u64 | 压缩载荷长度 |
| 32 | u64 | EFI 原始长度 |
| 40 | u32 | 压缩载荷 CRC32 |
| 44 | u32 | EFI 原始 CRC32 |
| 48 | byte[16] | 保留，必须为零 |

压缩采用 LZMA-alone，8 MiB 字典，头中写入确定的展开长度。
加载器使用 EDK2 自带 LZMA 解码器，限制展开后 EFI 不超过 256 MiB。
主机限制整个镜像不超过 64 MiB，烧录前还核对设备报告的实际 boot 分区和下载容量。
CRC32 用于检测传输和存储损坏，不提供来源认证。

只接受唯一的非可移动 GPT `boot` 分区。分区协议由 EDK2 PartitionDxe 提供；
重复匹配会拒绝启动，不猜测 LUN 或固定 LBA。加载器、默认启动和临时启动的增量补丁为
`tools/patches/edk2-sagit/0005-sagit-boot-payload.patch` 和
`tools/patches/edk2-sagit/0006-sagit-direct-boot.patch`、
`tools/patches/edk2-sagit/0007-sagit-temporary-boot.patch`。
构建入口通过 `tools/edk2-sagit.json` 校验并应用包含这些修改的完整源码快照。
现有源码与上一版快照完全一致时自动应用增量补丁；冲突时保留开发者修改和暂存区。

## 没有串口时安装系统

先安装纯 EDK2，然后使用 Linux USB 存储导出器：

```sh
python3 scripts/flash.py --image build/edk2-sagit/boot-sagit.img --boot
make usb-storage
python3 scripts/usb-storage.py start --no-uart --device DEVICE
python3 scripts/usb-storage.py status --no-uart --device DEVICE
```

`DEVICE` 指向带有 userdata 的整个导出 LUN。工具读取 GPT，报告设备 GUID；
可以通过 `--disk-guid GUID` 固定预期身份。它不再默认使用开发手机的 GPT GUID。
APFS 安装仍使用绑定设备与镜像的 `plan` / `install`，详见根目录 README。

写入结束后：

```sh
python3 scripts/usb-storage.py flush --no-uart --device DEVICE
```

当前导出器没有 USB 重启控制命令。无串口模式完成主机块设备刷新后，需通过
电源键重启；报告不会将主机刷新伪报为设备串口确认。`reboot --no-uart` 明确拒绝，
避免静默假成功。有串口环境保留原流程。

## UniXNU / DarwinBoot 的取舍

参考版本：

- [UniXNUDxe，8f21e32](https://github.com/UniXNU/UniXNU/blob/8f21e32eeef93813ae25ee7871973318bc684afa/package/UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.c)：将固件中的引导应用注册为首个启动项。米 6 在 BDS 中优先执行现有桥，沿用自身的 ramdisk 载荷与板级交接。
- [UniXNU bootimg，141d16a](https://github.com/UniXNU/UniXNU/blob/141d16a43bfe46859487f32c0519c428cc473a49/scripts/unixnu/bootimg.py)：Android 容器打包与解包回读核对。
- [UniXNU-sheng，251b291](https://github.com/UniXNU/UniXNU-sheng/blob/251b291adf941b9953fe3ae66a808b9b626e164d/scripts/package-sheng-xnu-boot.py)：固件与 ramdisk 启动载荷分离。
- [DarwinBoot，d099354](https://github.com/UniXNU/DarwinBoot/blob/d099354d0eab34df5088ca1ba89c9c88bc3260bb/DarwinBootPkg/Driver/DarwinBoot.c)：UEFI loader 协议、Mach-O / ADT / trust cache 加载。
- [UniXNU 启动管理配置](https://github.com/UniXNU/UniXNU/blob/141d16a43bfe46859487f32c0519c428cc473a49/docs/reference/boot-configuration.md)：EFI 启动项、屏幕菜单与无输入超时。

本次参考的是打包和自动启动设计，没有直接引入 UniXNU 菜单或替换现有桥。
当前 DarwinBoot 只接受 EL1，并通过其协议从文件系统读取 kernel / ADT 等文件；
现有米 6 桥支持 EL1 / EL2，已包含 UFS、显示、GPU、USB 等板级交接。
完整切换需要移植这些交接和内存保留规则，再验证现有 APFS 桌面。
后续接入 UniXNU 时，可先以 `efi` 启动项执行现有 XnuBridge，保留当前设备路径。

## 验证范围

2026-10-23 已完成 EDK2 sagit DEBUG 构建和组合镜像打包回读。
23 项主机测试通过。实际 PEI 保留逻辑、UEFI 加载器和 EDK2 LZMA 解码器在
ASan / UBSan 下覆盖磁盘加载、32/64 位 FDT、原始 ramdisk 被覆盖后的副本加载、
非法内存范围、分配失败、损坏载荷、按键取消及 EFI 失败清理。
增量补丁与完整源码快照生成相同的 Git tree；上一版源码实际升级后逐文件一致，
开发者暂存区保持不变。

真机通过原厂 Fastboot 的 `boot` 命令临时启动组合镜像。UART 确认
`XNU payload source: incoming ramdisk`、`ExitBootServices OK` 和 Darwin 内核输出。
换入已包含 UFS 交接、驱动镜像及用户态配置的完整桥后，继续进入 launchd 和
Bash 5.2.21。带随机标记的命令返回 `uid=0`，`mount` 确认
`/dev/disk6s1 on / (apfs, local, read-only, journaled, noatime)`。
证据保存在 `build/direct-boot/hardware-system-incoming-ramdisk/`；此项验证到交互用户态。

本轮未使用分区烧录命令。本机原有固件和桥的发布路径产物经 SHA-256 核对未变。
实际按键恢复和持久镜像断电重启仍待单独验证。
