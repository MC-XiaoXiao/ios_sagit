# Mi 6 CPU DVFS 真机验证

2026-10-23，sagit / MSM8998 v2.1，iOS 18.7.10 22H374 原生 Kext、
本地 Darwin 24.6.0 / XNU 11417.140.69，四核和八核启动。

CPU 调频沿用现有 XNU perfcontrol v5、IOWorkLoop、IOUserClient 和 QcomOSM。
本轮修复启动硬件初始化和诊断客户端签名，未替换 Apple PMGR/CLPC ABI。

## 修复与证据

1. 原启动未带 `--cpu-osm-handoff`，OSM LUT 为空，自动策略不可用。
2. 启用交接后，熔丝校准、CPRh 和 LUT 写入均通过，但两簇停在辅助时钟。
   desired index 已改变，sequencer PC 均为 `0x188`；两簇 SAW AVS control/limit
   都为零。补齐小米 v2 板级 SAW 初始化后，PLL 才切到启动目标。
3. 性能簇停核后，原厂 sequencer 将共享 LMH CFG 从 `0x102` 改为 `0x105`，
   对应 OSM 时钟从 200 MHz 改为 100 MHz。交接末尾的运行检查接受这两种配置；
   初始化前仍严格要求 200 MHz。
4. 原 `powerctl` 被 iOS Sandbox 拒绝打开服务，错误为 `0xe00002e2`。
   新工程签名中的原生 class entitlement 仅包含 `PowerUserClient`，真机打开、
   scalar 读写通过。没有增加 Sandbox 内核例外。

SAW 寄存器布局来自
[msm-spm.c](reference/xiaomi-msm-spm.c)，板级策略来自
[msm8998-pm.dtsi](reference/xiaomi-msm8998-pm.dtsi) 和
[msm8998-v2.dtsi](reference/xiaomi-msm8998-v2.dtsi)。保留原作者和 GPL 通知；
固定 commit 和 SHA-256 见 [来源清单](osm-firmware-sources.json)。

## 真机结果

| 项目 | 簇 0：Silver | 簇 1：Gold |
| --- | --- | --- |
| 已枚举 quad-core 档位 | 22 | 30 |
| 最低档 | 300 MHz | 300 MHz |
| 中档手动请求 / PLL 配置读回 | 960 MHz | 979.2 MHz |
| 最高档手动请求 / PLL 配置读回 | 1900.8 MHz | 2361.6 MHz |
| 自动模式限频 | 960 MHz | 请求 1000 MHz，选定 979.2 MHz |
| 清除上限 | 恢复 1900.8 MHz ceiling | 恢复 2361.6 MHz ceiling |

八核测试通过 XNU 原生 `kern.sched_thread_bind_cpu`，让每个在线 CPU 各运行
一个诊断线程。该接口保留原来的 development 构建和 `enable_skstb` 启动参数保护。
两簇的满载利用率均达到 1000‰，自动请求和锁定 PLL 配置分别为 1900.8 MHz、
2361.6 MHz。工作线程独立检查单调时钟截止时间，诊断结束后解绑、回收，
并继续采样空闲状态。

最终五秒测试中，八个线程的工作时间为 4.9910–4.9999 秒，join 和解绑错误均为零。
退出后 Gold 的利用率和需求均为零，自动回到 300 MHz 辅助源；Silver 仍有系统后台
负载，保持 1900.8 MHz。两簇策略状态均为 `Automatic (0x1)`。

工作线程不修改推荐核心掩码、核迁移策略或 CPU hotplug。普通 Bash 工作负载
在本镜像中只进入 Silver；性能簇的满载自动调频结论来自上述显式绑核测试。

频率证据分为 accepted desired index、RCGR 选择和 PLL L/LOCK 配置。
300 MHz 档使用辅助源，所以 `local_pll_configured_hz=0`；这不是零频率。
没有把 LUT 电压当作 ADC 读数，也没有将配置读回写成瞬时频率测量。
温控、原生 powerd 接线、长期稳定性和系统 suspend/resume 不在本次通过范围内。

## 复现

```sh
make test-osm
make test-osm-sanitized
make power-client
python3 -m unittest discover -s tests -p test_power_integration.py -v
python3 -m unittest discover -s tests -p test_macho_development_sign.py -v
```

EFI 构建在既有启动参数上增加 `--cpu-osm-handoff`。
绑核负载测试另在 `--extra-boot-args` 中加入 `enable_skstb`；它不是调频运行的前提。
新 `powerctl` 及其 trust cache 随辅助 Drivers 卷一起打包。

设备命令：

```sh
p=/System/Library/Extensions/powerctl
$p list
$p read 6 24 0
$p read 6 24 1
$p frequency 0 960000000
$p frequency 1 979200000
$p limit 0 960000000
$p limit 1 1000000000
$p auto 0 1
$p auto 1 1
$p limit 0 0
$p limit 1 0
$p load 5
```

原始证据目录为 `build/cpu-dvfs-20261023/`：

- `boot-01/`、`boot-02/`：复现辅助时钟停滞和 CPRh/SAW 状态。
- `boot-03/`：SAW 修复后，复现运行分频检查错误。
- `boot-04/`：四核启动、自动调频和用户客户端原始拒绝。
- `boot-08/`：八核启动、原生 entitlement、完整档位枚举。
- `manual/`：双簇最低／中档／最高档、自动限频、恢复默认上限。
- `boot-08-bound/`：首次八核绑核满载；采样饥饿导致超时，已中断诊断进程。
- `boot-08-final/`：独立截止时间版本的八核负载、线程回收和空闲采样。
- `final-status/`：同次启动后续的 root Bash 状态核对，两簇自动模式和档位数均通过。
- `result.json`：断言、源码与产物哈希；`bridge-argv-08*.json`：完整构建参数。

最终负载命令返回状态为零，但详细串口采样输出超过采集器原设的 40 秒回复期限。
工作线程的五秒期限、解绑和 join 均已独立核对。后续状态核对没有超时。
重放完整采样时，采集器的 `--console-command-timeout` 应保留默认 90 秒。

设备使用 Fastboot stage、CRC32 核对和 `oem run-efi` 启动临时 EFI。
本轮没有刷写 boot 分区或重装 APFS。
