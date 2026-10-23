# 原生 IOGPU 资源与队列桥接

QcomGPU : IOGPU : IOAcceleratorES 使用 Apple 匹配、Family 生命周期、设备/任务命名空间、
资源对象、通知队列和 fence machine。当前 Kext 源码版本为 1.92.0，声明 IOGPUFamily >= 2.0.0。
集合纳入 IOSurface、IOReportFamily 及安全依赖，不装入 AGX 硬件驱动。

GPU1.86 / Metal0.70增加原版Metal的setResponsibleTaskIDs:count:桥接；私有命令版本为4，
Apple的SubmitArgs、资源句柄及通知ABI保持不变。DWORD任务ID按调用时复制，最多64项，
以顺序操作影响其后的GPU命令；null清空列表。提交共享区含有有界、16字节对齐的ID前缀，
准入和indexed/framebuffer背压读取均检查实际命令偏移。内核将列表冻结在真实作业ticket中，
资源和IOSurface权限仍由原生IOGPUDevice的owningTask决定。只有真实像素工作成功退休后，
GPUCompletedResponsibleTaskIDs才更新；gpuctl stats可检查这份记录。此阶段传递任务归属提示，
尚未接入系统能耗计费，IOGPUNativeTaskEnergyAccounting明确为false。64项边界、原生方法签名、
交叉编译和74项集合验证已核对；round60已通过版本4空任务前缀的64资源两笔异步回读及4096像素Render，非空任务前缀尚未取得硬件证据。原版CARenderer已越过该selector，随后在AIR常量特化失败；系统UI加速仍未通过。

**已实现原生 buffer / IOSurface 资源和异步命令准入到 A540 CP 的桥接，支持 Fill32/Barrier/Copy32
及 MSL/AIR AOT 或 JIT shader 的 Compute/Render 描述符。
用户态已有 [Metal 着色器、Render/Compute 和线性纹理子集](metal.md)，系统插件入口已实现。round-41 已证明旧版本原生 CP 队列；round-43 已取得原版 Metal 的设备发现结果，像素路径继续真机复测。** 已有保留页表的 Adreno SMMU IOMapper、SCM/PAS-13 服务及 EFI 交接生产者；
仍需已初始化 GPU、实际平台资源策略和有效交接，这些条件不满足时，资源/队列创建和提交返回 NotReady。
IOGPUNativeSubmission 属性与 GPU::NativeSubmission 能力位只在 CP 实际就绪时开启。

## IOSurface 导入

`NewResource` 复用 Family 的原生类型 `0x82`，+56/+60 为 surface ID / plane，
+64 cache options 为零；拒绝固定 GPU VA、用户地址、保护属性和资源组。
`IOGPU::nativeCopySurfaceForTask` 是字节审计后的重定位别名，调用真实 IOSurfaceRoot task lookup，
校验当前设备任务对 ID 的访问权。未添加全局 ID 查找或绕过原生 namespace 的替代接口。

实际 `IOGPUResource::newResourceWithIOSurface` 由 IOSurfaceDeviceCache 创建内部 `0x80` 根资源和
客户端 `0x82` plane 资源。根资源持有原始 IOGPUSysMemory / IOMemoryDescriptor，按原始 cache 属性
通过 QcomGPUMapper 映射一次；客户端复用该 native map，并持有带偏移的 buffer view。
返回 GPU VA 与硬件描述符各自加一次 `baseOffset + planeOffset`，不会重复叠加。
根资源不强引用 IOSurface；客户端资源及在途 ticket 保持 surface retain/use count，避免缓存引用环。

支持非平面、plane 0、element 1×1、每像素 4 bytes 的线性 RGBA/BGRA 表面。
width/height 最多 16384，行距至少 width×4、64-byte 对齐且不超过 A5xx pitch 字段；
原始描述符和 DMA 长度必须一致、整页、至多 16 MiB，图像区域必须完整落在该描述符内。
有保护选项、tiling、压缩或视频多平面的表面均拒绝。尺寸上限仍受 16 MiB 实际分配配额约束。
`ResourceData.privateInfo` 内 64-byte 厂商元数据记录真实格式、行距、偏移和分配长度，其余清零。
外层 88/208-byte 结构、选择器和虚表槽数均不变。创建不增加冲突 cache-mode 的 CPU 映射。

提交前、真实 timestamp/确认 DMA drain 后，用原始 prepared descriptor 的
`kIOMemoryIncoherentIOFlush` 完成非一致性 CPU/GPU cache 同步。SDM835 的 XNU 目标没有定义
`__ARM_COHERENT_IO__`，该操作实际进入 ARM64 cache clean/invalidate 路径。
GPU 写资源在数据可见后调用原生表面修改标记，推进 seed / CPU invalidation 标记，然后才发布成功 fence。
缓存同步失败按 I/O 错误结束并关闭提交；DMA 停止无法证明时仍保留整份 ticket、surface 和映射。

物理页在 prepare 期间建立有序索引；复制重叠、附件反馈和索引 CPU 快照检查实际 backing range，
不能用不同 surface ID、资源 ID 或 GPU VA 绕过。索引快照也会拒绝与在途作业重叠的范围。
资源 IOVA 由同一受限设备 IOMapper 分配；实际执行使用每作业私有页表，仅映射该作业持有的资源。
尚无持久的每任务 VA 分配器；用户仍须等待 GPU 完成再进行 CPU 访问。

硬件就绪才发布 `IOGPUNativeIOSurfaceImport=true` 和 `GPU::SharedSurfaces`。
Metal 入口及支持边界见 [Metal 适配](metal.md#iosurface-纹理)。审计方法、工厂和 getter 的固定字节
保存在 `drivers/abi-22H374/apple-gpu-abi.json`、`drivers/abi-22H374/apple-surface-abi.json` 以及 `build/drivers/gpu-surface-abi-inspection.json`。

## 原生 API

GPU1.88 / Metal0.76 离线补齐 `newBufferWithBytesNoCopy:length:options:deallocator:`。
原版 Family 的资源类型 `0x80` 直接导入 owningTask 的内存；参数 +56 是暴露地址，
+64 是 allocation base，之前将后者命名为 clientLength 不准确。当前仅接纳二者相同、
原生整页对齐、默认 CPU cache 的 Shared 分配。大小、地址加法、资源/连接预算
在原生工厂前检查；原版 VM region 检查和 descriptor 所属任务保持有效。
不调用自己的分配工厂，不复制颜色数据，也不建立另一个 CPU cache alias。

同一个 `0x80` 还用于 IOSurfaceDeviceCache 的内部根资源，两者由 device owner 区分，
不允许客户端资源充当 IOSurface 根。外部 buffer 沿用原版 IOGPUSysMemory、prepared
descriptor、IOMapper 和在途作业引用；提交前及真正退休后进行原有 cache 同步。
Metal 只在成功导入后接管 deallocator，资源释放后在 transport lock 外调用一次；
创建失败不调用它，nil deallocator 也不会使外部内存进入 Metal purgeable 状态。
GPU1.91接纳原版descriptor工厂选择的kIODirectionOut只读输入；kIODirectionInOut仍可读写。round79已验证原始只读映射的GPU blit/compute/sample读取及写入拒绝、原数据不变；这是输入资源权限，渲染输出仍使用独立可写资源。
所有GPU写目标在retainBuffer时检查原始描述符权限，作业页表按相同权限设置只读AP；
IOSurface视图继承backing权限。原生IODMACommand也把Out转换为kIODMAMapReadAccess，
不会扩大进程VM权限。Private、WriteCombined请求、子分配和地址范围数组暂不接纳。

round77捕获16条client-layout拒绝，已记录的描述符均为direction=2，长度、DMA长度、offset及owner正常；
这证实原有只接纳InOut的检查阻断了实际UI输入。GPU1.91已交叉编译；现有metalctl blit-test
加入mprotect后导入的只读来源，复用原有blit/compute/sample回读，并检查写请求失败及内容不变；尚待真机执行。
原版工厂与参数位置的只读证据在 `build/gpu-runtime/ui-shared-resources/apple-client-buffer.json`。
它补齐 GPU 资源导入；显示端去掉逐帧 CPU 复制仍依赖独立的 DPU DMA 映射。

类型 1 仍由 IOGPU::newUserClient 初始化原生客户端、IOGPUDevice、任务及 stamp 映射。
类型 33 保留 Apple 内存信息客户端；诊断类型 0x47505502 保留独立协议 2。
诊断连接要求管理员。原生类型1/33允许管理员，或由 Apple `IOUserClient::copyClientEntitlement` 核实 `com.apple.backboardd` 为 OSBoolean true 的系统合成服务。角色来自实际22H374 backboardd 签名 entitlement；不按进程名、UID 或用户传入属性授权。返回的 entitlement 对象按 retained 约定释放。

GPU1.72 另允许同时持有原生 `com.apple.security.iokit-user-client-class` 数组中的 `IOGPUDeviceUserClient`、`com.apple.QuartzCore.displayable-context=true` 和 `com.apple.QuartzCore.secure-mode=true` 的系统显示客户端。仍由 `IOUserClient::copyClientEntitlement` 读取签名权限，Family 的任务、资源、IOSurface 检查和独立诊断连接的管理员限制保持有效；只有 backboardd 角色计入 compositor。工程镜像增加 SpringBoard 的精确可执行路径，round-44 已记录其原生 type1 连接成功；这不是通用应用或 UI 像素加速的验证。

GPU1.56.0 让带相同已验证角色的系统合成服务使用编译器注册的程序。原始 ISA `RegisterProgram` / `DeleteProgram` 在每次 externalMethod 调用时独立调用同一 `nativeShaderTask` 鉴权，接受管理员或签名 backboardd 角色；不依据进程名、UID 或调用方字典。Family 的任务/资源命名空间、IOSurface 权限、程序配额及提交隔离继续适用。普通应用和通用 ISA 沙箱仍未开放，权限检查尚未在目标系统验证。策略与证据见 [UI 服务访问记录](ui-service-access.json)。

### Mercury 初始化失败的静态复核（2026-10-19）

本次仅分析修复方案，没有连接、启动、测试或写入设备，也没有修改驱动、扩展或固件。
此前实际 Mercury 日志中的 `WallpaperCoordinatorError.unavailable` 对应共享 Resources 为空；
其初始化只在 `MTLCreateSystemDefaultDevice()` 或 `newCommandQueue` 返回 nil 时失败。
初始化失败被 once 缓存，因此重新唤醒不会重新创建这些资源。该阶段早于壁纸纹理、机型缩放、
WallpaperMetalView 和 drawable 尺寸处理。现有日志没有区分设备和队列两条失败路径。

静态发现驱动准入与固件内 Mercury 不兼容。`QcomGPU.cpp` 的 `newUserClient` 对非管理员
原生 type 1/33 连接调用 `nativeShaderTask`，只允许 backboardd，或同时具有
`displayable-context`、`secure-mode` 和 `iokit-user-client-class` 中 `IOGPUDeviceUserClient`
的显示客户端。固件内提取的 Mercury（SHA-256 `9e123c8b9598622bcb69e9698157f4f903985f1da26968db01553b256868615c`）
有 `com.apple.posterkit.provider=true` 和 `com.apple.QuartzCore.secure-mode=true`，
没有另两项显示权限，也没有 backboardd 权限。因此非管理员 Mercury 若到达该分支，
必然返回 `kIOReturnNotPrivileged`（`0xe00002c1`）。root 工具通过管理员分支，不能据其成功
推断 Mercury 也能取得设备。这里是自研驱动附加的准入策略，没有另行创造一个 Apple entitlement。

已核对上次启动包的实际 `keystore-policy.kernelcache`（SHA-256 `1d98b0a66397ee65022bfe63a6a427fa9f671aba87758e6cdfd173245cab9f5e`）：
内嵌 GPU1.137.0 的 `nativeShaderTask` 位于 `0xfffffff006d26578`，`newUserClient` 位于
`0xfffffff006d2679c`；GPU 整个 text 段与 bridge 策略处理前逐字节一致。
实际打包 Metal 插件 SHA-256 为 `16a36a7eea0a46a4ad30fd6d7ffeda6763a56f2ac1b162949dfab1d78eb6765d`。
其 `initWithService:error:` 在 `0x6670` 调用原生连接，`NativeConnection::open` 在 `0x51f7c`
打开 type 1；打开失败返回 nil。原生 Metal 的设备发现会排除初始化返回 nil 的插件实例。
因此上述拒绝足以形成 Resources 不可用的完整静态失败路径。
插件内附构建 JSON 的旧哈希不能代表当前二进制；本次使用实际 readback 文件反编译核对。

建议先补齐受限的原生 Poster 角色，复用现有 provider/secure-mode 权限并验证任务代码身份，
由同一个判断覆盖设备准入和 `QcomGPUNative.cpp` 中的 JIT Register/DeleteProgram 复核。
仅修改 `newUserClient` 会在加载壁纸着色器时再次遭拒。现有 JIT ABI 接受机器码，结构验证
并非通用 ISA 沙箱，不能据此将所有应用或任意自带 provider entitlement 的程序加入信任范围。
针对当前开发固件，可先固定 Mercury 的已验证 CodeDirectory cdhash
`1f438cab478a18f10e84e8f33e1c79487b5179d9`，并同时检查上述两个角色权限；新增其他
原生 Poster 前逐个核对身份。任务代码身份可通过目标内核已导出的 `cs_get_cdhash` 获取，
不按进程名、路径或 UID 授予驱动/JIT 权限。当前提取文件的 CodeDirectory flags=2、platform=0，
162 个代码页及 XML/DER entitlement special slot 哈希均已离线核对；没有验证 Apple CMS 链，
不能假定额外要求平台签名标志就会兼容当前固件。此处仅给出方案，没有实现或放宽权限。

沙箱是另一层：上次启动的 `development-sandbox-iokit-path` 没有 Mercury，且原生配置可能按
Apple GPU 服务/客户端类匹配。缺少路径例外本身不证明原生沙箱已经拒绝，也不是驱动拒绝
的替代解释。应记录实际拒绝的 hook、服务/客户端类、连接 type 和错误，再决定是适配
原有 GPU 类规则还是增加开发环境的精确例外；保留其余策略。只加路径不会消除驱动检查。
目前保留的 UART 没有 Mercury 的对应 IOKit 调用证据，首次失败也可能发生在插件加载/
设备发现阶段，所以本次确认的是必修兼容性缺口，尚未证明它是唯一运行时原因。

后续实现应先为插件设备初始化及队列的 newQueue、priority、shmem、notification queue/
port 等步骤记录阶段与原始 IOReturn。初始化尚无 GPU 连接时使用用户态日志，不能依赖
GPU 的 MetalFailure 上报；同时让 `initWithAcceleratorPort:` 保留其当前被丢弃的错误信息。
队列失败仍须释放已创建的资源，不能伪造对象或跳过 unavailable 分支。
离线验证范围为角色正反向准入、两处鉴权一致、错误路径资源清理及交叉构建。
未来另经用户允许运行时验证时，必须观察实际 Mercury 成功创建协调器及画面；新日志若
转为 shader、格式或管线失败，再按真实请求处理。不能以 root 探针、全局帧计数、
独立工具 UIScreen 0×0 或旧诊断错误使用 Depth32Float 的结果替代。

证据位于 `build/setup-background/20261019/mercury-original-entitlements.json`、
`mercury-static-gpu-access.{json,asm}`、`mercury-static-{device,connection,queue}.c`、
`mercury-static-analysis.json` 及此前的 `mercury-render-failure-evidence.json`。

| 选择器 | 功能 | 当前限制 |
| --- | --- | --- |
| 0..5 | GPU、名称、stamp、共享内存及 ID 查询 | 沿用 Family，按精确尺寸清零并检查输出 |
| 7 / 8 | 创建 / 删除命令队列 | 原生 1032-byte QueueArgs；普通优先级，最多 8 个 |
| 9 / 10 | 创建 / 删除资源 | 原生 88-byte ResourceArgs；类型 0 buffer / 0x80 用户分配 / 0x82 IOSurface，每连接最多1024个，预算按实际窗口，单资源至多16 MiB |
| 13 / 14 | 创建 / 删除 shared memory | 每块最多 64 KiB，连接包装层最多 32 块（GPU1.72 起；旧版为8 KiB） |
| 15 / 16 | 创建 / 删除通知队列 | 原生 40-byte 条目，最多 256 项、8 个队列 |
| 25 | 绑定通知队列 | 句柄在当前设备命名空间解析 |
| 26 | 提交命令缓冲区 | 原生 4 scalar + 40-byte SubmitArgs，每次 1 个缓冲区 |
| 0x1000 / 0x1001 | 厂商 JIT 注册 / 删除 | 版本33；41832-byte image，当前连接的 shmem / program ID，最多 256 个动态程序 |

其余选择器、通知端口注册及 trap 关闭。用户地址仅限上述原版整页导入路径；不接受资源组、
子分配、保护内存或固定 GPU 地址。资源经设备专属 IOMapper 分配 IOVA，
以设备命名空间和类型检查解析，再写入作业独立的硬件页表；未实现持久任务 VA 分配器、抢占或多个硬件 ring。

## 资源与提交路径

资源工厂创建 QcomGPUResource : IOGPUResource，实际 IOGPUSysMemory 管理 backing。
资源建立用户 CPU 映射；QcomGPUMemoryMap : IOGPUMemoryMap 持有 QcomGPUBuffer，
由指定设备的 IODMACommand 准备描述符、建立并检查连续 IOVA。没有物理地址回退。
这些 map 不进入 Apple 硬件地址空间的链表，也不写 Apple GPU 页表。

QcomGPUCommandQueue 保留原生初始化、调度和 fence 注册，在槽 35 处理厂商编码：

1. 选择器 26 沿 Family 路径处理通知队列及 start/end token。
2. 在当前设备 shmem 命名空间取得保留引用，映射到内核，复制命令快照。
3. 在同一设备资源命名空间解析目标，检查类型、归属、长度、对齐和操作码。
4. 生成内核私有 PM4 IB，进入现有 A540 FIFO/ring，与诊断提交共享硬件 sequence。
5. 成功 post 后分配 channel 0 的 Family stamp，先推进 issued，再将事件加入真实队列，
   记录 submitted 并返回 Family。MIG 成功只表示命令已接收，不等待硬件完成。
6. Family 将队列事件复制到本次实际 IOGPUBlockFence 并登记；作业继续持有该 fence。
   IRQ/watchdog 在硬件 timestamp 退休后完成 cache / IOSurface seed 更新，再写原生 CPU stamp、
   唤醒并运行 Family fence machine，由其产生实际 end token。未完成的事件不能发成功通知。

这是厂商命令钩子的异步准入桥接；槽 205 的通用 IOGPUWorkQueue 工厂仍关闭，
不解释 Apple AGX segments。不同原生调用可以在前一作业执行时继续准入，受 ring / 资源配额限制。
诊断连接仍保留自己的同步接口。原生 stamp 页与 CP fence 页独立；CPU stamp 只代表已确认的
完成或带错误状态的作业退休。Family 将 stamp 页以只读属性映射给用户，用户不能写它制造完成。

`IOGPUNativeCompletionVersion=1`、`IOGPUNativeAsyncSubmission` 和
`GPU::AsynchronousSubmit` 标记这条路径。通知条目保持真实 Family 的 40 bytes：
token、CPU start/end 时间、status、reserved、auxiliary；用户态必须等待对应的 end token。
`NativeConnection::waitCompletion` 消费原生环，传播错误，使用有界等待；目前以 1 ms 轮询消费，
没有开放 Mach 通知端口。Family 的时间是其 CPU 记账时间，不能当作校准后的 GPU 执行时间。

单 ring 的 stamp 严格 FIFO 退休，后完成值不能跨越未退休作业。空闲时 issued 等于 completed。
在 Family 的计数范围阈值前，只有硬件作业、原生 pending fence / callback 和其他 Family 调用全部
排空，才清理各队列旧事件并换代；公开 token 不回绕，`IOGPUNativeCompletionEpoch` 记录内部换代。
信号发送可能同步进入 Family scrubber，因此先释放其 fence 锁再唤醒，避免重入死锁。

## 作业私有 GPU 页表

1.22.0 为每份原生提交建立独立、初始为空的 AArch64 stage-1 页表，使用独立 ASID。
页表只包含实际保留的 buffer / IOSurface backing、私有索引副本、只读命令/着色器 IB、
只读 ring、已固定的只读 PM4/PFP/GPMU 固件，以及绘制专属的可写 flush timestamp 页。
不复制整个内核页表，不映射其他任务或其他作业的分配，也不把页表存储本身映射给 GPU。
原生完成页保留在内核地址空间，GPU 用户着色器不能通过私有树改写它。
IOSurface view 按原始 backing 映射；经当前任务授权共享的 surface 仍可由不同作业合法使用。

实际 ring 在 IB 之前生成受控的 idle、protected-mode / APRIV、WAIT_FOR_ME 和
`CP_SMMU_TABLE_UPDATE`，使 CP 切换到该作业的 TTBR0/ASID，并失效旧 draw state。
IB 完成后执行同样的有序切换，返回保留的内核 TTBR0，随后才写全局完成 timestamp。
固件和 ring 在两个地址空间中的 GPU VA/物理页一致；没有引入 Apple GPU 页表格式或用户提供的页表地址。
硬件序列参考固定 MSM8998 的 `adreno_iommu.c` 与 A5xx PM4 XML，来源记录在 reference 清单。

CPU 健康检查只接受保留内核根或已登记作业的根，用原生 64-bit MMIO 读取 TTBR，
避免将两次 CPU 32-bit 读取拼成假根；对固件可能分两次总线写入的过渡最多采样 8 次、间隔 1 μs，
稳定的未知根仍立即关闭提交。内核 map/unmap 显式对内核 ASID 执行 TLBIASID/TLBSYNC；
私有树发布前和已提交树退休时也执行对应 ASID 的真实失效握手。
仅在完成 timestamp 之后、该私有根已不再被选中且 TLBSYNC 成功，才释放页表和复用 ASID。
错误路径先实际 halt PFP/ME、确认 AXI drain，再恢复内核根并清除上下文旧 TLB，随后退休私有树。
无法证明这些条件时保留页表、ASID、全部 DMA backing 和平台租约，不生成成功通知。

驱动启动及有界 CP 恢复后，用真实 Engine 向私有 buffer 写模式并等待返回内核地址空间后的 timestamp，
再读取模式、退休页表。只有这条运行时自检成功，才发布 `IOGPUNativeAddressSpaces` 与
`GPU::IsolatedSubmit`。这里实现了自检代码，本轮没有在设备执行它。
原生提交现在要求 `--gpu-firmware` 产生的 64-byte 固件交接、固定的只读固件 PTE、原生 mapper
和自检成功；旧 retained-only / 外部 mapper 路径继续提供其已有内核诊断接口。
`GPUAddressSpaceMode`、`GPUAddressSpaceSelfTestPassed/Result` 描述实际状态。

隔离粒度是每份原生作业；同一任务的不同作业也使用独立根。Apple 资源 ID、返回 IOVA、
88/208-byte 资源结构、40-byte SubmitArgs、Family fence 和 shader/JIT 协议均保持原值。
这仍不是非特权机器码沙箱，JIT 登记继续只允许管理员连接；完整语言及持久任务 VA 管理尚未实现。

## 厂商命令编码和用户态

定义位于 drivers/include/GPUNativeProtocol.h。外层 SubmitArgs 保持 40 bytes：
commands shmem ID、segments、sideband、flags、startToken、endToken、resources、reserved。
除 commands 和 token 外，其余为 0；scalar 为 [queueID, 0, 1, 40]。
token 严格递增，endToken = startToken + 1，拒绝回绕。

commands shmem 内是 16-byte CommandHeader，随后 1..64 个 32-byte GPU::Command。
magic = 0x47505543，version = 3，保留字段为 0。Fill32 的 destination 为原生 32-bit 资源 ID。
Copy32 将源/目标 ID 打包在 destination 的高/低 32 位，将源/目标偏移打包在 offset 的高/低 32 位，value 为 0。
双方均检查归属、范围、对齐和非重叠；源与目标一起保留到实际 fence 完成。
Fill32 只在资源范围内写入；Barrier 的其他字段为 0。不接收 PM4、GPU 地址字面量、
用户 IB 指针或 AGX 命令缓冲区。

ShaderDispatch/ShaderDraw 要求单条命令，固定偏移携带 `GPUShaderProtocol.h` 的 POD
快照。shader 命令版本为 33，Dispatch 为 824 bytes，Draw 为 2816 bytes，携带
独立的 Vertex/Fragment 各 8 项 buffer binding、80-byte 语义渲染状态、D32/S8 附件和 72-byte 深度/模板状态及 UInt16/UInt32 索引、instanceCount、baseVertex/baseInstance。
支持混合、颜色写掩码、viewport/深度区间、剔除、绕序、深度比较和前后面模板操作。
状态中的浮点数以 IEEE-754 位模式传输，内核以整数验证范围并生成硬件编码。
Apple 选择器、原生命令版本 3、Family 对象 ABI 保持原值；JIT 登记版本为 30。
旧 shader / JIT 描述符会被拒绝，需配套更新用户态 bundle。
当前 MSL 程序还可携带 `depthTextureMask`，将 D32 / D32S8 深度平面接入原生 read/sample/compare；普通深度纹理可有最多15级 mip，MSAA 仍限单级，使用恒等 swizzle，禁止深度写入和 stencil/cube 着色器绑定。颜色 IOSurface 不允许重解释为深度。该类型掩码复用旧 MSAA 保留 word，结构尺寸不变，旧 Shader/JIT 版本仍会拒绝。D32/S8 共用按像素字节数对齐的线性 mip 布局；D32S8 两类平面保留各自的层步长。绘制和 resolve 附件必须先选出具体 mip/slice，内核拒绝未展开的 mip 链。
内核用 program ID 和登记表指纹查找内置或当前连接登记的 A540 程序，验证每个 buffer 的
线性访问 footprint、texture 描述符及渲染状态，生成私有 shader/state/PM4；
提交描述符不携带机器码。JIT 注册另使用厂商选择器，要求已有管理员权限并信任用户态编译器；
元数据检查不构成机器码访存安全证明，不能向非特权应用开放。纹理和 VFD 顶点输入仍以原生 buffer 资源承载，未新增 Apple 资源类型。
Draw 同时携带活跃 attribute/layout 的语义快照；内核基于索引扫描结果与实例 divisor 验证 footprint，再生成 VFD。
具体 AOT / JIT、Render/Compute 和纹理限制见 [Metal 说明](metal.md)。

索引绘制的资源仍为普通原生 buffer。内核拒绝仍与在途 GPU 写入重叠的索引快照；待前序操作完成后，从已准备的
IOMemoryDescriptor 复制完整索引范围到作业私有 DMA buffer，扫描相同副本以确定
baseVertex 修正后的最大顶点；硬件只读取该副本。triangle strip 的全 1 restart
标记不计入 footprint，普通 triangle list 不启用 restart。顶点/实例 buffer 的
footprint 分别按 program 的 instanceMask、实际索引和 baseInstance 校验。
每份索引快照最多 16 MiB，在途快照总量最多 16 MiB，完成/失败/隔离沿用完整作业生命周期。

JIT program description 为 2424 bytes，包含 32 项固定宽度 varying、16 项 attribute 寄存器记录及实际
FS barycentric/位置分量元数据。内核校验语义身份、float32 宽度、活跃 compmask/inloc、
寄存器和容量，再按片元输入链接 VS 输出，生成完整 SP/VPC/HLSQ/GRAS/RB 状态。
用户态 pipeline 工厂共用同一个链接器；shader 编译、AOT/JIT 混用和数组声明顺序
不会改变原生资源 ID 或资源/队列选择器。设备端插值行为仍未经过真机验证。

GPU::NativeConnection 提供资源、队列、shmem、通知队列、实际 IOKit 通知消费和提交方法。
gpuctl native-info 查询设备；native-fill-test 等待真实 end token 后读回。
native-async-test 连续准入两份不同快照，再分别等待结束并检查数据，
用于以后明确安排的设备检查。本次只交叉链接，未执行目标程序。

## 生命周期和失败处理

同一连接只允许一个 Family 调用在途；该调用完成准入后就可接收下一次提交。
有在途作业的队列删除、通知重绑定返回 Busy；并发 Family 调用同样返回 Busy，
避免等在 Family 队列锁上阻塞 IRQ。clientClose 等当前调用结束，
再在 gate 外执行原生清理。服务停止时先 halt/退休硬件作业，排空 Family 调用，
关闭原生客户端，最后执行 IOGPU::stop。

作业保留 native device、实际 Family fence、native queue、native resource、DMA buffer 和私有 IB。
设备引用覆盖其任务/命名空间的生命周期；用户删除资源句柄或关闭连接不会提前解除在途映射。
超时沿用 CP halt/VBIF drain；halt 失败时保留完整 DMA 对象图及服务，
原生失败通知按 FIFO 发布，返回错误也不会解除映射。Family 调用不能安全排空时同样保留 Family 和服务，
避免提前释放仍在使用的 fence machine。全局另限制客户端数、活资源数和保留内存。

GPU backing、ring、fence、IB 使用正常非缓存 RAM，CPU-only 诊断缓冲区使用正常缓存 RAM。
Buffer MD 的 MapperNone 和缓存模式共用位段，不能与 WriteCombine 叠加。
DMA 始终要求显式设备 mapper。用户 task 的 Buffer MD 先建立内核映射再清零，
避免将用户 VA 交给内核 bzero。

## 私有 ABI 证据

契约为 drivers/abi-22H374/apple-gpu-abi.json。当前 arm64 样本是 iOS 18.7 internal / 22H368，
SHA-256 aa65c36c486a8b5cb1d7a2d565d1d6b3cc228b3c172f8c69c5da664ee19832cf。
编译 SDK 为 iOS 18.6。

| 类 | 实例字节 | 虚函数槽 |
| --- | ---: | ---: |
| IOAcceleratorES / IOGPU | 136 / 744 | 168 / 215 |
| IOGPUObject / IOGPUTask | 16 / 464 | 16 / 21 |
| IOGPUEventMachine / IOGPUDeviceUserClient | 5200 / 304 | 25 / 189 |
| IOGPUEventFence / IOGPUFenceMachine | 208 / 120 | 18 / 15 |
| IOGPUDevice / IOGPUDeviceShmem | 256 / 104 | 29 / 22 |
| IOGPUMemory / IOGPUSysMemory | 112 / 168 | 29 / 29 |
| IOGPUMemoryMap / IOGPUResource / IOGPUCommandQueue | 72 / 160 / 1408 | 29 / 33 / 36 |

编译期检查引用计数 +12、资源 map +32 / memory +40 / length +120 / client map +144、
设备 task +80、客户端 device +288、队列事件 +1176/+1240、当前 fence +1304、队列错误 +1312、
fence 起止事件 +48/+112 与状态 +200、GPU event/fence machine +320/+336。
IOGPUObject 的 +12 占用 OSObject 尾部填充，必须显式声明，防止 C++ 子类重用。
已恢复其 retain/release/init 覆盖，不能继承普通 OSObject 引用计数。

| 证据 | 当前地址 / 槽 | 用途 |
| --- | --- | --- |
| IOGPU start / stop / free | 0xfffffff005f40a78 / 0xfffffff005f41bb4 / 0xfffffff005f41d5c | 生命周期 |
| IOGPU newUserClient | 0xfffffff005f42434 | 原生设备连接 |
| 系统内存 / 资源 / 工作队列工厂 | GPU 槽 200 / 202 / 205 | 经原生包装函数与 AGX 工厂交叉确认 |
| Resource init / initialize / mapping options | 0xfffffff005f2af6c / 0xfffffff005f2ce14 / 0xfffffff005f2ce9c | 原生对象和 map 钩子 |
| 资源创建 / 返回数据 | 0xfffffff005f39fa8 | 输入 88 bytes，输出至少 80+128 bytes |
| Queue init / submit | 0xfffffff005f43ff0 / 0xfffffff005f4451c | 槽 27 初始化、Family 提交入口 |
| Queue command-buffer 钩子 | 0xfffffff005f4606c，槽 35 | 厂商编码接入点 |
| Family submit/fence 包装 | 0xfffffff005f447b4 | token、队列错误、通知 |
| Event advance / add / signal | 0xfffffff005f254d0 / 0xfffffff005f265f0 / 0xfffffff005f273b8 | 真实 channel stamp、事件和唤醒 |
| Fence retire / BlockFence notify | 0xfffffff005f29f3c / 0xfffffff005f28830 | 原生退休与 40-byte 通知 |
| DeviceUserClient start | 0xfffffff005f2474c | 用户 stamp 映射使用只读 options 0x4001001 |
| 选择器 26 | 0xfffffff005f2304c | 4 scalar，单项参数 40 bytes |
| Device retained lookups | 0xfffffff005f39114 / 0xfffffff005f39518 / 0xfffffff005f396dc | resource / shmem / queue |
| SysMemory descriptor getter | 0xfffffff005f4b5c4 | 描述符与 DMA 映射 |
| IOGPUObject init / release | 0xfffffff005f27778 / 0xfffffff005f277f8 | 原子计数和 finalize |

构建校验映像哈希、类继承/大小、私有函数字节、虚表重定位、导入，以及服务、资源、
队列、map、系统内存、任务、设备客户端共 7 个编译器虚表。未恢复的方法仍是
relocation-only nativeSlotNN，不通过这些占位声明调用原生函数。
Apple GPU 页表路径和资源组钩子不属于开放 API。

离线检查不能证明真机 ABI、寄存器、固件交接、DMA 一致性或时序正确。
本轮异步路径的字节及调用链证据保存在 `build/drivers/gpu-async-abi-inspection.json`。
后续包括实际引导交接验证、持久任务 VA 管理、非特权 ISA 隔离和通用 native work queue / 事件编码；完整 Metal 能力位继续关闭。

保留固件页表的原生 `QcomGPUMapper : IOMapper` 见 [硬件提交说明](hardware-submission.md)。资源分配仍共用设备 IOVA 窗口，执行时切换到独立作业页表；未通过实际引导交接或真机回调验证。

平台租约现在覆盖原生资源的映射、提交和销毁。实际时钟/电源配置失效时，原生作业以错误通知结束，
保留 job/ticket 及其资源；成功 halt/AXI drain 和 SMMU 销毁之后才释放平台保护。
这不改变 command/shader/JIT ABI；有有效固件交接时提供有界 CP 恢复，见硬件提交说明，电源塌陷仍未实现。

GPU 1.43.0 的 Compute Dispatch 使用三维 groups/local。共享 dispatchFootprint 在原生准入和 PM4 编码前检查
每组最多 1024 个线程、local 1024/1024/64、每轴 65535 组和总调用量 16777216。
当前 buffer 地址由编译器插入的 SSBO 字节检查约束；无守卫的旧程序保留全局 X 元素 footprint。
当前厂商 shader/JIT 版本为 24/21；Dispatch 为 792 bytes，Draw 为 2168 bytes；Apple resource/queue/submit ABI 和原生命令版本 3 保持原值。

QGPU_BOUNDED_BUFFERS 标记的编译程序按真实 SSBO 区间逐分量检查。原生 bindingBytes
验证其最小元素长度和资源 offset/bytes，SSBO 状态使用同一 byte range；无标记的旧程序仍检查
静态 invocation/vertex/instance footprint。VFD 顶点属性和私有索引快照的 footprint 校验独立保留。
声明该标记不能代替机器码验证，JIT 仍为 administrator-only。外层 Apple 资源/队列/提交 ABI 未改。

动态共享内存经同一条原生 ShaderDispatch 路径提交：200-byte 快照尾部保存 8 项 threadgroupLengths，
程序以 threadgroupMask 标识实际使用的槽位，sharedBytes 保存按 16 bytes 对齐的静态前缀。
GPUShaderProtocol.h 的 threadgroupFootprint 同时用于 Metal 编码、原生准入和 AdrenoPipeline；
它校验非零/对齐长度及合计 32 KiB 上限，并生成相对动态池的 offset/length 常量，用户不能指定共享内存地址。
PM4 在 CS 常量区的前 16 words 发布这些值，IR3 的 driver params/immediates 放在其后。
JIT description 为 1736 bytes，image 为 20288 bytes；Apple 选择器、资源/队列/提交 ABI 及原生命令版本 3 保持原值。

GPU 1.43.0 的厂商 shader 版本为 24：Texture 保存 mipLevels、swizzle、layers 和 layerStride（40 bytes），Sampler 保存七个 uint32（28 bytes，LOD 为 unsigned 8.8），Draw 合计 2168 bytes；Dispatch 为 792 bytes，JIT 为 19。
颜色采样用完整 mip 链的 layer 大小做资源和物理别名检查，附件用所选层的单级 plane。
线性多级布局按 A5xx aligned-base-pitch minification、256-byte 最小行距、末级 32-row 保留和 4 KiB layer 对齐计算；
内核重新计算而不接受任意层地址。IOSurface、Depth/Stencil 保持单级。
生成 mip 的 Compute 作业只绑定前一层和下一层的独立范围，参数快照单独保留，经原有串行完成路径逐级消费。

Compute 纹理读写已接入同一原生桥：Dispatch 末尾包含 8 个 Texture，编译结果另记录 imageMask/readMask/writeMask。
准入阶段检查活动纹理完整 mip 链、资源所属设备和 IOSurface 元数据，将实际 IOVA/byte size 放到 CS SSBO 8..15，普通 buffer 保持 0..7。
纹理几何常量占 word 16..47，线程组常量保持 word 0..15；48-word 前缀后的 driver/immediate 区独立分配。
原生桥不接受用户提供的几何常量或纹理 GPU 地址；写资源使用已有 GPU-write/cache/surface/fence 生命周期。
RGBA8/BGRA8 读写和动态 mip 访问由受范围检查的 IR3 LDGB/STGB 实现；仅查询尺寸也能获得完整前缀。
Query、read、write 的 usage 需求分别处理，Metal 每次 dispatch 保存对象和描述符。Compute 采样已接通；完整 read-write tier 与目标运行尚未验证。

Compute 采样复用 images[0..7]，Dispatch 另含 8 个 sampler 状态；sampleMask/samplerMask 表示独立活跃槽。
原生资源解析使用 imageMask|sampleMask，采样要求 ShaderRead usage，并与本次 image/buffer 写入做物理重叠检查。
A540 sampler/texture 状态进入 CS block 5，普通 SSBO 与图像 SSBO 仍用 block 15；Apple 外层 ABI 保持不变。

Sampler 现为 28 bytes，Dispatch/Draw 为 792/2168 bytes，Shader/JIT 为 24/21。
原生桥按 sampler 生成镜像标志，CS word48..55 / FS word0。Compute 使用 dispatch geometry 时前缀为80 words；不使用 geometry 的 sampler 程序保留64 words，FS 保留16 words。
显式 LOD 用 CS logical+8 / FS1 的无 clamp 状态；隐式/bias/gradient 使用原状态。
边界表位于 512-byte 对齐的私有只读 IB，随作业保留到真实完成；无外部表地址或额外用户资源。

颜色格式 5/6 现分别表示 RGBA8/BGRA8 sRGB；原生准入沿用四字节布局、任务资源、几何常量和完整 mip 范围检查。IOSurface 只放宽对应的线性/sRGB 存储解释，任务授权、R/B 存储格式、尺寸、行距和单层限制保持核对。采样与渲染目标下发 A5xx sRGB 控制位；Compute 读写由配套 shader24/JIT21 的转换 helper 处理。mip 生成绑定相邻非重叠的 source texture 与 destination image，保留物理别名拒绝和逐级原生完成 fence。没有增加 Apple 外层 selector 或改变对象布局，也未执行目标系统。

纹理视图不创建新 IOGPU resource；共享原有 backing/task/session，把选定 mip 的实际 offset、尺寸、行距和可见级数传入同一个提交后端。分配仍保留 fdl5 最后一级 padding，准入与 alias/SSBO byte-size 使用实际访问范围。单级 IOSurface 保持授权与完整几何核对，接受兼容的四字节颜色重解释。shader24/JIT21；二维颜色数组寻址已实现；不开放 GMEM blit；颜色读取 swizzle 已实现，尚未运行目标系统。

原生非整除 Compute 提交记录原始 grid/reserved，先验证 ceil 组数和实际线程数配额，再在同一私有 IB 内编码最多 8 个完整组/尾组区域。代码与资源只上传一次，每个区域使用实际 local size 和独立 geometry/driver 常量，整次 dispatch 结束后才发一次真实完成 timestamp。geometry 常量64..79来自原生准入，不接受用户 GPU 地址；flag128仅允许 Compute，完整保留此前 shared/image/sampler 常量区。shader24/JIT21，Dispatch792 bytes；尚未执行目标系统。

二维颜色数组在同一IOGPU buffer内保存逐层mip链。厂商Texture的layers非零表示2DArray，layerStride保存原始页对齐层间距；视图调整offset及可见层数而不重新分配。原生准入验证层数、实际span、shader类型位和任务资源，Compute常量80..95由内核生成。A5xx sampler下发原层间距与可见DEPTH，Render附件先选择单个slice/mip再交给现有颜色目标路径。物理别名核对按层间有间隔的区间集合扫描已固定物理页，允许未重叠的切片/mip共享buffer，继续拒绝真实反馈与采样/SSBO写入重叠；不同IOVA/资源ID不绕过检查。Shader24/JIT21，Texture44/Dispatch792/Draw2168/Sampler28 bytes；未执行目标回调。

顶点采样继续使用同一 ShaderDraw 原生提交：末尾的 vertexTextures[8]/vertexSamplers[8] 由任务资源解析为独立 VS 描述符，vertexReserved 必须为0。A5xx 的 VS block0 与 FS block4 分别上传 sampler/texture 和显式 LOD shadow，两个阶段各自保留16-word常量前缀。资源和视图随整个 job 保留，仍在真实 CP fence 后释放。VS/FS 的只读别名相互允许；两个采样阶段均与附件及 fragment 写 buffer 做已有 strided 物理页核对。Shader24/JIT21，Draw2168 bytes；Apple 外层命令版本和对象布局保持原值，目标回调未验证。

图形采样资源现按每阶段8个texture/sampler槽准入，sampleMask、samplerMask与explicitSamplerMask独立；稀疏槽和交叉配对不要求texture/sampler同号。统一graphicsTextures检查要求未引用槽为空，并逐槽核对数组标志；任务授权和真实fence生命周期沿用原实现。Draw为2168 bytes（Shader24/JIT21），所有活动采样平面进入原有分层物理区间反馈/写入别名检查。

Cube/CubeArray沿用IOGPU资源命名空间，Texture.cube标记维度，layers为数组立方体数量；物理面数为6倍元素数。Shader24/JIT21、Texture44/Dispatch792/Draw2168、description1736/image20288需成套更新。Cube类型位与cubeSamplerMask由真实NIR采样指令导出，逐槽验证维度及归一化sampler；A5xx TYPE2和DEPTH使用cube数。原有分层物理区间检查改为扫描实际面数，阻止面视图/附件与采样或写入重叠。

Cube image与query沿用Compute的imageMask/readMask/writeMask和原生IOGPU资源准入。类型位16..23可属于image或sample，cubeSamplerMask仅与sample中的Cube槽对应；查询无需sampler。80..95的Cube层信息使用物理面数（Cube为6，CubeArray为6乘cube数）和原面间距。所有Cube Compute程序保留96-word前缀。Shader24/JIT21、20288-byte JIT image和16KiB代码配额需成套更新；native注册仍堆内复制后验证，较大代码在64KiB只读job IB中通过SP对象地址取指，原有完成fence和隔离生命周期不变。

GPU1.57.0 的 Shader34/JIT31 增加2-byte half buffer stride，复用既有资源/任务和提交 ABI，结构尺寸不变。编译器按实际绑定字节长度保护每个 half 访问，A5xx 使用16-bit buffer 指令；half 计算和原生片元范围见 [Metal 说明](metal.md#原生-half-片元与内存访问)。


## 有界准入与通知消费

GPU1.69 保留原版 Submit selector、40字节参数及通知格式。预检查在 reserveTokens 和 Family 调用之前识别全局作业额度、CPU 索引读取、活跃附件快照压力及内部 stamp 换代的排空条件，仅在此处返回 IOKit vendor code0x100（0xe3ff8100）；该次没有 token/fence，允许原样重试。IOGPUNativeAdmissionBackpressureVersion=1 公布这个语义。实际厂商命令编码错误及其他 Family/硬件失败仍按原有错误路径处理，不能重试为成功。GPUNativeAdmissionDeferred 为独立的延后次数。

GPUNativeCompletion.h 的生产窗口用于 Metal0.59，消费真正的 IODataQueue；start 不退休，后续 start 可先到，end 必须匹配待完成队首并具有有效时间。全设备预算16笔，退休与重试回调独立于调用者的提交目标。四个内建 blit 布局均复制进只读 IB，CPU 参数页复用仅依赖准入返回。该实现已进入离线构建，尚无新的真机执行证据；上文 round-41 只证明旧版本的原生 CP 路径。


## 原生资源规模与计账

GPU1.70 / Metal0.60 将连接命名空间、在途作业引用和实际 DMA 映射拆开。原生连接最多1024个 buffer/IOSurface资源，单作业最多64个不同资源，全局最多2048个客户端资源对象。ticket 及 Job 使用64项引用和64位 surface-write mask，最高位也按 UInt64 处理，覆盖现有 MRT、两阶段纹理/缓冲区及深度模板的组合。shared-memory、队列和诊断连接原有上限保留，Shader36/JIT33、88/208-byte资源结构和Apple selector均不改变。

IOGPUNativeMemoryLimits 是64字节版本1数据，按真实 mapper 分配窗口计算连接字节上限 min(256 MiB, aperture/2) 和全局上限 min(384 MiB, 3×aperture/4)，均向下对齐16 KiB。单资源上限为 min(16 MiB, client limit)。当前离线 gpu-policy.json 的窗口为256 MiB，对应连接128 MiB、全局192 MiB；这些是计账上限，仍受 IOVA可用区间、guard、缓存根和内核私有作业空间约束，不代表可用RAM或保证分配量。Metal从该原生数据读取 maxBufferLength 和 recommendedMaxWorkingSetSize，拒绝缺少该版本能力的旧内核。

连接表按实际原生资源的 chargedBytes 记录 ID 与费用；创建前预检，原版 Family 创建成功后核验实际费用与归属。若实际计账不能接纳该 ID，先用原版 DeleteResource 回滚，失败则走已有的 gate 外 clientClose，避免返回未跟踪的句柄。命名空间删除只收回连接费用；全局在客户端 buffer/import 对象真正 free 时才减费，在途 ticket 对象继续保留其费用。内部 IOSurface 缓存根不计入这份客户端对象费用，其实际映射仍由独立 mapper 账本跟踪。下溢/重复释放不会回绕为大额空闲预算，会设置计账故障并拒绝新保留。

GPU mapper 的账本及映射记录扩为4096项，内核保留页表元数据扩为512页。记录地址稳定，另有按地址排序的索引；分配查找为一次有序扫描，释放只在原有 unmap/TLBSYNC 成功后移除记录，隔离和退休待确认的区域继续占位。空洞复用保留两侧guard页。没有改变16 MiB单次映射或原有私有树权限/固件保留策略。

原版 newResourceWithIOSurface 每次分配新的0x82客户端资源，0x80根在 IOSurfaceDeviceCache 中共用；依据现有 gpu-surface-abi-inspection.json 的0xfffffff005f2c434工厂及0xfffffff005f3a6f4注册路径。不同导入拥有独立 view，但可能指向同一 prepared backing。私有作业树对这样的 backing 只映射一次，避免重复 PTE 导致 Busy；物理重叠、CPU索引快照和附件反馈检查仍按实际范围执行，不因去重而放宽。

GPUMemoryStatistics 是136字节只读快照。gpuctl stats/registry/diagnose 解码客户端数、客户端资源对象/字节计账、活跃/隔离 DMA 记录、映射payload及包含guard的跨度，以及内核共享树页数。缓存根、IB池和私有辅助buffer会使映射数据与客户端对象费用不同；页表计数只描述内核共享树，未合计每作业私有树。快照不暴露地址，正常约每秒发布，故障和停止时沿用即时快照。

现有 gpu-submit 增加1024项命名空间、预算和第63位检查，现有 SMMU context检查增加1024项映射的空洞复用及记录指针稳定性；未增加测试文件。gpuctl native-async-test 现在持有64个资源，两笔各64个Fill32，逐资源核对两个范围；GPU_COMPLETION 添加resources=64，因此不能与旧版单资源异步样本直接比较。该命令未在设备执行。构建记录见 build/gpu-runtime/native-memory-offline.json；Metal发现、UI像素和动画仍待实测。


### 不可变行填充布局

GPU1.71 增加blit_fill_rows的32字节规范参数冻结；IOGPUNativeBlitLayoutVersion=2，Metal0.63同时核对该能力及实际registry。内核调用同一生产planner，复核全部8字段、binding offset/长度和grid，随后复制到作业只读IB，CPU上传页仍可在准入返回后复用。原四个blit布局保持不变，transfer/compute统计沿用已有分类。新入口用于Render load-clear的有效像素行，不写行间padding；目标执行仍待验证。


round-51 最新真机对照：GPU1.81 / Metal0.66 默认关闭手动供电恢复，可信CP重申64位地址模式后原生CP回读通过，首个Compute仍发生SMMU转换故障。CP导出的HLSQ快照与round49逐字节一致，SP aperture仍不可用。尚无UI合成GPU帧或流畅动画验证；详见[硬件调试记录](hardware-bringup.md)与 `build/gpu-runtime/round-51-evidence.json`。

### Mercury 渲染修复进度（GPU145–146，2026-10-19）

当前任务已授权实现和真机调试。GPU145 为原版 Mercury 增加现有 provider/secure-mode
角色加上述固定 cdhash 的准入；设备打开和 JIT 注册仍使用同一个判断，不授予 compositor
角色。插件记录设备和队列失败阶段。保留壁纸任务已安装的 shared-cache/PosterBoard 补丁。

Round169 用户仍看到纯黑。本轮原生日志首次明确：Mercury 已进入自研插件，
`initWithAcceleratorPort:` 返回具体能力检查错误，发生在 `IOServiceOpen` 之前；因此这一轮
尚未验证新增准入判断。不能继续把当前实际故障归因为驱动拒绝，也不能认为角色补齐已修复背景。
错误时间为本轮启动后的 04:33:50，旧轮次日志另行排除；初次正向日志查询命中历史201条上限，
随后使用同一启动的反向错误查询取得此证据。

GPU146/Metal1.8 将插件初始化能力协商改为原生 type1 连接上新增 `0x1007`，一次返回120字节
快照，包含协议/JIT/registry版本、动态ready状态、内存限制和可选功能。由同一个任务身份判断
鉴权，在现有 native gate 内生成快照。所有客户端使用同一路径，不依赖进程可见的自定义
IORegistry属性，也没有旧驱动回退。保留原有ABI/资源上限检查，错误显示具体实际值。
尚未证明169的属性检查失败究竟由哪些属性或可见性规则引起；新接口的实际结果待170验证。

本轮没有重复GPU像素/fence全套：GPU145–146没有改变ISA、资源所有权、提交及读写fence。
GPU145角色正反向和逐字节身份不匹配测试、GPU145/146交叉构建及实际嵌入检查已通过。
169 runner返回1是移除可选kprintf spam后未采到期望CPU标记；不能记为四核验证通过，
也不能据此认定CPU失效。三个控制命令均0，无运行panic。原生GPU完成262、失败0全部属合成进程，
不能证明Mercury绘制。直接command扫描的截图返回unavailable，用户实看结果仍是判据。

新证据：`build/gpu-runtime/ui-poster-native-access/hardware-evidence.json`、
`round-169-errors/report.json`、`ui-metal-capabilities/embedded-verification.json`。

### 原生 Fluidity 管线补齐（GPU147 / Metal1.9，真机待验）

Round170 已验证 GPU146 的原生能力协商：Mercury 通过设备和队列初始化，进入
`FluidityWallpaperRenderer.Error.setupFailed`。驱动错误报告为默认库没有目标函数，
SDF 管线收到空 vertex/fragment。该轮无运行 panic，243 笔完成均来自合成进程，
不能当作 Mercury 已执行 GPU 作业。用户此前确认纯黑的是 round169。

核对原版 Metal 后，`newDefaultLibrary` 应加载 `NSBundle.mainBundle/default.metallib`；
自研实现此前总返回内部 AOT 诊断库。Metal1.9 改用原生路径，内部合成和 metalctl
诊断库改成明确加载各自资源，不向应用返回替代库。

实际 Mercury 库的四个 Fluidity 函数已提取并通过生产 AIR C API 与 A540 后端：
三个普通入口及主 fragment 的八种 Bool 常量组合全部可编译。补齐 packed i8/i16
加载与符号扩展、标量局部变量、sincos 输出指针、ushort 顶点索引和只读常量表，
主机和设备 lowering 保持一致。常量表展开为查找函数，避免生成后端不支持的隐式 UBO。

原版 Mercury 初始化函数 `0x10000874c` 还明确设置 BGRA8（80）和 RG32Float（105）
双附件；SDF 写附件1，主 fragment 读取附件1。GPU147 使用固定 Mesa 的
R32G32_FLOAT 配置（A5xx RB/TFMT `0x67`），补齐 8 字节像素布局、浮点双字清屏、
MRT 写入和 framebuffer 输入。清屏参数沿用内核不可变冻结和范围校验，新程序
`blit_fill_pairs` 不改变旧的32位填充程序。协议 Shader39，JIT36。

本候选136个 AOT 条目，registry `cc6368962a55a5db`。原生诊断同时检查非归一化
浮点值、RG32 附件1写入、fetch 后 BGRA8 像素和缓冲区前后及行间 padding。
构建与实测证据写入 `build/gpu-runtime/ui-rg32-render/`；本段编写时尚未上机，
不声明背景已修复。原生读取只读附件仍走既有快照路径；后续消除复制必须由
实际 backing 重叠检查证明与所有写目标不别名，不能只比较资源 ID 或 GPU 地址。

GPU147 候选离线验收已完成：gpu-submit ASan/UBSan、生产编译器完整检查和
EFI 实际嵌入读回均通过。重启 round170 后设备停在固件 `Battery prfile`，Fastboot
未枚举，round171 尚未启动；已请求手动恢复。候选修复不能记作真机背景修复。

### RG32 与真实 Fluidity 管线验收；memoryless 后续（round172–174）

Round172 已在 A540 真机通过 RG32 浮点清屏、MRT slot1 写入、slot1 framebuffer
fetch：每阶段35像素及前后/行间 guard 全部正确。直接加载原版 Mercury 的22函数库，
三个无常量函数和主 fragment 八组 Bool 专用化、九条双附件管线全部通过。原生背景
进程仍反复退出，因此 shader 可编译不是背景出图证据。

CARenderer 前四帧各4096像素正确；opacity=0帧只有清屏，不增加 render-kind 计数，
旧诊断据此退出，八帧未全部验收。诊断现同时计算 render 和 compute shader 工作，
并继续要求像素正确、GPU完成增长、失败计数不变；没有改变生产绘制路径。

进一步取得完整原版初始化/绘制反编译：`0x10000a1e0` 为 RG32 设置 storage=3，
`0x10000d404` / `0x10000d4b0` 对附件1设置 Clear / DontCare，SDF 与主 fragment
在同一 encoder 内使用它。插件此前拒绝 memoryless。Apple 文档及随附SDK规定
memoryless 内容只在 pass 内有效，不能Load/Store或blit。

Metal1.10 实现兼容路径：公开纹理只有布局，无 native backing；每个 encoder 创建
独立 private GPU 存储并由命令缓冲保留至实际完成。拒绝跨 pass Load/Store、普通
采样、CPU/blit访问。A540 当前仍用线性系统内存，不能据此宣称已实现 GMEM 或节省
tile-memory 带宽。新测试用同一逻辑纹理提前编码两份不同Clear值的pass，检验
浮点blend/fetch和并行在途的隔离。GPU147及Display122未改。

证据：`build/gpu-runtime/ui-fluidity-runtime/mercury-init-full.c`、
`mercury-render-full.c`、`round-172/uart.txt`、
`ui-memoryless-pass/apple-memoryless-contract.json`。Round174开始验证，尚无背景验收。

### 原生 TIFF 格式阻塞（round175，GPU148 / Metal1.11 修复候选）

用户确认 round174 背景仍纯黑；RG32/memoryless 测试通过不能当作背景已恢复。
Round175 直接通过原版 MetalKit、原始应用路径和 Private 存储选项加载 `maps.tif`，
确实失败：`Failed to allocate texture memory`。同一进程的内核错误报告明确为
format110 / 256×2048 / usage1，Shared与Private各一次。原始 TIFF 标签为
4×16bit、SampleFormat1（无符号整数）；`filmic_lut.tif` 同样是4×16bit unsigned。
这是 RGBA16Unorm，不能当成此前已支持的 RGBA16Float。

GPU148/Metal1.11 新增仅采样 RGBA16Unorm，沿用8Bpp布局与上传，使用固定 Mesa
TFMT5_16_16_16_16_UNORM=`0x60`；不扩大该格式的 render/write 能力。Shader协议40，
JIT36，136条AOT指令及registry `cc6368962a55a5db`不变。新增主机编码检查区分
UNORM16(0x60)与FLOAT16(0x62)，实际资源测试同时加载maps和filmic_lut，将纹理中心
四个像素经 GPU 采样到BGRA8，再与GPU原始16位blit读回的归一化期望比较。

另：round175 Apple CARenderer八帧全部4096像素正确。旧诊断依赖约每秒发布的
IORegistry统计，现使用既有 `0x1003` gated live snapshot，检查真实GPU shader完成
计数，不强制退休、提交或修改计数。包含compute清屏；生产渲染路径不变。

证据：`ui-fluidity-assets/hardware-evidence.json`、`TIFF-layouts.json`、
`round-175/uart.txt`、`ui-rgba16-unorm/`。候选正构建/验证，未宣称原生背景出图。

Round176 实测补齐：maps（256×2048）与filmic_lut（4096×64）均由原版MetalKit成功
加载为format110/Private；各四个中心像素的GPU归一化采样与原始16位blit读回一致。
用户仍见纯黑；后续原生样本1468完成、0失败，其中合成1462，另六笔恰为资源探针，
不能算作Mercury绘制。Mercury仍退出重建，缺少当前CPU崩溃原因。

现有Metal错误报告异步发送，遇到Swift对nil结果直接trap可能丢失。Metal1.12将已去重
的错误元数据通过同一鉴权selector同步送达（不获取作业transport mutex，不提交GPU，
不修改原NSError），并在未实现协议/SPI selector异常前记录具体类和selector。保留原
异常语义。日志探针增加按本次启动定位的查询，避免逆序扫描中设备时钟回退导致旧启动
记录混入。GPU148、原版Mercury、显示和系统选择策略均未改；round177正采集。

### Round177 结果与 Mercury 场景初始化离线复核（2026-10-20）

Round177 的7003笔完成作业全部为 compositor。Mercury 仍反复退出，两份主线程
stackshot 分别位于第一个 function 编译和 fragment 专用化；没有得到退出原因。
Metal1.13 已离线删除编译时丢弃的 NIR/ISA 文本生成，11个真实 Fluidity variant
完整程序图像与父版本相同，尚未验证设备效果。

当前候选 `ui-mercury-init-audit` 继承该生产版本，补充有界原生退出事件及资源采样，
并令 Fluidity 探针使用 Mercury 原始写掩码和混合参数。已交叉构建并核验 HFS /
EFI 嵌入，设备充电期间没有上机。原版 PosterKit 在 Mercury 同步初始化返回后才
设置 `pui_didFinishInitialization`；宿主还要求 `contentState==2` 才认为内容就绪。
详细服务依赖、证据边界及下一步见 [离线复核](hello-mercury-lifecycle-review.md)。
