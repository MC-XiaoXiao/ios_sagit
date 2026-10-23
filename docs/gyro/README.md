# SPI 总线与陀螺仪

新增独立 `GenericSPI.kext`、`QcomSPI.kext` 和 `SPIGyro.kext`，接入项目统一
交叉编译、Apple 原生 HID ABI 校验和 `gyro` kernelcache 集合。实现包含实际
QUP FIFO 传输、两种芯片的寄存器操作和 `IOHIDDevice::handleReport` 调用。
本阶段只做离线验证，未连接、部署、重启或测试手机。

**当前边界：Apple HID 接口已在代码和链接层接通；SSC 的板级资源交接尚未完成，
CoreMotion 能否在该 iOS 系统中实际收到事件尚未验证。默认设备树不启用 SPI。**

## 项目与硬件依据

项目是 SDM835 的 XNU/IOKit 移植，不是调用 `CMMotionManager` 的普通 iOS App。
原有 `GenericI2C`/`QcomI2C` 是触摸屏 QUP5 后端，不能用来代替传感器 SPI。

小米开源 MSM8998 DTS 中存在 SSC/SLPI 服务和 `slpi_v1` 固件选择，见
[`xiaomi-msm8998.dtsi`](../sdm835/reference/xiaomi-msm8998.dtsi)。
本次保存的 sagit 原厂传感器配置来自公开的固件文件镜像：

| 配置项 | 值与解释 |
| --- | --- |
| gyro 候选器件 | LSM6DSM、配置中拼作 `ICM26090` 的 InvenSense 变体 |
| 总线配置 | `0x1001`，片选项为 `0`；SPI 配置，不是 I²C 7 位地址 |
| GPIO 候选 | LSM6DSM 为 117，InvenSense 为 118 |
| gyro 轴变换 | `[-1, 2, -3]` |
| 芯片身份 | 驱动分别要求 WHO_AM_I `0x6a` / `0x20`，不根据配置名假定实装芯片 |

配置文件仅列出候选，不能证明手上设备装有哪种芯片，也不提供 AP 可访问的
SSC QUP MMIO 地址、时钟控制和安全域所有权。`ICM20690` 的身份值、不同于
MPU6500 的量程位位置，以及启动延时另由上游 Linux 驱动核对。
配置、ST 官方寄存器定义、Linux SPI/ICM 驱动和 Apple HID 头文件的来源与
SHA-256 见 [`reference/sources.json`](reference/sources.json)。
其中固件配置是镜像中的原始文件，不当作厂商公开编程规范。

## 分层与 Apple 接口

```text
AppleARMIODevice（平台提供 QUP MMIO 与可信资源交接）
  QcomSPI : GenericSPIController : IOService
    GenericSPIDevice（按片选发布、独占打开）
      SPIGyro : 原生 IOHIDDevice
        原生 IOHIDInterface
          原生 IOHIDEventDriver
            IOHIDEvent::gyroEvent → Apple HID 事件系统

用户侧检查：gyroctl → CoreMotion.framework → CMMotionManager
```

`GenericSPI` 是本项目的明确传输接口，没有将其命名为 Apple 私有 SPI 类，
也不声称实现 `AppleSPIMC` 的二进制协议。Apple 对接边界是原生
`IOService`、工作循环、命令门、PM 和 `IOHIDDevice`。

HID personality 匹配 `SPI` transport、`SPI Gyroscope` product 与
Sensor/Gyrometer3D (`0x20/0x76`)。Apple 原生 `IOHIDEventDriver`
解析 `AngularVelocityXAxis/YAxis/ZAxis` (`0x457/0x458/0x459`)，再生成
gyro 事件；依据是项目已固定的
[`IOHIDEventDriver.cpp`](../gpio/reference/IOHIDEventDriver.cpp) 中
`parseGyroElement`、`handleGyroReport`、`parseSensorPropertyElement`。
没有使用自定义用户客户端来代替这条输入链路。

`apple-hid-abi.json` 固定原厂 iOS 18 provider 的 SHA-256、208 字节实例尺寸
和 207 个虚表槽；`Build.json` 约束本驱动的描述符、上报、getReport、
setReport 等入口。完整集合验证原生 Family 依赖、符号与重定位。
这不证明 CoreMotion 的系统服务匹配、权限和运行时消费已经成功。

## 独立 SPI 驱动

`GenericSPIController` 从设备树发布 `GenericSPIDevice`，拒绝重复片选、
无效模式和频率。客户端先独占 `open`，再同步 `transfer`；同一总线通过
锁串行化。配置固定在 nub 中，客户端不能在请求中改到其他设备。
硬中断上下文禁止同步传输，停止总线时先让各设备恢复寄存器。

`QcomSPI` 使用 QUP v2 寄存器、8 位 MSB-first、硬件低有效 CS、
模式 0/3、FIFO PIO。一次事务包含命令及有效载荷，并保持 CS。
支持发送、接收和全双工；TX 按 QUP 非打包格式左对齐，RX 右对齐。
成功后才向调用者提交接收数据；超时、FIFO 错误会释放 CS、丢弃部分数据并
有界复位。复位失败或发现控制器意外处于活动态会锁存 offline。

统一 API 上限 64 字节；硬件后端还受实测 FIFO 容量限制，超出返回
`Unsupported`，不会截断。当前不含 block/BAM DMA、scatter/gather、
非 8 位字长、高有效 CS、模式 1/2 或动态改频。1 MHz 以下固定频率由
引导资源所有者实际设置并保持，本驱动不擅自重配共享根时钟。

## 芯片、事件与生命周期

`SPIGyro` 先验证型号与明确提供的安装方向，再读取完整原寄存器快照。
LSM6DSM 使用 208 Hz、±2000 dps、BDU、自动地址递增和小端输出；
ICM20690 使用 200 Hz、±2000 dps、41 Hz DLPF 和大端输出，量程配置
采用该芯片的 bits 3:2。关闭 FIFO/DMP 与不适用的自测/触发模式。
两种芯片都有启动等待、旧样本排空、data-ready 检查、模式复验和失败回滚。
ICM 的中断状态清除方式显式配置，避免其他寄存器读取提前清掉 data-ready。

所有 SPI 字节都来自真实寄存器事务。角速度用整数换算成 µrad/s，
经合法的三轴旋转后打包为 13 字节 HID 输入报告。描述符的逻辑/物理范围
均为 ±41,000,000，SI rotation 单位为 rad/s，十进制指数为 -6；
Apple 的 `getScaledFixedValue(Exponent)` 因此输出 rad/s 的 16.16 值。
坐标变换必须是行列式 +1 的轴置换，避免错误的镜像变换。

Feature report 2 实现 `ReportInterval`，范围 5,000–1,000,000 µs，
默认 10,000 µs。原生 HID 的属性写入会调用驱动 `setReport`，实际改变
轮询间隔；物理 ODR 保持 200/208 Hz，慢速读取会舍弃中间样本，不提供批处理。
时间戳来自原生 HID 上报时的主机时钟，没有伪造芯片采样时间。

定时器、PM、报告快照和采样间隔更新使用同一工作循环/命令门。
没有新数据、传输失败、HID 拒收、待机和过期快照都不会返回伪造的零值或
旧数据。错误达到上限会停止采样；PM 再启用时可重新初始化。
停止时先取消采样，进入待机，再恢复原始寄存器。回滚失败保留原快照供
后续清理重试，并记录错误。

IORegistry 诊断包括 `GyroModel`、`GyroHardwareRateHz`、
`GyroLastError`、`GyroSamplesDelivered`、`GyroHIDDeliveryFailed`、
`GyroStandbyFailed`、`GyroRestoreFailed` 和 `HIDInputOffline`。
`GyroFactoryCalibrationApplied=false` 明确表示尚未使用单机校准值。
不提供加速度计、姿态融合、温漂补偿、FIFO 时间重建或深度休眠唤醒。
IMU 的控制寄存器是共享资源，交接必须覆盖整个 IMU。

## 板级资源契约与尚缺部分

QcomSPI 仅匹配 `qcom,spi-qup-v2` 的 `AppleARMIODevice`，且必须收到
`qcom,spi-handoff` 的 **8 个小端 uint32_t**：

| word | 内容 |
| --- | --- |
| 0 | 版本 1 |
| 1 | flags=31：AP 访问授权、QUP 独占、固件/SLPI/DMA 已停止、时钟保持、引脚及供电保持 |
| 2 | 实际 core/wire 频率，100,000–1,000,000 Hz |
| 3 | 保留字段，必须为 0 |
| 4、5 | 已核实 MMIO base 的低/高 32 位 |
| 6、7 | 已核实 MMIO span 的低/高 32 位 |

驱动还检查实际映射与记录相等、QUP v2 硬件版本和 RESET 空闲状态。
子节点以 `#address-cells=1` / `#size-cells=0` 表示片选，必须提供：

- `compatible="st,lsm6dsm"` 或 `"invensense,icm20690"`。
- `reg`：片选，Apple DT 转成一个 LE64 值。
- `spi-max-frequency`：实际固定频率；`spi-mode`：0 或 3，均为 LE32。
- `gyro,mount`：三个有符号 LE32 源轴编号；sagit 配置参考值为 `[-1,2,-3]`。

`scripts/build-bridge.py` 已加入这些属性的 FDT→Apple DT 字节序转换。
测试中的 `0xdead000` 只是合成地址，不能用于设备。

**当前没有为 sagit 添加真实 SPI 节点或自动启用开关。** 仍需从固件/板级证据
确定 SSC QUP 的真实地址、AP 访问许可、片选/引脚控制、clock/power owner，
并实现实际交接生产端。这些不是填写 flags 就能完成的硬件动作。
另一条可选架构是保留 SLPI 并实现 SMGR/QMI 传感器客户端，本次没有实现它，
也没有将 SSC SPI 当成现有 BLSP 触摸总线。

## 离线构建与验证

```sh
make spi-drivers
make gyro-drivers gyro-client
make test-gyro
make test-gyro-sanitized
make gyro
make verify-gyro
```

产物：

```text
build/output/Extensions/GenericSPI.kext
build/output/Extensions/QcomSPI.kext
build/output/Extensions/SPIGyro.kext
build/output/gyroctl
build/output/kernelcache.gyro
build/output/kernelcache.gyro.manifest.json
build/output/kernelcache.gyro.verification.json
```

`gyro` 是独立 26-Kext 集合，用于本接口的离线链接检查；没有替换已有
触摸、显示或 APFS 运行集合。宿主测试覆盖生产 QUP 引擎、CS 连续性、
字节序、边界、错误与超时锁存，两种芯片初始化每一步的故障注入、
不确定写入后的回滚、待机/恢复、单位和安装方向，以及实际 HID 描述符。
Python 测试覆盖原生匹配、SPI 独立依赖和 Apple DT 转换。
验证结果与产物哈希见 [`validation.json`](validation.json)。

`gyroctl` 链接 SDK 的原生 CoreMotion，默认只检查 `gyroAvailable`，
`--sample` 在 5 秒内尝试读取 100 个有序且有限的角速度样本。当前仅
交叉编译，未执行；后续应在有适当签名、系统服务和运动权限配置的环境中
验证。HID 链接成功不等于 `gyroAvailable=true`，也不保证 DeviceMotion 可用。
