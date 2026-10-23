# 可选 USB CDC ACM 内核控制台

`QcomUSB.kext` 增加了独立的 `QcomUSBCDC` Device personality。启用后，设备通过
USB 2.0 Full/High Speed 向电脑提供 CDC ACM 通信接口，输出内核串口日志，接收
内核串口输入；Linux 主机通常由 `cdc_acm` 识别为 `/dev/ttyACM0`，后缀以实际枚举
结果为准。这是设备端 gadget 功能，不能把普通 Host 端口或仅供电线变成串口。

**这是软件实现与离线验证结果，尚未完成真机枚举验证。** 现有 USB 栈要求有效的
连接器、电源、时钟、PHY、NoC、DMA 交接及隔离证明。SDM835 的板级资源交接仍有
未完成部分；SM8150 的 RPMh、GCC、PHY 和互连资源生产端不能用 SDM835 的实现
代替。因此加入 CDC 功能不等于任意 855 手机已经可以直接连线使用。

## 开关和 USB 身份

默认关闭，保留 `QcomUSBDevice` 与 Apple Device Family。启动参数启用：

```text
serial=3 qcom.usb-cdc=1 qcom.usb-cdc-vid=<VID> qcom.usb-cdc-pid=<PID>
```

VID/PID 必须显式指定为该设备有权使用的 USB 身份。项目没有分配 USB VID，
因此没有借用 Linux Foundation、Qualcomm 或 Apple 的标识。VID 范围为
1..65534，PID 范围为 1..65535。缺少或非法身份时 CDC 启动失败，日志打印原因，
不会自动切回 Apple USB personality。固件传递数值时建议使用十进制；桥接构建器
提供十六进制解析并生成十进制启动参数：

```sh
python3 scripts/build-bridge.py <原有板级参数> \
  --usb-cdc-console --usb-cdc-vid <VID> --usb-cdc-pid <PID>
```

`<VID>`、`<PID>` 和 `<原有板级参数>` 是待替换的占位符。USB console 参数不会
自动补齐板级资源、选择固件地址或关闭 DMA 安全检查。需要同时使用本项目带有
`ml_usb_console_*` 导出函数的内核；仅替换 Kext 不够。
`serial=3` 使内核 console 初始化并启用串口输入/输出；桥接构建器会自动添加它。

一个 Device role nub 只匹配一种 personality：

| 选择 | `USBDevicePersonality` | 驱动 |
| --- | --- | --- |
| 默认 | `Native` | `QcomUSBDevice` |
| `qcom.usb-cdc=1` | `CDCACM` | `QcomUSBCDC` |

启用 CDC 时同一控制器不再提供 Apple DeviceMux、同步或其他原生设备功能；
Host 模式不变。改变开关需要重启。物理接口还必须处于 Device/Sink 角色并有真实
VBUS/连接器准入，不会强制写死角色或绕过 Type-C 协议。
内核 console 是全局通路，多个控制器只能由一个活动 CDC 实例独占，直到旧实例
完全停止并释放 DMA，防止多个端口同时消耗日志或互相关闭输入。

## 主机使用

连接具备数据传输能力的 USB 线，待 USB 平台资源和 `QcomUSBCDC` 启动后，在
Linux 主机检查：

```sh
dmesg --follow
ls /dev/ttyACM*
```

用串口终端打开实际设备，例如：

```sh
picocom -b 115200 /dev/ttyACM0
```

终端须设置 DTR；驱动收到 `SET_CONTROL_LINE_STATE` 中的 DTR 后才从日志队列取
数据。115200/8N1 是默认的 CDC line coding，USB 链路不受 UART 波特率限制。
RTS 和 line coding 会保存、回读，不对应真实 UART 引脚。没有实现 SEND_BREAK，
描述符也没有声明该能力。Linux 用户需具备该 TTY 的访问权限。

这条通路接到内核已有的串口输入/输出接口，不会自动启动 shell，也不创建设备端
`/dev/ttyGS0`。是否可交互取决于内核 console 使用者及已有用户态控制台服务。
不要把主机成功打开 TTY 当成设备已经运行 shell。

## 传输和生命周期

- 通信接口 0：CDC Header、Call Management、ACM、Union 描述符，interrupt IN
  `0x82` 发送 10 字节 `SERIAL_STATE`。
- 数据接口 1：bulk OUT `0x01`、bulk IN `0x81`；FS 最大包 64 字节，HS 为
  512 字节。描述符包括 Device Qualifier 和 Other Speed Configuration。
- 支持地址、配置、接口、端点 HALT/状态查询，以及 GET/SET_LINE_CODING 和
  SET_CONTROL_LINE_STATE。拒绝错误方向、错误接口、长度和未支持的能力请求。
  USB2 远程唤醒、USB3、modem call management、break 均未声明。
- 复用现有 `DWC3::Control`、`Transfer`、`Core` 与 `DMARegion`。每个数据端点
  同时最多一个有限 TD；IN 为整包倍数时追加 ZLP，防止主机等待短包。
- 启动时分配好 DMA 缓冲和 TRB，运行期 IRQ/定时器不进行映射、分配或睡眠。
  IRQ 与 5 ms 定时器使用角色 nub 的共享 workloop。
- 配置、接口和 HALT 变更先停止相关 TD，等待真实 END 命令完成，再接受状态
  阶段；状态完成前不重新提交数据。END 等待上限 10 ms，EP0 策略上限 5 s。
- USB reset 清除旧配置及地址，保持 DCTL.Run 和事件环；保留已武装的 SETUP TD，
  防止同一事件批次中的 USBRST、CONNECT_DONE、首次 SETUP 被丢失。若 EP0
  正在 DATA/STATUS，则等待真实 END 完成后重新武装。数据端点也必须完成 END，
  才清除 HALT 和配置。普通总线复位不会软断开控制器。
- suspend 暂停新数据提交，恢复后继续。逻辑 USB disconnect 清空配置和 RX，
  仍消费事件并保留定时器，允许物理 Type-C 连接未变化时重新枚举。只有真实角色
  撤销、停止或故障才停止整个控制器。重新枚举保留尚未发送的日志。
- 回收只在 TD 完成、END 完成或已证明 whole-core halt/父级隔离后进行。停止
  超时保留 DMA 映射、驱动对象及资源租约，并请求父级恢复。绝不因超时而释放
  硬件仍可能访问的内存。

## 缓冲与边界

内核在解析 `qcom.usb-cdc=1` 后即可开始缓冲，因此没有物理 UART 也能保留一部分
USB 启动前日志。TX 固定 64 KiB，满时丢弃新字符；console 热路径采用 trylock，
竞争时也允许丢弃输出，避免 console 或 panic 路径死锁。RX 固定 4 KiB。驱动保留
一个至多 512 字节的已完成 OUT 块，当内核 RX 满或锁忙时保留未接收后缀，暂不
提交下一次 OUT，让 USB 主机得到 NAK 背压。

不会阻塞内核等待主机打开终端，不会无界增长内存。DTR 未置位、USB 尚未就绪、
挂起或掉线期间日志仍受 TX 容量限制。已经从 TX 队列取出并提交的至多 512 字节
可能在拔线、总线复位或取消时丢失；其余排队数据不会在重连时清空。

USB 控制器、资源驱动和内核调度尚未就绪时不能即时传输。panic 可继续尽力缓冲，
但若 IRQ/workloop 已停止，**不保证 panic 日志经 USB 发出**；本实现不是早期
固件 USB 调试器，也没有接管 UEFI 的 USB 协议。

## 离线验证与真机验收

```sh
python3 scripts/toolchain.py test-drivers --target SDM835 --test usb-cdc
python3 scripts/toolchain.py test-drivers --target SDM835 --test usb-cdc --sanitize
python3 scripts/toolchain.py test-drivers --target SDM835 \
  --test dwc3-control --test dwc3-transfer --test usb-dma
```

`usb-cdc` 检查 FS/HS 描述符结构及长度、合法/非法标准与类请求、状态阶段后的
DTR/配置提交、HALT 清除、reset、RX 背压后缀，以及真实 DWC3 EP0 模型中的
SETUP/DATA/STATUS 和 OUT 包容量。复位回归调用驱动使用的真实复位策略后端及
Control/Transfer 类，覆盖同批 USBRST/CONNECT_DONE/SETUP、DATA 中断、END
竞态、END 超时仍保留 DMA 所有权。该目标还用实际 ARM64 Apple Kext SDK 编译
`QcomUSBCDC.cpp`。这些测试不能证明电气链路、DMA stream ID 或固件交接正确。

真机需要分别验证：默认 Native 路径不变；CDC 完整枚举；FS 与 HS；无 UART
日志回放；双向长文本；DTR 开关；挂起/恢复；重复拔插；正在收发时 reset、
取消及角色转换；RX 消费者暂停时无输入丢失；错误资源/身份安全拒绝。

## 资料

- [USB-IF Communications Class 1.2](https://www.usb.org/document-library/class-definitions-communication-devices-12)：CDC 描述符与类请求定义。
- [Linux gadget serial 文档](https://docs.kernel.org/usb/gadget_serial.html)：设备端 serial gadget 与 Linux 主机 ACM 用法。
- [Linux v6.12 CDC ACM function](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/gadget/function/f_acm.c)：接口关联、line coding 和 SERIAL_STATE 的公开实现参考。
- [Linux v6.12 DWC3 gadget](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/dwc3/gadget.c)：DWC3 端点与异步传输取消的实现参考。

新增代码按 USB 协议独立实现并复用项目已有传输层；没有复制 Linux GPL 驱动。
