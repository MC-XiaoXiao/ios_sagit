# LG V40 / SDM845 EDK2

目标为 LG V40 ThinQ，代号 `judypn`。本次构建 EDK2 固件的 Android boot 包装，
不构建 V40 XNU 内核、不移植 Mi 6 的 iOS 外设驱动，也不操作手机。
按设备没有引出串口的要求，固定 `USE_UART=0`，使用 framebuffer 输出和 USB Fastboot。
编译与离线结构验证不能代替 V40 真机启动、USB 枚举和 UFS 写入验证。

## 位置和构建

```sh
make edk2-judypn JOBS=3
make test-edk2-judypn
make verify-edk2-judypn
```

脚本入口为 `scripts/build-judypn.sh`，固定 `judypn / DEBUG / GCC5 / no UART`。
首次准备由 `scripts/prepare-edk2-judypn.py` 完成，按 Git tree 中的精确提交初始化依赖；
可用时从 Mi 6 的本地 Git 数据获取独立副本。之后不需要重新下载已存在的固定版本。
宿主依赖沿用 Mi 6：GNU make、Git、Python 3、uv、GCC/G++、
`aarch64-none-linux-gnu` 工具链、gettext，以及宿主的 uuid/zlib 开发文件。
隔离的 Python 3.10 环境安装 Pillow 12.3.0 和 GitPython 3.2.0。

| 路径 | 用途 |
| --- | --- |
| `boot/edk2-sdm845` | V40 上游源码及独立子依赖 |
| `tools/patches/edk2-judypn` | V40 平台及无串口会话补丁 |
| `build/edk2-judypn/workspace` | V40 构建中间文件 |
| `build/edk2-judypn/boot-judypn.img` | Android boot v1 镜像 |
| `build/edk2-judypn/uefi-judypn.fd` | 7 MiB EDK2 FD |
| `build/edk2-judypn/AndroidFastbootApp.efi` | 可独立运行的 Fastboot 应用 |
| `build/edk2-judypn/FastbootPayloadProbe.efi` | 输出成功标记并返回的 EFI 测试载荷 |
| `build/edk2-judypn/verification.json` | 镜像、固件内容和隔离检查报告 |
| `build/edk2-judypn/source-versions.json` | 本次使用的固定版本 |
| `build/edk2-judypn/SHA256SUMS` | 交付文件校验值 |
| `build/edk2-judypn/logs/build.log` | 构建日志 |

Mi 6 原有的 `boot/edk2-msm8998`、`scripts/build-sagit.sh`、
`tools/patches/edk2-sagit` 和 `build/edk2-sagit` 保持原样。
V40 不把 Mi 6 的源码、Conf、BaseTools、SimpleInit 生成目录或工作区做成可写共享链接，
也不使用 Git alternates。两者只复用现有编译器包装脚本及相同补丁内容。
构建前保存 Mi 6 源码、版本、脚本、补丁和 boot/FD 产物的哈希基线；构建后逐个比对。

## 固定来源

旧 `edk2-porting/edk2-sdm845` 地址已指向统一的
[edk2-porting/edk2-msm](https://github.com/edk2-porting/edk2-msm)。
采用包含独立 [judypn 配置](https://github.com/edk2-porting/edk2-msm/blob/e1952621f419f8db60ed28271264e1b5184c571d/Platform/LG/sdm845/judypn.dsc) 的版本。

| 仓库 | 提交 |
| --- | --- |
| edk2-msm | `e1952621f419f8db60ed28271264e1b5184c571d` |
| edk2 | `e7aac7fc137e247edad22f7ee53b9a1fba227397`，与 Mi 6 一致 |
| edk2-platforms | `982212662c71b6c734b7578526071d6b78da3bcc` |
| SimpleInit | `29e062805776db5ab7a61d07ef040d43aed71161`，与 Mi 6 一致 |
| edk2-msm-binary | `d097b8a8bf3641e5e0c8b36ef1ac9d7cc1a0bd94` |

`.gitmodules` 中 `branch` 字段有旧值，不能作为这些子模块的构建版本；以父仓库
Git tree 中的 gitlink 为准。没有直接让 V40 使用 Mi 6 的板级内存映射或驱动二进制。

## 与 Mi 6 的功能对应

| Mi 6 功能 | V40 处理 |
| --- | --- |
| 内存映射表生命周期修复 | 上游已改为 `MemoryPeim` 在返回前同步调用 `InitMmu` 消费局部表，没有返回悬空指针，不重复套旧路径补丁 |
| Android boot 包装 | 保留 V40 上游 v1、Android 9.0.0 / 2020-09 字段；不套用 sagit v0 配置 |
| UARTDM 双向串口 / Shell | 按要求不移植；保持屏幕输出和 Shell |
| Qualcomm USB Fastboot 驻留 | 复用现有 QcomFastboot 传输实现，接入 SDM845 驱动和 V40 启动管理器 |
| 最大 64 MiB 下载、8 位 DATA 长度、CRC32 | 复用 `tools/patches/edk2-sagit/edk2-fastboot-app.patch`，仅应用到 V40 的 EDK2 副本 |
| `oem run-efi` | 保留 LoadImage / StartImage / UnloadImage；返回后重启 Fastboot 会话，适合没有串口的设备 |
| boot 原始镜像写入和写后读回 | 支持 `boot_a` / `boot_b` 和原始 boot v0/v1；按 GPT 动态找到 UFS LUN，校验两级 CRC、范围、容量和对齐 |

V40 USB 序列号固定为 `edk2-judypn`，与 Mi 6 的 `edk2-fastboot1` 不同；
`getvar product` 为 `judypn`。除明确的 boot 分区外拒绝写入，erase 和 set_active 不提供。
`flash boot` 根据两个 boot 分区的 Qualcomm GPT active bit 50 选择当前槽；
两个槽都标记或都没有标记时拒绝猜测，使用者可明确指定 `boot_a` 或 `boot_b`。
支持 `getvar current-slot`、`has-slot:boot`、`slot-count` 和相应 boot 分区容量查询。
Fastboot 不修改槽属性；上游原有 BootSlotDxe 仍负责其启动时槽维护逻辑。

构建修补另外包括：BootShim 使用相同的交叉编译前缀、限制并发数，以及把上游
V40 固件中误引用的 G7 DTB 改为固定版本的 `judypn` 板级 DTB。
FD 位于 `0xce000000`，大小 `0x700000`；BootShim + FD 经 gzip 压缩后附加 V40 DTB，
再由上游 mkbootimg 封装成 Android boot v1，page size 为 2048。

## 离线验证和边界

`scripts/test-edk2-judypn.sh` 直接编译实际 QcomFastboot 源码，在模拟 GPT/BlockIo 上
通过 ASan/UBSan 检查 A/B 槽选择、拒绝模糊槽、boot v0/v1、截断镜像、只读介质、
GPT/分区表 CRC、重复分区、越界项、非法块大小，以及写入后的读回失配。
它不模拟 USB 控制器和真实 UFS 时序。

`scripts/verify-edk2-judypn.py` 独立解析 Android header 和镜像 ID，解压 kernel，
逐字节核对 BootShim、FD 和 V40 DTB；然后解开 FD 中的 LZMA 固件卷，
检查 QcomFastboot、AndroidFastbootApp 和测试载荷确实被打包为 AARCH64 PE32。
另检查无 UART 构建参数、独立子依赖及 Mi 6 哈希基线。

真机枚举成功后，可通过下面的无串口命令测试 EFI 下载与执行：

```sh
fastboot -s edk2-judypn getvar product
fastboot -s edk2-judypn stage build/edk2-judypn/FastbootPayloadProbe.efi
fastboot -s edk2-judypn oem crc32
fastboot -s edk2-judypn oem run-efi
```

屏幕应显示 `FASTBOOT_EFI_PAYLOAD_OK`，载荷返回后应重新枚举为 `edk2-judypn`。
这些仍是待真机验证的行为。本次没有运行 fastboot、adb、串口或刷写命令，
没有验证 LG V40 的具体运营商版本、解锁状态、原厂 ABL 兼容性或屏幕/USB/UFS 运行。
