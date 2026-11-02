# XnuBridge 目录

| 目录 | 内容 |
| --- | --- |
| `core/` | Mach-O 加载、Apple DT、启动参数与公共接口 |
| `entry/` | UEFI / raw 启动入口、ARM64 跳转与链接脚本 |
| `menu/` | 启动菜单、图形渲染、字体资源 |
| `services/` | 桥服务、USB Fastboot、开发载荷接收与生命周期 |
| `platform/` | 平台控制台、启动日志与看门狗 |
| `firmware/` | 固件协议、镜像校验、RPM 公共访问辅助 |
| `memory/` | 内存范围、固件保留区与 SMEM 保留区校验 |
| `handoff/` | CPU、GPU、显示、USB、UFS、输入、基带和 SMEM 接管 |

接管相关源文件及其专用头文件统一放在 `handoff/`。
跨模块引用使用相对路径。文件名保持不变，构建产物仍按原文件名生成。

主构建入口是 `tools/project/build/bridge.py`。
`tools/project/build/runtime.py` 负责使用已有构建报告重新封装 EFI。

`services/fastboot.c` 是桥的 USB 服务，不属于菜单。UEFI 入口负责启动、暂停和销毁；
前台通过通用 host 回调在应用层调度服务。USB 事件回调不解析命令、不写分区。
进入其他 EFI 应用前暂停，返回后恢复；进入 XNU 前停止 USB 并释放全部事件。
`--bridge-fastboot` / `--no-bridge-fastboot` 独立控制编译，米 6 默认启用。

在桥界面停留期间，电脑可直接使用 `fastboot getvar bridge-service`。
首次 USB 命令取消自动启动；下载时仍可移动菜单选择，完成后再确认启动。
前台 Fastboot 菜单项保留，用于进入原固件应用。

```sh
./project.sh device bridge --kernel kernelcache --dt board.apple-dt --ramdisk root.img --boot
./project.sh device bridge --flash-boot boot-sagit.img
./project.sh device bridge --reset
```

所有开发载荷先 `fastboot stage`，再 `oem bridge load <kernel|dt|ramdisk|trustcache> <CRC32>`。
单次下载上限 64 MiB，已保留载荷与新下载合计上限 144 MiB。断传 30 秒后重启传输，
丢弃未完成下载并保留已提交载荷。新载荷校验成功才替换同类旧载荷。
`getvar bridge-crc32:<名称>` 返回当前输入的 CRC32。

kernel 使用已解包的 ARM64 Mach-O；DT 使用构建产出的 Apple DT，不接受 FDT/IM4P。
ramdisk 是以 16 KiB 为单位的原始磁盘镜像，复制进内核保留区并通过
`/chosen/memory-map/RAMDisk` 交给 XNU 的内存磁盘驱动。使用它作为根盘时，DT 的
`chosen/bootargs` 需设置 `rd=md0` 及对应文件系统参数；它不会替换已有的 DriverImage。
未上传的输入继续使用内置版本，`continue` / `oem bridge boot` 启动当前输入。
载荷只保留在 RAM；仅 `flash:boot` 持久写入，沿用固件 GPT 查找、大小检查和回读校验。
