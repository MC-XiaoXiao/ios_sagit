# Xiaomi Mi 6 基带驱动进度

目标是让 sagit 的 MSM8998 集成基带接入本项目的 iOS 18。当前已连接原生固件启动、内存权限、SMEM/SMP2P、IRQ、多通道 GLINK/QRTR、RMTFS 存储服务、TFTP 文件服务与 sysmon 关闭路径；跨处理器事件生产端、Apple 电话服务及蜂窝数据路径仍在开发中。默认基带设备树节点保持 disabled。启动还要求完整 EFI 内存记录、原始 NV 分区身份策略、真实的 RPM/GCC 资源交接和冷启动证明；当前生成命令不启用该节点。

没有进行真机启动、刷写、射频操作或设备访问。下列测试中的寄存器、远端处理器与安全监控器响应由模型提供，不能证明实际固件通过设备签名校验或基带已工作。

## 输入与证据

- 主要 Apple 固件为 `firmware/iPad_10.2_18.7.10_22H374_Restore.ipsw`；iPhone 18.6.2 仅用于对照。内核、原生 KEXT 匹配项、CommCenter 依赖与启动条件见 `ios18-model.json`。
- `ipad-runtime-libraries.json` 来自 iPad 原始 sealed Cryptex，记录 61 个 dyld 缓存成员和 10 个实际运行库的 UUID、依赖、导入、导出及相关字符串。SDK 的 `.tbd` 仅作为声明证据。
- `ipad-apple-control.json` 记录 8 个单次标量控制调用的实测 selector、输入/输出数量，以及对应机器码、地址与 SHA-256。复位 API 包含多个控制调用，已列入 unresolved，不能按一个 selector 实现。报告尚未证明通知消息、控制值语义或完整的 AppleBaseband ABI。
- `ipad-apple-notifications.json` 使用实际 `LC_FUNCTION_STARTS` 范围核对事件注册、完整回调、跳转表和诊断字符串。确认 `IOGeneralInterest`、18 个消息编号及回调的 refcon/service/messageType/messageArgument 传递；`RegisterForReset` 特殊处理 `0xe3ff8007`（wake-asserted）和 `0xe3ff800d`（coredump-ready）。消息参数布局、事件顺序和高通状态映射仍待实现。
- 已取得 `firmware/sagit-baseband/mba.mbn`、`modem.mbn` 及两个服务描述文件。来源为固定提交的社区原厂提取仓库；URL、大小和哈希见 `firmware-sources.json`。尚未确认其原始小米发布包身份，也没有进行设备端认证。
- `reference/sources.json` 固定 Linux v6.12、libqmi 1.36.0、linux-msm/rmtfs 与 tqftpserv 的具体提交。GCC 的时钟、复位数据另外复用 `docs/power/reference/sources.json` 中已校验的 Linux v6.12 文件。参考源码不直接参与构建；RMTFS/TFTP 的 BSD 版权通知同时保存在源码和构建 KEXT 的 Resources 中。
- `stock-firmware-source.json` 固定社区固件归档及 `NON-HLOS.bin` 的哈希。`baseband_stock.py` 用 mtools 从普通文件中读取数据、按原始 ELF 偏移重组 modem，并验证 MBA/modem/两份 JSN 与现有输入逐字节一致。已提取 `modem_pr` 与两份 JSN 共 113 项、4,692,735 字节，MCFG 索引中的全部文件路径均存在；该镜像没有 `modem_pr/so`。原始小米完整发布包身份仍未证明。没有执行归档中的刷写程序或脚本，也没有填充镜像未提供的 FAT 尾部。
- `drivers/abi-22H374/apple-storage-abi.json` 核对同一 iPad 内核的 IOStorage/IOMedia 类大小、虚表和 6 个 getter 的实际机器码；ARM64 编译探针检查原生存储后端使用的槽位。这些证据不等同于全部存储 API 已通过运行验证。
- `remote-services.json` 固定 MSM8998 的 sysmon/GLINK SSR 名称与相关原始 modem 字节位置。Linux 的 MSM8998 ADSP/SLPI 匹配项分别复用 `msm8996_adsp_resource` 和 `msm8996_slpi_resource_init`；sysmon 名称是 `adsp`/`slpi`，清理边名则是 `lpass`/`dsps`。镜像中的字符串位置只证明字节存在，不证明实际通道、QMI 版本或握手行为。

## 已实现的代码

代码位于 `drivers/baseband/QcomBaseband/`：

| 组件 | 已完成 | 当前范围 |
| --- | --- | --- |
| FirmwareImage | ELF32 Hexagon、MDT 元数据与段边界检查、重定位、BSS 清零 | 使用真实 modem 的 28 个加载段测试；不替代 PBL/MBA 的签名认证 |
| Q6::Core | MSM8998 Q6/PBL/MBA 寄存器启动、元数据认证、逐段 code-length 发布、超时及错误处理 | 寄存器模型测试；实际 MMIO 后端已交叉编译 |
| Boot / NativeController | MBA→元数据→MPSS、资源租约、权限转换、CPU 写入、READY、IRQ、关闭与 GLINK/QRTR 轮询 | 已接入 `QcomBaseband::start()`；需要实际交接才能调用硬件，尚未在设备上验证 |
| Secure::Assignment | MP_ASSIGN 描述符、SMC32/64 扩展参数、busy 与 A6 中断续调 | 原生 NativeAssignment 使用保留的物理连续 Normal NC 内存和 QcomSCM 的全局事务锁 |
| SMEMPool / QcomSMEM | v11/v12 实时分配、全局分区回退、AP 串行锁加 TCSR 硬件锁 3 | 与 ADSP 共用一个 AP 锁拥有者；必须先有 EFI 最终内存图核验的 2 MiB SMEM 保留区；不清零已有负载 |
| SMP2P / NativeSignals | 原子字快照、版本最后发布、SSR 协商/确认、STOP 位及三路原生 IRQ | 已接入启动/关闭；保留的旧 READY 不能直接作为本轮完成，watchdog 在主中断上下文只置原子中止标志 |
| FIFO / MemoryRing / Link | 直接共享内存读写、环形队列、内存屏障、邮箱通知、READ_NOTIF | 映射与冷启动代次仍由上层提供；发布结果不确定时停止重发 |
| GLINK::Channel / Multiplexer | 一次链路版本协商、独立通道 OPEN/intent/分片/关闭、交叉协商及不复用本地 ID | 原生连接 IPCRTR、sys_mon、glink_ssr；共享环形队列模型覆盖三通道同时收发、64 KiB 阻塞、半关闭及新 ID 重开 |
| QRTR::Router / QMI | v1/v2 wire header、服务发现、独立客户端端口、请求关联、超时、流控与结果 TLV | 已测试 DMS 形状的请求/响应；尚未实现完整 SIM/NAS/WDS/语音业务 |
| QRTR::ServiceHost / RMTFS::Engine | 本地服务发布、Open/Close/Allocate/RW_IOVEC/DeviceError、整批预检、响应保留、对端退出和超时 | 服务号为十进制 14；在两端实际 MemoryRing/Link 模型上走通服务发现和 NV 字节读写；未实现 ForceSync 主动通知 |
| NativeRMTFSStorage / StorageBlocks | 原始 IOMedia 身份与拓扑核对、独占打开、512 字节请求到 512/4K/16K 块的适配、实际完成数与持久化检查 | Normal NC 共享区映射和 AP/MSS R/W 权限；真实分区尚未访问，当前没有设备 NV 身份策略或 NV 数据副本 |
| TFTP::Server / Files | QRTR 4096 v1/0、独立传输端口、RRQ/WRQ、选项、窗口、重传、流控及退出清理 | 原生与 QRTR 客户端共用端口分配器；提供固定来源的完整 MCFG/描述文件目录（113 项），缺失文件返回错误；临时文件存入有容量限制的本轮启动内存 |
| Sysmon::Client / ShutdownSequence | SSCTL 43 的 v2/0x12、v1/0 关闭与旧 sys_mon 文本后备；QMI 事件编码、响应关联、超时及旧 ACK 隔离 | 已接入原生停止路径；没有协议确认时执行 SMP2P 后备，AXI 停止/复位仍由 Boot 强制完成；尚未连接其他处理器的事件生产端 |
| SSR::Client | GLINK DO_CLEANUP 48 字节请求、12 字节以上响应、序号、边名、停止代次及通道身份检查 | 原生消费端已轮询；发送 API 要求实际停止拥有者证明，当前没有生产端注入外部处理器事件，不会自动宣告清理成功 |
| QcomBaseband | IOKit 匹配、管理员诊断、固件复制与 SHA-256、启动 gate、IRQ/timer 和状态报告 | 用户客户端仍只有 GetInfo/GetStatus；未提供 Apple 电话 ABI，TelephonyReady 保持 false |
| QcomGCC | MSS 的 6 个时钟和 MSS_RESTART 数据 | 写操作继续要求真实的所有权交接；没有添加自动授权 |
| AXI::Controller / HaltIO | Q6、modem、non-cache 三路 TCSR 停止，等待 ACK/IDLE，专用复位及读回 | 发生不确定结果时保留请求和资源；不能只记录超时后继续回收远端 RAM |
| ResourceLease / NativeResources | MSS 独占控制、CX/MX、XO/QDSS 和 6 路 GCC 时钟的取得、失败回退及释放 | 已连接 Board；仍要求现有完整 RPM/GCC 交接，没有按 compatible 自动授权 |
| MemoryHandoff / NativeMemory / Admission | 224/272 字节 EFI 记录、独立固件/目录载荷、三次 HLOS 确认、Normal NC 目标映射及独立 CPU 权限账本 | 元数据认证失败先停止/复位再回收；不确定的 SCM 完成状态保留完整上下文和资源 |

MBA 是给 PBL 的整个镜像，不按其 PT_LOAD 段装入 MPSS。sagit 的 MPSS 保留区为 `0x8d000000..0x94000000`，MBA 为 `0x94500000..0x94700000`。元数据另行从 `0xa0000000..0xc0000000` 中实际分配 16 KiB；`0x94400000` 位于 Venus 视频固件保留区。`--baseband-firmware` 编译 EFI 分配/核验代码，生成时不会宣称这些 RAM 已在设备上保留，也不表示设备端认证成功。

约 54 MiB 的 modem 不放入上限为 4 MiB 的 Apple 设备树。EFI 载荷独立包含原始 MBA/modem 字节，运行时另行分配 LoaderData，清理其 EFI WB 缓存，并在最终内存图中核对 MPSS/MBA/RMTFS 的 ReservedMemoryType、元数据/固件/启动 arena 的 LoaderData 和全部不重叠关系。只有此时才发布 `qcom,baseband-handoff` 和五个 memory-map 范围。退出 EFI 失败会撤销记录并释放本次分配，绝不释放 MPSS/MBA/RMTFS 或启动 arena。

原生驱动将固件源复制到 OSData 后再检查两份 SHA-256 和 ELF 边界。Board 取得资源后通过真实 SCM MP_ASSIGN 确认三个目标区的 HLOS 权限，随后才建立 Normal NC 映射；CPU 写入另受所有权账本约束。保留区证明、字节哈希、HLOS 权限完成和 PBL/MBA 签名认证是不同步骤，不能互相替代。

`boot/bridge/smem_reservation.h` 逐项检查最终 EFI 内存描述符，包括未排序的分段保留区、缺口、重叠冲突与地址溢出。每次 `ExitBootServices` 重试都会清除旧记录并重新检查。只有完整的 `EfiReservedMemoryType` 覆盖、且不与 XNU 启动 arena 相交时，才发布 `/chosen/memory-map/QcomSMEM`。这是分配器排除证明，不是远端所有权或固件签名认证。

RPM 时钟接口使用独立的内核 consumer 15，区分 active/sleep。XO 是使能位，QDSS 是固件 STATE 值；没有把 Linux 时钟框架的 Hz 包装值直接当成该状态。启动策略沿用 Linux MSS 对 CX/MX 的最高请求（经 MSM8998 rpmpd 限制为 BINNING 512），并要求 QDSS 状态 4；没有接受未经验证的较低启动性能档。它保留完整 AP 启动基线和上限，必须经过真实 RPM ACK。GCC 独占租约要求 6 个时钟和 MSS_RESTART 都已获交接、复位已断言、没有现有软件时钟用户；租约存在时拒绝其他 consumer 修改这些 MSS 资源。当前代理资源一直保留到确认停止，没有假定固件已经发送 handover。

RMTFS 使用 sagit 的 `0x88f00000..0x89100000` 共享区；IOVEC 的 `phys_offset` 按完整物理地址解析。服务仅接受 `/boot/modem_fs1`、`/boot/modem_fs2`、`/boot/modem_fsc`、`/boot/modem_fsg`，分别映射原始 `modemst1`、`modemst2`、`fsc`、`fsg`。每批累计传输上限为 2 MiB，在第一次 I/O 前检查全部存储/共享区范围。写请求（包括空向量）须经过真实 synchronize 才会返回成功；短 I/O 或错误不会用补零和丢弃写入来掩盖。TX 阻塞时保留同一响应，避免再次执行 NV 写入；对端退出会同时撤销旧请求和缓存响应。完成后的 QMI transaction ID 可以合法复用，不能据此跳过下一次请求。

共享区在 MSS 复位和资源租约下执行 HLOS 确认、Normal NC 映射及 HLOS/MSS R/W 赋权。认证完成后、等待 READY 时即服务 IPCRTR/RMTFS；冷启动须先观察到远端空游标，错过该窗口会拒绝继续，不清除远端游标。正常等待 STOP_ACK 时继续处理 NV 请求；AXI 停止/复位后关闭句柄并收回共享区。watchdog 阻止后续数据传输，已完成前缀仍需持久化；无法确定 SCM 或 I/O 完成状态时保留整个上下文。

GLINK 的版本协商由连接统一处理，收到的早期 OPEN 会复制保存，不占住 FIFO 头等待后面的 VERSION_ACK。命名通道各自保存 intent、待发和接收数据；sys_mon 与 glink_ssr 等待远端提供后才打开。独立通道关闭必须完成双向 CLOSE/CLOSE_ACK，期间保留旧对象；重开分配新的本地 ID。相同参数的重复版本报文只确认，不重置通道或 intent；活动通道上的特征变化需要新连接拥有者。主 IPCRTR 在运行中关闭会触发原生故障停止，尚不自动重启整个 MSS。

停止流程优先查询 SSCTL 服务并选择 v2 的 modem 实例 `0x12`，其次是 modem 的 v1 实例 0；未发现服务时可使用远端提供的 sys_mon。旧关闭文本包含结尾 NUL，其他处理器事件文本不含 NUL；超时后旧通道的无序号 ACK 不会完成下一请求。SSCTL 响应使用新客户端端口及事务 ID 关联，单独的 shutdown-ready indication 不算成功。正常等待期间继续服务 NV，请求被拒绝、缺失或超时后执行有新 ACK 证据的 SMP2P 后备，并撤销 AP STOP 位。协议 ACK 无论是否成功，都不能替代后续 AXI/复位和内存回收证明。

`baseband-services` 集成测试让模型中的远端在收到 SSCTL 关闭请求后再提交最后一批 NV 写入，只有 RMTFS 实际字节写入并 synchronize、远端收到成功响应，同时通过 TFTP 从完整目录中收齐实际 sagit MCFG 文件字节后，才返回对应的 SSCTL ACK。测试使用与原生代码相同的 Link、QRTR、多服务轮询、TFTP、sysmon 和 ShutdownSequence；这仍然是宿主模型，没有执行设备固件。

TFTP 使用 `/readonly/firmware/image/` 和 `/readwrite/` 两类路径。当前 `prepare-baseband` 校验原始 MBA/modem 与 stock 归档一致，并把全部 113 项 MCFG/描述文件编码为独立目录。目录最多 256 项、总计 8 MiB，不能包含重复、越界或路径别名。MCFG 的实际短目录名与其 `mbn_sw.txt` 索引逐项一致，不扩展、猜测或改写路径；缺失文件返回 TFTP ERROR。此文件集的存在不证明设备固件已经请求它们。

交接 v2 使用 272 字节记录，在原有字段后增加目录偏移、长度和 SHA-256。约 4.71 MB 的目录追加在 EFI 的 MBA/modem 载荷之后，仍由最终内存图证明完整 LoaderData 覆盖；不放入设备树，也不放入 raw/Android 入口。原生复制、校验整个目录后才交给文件存储。保留 v1 的 224 字节解码和小型 DT 目录兼容路径；当前完整输入默认生成 v2。两种记录的有效 flags 都只在 EFI 最终图核验成功后发布，退出失败均撤销。

`/readwrite/` 是按远端节点隔离的临时内存文件，不访问 NV 或宿主文件系统。最多 16 个文件，每个 64 KiB；写入确实保存字节后才能 ACK，短尾块结束传输，重发仅补发 ACK，不重复写入。支持 `blksize`（协商上限 8192）、`wsize`（上限 16）、`timeoutms`、`tsize`、只读 `rsize` 和 `seek`；每个传输独占新端口，共享 QRTR 流控。最多 8 个会话，有限重试与绝对生命周期约束；错误响应有容量限制，过早 DATA 不阻塞后方 RESUME。临时字节在拥有者确认停止、退役本轮上下文后清除，没有宣称跨启动持久化。

可选 `--baseband-nv-policy PATH` 需要与 `--baseband-firmware` 一起使用。JSON 顶层仅有 `schema: 1` 和 `partitions`；后者必须包含上述四个分区，每项明确给出规范 `uuid`、整数 `lun`、字节单位整数 `start`/`bytes`，以及 `read-only` 或 `read-write` 的 `access`。构建器只编码身份和权限，不读取 NV 内容；驱动再逐项核对同一 SoC 的 QcomUFS→IOBlockStorageDriver→整盘 IOMedia→IOGUIDPartitionScheme→分区拓扑、UUID 和范围，并独占打开。缺少策略或任一分区时保持诊断状态，没有静态 UUID、空白 NV 或影子写入回退。

## 离线命令

从项目根目录运行：

```sh
make prepare-baseband
make analyze-baseband
python3 scripts/baseband_services.py
python3 scripts/baseband_stock.py
python3 scripts/baseband_apple_notifications.py
make test-baseband
make test-baseband-sanitized
make baseband-driver
make baseband
make verify-baseband
python3 scripts/build-bridge.py --kernel build/output/kernelcache.baseband --smem-handoff --output build/baseband/bridge
python3 scripts/build-bridge.py --kernel build/output/kernelcache.baseband --baseband-firmware build/baseband/firmware --output build/baseband/firmware-bridge
```

重新分析运行库使用 `make analyze-baseband-runtime`。其只读解压器由项目内的 `libzbitmap.c` 构建；机器码分析依赖 Capstone 5.0.6，当前安装在 `build/baseband/python/`，可使用以下命令恢复：

```sh
python3 -m pip install --no-deps --target build/baseband/python capstone==5.0.6
```

驱动输出隔离在 `build/baseband/Extensions/`。`build/output/kernelcache.baseband` 是离线链接产物，包含 QcomUFS 存储后端在内的依赖闭包；链接成功只证明集合的结构、导入和重定位检查通过。它尚不代表可启动的基带功能。

`--smem-handoff` 仅为 EFI 路径准备上述保留区检查，并只启用共享 SMEM 服务。生成时的保留区记录为零；raw/Android 入口不会填充它，因此原生服务会拒绝该入口的不完整交接。该选项不启用基带或 ADSP、不取得它们的资源，也不执行生成的 EFI。当前基带内核集合没有 `--full-memory` 要求的内核支持，本轮验证使用独立启动 arena。

`--baseband-firmware` 包含 SMEM 交接，增加固件载荷和内存准备；基带节点仍 disabled，也没有添加 GCC MSS 交接权限或替换 RPM 的完整 AP 基线策略。当前 v2 的 raw/Android 载荷不含 MBA/modem 启动镜像和完整 TFTP 目录，也不发布有效的固件内存记录。宿主测试实际执行相同 EFI 分配/最终核验代码，使用进程自己的匿名内存和 EFI 回调模型；这不是设备启动测试。

## 继续完成目标所需的工作

1. 完成板级资源交接生产端：在 EFI 中证明原 MSS 固件拥有者已经退出、冷复位/AXI 与共享 GPLL0 状态，再授予原生驱动所需的 6 路 GCC 时钟/MSS_RESTART 权限，结合完整 AP RPM 策略；不复制静态授权模板。
2. 继续完成整个 MSS 的 SSR 重启代次和恢复策略；当前已有多通道与独立通道新 ID 重开，但主连接故障仍停止/复位。未知安全事务继续保留资源，不能把可重开一个辅助通道当作整个基带已恢复。
3. 把 sysmon/SSR 与其他处理器的真实生命周期生产端连接，并完成 RMTFS ForceSync 主动通知。TFTP 已接入该 sagit 镜像提供的全部 113 项 MCFG/描述文件，其他固件请求仍需依据实际协议证据扩展。SSR 发送接口需要停止拥有者证明，不能由固定状态触发。RMTFS 仍需原始 NV 分区身份、完整设备交接与后续验证。
4. 继续核对 AppleBaseband 控制、通知和 QMI 传输 ABI，完成实际适配。`_LimitLoadToDeviceTree=baseband` 只是 CommCenter 的启动门槛，当前不会仅为满足这一条件重命名 modem 节点。
5. 实现 SIM、注册、数据会话及 IPA/GSI 蜂窝数据路径，接入 iOS 所需的网络与电话服务，再进行完整离线集成验证。

任务仍保持进行中；以上未完成部分不包含在当前离线验证的完成声明中。
