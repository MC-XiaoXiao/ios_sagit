# Mi 6 无串口烧录与自动启动

入口为 `scripts/build-boot.py`、`scripts/flash.py`，以及 EDK2 的
`PlatformBootManagerLib/XnuBoot.c`。不需要设备串口，也不绑定开发机的手机序列号。

## 两种启动方式

持久启动：

```sh
make edk2-sagit
python3 scripts/build-boot.py --bridge PATH/XnuBridge.efi
python3 scripts/flash.py --image build/boot/boot-sagit.img --boot
```

每次上电由原厂 bootloader 进入 EDK2。EDK2 从 GPT 名称为 `boot` 的分区读取
组合镜像的 ramdisk，验证并解压 EFI，通过 `LoadImage` / `StartImage` 执行。
桥继续完成米 6 的硬件交接并进入内核。读盘不依赖 Android bootloader 是否把
initrd 发布给 UEFI，也不扩大 6 MiB 的固件区。

自动启动默认开启，`--boot-delay` 为 0–30 秒，默认 0，不等待倒计时。
EDK2 在菜单和 BootOrder 枚举前启动桥，不依赖串口编译开关。
读取载荷前和解压后都读取现有 UEFI 按键输入协议；按住音量键可取消启动并
进入 USB Fastboot。`--boot-delay 5` 可恢复五秒倒计时。
显式指定 `--no-autoboot`、没有载荷、校验失败或 EFI 返回时也进入 Fastboot。
内核进入后若挂起，恢复入口仍是原厂 Fastboot。

一次性启动：

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

组合镜像自动启动读取的是设备 `boot` 分区，不支持 `fastboot boot` 临时启动该镜像。
工具不会把 Fastboot 的命令确认当作 XNU 已启动。

## 镜像格式

外层保留已使用的 Android boot header v0、EDK2 gzip 和附加 Android DTB。
镜像 `name` 为 `sagit-mi6boot2`，表示固件支持零倒计时。
新加载器仍可读取旧标记 `sagit-mi6boot1` 的镜像；旧固件可继续用于恢复和
带倒计时的自动启动，但打包器及烧录器会拒绝将零倒计时自动启动交给旧固件。
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
重复匹配会拒绝启动，不猜测 LUN 或固定 LBA。加载器及默认启动的增量补丁为
`tools/patches/edk2-sagit/0005-sagit-boot-payload.patch` 和
`tools/patches/edk2-sagit/0006-sagit-direct-boot.patch`。
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

2026-10-23 默认启动修改已完成 EDK2 sagit DEBUG 构建、无串口宏的 AArch64
启动管理器编译，以及真实 APFS EFI 的组合镜像打包回读。
22 项主机测试通过，其中实际 UEFI 加载器与 EDK2 LZMA 解码器覆盖 24 个用例：
零倒计时、可选倒计时、无输入设备、按键取消、解压后取消、旧镜像兼容、
损坏载荷、重复分区及 EFI 失败清理。增量补丁与完整源码快照生成相同的 Git tree。

本轮没有烧录设备。本机原有固件和桥的发布路径产物经 SHA-256 核对未变。
新组合镜像的原厂 bootloader 接受、实际按键恢复和断电重启链
尚待真机验证；不能将已有临时 EFI 启动记录当作此新路径的硬件验收。
