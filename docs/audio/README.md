# iOS 18 原生音频输出接口

2026-10-20：新增 `QcomAudio.kext`，包含原生 `IOAudio2Device` 前端和具体的
`QcomADSP` SMEM／GLINK／APR／ASM 输出后端。**支持保留运行固件及已配置板级路由的交接；
项目尚无实际交接生产端，设备树默认禁用，当前不能宣称内置扬声器可用。**
没有部署、刷写或进行真机测试，也没有运行验证 Core Audio / mediaserverd。

## 项目分析与接入选择

本项目是 Linux 上交叉编译的 XNU 11417 / iPhoneOS arm64 移植，不是普通
AVFoundation 应用。原版 iPad7c 内核缓存包含 `com.apple.iokit.IOAudio2Family`；
AppleARMDMAAudioDevice、USB Audio 的 Audio2Engine 和 AppleUSBMike 都继承
`IOAudio2Device`。原厂实例为 232 bytes、197 个虚函数槽，父类 IOService
为 136 bytes、168 槽。

Apple 的[公开 Audio Family 说明](https://developer.apple.com/library/archive/documentation/DeviceDrivers/Conceptual/IOKitFundamentals/Families_Ref/Families_Ref.html)
描述了内核 Family 与用户态 HAL 的关系，但其中的 `IOAudioDevice` /
`IOAudioEngine` 属于 IOAudioFamily，不能替代本项目实测缓存里的 IOAudio2 ABI。
本实现依据本地原厂二进制恢复接口；没有把公开 macOS 类重命名为 iOS 类。

小米[官方 sagit 分支](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/sagit-o-oss)
及项目已有的 `docs/sdm835/reference/xiaomi-sagit-msm8998-mtp.dtsi` 表明：

- `sound-9335` / SLIMbus 使用 Tasha（WCD9335）；`sound-tavil` 禁用。
- 四路 MI2S 配置 RX line 1、TX line 2，并有对应 pinctrl。
- BLSP2 QUP4（`i2c_10`）连接 TFA98xx 智能功放，地址 `0x34`，
  reset GPIO 41、IRQ GPIO 42；speaker ID 由 GPIO 100 参与识别。
- LPASS 的 ADSP、GLINK 和 SMMU 是独立资源，现有 RPM/modem 组件不能直接
  当作音频 DSP 后端。

这说明仅接通 I²C 功放控制或把 PCM 写入普通内存都不足以实现扬声器输出。
实际还需 ADSP 固件与认证/启动、GLINK/APR、AFE/ADM/ASM 路由、DSP 可访问
的 DMA 映射、时钟/pinctrl，以及匹配硬件的功放固件和校准。

## 已接通的 Apple 接口

实现位于 `drivers/audio/QcomAudio`，头文件投影位于
`drivers/include/IOKit/audio/IOAudio2Device.h`。

| 接口 | 行为 |
| --- | --- |
| 原生 `newUserClient` | 直接继承 Apple IOAudio2DeviceUserClient，保留 `com.apple.private.audio.driver-host` 权限检查、分派及通知端口 |
| `output streams` | 单路 stream ID 1，48 kHz、双声道、16-bit signed little-endian interleaved PCM |
| format/sample rate | 原生 40-byte 格式；采样率为 unsigned 32.32，不能传 IEEE double 或裸整数 Hz |
| `clientMemoryForType` | 原生类型 `0` 映射 40-byte status；`0x10000001` 分派到输出流 PCM descriptor |
| `startIO` / `stopIO` | 工作循环串行化后端启停；调用原生实现更新 `is running` 与 Mach 通知 |
| 配置变更 | 原生 `conf` 通知和 32-byte 回执；只接受已公布的固定格式，不假装支持重采样 |
| `setStreamActive` | 停用时同步停止输出；启用后由原生 start 请求启动 |
| 时间戳 | 使用后端渲染帧数与 mach 观测时间；重启改变 seed |
| PM/关闭 | 停止定时器、同步排空；失败保留 DMA 所有权及对象，不提前释放映射 |

没有覆盖原厂用户客户端，也没有提供绕过权限的调试播放客户端。
没有伪造 Apple 专属控制器寄存器、音频中断或播放进度。
轮询只是采集硬件状态，不会根据定时器次数递增 sample time。

`IOAudio2Device::start` 会立即 registerService；子类延迟发布，直到 PCM、
status 和 PM 完整初始化。共享 status 中用户可写的两个 client position
从不参与 DMA 地址、长度、代次或对象生命周期的判断。

原生接口依据保存在 `native-interface.json`。所有别名通过
`drivers/abi-22H374/apple-audio2-abi.json` 固定到提供映像的 SHA-256 和虚表槽；
这是本项目的语义别名，不宣称是 Apple 公开 SDK 的方法声明。
未恢复的未用槽保持 `nativeSlotN` 命名，不直接调用。
更换 iOS 缓存必须重新核对，版本号相同也不能跳过。

## ADSP 传输与输出后端

`QcomADSP` 继承 `AudioOutputBackend`，与前端编译在同一 Kext 内。它通过原生
AppleARMIODevice 匹配 `qcom,msm8998-adsp-transport`；`QcomAudio` 只在
`AudioOutputReady=true` 时匹配。默认 `device_trees/msm8998.dtsi` 的 ADSP 节点禁用。

已实现的实际传输路径：

1. 打开同一总线上的共享 `QcomSMEM` 服务，由它串行化全部 AP 调用者并获取
   TCSR 硬件锁 3，读取既有 SMEM 分配快照。GLINK descriptor 478、TX FIFO 479、
   RX FIFO 480 必须来自 AP/ADSP v12 私有分区；SMP2P inbound 429 可位于该分区
   或全局分区。拒绝 GLINK 的全局回退及 v11 heap，不会重置远端拥有的游标。
   逐项 Lookup 与快照比对偏移、长度和 uncached 属性，再借用服务持有的映射。
   ADSP 不重复映射 SMEM/TCSR，也不独立获取硬件锁；服务持续打开至完整关闭传输。
2. 校验 `slave-kernel` 的 ADSP READY/FATAL 和 SSR toggle，再进行 GLINK v1
   协商、双向 OPEN、8 个接收 intent、remote credit、分片重组、READ_NOTIF
   和 RX_DONE/reuse。控制回复受背压时保留状态，发送结果不确定时不重发。
3. 在 `apr_audio_svc` 通道发送真实 APR v2 命令；核对 service/domain/port/token、
   包长和类型。ACCEPTED 不能代替完成；已发布命令超时会隔离整个 epoch，
   迟到回复不能解除隔离或授权释放内存。
4. 通过 ADSP Core GET_STATE 确认可回应；输出准备时发送 ASM MAP、OPEN_WRITE_V3、
   PCM format、ADM matrix mapping 和 session time 查询。仅确认后的 MAP handle
   可进入 WRITE_V2；保留的 AFE/MI2S/功放/COPP 不在本驱动中重新初始化。
5. 将用户可写的 4096-frame PCM 环形缓冲复制到明确保留的 16 KiB DSP 窗口，
   固定 512-frame period。最多提前 2048 frames，公布相同 safety offset。
   WRITE_DONE 必须匹配本次 token、窗口地址、map handle 和成功 status；乱序
   完成按序退休，早到完成不会扩大渲染时钟之外的预取范围。
6. 停止使用 PAUSE + FLUSH；释放使用 CLOSE + UNMAP。任何不确定的停止、映射
   或解除映射保留对象、描述符、窗口和供电要求。关闭传输还需双向 CLOSE/ACK；
   不完整关闭保留 endpoint 租约，避免重新匹配后接管旧会话。

时间戳的帧数来自 `ASM_SESSION_CMD_GET_SESSIONTIME_V3` 返回的真实渲染时间，
不使用定时器计数或 WRITE_DONE 数量。对应 host time 暂为收到 APR 回复时的
`mach_absolute_time`，因此包含通信延迟；精确 AVTimer↔mach 时钟关联及 HAL
时序还没有运行验证。DSP absolute microseconds 不能直接冒充 mach host ticks。

轮询和真实 GLINK IRQ 共用工作循环。同步命令等待时主动处理 RX，不依赖被自己
持有的 command gate 外的回调，避免等待回复时死锁。协议错误、旧代完成、
时间倒退、后端错误或 250 ms 无渲染进度会停止输出。当前没有 DSP reset 后
的自动回收；恢复需要新的、可信的资源交接和设备生命周期。

## 板级交接及当前边界

本后端接受 72-byte、小端的 `ADSP::Handoff`（`ADSPProtocol.h`），由可信加载层
在验证实际硬件状态后生成，作为 `qcom,adsp-handoff` 不透明二进制属性。
它不是可用普通属性声明替代硬件初始化的开关。

- 必须证明固件已经运行、AP endpoint 独占且全新、SMEM 为 uncached、供电与
  时钟保持；必须能在既有 SMEM 中找到实际分配、实时 READY 和可回应的 Core。
- ADSP 第 0 个 `reg` 必须是 `0x17911000/0x1000` APCS。独立 `QcomSMEM`
  节点持有 `0x86000000/0x200000` SMEM 和 `0x01f40000/0x20000` TCSR，
  并要求 `/chosen/memory-map/QcomSMEM` 排除该共享 RAM。GLINK 中断为 SPI 157，
  Apple specifier 为 189、edge。SMP2P 状态仍由有界轮询检查。
- `/chosen/memory-map/ADSPFirmware` 必须保留固件实际物理范围。
- 完整输出交接还须证明 AFE quaternary MI2S RX `0x1006`、时钟/pinctrl、匹配
  硬件的已校准功放和独占 ADM COPP 就绪，分配一个空闲 ASM session。
- 输出的第 1 个 `reg` 必须精确对应 16 KiB 的保留 PCM 窗口，并通过
  `/chosen/memory-map/ADSPPCM` 从主机可分配内存中排除。交接须提供经过确认的
  DSP 地址映射；驱动不把任意 CPU 物理地址当作 ADSP IOVA。

仅传输模式不公布 `AudioOutputReady`，因此不会产生一个能启动但无实际输出
路由的 Apple 音频设备。`prepareOutput` 如遇到不能确认已撤销的资源，会返回
显式 `Retained`；前后端均保持引用，只有确认释放后才可回收。

当前项目没有 ADSP PAS 冷启动/认证加载、ADSP SMEM 冷分配、AFE/MI2S 冷初始化、
TFA98xx 固件与校准、以及这些资源的真实引导交接生产端。现有 QcomSCM 服务
限定于 GPU，不能直接挪用其 PAS peripheral 13 作为 ADSP 服务。上述缺项仍然
阻止默认设备出声；本次实现没有借助真机日志来确认这些条件。

两个后端 PM 状态都维持父级供电要求。低状态仅停止播放，不授权 ADSP/功放
电源域坍塌；系统深度休眠尚不支持。本阶段没有录音、耳机插拔、听筒切换或
硬件音量控制。用户态格式转换、系统路由和音量仍需运行验证。

Linux/小米协议依据及固定提交、SHA-256 见 `reference/sources.json`。
GLINK/SMEM/APR/ASM 后端使用 GPL-2.0-only，Apple 前端保留 BSD-2-Clause；
分发说明及许可证位于 `drivers/audio/QcomAudio/NOTICE.ADSP.txt` 和 `LICENSE.ADSP`。

## 离线验证

```sh
make audio-driver
make test-audio
make test-audio-sanitized
make audio
make verify-audio
```

这些目标只编译、测试和生成本地 kernelcache；不会访问 USB/UART/fastboot 或设备。
`audio` 是独立 platform + IOAudio2Family + QcomAudio + QcomSMEM collection，不修改既有
graphics-runtime/APFS 默认集合。

验证覆盖原生 197 槽布局和全部基类导入、原厂 dispatch 表的 selector/参数规格、
32.32 格式、共享 SMEM 权限/分区/映射一致性与 READY/SSR、双向 GLINK 握手、分片与 credit 背压、FIFO 回绕、
APR 错包/迟到回复/超时隔离、ASM 部分准备回滚、停止失败保留、乱序完成、
渲染时钟与预取边界、原始 DTS→Apple DT，以及完整 Kext 依赖/重定位闭包。
构建成功只证明离线接口和链接成立，不等价于系统音频服务识别或扬声器出声。

2026-10-20 离线结果：3 组主机 C++ 测试及 ASan/UBSan、8 项音频集成测试、
19 项工具链测试、49 项启动桥接测试、5 项链接器测试均通过。
生成的音频集合包含 31 个 Kext，依赖及重定位校验通过。
产物、原生提供映像、输入和日志的 SHA-256 记录于
[`offline-validation.json`](offline-validation.json)。
