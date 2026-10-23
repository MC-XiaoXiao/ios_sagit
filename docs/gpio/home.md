# 前置 Home／指纹键

`GoodixFingerprint.kext` 和 `FPCFingerprint.kext` 按传感器厂商命名；共享代码位于
`FingerprintHID.kext`。实现范围是前置 Home 按下/松开，不包含指纹录入、认证或
Touch ID。所有驱动都继承 Apple `IOHIDDevice`，使用原生 personality 匹配
`AppleARMIODevice`，经 `IOHIDInterface` / `IOHIDEventDriver` 发布输入。

2026-10-17 真机已成功加载 `fpcsgt`，传感器信息命令返回 0，并发布 FPC Home HID；
原生 IOHIDEventDriver 已打开该设备。Goodix 探测失败后完整退出，随后 FPC 成功。
当前确认到安全应用通信和 FPC 输入服务启动，实际 Home 按下/松开与上层行为须由
物理输入采样确认；没有实现或验证指纹录入、认证。

## 接线与匹配

原厂 DTS 同时列出 FPC1268 与 Goodix 模块，使用 TLMM GPIO35 供电使能、GPIO37
复位、GPIO121 中断；SPI 是 BLSP2 QUP6。FPC 固件调用 `qsee_spi_open(11)`，与
该零起始控制器编号对应。IRQ121 是传感器通知，不代表 Home 是否按下。

现有板级 DTS 使用一个物理 `fingerprint` provider，包含两个兼容字符串。
两个芯片 Kext 使用相同的默认 IOKit 匹配类别，Goodix 优先，启动失败并完整撤销
自己的会话后可尝试 FPC；成功识别的驱动独占这个 provider。不会创建两份实体
传感器、并发控制同一复位引脚。若安全应用进入无法完成的 listener 请求，标记
provider 为隔离状态，禁止另一个驱动继续试探。

Home 节点默认禁用。桥接器只有显式传入已准备的固件时才启用它，合并授权
GPIO35/37/121、GCC 时钟54/66和根时钟26；保留原有 UART、显示、存储和 USB
资源授权。GPIO 属性使用标准 `<&tlmm pin flags>`，驱动核对真实控制器 phandle
和复位的低有效极性。SPI 寄存器、DMA 和 SPI pinmux 由原厂安全应用控制。

## 已实现的协议

已自行下载公开设备仓库中的四个原厂 HAL 库，以及对应 MIUI 固件包，并用
IDAPython 分析。库和固件仅保存在 `build/`；URL、SHA-256、函数地址和字段证据见
[`fingerprint-protocol.json`](fingerprint-protocol.json)。

FPC 使用 `fpcsgt`。64字节请求头指向一个4096字节物理共享区，传感器命令组为10：
命令0查询手指是否已离开（结果0为仍在、1为离开），命令1/2分别准备松开/按下
中断，命令4进入深睡模式。每次确认状态后重新准备相反方向的通知；持有状态下
用定时器补充检查。负数状态不会变成 Home 事件。

Goodix 使用 `goodixfp`。先读取原厂配置并限定已核对的 `Milan F/A/HV` 协议，再执行
初始化和Z方向导航配置；这三个HAL分支的初始化、IRQ及导航布局一致。需要额外
配置下载的Oswego M及未知系列明确拒绝。中断消息为223776字节，只解析
FINGER_DOWN/FINGER_UP位；图像就绪、复位和移动通知不生成 Home。HAL要求的
继续传输阶段有次数上限。无新事件时保留状态，HID发送失败后仍保留待发送状态。

Home 报告是 Consumer页的 Menu（0x0c/0x40），一个状态位与7个填充位；区别于
键盘 Home 和浏览器 AC Home。重复通知不生成重复按键。暂停、停止或连续错误后
发送中性状态；报告失败保持重试。

驱动在调用原生 `IOHIDDevice::start` 前创建工作循环，并覆盖 `getWorkLoop`。
原生 HID 的报告锁、异步请求与驱动自己的 command gate 使用同一个循环；循环的
最后一个引用在原生 HID 清理完成后释放，避免锁顺序冲突和停止时的悬空引用。

QSEE传输使用64位Qualcomm SCM，4GiB以下、物理连续且不缓存的共享区。
共享区通过现有 `IOBufferAllocation.h` 使用 Normal NC 映射；ARM 上的
`kIOMapInhibitCache` 是 Device 内存，真机证明它会在 `bzero` 的 DC ZVA 指令处
触发异常，故不能用于本协议缓冲区。SMCCC 在安全调用前后执行 DSB/ISB。
整个调用通过 `QcomSCM` 的内核平台函数取得同一个monitor锁，与GPU/PAS调用
串行执行；底层SMC入口也复用该服务的实现。平台函数只接受同一SoC总线下的
`FingerprintHID`内核客户端，没有用户态callback接口。
第63轮原生线程栈发现FPC采样持有SCM锁时，GPU的缓存状态校验也在等待该锁，
且FPC工作线程占用原生HID循环的gate。QcomSCM 1.0.2已将租约/缓存状态锁与
monitor锁分开，`Validate`和不调用SMC的`Release`只取得前者；真正的PAS与QSEE
事务仍串行执行。QSEE在释放monitor锁前原子锁存隔离状态，PAS取得monitor锁后
再次检查；停止和终止判断包含已经准入的QSEE调用。Kext交叉构建、Apple匹配/
导入校验及既有GPU提交检查已通过。第64轮时，FPC运行期SMC移出HID gate
仍待实现，不能将这项锁拆分视为整个HID阻塞已经解决。第64轮GPU/Metal诊断镜像
仍使用冻结的旧SCM，并暂未启用可选Home固件，用于独立核对原生UI编译通路。
检测固件是否支持 modified-command whitelist，按原厂协议提供真实物理指针、
共享区长度与SG描述；不使用虚拟地址或绕过该检查。SMC中断继续调用，忙状态
有限重试；完成后才读取响应或回收内存。
传输的参数布局、带x6令牌的续调、忙状态重试和隔离逻辑共用可在主机测试的实现，
测试直接调用驱动使用的传输代码，包括保留原始x1..5与x6令牌的续调、次数上限和
忙重试；真实SMC指令和传感器交互仍需真机阶段确认。

2026-10-20 的接入实现把运行时配置、采样与重新布防移入
`THREAD_CALL_OPTIONS_ONCE` 串行工作项，同一会话缓冲不会被并发调用。
HID gate 只接收结果和投递报告；电源切换递增 generation，丢弃在途旧采样，
停止时先在 gate 中取消请求，再在 gate 外等待工作项退出，最后卸载 TA。
空闲补采样改为 1 秒，按住时仍为 20 ms。实际 IRQ 可提前触发采样。
这项隔离不保证安全世界调用本身没有全系统开销，仍需检查实际 IRQ 频率和调用耗时。

首轮最新 GPU 基线启动已发布 FPC Home 和原生 IOHIDEventDriver，
`HIDInputOffline=0`、`HomeSecureWorker=1`。该轮存在启动慢和原生重启未完成的现象，
不能据服务已发布宣称整体输入集成通过；原始记录位于
`build/input-touch/runtime-20261020/boot-01*`。新增只读 `inputctl registry`
提供成功采样数、IRQ/定时器请求数、最近和最大安全调用耗时，便于继续定位。

第 03 版诊断在 uptime 129.447 秒记录 2386 次成功采样、2387 次 IRQ 请求，
最近安全事务为 45,943 µs，没有定时器请求，表明重新布防产生的锁存边沿
形成了连续查询反馈。第 05 版在成功处理 FPC 状态后，通过 QcomGPIO 的
`acknowledgeGPIOInterrupt` 确认已由本客户端屏蔽的源，再恢复 IRQ。
它不会清除其他客户端的源或未屏蔽的 IRQ。相同启动基线的第 05 版在
uptime 80.075 秒记录 13 次成功采样、12 次定时器请求，没有连续 IRQ 请求；
触屏、侧键、FPC Home 的三个原生 IOHIDEventDriver 均已匹配。

## 固件准备与桥接

```sh
make prepare-input
make input
make verify-input
# 仅生成桥接产物；不连接或启动设备
make bridge BRIDGE_ARGS="--kernel build/output/kernelcache.input --input-firmware build/input/firmware --output build/input/bridge"
```

`prepare-input` 下载已固定SHA-256的固件包，读取NON-HLOS内的FAT16文件系统，
核对MDT与每个分段的长度，按原厂加载协议组合安全应用。输出为
`build/input/firmware/{fpcsgt.bin,goodixfp.bin,manifest.json}`。已有下载可复用：

```sh
python3 scripts/toolchain.py prepare-input --firmware-archive /path/to/firmware.zip
```

桥接器验证manifest和应用包哈希，将字节数据放进Apple设备树。真正的认证加载
由原厂QSEE完成；没有把Android HAL直接链接到iOS内核，也没有安装Android服务。

## 运行前提与限制

- 需要启动固件已经初始化QSEE、应用内存区和cmnlib；原厂DTS声明由appsbl/UEFI
  完成这些步骤。驱动不重新划定安全内存区或接管其他已加载应用。
- 目前没有实现QSEE存储或keymaster listener。若安全应用请求这类服务，会停止
  Home输入、隔离会话并保留其物理缓冲与资源，防止安全世界继续访问已回收内存。
  未完成的安全事务同时隔离SCM服务，停止后续PAS等安全调用，直到平台复位。
  是否会在本机的仅导航初始化中出现该请求，需要后续真机阶段确认。
- 平台深度休眠唤醒尚未实现。逻辑暂停保留供电与时钟，不承诺断电恢复。
- Goodix当前支持已分析的Milan F/A/HV配置；Oswego M及未知配置不会套用已有布局。
- 原厂Goodix HAL还会通过keymaster注入认证材料；本实现不执行指纹认证或该注入。
  安全应用是否允许这种仅导航调用，需要真机确认；拒绝时不会发布虚假的Home状态。
- 固件真实性、权限交接、GPIO供电、SMC返回和iOS上层Home策略均须在真机阶段确认。

接线来源为 [小米公开内核](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/sagit-n-oss/drivers/input/fingerprint)，
HAL来源为 [公开设备vendor仓库](https://github.com/TheMuppets/proprietary_vendor_xiaomi_sagit/tree/f73f45fdeaf787a17aede8f77f2608c6dacdb786/proprietary/vendor/lib64)，
固件来源及校验值记录在准备工具和协议清单中。Linux代码只用于硬件协议参考，
驱动的生命周期、匹配、中断分发和HID接口均采用Apple IOKit模型。
