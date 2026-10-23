# ARM64 XNU 引导桥

Mi 6 已通过 EDK2 驻留 Fastboot 下载 EFI 桥进入 XNU，输出 Darwin、pmap 和
VM 启动日志。GOP 的 1080×1920 framebuffer 已传给内核，`mirror-console=1`
使串口和屏幕同时显示启动文本；用户已确认屏幕显示。桥不写手机分区。

## 构建和测试

```sh
make build TARGET=SDM835 JOBS=3
make bridge
make test-bridge
# 已处于 EDK2 Fastboot：连续启动两轮，观察看门狗复位并重新枚举
make test-bridge-device
# 已处于 EDK2 Shell：首轮经 UART 进入 Fastboot
make test-bridge-device BRIDGE_TEST_ARGS=--from-shell
```

需要宿主 Clang、lld-link、ld.lld、llvm-objcopy、dtc、Python 及原有本地 XNU
工具链。QEMU 用于原始 ARM64 入口的集成测试；未安装时该项测试会跳过。
构建只读取当前项目；参考项目的 DTS 预处理/编译流程用于对照，没有运行时依赖。

产物均在 `build/bridge/`：

| 文件 | 用途 |
| --- | --- |
| `XnuBridge.efi` | 自带 Mach-O 和 Apple DT 的 ARM64 EFI 应用 |
| `xiaomi-sagit.dtb` | 标准 dtc 编译的大端 FDT |
| `xiaomi-sagit.apple-dt` | 面向 XNU 的小端展平树 |
| `Image` | Linux ARM64 Image 头、可重定位的原始入口 |
| `boot-bridge.img` | 原始入口 gzip 加厂商 DTB，Android boot v0 包装 |
| `build.json` | 内核及产物的 SHA-256、构建参数 |
| `nvram-proxy.bin` | sagit 的 16 KiB 易失性 CHRP v2 NVRAM 交接镜像 |

`Image` 的通用交接已在 QEMU EL1 和 EL2 下验证，测试载荷检查 Mach-O 入口及
`boot_args` ABI；这是加载器测试，不代表 SDM835 内核可在 QEMU virt 上运行。
`boot-bridge.img` 已生成，原厂 ABL 的直接启动路径尚未真机验证。
不把 `.efi` 当作 Android kernel 直接包装。

设备测试默认使用 `tools/device-profiles/sagit.json`，先核对 product，再下载、
核对 CRC32、执行 `oem run-efi`。每轮保存 `uart.bin`、`uart.txt`、命令日志和
`report.json`。多轮模式要求内核日志和看门狗恢复都成功，否则停止重试。
原有 `scripts/fastboot-debug.py` 仍用于返回 Shell 的 EFI 载荷和固件更新测试。

## 加载和内存约定

`boot/bridge/core.c` 共用 Mach-O 验证、段加载、BSS 清零、Apple DT 修正和
`boot_args` 构造。验证命令边界、文件边界、段重叠以及入口是否位于可执行的
文件数据内；零长度预链接段不参与最低加载地址选择。

EFI 入口从固件内存图选择连续的 256 MiB ConventionalMemory，整块分配后才
交给 XNU。Mi 6 默认最低候选地址为 `0xa0000000`。当前只让内核管理这块独立
窗口，没有把完整 6 GiB DRAM 宣称为可用内存。保留区和存活的 EFI 对象不会被
包含在这个窗口中。物理地址与内核链接地址保持相同的 2 MiB 内偏移。

最终内存图获取后直接调用 `ExitBootServices`，失败时按新 map key 重试。
清理内核、DT 和参数的缓存后，屏蔽中断、关闭 MMU/cache，转入 EL1 和 Mach-O
入口。XNU 自己建立页表。EL2 原始入口也会配置 EL1 的 timer 访问并降到 EL1。

原始入口按 ARM64 Linux/Android ABI 接收 `x0=固件 FDT`，解析 memory、
memreserve、reserved-memory、initrd、simple-framebuffer，并排除自身和 FDT
占用。不能安全选择连续内存时不跳转。

## 标准设备树与通用 ARM64

`device_trees/msm8998.dtsi` 描述 Kryo CPU、GICv3、定时器、UARTDM 和看门狗；
`xiaomi-sagit.dts` 选择串口并提供 XNU 启动元数据。源文件由普通 `dtc` 编译，
没有使用参考项目修改过字节序的 Apple dtc。

转换器按属性语义处理地址：FDT 多 cell 地址变为 LE64，CPU MPIDR、phandle 和
频率变为 LE32，字符串与字节数组保留。标准 `phandle` 同时导出为 XNU 所需的
`AAPL,phandle`；SoC 总线在 Apple DT 中命名为 `arm-io`。运行时替换内存窗口、
正在运行的 CPU 状态及 `/chosen/memory-map`，不固定 CPU 节点名称。

Apple DT 属性只保证 4 字节对齐。EFI 和 raw 入口都使用 `-mstrict-align`，
防止编译器把 CPU 状态等短字节复制合并成未对齐的 64 位读写；EDK2 可启用
`SCTLR.A`，这种访问会在进入内核前触发对齐异常。

平台 console 由 DT 选择 UARTDM、PL011 或 16550/DW APB UART。UART 时钟、引脚
和波特率由固件预先初始化；桥不是独立的 GCC/TLMM 或显示驱动。
其他 ARM64 板可指定自己的 DTS、内核和最低候选物理地址：

```sh
python3 scripts/build-bridge.py --dts device_trees/qemu-virt.dts \
  --kernel /absolute/path/to/matching.macho --arena-min 0x40000000 \
  --watchdog-seconds 0 --output build/other-board
```

通用的是 ARM64 引导接口；内核仍需匹配该板的 BSP、GIC 和外设驱动。32 位 ARM
不在此 ABI 范围内。非 sagit DTS 默认不自动附加小米厂商 DTB，若需 Android
包装，显式提供该板的 `--android-dtb`。

## NVRAM 交接

sagit 的 `/options` 由 XNU 的 `IODTNVRAM` 接管。DTS 在 `/chosen` 显式配置
`nvram-bank-size=<0x4000>`、`nvram-bank-count=<1>`、`nvram-current-bank=<0>`。
`scripts/bridge_nvram.py` 按 XNU `IONVRAMCHRPHandler.cpp` 的格式生成空的
CHRP v2 bank：32 字节 `2nvram` 头、到 0x2000 为止的 `2common` 分区、剩余的
`2system` 分区。分区长度包含 16 字节头，以 16 字节为单位，整数为小端。
CHRP 头使用折叠加和校验；Apple Adler32 覆盖 bank 的偏移 20 到末尾。

转换器生成内联 `nvram-proxy-data`，三项 bank 参数转换为 LE32，镜像字节原样
保留。EFI 和 raw Image 共用同一份 Apple DT；`bridge_prepare` 把整棵树复制到
内核 arena，并将其包含在 `topOfKernelData` 以下，因此交接不依赖固件缓冲区
的生命周期。构建额外保存 `nvram-proxy.bin` 和 SHA-256，元数据明确标记
`persistent=false`。不声明 NVRAM 的自定义 DTS 不生成该镜像；不完整的参数、
无效 bank 范围、损坏的镜像校验和或分区会使构建失败。

当前镜像不导入 EDK2 变量，不包含伪造的签名、nonce 或安全熔丝数据，也没有
NVRAM 控制器持久化写回。断电后变量不会保留。这是引导快照交接，后续需另行
实现平台 NVRAM 控制器及存储后端。

2026-10-15 真机使用包含 corecrypto、AppleImage4、AMFI 的 23 Kext platform
kernelcache 验证：XNU 识别 16384 字节 bank、count=1、current=0，解析出
common `(0x30, 0x1fd0)` 和 system `(0x2010, 0x1ff0)`，完成 `ConfigureIOKit`。
原来的 `IONVRAMCHRPHandler creation failed` 已通过。随后 AppleImage4 因缺少
`/chosen/crypto-hash-method` 报出 `non-sensical crypto hash method`。DTS 现已
补上原生 Image4 接受的 `sha2-384`，真机越过该检查，继续到 trust-cache 初始化，
并因 `amfi interface not available @kern_trustcache.c:589` 停止；AMFI 模块
已经在集合中，但接口尚未注册。这不是完整启动通过。日志保存在
`build/nvram-handoff/hardware-sha384-1/uart.txt`，对照摘要
在 `docs/sdm835/ios18/nvram-handoff-validation.json`。

## RTC 与 arm-io

Apple DT 的 `/arm-io` 使用板级 `arm-io,sdm835` 兼容标识，以匹配
`SDM835IO`；原生 `AppleARMIODevice` 下的 SPMI 控制器负责读取 PM8998 RTC。
板级 SPMI 参数 `qcom,pmic-rtc=<0 0x6000>` 经桥转换为两个 LE32 单元。
成功读取后发布真实 `IORTC` 服务，并由平台专家提供日历接口；XNU 原有
30 秒等待及日历初始化顺序保持不变。当前只支持原始计数器起点和 RAM 中
的时间修正，尚无厂商 UTC 偏移导入、持久化设时或唤醒闹钟。
详细根因、Apple VMA2 DT 对照、短轮询修复与真机后续停点见
[RTC 验证记录](ios18/rtc-validation.json)。

## Framebuffer 和双输出

EFI 从 GOP 查询 framebuffer 基址、大小、分辨率、行跨度及像素格式。检查
BGRX8888/对应 bitmask 与范围后写入本版 XNU 的 `Boot_Video`，使用文本模式；
物理地址标志通过最低位传递。Mi 6 验证值为 `0x9d400000`、1080×1920、stride
4320，32 bit。没有 GOP 或格式不支持时明确记录，并保留串口输出。

XNU 获取屏幕后，`mirror-console=1` 把同一 console 字符送到串口和 video
console。视频初始化成功后才打开镜像，默认未给 framebuffer 的板不走屏幕
路径。原始入口可转交固件 DT 中标准 `simple-framebuffer` 的参数，不猜测显示
控制器地址或私有扫描输出寄存器。

SDM835 的这块 framebuffer 是保留 DRAM，现通过 XNU 的 `VM_WIMG_WCOMB`
映射为 ARMv8 Normal Non-Cacheable（MAIR 0x44），允许合并像素写入。
文本滚屏、串口与屏幕镜像保留相同内容，启动耗时见[多核测试](../smp/README.md)。

## 看门狗与 panic 输出

MSM8998 使用厂商 DTS 中的 `0x17817000` 看门狗，时钟 32765 Hz。
使能 EN 和 UNMASKED_INT_EN；bite 早于 bark，防止先触发未注册的 bark 中断。
默认 15 秒，作为每轮调试的复位期限，故意不喂狗。5 秒自测已确认计数递增、
硬件复位及 EDK2 Fastboot 恢复。普通启动也反复验证了自动恢复。

```sh
# 调整或关闭调试复位期限
make bridge BRIDGE_ARGS=--watchdog-seconds=60
make bridge BRIDGE_ARGS=--watchdog-seconds=0
# 独立验证硬件复位
python3 scripts/build-bridge.py --output build/bridge/watchdog-check \
  --watchdog-seconds 5 --watchdog-test
python3 scripts/test-bridge.py --payload build/bridge/watchdog-check/XnuBridge.efi \
  --output build/bridge/watchdog-check/hardware --seconds 60
```

`XNU_SOFTWARE_PANIC=1` 是 SDM835 的默认编译开关。它只替换 XNU
`consdebug_putc` / `consdebug_putc_unbuffered` 的字符输出后端为直接 UART，
并在屏幕已就绪时输出同样的字符，同时保留 `debug_putc()` 对原生日志缓冲区
的写入。没有自定义 panic 报告格式，没有替换 XNU
的 `_doprnt`、paniclog 格式化或 debugger 状态机，也没有把 panic 改成可返回
的警告。早期 UART 使用临时映射，正常串口初始化后撤去该映射。

```sh
# 原输出通道 / 直接串口输出通道
XNU_SOFTWARE_PANIC=0 make build TARGET=SDM835 JOBS=3
XNU_SOFTWARE_PANIC=1 make build TARGET=SDM835 JOBS=3
# 在调度器、线程和 KDP 就绪后主动触发原生 panic，留足报告输出时间
python3 scripts/build-bridge.py --panic-test --watchdog-seconds 30 --output build/bridge/panic-check
python3 scripts/test-bridge.py --payload build/bridge/panic-check/XnuBridge.efi \
  --output build/bridge/panic-check/hardware --seconds 90
```

切换宏时构建器会使相应输出对象失效。桥在内核、DT 和 boot_args 后预留并
清零 1 MiB 普通 RAM，将其包含在 `topOfKernelData` 以下，防止 VM 重新分配。
通过 `/pram/reg`、`/chosen/embedded-panic-log-size` 和
`/chosen/memory-map/PanicLog` 传给 XNU。标准 DTS 用 `pram@0` 占位，转换到
Apple DT 时命名为 `pram`；自定义板的 DTS 也需包含这些占位属性。
XNU 原有代码负责设置 `gPanicBase`、`panic_info`、panic header、日志长度和
stackshot，不使用 Apple 专用 panic 硬件。缓冲区每次启动清零，不保证跨复位
保留，主机 UART 日志负责保留证据。

SDM835 的绝对 MMIO 地址允许 SoC 基址为零，且没有 Apple
`cpu-debug-interface`；调试初始化跳过该板的非零基址检查，仍保留原生
panic-trace 功能依赖检查。panic 缓冲区沿用 XNU 的正常初始化顺序，避免
在 VM 分配器就绪前初始化扩展 panic 日志。

2026-10-15 真机自测输出了原生 `panic(cpu ... caller ...)`、内核 UUID、任务
信息、10 帧 LR/FP 回溯和 `Stackshot Succeeded`，随后看门狗恢复到 EDK2
Fastboot。用户确认 framebuffer 同样显示 panic、回溯和末尾信息。完整记录在
`build/bridge/panic-fix/hardware-2/uart.txt`，纯 panic 文本在
`build/bridge/panic-fix/native-panic.txt`。测试脚本要求原生报告头、至少三帧
回溯和 stackshot 结束信息同时存在；只有早期字符串或截断报告会失败。
同一测试脚本通过 UART 文件锁避免并发测试互相读取串口。
在调试设施、VM 和线程尚未就绪的极早期，XNU 原有短消息及停机策略仍保留。

构建时产生的新随机种子用于此次调试镜像，UEFI RNG
可用时会在运行时替换；这不是量产安全启动或持久崩溃采集方案。

### AMFI 引导参数

`/chosen/amfi-only-platform-code=<1>` 由 DTS 明确提供，转换到 Apple DT 时为
4 字节小端整数。原生 AMFI 要求该属性存在且长度正确；值 1 保留平台代码
限制。2026-10-15 真机补齐后已加载 AMFI、Sandbox 和 vnguard 策略，并越过
trust-cache 初始化。随后暴露的 IIG 调用桩 `IORPC.kernelContent` 缺失问题
也已在生成步骤修复，两轮真机均进入后续 BSD 初始化，详见
[IIG 验证记录](ios18/iig-compat-validation.json)。

## 验证范围

主机检查覆盖 Mach-O 截断/越界/不可执行入口、段复制及清零、FDT 字节序和
`AAPL,phandle`、保留区和 4 GiB 以上内存、framebuffer 范围以及 watchdog
编程序列。QEMU 验证 EL1/EL2 原始入口、PIE 重定位和真实函数交接。
宏开/关分别编译 XNU 原始 printf 源码，检查只有输出后端发生变化。

真机日志和 SHA-256 由 `bridge-validation.json` 指向当前证据。这里只验证基础
内核启动和调试输出；没有声称完成 SMP、原厂 Android boot 直接启动、驱动集合
加载或完整 iOS 用户空间启动。

硬件寄存器依据为项目固定版本的
[Xiaomi watchdog_v2.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/3fb35ef1f0fe1f714be3df39724cbc8eb217181e/drivers/soc/qcom/watchdog_v2.c)，
原始 ARM64 入口约定参考
[Linux ARM64 boot protocol](https://docs.kernel.org/arch/arm64/booting.html)。
