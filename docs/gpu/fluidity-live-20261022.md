# USB 实机动画优化记录（2026-10-22）

状态：进行中。用户仍反馈系统动画卡顿、部分横向断条和渲染错误；没有达到系统 UI 约 60 FPS 的验收条件。

## 环境与保留项

- Mi6 / SDM835 / Adreno 540；iOS 18.7.10（22H374）。保留原厂 IOGPU、IOSurface 和合成器。
- 基线：`build/usb-bringup-20261022/numeric-mask-bridge/XnuBridge.efi`，SHA-256 `f3f8b72bd592c7613b478b0250f5f1138a95d5de455a0e524fe8f7e01e2755bd`。
- 测试镜像临时从 RAM 启动，未刷写分区。沿用已调通的 USB、设备树和内核转换配置。
- 原文件备份、构建收据和日志：`build/gpu-live-20261022/`。仓库现有其他改动未清理或回退。
- GPU 实测约 596 MHz。UEFI DEBUG 时钟输出不能作为运行时 CPU 频率证据。

## 已验证的显示调度问题

命令模式面板的 PP_DONE 到来后，旧实现完成当前请求，但已准备好的下一请求等待下一次 VBL 才启动。它错过当前 TE，导致连续动画接近隔帧显示。

`QcomDisplay` 1.0.25 在真实 PP_DONE 完成后、VBL 通知前尝试启动已就绪的待处理请求。保留原生 fence、ready bits 和请求退休条件；不把超时视为显示完成。

时间线中，已排队请求的“上一帧完成 → 下一帧启动”中位数由 2.355 ms 降至 0.037 ms。

固定工作负载为 `displayctl layer-animation`：原生 CAContext 中 20 秒简单动画，经 backboardd、GPU 和显示驱动。下表只统计本次新增显示完成帧，不包含历史环形记录。

| 镜像/运行 | FPS | 帧间隔 P50 | 帧间隔 P95 | >20 ms 间隔 | 失败 |
| --- | ---: | ---: | ---: | ---: | ---: |
| B，显示修复前 | 30.00 | 33.339 ms | 33.366 ms | 599 | 0 |
| C，显示修复后首轮 | 48.87 | 16.672 ms | 17.095 ms | 42 | 0 |
| C，显示修复后热运行 | 58.19 | 16.669 ms | 16.758 ms | 15 | 0 |
| D，空常量窗口补零后单轮 | 40.01 | 17.123 ms | 33.374 ms | 400 | 0 |
| E，4 B 对齐完整窗口 staging | 59.99 | 16.669 ms | 16.742 ms | 0 | 0 |

证据目录依次为 `device/layer-before-drain`、`layer-after-drain`、`layer-after-drain-warm`、`layer-zero-window`、`layer-aligned-window`。E 的 1200 个新增显示完成帧间隔均未超过 20 ms。C/D 尚缺同条件多轮比较，不能把 D 认定为性能提升。简单动画结果不等于桌面或应用切换帧率，也不证明光学显示没有横向断条。用户在 E 上仍明确反馈 App 打开、负一屏、App Library 和滑动动画卡顿。

## GPU 常量窗口

Metal 1.52.7 的编译器先选择较宽的静态只读范围，再利用空闲常量窗口容纳小范围。主机逐位检查覆盖半精度跨行/分量布局、边界和原有 32 位常量。

- B：短绑定通过 GPU 私有 scratch 补零和复制，只读取实际有效的完整 32 位字；不足一个字的尾部仍走原有有界读取。
- D：完全越界或不存在的窗口只生成零值，不读源地址。
- E：完整 64 B 窗口的源地址只有 4 B 对齐时，也通过私有 scratch 对齐。简单原生动画已测到 59.99 FPS，系统动画仍未达标。

scratch 位于进度页偏移 1536，避开时间戳、计数器和 GMEM 区域。既有 flush/wait、GPU 复制完成顺序和物理别名检查保留。主机测试检查 packet 顺序、范围、对齐、别名和变体选择。

A/B/D/E 实机 `constant-window-test`、`word-window-test`、540×960 IOSurface `ca-render-probe` 均通过，GPU 失败记录为零。E 的 offset=4 GPU 生产/消费和多重采样用例逐像素通过。这些测试没有覆盖用户反馈的全部图形错误。

## CPU 运行镜像核对

用户提出 CPU 瓶颈后，在 E 实机读取到：

- `CPUAutomaticPerformanceAvailable=false`。
- `SecureOSMInitialized=false`。
- 运行镜像保留的是 QcomOSM 1.2.0，源码当前为 1.4.0；不可用当前源码解释旧二进制全部行为。
- EFI 构建收据 `cpu_count_requested=4`、`cpu_osm_handoff=null`。这证明配置请求四核，不代替运行时在线核心数读取。
- 新版源码中的 OSM handoff/LUT 诊断属性在旧驱动中不存在，不能将缺失属性解释成数值零。

证据：`device/cpu-osm-properties/command-1.txt`、`device/cpu-legacy-state/command-1.txt`，以及 `device/matching-aligned-window/collection/org.darwin.driver.QcomOSM.kext`。

以上是 E/E2 的历史状态。2026-10-23 的实验版本已初始化 OSM/CPRh，并启用 XNU 自动调频策略；实际物理频率仍未确认。系统继续使用四核。`gpuctl cpu-status` / `cpu-sample` 区分 desired 档位与物理频率，不能把目标寄存器读回当成升频完成。

### CPU 初始化实验（2026-10-23）

- Bridge 在访问前、ExitBootServices 后分别用 AT/PAR 检查 MMIO 地址的读写转换、物理地址和 Device 属性。实机新实验均一次通过，备用重映射分支没有实机触发证据。
- CPRh 初始化遵循 vendor 的关闭 sensing loop → 配置计数模式/校准表 → 最后启用顺序。原固件保留的 BUSY 位不能作为冷初始化等待条件；此前固定等待 5 ms 的版本在真机失败。保留所有权检查、保留位和逐项读回。
- 性能核簇无运行核心时 ACD 本地握手失败。通过原厂 PSCI 临时唤醒 MPIDR 0x100，完成两个簇初始化后确认该核 CPU_OFF，再进入四核 XNU。
- `osm-peer-140` 实机初始化 stage=8/error=0/enabledMask=3；iOS 返回 `OSMInitializationHandoffAccepted=true`、`OSMHandoffStatus=accepted`、`OSMLUTValidated=true`、`CPUAutomaticPerformanceAvailable=true`。
- `osm-observe-141` 增加按需、同工作循环串行的只读 IORegistry 快照。未修改沙盒规则，未开放电压/频率写接口。
- 该版 10 秒采样四核负载均为 100%；策略状态 Automatic，目标为 1,900,800,000 Hz，频率目标已切换 83 次。OSM 周期计数在全部样本中为 0，不能据此声称实际升频成功。
- 两次初始化成功的实验从 EFI 到 Bash 约 54.8 秒，慢于 E2；该实验尚不能作为性能改进版本发布。正在检查实际时钟源/PLL/OSM 执行状态。

证据：`device/boot-osm-peer-140`、`device/osm-runtime-properties`、`device/boot-osm-observe-141`、`device/osm-live-load/export-2-0.txt`。

验证：五组 CPU/OSM/CPRh 主机测试通过，20 项 OSM Python 集成测试通过，OSM firmware/initialization 与 CPRh 三组 ASan/UBSan 通过。测试只证明对应模型和构建检查，实际频率和系统 UI 流畅度仍待实机确认。

## 剩余系统动画瓶颈

`device/desktop-display-drain-01` 记录 1282 个物理触摸事件和 1316 个活动 GPU 作业，无 GPU 作业失败。用户确认有所改善但仍卡顿。输入窗口含停顿和不同操作，不把它的平均帧率作为固定负载基准。

已记录的全屏操作：

| shader | 中位 GPU 时间 | 范围 |
| --- | ---: | --- |
| `fluidity::fragmentShader` / `a7aba4cb1419480f` | 28.34 ms | 1080×1920，四个常量窗口已启用 |
| `narrow_blur_7_frag_lph` / `8cf8bdfaa5d447c9` | 约 21.6 ms | 1088×1928 |
| `narrow_blur_19_frag_lph` / `300ab782b1c37127` | 约 18.1 ms | 1082×1922 |
| `fixed_frag_lph_cpf` / `0c4904fba586a14e` | 约 6.88 ms | 1080×1920，仍有常量窗口未启用的记录 |

fluidity 的记录没有 framebuffer snapshot/copy，不能把耗时归因于全屏复制。需要匹配同一操作的硬件计数器再区分纹理、算术、内存和 RB 等待。用户要求先处理流畅度，再处理其他渲染错误。

## 镜像追溯

下列路径均相对 `build/gpu-live-20261022/device/`，文件名均为 `bridge/XnuBridge.efi`。

| 名称 | 内容 | SHA-256 |
| --- | --- | --- |
| `staged-527` | B，短窗口 GPU staging | `b7cbe558bf9f3328ae72480dc1547921a658d16eb44cb5861f7414dae704df44` |
| `display-drain-525` | C，增加 Display 1.0.25 | `838258fb1a559d7e943c62239896e0ec46856b8b8798af5c78b3d6cf8c8fdfa3` |
| `zero-window-527` | D，增加空窗口补零 | `96a1bf2ba08d0c6977e2ce7f9c8ba52712d6867d9e6ced2f105effb98454c753` |
| `aligned-window-527` | E，增加 4 B 对齐完整窗口 staging | `a94703b7debe8332775e9a0f956d7bcdc34ccc12a297396cb11e31f43b65872b` |

每次替换的 kext、kernel 和保留文件散列见对应 `receipt.json`；不得仅凭 bundle 版本判断 D/E，因为这两版 kernel 变化没有修改 Metal 版本号。
