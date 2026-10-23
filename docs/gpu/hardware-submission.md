# A540 硬件提交

实现位于 `drivers/gpu/QcomGPU/AdrenoSubmit.h`、`QcomGPUHardware.cpp` 和
`QcomGPUBuffer.cpp`，原生 DMA mapper 位于 `QcomGPUMapper.cpp` / `AdrenoMMU.h`。这是接管已初始化 GPU 的硬件提交路径，支持 CP 内存填充、
屏障和原生缓冲区复制；`AdrenoPipeline.h` 另实现 AOT/JIT shader 的 Render/Compute、索引/实例绘制
和线性纹理状态，用户态见 [Metal 适配](metal.md)。
桥已补 RPM、CX/GX、私有 GPU 时钟、CP/PFP/GPMU 和 bank 0 的 SMMU 初始化，
并接续原生 RPM 通道；共享控制总线/XO/GPLL0 和 PLL 模拟配置仍来自前一阶段。
当前没有真机验证记录。
原生 IOGPU buffer/异步准入队列也已接入同一个引擎；接口、生命周期和编码见
[`native-iogpu.md`](native-iogpu.md)。

Compute 的动态 threadgroup 参数已接到 `GPUShaderProtocol.h` 与 `AdrenoPipeline.h` 的提交桥。
每次 dispatch 按活动槽位打包共享内存，静态前缀和动态长度合计最多 32 KiB，动态区按 16 bytes 对齐。
原生准入计算相对偏移/长度并通过 CS 常量区的前 16 words 下发；其后保留 IR3 的 driver params/immediates。
编译器对每个动态参数分别检查字节范围，再生成受总 shared allocation 约束的 LDL/STL/原子指令。
仍使用 A5xx 的固定 `HLSQ_CS_CNTL_1 = 1` 配置；本次没有引入 A6xx 的共享内存寄存器或执行真机测试。

Compute 纹理读写也经过原生 IOGPU 提交桥：普通 buffer 占 CS SSBO 0..7，8 个线性 RGBA8/BGRA8 纹理占 8..15，
按完整 mip 链下发 base/byte-size 表（state block 15）。原生准入生成 word 16..47 的几何常量，前 16 words 保留线程组布局；
IR3 driver params/immediates 在完整前缀之后分配。实际 LDGB/STGB 同时受坐标/mip 和最终 SSBO 字节范围检查。
写纹理复用资源保留、cache 同步及真实硬件完成事件；不使用 CPU 像素执行。没有进行目标运行验证。

Compute 采样使用 CS texture state block 5 和独立的 texture/sampler 索引。原生桥同时解析采样和 image 资源，
拒绝采样范围与本次 image/buffer 写入的物理重叠，按实际纹理最高槽下发 CS TEX_COUNT，并更新 HLSQ CS 状态。
纹理/采样器字段与已有 FS 编码共用，MSL/AIR AOT/JIT 生成真实 SAM/SAML；不通过 CPU 或 SSBO 实现过滤。

Sampler 的预定义边界色表使用 A5xx 128-byte entry，按 512 bytes 对齐上传到 GPU 只读 IB，随作业 fence 保留。
AF 档位、repeat/mirror/border/edge、UNNORM 按固定 fd5 规则编码；NPOT mirror-clamp 在着色器中作 abs 与梯度方向反射。
原生 sampler 标志进入 CS 64-word / FS 16-word 常量前缀。显式 LOD 使用 CS logical+8 / FS1 的 unclamped sampler；
同一纹理仍可同时使用原始 clamped sampler，梯度指令为 SAMGQ。未执行目标硬件。

颜色 mip 采样现在下发 MIPLVLS、PITCHALIGN=2（256-byte 最小 pitch）和完整 layer pitch，
sampler 的 mip 过滤与 unsigned 8.8 LOD clamp 同步下发。多级资源准入及别名检查覆盖整个 layer；
渲染附件采用选中 mip 的地址、尺寸和行距。`generateMipmaps` 经 AOT Compute 及真实完成队列逐级生成，未运行 A540 真机。

## 实际执行路径

1. 接收已解析的 Apple `IOMapper` 对象，或按显式 SMMU 交接创建 `QcomGPUMapper : IOMapper` 设备 DMA 域。未解析的 mapper ID 直接拒绝，避免无期限匹配等待。
2. `IOBufferMemoryDescriptor` 以 `kIOMapWriteCombineCache` 分配 CPU Normal Non-Cacheable RAM，
   支持用户数据的字节/非对齐拷贝，避免把 RAM 当作 Device memory 导致 alignment fault；
   `IODMACommand` 准备映射并
   检查连续 IOVA、对齐和 49-bit 地址边界。没有 physical-address fallback。
3. 检查固件交接与寄存器状态。接管 32 KiB ring，禁用 RPTR shadow；使用真实
   `CP_RB_RPTR` 和保留一个 DWORD 的满/空区分。
4. CP 写回启动 timestamp，且 ring 已消费、GPU idle 后，才开放硬件能力。
5. Submit 复制并校验描述符，生成内核私有 IB；ring 写入
   `CP_INDIRECT_BUFFER_PFE`，随后 `CP_EVENT_WRITE(CACHE_FLUSH_TS | IRQ)`。
6. `dsb oshst` 发布 ring/IB，写 `CP_RB_WPTR` 门铃。CPU 不写完成序号。
7. `IOInterruptEventSource` 处理真实中断；`IOTimerEventSource` 每 10 ms 检查硬件写回，
   支持漏中断时回收，500 ms 无完成触发故障。栅栏页只由 CP timestamp 更新。

Fill32 编码为 `CP_MEM_WRITE`，IB 尾部强制加入 `CP_WAIT_MEM_WRITES` 和
`CP_WAIT_FOR_IDLE`，之后才执行 ring 上的 cache flush timestamp。
PM4 Type7 包带 count/opcode 奇校验。XML 寄存器索引先乘 4，作为 byte MMIO offset。

## 平台交接

GPU 的 Apple DT provider 提供 `qcom,gpu-handoff`，类型为 40 bytes OSData，
所有字段为小端，严格拒绝长度、版本、保留字段或未知标志不匹配：

| byte offset | 字段 | 要求 |
| --- | --- | --- |
| 0 | uint32 version | 1 |
| 4 | uint32 flags | 127，下面 7 个承诺全部满足 |
| 8 | uint32 chip | `0x05040001` |
| 12 | uint32 firmwareMode | 1，已初始化且保留的 CP 固件 |
| 16 | uint64 pm4 | 与 `CP_ME_INSTR_BASE` 实际寄存器完全相同 |
| 24 | uint64 pfp | 与 `CP_PFP_INSTR_BASE` 实际寄存器完全相同 |
| 32 | uint64 reserved | 0 |

flags 依次为 clocks pinned、power pinned、NoC pinned、exclusive device mapper、
firmware initialized、non-secure、firmware quiesced。这是 loader/平台提供者的
所有权契约，不能为了启用驱动把尚未满足的标志全部置位。

平台负责初始化时钟、电源、带宽、SMMU、CP 微码、GPMU 和经认证的 ZAP，关闭
preemption/自动 power collapse，并停止旧提交者；固件和映射必须保持到驱动停止。
新的设备 IOMapper 必须管理这个实际 Adreno SMMU 域、保留固件映射和低 VA 区域，
处理页表/TLB 一致性，并且不得把新 DMA 分配与固件地址重叠。
按照当前 XNU 的 Apple IOMapper 发现机制，GPU provider 的 `iommu-parent`
应指向已解析的设备 mapper 对象；仅注册一个全局 mapper 不能满足要求。

驱动额外检查 `SECVID_TRUST_CNTL=0`、`GPMU_GENERAL_0=0xbabeface`、
`GPMU_GENERAL_1=0`、context-switch control 关闭、PFP/ME 都已 halt、没有错误或
SMMU stall、旧 ring idle 且读写指针相同。保留硬件 RPTR 对应的位置，避免假设
只写 WPTR 就能把只读 RPTR 重置为零。随后用 CP timestamp 做实际执行自检。

**已实现保留页表的 Adreno SMMU IOMapper 和 EFI 交接生产者，尚未验证实际平台状态。
默认 DT 仍禁用 GPU，不能只填 compatible 或标志便把当前镜像当作可运行驱动。**

## EFI 交接生产者

统一入口 `scripts/build-bridge.py --gpu-handoff-profile <policy.json>` 已接入
`boot/bridge/gpu_handoff.cpp`。不限定手机型号；策略版本 2 包含 `iova_first`、
`iova_bytes`、`average_bandwidth`、`peak_bandwidth`，以及 `rpm_domains` 的四条
`[id, active_level, sleep_level]`、`rpm_clocks` 的 27 条 `[index, active_kHz_or_bool, sleep_kHz_or_bool]`、
`rpm_interconnect` 的八条 `[id, maximum_Hz_or_Bps, active_floor, sleep_floor]`，以及
`rpm_clock_limits` 的12条 `[index,active_max,sleep_max]`，身份为0..8、12、19、26。
限制为0时明确释放该AP限制；其他值与请求一起传给RPM，不能省略后沿用未知旧值。
字段允许 JSON 整数或 `0x` 字符串。资源身份与现有 RPM 命名空间完全相同，必须完整且不重复。
电压级别、branch 值、时钟与带宽范围、三条共享时钟的跨表一致性会检查；不生成猜测的默认基线。
这些值是平台保留策略，不能把它们视为已观测或已 ACK 的固件历史；原生 RPM 驱动仍会确认全部请求。

构建期只启用这条可选路径并预留版本/标志为零的 GPU/SMMU/NoC 输出位置。
EFI 阶段复用驱动的 GCC/GPUCC retained-state 检查和 RPM 配置校验，读取实际 CP 微码
地址、SMMU bank 0 的 TTBR/ASID/TCR/MAIR 与精确 SID 路由。完整遍历最多 256 张
4 KiB 页表，拒绝环、别名、错误描述符、不可访问内存及重叠 stream，并证明 IOVA 窗口为空。
页表必须来自 EFI 可访问的普通 RAM，不能跟随未经验证的物理指针读取 MMIO 或受保护页。

选择 XNU 的 256 MiB arena 时，排除每张保留页表和所有 block/page 映射的物理范围。
当前 XNU 只获得这一 arena；保留的固件/页表和其他 GPU 可达内存因此不进入它的页分配器。
这个检查在任何内核载荷写入之前进行；不能事后才发现 arena 覆盖了 GPU 内存。

ExitBootServices 成功后，桥才拥有旧 EFI 提交者的停止边界：重新核对全部时钟、路由、
上下文和页表快照，等待旧 ring 完成，屏蔽 GPU IRQ，halt PFP/ME 并等待 AXI drain。
随后清理全部页表到 PoC、执行真实 ASID TLB 同步，再次验证状态，最后复制三份
原生交接记录到 Apple DT。任何失败都不会进入 XNU；EFI 已退出时由既有 watchdog 返回固件。
裸 ARM64 Image 入口会明确停止，因为它没有 EFI 的所有权边界。

此路径已经完成 ARM64 EFI PE/COFF 与 raw Image 的离线链接检查，使用的合成策略
和临时启动镜像均已删除。尚无实际 GPU 策略或真机执行证据；CP/GPMU 初始化可用下面的
可选固件路径，未启用 bank 0 可使用下面的 SMMU 初始化入口；关闭的 GX 和私有时钟
可使用下面的桥内电源启动入口。默认入口保持 GPU disabled。

## 签名 ZAP 装载与安全模式转换

`build-bridge.py --gpu-handoff-profile <policy.json> --gpu-zap-firmware <image.mdt>`
已接实际装载路径。也接受 monolithic `.mbn`；同目录、同基本名称的 `.b00`..`.b99`
自动封装到版本 1 的 `QZAP` 传输容器。原始 ELF、哈希/签名和段字节不修改，逐文件及
容器 SHA-256 写入构建报告。构建报告不声称签名已验证；实际 PAS-13 认证必须成功。

生产用 `AdrenoFirmware.h` 同时处理 packed MDT、独立哈希段和 monolithic MBN：
验证 ELF32 little-endian 头、紧随 ELF 头的 program headers、唯一 hash segment、
段长度、边界、重叠、对齐及文件完整性。PAS 元数据按原始头/program headers + hash
紧凑排列，保留所有签名字段；载荷按 segment 的 p_paddr 偏移复制，BSS、空洞和末尾
padding 清零。重定位镜像调用 PAS_MEM_SETUP；固定地址镜像保留签名地址并跳过该调用。
上限为 100 个 program headers、4 MiB 元数据、16 MiB 载荷区域和 32 MiB 容器。

EFI 阶段通过 AllocatePages 和 EFI CPU Architectural Protocol 分配独占的、低于
4 GiB 且 uncached/CPU-XP 的元数据、载荷、32 KiB ring、4 KiB fence 和八页页表池。
普通新分配先进行 writeback/invalidate，再切换为 UC，避免保护后仍有旧 cache line 写回。
这些分配也避开旧 GPU 的全部映射和页表；固定地址镜像接受 EFI 可以实际分配的
物理区域，或下面显式交接的 GPU carveout。全部分配发生在选择 XNU arena
之前，内存保留到平台复位。未进入 PAS 的准备失败会撤销可以确认的普通分配。

已有 GPU carveout 可在策略中添加 `zap_memory: [base, bytes]`，写入固定 16-byte
LE64/LE64 的 `qcom,gpu-zap-memory`。这声明该区域由当前 GPU 启动者独占、CPU 可访问且
uncached；仍会核对完整范围是 EFI ReservedMemory、UC，未标 RP/WP/RO/runtime，
不与旧 GPU 映射/页表重叠且容纳签名目标。不会依据“reserved”本身推断所有权。
借用区不 Allocate/Free、不在 PAS 前读取或做 cache maintenance；数据只在 PAS 初始化
及必要的 MEM_SETUP 成功后写入。没有明确策略时，不使用或继承其他 DT 中的保留区。

EFI 退出、旧 CP halt 和 AXI drain 后，桥在独立空闲的 8 MiB VA 窗口建立实际 ring/fence
映射并同步 TLB，内核的 allocator window 始终为空。随后通过与原生 QcomSCM 共用的
`SMCCC.h`/`SCMPAS.h` 进行 PAS_INIT_IMAGE、可选的 MEM_SETUP、段复制及 AUTH_AND_RESET。
认证以后 CPU 不再读取、清理或释放受保护的元数据/载荷。未知完成或失败会保留整个图，
停止进入 XNU；已启用的 watchdog 负责复位。

只有真实 PAS 成功后，启动编码器才接受 secure CP 并提交 `CP_SET_SECURE_MODE(0)`、
WAIT_FOR_IDLE 和 CP timestamp。必须读到 trust=0、实际 fence 写入和空闲 ring，才会
公开硬件能力；不会直接写 SECVID_TRUST_CNTL。EFI 期间保持 GPU IRQ mask=0，验证后再
停止 CP/AXI，保留 boot ring/fence/页表映射给原生 mapper。原生 GPU 的 ZAP resume
路径也使用同一个转换序列，避免把 SCM 恢复后的 secure 状态直接当成可提交状态。

只启用 ZAP 装载时，CP/PFP、GPMU、电源/时钟及基本 SMMU 仍需由前一阶段初始化。
布局及 metadata packing 依据固定的 Linux v6.12
[mdt_loader.c](https://github.com/torvalds/linux/blob/v6.12/drivers/soc/qcom/mdt_loader.c)，
原文与哈希已并入 reference manifest；新增解析器沿用 GPL-2.0-only。

## CP/PFP 与 GPMU 固件启动

统一入口增加 `--gpu-firmware <目录>`，同时要求 `--gpu-handoff-profile` 和
`--gpu-zap-firmware`。目录包含原始 `a530_pm4.fw`、`a530_pfp.fw` 和
`a540_gpmu.fw2`；仓库不提供这些文件。原始字节及 SHA-256 封装到 `QGFW` v1：
32-byte 头、三个 16-byte 文件记录、16-byte 对齐载荷，单文件最多 1 MiB。
PM4/PFP 装入实际微码 buffer 时跳过文件首个 word；GPMU 检查剩余 word 数、ID=2、
元数据长度及 4096-word instruction RAM 边界，生成每包最多 127 word 的 type4 写入。
包装和解析都不等同于认证。

策略必须提供 `firmware_power: {"frequency_hz": ..., "voltage_uv": ...}`，转换为
16-byte LE32 `qcom,gpu-firmware-power`（版本 1、频率、微伏、reserved=0）。
电压来自明确的既有工作点策略；不会从时钟反推，也不在这里建立电压 vote。
运行时 GPUCC GFX3D 频率需在 100 ppm 内匹配；策略值可用范围为 19.2 MHz 至 1 GHz、
0.4 V 至 2 V，电压须整毫伏。这些是协议表示范围，实际硬件工作点仍由平台策略负责。

EFI 阶段预分配独占 UC/CPU-XP 微码与 GPMU 命令 buffer，避开旧 GPU 的所有物理映射。
退出 EFI、等待旧 ring idle、halt CP 并排空 AXI 后，桥在同一个独立 boot VA 窗口建立
只读微码/命令映射，完成真实 ASID TLB 同步，内核分配窗口仍为空。
随后执行以下启动步骤：

1. 实际写入并读取 RBBM_SW_RESET_CMD，重新 halt ME/PFP、屏蔽 IRQ 和排空 AXI。
   要求真实 RPTR/WPTR 均为零；绝不写入只读 RPTR 模拟复位。
2. 再核对完整的时钟、SMMU context/stream、TTBR 和页表快照；复位改变了映射状态就停止。
3. 配置 A540 的 VBIF、RBBM、UCHE/1 MiB GMEM、队列阈值、HWCG、bank bit=15、
   CP register protection、64-bit 地址模式及 LMLOADKILL workaround。
   装入 PM4/PFP/RB 地址并读取确认，再解除 halt，发送 A540 的八-word ME_INIT 并等待 idle。
4. 按单工作点写入 GPMU AGC 协议和 SP/TP power-collapse 参数，暂时关闭 CP protected mode，
   执行只读 GPMU IB，再恢复 protected mode。待 IB 完成才释放 CM3 reset。
   清除的 GENERAL_0 必须重新应答 BABEFACE，GENERAL_1/CP 错误必须为零；最多等待 100 ms。
5. 再次 halt/drain，执行签名 ZAP 的 PAS 装载和认证，然后使用共享 Engine 的
   CP_SET_SECURE_MODE(0)、WAIT_FOR_IDLE 与真实 timestamp 自检。成功停止 CP/AXI 后发布交接。

全部微码、GPMU 命令、ring/fence、页表和受保护 ZAP 资源保留到平台复位，原生 mapper
保留这些映射。失败或未知完成不进入 XNU，不释放可能仍被硬件使用的资源。
该路径初始化 CP/GPMU，继续复用已开启的电源/时钟；SMMU 默认保留，或由下面的入口
初始化未启用的 bank 0。额外的桥内电源启动入口见下节；运行时 reset 恢复和 DVFS 仍待实现。
离线检查覆盖原始固件包装、边界、ME_INIT/GPMU 包、应答超时、非安全 timestamp 和三种
EFI/raw 交叉链接。合成固件及临时镜像已删除，没有认证真实签名或执行真机测试。

## SMMU bank 0 冷初始化

统一入口增加 `--gpu-smmu-init`，要求同时启用 `--gpu-firmware`、`--gpu-zap-firmware`
和 `--gpu-handoff-profile`。代码位于 `boot/bridge/gpu_smmu.h`。GPU 使用 SID 0，
stage-1 ASID 按 context index 取 0；寄存器和配置顺序依据固定版本的
`arm-smmu.c`、`arm-smmu-qcom.c` 和 `msm8998.dtsi`，来源及哈希见 reference manifest。

准备阶段只读取硬件，核对 S1/stream matching/4 KiB AArch64 能力、64 KiB 寄存器区、
bank 0 非 stage-2 且 SCTLR.M=0、无已有故障，以及安静的 CP。完整记录全局、bank 0
和全部 SMR/S2CR 状态；拒绝 SID 0 的掩码/重复匹配、其它 SID 使用 bank 0、SID 0 已经
translate 到其它 bank，或没有可用 stream slot。已启用的 bank 0 使用既有保留路径。

桥在选取 XNU arena 前分配独占、低于 4 GiB 的 UC/CPU-XP 根表并清零。
实际 SMMU 根表使用 4 KiB，物理分配按 iOS18 的 PAGE_MAX_SIZE=16 KiB 独占预留。
ring/fence、页表池和微码/命令 buffer 也按 16 KiB 对齐并取整，避免原生 CPU 映射
表页时与相邻受 PAS 保护的载荷共用一个 CPU granule。分配总长和对齐后可用长度分开记录，
复制/映射只使用可用区，EFI 阶段撤销分配使用原始总长。
和其它 boot 分配一样，它不会进入 XNU 页分配器。退出 EFI、halt CP/排空 AXI 后先完成
真实 GPU reset，再核对旧 SMMU 快照，执行 bank 0 初始化：

1. 更新全局 fault/client 配置，要求 unmatched stream fault；暂时使 GPU SMR 无效。
   其它 stream 的 SMR/S2CR、其它 context 和受保护 bank 不写入，也不进行 bypass 探测。
2. 配置 CBA2R=1、CBAR=0x1f300，先写 TCR2/TCR，再写实际 TTBR0 根地址。
   输入/输出位宽按 ID2 能力限制在 32/36/40 bit，4 KiB granule、TTBR1 禁用、
   MAIR[0]=0x44、SCTLR=0x1167；页表和数据均按现有 mapper 的 Normal-NC 格式处理。
3. 在新 VMID 0 下执行 CBn_TLBIALL/TLBSYNC，清除包括旧 global 项在内的全部 VMID-0 缓存；
   再启用 context、SID 0 精确匹配及 translate 路由，对 ASID 0 再次同步。
   每次最多等待 100 ms；没有全局 SMMU TLB flush。每项配置写入均读取确认。
4. 从真正配置后的寄存器与页表重新采集 120-byte SMMUContract，再使用现有生产用
   RetainedPageTables 映射 boot ring、fence 和只读微码/命令。内核 allocator window 保持为空。
5. 完成 ME_INIT、GPMU、签名 ZAP 认证、非安全转换和实际 timestamp，停止 CP/AXI 后，
   将同一份原生交接记录交给 QcomGPUMapper。原生 ABI 和 Kext 不需要新增虚槽或接口。

任何冲突、快照变化、写入未生效或未知 TLB 完成都会停止进入 XNU；已修改硬件后的
根表及全部 boot 分配保留到平台复位。仍要求 GPU CX/GX、GCC/GPUCC 已开启、固件允许
非安全 SMMU 配置及明确的 RPM 策略。这里不处理已有活动 context 的格式替换或故障恢复。
额外的桥内电源入口见下节；已有有界原生 CP reset 与作业私有页表，系统 sleep、DVFS 和持久任务 VA 管理仍待实现。

现有 GPU 检查覆盖寄存器初始化、40-bit 新根交给原生页表实现、其它 stream/bank 保留、
冲突拒绝、TLB 超时和写入失败；现有桥检查覆盖 CLI 依赖。五种 EFI/raw 配置已完成
交叉链接，合成固件、策略、内核副本和启动镜像已删除。没有执行目标设备或真实认证。

## 桥内 RPM、电源与时钟启动

在上述固件/SMMU 参数之外增加 `--gpu-cold-start`，统一构建入口会检查实际内核 collection
包含 `QcomRPM 1.4.0` 或更新版本。实现位于 `boot/bridge/gpu_power.h`，复用原生 GCC、
GPUCC/GDSC/Fabia、RPM transport/regulator/clock/interconnect 后端。

该模式从 **GX 已关闭且 SW_COLLAPSE=1** 开始；CX 可以已开启或关闭，控制位与状态必须一致。
桥在分配 XNU arena 前只读取并记录状态。控制总线、XO、共享 GPLL0 和私有 PLL 的模拟
CONFIG/TEST 配置须由固件提供；不重置或重调 CPU/内存共享 PLL，也不猜测模拟参数。
GX 关闭的证明取代旧 GPU 页表遍历；旧任务不能跨电源关闭重放，新的空根在复位及隔离后安装。

`firmware_power.frequency_hz` 必须是固定 MSM8998 OPP，CX/MX 的 active level 都要达到下表。
`voltage_uv` 仍由明确的平台策略提供给 GPMU，RPM corner ACK 不等于实测电压。

| GPU Hz | 最低 CX/MX corner |
| --- | --- |
| 257000000 | 48 |
| 342000000 | 64 |
| 414000000 | 128 |
| 515000097 | 192 |
| 596000097 | 256 |
| 670000048 | 320 |
| 710000097 | 384 |

退出 EFI 并复核原始状态后，桥依次完成：

1. 建立独占 GLINK-RPM 通道；四条 domain、27 条 AP clock、八条 interconnect 以及 scaling
   请求逐项收到实际 message-ID ACK。未确认的资源不能进入供电或时钟写入。
2. 开启拥有的 GCC GPU gates，开启 CX、解除 CX/RBCPR reset；保留 PLL 模拟配置，编程
   L/alpha/CAL_L，完成必要的 Fabia 校准、UPDATE/ACK/LOCK，设置固定 GFX3D 工作点。
   读回频率允许 100 ppm 量化误差，供电映射也覆盖该误差，避免随后 gate admission 拒绝向上取整频率。
3. 执行 GX reset/clamp/retention/GDSC 与 GPU 分支时钟启动，建立真实时钟状态租约。
   屏蔽 IRQ、halt CP、等待 AXI drain，复位 GPU；按 RBCCU→SP 顺序启动内部块并等待 power-ready。
4. 配置新 SMMU 根和精确 SID 0 路由，再以实际 RPM BIMC/EBI grants 接管 GPU NoC。
   bank 0 可以曾经配置过，但只有本模式已经证明 GX 关闭、复位并隔离 CP 时才允许替换。
   CBn_TLBIALL 清除新 VMID 的旧 global/non-global 缓存，随后按 ASID 同步；不改其它 context/stream。
5. 执行既有 PM4/PFP/GPMU、签名 ZAP 和非安全 timestamp 启动路径；再次停止 CP/AXI，
   排空 RPM TX/RX，封存生产者，发布 GPU/SMMU/NoC 及 RPM 通道记录。

SMMU 全 ASID 失效使用 `CBn+0x618`，依据
[ARM SMMUv2 IHI 0062D.c §16.5.30–31](https://documentation-service.arm.com/static/5f900d34f86e16515cdc08fb)
及固定 MSM8998 下游 `arm-smmu-msm8998.c` 的定义。它覆盖该 context VMID 的 unlocked
条目；仅 TLBIASID 无法保证清除旧 global 条目。来源及 SHA-256 见 reference manifest。

RPM policy 交接版本提升为 2，新增 `qcom,rpm-transport-handoff`，固定 48 bytes、小端 12 个 u32：
`version=1, flags=7, txOffset, txLength, rxOffset, rxLength, txHead, rxTail, messageID, remoteID, 0, 0`。
flags 表示 Connected、Drained、ProducerStopped；生成前必须没有在途请求或未完回复，且 TX/RX 都为空。
桥封存后不能再发送。原生 RPM 核对 TOC、管道几何、游标与 TX drain，保留本地游标和消息 ID，
消费可能迟到的 reset/close，再重新 ACK 自己的资源基线；不会重新握手或把 FIFO 游标归零。
远端 RX head 可以在交接后推进；旧 ACK 不得完成新的 vote。版本 1/未交接的入口保留原初始化行为。

任何 ACK、PLL/GDSC/内部块 ready、路由、写入读回或 TLB 完成不确定，都停止进入 XNU并保留资源到复位。
现有 power/GPU/bridge 检查覆盖这条状态机及失败分支，五种 EFI/raw 配置完成离线链接。
临时合成固件、策略和镜像已删除；未验证真实时序、供电、签名认证或 GPU 执行。

## 原生 SCM/PAS 与 ZAP 恢复

`QcomSCM.kext : IOService` 通过 Apple `AppleARMIODevice`、`IONameMatch` 和
Kext 依赖匹配。逻辑 DT 节点使用 `compatible="qcom,scm-msm8998"`、`method="smc"`，
没有伪 MMIO 资源。服务同步注册后发布 `SCMControllerReady`；GPU Kext 1.19.0
声明对其 1.0.0 的依赖，且只使用同一 arm-io bus 下的唯一 SCM 服务。

`SCMPAS.h` 和原生后端实现实际 `smc #0`：先用 fast INFO/IS_CALL_AVAIL 探测 SMC64，
未知 64-bit 函数可回退到 SMC32，不猜测旧式 SCM。普通 PAS/BOOT 调用使用 standard SIP
FID，x1 携带 Qualcomm 参数数量/类型，x2..x4 传 PAS 参数。中断返回 1 时按 A6 quirk
使用 x0=1、原始参数和返回的 x6 cookie 续调，最多 1024 次；busy -12 每 30 ms
重试，最多 20 次。WAITQ_SLEEP 和未知结果关闭会话，不能当作成功或重发原始事务。

GPU 的 warm 路径先持有 RPM/GCC/GPUCC 与 BIMC/EBI 租约，确认 PFP/ME halt、AXI drain
和非安全状态，再调用 `BOOT_SET_REMOTE_STATE(0, 13)`。返回成功后再次检查 halt、
drain、idle 和 trust 状态；允许 PAS 恢复后出现已认证的 secure 状态，建立 mapper/DMA
后用上面的 CP_SET_SECURE_MODE 序列退出，再完成真实 timestamp 自检。
运行准入还检查 SCM 会话 token；`GPUZAPReady` 表示本次恢复调用已成功。
停止时在 CP 隔离和 owned mapper 关闭后释放会话，不关闭 loader 拥有的 ZAP。

原生内核平台函数 `SecureFirmwareRequest` 使用 64-byte、version=1 的类型化请求：
Acquire/Resume/Validate/Release，以及 InitImage/MemSetup/Authenticate/Shutdown。
只允许已 open 的同 bus `QcomGPU` 调用，限定 PAS ID 13，无用户态客户端、裸物理地址或
任意 SCM 指令入口。64-bit 单调 token 防止旧持有者复用；保留字段、输入指针类型和
调用状态都会检查。调用者必须继续保持 GPU 电源、时钟、NoC 和所需的 CP/AXI 隔离。

冷装载的 PAS 接口已有实际调用和资源生命周期：InitImage 复制 ELF32 little-endian
元数据到独占的、连续且 4 KiB 对齐的 uncached `IOBufferMemoryDescriptor`，保持 prepare；
MemSetup 接收已保留且 prepare 的连续物理内存 descriptor，检查范围、地址宽度和与
元数据不重叠。Authenticate 在载荷写入后执行 native IOMemoryDescriptor cache store，
再调用 PAS_AUTH_AND_RESET。Shutdown 得到真实成功后才 complete/release 两份内存。
元数据最多 4 MiB，载荷区域最多 16 MiB；SMC32 要求整个范围在 4 GiB 以下。
当前接口顺序为 InitImage → MemSetup → Authenticate → Shutdown，服务结果失败仍保持
可以确认的阶段；metadata/region 设置被拒绝或任意未知完成会保留内存和持有者到复位。

桥已连接 `.mdt/.bXX`/`.mbn` 装载、重定位与 PAS 认证；原生 Kext 当前仍采用 loader
保留的 ZAP 会话，不从用户态接收签名文件。CP/GPMU 初始化由上述 EFI 桥处理；
原生运行时的 reset/sleep 恢复仍未实现。
SCM 服务本身不能生成签名固件。
默认 GPU 仍禁用，离线编译和主机检查没有执行目标设备的 SMC。

调用约定、PAS 参数和 A6 行为依据固定的 Linux v6.12
[qcom_scm-smc.c](https://github.com/torvalds/linux/blob/v6.12/drivers/firmware/qcom/qcom_scm-smc.c)、
[qcom_scm.c](https://github.com/torvalds/linux/blob/v6.12/drivers/firmware/qcom/qcom_scm.c) 及
`smccc-call.S`，原文和哈希已并入现有 reference manifest。QcomSCM 采用 GPL-2.0-only，
独立 NOTICE 随原生 Kext 打包；GPU 的 Apple IOGPU ABI 不增加自定义虚槽。

## 保留运行配置的平台租约

`QcomGPUPlatform` 在访问 GPU/SMMU MMIO 和建立 DMA 映射之前，通过同一 Apple
`GenericARMIO` bus 的 `ClockControllerRequest` 获取 RPM、GCC、GPUCC 租约。
这是现有 controller 的内核协议扩展，沿用各自的 IOKit matching 和 command gate；
consumer 12 专用于 GPU，普通 power user client 无法构造这些请求。
`TargetedRequest` 仍为 48 bytes；新增操作为 RetainGPUState、ValidateGPUState、ReleaseGPUState，
id/flags/reserved 必须为零，value 在获取时为零，此后为该 controller 返回的不回绕 token。
旧 v1/v2 操作编号保持不变。GPU Kext 1.19.0 要求 GCC/GPUCC 1.2.0、RPM 1.3.0、SCM 1.0.0；
公共 QcomPowerController 的 C++ 实例布局已变化，其兼容版本同步提升到 1.1.0，
依赖它的其他 power Kext 也必须一起重建，不能混入旧版本。

GPUCC 租约只读取并记录已开启的 CX/GX、电源控制/夹钳、复位、分支、根时钟、Fabia
PLL 的运行/锁定状态和分频配置。所需的 RBCPR、GFX3D、RBBM timer、CXO 必须已开启。
它不会调用普通 `RetainDomain`：该流程即使面对已经开启的 GX，也会执行复位/夹钳序列。
租约持有期间，GPUCC 的时钟、频率、电源域和复位修改统一返回 Busy。
GCC 租约保留 GPU BIMC、配置总线及 GPU PLL 输出分支，其他外设的时钟请求继续工作；
GPU CFG AHB 被标记为永久 critical，普通 release 也不能关闭它。
两处配置检查使用实际寄存器；状态变化或读到未知值后，租约不可恢复地保留到平台复位。

RPM 获取租约前必须完成完整 clock handoff，传输正常，且 CX/MX active level、BIMC Hz 和 EBI
平均 B/s 都有非零的已确认请求。声明完整初始域交接时，GPU 首次获取租约会自行触发基线初始化；
不再要求 USB 或其他客户端先投票。租约把这些现值作为软件最低值：其他客户端仍能提升请求，
降低或释放其请求不会把共同资源降到 GPU 获取时的值以下。带宽先聚合再取下限，不重复累加
既有流量。这里只保留已有的 BIMC/EBI 请求，不构造未确认的 GFX master RPM ID，
也不把请求当成物理频率/电压测量。缺少这些先决状态会拒绝硬件接管，不会猜测电压或带宽。
GPU 专用 NoC QoS 和静态平均/峰值带宽租约已实现，见下节。按工作负载动态调节带宽，
以及从实际固件采集初始 RPM 基线的生产者仍需后续实现。

启动 CP 前后、提交/映射及运行中的 watchdog 都检查平台租约；空闲时每 100 ms 检查。
关闭时先 halt CP/等待 AXI drain，再完成 DMA、销毁自己的 SMMU 页表，恢复 NoC QoS 并撤销
GPU 的带宽投票，最后逆序释放 GPUCC、GCC、RPM 租约。释放只移除软件保护，保持引导者的硬件状态；后续普通 RPM 事务
才可按当前消费者重算更低的请求。CPU 低电源状态只暂停新提交，仍持有全部租约。

若时钟/电源配置丢失，驱动不再尝试 GPU/SMMU MMIO，也不释放可能仍被硬件访问的缓冲区、
映射或平台租约。诊断等待者收到错误，原生 Family fence 按 FIFO 通知失败，但 job/ticket/资源引用继续保留到平台复位。
这个路径没有实现自动恢复、电源塌陷、系统休眠、DVFS 或冷启动，也没有真机执行记录。

GCC/GPUCC 寄存器和 Fabia 状态依据项目固定的 Linux v6.12 源码：
[GCC](https://github.com/torvalds/linux/blob/v6.12/drivers/clk/qcom/gcc-msm8998.c)、
[GPUCC](https://github.com/torvalds/linux/blob/v6.12/drivers/clk/qcom/gpucc-msm8998.c)、
[PLL](https://github.com/torvalds/linux/blob/v6.12/drivers/clk/qcom/clk-alpha-pll.c)。
内核匹配、服务对象和平台调用使用本项目 Apple IOKit 模型。

## GPU 的 BIMC/EBI 路径

`QcomGPU.kext` 1.18.0 在已有三份平台租约之上，使用 `QcomGPUNoC` 管理 GPU 的
AP-owned BIMC GFX3D 端口 1。依据固定的 Qualcomm 源码，此节点采用 bypass；
它的本地 `BKE_EN` 位于 BIMC `0x01000000 + 0x8000 + 1*0x4000 + 0x300 = 0x0100c300`。
驱动只修改 enable bit 0，保存原值，保留其他位及 health/threshold/override 配置。
不会给 AP-owned GFX master 发送远程 RPM master 请求。
证据是 [MSM8998 总线表](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/3fb35ef1f0fe1f714be3df39724cbc8eb217181e/arch/arm/boot/dts/qcom/msm8998-bus.dtsi)
和 [BIMC 实现](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/3fb35ef1f0fe1f714be3df39724cbc8eb217181e/drivers/soc/qcom/msm_bus/msm_bus_bimc_adhoc.c)。

硬件接管现在要求 GPU provider 的第三个 `reg`（index 2）为 `0x01000000 / 0x80000`。
index 0 仍是 GPU，index 1 留给 Adreno SMMU；匹配和资源仍由 AppleARMIODevice 提供。
另需 `qcom,gpu-noc-handoff`，共 32 bytes 小端记录：

| byte offset | 字段 | 要求 |
| --- | --- | --- |
| 0 | uint32 version | 1 |
| 4 | uint32 flags | 3：独占 port 1、路由保持固定 |
| 8 | uint64 average | GPU 平均带宽，B/s，非零 |
| 16 | uint64 peak | 峰值带宽，B/s，至少等于 average |
| 24 | uint32 port | 1 |
| 28 | uint32 reserved | 0 |

这些请求由实际平台能力决定，不能为启用驱动填入假值。它们还必须通过 RPM 的最大值和
共同负载检查。框架诊断路径不需要该属性；存在 GPU 固件交接时，缺少该记录或第三份资源会拒绝
硬件启动。引导桥按 `IIQQII` 转换字段，不会交换 64-bit 带宽值的高低半部。

驱动先持有电源/时钟租约，实际检查 CP halt 并等待四个 VBIF XIN drain，之后才允许 QoS 写入。
此顺序也覆盖外部 IOMapper 路径。GPU 以 kernel consumer 12 投票共享 BIMC `0x2002` 和
EBI slave `0x2104`，先确认 sleep 基线、提高 active 峰值时钟，再提高平均带宽，最后设置 bypass。
峰值时钟请求为 `ceil(peak / 8)` Hz，至少 19.2 MHz，再向上对齐到 kHz。
该消费者不能通过 RPM 接口修改其他总线节点；普通 power user client 仍不暴露写带宽操作。

共享 RPM 后端同时聚合 GPU、USB 和已有 EBI 平均请求，并考虑 SNOC→BIMC 的平均值。
最低时钟为 `ceil(max(EBI_AB, SNOC_BIMC_AB) * 153% / 2 / 8)`，向上对齐 kHz，
再与引导下限、消费者峰值时钟和 retained 下限取最大值。平均量在两个通道分摊，峰值不分摊。
提高带宽前必须确认足够的时钟；降低时先确认带宽下降，再降低时钟。
明确拒绝带宽更新时恢复此前的派生时钟下限；带宽已确认后若时钟步骤失败，或任一步完成未知，
整组 BIMC/EBI/SNOC 请求停止复用并保留资源。单独的非法请求在任何传输/初始交接前拒绝。

运行期间，NoC 的实际 bypass 位和已确认 RPM 请求参与原有健康检查；失败会关闭硬件准入。
停止时必须再次证明 CP/AXI 已隔离，并完成所有 DMA/SMMU 销毁，再恢复原 BKE enable 位，
依次撤销 GPU 的平均带宽和峰值时钟，最后释放平台租约。失败或未知 ACK 会保留该图到平台复位。
引导者/其他客户端的下限继续有效，释放 GPU 请求不承诺物理总线关断。

该实现管理静态带宽预算，没有按命令负载做 DVFS，也没有进行 BIMC 或 GPU 真机访问。
`AdrenoInterconnect.h`、`QcomGPUNoC.cpp` 和公共 RPM 路径使用 GPL-2.0-only，随 Kext 的现有
`NOTICE.SMMU.txt` 一并声明。

## RPM 初始基线和服务启动顺序

RPM provider 可添加 `qcom,rpm-domain-handoff`。它是完整四个 16-byte 记录，
与既有 `qcom,rpm-handoff-version=1`、27 个 clock 记录和八个 interconnect 记录共同使用。
每条为小端 `uint32 id, activeLevel, sleepLevel, reserved`，id 必须各出现一次：
`0x1000` CX、`0x1001` MX、`0x1002` SSC CX、`0x1003` SSC MX；reserved 为零。
level 只能使用 RPM 后端认可的离散值。当前级别和睡眠级别表示引导者持有的完整 AP 请求，
不能从频率或 GDSC 开启状态猜测。GPU 使用的 CX/MX active level 和 BIMC/EBI 下限必须非零。
缺少完整域记录时，仍可接管已经由现有内核消费者确认的状态，但不会伪造初始 ACK。

初始化首先重新发送八个 active/sleep 域基线，收到真实 ACK 后才标记已知；
然后保存 52 个 clock 状态，在启用全局 clock scaling 前确认全部 16 个 interconnect 基线，
最后启用 sleep/active 两个 scaling set。不需要额外提升 BIMC 时钟的冷缓存路径共 78 个资源消息；
新的共享带宽下限可能增加 BIMC 时钟消息。
这里的“初始化”只指已声明的 RPM 资源请求，不包含 GPU 冷启动。
所有普通投票先通过资源/值/聚合校验再触发传输；初始化期间也保留首个实时消费者的投票。
域基线作为永久引导下限，GPU 临时租约释放不会把它清零。已完成阶段不会重复发送；
明确拒绝可重试，超时或未知完成会保留状态到平台复位。

GCC、RPM、GPUCC、SCM 在同步注册其 IOKit 服务后，分别发布 `GCCControllerReady`、
`RPMControllerReady`、`GPUCCControllerReady`、`SCMControllerReady`。GPU personality 的 `IOResourceMatch`
使用匹配 `IOResources` 的 dictionary，内部同时匹配四个资源；实际平台请求仍限定同一
`GenericARMIO` bus 下的唯一控制器。它等待服务发布，不声称电源或 GPU 已开启。
这一匹配结构依据 [Apple XNU 11215.81.4 的 IOService/IOResources 实现](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/iokit/Kernel/IOService.cpp)。

控制器暂时 Offline/Busy/NotReady 或分配内存暂时失败时，GPU 在自己的 Family gate 内
每 100 ms 重试获取尚缺的租约；已持有的 token 会先复查并保留。
只允许在尚未映射 GPU/SMMU 寄存器、尚未建立 DMA 时重试，连续准入开启的等待最多 10 秒。
此时 `GPUStage=resource-wait`、`GPUStartupPending=true`，资源/命令准入保持关闭。
成功后完成现有 CP 启动自检才公开硬件能力；失败公开 `GPUStartupResult` 并停止重试。
未知 ACK、变动的租约、无效交接和 CP 自检失败均不会重启硬件。PM 暂停期间保持租约，
重新开启准入时更新等待期限；这不构成系统休眠或掉电恢复支持。

既有 `scripts/build-bridge.py` 现在按记录转换上述 FDT 属性，以及 40-byte GPU 和
120-byte Adreno SMMU 交接。32-bit 字段转 LE32，64-bit 地址/限制整体转 LE64，
不会把一个 64-bit 值当成两个独立 cell 调换高低半部。记录数和长度必须匹配。
`apple_native` 节点沿用原始 Apple DT 字节；驱动仍检查交接版本和保留字段。
默认 DT 仍禁用 GPU，工具不会生成假基线、CP 固件地址或 SMMU 所有权标志。

## 保留 SMMU 页表的原生 IOMapper

`QcomGPUMapper : IOMapper` 由已匹配 `AppleARMIODevice` 的 GPU 服务创建，作为同一 provider
的设备 mapper 附着、启动并注册。`fIsSystem=false`，不会替换全局 mapper 或 USB 等设备的域。
创建完成才把真实对象绑定到 provider 的 `iommu-parent`；若已有 mapper 对象，继续使用原有显式映射路径，
两条路径不能同时声明所有权。启动前系统 mapper 的选择必须已完成，不能仍为 XNU 的等待哨兵值。

GPU provider 需额外拥有第二个 `reg`：`0x05040000 / 0x10000`，以及第二个 level IRQ：
Adreno context bank 0 的 SPI 329。原有 GPU MMIO / SPI 300 仍分别为索引 0。
这些资源不能同时交给另一个 SMMU driver。驱动先确认 CP 停止，再实际请求 VBIF halt 并等待 AXI drain。
随后验证 stage-1、4 KiB granule、context geometry、实际 ASID/TTBR、TCR/MAIR 和完整 stream 路由，
只接管 bank 0；secure/其他 context、全局控制及 SMR/S2CR 配置不被重写。

新增 `qcom,adreno-smmu-handoff` 为小端、固定 120 bytes OSData：

| byte offset | 字段 | 要求 |
| --- | --- | --- |
| 0 / 4 | uint32 version / flags | 1 / 31 |
| 8 / 12 | uint32 bank / asid | bank=0，ASID 为实际 TTBR 的高 16 bits |
| 16 / 20 | uint32 inputBits / outputBits | IOVA 32..40；PA 32、36 或 40，均不能超出 ID2 能力 |
| 24 / 28 | uint32 streamCount / reserved | 1..8 / 0 |
| 32 | uint64 ttbr | 完整的活动 TTBR0，含 ASID；4 KiB 表页对齐 |
| 40 / 48 | uint64 first / end | 专属空闲 IOVA 窗口，[first,end)，4 KiB 对齐，至少从 16 MiB 开始，窗口 8..512 MiB |
| 56..119 | 8 × {uint32 slot,id} | 精确 SMR slot/SID；不使用的记录为零 |

flags 为 `ExclusiveContext`、`BootTablesReserved`、`TablesPublished`、`LifetimeResourcesPinned`、
`ProducerStopped`，含义依次为上下文独占、旧表页由引导层保留、旧表写入/缓存已发布、
fabric/时钟/电源在整个交接生命周期固定、旧生产者停止。它们是需要实际满足的承诺。
硬件还必须为 AArch64、TTBR1 关闭、Normal-NC 页表遍历、outer-shareable、MAIR[0]=0x44，
翻译/故障报告/故障 IRQ 与 ASIDPNE 开启，stall-on-fault 关闭；具体寄存器值在实现中严格校验。

`RetainedPageTables` 借用旧树的 CPU 映射并保留原有固件 leaf/block 映射，绝不释放旧物理表页。
驱动遍历并确认整个分配窗口为空，且 CP PM4/PFP 地址仍能通过旧树翻译；保留该树供内核与诊断使用。
新增中间表来自真实 `IOBufferMemoryDescriptor`，使用 host physical contiguity 与 Normal-NC cache 选项，
逐页核对物理跨度、对齐和输出地址宽度。`MapperNone` 与 Buffer-MD cache mask 的重叠位不会被混用。
新链接先清零子表再发布，leaf 包含 AF/nG、只读或 RW、outer-shareable 和 XN；stage-1 的 write-only 请求按 RW 表达。
每次变更显式针对内核 ASID，通过公共 SMMU context TLBIASID/TLBSYNC 等待完成；
这允许 CP 正在运行另一个已登记的作业 ASID。同步失败后，表页和映射都不能复用。

GPU 1.22.0 在 `AdrenoAddressSpace.h` 实现作业独立的空页表，由 `QcomGPUMapper` 填入本作业的
真实 prepared backing。固件与 ring 只读，IB/代码/常量只读，私有索引只读，flush timestamp
使用独立可写页；全局完成页与其他作业资源不进入私有树。原生 ring 通过 A5xx
`CP_SMMU_TABLE_UPDATE` 在 IB 前后实际切换根与 ASID，切回内核树后才写完成 timestamp。
该设计共用单 ring 和设备 IOVA 分配器，隔离粒度为每份提交，未新增持久的任务 VA 分配器。

健康检查以 64-bit MMIO 读取 TTBR0，只允许内核根或正在保留的作业根；
未知值最多重读 8 次、间隔 1 μs，容纳固件写寄存器对的短暂过渡，持续未知则停止提交。
只有硬件 timestamp、根已退出及 ASID TLBSYNC 都成立后才释放私有表页、原始 backing 并复用 ASID。
CP 错误/超时成功 halt/drain 后，先恢复内核根及上下文 TLB，再销毁失败作业页表；未知状态保留整个对象图。
启动和原生 CP 恢复后还会通过真实 Engine 执行私有写入/完成自检，再开放隔离提交。
自检代码已经实现，本轮未在硬件执行；旧 retained-only 或外部 mapper 配置继续提供内核诊断能力。
原生路径要求桥的 `--gpu-firmware` 和有效的固定只读固件页交接。
完整映射及生命周期规则见 [原生私有页表](native-iogpu.md#作业私有-gpu-页表)。

硬件命令依据 [固定 MSM8998 驱动 adreno_iommu.c](https://github.com/LineageOS/android_kernel_qcom_msm8998/blob/d152ec623891d141ad5231ef5033c068e82ba1b1/drivers/gpu/msm/adreno_iommu.c)
的 A5xx table update/idle 顺序和固定 PM4 XML；APRIV/protected-mode 编排同时对照
[Linux A5xx 页表切换原始补丁](https://lkml.iu.edu/hypermail/linux/kernel/1905.3/04872.html)。
当前没有硬件抢占，不建立或使用该补丁中的抢占记录与 TTBR1 split tables。

原生 `iovmMapMemory` / `iovmUnmapMemory` 实现 `IODMACommand` 普通 descriptor/page-list 路径，
按 CPU 的真实 PAGE_SIZE 展开为 4 KiB SMMU 页，返回连贯 IOVA，支持非连续 PA、宽度/对齐限制及前后 guard page。
当前仅面向 GPU 自建 buffer；paging/device/identity/fixed-address map 和旧式 `iovmInsert` 明确拒绝。
分配账本同时保留 descriptor 与 DMA command；原生 unmap 的 owner、地址、长度必须完全匹配。
SMMU 故障 IRQ 在平台租约仍健康时记录 FSR/FAR、终止异常 stalled fault，交由 GPU 的 Family gate 停止 CP 并使队列失败。
若平台状态已经丢失，则跳过 SMMU MMIO 并直接保留相关资源。
CP 启动也在同一 gate 内完成，避免 MMU 故障和重新放行 CP 的竞态。

成功解除映射并确认 AXI 停止后，shutdown 才移除新中间表链接、完成最后 TLB 同步并释放新表。
旧表和固件映射保持不变。仍有资源、同步失败或所有权变化时保留 mapper、MMIO 和 provider lease。
GPU buffer 使用 `setMemoryDescriptor(...,false)`、显式 `prepare/complete` 和 `clearMemoryDescriptor(false)`，
避免 Native 自动完成吞掉错误；失败的 complete 不重试。`QcomGPUBuffer::free` 同时保留自己的 prepare 引用：即使 Native DMA complete 已消费元数据，
也不会在 map/unmap 未退休时 unwire/free 仍可被 DMA 访问的页。
故障后的隔离解除/平台 reset 回收仍待实现，目前失败隔离会一直保留资源直到平台重置。

IOMapper 的 208-byte / 174-slot 原生 ABI、IODMACommand 的 120-byte / 31-slot ABI 对照已固定的
同一 iOS18 kernelcache 验证，另外检查实际 `QcomGPUMapper` 的 174-slot 编译器虚表。
现有 gpu-submit 检查已覆盖保留固件 block、空闲窗口/地址别名拒绝、map/unmap、失败回滚、
停止后销毁和未知 TLB 完成时的表页保留。它不模拟原生 IOKit callback，也不证明 MMIO/SMMU 在设备上可运行。
可复用将来的 `gpuctl native-fill-test` 和已有 Metal 示例做端到端验证；本轮没有运行目标程序。
本路径链接公共 `QcomSMMU/ContextTLB.h` 的 GPL-2.0-only 实现，新增 Kext 资源 `NOTICE.SMMU.txt`
保留相应许可证与源码位置；其他文件保留原有许可声明。

## 诊断连接的用户态提交格式

协议版本 2，连接类型 `0x47505502`。旧版本内部客户端会在打开时被拒绝。
Submit 的 4 个 scalar 是 queue、command buffer、offset、bytes；command buffer 中
连续存放 1..64 个 `GPU::Command`，每个 32 bytes：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| opcode | uint32 | Fill32=1、Barrier=2 |
| reserved | uint32 | 必须为 0 |
| destination | uint64 | 当前连接、同 context 的 buffer 句柄 |
| offset | uint64 | 目标 buffer 的 byte offset，4-byte 对齐 |
| bytes | uint32 | 填充字节数，非零且 4-byte 对齐 |
| value | uint32 | 重复写入的值 |

Barrier 的其他字段全为零。命令没有 GPU VA、寄存器、IB 指针或任意 PM4 字段。
单个生成 IB 最多 4096 DWORD，超过容量整体拒绝，零命令也拒绝。
诊断连接保持 Fill32/Barrier；原生连接命令版本 3 另支持 Copy32 和已登记 shader 的
draw/dispatch 描述符，见 [原生桥接](native-iogpu.md) 和 [Metal 说明](metal.md)。
不接收任意 PM4、用户 IB 嵌套或用户提供的 shader 机器码。

成功 Submit 表示私有 IB 已发布到硬件 ring，返回队列内递增 fence；不表示完成。
全局硬件 sequence 是 uint32，耗尽后拒绝新提交，避免回绕歧义。
GetFence 拒绝零号和未发出的编号；fence 以当前连接的 queue 为作用域。
已完成 fence 在后续故障后仍保持完成，
尚未完成的 fence 返回 I/O error。没有 CPU 伪完成或恢复后重放。

## 生命周期与故障

最多 16 个在途 job。每个 job 持有私有 IB、目标 buffer 和 client 的引用，只有硬件
栅栏完成或 DMA 已确认停止后才释放。索引绘制还持有最多 16 MiB 的内核私有索引快照，
全 GPU 在途快照受 16 MiB 上限约束，同样在 halt 失败时保留。关闭连接可以撤销用户句柄，但不会提前释放
正在被 GPU 使用的映射。源 command buffer 自身未作为在途目标时，提交后可修改/销毁，
私有 IB 已是快照。

目标 buffer 在途时 CPU 读写/销毁返回 busy；队列有在途 job 时销毁也返回 busy。
所有请求、完成、关闭和故障都在同一个 Apple 工作循环锁下执行。

CP/RBBM/UCHE/GPMU 错误、SMMU stall、异常 RPTR、伪造/倒退 fence 或 watchdog
触发时停止准入，mask GPU IRQ，halt PFP/ME，并请求四个 VBIF XIN 全部 halt。
确认 AXI drain acknowledgement 后才能清理 DMA；超时则保留服务和整个在途映射图，
进入隔离状态直到平台 reset，避免 use-after-free。满足下面条件的 CP 故障可执行有界复位恢复；
失败作业不会 replay，平台电源丢失或 SMMU 故障继续隔离，没有系统 sleep 恢复。
PM 低状态仅暂停准入，继续回收已有 job，平台必须保留 clocks/power/SMMU。

## 原生 CP 复位恢复

GPU 1.19.0 在实际 AXI drain 后，可以恢复 CP 错误、中断/RPTR 异常或提交 watchdog。
桥和内核共用 `AdrenoStartup.h` 的 A540 reset/ME_INIT/HWCG/GPMU 实现；
`boot/bridge/gpu_firmware.h` 只保留兼容的薄入口，避免两套寄存器初始化代码漂移。
原生编排位于 `AdrenoRecovery.h` 和 `QcomGPUHardware.cpp`。

桥必须使用 `--gpu-firmware` 并完成固件、ZAP 和非安全 timestamp 启动，才发布
`qcom,gpu-firmware-handoff`。构建期预留全零位置，运行时成功后填写固定 64-byte 小端记录：

| offset | 字段 | 要求 |
| --- | --- | --- |
| 0 / 4 | u32 version / flags | 1 / 31：loader 所有、GPU 只读、保留到复位、已初始化、允许 reset |
| 8 / 16 / 24 | u64 PM4 / PFP / GPMU IB VA | CP 地址匹配主交接，页对齐、不重叠 |
| 32 / 36 | u32 PM4 / PFP mapped bytes | 页对齐，各 4 KiB..1 MiB，含零填充 |
| 40 | u32 GPMU IB words | 1..0x1021 |
| 44 / 48 | u32 GPU Hz / microvolts | 与已初始化固定工作点相同 |
| 52..63 | 三个 u32 reserved | 0 |

原生 `QcomGPUMapper` 要求三个区域都在自己的空闲分配窗口之外，逐页检查实际 4 KiB
Normal-NC、AF/nG、XN、unprivileged read-only 描述符，固定并保留每个完整 PTE 的原值。
此后普通健康检查、map/unmap 的 TLB 事务和 reset 阶段都重新比对这些 PTE。
不接受 block mapping、可写叶子或只有声明没有实际映射的固件；不把固件 VA 当成 CPU PA
读取，也不访问受 PAS 保护的镜像或元数据。旧保留入口没有这份记录时，仍提供原有提交和隔离能力。

恢复在已有 Apple 工作循环/命令门中执行：

1. 关闭准入，屏蔽 IRQ，halt ME/PFP 并等待真实 AXI drain。确认停止后让所有失败作业以
   错误结束，释放已能安全回收的 job 引用；没有完成的命令不生成成功 fence，也不重放。
   已提交的原生失败队列和诊断队列不能继续提交，客户端需创建新队列。
2. 信号通知原生失败事件，运行 Family fence/通知退休，并等待当前 Family 调用退出。通过 timer 重新调度，
   不占着命令门阻止旧调用返回；最多等待 1 秒。PM 低状态仅延迟恢复，Shutdown 取消它。
3. 确认 RPM/GCC/GPUCC、NoC、SCM 会话和原生 mapper 全部健康，再执行真实 RBBM reset。
   复查完整 SMMU 身份和所有固定固件 PTE，同步实际 context TLB，按 RBCCU→SP 顺序等待 ready。
4. 在原生私有 ring 上用保留的 PM4/PFP/GPMU 映射重建 ME_INIT、A540 寄存器与 GPMU。
   每阶段重查资源，停机后再次执行真正的 `BOOT_SET_REMOTE_STATE(0,13)`；不把旧软件
   Warm 状态当作本次 ZAP 恢复，也不重新读取或认证 loader 所有的受保护镜像。
5. 通过同一生产用 Engine 执行 CP_SET_SECURE_MODE(0)、WAIT_FOR_IDLE 和新 timestamp，
   同时确认 trust=0、ring idle 和真实 fence 写回，再开放新提交。旧 job/ticket 已全部离开
   硬件时间线，新的序号从自检开始，不能和旧完成误配。

每个常驻服务最多尝试三次复位；任一次恢复失败便停止后续自动恢复。halt/drain 失败、
电源/NoC/SCM 丢失时不再访问 GPU，保留整个 DMA 图直到平台 reset；SMMU/页表故障也
不通过自动重建来清除隔离。失败作业的私有树须先安全退休，恢复成功后再执行私有页表自检；
此处不包含持久任务 VA 管理、系统 sleep 或 DVFS。
IORegistry 发布 `GPURuntimeRecoveryAvailable`、`GPURecoveryPending`、`GPURecoveryAttempts`、
`GPURecoveryCount`、`GPURecoveryStage` 和 `GPURecoveryResult`；不会把尝试计数当作成功计数。

原始 A5xx reset 顺序依据固定 `a5xx_gpu.c::a5xx_recover`，原文及哈希已在 reference 清单。
现有 GPU 模型直接运行共享恢复编排，覆盖旧 IB 不重放、GPMU/SMMU/ZAP/非安全转换失败，
以及真实编码器生成的 timestamp；这不能证明设备时序或原生 IOGPU 回调已经在硬件运行。

## 桥内 CP DMA 提交与读回

`boot/bridge/gpu_submission.h` 接入 `gpu_handoff.cpp::loadZap` 的真实 bootstrap ring。
启用 `--gpu-zap-firmware` 的保留、固件初始化、SMMU 初始化和 cold-start 路径，
在非安全 timestamp 成功后继续提交 Fill32 → Barrier → Copy32 → Barrier 的私有 IB。
使用共享的生产 `Adreno::Encoder` / `Engine`，命令包含实际 IOVA；CPU 只初始化守卫值，
不填入预期结果或完成序号。等待最长100 ms；只有 CP 的 CACHE_FLUSH_TS 完成后，
才以 volatile 读取检查源和目标各一整页，包含写入区间以及所有未写入的边界。

工作区复用已有16 KiB对齐、UC/CPU-XP 的 fence reservation：4 KiB timestamp、
4 KiB只读 IB，以及分别4 KiB可读写的源、目标页。4 KiB GPU PTE 的权限独立；
不将 CPU 地址当作设备地址，也不占用私有页表池。原有 ring、固件和8 MiB bootstrap
IOVA 窗口保持原布局。XNU arena 检查还显式排除全部 EFI reservation，包含对齐前缀
及没有 GPU 映射的 PAS 元数据/签名镜像区域。

`SubmissionIO` 从最终 bootstrap engine 启动开始保留 fatal IRQ 位。读取状态及确认
中断前均采样，因此 `Engine::stop` 清除硬件错误位后，交接仍能拒绝停机期间发生的错误。
无论 IB/读回是否成功，都要在时钟所有权仍有效时尝试 halt/AXI drain；所有权丢失则停止
访问 GPU，保留全部内存至复位。只有提交、读回、停机、错误检查和既有页表/平台校验均
成功，才发布现有40/120/32/64-byte交接记录。没有增加 Apple 选择器或修改 Shader/JIT ABI。

统一 `scripts/build-bridge.py` 的构建清单记录该检查是否启用、所需工作区、命令和完成条件；
`hardware_boot_verified` 保持 false。普通 retained-only、没有 ZAP bootstrap 的模式不运行该检查。
本轮只复用现有 GPU 提交测试覆盖正常、超时、守卫损坏、CP错误、所有权丢失及停机错误保留，
并交叉链接 retained/ZAP/firmware/SMMU/cold 五种 EFI/raw 模式。合成固件、策略和启动镜像
已删除，仅保留构建记录及目标文件；没有执行 A540、PAS 认证、Metal 或真机测试。

## 离线验证

`tests/gpu_submit_test.cpp` 使用生产用 `Engine`、编码器和描述符校验器，模拟 MMIO
和 DMA，独立解释生成的 PM4 并写目标/栅栏。覆盖：

- PM4 包类型、长度和奇校验，以及填充的实际目的地址/值。
- 跨 context、陈旧句柄、越界、未知命令、IB 容量和 command 快照。
- 未执行时 fence 不前进，执行后数据/IRQ/栅栏更新。
- ring 跨末尾回绕、占满与回收，异常 RPTR 和不可能的 fence。
- 启动拒绝、自检超时、硬件错误、halt 超时及保留 DMA 所有权。

运行 `make test-gpu`、`make gpu`、`make verify-gpu`，另对提交测试启用 ASan/UBSan。
`gpuctl fill-test` 是未来目标系统的显式测试入口，本次只编译，没有执行。
这些验证不能证明固件接管、寄存器假设或真机时序已经正确。

硬件来源及固定哈希见 [sources.json](reference/sources.json)，主要对照
[Linux A5xx 提交/IRQ 实现](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/msm/adreno/a5xx_gpu.c)
及同版寄存器 XML。PFP/ME halt 位还对照了同清单中的 Qualcomm 下游快照代码。

sRGB 格式沿用相同 IOGPU 资源和四字节布局，原生准入识别 format5/6 并核对 IOSurface 的对应 RGBA/BGRA 存储。采样设置 `TEX_CONST_0_SRGB`；颜色目标同时设置 RB 与 SP 的 `COLOR_SRGB`，R/B 交换与颜色空间独立。Compute read/write 共用生成的分段转换 helper，几何常量仍在 word16..47；mip 生成使用相邻非重叠子区域的硬件 sampler 和受限 SSBO store，经真实 fence 串行执行。shader24/JIT21 配套更新，Dispatch792 / Draw2168 / Sampler28 bytes 与 Apple 外层 ABI 不变。本轮仍为离线实现与检查。

共享RPM策略现要求QcomRPM1.10、27项时钟请求和12项显式MAX限制。末项index26为PIMEM；原0..25身份不变。
index19 的 QDSS 使用0..4的 STATE 枚举，不能填入KHz频率。原生和引导入口均拒绝
范围外的 active/sleep 值，避免把RPM钳位后的ACK误作原值已生效的证明。
USB与GPU都必须显式提供其active/sleep值，不能把固件低功耗请求或缺项当作零基线。
桥接的读取长度与循环使用Native相同的`RPMHandoffClockCount`，两个set的ACK先于
全局缩放；普通与cold GPU策略均检查实际集合中的QcomRPM版本。
