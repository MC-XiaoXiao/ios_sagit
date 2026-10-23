# Metal 着色器、Render/Compute 与线性纹理

Metal1.48.0 对经实际 NIR 和插值声明确认不依赖 MSAA 的片元变体复用编译结果，
保持完整 JIT 格式和采样相关独立编译路径。主机对照、原版 iOS18 分层依据及限制见
[冷编译优化记录](ios18-cold-compile-optimization.md)；未部署或进行真机测试。

Metal1.31.0 离线补齐 iOS18 QuartzCore 的运行时 any/all、exp/log/atan2/powr/三参数极值及常量纹素偏移采样。
8个原版入口新增通过普通/MSAA A540 编译，已有138个AOT程序的代码与元数据保持一致；未进行真机测试。
范围、性能处理和剩余缺口见 [iOS18显示着色器离线记录](ios18-display-shaders-offline.md)。

round80 使用 GPU1.92.0 / Metal0.81.0 / Display1.0.12，背光接管禁用，Shader37/JIT34。
线段、受限渲染区域及原版纹理标志语义修正后，原生合成器持续提交，后续采样累计准入2973笔。
实际扫描已出现状态栏和欢迎文字，但截图存在缺笔与矩形覆盖；用户视频还显示旧语言残笔叠在 hello 上、
内容区反复亮暗切换。**UI正确性和流畅度未通过，完成计数不能代替像素验证。**
round81/82 仅更新 metalctl 诊断信息，驱动保持 round80 版本。2048行纹理拷贝读回为0进一步
定位为512KiB的GPU填充源本身全0；CPU写入同一缓冲区后，CP行拷贝8192/8192字节正确。
16KiB填充/普通拷贝、双队列fence、混合/裁剪、资源生命周期、heap和受限清屏检查通过。
大缓冲区填充失败与闪烁的因果关系尚未证明，继续检查尺寸描述和Compute边界。
round82后续用户观察文字继续切换且无重影，但未注意该次启动最初是否闪烁。
同启动稳定现场的16次采样有12次有效、11次像素hash变化；约40秒内合成准入维持493，
没有新拒绝。截图可见欢迎笔画、底部提示和信息按钮，无先前明显残字叠加；可能复用既有表面，
不能由持续显示推断持续GPU渲染。证据见round-82-stable/evidence.json。
只读client buffer的GPU blit、compute、纹理采样读回及拒绝写入在round79已通过；负向写入检查
现位于blit套件末尾。像素拷贝、清屏及原生显示同步继续排查，见build/gpu-runtime/round-80-evidence.json、
round-80-video/evidence.json、round-81-evidence.json和round-82-evidence.json。
round83进一步测得65,532字节全部正确、65,536字节完全未写、65,540字节全部正确，且范围外保护字节未变。
随后GPU页表健康检查读到TCR=0x802098（预期0x802018），即时故障快照已回到预期值；
FSR=0x400、FAR=0，之后提交报0xe00002d7。131,072字节探测因此未完成。
用户观察Setup闪烁，疑似SpringBoard或其他组件重启后恢复正常；最近8份崩溃头没有匹配本次启动，进程重启尚未证实。
用户随后明确当前可能熄屏，因此静态扫描/固定计数不能判为卡死或渲染恢复，软件唤醒返回也不是实际亮屏证明。
记录见round-83-live/evidence.json；这两项故障尚不能独立证明全部闪烁的原因。
GPU1.93.0 / Metal0.82.0（Shader38/JIT35）待上机：缓冲区长度由内核准入的精确字节数写入
原生常量区，AOT和JIT的SSBO边界检查共用该长度；整64KiB硬件描述改用非零低维，
额外描述字节不扩大着色器访问范围或页表映射。TCR仅在EPD0短暂置位时最多重读8次，
仍要求恢复精确预期值；持续EPD0及任何其他变化保持失败。主机提交/编译检查和交叉构建通过，
round84真机512KiB及65532/65536/65540/131072字节读回全部正确，保护区错误0；整个blit、fence和render-state通过。
4CPU验证通过，无panic，原生合成准入从107增长到365再到701、拒绝0，页表TCR故障未重现。
但用户仍见闪烁/叠字，新视频VID_20261018_215040.mp4明确显示欢迎文字区域短暂变黑/变亮矩形，
状态栏和底部提示同时正常；完整UI尚未修好。原版CARenderer在1080×1920 IOSurface基础两层合成
2073600像素正确。继续以同一表面的透明度、移动和局部更新检查复现；证据round-84/evidence.json、
round-84-ca/report.json、round-84-video/evidence.json。
round85的64×64连续5帧透明度/移动/局部更新均正确；全屏CPU字节核对超时，不能算通过。
round86仅改用CPU复制后扫描，实际copy日志及用户观察确认Setup仍闪烁；直接扫描不是充分解释。
round87原版CARenderer在64×64和256×256同一表面连续8帧均逐像素通过，含透明CGImage和局部损伤恢复。
Metal0.83待上机修正：原版QuartzCore在commitAndWaitUntilSubmitted返回YES时立即执行待回收资源闭包，
NO时将同一闭包接入addScheduledHandler:。旧实现空标记准入就返回YES，且第一条真实操作就公开Scheduled，
可能在后续弱引用操作/依赖仍等待时提前回收资源。现各条操作只记录私有准入，完整操作列表全部准入后
才公开Scheduled并允许helper返回YES；不阻塞共享提交目标或伪造GPU完成。字节级审计见
build/gpu-runtime/ui-submission-lifetime/apple-submit-audit.json，闪烁因果关系仍待实测。
round71通过四参数线性纹理SPI的同启动崩溃回归，22笔原生系统合成器GPU作业完成。
round73通过CARenderer/Metal/IOSurface到显示MMU新IOVA的直接扫描及参考图案像素核对。
这些较早轮次未形成完整Welcome与流畅动画的证明；round80的持续提交也仍伴随像素错误。

`QcomMTLDriver.bundle` 实现公共 Metal 协议的 AOT / 运行时 JIT 加速路径，
连接 `QcomGPU.kext` 的原生 IOGPU 资源和异步准入队列。
着色器由真正的 Mesa GLSL/NIR/IR3 编译器生成 A540 机器码；内核建立绘制、
计算及纹理状态，通过 CP 执行并等待硬件 fence。
**当前覆盖受限的 MSL / AIR 编译子集，尚非完整 Metal 实现。系统 UI 的 GPU 加速及动画流畅度尚未通过；设备未就绪时工厂返回 NSError。**

## 共享分配与 UI 接口审计

`newBufferWithBytesNoCopy:length:options:deallocator:` 现复用原版 IOGPU 的 client-buffer
工厂。支持默认 CPU cache 的 Shared 整页分配，内容指针保持原地址；创建失败
不接管释放回调。成功后由资源和视图保留原分配，原生 GPU 完成与 cache 同步沿用现有
通路，释放回调在 transport lock 外执行。外部所有者的内存不进入 Metal purgeable 状态。
GPU1.91接纳原版VM检查标记为只读的输入：准入拒绝写入，作业页表保留只读AP；round79通过硬件读回和拒绝写入检查。
Private、WriteCombined及子分配暂不支持。round70原版QuartzCore已从该buffer调用线性纹理SPI，round71修复后通过合成作业回归。
依据 [Apple 的原分配复用语义](https://developer-rno.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Mem-Obj/Mem-Obj.html)，
原生参数审计与内核边界见 [IOGPU 桥接](native-iogpu.md#原生-api)。

IOSurface进口复用backing；Display1.0.11另行实现并在round73验证了受限布局的显示专属DMA映射和直接扫描。
这与Metal buffer接口是两项独立验证。UI残余项包括Welcome动画的实际激活、持续提交，
以及欢迎效果运行时所需的shader/格式。round76实际请求了A8字形纹理和256×1的RGBA16Float采样纹理，
Metal0.79/GPU1.90增加这两种原生硬件采样格式。A8的RGB为零、alpha取存储通道；RGBA16Float每像素8字节。
它们仍不支持Render附件、MSAA或原始SSBO图像读写，不能据此声明模糊效果已通过；完整Metal family保持关闭。
原版欢迎动画属于 SpringBoard 的 SBDashBoardSetupView/SBCursiveTextView，
资源与停帧证据见 [当前硬件记录](hardware-bringup.md#当前离线进度与欢迎页停帧)。

Metal0.77补齐原版QuartzCore实际调用的
`newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:`。只接纳受限2D线性
buffer视图，校验offset、行/图像跨度和原分配长度，保留原buffer资源及所有权；不上传或复制像素。
round70同启动的9份backboardd缺失选择器异常是明确的提交阻断；round71同启动未匹配
到新的backboardd/SpringBoard崩溃，并有22笔合成作业完成。这不代表所有私有SPI完整。

使用本地iPhoneOS18.6 SDK的libclang协议清单和实际适配类进行静态核对，诊断为0。
732条SDK实例声明与489条适配实现声明属于不同集合，不能算完成百分比；原生
`_MTLDevice`继承方法也不能直接列为缺失。原始清单和分类结果保存在
`build/gpu-runtime/ui-display-context/{sdk-protocol-inventory,implementation-selector-inventory,sdk-selector-difference,interface-audit}.json`。

仍缺少的常用接口包括共享纹理handle、compute dispatch-type创建入口、Render晚设置的
store action、depth bias/clip、Render/Compute资源与heap使用声明、间接draw/dispatch、
parallel render encoder及residency set。同步仍有CPU事件等待和显示fence/可恢复DMA停止
的限制；FP16颜色纹理和完整AIR/MSL ABI未完成。高级family、mesh/tile/tessellation、
raytracing、sparse和counter等能力保持关闭。接口清单用于继续实现，不将未经运行时观察
的缺口直接当作当前停帧原因，也不以返回成功的空实现掩盖失败。

## AIR 辅助函数与原生合成特化

Metal0.73 将原版 AIR 内部辅助函数接入共享编译前端。此前 `fixed_frag_lph_cpf` 的 darken/lighten 混合会因尚未内联的函数调用而失败，颜色矩阵分支还会保留辅助函数参数中的空纹理调用。现在先按实际函数常量裁掉无效路径，再逐轮内联所选入口仍然调用的模块内函数；每轮进行常量传播、局部内存提升与死代码清理。这样颜色 opcode 分派中的未选效果和未使用资源能在继续展开前消失。没有按 QuartzCore 函数名替换着色器。

函数常量依赖与 `is_function_constant_defined` 求值覆盖可达辅助函数。原版 `MTLFunctionConstantValues`、`MTLFunctionConstant` 反射对象、`MTLFunctionDescriptor` 和 Render/Compute 管线仍使用原来的系统接口；辅助函数内的常量现在也进入必需值检查、有效值快照及特化缓存键。构建生成新的归档上下文，旧编译器生成的私有缓存不会被误用。主机多入口选择也只展开所选函数，避免未请求入口中的不支持代码阻塞编译。

内联使用目标 libLLVM 已导出的 C API 与 `always-inline` pass。内部函数的 inlining 属性作为优化提示处理；没有实现 A540 设备函数调用 ABI。限制为可达图256个函数/65536条指令、每轮复制前估算16384条指令、最多16轮；最终沿用4096条入口 IR 和原有 JIT/ISA 校验。递归、间接调用、无法内联和超预算会失败，不能转成空绘制或伪造完成。实现依据 [LLVM 的内联 pass](https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-18.1.8/llvm/lib/Transforms/IPO/AlwaysInliner.cpp)，对外沿用 [Apple 函数特化接口](https://developer.apple.com/documentation/metal/using-function-specialization-to-build-pipeline-variants?language=objc)。

离线使用未改写的 QuartzCore AIR，明确给出选定常量，编译了一个顶点与六个片元变体：顶点颜色、纹理采样、source-over、darken、lighten、3×3颜色矩阵。六个片元均保留 color0 输出，采样保留 texture3，矩阵保留 buffer4，三种附件混合保留 framebuffer0 读取；普通与 MSAA A540 机器码均通过生产 JIT profile。两条前端有六组 GLSL 完全相同；矩阵仅有等价 float32 常量拼写差异，完整后端镜像逐字节一致。变体输入与镜像见 `build/gpu-runtime/ui-air-composition/runtime-after.json`。

这些常量是离线选择的覆盖案例，并非 round61 捕获的完整运行时参数。最初缺少 destination index 的预备输入会禁用所有输出，不计为渲染成功证据。现有编译器检查只增加一个嵌套辅助函数案例及必要的缺值/递归检查，未增加测试文件或独立脚本。动态链接、完整颜色程序、较高资源槽位、管线绑定反射及目标 LLVM/Metal 实际执行仍需继续；本轮未连接、启动或写入设备。

## 编译和调用路径

```text
Shaders.metal / Shaders.air.ll / 外部 .air 或 .metallib / ShaderBuild.json
  -> scripts/toolchain.py build-gpu-shaders
  -> MSL 语法前端，或 MTLB 解包 + LLVM AIR SSA/元数据前端 -> GLSL 430
  -> Mesa GLSL -> NIR -> IR3 (GPU 540) -> A540 机器码和寄存器分配
  -> GPUPrograms.h（同时编入 Kext / 用户态）+ default.qmetallib（登记索引）

QcomMTLCreateDevice / QcomMTLCreateSystemDefaultDevice
  -> MTLLibrary / MTLFunction / RenderPipelineState / ComputePipelineState
  -> RenderCommandEncoder / ComputeCommandEncoder / BlitCommandEncoder
  -> 原生 IOGPUResource / IOGPUCommandQueue / Family fence
  -> 内核快照描述符、检查登记表指纹、资源归属和范围、保留资源
  -> Adreno::PipelineEncoder / 私有 PM4、shader code、常量和 scratch
  -> A540 CP_EXEC_CS / CP_DRAW_INDX_OFFSET -> 真实 CP 完成
```

工厂使用 Apple `IOServiceMatching`、IORegistry 和 IOKit.framework，检查原生命令
版本 3、完成协议版本 1、着色器命令版本 34、JIT 版本 31、相同的登记表指纹、私有 GPU 地址空间和硬件就绪属性。
Apple 原生选择器、资源布局和外层 SubmitArgs 沿用已有 iOS 18 ABI 契约。
Kext 已声明 `MetalPluginName=QcomMTLDriver` 和 `MetalPluginClassName=QcomMTLDevice`，
bundle 主类实际继承系统 `_MTLDevice` 并实现 `initWithAcceleratorPort:`。
系统 `MTLCreateSystemDefaultDevice` 加载路径的代码、元数据和离线安装目录已接入；
round-43 已通过原版系统工厂发现 Adreno540；这证明设备加载，着色器执行和像素路径仍有独立故障。

## 系统插件入口

对实际 iOS 18.7.10 / 22H374 arm64 的 Metal.framework、AGXMetalA10 和 IOGPU 库做了只读提取和静态核对。
源 DMG、提取工具、图像和入口机器码哈希见 [插件 ABI 记录](../../drivers/abi-22H374/metal-plugin-abi.json)。
Metal.framework 匹配 `IOAcceleratorES`，按 `MetalPluginName` 拼接
`/System/Library/Extensions/<name>.bundle`，再选择指定类或
[NSPrincipalClass](https://developer.apple.com/documentation/BundleResources/Information-Property-List/NSPrincipalClass)。
加载器验证主类继承 `_MTLDevice`，用 32-bit `io_service_t` 调用
`[[deviceClass alloc] initWithAcceleratorPort:service]`；返回 nil 时不登记设备。
框架随后检查 `MTLDevice` / `MTLDeviceSPI` 并调用 limits、feature queries 和 workaround 初始化。22H374 的抽象 `_MTLDevice` 没有 baseProtocols 表；本设备明确采用公共 `MTLDevice`，通过 ABI 核对后用 `class_addProtocol` 采用框架实际注册的 `MTLDeviceSPI`。不创建同名私有协议，也不修改 Apple 类。
现有 AGXPrincipalDevice 在该系统中拒绝直接加载，因此本实现使用自己的设备类和真实框架基类。

`NativeMetal.h` 只声明已核对的方法和 264-byte limits 返回结构，不复制 Apple 私有实例变量。
ObjC 非脆弱 ABI 自动调整子类存储；limit getters 经 `-limits` 读取本实现的独立结构。
设备初始化验证 iOS 18.6/18.7、真实基类的 SPI 协议、服务参数类型和 limits 的大小/对齐。
框架队列、library builder、默认 loader 和 feature queries 由原生 superclass 初始化。
未识别的 feature profile 使用已核对的空 family 分支，`supportsFamily/FeatureSet` 保持 NO。
私有表面功能仍受限，继承框架基类不代表已经实现其全部 SPI。

传入端口由 Metal descriptor 持有；设备另行 `IOObjectRetain`，在销毁时先关闭原生连接再释放自己的引用。
显式 C 工厂复用同一初始化，shader registry、私有页表及硬件就绪检查仍适用。
原生连接允许管理员或带实际 `com.apple.backboardd=true` entitlement 的系统合成服务；程序注册/删除在每次调用时重新核实管理员或签名 backboardd 角色。普通应用访问、完整 CoreAnimation 和 Metal family 尚未完成。CAMetalLayer/drawable 的当前桥接范围见末节。

`make gpu-metal` 同时生成 `build/output/gpu-rootfs` 离线目录，包含系统扩展目录中的完整 bundle，
以及 `usr/local/bin/metalctl`。bundle、编译 dylib 和工具带 ad-hoc CodeDirectory；
复用现有 `bridge_trustcache.py` 汇总为 `build/output/gpu.trustcache`，清单记录每个文件的 SHA-256 / CDHash。
这只是离线暂存与工程信任缓存，尚未写入目标 APFS、接入实际启动信任链或改变系统权限。
`metalctl --system info` 直接调用系统 `MTLCreateSystemDefaultDevice()`；工具不再链接本 bundle。
原有显式模式仅在使用时通过 dlopen 优先加载旁边的 bundle，缺失时使用系统目录；AIR 文件也使用同一查找路径。
现有桥接的 `--trust-cache build/output/gpu.trustcache` 参数可携带这个模块；
五种离线链接已核对生成模块的哈希和 3 项 CDHash，首个静态模块使用空的离线 fixture，未验证真实固件信任链。
所有目标检查入口仍只交叉编译、不执行。

## 着色器

入口源文件配置在 `drivers/gpu/QcomMTL/ShaderBuild.json`。
增加或修改其中的 `.metal`、`.air.ll`、`.air` 或 `.metallib` 文件后，重新构建 Kext 和 bundle，使两端登记表一致。
编译过程按参数、属性和表达式降低代码，不按函数名称挑选硬编码机器码。
四个默认入口 `add`、`position`、`color` 和 `textured` 分别覆盖计算、顶点位置、
常量颜色和纹理采样。另有 `air_add/air_position/air_color/air_textured` 四个
独立 AIR 入口，通过位码读取路径编译。另有 MSL/AIR 各一个 `instanced_position`
顶点入口，分别以 vertex_id 和 instance_id 读取位置/偏移 buffer。
另有 MSL/AIR 各三个 varying 入口：位置/颜色/UV 输出、颜色片元和 UV 采样片元，加上 MSL/AIR 各一对 attribute 输入/颜色输出入口，最初合计二十个入口；加上控制流、工作组坐标和 buffer_indexing 示例，当前共二十六个入口。

当前 MSL 子集：

- kernel 返回 void，fragment 返回 float4；vertex 返回 float4 或含 float4 position 的接口 struct。
- Compute/Vertex 的 buffer(0..7) 支持 int/uint/float 标量及 2..4 分量向量指针，顶点输入只读。
  数组下标可由局部变量、循环、常量参数、顶点/实例或三维坐标组合；每次实际访问都有字节范围检查。
  支持 constant 标量/向量引用和只读结构体字段；结构体成员为 32-bit 标量/向量。
- Compute 支持运行时指定均匀工作组、1D/2D/3D 网格。每轴最多 65535 个组；
  local 各轴上限 1024/1024/64，每组总线程最多 1024，每次最多 16777216 次调用。
  内核检查绑定的真实字节区间，编译器从该区间的 SSBO descriptor 取得长度并检查实际下标。
- Fragment 可以使用 position、含 float32 varying 的 stage_in struct 和 texture(0)/sampler(0)，执行规范化二维 sample。
- 支持基本算术、构造、min/max/clamp/mix/abs/dot/normalize，以及 if/else、for、while、do/while、
  整数 switch、break/continue、提前返回、局部变量的词法作用域和 bool 条件。
  每个图形 return 都在对应路径写出 position/varying/color；缺少输出的返回路径会拒绝。
  自定义函数、浮点/64-bit 原子、普通线程局部数组、pointer escape 和完整 C++ 类型规则仍未支持。
- 编译器拒绝 private memory、额外 UBO、常量数据段、多纹理、
  超出初始指令/常量预算的程序。浮点行为采用当前 Mesa IR3 的 fast math 路径。

`newDefaultLibrary` 返回嵌入 AOT 库。`newLibraryWithSource` 对已登记源文本走 AOT，
其他源文本在设备进程内 JIT 编译，支持同步及 completionHandler 异步入口。
`newLibraryWithData/URL` 对已登记文件保留 SHA-256 快速加载路径；其他 AIR / metallib
进入运行时编译，不需要为了新着色器重编 Kext。默认 fast math 选项可用；
函数常量、宏、其他数学模式、动态链接库和 invariance 暂不支持。
`default.qmetallib` 仍是本项目登记索引，不能把任意 JSON 当作已编译机器码导入。
Linux 主机构建不需要 Apple 的 Metal 编译器。

编译器固定为 [Mesa 24.3.4](https://gitlab.freedesktop.org/mesa/mesa/-/tree/769e51468b49b2a42f0a0eaf71cf9eed5ff4e5de)，
源仓库、commit 和 sparse checkout 列表在 `tools/gpu-compiler/Source.json`。
主机工具使用 venv、Meson/Ninja 构建。MSL AOT 现在通过 `MSLMain.cpp` 调用与设备 JIT
相同的 `MSL.cpp` 前端，统一工具只负责构建和 JSON 传输，已移除重复的 Python 语法转换。
统一工具对旧独立编译器补充 Compute 上下文、
IR3 NIR options、零 base dispatch、显式 SSBO 绑定和 fragment position 降低，
并使用正常的 IR3 shader variant 编译/汇编路径导出真实寄存器、常量和系统输入分配。
现代 glibc 的 C11 线程兼容修复也集中在同一入口。未把 Mesa 的 Linux DRM 后端接入 iOS。
目标编译库和适配器显式限制为 ARMv8-A + CRC + Crypto，随 bundle 汇总实际编译依赖的 Mesa 许可声明。
A540 使用 Freedreno A5xx 路径；[Turnip 的目标是 A6xx 及后续 GPU](https://docs.mesa3d.org/drivers/freedreno.html)。
寄存器描述由固定版本的 rules-ng-ng 生成并保留许可证，内核只使用整数编码。

## AIR / metallib 编译

`tools/gpu-compiler/AIR.cpp` 使用主机 LLVM 23 的 IRReader、bitcode reader、类型系统、
SSA 指令和 verifier 解析 AIR。它独立实现 AIR 元数据/指令降低，未接入 LLVM 的 Apple GPU
代码生成器，也没有把 LGPL 的 metal2vulkan 实现链接到驱动。
运行时由独立的 `AIRRuntime.cpp` 调用 iOS `/usr/lib/libLLVM.dylib` 的导出 C 接口读取
IR、位码、类型、元数据并执行 verifier；不依赖私有 LLVM C++ 类布局。
构建时逐一检查这些 C 符号存在于 iOS SDK 的 libLLVM.tbd。
目标系统 LLVM 是否能读取某一新版位码，仍由实际系统版本决定；读取失败返回编译诊断。

统一入口 `make gpu-shaders` 自动编译主机前端，并按 `ShaderBuild.json` 导入：

```json
{
  "sources": [
    "Shaders.metal",
    "Shaders.air.ll",
    {"path": "Library.metallib", "entries": ["my_kernel", "my_fragment"]}
  ],
  "gpu": 540,
  "local_size": "variable",
  "profile": "aot-linear-buffers-linear-2d"
}
```

`.air` 可以是原始 LLVM bitcode，也可以带 Darwin/Apple 位码包装头；
`.ll` 必须是带 AIR64 triple、数据布局和 `air.version`/入口元数据的 AIR IR。
`.air.ll` 由 llvm-as 汇编为实际位码后再读入，生成的 `Shaders.air` 随 bundle 一起发布。
示例 IR 为本项目自写，未冒充 Apple 编译器输出。
`.metallib` 支持 MTLB v2：验证文件大小、section/group/tag 边界、NAME/TYPE/OFFT/MDSZ/HASH、
位码 SHA-256 和容器 TYPE 与 AIR stage 一致性。可通过 entries 选择支持的入口；
未指定则编译全部入口，出现不支持的函数会明确失败。Fat 容器、加密/原生 GPU 二进制库不支持。

前端从 `air.kernel/vertex/fragment` 和参数元数据恢复 stage、buffer binding、读写属性、
位置、顶点/实例 ID、全局线程 ID、texture(0) 和 sampler(0)。支持 float32/int32 标量与 2..4
分量向量、算术/位运算、比较/select、转换/bitcast、向量 shuffle/插入/提取、
float32 位置/varying 多成员输出 struct、片元 varying 输入、常用 AIR 数学 intrinsic 和二维纹理采样。
采样支持默认隐式 LOD、float32 bias 或显式 float32 level，规范化坐标、零 offset/min-LOD，拒绝稀疏驻留结果访问。

入口支持至多 128 个基本块、4096 条静态指令，包含条件/无条件 br、switch、多返回点、
标量/向量/聚合 phi 和循环回边；辅助函数须先内联。仅支持线程内 int32 标量 alloca，原子操作限下述 relaxed int32 子集，动态调用仍拒绝。
buffer 采用受检查的 word 视图，支持 float32/int32/uint32 及 2..4 分量向量。
GEP 保留 64-bit 相对字节地址的高低两个字，接受 int32 下标及其 zext/sext 地址扩展；
一般 int64 数值运算、pointer/address PHI、subword 和不对齐访问仍拒绝。
复合类型按 LLVM DataLayout 的固定字段偏移访问，pointee 元数据大小上限 4096 bytes；
不会把任意 GPU 虚拟地址当成用户参数。

运行时 MSL 前端为 `MSL.cpp`，AIR 前端为 `AIRRuntime.cpp`；二者生成受限 GLSL，
通过 `QcomGPUCompiler.dylib` 内存接口进入真正的 Mesa GLSL → NIR → IR3。
编译过程不启动子进程，不创建临时着色器文件，也不申请可执行 CPU 内存或使用 MAP_JIT。
运行时 AIR 覆盖上述计算、顶点、颜色和纹理示例，以及控制流、phi、常用算术、整数比较、select、
转换、向量/聚合操作和数学 intrinsic；浮点比较、AIR convert intrinsic、自定义函数调用暂时拒绝。
MTLB v2 在用户态检查分区、标签、函数名、位码哈希和 stage，再编译其中的 AIR；
运行时容器只接收 1..64 个可支持的 stage 函数，external 函数拒绝。

每台设备有带数量上限的弱引用内存缓存，按输入内容和格式区分编译结果；
同一设备的同步/异步建库串行处理，Mesa 全局状态另有进程内锁。
缓存不强引用设备/库形成环，也不写磁盘。
动态库及函数持有程序对象，pipeline/已记录命令继续保留函数；最后一个引用消失时
删除原生连接中的程序。多入口建库中途失败会回滚此前登记的程序。
完整 AIR ABI、Apple 原生 GPU 二进制和完整 Metal family 仍未实现。

### 控制流编译

MSL AOT/JIT 共用的语句解析器保留块、循环及 for-init 的变量作用域，并在每个 return 处发布图形输出。
`AIRControlFlow.h` 则由主机 LLVM C++ 前端和运行时 LLVM C 前端共用。
无环 CFG 按 postdominator 合流生成结构化分支；循环及较大交叉图保留每次调用的 block 状态，
生成真正的循环/分支，经 Mesa NIR/IR3 编成 A540 指令。
依据 [LLVM phi 语义](https://llvm.org/docs/LangRef.html#phi-instruction)，每条被选中的边先保存全部 incoming 值，
再写入目标 phi，处理循环变量交换，避免逐项赋值覆盖旧值。
没有人为截断源循环次数；运行超时仍交给已有的提交 watchdog 和故障恢复路径。

循环和偏移索引通过同一组 NIR 字节检查；pointer/address phi 仍不支持。
旧版没有 QGPU_BOUNDED_BUFFERS 的程序仍使用按 invocation/vertex/instance 计算的静态 footprint。
代码量、寄存器、分支栈、纹理和常量仍受现有 A540 profile 限制。

运行时读取器同时识别旧 LLVMBr=2 和 LLVM 23 拆分后的分支 opcode。
旧版 switch 使用公开 operand API；观察到新版表示时，才解析公开的 LLVMGetSwitchCaseValue 导出，
不读 LLVM 私有对象布局，也不把新接口作为旧 iOS libLLVM 的强制链接依赖。
接口参考 [旧版 C 枚举](https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-18.1.8/llvm/include/llvm-c/Core.h)
和 [公开 terminator API](https://llvm.org/doxygen/group__LLVMCCoreValueInstructionTerminator.html)。

现有示例库包含对应的 `control_flow` / `air_control_flow` kernel，使用动态循环、交换变量、switch 合流和提前返回。
两者已实际编成 A540 机器码；另在强制 llvmpipe CPU 模式执行生产前端生成的 GLSL，
核对 5 种路径、320 次调用的结果。这只验证编译转换的语言语义，未执行 A540 ISA 或 iOS 驱动。

格式参考固定于 [MetalLibraryArchive](https://github.com/YuAo/MetalLibraryArchive/blob/de573ba4a7b986bbecb3e4ce2464945d266a28e9/Sources/MetalLibraryArchive/Archive.swift)、
[MetalShaderTools 的 Apple AIR 样本](https://github.com/zhuowei/MetalShaderTools/tree/820e4301bc85c472bf5c44ebf2c8660fec0d0f70/sampleshader)
和 [metal2vulkan AIR ABI 描述](https://github.com/steelbrain/metal2vulkan/tree/720ef1ce9ae2a2068999a8675717418b9416283e/crates/metal2vulkan-air/src/abi)。
参考链接和 LLVM 主版本记录在 `tools/gpu-compiler/Source.json`。

### 32-bit 整数原子操作

Compute/Fragment 的 writable device atomic_uint / atomic_int buffer，以及 Compute 的静态 threadgroup
atomic_uint / atomic_int 标量或数组，支持 relaxed load/store、exchange、fetch add/sub/and/or/xor/min/max、
weak/strong compare-exchange。指针只能指向已声明的资源或元素；CAS expected 是匹配的线程内 int/uint 标量。
失败时 expected 得到观察到的值，成功时保留其值。weak 使用 strong CAS，不制造额外失败。

AIR 接入 air.atomic.global/local.* 的 int32 intrinsic（检查 order、scope 和布尔 volatile 参数），
以及 LLVM int32 atomicrmw/cmpxchg 和 relaxed atomic load/store。entry block 中单元素的 int32 alloca
降为线程内 GLSL 标量，用于 expected 回写；不分配 GPU private stack，不允许指针逃逸或 pointer PHI。
AIR 参数 ABI 参考 [intrinsic 定义](https://github.com/vertex-language/air/blob/3d277d16858a/intrinsic.go)
及 [对应的 xcrun 核对](https://github.com/vertex-language/air/blob/3d277d16858a/intrinsic_test.go)。

原子 load 降为 atomicOr(0)，store 降为 atomicExchange 并丢弃返回值；因此 load 也保守地登记 write mask，
当前要求 writable 绑定。raw uint 视图上的有符号 min/max 使用 CAS 重试，保留精确的 int32 位值和返回旧值。
A540 后端使用 Mesa 已有的全局/共享整数 atomic 指令，见固定版本的
[ir3_a4xx.c](https://github.com/chaotic-cx/mesa-mirror/blob/769e51468b49b2a42f0a0eaf71cf9eed5ff4e5de/src/freedreno/ir3/ir3_a4xx.c)
及 ir3_compiler_nir.c。Fragment 原子同时参与 SSBO 8–15 槽重映射和 no_earlyz 副作用处理。

生产 NIR 将 RMW/CAS 放进实际字节区间、四字节对齐及共享 base 回绕检查的有效分支。
无效访问不执行原子指令，返回零；不会转向地址零执行 RMW。该行为只约束受信编译器产物，管理员 ISA 注册策略不变。
Shader/JIT 协议为 24/21；本轮动态参数扩展厂商 Dispatch 与 program description，Apple 外层 ABI 保持原值。
atomic_counter / air_atomic_counter_kernel 示例覆盖跨线程竞争及屏障，atomic_values / air_atomic_values
覆盖有符号值、CAS expected 和成功标志。浮点/64-bit 原子、更强内存序和完整的地址/指针模型仍未实现。
本阶段未执行 iOS 或 A540 指令。

### Threadgroup 共享内存与屏障

Compute 编译支持函数内 threadgroup 标量/向量及固定长度数组，AIR 支持未初始化的 addrspace(3)
全局变量和 32-bit 标量/向量访问；每个工作组最多 32 KiB。threadgroup_barrier 接收常量
mem_flags::mem_none、mem_device、mem_threadgroup 及其按位组合。AIR 对应 air.wg.barrier(i32 flags, i32 1)。
屏障必须由同一工作组的所有线程一致到达；前端不证明任意控制流的组内一致性。

共享变量通过 NIR explicit shared layout 降为字节偏移，IR3 生成真正的 LDL/STL、FENCE/BAR。
NIR 在共享内存的已分配总字节范围检查每个分量，非法读取为零，非法写入跳过；同时防止 base+offset 回绕。
大下标、负数及高 32-bit 非零的相对地址访问先归到 32 KiB 偏移，再交给 NIR 检查；
加上任一已分配变量的 base 仍不会产生 uint32 回绕。该检查限制在编译器为当前程序分配的 shared 区域，不把一个数组内部的越界访问定义成合法行为。
底层模型参考固定版本 Mesa 的 [A540 容量](https://github.com/chaotic-cx/mesa-mirror/blob/769e51468b49b2a42f0a0eaf71cf9eed5ff4e5de/src/freedreno/common/freedreno_devices.py)
及 [fd5_compute.c](https://github.com/chaotic-cx/mesa-mirror/blob/769e51468b49b2a42f0a0eaf71cf9eed5ff4e5de/src/gallium/drivers/freedreno/a5xx/fd5_compute.c)：
A5xx 使用固定 HLSQ_CS_CNTL_1=1，未套用 A6xx 的 SHARED_SIZE 寄存器。
AIR 屏障参数参考 [实际 xcrun 签名对照](https://github.com/vertex-language/air/blob/3d277d16858a/intrinsic.go)。

编译器把实际 v->shared_size 写入 program 的 sharedBytes，并用 flags=32 标记实际工作组屏障。
sharedBytes 记录静态区域（有动态参数时包含末端的 16-byte 对齐填充），另以 threadgroupMask 标识活动的动态槽位；JIT description/image 为 1736/20288 bytes；
Shader/JIT 协议为 24/21，Dispatch/Draw 为 792/2168 bytes。内核检查容量、四字节对齐及 compute 阶段，
Metal 的 maxThreadgroupMemoryLength 和 pipeline.staticThreadgroupMemoryLength 返回容量及实际静态用量。
threadgroup_reduce / air_threadgroup_reduce 示例分配 256 bytes 并跨线程归约。
浮点/64-bit 原子、SIMD 屏障和完整语言内存模型尚未实现，未做真机验证。

### 动态 Threadgroup 参数

Compute 支持 `threadgroup T *name [[threadgroup(index)]]` 及标量引用；T 为 32-bit 标量、2..4 分量向量或 atomic_int/atomic_uint。
AIR 使用 `air.buffer`、`air.address_space = 3` 与 `air.location_index`；该命名空间独立于 device/constant buffer。
目前开放 0..7 共 8 个槽位，读写类型及原子内存序沿用上述子集；只读动态参数和指针逃逸尚未实现。

[setThreadgroupMemoryLength:atIndex:](https://developer.apple.com/documentation/metal/mtlcomputecommandencoder/setthreadgroupmemorylength(_:index:))
记录每个槽位的字节数。长度须是 16 的倍数，零清除绑定；每个实际使用的参数在 dispatch 时须有非零长度。
每次 dispatch 快照独立长度，后续重绑不会修改已编码操作；已优化掉的参数不占容量，也不要求绑定。
`staticThreadgroupMemoryLength` 返回静态前缀，有动态参数时包括末端的 16-byte 对齐填充。
内核按活动槽位顺序打包，静态与动态区域合计最多 32768 bytes；拒绝漏绑、溢出及非活动槽位的伪造长度。

共享的编译阶段将静态对象放在前面，动态 raw pool 的基址按 16 bytes 对齐，并把最终 NIR shared allocation 限为 32 KiB。
IR3 预留前 16 个常量 word，按槽位传入相对动态池的 byte offset/length；driver params 和 immediates 分配在其后。
这些常量由原生提交桥计算，用户不传入 GPU 地址。每个动态访问先检查自己的参数长度、四字节对齐及 64-bit 地址高位，
再进入共享内存 NIR 检查；越界读为零，越界写和原子操作不执行，不会落入相邻动态参数或静态区。
A5xx 仍采用固定 `HLSQ_CS_CNTL_1 = 1`，不套用 A6xx 的 SHARED_SIZE 寄存器。

`dynamic_threadgroup` 示例覆盖 3 个稀疏槽位、静态前缀、float3 padding、compound store、原子计数与跨线程归约；
`air_dynamic_threadgroup` 覆盖 64-bit GEP、跨线程相邻值和动态原子，并故意保留一个未使用的参数以检查活动槽位反射。
当前仍限制每个程序最多 4096 bytes 指令；复杂 AIR 程序可能因指令预算被拒绝，不会放宽成未校验的上传。
厂商 Shader/JIT 为 24/21，Dispatch/Draw 为 792/2168 bytes，description/image 为 1736/20288 bytes；旧厂商描述符会被拒绝。
本轮只有编译、常量/PM4 检查及 CPU 图形实现的语言语义核对，没有执行 iOS 或 A540 指令。

### Fragment buffer 到 A540 的桥接

Render encoder 支持 setFragmentBuffer、setFragmentBufferOffset、setFragmentBuffers 和
[setFragmentBytes](https://developer.apple.com/documentation/metal/mtlrendercommandencoder/setfragmentbytes(_:length:index:))。
Fragment 和 Vertex 各有独立的 8 个逻辑 buffer 槽；inline 数据上限均为 4096 bytes，调用时复制，
每次 draw 保存当时的 binding 并持有资源，直到真实硬件 fence 完成。
MSL 和 AIR 的 Fragment 参数可读取标量/向量、只读常量结构体，并向 device buffer 写入 32-bit 标量/向量。

A5xx 的图形阶段共用 SB4_SSBO=14；Compute 使用 15。参考固定版本 Mesa 的
[容量声明](https://github.com/chaotic-cx/mesa-mirror/blob/769e51468b49b2a42f0a0eaf71cf9eed5ff4e5de/src/gallium/drivers/freedreno/freedreno_screen.c)
和 [emit_ssbos](https://github.com/chaotic-cx/mesa-mirror/blob/769e51468b49b2a42f0a0eaf71cf9eed5ff4e5de/src/gallium/drivers/freedreno/a5xx/fd5_emit.c)，
本后端将 Vertex 放入物理槽 0–7，Fragment 放入 8–15。编译器同时重映射 load/store/RESINFO 的 binding，
反射保留 Metal 逻辑索引；原生后端一次发出完整 16 项的 byte-size/base 表，并分别设置 VS/FS SSBO_ENABLE。
Fragment program 的 flags 包含 bounded=8、bank=16；有写入时保留 no_earlyz=2，并按 Mesa 模型配置深度阶段。
共享的生产 NIR 变换按实际绑定字节数检查每个访问，越界读取为零、越界写入丢弃。

Shader command 版本为 24，Draw 为 2168 bytes；JIT 版本为 21，description/image 为 1736/20288 bytes。
该阶段的原生连接资源上限为32个、连接预算64 MiB；当前GPU1.70已改为独立的1024项命名空间、64项作业引用和按实际IOVA窗口计算的预算，见[原生资源规模与计账](native-iogpu.md#原生资源规模与计账)。单资源仍至多16 MiB。
内核校验 Fragment binding 的归属、范围和 write mask，并将其纳入与渲染附件/采样纹理的物理页别名检查。
示例 buffer_color / air_buffer_color 同时覆盖只读 palette、32-byte 常量结构体和稀疏槽 7 的写入。
这些能力仍需真机验证；尚未实现完整 Metal family、浮点/64-bit Fragment atomics、Fragment 纹理写入或 depth 输出。

### Mip 纹理、LOD 与生成

RGBA8Unorm/BGRA8Unorm 的 2D 纹理支持完整或截短的 mip 链，最多 15 级，级数也不能超过最大维度对应的完整链。
Shared/Private 均支持；每个资源仍受 16 MiB 上限约束，Depth/Stencil 与 IOSurface 保持单级。
多级颜色布局参考固定 Mesa 的 [fdl5_layout](https://github.com/chaotic-cx/mesa-mirror/blob/769e51468b49b2a42f0a0eaf71cf9eed5ff4e5de/src/freedreno/fdl/fd5_layout.c)
和 [fdl_pitch](https://github.com/chaotic-cx/mesa-mirror/blob/769e51468b49b2a42f0a0eaf71cf9eed5ff4e5de/src/freedreno/fdl/freedreno_layout.h)：
base pitch 按 256 bytes 对齐，后续层 minify 对齐后的 base pitch（至少为 1），再按 256 bytes 对齐。
最后一级保留至少 32-row 对齐空间，整个 layer 按 4096 bytes 对齐；单级纹理保留原 64-byte 行距规则。
CPU、blit、渲染附件与原生准入共用这些偏移和尺寸计算，内核检查采样整条链的范围与物理页别名。

replaceRegion/getBytes、buffer↔texture 与 texture↔texture 复制接受具体 mip level；颜色 render attachment 的 level 也会快照到正确子区域，
viewport/scissor 和附件尺寸按选定层计算。整面/范围复制沿用 SDK 的维度匹配与 levelCount 规则，跨层复制不按基级尺寸寻址。
sampler 支持 none/nearest/linear mip 过滤和用于隐式/bias/gradient 采样的 min/max LOD，带 clamp 的 Fragment sampler setter 会快照当前值。
A5xx MIPLVLS、PITCHALIGN、layer pitch 和 sampler 的 unsigned 8.8 LOD 按 [fd5_texture.c](https://github.com/chaotic-cx/mesa-mirror/blob/769e51468b49b2a42f0a0eaf71cf9eed5ff4e5de/src/gallium/drivers/freedreno/a5xx/fd5_texture.c) 下发；
none 模式将 clamp 限制为 0.125，以保留基级 min/mag 区分。

MSL sample 支持默认隐式 LOD、`level(float)` 和 `bias(float)`；AIR AOT 与运行时 C ABI 前端支持对应的 float32 level/bias。
仍限 Fragment texture(0)/sampler(0)、规范化坐标、零 offset/min-LOD，不支持 gradient/sparse；Compute 采样见下文。
`mip_textured` / `air_mip_textured` 示例使用随像素变化的显式 LOD。

`generateMipmapsForTexture:` 复用 AOT `mipmap_rgba8` Compute 程序，逐级绑定互不重叠的 source texture0 与 destination image7，并快照 sampler0。
A540 在像素中心作 clamp-to-edge bilinear 过滤，sRGB 在过滤前解码为线性 RGB，再由受限 image helper 编码写回；alpha 保持线性。
UNorm 与 sRGB 共用该程序，支持非二次幂、单行/单列及截短链。
每一级通过现有原生 dispatch 提交，串行队列在真实 CP fence 完成后才消费上一层结果，不使用 CPU 生成像素。
这是当前实现的过滤规则，尚未验证与 Apple GPU 的生成结果逐像素一致。

厂商 shader 版本为 24，Texture/Sampler/Draw 为 44/28/2168 bytes，Dispatch 为 792 bytes。
JIT 版本为 21，description/image 为 1736/20288 bytes，Apple 外层 ABI 不变，旧 shader 描述符会拒绝。
离线检查核对布局、实际 PM4 和 CPU 图形实现上的生产着色器语义；尚未执行 iOS Metal 方法或 A540 指令。

### Compute 纹理到原生提交桥

Compute encoder 支持 `setTexture:atIndex:` 和 `setTextures:withRange:` 的 0..7 槽，与 8 个普通 buffer 槽独立。
支持线性 2D RGBA8Unorm/BGRA8Unorm 的单级及 mip 链，Shared、Private 和已有单级 IOSurface 导入均复用原生 backing。
MSL `texture2d<float, access::read/write/read_write>` 支持 `read(uint2[, level])`、`write(float4,uint2[, level])`、
`get_width([level])`、`get_height([level])` 和 `get_num_mip_levels()`；level 可动态变化。
AIR AOT/JIT 支持已核对签名的 `air.read_texture_2d.v4f32`、`air.write_texture_2d.v4f32` 与宽高查询；不猜测其他 intrinsic ABI。

每次 dispatch 快照纹理描述符并持有对象。编译器记录实际活跃 `imageMask/imageReadMask/imageWriteMask`，
仅尺寸查询不要求读写 usage；明确指定 usage 时检查实际所需 ShaderRead/ShaderWrite，Unknown 保留为未指定。
内核重新计算整条 mip 链的范围，在当前 IOGPUDevice 命名空间查找资源，执行已有任务归属、IOSurface 格式/尺寸校验和 DMA 生命周期管理。
写入资源沿用真实完成后的 cache 同步、surface 修改标记及 Family fence；同一纹理读写通过一个 read_write binding 使用。

A540 后端把纹理映射到 CS SSBO 8..15（普通 buffer 为 0..7），下发 state block 15 的完整 16 项描述符表和 SSBO_ENABLE。
这些是访问线性存储的真实 IR3 `LDGB/STGB` 指令。前 16 个常量 word 保留线程组相对 offset/length，
word 16..47 按槽存放 `(width,height,basePitch,format|mipLevels<<8)`，全部由原生准入计算；IR3 driver params/immediates 从 word 48 之后分配。
两类参数可在同一程序使用，仅查询尺寸的程序也保留完整常量前缀。没有纹理时保留原来的 16-word 或零前缀。

着色器先检查 mip 和坐标，再计算同一 fdl5 线性布局偏移；最终 NIR 还按实际 SSBO 字节数检查访问。
越界读取为零，写入丢弃；RGBA/BGRA 按通道解包/重排，写入 clamp 到 0..1 并量化为 UNorm8，NaN 置零。
`threadgroup_barrier(mem_flags::mem_texture)` 连接 buffer memory barrier 和工作组执行屏障；跨工作组同步仍依赖派发边界。
`image_transform` / `air_image_transform` 示例通过 texture(0) 读取、texture(7) 写入，以 constant buffer 选择 mip。

Shader/JIT 为 24/21，Dispatch/Draw 为 792/2168 bytes，description/image 为 1736/20288 bytes。
Compute sampler 已接入；整数/其他纹理格式及完整 Metal read-write tier 尚未实现，`readWriteTextureSupport` 保持 TierNone，family 能力保持关闭。
本轮复用现有编译和提交检查；CPU llvmpipe 核对生产 GLSL 的 mip、格式转换、越界和 read_write 语义，没有执行 iOS Metal 或 A540 ISA。

### Compute 硬件采样与独立 sampler

Compute 的 `texture2d<float>` / `access::sample` 接到 A540 的真实 CS 纹理单元。
纹理和 sampler 各有独立的 0..7 槽；同一纹理可使用多个 sampler，同一 sampler 也可用于多个纹理。
Metal encoder 实现单个/批量 sampler setter 及逐项 LOD clamp，每次 dispatch 保存当时的状态并持有对象。
MSL `sample(sampler,float2)` 在 Compute 使用 level 0，`sample(...,level(float))` 支持动态显式 LOD。
AIR 使用已核对的九参数 `air.sample_texture_2d.v4f32`；隐式模式只接受零 bias 并按 Compute level 0 下发。

GL 中间表示先用组合 handle 表示一对纹理/sampler，绑定名携带各自索引。
GL opaque-uniform 降低后，共享 NIR 阶段在移除 deref 前恢复配对，再拆成独立的 texture/sampler 索引，
AOT 与设备端 JIT 使用同一实现；当前 GL 前端每个程序最多 32 个活跃组合，仍受 16 KiB 指令预算限制。sampleMask/samplerMask 只记录活跃资源，不把 sampler 索引当作纹理索引。
`sample_texture` / `air_sample_texture` 示例使用 texture(5)、sampler(2)、写纹理(7)；真实 IR3 为 `s#2, t#5`。

原生桥校验完整采样链并保留 backing，A540 下发 state block 5 的纹理和 sampler 描述符、CS TEX_COUNT 及 HLSQ 更新。
已有 Fragment 的格式、pitch、mip、nearest/linear 和固定点 LOD 编码由两个阶段共用；CS 与 FS 的计数分别恢复。
采样纹理与本次派发的 image/buffer 写入范围按实际物理 backing 检查，拒绝反馈重叠；同一资源的多个只读采样配对可以共用。
图像 SSBO 读写与查询几何常量保持上一节路径，采样本身使用硬件纹理单元，不由 CPU 或 SSBO 模拟过滤。

Shader/JIT 为 24/21，Dispatch/Draw 为 792/2168 bytes，description/image 为 1736/20288 bytes。
本轮使用现有编译/提交检查，CPU 图形实现核对过滤、LOD clamp、NPOT 重采样、格式及独立配对语义；未运行 iOS/A540。
MSL gradient2d、sampler 地址模式和各向异性已补齐到下节范围；AIR gradient ABI、Compute bias 和完整 Metal family 尚未实现。

### Sampler 地址模式、梯度与 LOD 语义

Sampler 描述符扩为 28 bytes，保存过滤、8.8 LOD、S/T/R 地址模式、maxAnisotropy、borderColor 和 normalizedCoordinates。
现有 Compute/Fragment setter 及 clamp 覆盖接口逐次快照全部状态。支持 edge、mirror-clamp-to-edge、repeat、mirror-repeat、
clamp-to-zero 和 clamp-to-border-color；边界色为透明黑、不透明黑、不透明白。当前仅有带 alpha 的 RGBA8/BGRA8 2D 纹理。
A5xx 只有一个 sampler 边界色，同一 sampler 的 S/T 若同时要求 ClampToZero 与非透明 ClampToBorderColor 会明确拒绝。
像素坐标要求 edge、无 mip 过滤、各向异性为 1；硬件下发 UNNORM_COORDS。Depth compare 与自定义边界色仍未实现。

各向异性接受 1..16，按固定 Mesa fd5 规则选不超过请求值的二次幂档位，并在 linear min/mag 处使用 ANISO filter。
边界模式下，后端把三个预定义颜色编码成 A5xx 128-byte entry 的 fp32/fp16/unorm/snorm/packed 表示，
整张表按 512 bytes 对齐放入作业私有、GPU 只读 IB；sampler 指向对应条目，沿用 IB 的真实 fence 生命周期。

MirrorClampToEdge 在 A5xx 原生模式下对 NPOT 有限制，本实现统一通过生产着色器的 abs 坐标和梯度方向反射处理，硬件使用 edge。
镜像夹取情况下，片元隐式/bias 采样先取得原始坐标的 dFdx/dFdy，再生成 gradient sample，保留跨零点处的 LOD 与各向异性 footprint；其他地址模式继续使用硬件隐式/bias 路径。
独立 GL 前端设置 scalarize_ddx，匹配 IR3 对导数的标量要求。MSL 的 Compute/Fragment `gradient2d(dx,dy)` 已接入 AOT/JIT，生成真实 SAMGQ。
AIR 现有 level/bias 使用同一 sampler/坐标模型；尚未猜测或添加没有签名证据的 AIR gradient intrinsic。

依据 iOS 18.6 SDK 的 MTLSampler.h，显式 `level(...)` 忽略 sampler/encoder LOD clamp。
编译结果新增 explicitSamplerMask，CS 为逻辑 sampler 的 +8 槽、FS 为槽 1 下发无 clamp 的状态；纹理索引保持原值。
普通/梯度/隐式 bias 使用原始 clamped 状态，同一 shader 可混合两类采样。NotMipmapped 仍固定基级。

镜像标志由原生桥按 sampler 生成：CS word 48..55，含采样时保留 64-word 前缀；FS word 0，含采样时保留 16-word 前缀。
没有 sampler 时，image-only 的 48-word、动态 threadgroup-only 的 16-word 与零前缀路径保持原值。
IR3 driver/immediate 区均排在实际前缀之后。Shader/JIT 为 24/21，Sampler/Dispatch/Draw 为 28/792/2168 bytes，description/image 为 1736/20288 bytes。
现有编译与提交检查核对了 shadow sampler、常量分区、边界表、AF/UNNORM 位和拒绝路径；没有执行 A540/iOS。

### 常量参数与受检查的 buffer 访问

Compute 的 [setBytes](https://developer.apple.com/documentation/metal/mtlcomputecommandencoder/setbytes(_:length:index:))
、Vertex 的 setVertexBytes 及 Fragment 的 setFragmentBytes 接收至多 4096 bytes，调用时复制到自己的 Shared buffer，覆盖原绑定。
每次 dispatch/draw 记录时保留当时绑定的对象，随后重绑或复用源内存不会改变已记录命令的参数。
长度为零解除绑定；分配失败通过现有 command-buffer 错误路径报告。

常量标量/向量通过 constant T& 读取；只读结构体成员遵循 Metal 的 4/8/16-byte 对齐，
float3/uint3/int3 成员占 16 bytes，后续字段不会误用 std430 的 vec3 尾部空隙。
AIR 根据真实 DataLayout 计算字段地址。MSL buffer 指针只能下标访问，不允许 pointer escape。

MSL 下标先限制在不会使元素步长乘法回绕的区间；AIR 的 GEP 用两个 uint32 保留 64-bit 地址，
越出可绑定范围的高字地址映射到无效 word 下标，不能截断后落到 buffer 开头。
AOT 与 JIT 共用 Interface.h 的 NIR 转换：从 A5xx SSBO resinfo 读取真实长度，按 32-bit 分量检查。
读取使用有界的回退地址并将无效分量置零；越界 store 不执行，部分向量逐分量处理。
原子 SSBO 操作不在此转换内，明确拒绝。现有主机编译检查直接执行该 NIR 转换与优化，
核对部分向量、越界 store 和偏移加法回绕，未执行 A540 ISA。

QGPU_BOUNDED_BUFFERS 是受信编译器声明；内核确保每个绑定至少含一元素（最少 4 bytes）、
其 offset/bytes 落在当前连接的资源内，并把同一范围交给实际 SSBO 状态。
这不是 ISA 沙箱：动态机器码登记仍要求 administrator，未放开普通应用提交权限。
subword/不对齐类型、结构体整体赋值及完整 C++/AIR 内存模型仍未实现。

### Compute 工作组和系统输入

MSL 与 AIR AOT/JIT 使用同一套 Compute 系统值映射，支持 uint/uint2/uint3 的
thread_position_in_grid、thread_position_in_threadgroup、threadgroup_position_in_grid、
threads_per_threadgroup、threadgroups_per_grid、threads_per_grid，以及标量 thread_index_in_threadgroup。
AIR 不再把三维全局坐标的 Y/Z 分量替换为零；i1 and/or/xor 也可用于合并分支条件。

前端生成 [可变工作组 GLSL](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_compute_variable_group_size.txt)，
Mesa IR3 从 driver constants 读取实际 local size。Metal 的
[dispatchThreadgroups](https://developer.apple.com/documentation/metal/mtlcomputecommandencoder/dispatchthreadgroups(_:threadsperthreadgroup:))
把每轴 group/local 尺寸写入厂商 Dispatch；原生 IOGPU 和 PM4 编码器共用尺寸/调用总量检查。
后端设置三轴 NDRANGE、实际维数、IR3 driver parameters 和 CP_EXEC_CS 的三轴 group count，
不改 Apple 外层提交结构。QGPU_VARIABLE_LOCAL_SIZE 标志由编译器导出，跨 stage 或未知标志拒绝；
旧固定 64-thread 程序不能用动态尺寸执行。

MTLCompileOptions.maxTotalThreadsPerThreadgroup 的 1..1024 限制保留到 library/function/pipeline，
编码时检查；源码缓存中的通用程序不随某次调用的选项改变，包装对象共享已有的登记程序。
设备 limits 和 Compute pipeline getter 与该上限一致。未实现非均匀尾组、
SIMD barrier/浮点与 64-bit 原子、间接 dispatch 或完整指针模型；dispatchThreads 各轴不整除时明确返回错误。

workgroup_coordinates / air_workgroup_coordinates 示例检查三维坐标、local index 和实际网格/组大小。
离线检查覆盖真实 A540 编译、NDRANGE/driver constants/CP_EXEC_CS 包及尺寸边界。
CPU 图形实现不支持可变组扩展时，只对工作组大小作显式固定特化来核对 GLSL 语言语义；
本轮固定特化核对覆盖 13 次 dispatch、5007 次调用和 3238 个输出元素，结果全部匹配。
这不验证 A540 指令执行或运行时可变尺寸；记录见 `build/drivers/gpu-compute-semantics.json`。

## Render、Compute 和纹理

| 接口 | 当前实现 |
| --- | --- |
| Compute pipeline / encoder | AOT/JIT kernel、8 个 buffer 及独立的 8 个动态 threadgroup 绑定、运行时 local size、三维均匀工作组；dispatchThreads 各轴需整除 local size |
| Render pipeline / encoder | AOT / JIT VS/FS、至多一个 RGBA8/BGRA8 color attachment、D32/S8 附件、triangle/triangle strip、UInt16/UInt32 索引、实例绘制、混合/颜色写掩码、viewport/scissor、剔除/绕序 |
| Render pass | Load/Clear/DontCare 和 Store/DontCare；Clear 用 GPU CP 填充颜色、深度和模板平面 |
| Draw 后端 | Sysmem/BYPASS，shader 上传、常量分区、SSBO、VFD/VPC 链接、光栅和 MRT 状态；绘制后 CCU flush timestamp + 等待 |
| Compute 后端 | CS 寄存器、系统输入、常量/SSBO 状态、NDRANGE 和 CP_EXEC_CS |
| Texture | 单采样 2D/颜色2DArray；RGBA8Unorm/BGRA8Unorm 及其 sRGB 格式支持至多 15 个 mip，Depth32Float/Stencil8/Depth32Float_Stencil8 保持单级，1..16384×1..16384（受单资源容量约束），64-byte 对齐行距 |
| Shared / Private texture | 原生 buffer backing；Shared 支持 replaceRegion/getBytes，Private 通过 GPU blit 上传和读回 |
| Sampler | 规范化/受限像素坐标、nearest/linear、六种地址模式与预定义边界色、1..16 各向异性、mip 过滤与 LOD clamp；depth compare 未实现 |
| Texture blit | buffer↔texture、texture↔texture；检查格式、范围、设备和重叠，按行或连续区域生成 CP copy |
| Buffer / Blit | 原有 Shared/Private buffer、Fill32、Copy32；单资源最多 16 MiB，按连接原有配额管理 |
| Queue / completion | 原生异步准入、Family 真实 end token、FIFO 和硬件错误传播；用户态按操作串行等待，每个 command buffer 最多 256 操作 |

当前没有 MSAA、压缩格式及深度数组视图。Compute 纹理读写/采样支持 RGBA8/BGRA8 的 UNorm 与 sRGB，见下文。
部分区域复制将行距和行数记录为一个操作，执行时逐行生成受限 CP Copy32；1080 行不会单独占用 1080 个操作。
不支持 CAMetalLayer drawable、heap、argument buffer、无保留引用命令、
fence/event 编码、计数器、mesh/tile shader 或 residency set。
`supportsFamily/FeatureSet` 和完整 `GPU::Metal` 能力位继续关闭。
GPU 时间尚未校准，时间查询返回 0。

原生 submit 返回成功后才将 command buffer 标记为 Scheduled 并调用 scheduled handlers。
每个操作等待 Family 的对应 end token；该 token 在 A540 timestamp、cache 与 IOSurface 修改记账
完成后才发出。整个 command buffer 的操作均结束后才触发 completed handlers；原生错误或等待超时
转为 Metal 错误。用户态仍使用串行 worker 和设备 transport lock，逐操作等待，不宣称多个
Metal command buffer 并行执行；通用 IOGPUWorkQueue、shared event 与 Mach 通知端口尚未实现。
原生准入和完成的具体布局、计数换代与引用规则见 [原生队列桥接](native-iogpu.md#资源与提交路径)。

### IOSurface 纹理

bundle 实现公共 `newTextureWithDescriptor:iosurface:plane:`，经原生 IOGPU 类型 `0x82`
导入当前任务的 IOSurface。内核实际验证 backing、格式、行距和偏移，再交给相同 A540 提交后端。
导入过程不新建像素存储、不上传副本；重复导入同一 surface 共用原生 device cache / DMA 映射。
`texture.iosurface` 返回持有的原始对象，`iosurfacePlane=0`，`isShareable=YES`。

当前支持非平面线性 RGBA8Unorm/BGRA8Unorm、plane 0、Shared 或 Private，
单级/单层/单采样、默认 CPU cache mode 和现有 ShaderRead / RenderTarget usage。
描述符尺寸/格式必须和表面一致；真实行距可以大于 width×4，但需 64-byte 对齐。
同一表面可用于 render target、fragment sampling、buffer↔texture / texture↔texture blit。
GPU 在真实完成及缓存同步后更新表面修改 seed；成功通知不会早于数据可见。

Shared 的 `getBytes` / `replaceRegion` 使用 IOSurface 原有 CPU 地址和匹配的只读/写入 Lock/Unlock，
CPU 不重复加内核 backing offset。每个 device transport lock 覆盖自身 GPU 提交与 CPU 纹理访问，
应用仍需按 Metal/IOSurface 规则同步其他设备、任务或直接 CPU 访问。
Private 的 CPU 访问仍不可用。受保护、多平面视频、压缩/tiled 表面和
CAMetalLayer drawable 没有实现；不宣称系统 Metal 插件已经注册。

`metalctl surface-test` 扩展原有 pipeline 人工诊断入口，创建非零 offset、额外行 padding 的表面，
编译覆盖 render/blit/sample、重复导入的 CPU 读回和修改 seed 检查。本轮只交叉编译该入口，没有执行。
接口语义参考 [Apple MTLDevice IOSurface 纹理方法](https://developer.apple.com/documentation/metal/mtldevice/maketexture%28descriptor%3Aiosurface%3Aplane%3A%29?language=objc)
及本地 iPhoneOS 18.6 SDK 的 IOSurfaceRef.h；原生工厂/任务查找使用固定 iOS 18 内核字节审计。

### 深度与模板

支持 `newDepthStencilStateWithDescriptor`、`setDepthStencilState` 和前后面独立模板参考值。
状态对象复制描述符；每个 draw 再快照状态和参考值。深度支持八种比较函数和独立写入开关，
模板支持八种比较、Keep/Zero/Replace/IncrementClamp/DecrementClamp/Invert/IncrementWrap/DecrementWrap，
以及独立的 stencil-fail、depth-fail、pass 操作与 8-bit 读写掩码。nil 状态恢复默认深度 Always/不写入和模板 no-op。
viewport 的 znear/zfar 可在 [0,1] 内独立设置，允许反向深度区间。

Depth32Float、Stencil8 和 Depth32Float_Stencil8 仅开放 Private/RenderTarget；不支持深度采样。
组合纹理使用同一原生 buffer 中不重叠的 D32 与 S8 平面，两者行距各自按 64 bytes 对齐。
它不向应用暴露内部像素排列。单资源 16 MiB 限额同时覆盖两个平面。
附件尺寸必须一致，pipeline 的附件格式必须逐项匹配。允许没有颜色附件的深度/模板 pass；
当前仍需要可链接的 fragment function。深度偏移、depth clip mode 切换、fragment depth 输出、
MSAA 和 resolve 尚未实现。

各平面独立支持 Load/Clear/DontCare 和 Store/DontCare。Clear 在绘制前由 GPU 填充整个附件，
不受 scissor/viewport 或写掩码限制；深度清除值钳制至 [0,1]，模板保留低 8 bits。
Load/Store 使用 backing 保留数据，DontCare 不保证清零。内核检查平面范围、归属、附件重叠和采样/写入反馈，
再生成 A540 深度/模板寄存器；LRZ/UBWC 关闭。绘制前使 CCU 深度缓存失效，绘制后刷新并等待深度 timestamp，
为后续 pass 和 blit 提供顺序保证。后续无深度/模板附件的 draw 显式清除相关状态。

buffer↔texture 拷贝对组合格式要求指定 `MTLBlitOptionDepthFromDepthStencil` 或
`MTLBlitOptionStencilFromDepthStencil`，分别传输 float32 或 uint8 平面；纹理互拷复制两个平面。
S8 支持非 DWORD 对齐的行/区域，边界字节在前序 GPU 操作完成后读取并合并，再由 GPU Fill32 写回，
其余对齐区域走 Copy32。这是初步正确性路径，尚未优化吞吐量。

### 顶点/片元接口和插值

顶点接口 struct 支持 float/float2/float3/float4 成员及一个 `[[position]]` float4；
片元通过 `[[stage_in]]` 读取同类 struct，position 成员来自 gl_FragCoord。
支持 `[[user(name)]]` 和按成员名生成的语义，支持 `center_perspective`（默认）、
`center_no_perspective`、`flat`。当前不支持嵌套接口、half/整数 varying、centroid/sample
插值或 pull-model。GLSL 保留字形式的 Metal 局部变量（例如 out/in）及成员会在降低时重命名。

AIR 从 `air.vertex_output` / `air.fragment_input` 的 user 或 generated 语义、float32 类型
和插值标记恢复接口；主机和设备读取器均支持。generated 字段名与显式 user 名称分别规范化，
以 SHA-256 的前 128 bits 标识身份，类型/分量数另外检查。每个 stage 的 GLSL location 是本地编号，
pipeline 按语义匹配，因此顶点与片元 struct 的成员顺序可以不同；AOT/JIT、MSL/AIR 可交叉链接。

编译器导出真实 VS 输出寄存器、FS 活跃 compmask/inloc、非位置标量数及逐像素 barycentric
寄存器。CPU 和内核共用接口校验/链接：拒绝活跃输入缺失、类型不匹配、重复语义、重叠位置、
非法寄存器、保留字段和超出 VPC 容量的记录；优化掉的输入不要求顶点输出。
最多记录 32 个用户语义；硬件输出表最多 32 项（含 position），VPC stride 不超过 127。
实际可接受数量取决于活跃分量及 position 的容量，而不是把 32 个 float4 都宣称为可用。

A540 按片元 inloc 建立 SP_VS_OUT/VPC_DST 映射，追加 position，设置 VPC_PACK、
VAR_DISABLE、插值表及 HLSQ/GRAS/RB 的 barycentric 状态。每次 draw 重写完整输出表和插值状态，
后续普通绘制不会继承旧 varying。此路径参考固定 Mesa 的 `ir3_link_shaders` 和 `fd5_program_emit`，
不接受用户寄存器包。实际光栅、插值精度和 provoking-vertex 行为仍待真机验证。

### 索引与实例绘制

已实现 `drawIndexedPrimitives` 的基本、instanceCount 和 baseVertex/baseInstance 入口，
以及 `drawPrimitives` 的 instanceCount/baseInstance 入口。支持 UInt16/UInt32，
indexBufferOffset 按元素大小对齐；triangle strip 使用对应位宽的全 1 restart 标记，
triangle list 将这些值当作普通顶点索引。baseVertex 为有符号 32 位数，内核检查
每个非 restart 索引加偏移后不为负、不溢出；零 count/instanceCount 为无操作。

每次最多 1048576 个顶点/索引、65535 个实例，count×instanceCount 不超过
16777216；start+count 和 baseInstance+instanceCount 不允许 32-bit 回绕。
Vertex shader 的 vertex_id 包含 start/baseVertex，instance_id 包含 baseInstance。
硬件 VFD 属性继续按顶点最大索引、实例步进和 baseInstance 检查实际 footprint。
Shader SSBO 访问改由真实 descriptor 长度和 NIR 守卫约束；带守卫的 VS 可以组合顶点/实例索引。
旧程序继续分别检查各自的静态索引 footprint。

索引内容在提交执行时从已准备的 IOMemoryDescriptor 复制到独立 DMA buffer，
扫描这个不可被用户映射的副本，再把副本地址写入 A540 的七 DWORD draw 包。
同一 command buffer 前面的 Compute/Blit 因而可以生成 Private 索引数据；
用户态编码阶段不读取 indexBuffer.contents，也不以用户提供的最大索引作为证明。
快照之后改写原索引无法改变此 draw 的取址范围。单份副本最多 16 MiB，整台 GPU
在途副本最多 16 MiB；成功 fence 或确认 halt 后回收，halt 失败则随完整作业图保留。
每次 draw 重写 VFD、restart、实例和驱动常量，避免随后普通绘制继承索引状态。

实例绘制同时支持显式 SSBO 与 MTLVertexDescriptor/VFD 输入。间接绘制和 GPU 控制的 draw 参数尚未开放。

### 顶点描述符与 VFD 输入

支持 `MTLVertexDescriptor` 的 attributes/layouts。MSL 可声明顶点 `[[stage_in]]` struct 的
`[[attribute(n)]]` 成员，或直接声明带 attribute 的 scalar/vector 参数；AIR 读取
`air.vertex_input`、`air.location_index` 与实际参数类型。MSL 的输入与输出 struct 可以不同。
GLSL/NIR/IR3 导出活跃 attribute 的分量、float32/int32/uint32 类型、真实寄存器和 compmask，
AOT/JIT 使用相同导出代码；优化掉的输入不要求 descriptor 或 buffer。

当前固定 Mesa GLSL 前端接受位置 0..15，顶点 buffer 绑定仍为 0..7。
支持 Float/Float2/3/4、Half/2/3/4、8/16/32-bit 有符号与无符号整数向量，
8/16-bit UNorm/SNorm、10:10:10:2 UNorm/SNorm、归一化 BGRA8 和 RG11B10 float 存储。
shader attribute 本身使用 32-bit scalar/vector；纯整数格式须匹配 int/uint shader 输入，
浮点与归一化格式须匹配 float。存储格式缺少的分量由硬件补默认值。
RGB9E5、half shader 参数、矩阵/数组 attribute、patch/tessellation 输入和完整函数反射尚未支持。

pipeline 创建时复制活跃 attribute 的 format/offset/bufferIndex 和对应 layout，
随后修改 `MTLVertexDescriptor` 不改变 pipeline。支持交错及独立 buffer，offset 最多 2047 bytes，
stride 最多 2048 bytes，并按格式元素大小检查对齐。
stepFunction 支持 Constant、PerVertex（stepRate=1）和 PerInstance（stepRate>=1）。
Constant 使用零硬件 stride；PerInstance 使用 VFD divisor 和 baseInstance，范围按
`baseInstance + floor(localInstance / stepRate)` 计算。`instance_id` 仍包含 baseInstance。

每次 draw 快照布局和绑定，内核在完成 Private 索引快照扫描后，按真正的最大有效顶点、
实例数、divisor、起始实例、stride、attribute 偏移及格式字节数检查每次读取的末端。
同一 buffer 同时用于 SSBO 时，两条访问路径都要通过 footprint 检查。
内核解析和保留原生资源，生成私有 A540 VFD fetch/decode/destination 寄存器，应用不传入寄存器或地址。
活跃 attribute 按稀疏 Metal 位置装入连续 VFD 槽，后续普通 draw 将 VTXCNT 归零，关闭旧输入。
此路径的离线检查覆盖真实 IR3 分配和 PM4；实际格式转换及实例步进结果仍需真机验证。

### 混合、视口和光栅状态

单个归一化 RGBA8/BGRA8 目标支持独立的 RGB / alpha 混合操作：Add、Subtract、
ReverseSubtract、Min、Max。支持 Zero/One、源/目标颜色及 alpha、对应反值、
SourceAlphaSaturated、常量颜色和常量 alpha 共 15 个因子；不支持双源混合。
Min/Max 忽略因子。`setBlendColorRed:green:blue:alpha:` 拒绝非有限输入，
将有限值限制到 [0,1]。颜色写掩码逐分量转换 Metal 与 A540 的位序，
掩码为零仍保留正常 draw 语义。内核以整数完成常量的 float32/binary16/
8-bit 编码，不引入内核浮点运算。

viewport 支持小数和负原点：原点绝对值不超过 16384，宽高不超过 16384；
非零宽高至少为 2^-15，znear/zfar 各自在 [0,1] 内，允许反向深度区间。零宽高 viewport/scissor 使 draw
成为空操作，不取消 render pass 的 clear。每次 draw 将 viewport 的像素中心覆盖范围
与显式 scissor 相交，再提交给内核。多 viewport/scissor 接口当前只接受一个元素。
支持 None/Front/Back 剔除和 Clockwise/CounterClockwise 正面绕序，默认不剔除、
Clockwise。A540 的实际光栅结果及绕序尚未在设备上验证。

pipeline 创建时复制混合/写掩码，每次 draw 再快照动态 viewport、混合常量、
剔除和绕序。随后修改 descriptor 或 encoder 状态不会改变已记录的 draw。
后端每次绘制都写入全部相关寄存器，包括关闭混合和清零常量，避免跨 draw/pass
继承旧状态。范围依据固定 Mesa 的 `fd5_blend.c`、`fd5_rasterizer.c` 和 A5xx
寄存器描述实现；没有增加完整 Metal family 声明。

## 安全提交和同步

`GPUShaderProtocol.h` 的 shader 命令版本为 24，定义 ShaderDispatch（648 bytes）
和 ShaderDraw（1016 bytes，含 80-byte RenderState、附件、72-byte DepthStencilState 、顶点布局和索引/实例参数），
每次原生提交只允许一个 shader 描述符，固定偏移在 CommandHeader + Command 之后。
提交只携带内置或当前连接登记的 program ID，不携带 GPU VA、寄存器或任意 PM4。
JIT 另用厂商选择器 0x1000/0x1001 注册/删除程序，Apple 原生资源和队列选择器保持原编号。
JIT 版本 21 通过原生 shmem 传入固定 20288-byte、无指针的 `QGPUProgramImage`，
其中 program description 为 1736 bytes，包含 32 项固定宽度 varying 和 16 项 attribute 记录；内核先快照，
再验证版本、A540、指令/常量/寄存器上限、stage、绑定 footprint 元数据及保留字段。
每连接最多 64 个动态程序，ID 从 0x80000000 起递增，删除后不重用；其他连接不能解析这些 ID。

**这是已有管理员连接上的受信编译器接口，不是针对不可信应用的 GPU ISA 沙箱。**
内核检查机器码布局和资源元数据，不证明机器码遵守声明的访存范围。资源的 IOVA 分配
仍由同一设备 IOMapper 管理；执行时每作业独立页表只包含本次保留资源及必需的只读固件/ring。
这不构成完整非特权 ISA 沙箱，尚不能对非特权应用开放动态机器码注册。
程序代码和常量在 gate 内复制进作业私有 IB，之后删除/关闭连接不会破坏在途作业。
它在解除 shmem 映射前复制整个描述符，验证版本、指纹、stage、保留字段、
全部 buffer footprint、texture pitch/尺寸、scissor 及渲染状态的有限数值/枚举范围；
纹理反馈及目标/顶点资源别名拒绝。
代码、常量和 PM4 位于 GPU 只读的私有 IB；绘制的内部 timestamp 使用独立可写页，
不再写入 IB。DMA 资源和私有页表保留至 fence 完成或确认 halt/drain、恢复内核根及 TLB 退休，
故障隔离继续保留完整对象图。

原生命令版本 3 仍支持 Fill32/Barrier/Copy32；诊断协议 2 保持 Fill32/Barrier。
Apple 原生选择器和对象 ABI 未变。shader 版本 1–17 和 JIT 版本 1–14 的旧描述符会被拒绝，
此次 Kext 与 Metal bundle 必须配套更新。
Copy32 使用 A5xx CP_MEM_TO_MEM，四字节对齐；用户态 fill/copy 的首尾非对齐字节在退休前序工作后合并，使用 Fill32 写回。
用户态每个设备的 transport lock 串行处理资源调用、上传/读回与提交，
提交前后执行共享域内存屏障并消费原生通知环。
原生 MIG 完成准入即返回；用户态 worker 在内核调用之外等待真实 end token，IRQ/watchdog 继续退休作业。
操作强引用 pipeline 和资源，完成后在 transport lock 外释放，避免析构重入死锁。
应用仍需遵守 Metal 的 CPU/GPU 资源使用同步规则。

## 统一构建和验证

```sh
make gpu-shaders                 # 首次构建固定 Mesa / LLVM AIR 主机前端和登记库
make gpu-driver                  # 自动生成着色器，构建 QcomGPU.kext 1.43.0
make gpu-client
make gpu-metal                   # bundle 0.33.0、metalctl、离线 rootfs 目录与信任缓存
make gpu                         # 更新 GPU kernel collection
make verify-gpu
python3 -m unittest discover -s tests -p test_gpu_integration.py -v
```

全部复用 `scripts/toolchain.py`，未新增构建或测试脚本。
`build/drivers/generated/GPU/shaders.json` 记录输入哈希、编译器 commit、真实机器码/元数据、
登记表指纹和生成文件哈希；库、Kext 和 collection 各有离线验证记录。
保留既有 gpu-submit、5 项集成检查和上一阶段的一个主机内存编译检查：
MSL / AIR 文本及位码实际编成 A540、修改算术常量改变机器码、重复编译的释放路径、
越界索引及元数据拒绝。主机检查不模拟 shader ISA 或光栅结果。
既有提交检查覆盖渲染状态 PM4、混合常量数值编码、非法状态拒绝和
后续 draw 状态复位，以及索引包、负偏移/溢出、strip restart 和实例常量。
现有内存编译检查增加 MSL/AIR 实例输入与独立 footprint 元数据检查，没有增加测试文件。
varying 变化继续扩展同一组检查，覆盖实际 AOT/JIT 交叉链接、插值代码生成、接口拒绝、
VPC/寄存器 PM4 和后续普通 draw 的复位。
本轮在相同检查中加入 D32/S8 平面、深度/模板寄存器、深度缓存事件、反向 Z、
附件重叠/非法格式拒绝和无附件状态复位，没有新增测试文件。

`metalctl pipeline-test` 是一个未来可人工执行的完整示例：计算加法、纯色绘制、
Shared→Private 纹理复制、采样绘制和 Shared 读回。`metalctl air-test` 另外从 bundle 的原始 Shaders.air 加载 AIR 函数并走同一示例。
`metalctl jit-test` 用未登记的运行时源码编译加 19 的 kernel、VS/FS 和采样 shader，再走同一提交示例。
`metalctl render-state-test` 在同一示例中加入局部 viewport、常量 alpha 混合及 R/A 写掩码，
并检查记录后的状态修改不影响此前 draw、后续默认 pass 正常复位。
`metalctl indexed-test` 复用这个示例，先 GPU blit 生成 Private UInt16/UInt32 索引，
再以非零 baseVertex/baseInstance 绘制两个实例，使用加法混合区分一遍/两遍覆盖。
`metalctl varying-test` 传递顶点颜色与 UV，比较颜色梯度及非均匀纹理采样结果；本轮只编译此入口。
原有 info/blit-test 保留。
**本轮仅交叉编译这些入口，未运行目标二进制、连接真机或修改显示/固件交接。**

系统插件的实际安装/加载验证、更完整的 MSL/AIR 语言支持、非特权 ISA 隔离、完整 Metal family 和显示同步仍需后续实现。

`metalctl depth-stencil-test` 是额外的可选真机入口：验证清除、深度拒绝、模板 depth-fail、跨 pass Store/Load 和组合纹理分平面回读。本轮仅交叉编译，未运行。

`metalctl vertex-test` / `vertex-air-test` 复用既有管线示例，检查交错 Float3/归一化 UChar4、每两实例步进、非零 baseInstance 和 descriptor 快照；仅编译，未运行。

### sRGB 颜色空间

`RGBA8Unorm_sRGB` / `BGRA8Unorm_sRGB` 沿用四字节线性布局与原生 IOGPU 资源桥。采样由 `TEX_CONST_0_SRGB` 在过滤前解码 RGB；渲染目标同时设置 `RB_MRT_BUF_INFO_COLOR_SRGB` 和 `SP_FS_MRT_REG_COLOR_SRGB`，按固定 Mesa fd5 模型执行颜色转换与混合。
Compute 的 read/write/read_write 经共用 MSL/AIR AOT/JIT image helper，在 guarded SSBO 访问周围执行分段 sRGB 转换。写入先处理 NaN 并限制到 [0,1]；alpha 始终保持线性，BGRA 的 R/B 交换独立执行。
清屏在用户态将线性 RGB 编码一次，然后提交原有 CP 填充；内核仍不使用浮点运算。replaceRegion/getBytes 和 blit 保持存储字节。IOSurface 继续按原有 RGBA/BGRA 字节格式、任务授权、尺寸与行距核对，允许对应的 sRGB 解释。

依据为 [Apple Metal 格式表](https://developer.apple.com/metal/capabilities/)、[MSL specification §8.7.7](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf) 与固定 Mesa 24.3.4 fd5 代码。新增格式要求 shader24/JIT21 成套部署；外层 Apple ABI 保持不变。完整 family/read-write tier 仍未开放，也没有运行目标设备或系统 Metal 加载。

### 共享纹理视图

三个 `newTextureViewWithPixelFormat:` 入口现接入同一个原生 backing。颜色格式限 RGBA8/BGRA8 的 UNorm 与 sRGB，支持非空的父级 mip 子范围、嵌套视图和 2D/slice0；相同格式的单级深度/模板纹理也可创建视图。`PixelFormatView` usage 可用于创建纹理；当前四种颜色格式的视图无需额外分配或拷贝。颜色 swizzle 已接通；二维颜色数组的切片视图已接通；立方体颜色视图已接通；跨格式深度视图仍返回 nil。

视图保留直接父纹理和 backing，公开 parentTexture、parentRelativeLevel 与 rootResource。CPU、blit、clear、render、Compute 和 mip 生成使用同一实际子范围偏移与尺寸。格式重解释只影响 GPU 对存储字节的解释；CPU/blit 保持原始字节。IOSurface 子视图保留单级尺寸、行距和任务授权，只允许已支持的四字节颜色解释。

分配沿用完整 fdl5 尾部 padding，准入/SSBO bounds/物理别名检查采用实际 mip 访问范围。2D sampler 的 array pitch 字段保留规范化对齐值，layer0 不用它寻址；不支持 GMEM 的 32×32 overfetch blit。复制检查覆盖所有来源/目标真实 mip 区间，不能用相对 mip 编号判断不同视图是否重叠。

厂商 shader 命令为 22，JIT image 为 19；Texture44 / Sampler28 / Dispatch792 / Draw2168 和 description1736/image20288 bytes，Apple 外层 ABI 不变。离线检查不代表目标系统已执行视图或 GPU 采样。接口参考 [Apple 纹理视图](https://developer.apple.com/documentation/metal/mtltexture/maketextureview%28pixelformat%3A%29) 与本地 iPhoneOS18.6 SDK MTLTexture.h。

### 颜色通道 swizzle

根纹理 descriptor 与 `newTextureView…swizzle:` 支持 RGBA、0、1 六种选择器，可重复任意分量。视图按照 Apple 文档将自己的选择器作用于父纹理的有效 swizzle；无显式 swizzle 的视图继承父模式，swizzle 属性公开最终模式。
A540 的 TEX_CONST_0 SWIZ_X/Y/Z/W 与 RGBA/BGRA 存储顺序组合。Compute read 在 sRGB 解码和 BGRA 调整后应用同一有效模式；alpha 只在被选作颜色通道并写入 sRGB 目标时按目标颜色语义编码。Compute write、render、clear 与原始字节复制忽略读取 swizzle。内部 mip blit 清除来源/目标 descriptor 的读取 swizzle，避免逐级累积通道置换；与 Apple 生成行为的目标一致性尚未运行验证。

GPU-owned image geometry 的 w[8..15] 保存可见 mip 数，w[16..27] 保存 swizzle，get_num_mip_levels、尺寸和范围检查分别掩码。非法坐标/mip 的受限 RGBA/BGRA read 仍在应用 swizzle 前返回全零，写入仍被忽略。深度/模板维持 identity-only 的现有 profile。

这次增加厂商 Texture 的 swizzle 字段：shader24/JIT21，Texture44、Dispatch792、Draw2168、Sampler28 bytes；description/image 仍是1736/20288 bytes。旧厂商描述符或 JIT image 会拒绝，Apple 外层 ABI 不变。参考 [Apple descriptor swizzle](https://developer.apple.com/documentation/metal/mtltexturedescriptor/swizzle)、[视图组合规则](https://developer.apple.com/documentation/metal/mtltexture/newtextureviewwithpixelformat%3Atexturetype%3Alevels%3Aslices%3Aswizzle%3A) 和固定 Mesa fd5_tex_swiz；尚未运行 iOS 方法或 A540 指令。

### 非整除 Compute 网格

`dispatchThreads:threadsPerThreadgroup:` 现接受任意正数网格尺寸。厂商 Dispatch 记录原始 grid，原生准入核对 `groups=ceil(grid/local)`、实际总线程数与请求组大小；新增保留字段必须为0，uniform 的 dispatchThreadgroups 保持 grid 全零编码。

A540 后端将 full/tail 的笛卡尔组合拆为至多8个区域，一次原生提交中只上传一次代码、资源与 texture/sampler/SSBO 状态，各区域更新常量、真实 local size 和 NDRANGE 后执行 CP_EXEC_CS。整个原始 dispatch 仍使用一次真实完成 fence。尾组只启动实际线程，因此共享内存屏障无需等待填充线程；动态 shared 长度仍使用调用时快照和原32 KiB检查。

MSL/AIR 前端使用原生只读 geometry64..79修正全局线程/组坐标与原始 grid/group 数。threads_per_threadgroup 返回实际组大小，dispatch_threads_per_threadgroup 返回调用时大小。普通buffer、shared、image、sampler 常量区位置保持原值；引用新geometry的Compute程序设置flag128并保留80words，driver/immediates排在其后。非Compute阶段拒绝该flag。固定local的可信ISA程序不能提交不足64线程的尾组，现有Metal编译器生成的Compute均为variable-local。

协议为 shader24/JIT21，Dispatch792 bytes；Texture44/Sampler28/Draw2168、description1736/image20288及Apple外层ABI保持原值。CPU GLSL验证明确按每个实际local size专化，不能证明A540变量组大小、共享内存或native回调已运行。

### 二维颜色纹理数组

`MTLTextureType2DArray` 支持现有四种 RGBA8/BGRA8 UNorm/sRGB 颜色格式，层数最多2048，实际分配仍受单资源16 MiB配额约束。存储采用每层完整 mip 链及4 KiB对齐层间距；非空 mip/slice 子范围、嵌套视图和2D/2DArray互转保留原层间距、父对象与 backing。CPU get/replace、buffer/texture blit、清屏、单切片 Render 附件及逐层 mip 生成使用所选切片的实际偏移。多切片复制先核对全部源/目标区间；普通纹理复制要求层数相同。

MSL AOT/JIT 接受 `texture2d_array<float>` 的 read/write/read_write/sample 和 get_array_size，二维坐标、slice 与 mip 是独立参数。Compute 查询/图像访问由原生常量80..95中的层数/字节层间距驱动；无效 slice/坐标/mip 的受限读取返回零，写入被抑制。Compute/Fragment 采样支持已有的隐式、level、bias（Fragment）和gradient2d路径，使用真实A5xx ARRAY_PITCH/DEPTH状态和IR3数组采样。sRGB/swizzle遵循此前的读取与存储语义。

厂商 flags 的8..15位按纹理槽标记数组类型，原生准入拒绝 shader 与资源类型不匹配。Compute 数组程序保留96-word前缀，普通geometry程序仍为80，Fragment sampler仍为16；description/image维持1736/20288 bytes。shader24/JIT21配套使用Texture44、Dispatch792、Draw2168、Sampler28 bytes，Apple外层ABI保持原值。采样/写入及Render反馈按各切片实际区间检查物理别名，不把层间未公开的mip当作访问；不同资源句柄映射相同物理页也保留检查。

本轮未猜测AIR数组intrinsic签名；AIR数组仍明确拒绝，现有二维AIR路径继续保留。IOSurface创建/导入仍是单层2D，兼容的单切片数组视图复用原有授权；多层导入被拒绝。数组深度、顶点选择Render层和完整Metal family尚未完成，也未运行目标系统或A540。参考[Apple二维数组类型](https://developer.apple.com/documentation/metal/mtltexturetype/type2darray)、[切片视图](https://developer.apple.com/documentation/metal/mtltexture/newtextureviewwithpixelformat%3Atexturetype%3Alevels%3Aslices%3A)及固定Mesa fd5_texture.c。

### 顶点纹理采样

VS 与 FS 现分别支持8个 texture 与8个独立 sampler 槽，Metal 的单项、范围及 LOD clamp 顶点绑定入口已接通。每次 draw 保存独立的顶点 Texture/Sampler 快照，并保留纹理视图、父资源和 sampler 到原有真实完成 fence。`initLimits` 的两个图形阶段的 texture/sampler 上限均为8，结构布局仍为264 bytes；完整 GPU family 继续关闭。

MSL AOT/JIT 接受顶点 texture2d/texture2d_array 的 sample、level 与 gradient2d；AIR AOT/JIT 扩展既有二维 sample intrinsic 到顶点阶段，数组/gradient AIR ABI 仍不猜测。无 LOD option 时使用 level0 和原 sampler clamp，显式 level 走忽略 sampler clamp 的 shadow 状态。颜色 mip/view/swizzle/sRGB、边界色和已有地址模式复用同一采样后端，MSL 示例用采样值改变顶点 Z 并输出 tint/uv。

A540 VS 使用 SB4_VS_TEX block0 与 TPL1_VS_TEX_COUNT，FS 仍使用 block4。两个阶段各保留16 words sampler 常量，mirror 标志按sampler槽写在word0..7，driver/immediates 在前缀之后；VS 的 GLSL uniform 名称独立，避免 CPU GL 链接时混用阶段状态。无采样的后续 draw 显式清除 VS texture count。

厂商 Draw 末尾增加 vertexTextures[8]、vertexSamplers[8] 和必须为0的 vertexReserved，Shader24/JIT21 配套使用 Draw2168 / Dispatch792 / Texture44 / Sampler28 bytes；description/image仍为1736/20288，Apple 外层 ABI 不变。原生准入允许 VS/FS 纹理及只读 buffer 互相读取相同 backing，继续按各切片实际物理区间拒绝附件反馈以及 fragment buffer 写入与两阶段纹理的重叠。

离线检查分别验证生成的 A540 代码、PM4 状态和 CPU 顶点/插值语义。CPU transform feedback 观察真实 GLSL 的 position/tint/uv；组合渲染按生产链接器导出的语义对应关系重排 GL fragment location。它们没有执行 A540、iOS Metal 方法、原生回调或目标 framebuffer 坐标变换。参考[Apple 顶点纹理绑定](https://developer.apple.com/documentation/metal/mtlrendercommandencoder/setvertextexture%28_%3Aindex%3A%29)、[MSL specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf)及固定 Mesa fd5_emit.c。

### 图形阶段多槽纹理

Vertex 与 Fragment 均支持纹理槽0..7、sampler槽0..7及单项/范围/范围LOD clamp绑定。纹理与sampler索引独立，稀疏绑定和同纹理多sampler可混合使用；MSL支持二维及颜色数组，AIR复用已有二维intrinsic签名。动态纹理索引、AIR数组/gradient ABI和完整Metal family仍未实现。

统一采样前端将组合句柄拆为真实texture/sampler索引。普通sampler使用硬件槽0..7，显式level影子sampler使用8..15；每阶段常量word0..7保存各自镜像标志，16-word前缀不变。VS/FS分别上传SB4 block0/4状态，texture count为最高活动纹理槽加一，两个count在每次draw中均重写。边界色表通过原有公共表及每sampler偏移共享，不因阶段合用而混淆。

每次draw仅快照shader实际使用的纹理与sampler掩码，绑定对象可在后续改写，已记录操作仍持有原对象及独立LOD值。未引用的厂商描述符槽必须为空；数组类型按每个texture槽核对。原生层将全部16个采样纹理与颜色/深度/模板附件及fragment buffer写入进行真实物理区间检查，读/读别名继续允许，资源总配额保持原值。

Shader24/JIT21配套使用Draw2168 bytes，Texture44、Sampler28、Dispatch792、description1736/image20288及Apple外层ABI不变。新增稀疏混合采样的MSL/AIR示例，既有编译与提交检查覆盖全8槽、交叉配对、影子sampler、独立阶段常量、空槽和类型拒绝；这些离线检查不等于目标硬件执行。范围接口依据[Apple fragment textures](https://developer.apple.com/documentation/metal/mtlrendercommandencoder/setfragmenttextures%28_%3Arange%3A%29)及本地iOS18 SDK声明。

### 立方体颜色纹理

Cube/CubeArray保存六个方形面，面顺序为+X、-X、+Y、-Y、+Z、-Z。arrayLength为立方体数量，CPU/blit/render的slice按面编号，Cube为6个面，CubeArray为6倍元素数量。每面使用原有fdl5 mip链及页对齐间距；嵌套mip/面视图保留父对象、真实偏移及原面间距。Cube视图长度为6，CubeArray为6的倍数，起始面独立保留；单面2D与面范围2DArray视图复用同一backing。逐面CPU复制、清屏、单面Render及GPU mip生成沿用现有平面路径，物理反馈/写入别名按全部实际面区间检查。

MSL AOT/JIT在VS/FS/Compute支持texturecube<float>、texturecube_array<float>的sample、level和gradientcube，Fragment另支持bias。Cube方向直接使用float3，不经过二维镜像abs转换；数组元素另附uint索引。A5xx使用TYPE2、原始面ARRAY_PITCH及按立方体数量编码的DEPTH。cubeSamplerMask记录实际cube采样器绑定，要求归一化坐标并启用对应sampler的无缝过滤；混合2D/cube绑定保持独立。AIR cube签名、Compute中的Cube直接image读写/查询已实现；深度cube及完整Metal family仍未实现。

Shader24/JIT21配套使用Texture44、Sampler28、Dispatch792、Draw2168 bytes，program description1736/image20288。Texture新增Boolean cube维度，layers在CubeArray中保存立方体数量；flags16..23标记各texture槽cube类型。新增保留字段必须为0，Apple外层资源/队列ABI和264-byte限制结构布局不变。离线检查包含原有54个A540指令字节保持一致、PM4 cube状态、面布局/视图/别名，以及CPU GLSL的六面、数组索引与LOD语义；没有执行iOS方法或A540硬件。接口依据[Apple CubeArray](https://developer.apple.com/documentation/metal/mtltexturetype/typecubearray)、[视图类型与长度](https://developer.apple.com/documentation/metal/mtltexture/maketextureview%28pixelformat%3Atexturetype%3Alevels%3Aslices%3A%29)、MSL6.13.6/6.13.7及固定Mesa fd5_texture.c。

### Cube直接图像操作与较大着色器

Compute的texturecube/texturecube_array现接受read/write/read_write，以及宽、高、mip数和CubeArray元素数查询。读写参数依次为二维坐标、face、可选cube元素、可选mip，write在前面增加float4颜色。原生图像常量保留在16..47，80..95记录实际面数和原始面字节间距；cube索引乘6前先核对face<6及cube<faceCount/6。无效坐标、面、cube或mip读取为零、写入被抑制。sRGB/BGRA/swizzle读转换及忽略store swizzle沿用同一生产helper，形状命名空间防止二维、Cube和CubeArray混用。Cube查询和image操作不要求sampler，cubeSamplerMask仍只记录实际硬件采样器。

A540的Compute预加载限制为32个128-byte单位，VS/FS共享64个单位缓存；超过容量时按固定Mesa fd5_compute/fd5_program模型从内存取指，清除对应BUFFER和缓存长度并跳过CP代码预加载，同时保留SP对象地址。图形阶段优先缓存可容纳的FS，其余阶段使用内存。当前每程序代码配额为16KiB，IB为64KiB，代码随只读private job页表及真实完成fence保留，故障时沿用隔离/保留规则。该路径通过离线状态编码检查，尚未在A540执行。

厂商协议为Shader24/JIT21，program description仍1736 bytes，扩大代码数组后的image为20288 bytes；Texture44/Dispatch792/Draw2168/Sampler28及Apple外层ABI不变。新增Cube复制、CubeArray复制、read_write更新和无sampler查询示例均生成真实A540代码；CPU GLSL核对原生导出的面视图、颜色转换、越界行为和未触及字节。AIR cube ABI、图形阶段直接image访问/查询、深度cube和完整Metal family仍未完成，所有目标运行均未验证。代码缓存依据[固定Mesa A5xx Compute](https://raw.githubusercontent.com/chaotic-cx/mesa-mirror/769e51468b49b2a42f0a0eaf71cf9eed5ff4e5de/src/gallium/drivers/freedreno/a5xx/fd5_compute.c)及同版本fd5_program.c。


### 编译期采样器

MSL 的全局 `constant sampler`、全局/局部 `constexpr sampler` 已接入同一编译器。局部声明遵循块和循环作用域，遮蔽外部同名采样器不会改写已生成的采样操作。支持 `coord`、`filter`、`min_filter`、`mag_filter`、`mip_filter`、`address`、`s_address`/`t_address`/`r_address`、`border_color`、`compare_func::never`、`reduction::weighted_average`、`max_anisotropy` 和 `lod_clamp`。默认地址为 `clamp_to_edge`；`mirror_clamp_to_edge` 是已有 API 采样器状态，不作为 MSL 常量枚举接受。声明、默认值和 pixel 限制依据 [Apple MSL 2.10、表2.7](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf)。

```metal
constant sampler filtering(filter::linear, mip_filter::linear,
                           address::repeat, lod_clamp(0.25f, 4.0f));
fragment float4 sample_color(texture2d<float> image [[texture(0)]]) {
    return image.sample(filtering, float2(0.5));
}
```

AIR 的只读地址空间2全局常量可为旧式 `i64` 或 `[2 x i64]`，并兼容 sampler 常量 bitcast。AOT 的 LLVM C++ reader 和设备端 LLVM C reader 共用位域、half 转换和状态校验，不按函数名替换实现。公开 Apple 编译输出及[固定版本采样器编码参考](https://github.com/steelbrain/metal2vulkan/blob/720ef1ce9ae2a2068999a8675717418b9416283e/crates/metal2vulkan-air/src/abi/sampler.rs)保存在 `build/drivers/gpu-constant-samplers-primary-reference.json`；较新工具链的 AIR 导入不构成 iOS18 运行保证。

Shader26/JIT23 的不可变程序前缀新增600 bytes，保存状态、实际使用掩码及物理槽映射。描述字段是过滤、8.8 LOD、地址、各向异性、边界色和坐标语义，原生后端生成 A5xx 描述符。MSL/AIR 前端按语义去重，最多16个常量状态。每阶段仍有8个应用 sampler 槽；隐式/gradient 使用外部槽 i，显式 level 使用 i+8，仅实际使用者占位。后端将常量采样器放入余下槽位，两种 LOD 模式分别分配。每个阶段总计最多16个物理状态、32个内部 GLSL 组合 uniform；耗尽返回编译错误。GL 链接后才分配虚拟索引，进入 IR3 前解析为真实槽，不扩大硬件数量。

Compute、Vertex、Fragment 的原生提交共用状态生成，常量采样器不要求额外应用 sampler 对象。纹理、程序和代码仍沿用原生准入及 fence 生命周期；阶段之间的状态和槽位独立。原生校验拒绝冲突/越界槽、非零保留字段、未使用的非零记录和非归一化 Cube sampler。显式 level 状态继续绕过 LOD clamp，mip_filter::none 保持只读基础级。pixel 状态限现有 clamp_to_edge、无 mip/各向异性且 min/mag 一致的后端子集。

比较采样、minimum/maximum reduction、bicubic、非零 AIR sampler bias、未知位域与无法精确表示为8.8的小 LOD clamp仍拒绝。较大有限 LOD clamp 饱和到4095/256，高于所有已支持 mip 级。RGBA32Float 等较宽格式、argument buffer 和完整 Metal family 的其余缺口保持记录；未执行真机或系统 Metal 加载。


### 多颜色渲染目标

支持8个 RGBA8/BGRA8 UNorm/sRGB 线性颜色附件，包括单 mip/slice 视图。`MTLRenderPipelineDescriptor.colorAttachments[0..7]` 保存各自格式、写掩码、RGB/alpha 运算与因子；`setBlendColor` 保持 Metal 的全局混合常量语义。渲染通道逐附件校验设备、usage、mip、slice、加载/存储动作、相同尺寸和 backing 区间，再记录全部 clear。sRGB clear 只编码 RGB，BGRA 顺序和 alpha 分别保留。所有附件和程序保留至原生完成；原生再次检查不同句柄映射相同物理页的情况，并核对附件与纹理读、阶段 buffer 的别名。仍受原有资源、内存及命令配额约束。

MSL 的片元结果结构体接受独立 `float4 [[color(n)]]` 成员，索引0..7可留空，字段顺序与附件槽位独立。既有标量 `float4` 结果仍输出 color0。AIR 接受相同 float4 渲染目标元数据及多个返回成员，输出采用各成员自己的目标索引，dual-source index仅接受0。两个 AIR reader 和 MSL 共用的后端在接口分配前按语义位置排序；AOT/JIT 为非顺序颜色声明生成相同寄存器和代码。

```metal
struct Colors {
    float4 base [[color(0)]];
    float4 normal [[color(3)]];
    float4 material [[color(7)]];
};
fragment Colors render_colors(constant float4 &value [[buffer(0)]]) {
    Colors out;
    out.base = value;
    out.normal = value.bgra;
    out.material = value * 0.5;
    return out;
}
```

Shader27/JIT24 的程序描述新增40 bytes，保存真实 IR3 颜色输出寄存器、掩码和零保留字段。color0不存在时，旧 `color` 字段为252，其他目标正常输出。Draw新增7份纹理描述及32-byte独立颜色状态，携带零保留字段；大小2704 bytes。description2384/image20936与同版内核、bundle和工具必须一起使用。Apple 外层队列/资源 ABI、264-byte limits布局、Texture44/Sampler28/Dispatch792保持原值，limits现返回8个颜色附件及256个 framebuffer storage bits；完整 family与read/write tier仍关闭。

原生后端按真实输出生成 `SP_FS_OUTPUT_REG0..7`，按最高已绑定附件槽加一设置 MRT count，逐目标写入地址、pitch、格式、sRGB、component mask与混合状态。只有 color7 时 count为8，空槽状态清零。涉及后续槽位时设置 `RB_BLEND_CNTL.INDEPENDENT_BLEND`，未写出的目标禁止颜色更新。共享视口、裁剪、深度/模板及完成缓存刷新沿用现有路径；裁剪尺寸在原生准入和 PM4 编码器重复核对。

寄存器模型依据固定 Mesa 的 [fd5_program.c](https://github.com/chaotic-cx/mesa-mirror/blob/769e51468b49b2a42f0a0eaf71cf9eed5ff4e5de/src/gallium/drivers/freedreno/a5xx/fd5_program.c)、同版本 fd5_emit.c/fd5_blend.c 与 a5xx.xml。接口依据 [Apple colorAttachments](https://developer.apple.com/documentation/metal/mtlrenderpipelinedescriptor/colorattachments)及本地 iOS18 SDK。公开 Apple AIR 的 color2元数据作为编码参考，其 half4及raster-order-group例子不作为已支持语言；未接入的 interlock 注解现在明确拒绝，避免丢失排序语义。

新增3个 MSL、2个 AIR 示例，总计87个 AOT程序，原有82个机器码保持一致。现有编译检查覆盖 AOT/JIT及IR/bitcode，PM4检查覆盖全8槽、稀疏槽、独立混合和缺少color0的目标；CPU llvmpipe仅对照4次未改写生产GLSL的输出及逐目标UNorm/sRGB/BGRA转换，不能验证A540光栅、混合、纹理或iOS方法执行。没有新增测试文件，也未进行真机或系统Metal加载测试。MSAA、分层渲染、dual-source、片元interlock、较宽格式与完整Metal能力仍待完成。


## 片元深度与丢弃

GPU1.48.0 / Metal0.38.0 使用 Shader28/JIT25。程序前缀最后一个保留字改为 `depthReg`，未写深度时必须为0；新增独立的片元 discard/depth 标志，2384-byte description、20936-byte image 和2704-byte Draw 的布局长度不变。更新时必须配套部署内核驱动、Metal插件和编译器。

MSL 片元结果结构支持 `float [[depth(any/less/greater)]]`，可与8路 `float4 [[color(n)]]` 混用，也可只写深度；`fragment void` 可不输出颜色。AIR 的标量 `air.depth` 输出与原有颜色聚合成员使用各自真实 IR3 寄存器。深度/模板 PSO 省略 `fragmentFunction` 时，插件使用私有 `raster_depth_only` AOT 程序维持原生 IOGPU 提交；省略函数且声明颜色附件的配置仍拒绝。

```metal
struct DepthColor { float depth [[depth(any)]]; float4 color [[color(0)]]; };
fragment DepthColor shade(float4 p [[position]]) {
    if (p.x < 32.0) discard_fragment();
    DepthColor out;
    out.depth = p.z;
    out.color = float4(dfdx(p.x), dfdy(p.y), fwidth(p.z), 1.0);
    return out;
}
```

`discard_fragment()` 和 `air.discard_fragment()` 共用 NIR 软件 demote 路径：记录每个片元的丢弃状态，屏蔽后续 SSBO 写入和受支持的原子操作，保留之前的写入，继续执行私有变量、读取和导数，最后在统一出口 KILL。A540 不宣称硬件 demote 能力。内部 GLSL 启用 demote 语法，并排除原先针对 discard 的循环退出变换；IR3 的调度依赖约束让显式导数和含隐式导数的采样留在最终 KILL 之前。MSL 的 `dfdx`、`dfdy` 和 `fwidth` 限片元阶段。语义依据 [Apple MSL 片元函数章节](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf)，AIR 的 float32 标量/向量浮点比较也已统一处理 ordered/unordered 与 NaN，不依赖可选的 LLVM fast-math 查询符号。观察到的 Apple AIR 签名及深度 metadata 记录在 `build/drivers/gpu-fragment-primary-reference.json`；现代 macOS 编译器样本不代表 iOS18 已运行验证。

A5xx 的深度平面标志用于 late-Z，受 `no_earlyz`、discard 或实际深度输出共同控制。只有真正写深度的程序才设置 `RB_FS_OUTPUT_CNTL.FRAG_WRITES_Z` 和真实 `SP_FS_OUTPUT_CNTL.DEPTH_REGID`，其他程序使用252空寄存器，LRZ继续关闭。此阶段仍不支持 `early_fragment_tests`、模板/采样掩码输出、framebuffer fetch 和片元图像写入；完整 Metal family 保持关闭，未做真机测试。当前构建与检查结果见 `validation.json`。


本轮离线结果：96个 AOT 程序（55 MSL / 41 AIR，Compute32），原87个程序的 ISA 字节未变；96份完整程序前缀和指令载荷均在最终 Kext 与签名插件中核对。未经修改的公共 Apple AIR discard 样本通过两个读取入口，生成一致 ISA。现有编译器、PM4 和5项集成检查通过，未增加测试文件；原生集合30个Kext及10个本地包、签名代码页和开发信任缓存已核验。没有新增真机或 CPU 图形执行；旧 MRT 的 llvmpipe 记录保留为历史证据。详细结果：`build/drivers/gpu-fragment-inspection.json`。


## 多重采样与颜色 Resolve

GPU1.49.0 / Metal0.39.0 新增私有线性 `MTLTextureType2DMultisample`，支持2×/4×；普通纹理仍为1×。颜色支持现有 RGBA8/BGRA8 UNorm/sRGB，深度/模板支持 D32、S8 及分开存储的 D32S8。多重采样纹理限制单 mip、单 slice，禁止 shader write。附件和 PSO 的样本数必须一致；颜色、深度和模板的 pitch、内存范围和别名检查均覆盖全部样本。样本数属于纹理/绘制状态，与着色器的资源类型掩码独立。

```metal
kernel void resolve_example(texture2d_ms<float, access::read> source [[texture(0)]],
                            texture2d<float, access::write> target [[texture(7)]],
                            uint2 p [[thread_position_in_grid]]) {
    float4 total = float4(0.0);
    for (uint s = 0; s < source.get_num_samples(); ++s)
        total = total + source.read(p, s);
    target.write(total / float(source.get_num_samples()), p);
}
```

MSL 的 `texture2d_ms<float>` 可在 Compute/Vertex/Fragment 中逐样本读取，并查询宽、高、样本数，不需要 sampler。Compute 使用带坐标和样本边界检查的 SSBO 地址；图形阶段生成真实 IR3 `ISAMM`，使用带 MSAA 标志的原生 texture descriptor。未绑定类型、样本越界和错误的普通/多重采样资源绑定受到相应检查；越界读取返回0。MSAA 数量编码占原生 image/query 格式字的28..29位，swizzle仍在16..27位，程序前缀新增独立 `multisampleMask` 及零保留字。

A540 的 RB、GRAS、TPL1 光栅/目标 MSAA 状态按2×/4×设置，颜色和深度/模板地址保持原生作业的资源生命周期。已接 alpha-to-coverage，并提供1/2/4样本默认位置。参考固定 Mesa 的 `fd5_gmem.c`、`fd5_texture.c`、`fdl5_layout.c`、`ir3_compiler_nir.c` 和 `u_sample_positions.c`；布局 `cpp=formatBytes*samples`，每像素样本沿 X 方向连续。

颜色附件允许 `MultisampleResolve` 与 `StoreAndMultisampleResolve`。编码器在任何 clear/绘制之前核对 resolve 的格式、范围、单样本目标和所有附件/目标别名；`endEncoding` 在现有原生队列中记录私有 `resolve_color` Compute 作业，等待前序真实 fence 后再读取源附件。运算先把各样本解码到线性 float4，平均后只编码一次 sRGB；BGRA 顺序及线性 alpha 沿用现有 helper，framebuffer resolve 忽略源视图 swizzle。源和目标保留到原生完成，实际像素计算由 GPU 指令执行。用户 usage 无需额外声明 ShaderWrite 来使用 render resolve 目标。

同样本数纹理之间的复制覆盖全部样本；不同样本数复制、MSAA buffer/texture 复制、mipmap生成和 IOSurface MSAA 导入被拒绝。兼容的颜色/深度视图保留样本数。自定义 sample positions、MSAA数组、memoryless、sample-frequency输入/插值、sample-mask输出、深度/模板resolve、alpha-to-one及尚无已核对签名的 AIR 多重采样纹理 intrinsic 仍未接入；公共 Apple AIR 多重采样样本只作接口参考，未把未支持的 sample/centroid 输入当作已实现。完整 family保持关闭。

Shader29/JIT26 必须配套内核、插件、编译器和工具。Texture48、Sampler28、Dispatch824、Draw2816、description2392、image20944 bytes；Apple外层 IOGPU ABI和264-byte limits不变。既有普通纹理和片元深度/discard路径保留。

本阶段没有真机或 iOS Metal 执行。颜色 resolve 的 CPU llvmpipe 对照只把 variable local-size 声明换成 local_size1，保留完整生产 shader body/helpers；8组2×/4×、RGBA/BGRA、UNorm/sRGB结果与参考计算最大差1 byte，额外越界线程不写入，行尾padding保留。该对照不验证A540 MSAA光栅、采样描述符或native执行。当前构建结果见 `validation.json` 与 `build/drivers/gpu-msaa-inspection.json`。


## 逐样本片元执行与插值

GPU1.50.0 / Metal0.40.0 在已有的2×/4× MSAA 路径中接入 `uint [[sample_id]]`、深度/模板测试前的 `uint [[sample_mask]]` 输入，以及片元结果结构体的 `uint [[sample_mask]]` 输出。AIR 两个读取器均识别 `air.sample_id` 和 `air.sample_mask`，保留实际 `uint` 类型与重复语义检查。MSL stage_in 和 AIR varying 新增 perspective/linear 各自的 centroid 与 sample 插值；未声明采样属性的输入仍使用 pixel 插值。`sample_position`、插值查询 intrinsic、post-depth coverage、early_fragment_tests 和 framebuffer fetch 尚未接入。

每个片元函数拥有独立编译的单采样和 MSAA 版本，包含各自的代码、立即数、资源使用和物理寄存器。两份版本使用同一个公共函数 ID；JIT 注册、删除和资源保留也只有一个所有者。Metal 创建 PSO、编码资源，内核进行资源检查和构建 IB 时均按 `rasterSamples` 选择版本。单采样下 sample/centroid 输入降为 pixel 插值；MSAA 下保留对应采样点。子版本不会成为额外的可见 Metal 函数。

A5xx 的逐样本执行需要真实的 HALF SampleID 输入，并设置 `RB_RENDER_CONTROL1.SAMPLEMODE=3`。统一工具链定向移植 [Mesa MR44343](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/44343) 的活跃 SampleID 修正和寄存器定义，保留原有固定 Mesa 提交。原生提交使用编译器实际分配的 HLSQ pixel/centroid/sample 寄存器，分别开启 GRAS/RB 插值通道，修正 linear-centroid 的字段，开启 center-RHW 所需通道。覆盖输出使用独立的 RB 写入标志和 SP 掩码寄存器；1× 附件关闭该输出。掩码输出同时使用 late-Z，避免覆盖筛选之前提交深度。

[Metal 着色语言规范](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf) 要求 position 在逐样本执行时仍来自像素中心。共享 NIR 路径在入口处计算像素中心 XY；使用窗口深度和 reciprocal-W 的细粒度四元组梯度，把 Z/W 从实际采样点移回像素中心。该计算早于分支和 discard，center-qualified varying 继续使用 pixel 插值器。此硬件模式和位置校正仍需后续真机验证。

Shader30/JIT27 的 pointer-free description 为2424 bytes，完整 image 为41832 bytes，包含第二个20856-byte 片元版本。新增字段记录执行模式、采样 ID、覆盖掩码和四种插值寄存器；presence bit 区分无效字段和合法 R0。内核检查两份前缀、阶段、寄存器范围、公共语义及资源声明；非片元 image 的子版本必须全零。Apple 外层 IOGPU ABI、264-byte limits 和 Draw2816 保持原尺寸，旧 JIT image 不兼容。

本阶段使用新增的 `SampleShading.metal` / `SampleShading.air.ll` 作为真实 AOT 内置示例，扩展已有编译器与 PM4 检查，核对两份代码和完整前缀、采样执行及覆盖输出。没有增加测试文件，也没有执行 Metal 系统加载或 A540 硬件作业。完整 Metal family、read/write texture tier 与通用 JIT 权限保持原限制。

当前离线产物包含106个公共 AOT 函数（63 MSL / 43 AIR）及51个 MSAA 片元子版本；157份完整2424-byte 前缀和 ISA 均存在于实际 Kext 与签名后的 Metal 插件。原有99个函数的 ISA 和立即数保持一致。现有编译器、PM4、5项集成检查，以及10个本地 Kext / 30个 collection Kext 的离线检查均通过；详见 `validation.json`。


## 深度与模板 Resolve

GPU1.51.0 / Metal0.41.0 为2×/4× D32、S8 和分平面 D32S8 附件接入原生 GPU resolve。`depthResolveFilter` 支持 Sample0、Min、Max；`stencilResolveFilter` 支持 Sample0 和 DepthResolvedSample。后者按深度过滤器选择模板样本，即使没有设置深度 resolve 目标也需要读取原始深度附件；这是 [Apple 的接口语义](https://developer.apple.com/documentation/metal/mtlmultisamplestencilresolvefilter/depthresolvedsample)，同时核对了 iOS18 SDK 的 `MTLRenderPass.h`。缺少深度附件时拒绝 DepthResolvedSample。目标必须是同格式、同尺寸的单采样纹理；D32S8 深度/模板目标可以是同一个纹理的非重叠平面。

两个内部 `Resolve.metal` Compute 程序经普通 MSL/AOT IR3 编译，源、目标和32-byte参数使用已有受边界检查的 SSBO buffer ABI；没有将深度/模板伪装成颜色 image，也不在 CPU 上执行 resolve。深度复制选中样本的原始32-bit位模式，用 `as_type<float>` 作比较。Min/Max 仅在严格较小/较大时换样本，相同或不可比较时保留先遇到的样本；深度与模板遵循相同规则，避免相同深度值时取到不一致的模板。

模板按一个 invocation 拥有一个目标32-bit字的方式写回四个像素，最后不足四像素的字保留其余填充字节。这样无需共享字的逐字节 RMW 或额外原子。MSAA S8 源使用线性、逐像素连续样本布局，源/目标 pitch 独立。共享 `depthStencilResolvePlan` 校验尺寸、格式、采样数、平面范围及依赖，再生成实际 buffer 范围和网格。

Metal 桥扩展为同时检查所有颜色、深度、模板附件与 resolve 目标；检查在记录 clear 之前完成。深度和模板 resolve 在绘制后进入原生异步队列，源纹理、目标纹理、模板依赖的原始深度和参数缓冲区均保留到真实 native fence 完成。MultisampleResolve 和 StoreAndMultisampleResolve 都受支持，源内容由现有私有线性存储保留。Shader30/JIT27 与现有结构尺寸保持一致，新增 AOT 内容由 registry 区分。

MSL `as_type<T>` 现在支持 float/int/uint 的32-bit scalar/vector1..4，要求源与目标分量数一致。生成 GLSL 的 typed overload，由 GLSL/NIR 检查源类型，使用位重解释与同位宽整数转换。不同标量宽度、half/short/char/bool、利用 float3 padding 跨分量数转换仍未接入。

当前有108个公共 AOT 函数和51个 MSAA 子版本；旧106个函数和51个子版本的 ISA/立即数保持不变。已有编译器和 PM4 检查覆盖新计划与真实后端。另用 CPU llvmpipe 执行实际 resolve GLSL，只替换 variable-local-size 声明：6组深度/模板配对及2组不依赖深度的模板 Sample0，通过逐位结果、部分字、行填充和越界 invocation 检查，包含重复深度、signed zero 和 NaN 比较。这不是 A540、Metal 方法或系统加载的执行证据。没有新增测试文件；实际目标验证和完整 Metal family 仍未开启。


## MSAA数组与切片

GPU1.52.0 / Metal0.42.0 接入私有、单 mip 的2×/4× `MTLTextureType2DMultisampleArray`，以及 D32/S8/D32S8 的普通或多采样数组附件。每层按页对齐，整个资源继续受16 MiB上限约束。D32S8 数组先放置全部深度层，再放置全部模板层，两类平面保存各自的层步长，切片视图继续引用原始 backing。普通2D/2DArray和2DMS/2DMSArray的视图必须保留采样数；MSAA cube、shader write 和多 mip 被拒绝。

颜色 MSL `texture2d_ms_array<float,read>` 支持 CS/VS/FS 的 `read(coord,array,sample)`、width/height/array_size/num_samples 查询。Compute 通过已有受检查的 SSBO image bank 加上 native layer stride 定位样本；query-only 情形也保留 MSAA 与数组类型。Vertex/Fragment 使用真实 `sampler2DMSArray` → IR3 `ISAMM.a`，虚拟绑定168..175在 IR3 前重映射到 native fetch 槽8..15。描述符同时包含 sample count、array pitch 和层数，动态坐标、层和样本越界返回零；不占用公开 sampler 槽。

Metal 桥把颜色、深度和模板的源/目标 slice 统一转换为实际平面偏移，再交给原有绘制或颜色/D32/S8 resolve 内核。视图范围与实际物理区间均受检查；深度/模板仍是 render/resolve/blit-only，不开放 shader sampling。组合格式的跨切片复制先收集、排序深度和模板全部区间，再检查源/目标重叠。clear、复制及 resolve 的访问范围只包含所选切片。一个 pass 仍只能渲染一个指定 slice，`renderTargetArrayLength>1` 的 layered rendering 没有接入。

平台范围需要区分：iOS18 SDK 的 [纹理类型枚举](https://developer.apple.com/documentation/metal/mtltexturetype/type2dmultisamplearray) 含有 iOS14起可用的 type8；[Metal SL规范](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf) 将这类 shader 类型列在 macOS 支持项中，没有将其列为标准 iOS SL 能力。本工程使用自有 MSL→NIR→IR3 编译器和 native 驱动 profile 实现该路径。AIR MSAA/数组 intrinsic 没有已核对签名，继续拒绝；不据此宣称 Apple iOS GPU family 兼容。

Shader31/JIT28 支持同一纹理同时具有 multisampleMask 和 array type bit；cube 组合继续拒绝。结构尺寸保持 Texture48、Dispatch824、Draw2816、description2424/image41832，Apple 外层资源/队列 ABI 不变，旧版协议不能混用。

四个 `Multisample.metal` 内置函数覆盖数组读取、query-only、VS和FS路径。现有编译器核对 AOT/JIT 完整前缀和 ISA；PM4检查覆盖层步长、MSAA描述符、切片及 D32/S8 resolve 计划。CPU llvmpipe 运行原始生成 GLSL，只将 variable local size 改为1×1×1：四组2×/4×、三层 RGBA8/BGRA8 sRGB 读取/平均与两组查询通过，额外XYZ invocation和填充区未被写入。这不执行 A540、Metal方法或原生队列。未新增测试文件，临时验证脚本已删除；真机、系统加载和完整 family 仍未验证。

## 深度纹理读取与比较采样

GPU1.53.0 / Metal0.43.0 将私有 D32、D32S8 的深度平面接入 Compute/Vertex/Fragment 的纹理读取；S8 仍为 render/resolve/blit 平面。支持 `depth2d<float>`、`depth2d_array<float>` 的 read、尺寸查询、sample 和 sample_compare，以及 depth2d_ms / depth2d_ms_array 的逐样本 read/query。MSAA数组沿用自有编译器扩展，不声称符合 iOS Metal family。深度仍只允许单 mip、恒等 swizzle，禁止 shader write、depth cube 和 stencil shader read。

MSL AOT 与设备 JIT 共用类型掩码。Shader32/JIT29 将旧 MSAA 保留 word 定义为 `depthTextureMask`，尺寸保持2424/41832 bytes；内核拒绝格式与程序类型不符、深度写入或 cube/深度组合。Apple 外层 IOGPU 选择器、资源、队列和 fence ABI 保持原值，用户态需配套更新。D32S8 读取仅包含深度平面；数组视图保留独立层步长。IOSurface 导入仍只接受既有颜色格式。

图形 fetch 和所有阶段 sample 使用 A540 `TFMT5_32_FLOAT`（0x4a），Compute read 使用受边界保护的 float32 原始 word。比较采样生成真实 IR3 shadow 指令（如 samgq.s / samgq.a.s），由硬件对每个 texel 比较后过滤；没有先过滤深度再做标量比较。API 与 constexpr sampler 均支持 Never/Less/Equal/LessEqual/Greater/NotEqual/GreaterEqual/Always，编码于语义 filters 的 bits8..10。普通采样与比较采样可共享纹理及逻辑 sampler，GL 前端使用独立虚拟 uniform，在 IR3 前合并到相同硬件槽。

该阶段在单 mip 上用各向同性梯度区分显式 LOD 的 min/mag；1.54.0 已用直接 LOD 指令替换该近似路径，见下节。片元隐式梯度、bias、gradient2d 和 MirrorClamp 梯度反射均进入同一硬件比较路径。比较采样的坐标、层号、参考值与 LOD 以函数参数只求值一次；包含原子副作用的坐标/参考值经真实 IR3 核对为两个加法指令。render 后的深度 CCU flush、UCHE 可见性、原生 backing 别名检查与 fence 保留沿用既有提交后端。

新增 `Depth.metal` 的六个实际 AOT 示例，并纳入现有 AOT/JIT 等价检查与 PM4 编码检查。CPU llvmpipe 运行生产 GLSL，核对四类读取、八种比较函数、PCF 顺序、普通/比较采样共用资源和显式 LOD 的 min/mag 选择；只调整本地工作组声明及虚拟 sampler binding 数字。没有增加测试文件，临时探针已删除。AIR 深度 intrinsic 尚未取得可核对签名，继续拒绝；未执行 iOS Metal 或 A540。接口参照 [Apple MSL](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf) 的 depth2d / depth2d_array 成员函数，以及固定 Mesa fd5_format / fd5_texture / IR3 实现。

## 深度 mip 链和梯度参数

GPU1.54.0 / Metal0.44.0 将私有 D32、S8 和 D32S8 扩展到最多15级线性 mip，实际级数仍受尺寸和16 MiB资源上限约束。颜色、D32、S8 统一调用布局代码：mip 链的基础 pitch 按64个像素对齐，逐级缩小并保持该格式的 pitch 对齐，末级补齐32行，整条链按页对齐。D32S8 先存放所有深度链，再存放所有模板链，两类平面保留独立的数组层步长。

Metal 纹理视图可选择 mip 范围、slice 范围和嵌套子视图；深度/模板只允许相同格式及恒等 swizzle。render、clear、blit 和 resolve 附件均先选择实际 mip/slice，内核再次核对尺寸、范围和 backing 别名，拒绝直接把整条 mip 链当作附件。MSAA 仍限单 mip。D32 shader read/query/sample/compare 可选择 mip；S8 继续用于附件、复制与 resolve。按照 [Apple generateMipmaps](https://developer.apple.com/documentation/metal/mtlblitcommandencoder/generatemipmaps(for:)) 的格式要求，自动生成 mip 仍限可渲染且可过滤的颜色格式，深度 mip 由逐级渲染或复制填充。

比较采样的显式 LOD 和非片元默认 LOD 直接进入 NIR `txl`、IR3 `saml.s / saml.a.s`。私有 GLSL 编译桥在需要比较采样时启用 [EXT_texture_shadow_lod](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_shadow_lod.txt)，供数组 shadow 的 `textureLod` 重载使用；不对系统声明 OpenGL 扩展。此路径保留精确 LOD 和跨 mip 比较后的过滤权重，显式 level 沿用独立 sampler alias 的 clamp 策略。普通 MSL/AIR 的2D/数组 gradient 采样也改用实参只求值一次的辅助函数，坐标、层号和梯度中的副作用不会因 MirrorClamp 变换被重复执行。

`Mipmaps.metal` 新增五个实际 AOT 入口，覆盖动态深度 mip 读取、Compute/Vertex/Fragment 采样和带原子副作用的梯度参数。Shader33/JIT30 拒绝旧版本，线格式尺寸保持不变。沿用现有编译器与提交检查，核对 AOT/JIT 的完整前缀及机器码、D32/S8 非对称布局与嵌套视图，以及 PM4 深度纹理的 mip 字段。CPU llvmpipe 执行生产 GLSL，覆盖四级原始读取、无效 LOD、20组 nearest/linear mip 与比较过滤结果和两次独立原子的单次执行；该结果不代表 A540 或 iOS Metal 已运行。未增加测试文件，未进行真机测试。

## 原生 UI 呈现与合成

GPU1.55.0 / Metal0.45.0 保留系统 `_MTLDevice`、CAMetalLayer/CAMetalDrawable、IOSurface 和 IOGPUFamily 对象，将厂商适配放在 QcomMTL 与 A540 后端中。未实现替代 QuartzCore、CoreAnimation 或 CARenderServer。实际22H374 Metal/QuartzCore 方法、类型编码和调用点见 [原生呈现 ABI](../../drivers/abi-22H374/ui-presentation-abi.json)。

原生 CAMetalLayer 通过公开 `newTextureWithDescriptor:iosurface:plane:` 创建 drawable 纹理；桥接接受该路径的单层线性 RGBA/BGRA8 UNorm/sRGB、零旋转与恒等 swizzle，并保留 descriptor 的 isDrawable/framebufferOnly 标记。usageUnknown 可用于原生 layer 的默认配置。Metal0.81按原版IOGPUMetalTexture将usage与framebufferOnly分开保存，不再强制改写成RenderTarget；对照原版AGX片元绑定，graphics采样不因该私有标志提前失败，仍检查实际usage、所有权、格式、范围及附件反馈。CPU读写与blit限制保留。另补全原生slice:入口的slice0适配。多平面、压缩或受保护表面继续拒绝。原版地址及代码见build/gpu-runtime/ui-render-primitives/apple-texture-binding-audit.json。

命令缓冲区实现公开的三种 presentDrawable，以及原生 presentDrawable:options:；记录时保留真实 drawable 和纹理，检查设备归属并拒绝重复呈现。所有 A540 CP timestamp、Family end token 和 IOSurface cache 发布完成后，才调用真实 drawable 的 present/presentAtTime/presentAfterMinimumDuration/presentWithOptions。呈现时间与回调由系统对象管理，没有合成显示完成事件。目前等待 GPU 完成会增加延迟；尚未接入可供原生显示服务等待的 A540 共享 fence。

普通 MTLEvent 提供同设备的有序等待与 signal。MTLSharedEvent 工厂沿用 `_MTLDevice` 创建的实际 `_MTLSharedEvent : IOSurfaceSharedEvent`，由系统负责 Mach port、handle 与通知；适配只把命令顺序接到真实 GPU 完成及系统事件值。等待期间释放设备 transport lock，其他队列可以完成并 signal，避免相互阻塞。当前采用 CPU 等待并设5秒超时，未实现硬件事件依赖或验证跨进程运行。

过渡入口 `QcomMTLEncodeComposition` 使用已有 Apple Metal library/pipeline/render encoder，记录最多8层的背到前预乘 alpha 混合、opacity、contents crop、仿射四边形和 scissor。CPU 只计算顶点几何与仿射坐标；清背景、纹理采样、光栅化、裁剪和混合进入 A540。背景使用单次 GPU draw，避免全屏逐块 CP fill。输入是同设备 MTLTexture，输出可为 IOSurface 导入纹理或真实 CAMetalDrawable.texture；调用方仍通过标准 presentDrawable/commit 提交。该厂商入口没有遍历 CALayer 树，也未被系统 CARenderServer 自动调用，不作为新的系统合成框架。

三个 `Compositor.metal` AOT 入口加入默认库，现为126个公开入口、57个MSAA子变体；Shader33/JIT30 和外层 Apple IOGPU ABI 不变。非管理员 backboardd 角色允许原生连接和内置 AOT；原始 ISA 注册仍要求管理员。现有严格的任务资源归属、backing 别名、范围与就绪检查继续适用。

统一命令 `scripts/toolchain.py pack-drivers --target SDM835 --collection graphics --kernelcache <原生kernelcache> -j 3` 生成 `build/output/kernelcache.graphics`。该集合继承 display-runtime 和 gpu 的根依赖，保留原生 Apple 匹配与 provider；集合继承由同一个 toolchain 解析，未增加独立构建脚本。GPU Metal 插件仍在 `build/output/gpu-rootfs` 离线暂存，实际安装、信任链和系统加载尚未执行。

本阶段沿用现有编译、ABI/KPI、集合结构及5项集成检查，没有新增测试文件。CPU llvmpipe 执行实际生成的三个 GLSL shader，核对 UNorm/sRGB 两组图像中的预乘混合、opacity、裁剪、旋转四边形和 scissor，最大字节误差为1；该检查由夹具提供几何/PSO 状态，没有运行 Objective-C 合成函数、CAMetalLayer、iOS Metal 或 A540。临时探针已删除，结果见 `build/drivers/gpu-ui/semantics.json`。

仍需接入系统 CoreAnimation 的实际库与着色器要求、验证 CARenderServer 选择设备、打通共享 scanout/显示 fence，再进行目标运行。当前只能确认基础桥接代码与离线构建，不能确认系统 UI 已获得 GPU 加速。

## QuartzCore 原生库与队列

Metal0.46.0 根据实际22H374 QuartzCore MetalContext 的调用补充原生适配。该框架通过 `newLibraryWithURL:error:` 加载自身的 `default.metallib`，该文件是含两个切片的 FAT：一个通用 AIR64/MTLB，一个 Apple GPU MH_EXECUTE。只读提取、原始哈希、切片与方法类型记录见 [原生库 ABI](../../drivers/abi-22H374/coreanimation-library-abi.json)。原始文件仅用于本地分析，没有放入插件或修改系统框架。

`LibraryArchive.h` 由 host AOT reader 和 Metal 进程内 loader 共用，替代原先 Python 与 Objective-C++ 各自实现的薄 MTLB 解析。支持 FAT32/FAT64 的两种字节序、唯一 AIR64 切片和 MTLB v2；核对结构长度、架构重复、切片对齐/重叠、tag、元数据和 AIR 范围，并由各调用方验证每个 AIR 的 SHA256。其他架构的机器码不会交给 LLVM 或 A540。多份 AIR64 切片存在歧义、没有可移植 AIR、越界或损坏时返回编译错误，不能用 AGX 原生指令作为回退。

`newLibraryWithData/URL` 对 MTLB/FAT 先复制并核验数据，保存函数目录；`newFunctionWithName:` 请求某个入口时，才对该 AIR 模块中同名的 stage 函数降低、检查 TYPE、编译和注册。一个库中暂未使用的复杂函数不再阻断整个库加载。共享模块的其他入口不会被错误返回；缺失入口会失败。编译由 device lock 串行化，library 内部按函数缓存注册结果并保留真实程序生命周期。编译失败继续返回 nil/NSError；非管理员的原始 ISA 注册限制保留。`newDefaultLibraryWithBundle:error:` 优先采用 Apple 的 default.metallib，并保留厂商默认库入口。

QuartzCore 在创建命令队列后设置 `setSubmissionQueue:` 和 `setCompletionQueue:`。桥接保留私有 serial wrapper，将其 target 设置为传入的系统 dispatch queue，再激活；目标配置目前须在首个提交前完成。这样调用方传入 concurrent queue 时仍保持每条 Metal queue 的 FIFO。此适配不支持运行中更换 target，也尚未接入原生 GPU priority 的所有变体。

实际 `commitAndWaitUntilSubmitted` 的类型编码是 `B16@0:8`，返回 BOOL；QuartzCore 按该返回值选择待回收资源的处理路径。实现复用同一 FIFO，可在调用线程直接 drain，避免同步调用阻塞其 submission target 后又等待投递到该串行 target 的 block。另一个 executor 已运行时只等待本 buffer 的受理状态；scheduled/completed 仍由真实原生提交和 GPU 完成推进。当前后端直接 drain 时可能等到 GPU 完成，仍有延迟限制；没有据此伪造显示完成或 GPU 时间。

只读检查成功解析了实际 QuartzCore 库的178个函数并验证全部 AIR 哈希，也保留 SDK CoreImage 薄库的既有检查。沿用现有5项集成与编译器检查，仅增加 FAT 边界和选定 AIR 入口的少量断言，没有新测试文件。原生 fixed_vert_lph_spc 的实际编译仍因条件输出接口失败：函数常量特化/static initializer、条件参数/point-size/varying、half 和后续着色器操作仍待实现。函数目录可读取并不代表这些着色器已能运行；未执行 iOS Metal、A540、系统插件加载或 CARenderServer。

## AIR 函数常量与条件接口

GPU1.56.0 / Metal0.47.0 将原生 AIR `air.function_constants` 元数据接入反射和特化，主要服务于实际 QuartzCore 库。使用系统 `MTLFunctionConstantValues` 的索引、名称、range、reset 和 copy 实现；通过已核对的只读快照方法取得值对象及其类型/数据，不访问私有实例变量。未特化的 MTLFunction 返回原生 Metal `MTLFunctionConstant` 对象组成的字典；同步和异步 `newFunctionWithName:constantValues:` 创建独立的实际程序。异步入口在派发前复制名称和值，后续修改原值对象不会改变该次编译。方法与类型编码见 [函数常量 ABI](../../drivers/abi-22H374/function-constants-abi.json)。

支持 bool、8/16/32/64-bit 整数、half/float 及2..4分量的常量载荷；这不等于支持 half shader 运算。常量值按 Metal 的逻辑大小紧密存放，例如 float3 是12 bytes。索引先于名称匹配，类型必须一致，缺少 required 常量会失败。已支持类型范围内的无关常量不会进入有效值缓存键；bool 先归一化，未提供与提供零值保持区别。每个模块最多256项，输入快照最多4096项，索引范围0..65535。当前 MSL 源语法和原始 AIR 文件的预加载路径尚未提供相同的特化入口；此接口面向 MTLB/FAT 中的 AIR。

host AIR 前端与设备 LLVM C API 前端共用 `AIRSpecialization.h`。编译器在独立 LLVM 模块中求值受限静态初始化，支持已实现的纯算术/比较/转换、直接 constant-address-space 全局读写与有界控制流，最多8192条初始化指令。`air.is_function_constant_defined` 与条件谓词按实际值处理，然后运行正常 LLVM 常量传播、控制流简化与死代码清理。这里只改变编译器的 LLVM IR，不执行任意 CPU 初始器或创建 CPU JIT 可执行映射。未实现的初始化操作明确报错。

条件参数/返回成员沿用 AIR 的 `air.function_constant` 描述。false 或 undefined 条件对应的字段从实际接口中排除；先清除不需要的返回计算，再优化，避免这些字段保留无用的纹理/缓冲区读取。主位置输出等必要接口仍须存在，内核继续检查最终资源掩码、范围和程序描述。语义参照 [Metal SL规范](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf) 的函数常量和可选结构成员，以及 [required](https://developer.apple.com/documentation/metal/mtlfunctionconstant/required)。

实际22H374 QuartzCore 的 `fixed_vert_lpf_spc` 已通过 host 和运行时两条编译路径：已生成 A540 image 的变体保留 MVP、纹理矩阵、颜色和两条 varying，资源为 buffers2/3、attributes0/1/3，两条路径的 GLSL 和352个机器码字完全一致。只设置 required 的 fc_generic=false 的另一个变体已通过 host AIR lowering：未提供的可选纹理坐标被排除，仅保留 buffer2 和颜色 varying。前一个变体的 image 通过现有 JIT profile 检查；这验证编译和描述符边界，没有运行 iOS Metal、IOGPU 提交、A540 或显示。原生 half/片元路径、更多 pipeline/function SPI 和完整 CoreAnimation 效果仍未完成。

host 前端对包含函数常量的 AIR 要求显式传入 `--function-constants values.json`。嵌入式 AOT 别名暂不保存特化键，因而拒绝这类隐式编译；原生 Metal 库通过上述运行时路径按实际常量值缓存各个变体。

系统合成服务的动态程序登记使用与连接创建相同的任务鉴权：管理员或真正签名的 `com.apple.backboardd=true`。每次 RegisterProgram/DeleteProgram 都重新核对 owning task；任务程序配额、原生资源/IOSurface 命名空间和每作业隔离不变。此接口仍面向可信任务，未声明普通应用的 ISA 沙箱；策略见 [服务访问记录](ui-service-access.json)。

沿用现有编译器和集成检查，只在原有编译器文件中增加必需常量、可选 buffer 和匹配规则的少量断言，没有新测试文件。实际原生库编译证据位于 `build/drivers/gpu-function-constants`；临时原生编译探针在使用后删除。系统插件加载、CARenderServer 选择和实际 UI 加速仍待验证。


## 原生 half 片元与内存访问

GPU1.57.0 / Metal0.48.0 接入原生 AIR 的 half 标量和2..4分量向量，覆盖 SSA 算术、比较、PHI/select、float16/float32 转换，以及已实现纹理操作的 half 结果和写入。内部使用 Mesa 已有的显式 binary16 类型和 NIR/IR3 指令，转换和运算保留 half 舍入。中间 GLSL 使用的 AMD 扩展只用于私有编译语言，没有向应用宣称 OpenGL 或 AMD 能力；对外仍是 Metal 和原生 IOGPU。

阶段接口继续使用现有 float32 寄存器传输，在 half 参数/返回边界显式转换。half varying 的语义身份附带精度标记，避免与相同名称的 float varying 错误链接。片元颜色允许 float/half 标量和1..4分量向量，经现有 vec4 目标传输；缺失颜色分量填0，alpha 填1。AIR saturate/fast_saturate 和 dfdx/dfdy/fwidth 已接入，half 导数经 float32 运算后收窄。没有扩大设备的完整 Metal family 声明。

half device/constant buffer 使用同一绑定的16-bit SSBO 视图，依据 LLVM DataLayout 计算字节偏移；每个分量单独执行2字节范围、对齐及溢出检查。A5xx buffer 指令发射改用 TYPE_U16 和 half 寄存器，保留对应半字偏移；不会通过32-bit store 覆盖相邻分量。新增2字节 stride，因此 Shader34/JIT31、Kext 和 Metal bundle 必须配套，description2424/image41832/Draw2816 的结构尺寸不变。尚不支持 half threadgroup 存储、16-bit 整数通用运算或 MSL half 源语法。

函数常量分析改用有界迭代遍历，并在入口内共享 visited 集合，支持真实固定片元中的长 SSA 链和循环。静态初始化允许最多4096 bytes 的 constant-address-space 对象，按 LLVM 布局读取和重建结构体、数组、向量字段；越界、padding 和跨字段访问报错，初始化指令总量仍为8192。条件 render-target 元数据按键读取，不再假定标签位于固定位置。未使用 buffer 的 stride 与运行时前端保持一致，最终清零。

原版22H374 QuartzCore 的 `upsample_vert_lph` 和 `upsample_frag_lph` 已通过 host 和生产运行时编译路径；运行时 image 通过现有 JIT profile，普通和 MSAA 的实际内核阶段接口链接均通过。片元保留四次纹理采样、half 权重/颜色运算以及 buffer6 的 half EDR scale 读取。另有 `fixed_vert_lph_spc`、`attachment_clear_frag_lph`、`path_clear_frag_lph` 编译证据。见 [适配范围与产物](native-fragment-profile.json)。这些检查没有执行 iOS Metal、IOGPU 提交或 A540。

Metal0.49.0 进一步修复固定片元的函数常量特化。原版 AIR 含有未启用分支的 NaN/undef 占位值和带 noundef 参数的 helper 调用；LLVM23 在先执行 InstCombine 时，会将尚未清除的路径传播为 poison。共用前端先展开签名已验证的 `air.any/all.v2..4i1` 布尔归约，再依次执行 SCCP、ADCE、InstCombine、SimplifyCFG、ADCE，保留原来的浮点标志、地址空间和元数据，也继续拒绝整个活跃输出为 poison 的程序。没有按 QuartzCore 函数名替换着色器。

原版 `fixed_frag_lph_cph` 的纯色拷贝、纹理拷贝、纹理乘颜色、纯色 source-over、纹理乘颜色 source-over 五种配置已通过生产运行时 JIT 和独立 host 编译。source-over 使用单独绑定的目标快照纹理0，执行 `source + destination * (1-source.a)`；纹理源使用纹理3/采样器0，LOD bias 来自原生 buffer2。常量值与反射、完整程序描述、ISA、immediates 和普通/MSAA 阶段链接证据见 [原生片元适配记录](native-fragment-profile.json)。配置仅用于验证原版 AIR；实际 Metal 调用仍由原生 `MTLFunctionConstantValues` 提供常量并建立缓存变体，驱动没有强制设置这些配置。

本次没有改变 Shader34/JIT31 协议或 GPU1.57.0 Kext。framebuffer fetch、R8/mask 等常用原生格式、更多 pipeline/function SPI、显示共享 scanout/fence 和系统加载仍待接入；上述检查没有执行 iOS Metal、IOGPU 或 A540，不能据此宣称系统 UI 已使用 GPU。


## 原生附件读取与GPU快照

GPU1.58.0 / Metal0.50.0 将 Metal `[[color(n)]]` 片元输入和 AIR 的 `air.render_target` 元数据接入同一编译路径。AIR 接受 float/half 标量及2..4分量向量；MSL 源 profile 接受 float 标量/向量的直接参数及 stage_in 结构字段。附件通过现有 MTLRenderPipelineDescriptor、render encoder 和 IOGPU 资源描述传入。原生固定片元允许两个参数引用同一合法位置输入，均按已核对的 float4、center/no-perspective 元数据映射。

A540 当前使用 sysmem BYPASS 渲染。固定 Mesa 的 framebuffer-fetch 能力只在 A6xx 路径开放，因此这里用实际 GPU Compute 程序在每个输入三角形前复制所需附件，再以原生颜色格式和 sample 布局读取当前像素。按图元复制保留同一 draw 内重叠图元的 source-over 顺序；TriangleStrip 的重启索引、奇偶 winding 和实例顺序沿用实际输入。普通、2×/4× MSAA 共用一个快照描述符，按 native sample count 选择读取，保持普通 varying 的中心插值。

应用的 Metal 纹理和采样器绑定继续沿用现有规则。内部快照在16个硬件纹理描述符的空闲位置分配，普通读取和 MSAA 读取共用该位置；无空位时返回错误。Program 原保留字改为 framebufferMask，Shader35/JIT32 要求 Metal、Kext 与工具配套，description2424/image41832/Draw2816/Dispatch824 bytes 不变。原有纹理/query 常量偏移不变，附件信息追加在48..79字。

快照由作业私有 IOBufferMemoryDescriptor 承载，前4 KiB 为独立 flush timestamp 页，各附件随后按页对齐。单个分配仍限16 MiB，全部未完成作业的快照配额为64 MiB；跟随原生作业隔离、完成及错误回收。颜色/深度和 UCHE 的可见性通过既有 GPU flush 加递增 timestamp 等待衔接，复制的是原始附件数据，像素混合仍由原版片元执行。没有 CPU 像素复制路径。

原版 QuartzCore `fixed_frag_lph_cph` 的两种 framebuffer source-over 配置和 `path_blit_frag_lph` 已通过生产运行时 JIT、独立 host 编译、完整描述/ISA/immediates 对比，以及1×/2×/4×实际内核 PipelineEncoder 编码。path_blit 的离线编码使用 RGBA8 的红通道验证标量输入，R8 mask 格式仍待实现。当前共有128个静态入口、58个 MSAA 变体；沿用现有检查文件，未增加测试文件或持久脚本。接口、内存配额和证据路径见 [附件读取记录](../../drivers/abi-22H374/framebuffer-fetch-abi.json)。

逐图元全附件复制与状态上传代价较高，64 KiB 私有 IB 满时会在投递前返回 NoResources，大 draw 分批尚待实现。通用 buffer/texture raster-order groups、imageblocks、R8 等原生格式、系统 pipeline SPI 与显示共享 fence/scanout 仍待接入。上述结果不代表已执行 iOS Metal、IOGPU 或 A540，也不能确认系统 UI 已获得 GPU 加速。


## 原生 R8/RG8 mask 格式

GPU1.59.0 / Metal0.51.0 接入系统 `MTLPixelFormatR8Unorm` 和 `MTLPixelFormatRG8Unorm`，分别使用1/2 bytes 每像素，缺失分量为 `(r,0,0,1)` / `(r,g,0,1)`。普通及越界 read 都按指定分量与缺失分量区分，再应用视图 swizzle。没有把 mask 扩展成四字节伪纹理；A540 纹理和颜色目标分别使用实际的0x03/0x0f格式。

分配、CPU region 读写、纹理视图、blit、Render clear、采样、mipmap 和2×/4× MSAA 共用格式对应的字节步长。mip pitch 按64像素对齐，再按基础 pitch 缩小；硬件 PITCHALIGN 根据每像素字节数和 sample count 设置。视图须保留相同元素大小，拒绝 R8/RG8 与 RGBA8 间的错误存储重解释。clear 将 red 或 red/green 重复成既有 GPU fill pattern，片元写入、混合及附件快照仍走 A540 后端。

Compute 图像的 packed 像素共用32-bit字。生成的受保护 SSBO 访问按 byte offset 计算 word/shift，写入通过 CAS 只更新对应8/16-bit字段，避免相邻 invocation 丢失其他像素，并保留末字 padding。现有 RGBA8 保留直接32-bit写入。共享 MSL/AIR lowering 同时覆盖 mip、数组、cube/cube-array 和 multisample 读取/resolve；该内部 CAS 不向应用增加纹理原子接口或新的 Metal family 声明。

`newTextureWithDescriptor:iosurface:plane:` 接受原生 CoreVideo FourCC `L008` / `2C08` 的非平面 IOSurface。仍由 IOGPUFamily 按任务授权取得实际表面，核对 bytesPerElement、尺寸、rowBytes、保护/tiling和分配范围；单独的64-byte厂商 metadata 升至2。Metal drawable 保持既有 RGBA/BGRA8 范围。Shader36/JIT33 需与插件和Kext配套，现有 Texture48/description2424/image41832/Draw2816/Dispatch824 结构尺寸不变。

原版 QuartzCore `path_blit_frag_lph` 的 scalar color2 输入/输出已使用实际 R8、RG8 附件描述进入内核 PipelineEncoder；连同两种 source-over、三种格式、普通与2×/4× MSAA，共27组编码检查。原生 AIR 通过生产 JIT 和独立 host 的完整描述/ISA/immediates 对比。CPU llvmpipe 执行生产生成的 Compute GLSL，核对16组格式转换、缺失分量、越界、swizzle、mip、数组/cube、MSAA与MSAA数组resolve、硬件过滤mipmap；除 mipmap 量化最大1byte 外结果逐字节一致，其他层/级及 padding 保持原值。仅调整 CPU 不支持的 variable local-size 声明，没有运行 iOS Metal、IOGPU 或 A540。临时探针已删除，沿用现有编译/提交与集成检查文件，未新增测试文件或独立脚本。详见 [格式与证据](../../drivers/abi-22H374/mask-formats-abi.json)。

更宽的原生颜色格式、通用 raster-order groups/imageblocks、大 draw 分批、剩余 pipeline SPI 和显示共享 scanout/fence 仍待完成；系统 UI 加速尚未验证。


## 原生大 draw 分块提交

GPU1.60.0 / Metal0.52.0 在 A540 后端内部将附件读取 draw 编码为多个64 KiB IB。 提交参数一次复制到有界 OSData，图元展开与光栅状态发射分离，避免大型参数数组和递归 draw 叠加内核栈。仍是一个 Metal draw 和原生 IOGPU 作业；所有图元、实例、strip 重启及 winding 保持原顺序。编码空间不足时，恢复当前图元之前的命令头、上传数据尾和 footer 预留空间，再在新 IB 重试该图元，不会保留半个图元或提前执行已有部分。

IB 由 Apple OSArray/OSData 和既有 IOBufferMemoryDescriptor 管理，每个作业最多256个、共16 MiB，所有未完成附件读取作业的 IB 配额为64 MiB；附件快照另沿用原配额。全部 IB 在同一私有页表中只读映射，验证和分配成功后，一次写入 ring 中的同级 CP_INDIRECT_BUFFER_PFE 列表。页表只进入/退出一次，最终 kernel-tree timestamp 完成后才退休原生 fence/end token、发布 IOSurface 写入并回收全部对象。失败时不提交前半段；无法停机或退休页表时继续隔离并保留 DMA 内存。

跨 IB 保留递增 flush 序号，防止旧 timestamp 提前满足新等待；每个 IB 的 border-color 地址重新分配。监控只在作业进入 FIFO 队首后计时，私有 scratch 页中的真实 CP flush 序号推进表示仍有进展；倒退/越界会故障，连续500 ms无进展触发原有停机恢复。中间进展不会触发 Metal 完成回调或原生 end token。

沿用现有提交检查，核对384个图元跨 IB 的 copy/draw、实例参数、代码地址及 flush 顺序；容量拒绝不投递部分作业。现有 MMIO/页表 fabric 也核对多个 IB 只使用一对私有/内核页表切换，并在两段 DMA 后产生一次硬件式 timestamp。原版 QuartzCore 的 source-over 和标量 mask 另通过生产 JIT 与实际 PipelineEncoder 的小 draw 和大 draw 编码检查。接口和证据见 [分块记录](../../drivers/abi-22H374/draw-batches-abi.json)。未增加测试文件或独立脚本，没有运行 iOS Metal、IOGPU 或 A540。

单个图元及其快照仍须能放入一个64 KiB IB；逐图元全附件复制和完整状态上传的性能代价仍待优化。当前更大的提交容量不等于已验证系统 UI 加速。


## 原生 Function Descriptor 与管线创建

Metal0.53.0 直接接受原版 MTLFunctionDescriptor，通过 name 与 constantValues 选择已有 AIR/JIT 编译入口。specializedName 只改变返回函数的名称；相同有效常量仍复用已注册的程序，不修改共享程序或其他函数对象。描述符没有提供必需常量时明确失败。原版二进制 archive 只作为可选查找提示，普通请求可回退编译原始 AIR；要求命中 archive 时返回缺失错误，不能把 Apple GPU 机器码当成 A540 程序。

Render 与 Compute 补齐 options/reflection 形式及异步 completionHandler 形式，Compute 另接受原版 MTLComputePipelineDescriptor。异步任务在排队前复制描述符，回调报告 CPU 管线创建结果；GPU 完成仍由原有 IOGPU fence 决定。线程上限保存在 Compute 管线对象内，不修改共享函数；记录 dispatch 时验证执行宽度倍数承诺。动态/预加载函数、额外二进制函数、stage-in Compute、间接命令、顶点放大等尚未支持的描述符功能会明确拒绝。

BindingInfo/BufferTypeInfo 请求仍返回 Unsupported，reflection 输出初始化为 nil；没有替换 Apple 的反射类。原版 QuartzCore 的已检查 archive 管线查找调用带 options4 并传空 reflection 指针，这一路径缺失时返回明确错误，由调用方选择普通 AIR 编译路径。接口契约及离线证据见 [原生管线记录](../../drivers/abi-22H374/native-pipelines-abi.json)。尚未执行 iOS Metal 对象、系统合成或真机 GPU。

## 批量拷贝与填充

GPU1.66.0 / Metal0.56.0 保留 Apple MTLBlitCommandEncoder 和原生 IOGPU 提交接口，改进其 A540 后端。共享的 GPUBlit.h 同时限制64条原生命令和16,384 DWORD的私有 IB，预留最终两字屏障。A5xx Copy32 每复制4 bytes 需要6个 PM4 字，Fill32 每批需要3个头/地址字加数据；不会把64条大命令直接塞入一个 IB。小块及带行跨度的拷贝可共用一个原生结束事件。CPU 读取边缘字前必先退休已有批次，原有未覆盖字节保持不变。

至少16 KiB的对齐填充，以及总数据量达到该阈值、宽度/偏移/跨度均四字节对齐的拷贝，使用新 AOT 入口 blit_fill_words / blit_copy_words。每个 invocation 只写一个目标字，带跨度的拷贝按有效宽度寻址，保留行间 padding。非整工作组尾部使用已有原生 nonuniform dispatch；16 MiB极限分配改用128线程工作组，避免超过65,535组的限制。Render loadActionClear 也复用这条大块填充路径。着色器由现有 MSL 前端和固定 Mesa IR3 编译，继续使用已受保护的 SSBO 访问及真实 CP fence。

每个队列按需保留一页参数资源，等待实际 end token 后才复用；完成状态不确定时，队列停止后续提交和参数覆写。计算拷贝的原生准入检查实际 backing pages，拒绝两个独立 IOSurface 导入造成的重叠。该检查仅识别不可变的内置程序，其他 JIT 的普通读写语义保持原样。需要 CPU 边缘处理而没有可读映射时明确返回错误。

连续1 MiB拷贝的规划从1024次提交/等待变为一次原生作业；相同大小填充从128次变为一次。这里描述的是源码规划数量，没有测量新版本 A540 耗时或系统帧率。现有 metalctl blit-test 的16 KiB私有填充、字节边缘修改和共享回读覆盖新路径；surface-test 覆盖带 padding 的 IOSurface 拷贝。此次只扩充现有 gpu-submit 检查，不增加测试文件或独立脚本。

静态入口由128增至130，registry 为 c1b3c3d8bc09d4a6；Shader36/JIT33的线格式不变。Kext、Metal插件和默认着色器库必须配套构建。新版本未在设备运行，系统 Metal 加载、UI加速和动画流畅度仍待验证。

## 字节拷贝与参数冻结

GPU1.67.0 / Metal0.57.0 新增 blit_copy_bytes / blit_fill_bytes。字节级偏移继续通过原版 MTLBlitCommandEncoder 传入，符合 [Apple 的 iOS 拷贝接口](https://developer.apple.com/documentation/metal/mtlblitcommandencoder/copy%28from%3Asourceoffset%3Ato%3Adestinationoffset%3Asize%3A%29)。blit 不再读取源或目标的 CPU 像素映射；私有 IOSurface 的非对齐区域也走 GPU。大块对齐拷贝保留 word kernel，普通小块对齐分配保留已有 CP 批次；所有 IOSurface 拷贝均交给携带完整布局的计算提交，以便检查不同导入对象的实际物理重叠。

字节 kernel 在整个目标范围内按32-bit字分配线程。完整字可通过移位合并两个源字；边缘按实际行宽和跨度生成 byte mask，再由唯一线程保留同字中的其他字节。即使多行各只有一两个字节、相邻行共用一个目标字，也不会产生多个线程竞争写回。线程跳过行间 padding；源尾部与目标边缘只访问已有分配中的完整字，不跨出绑定范围。非对齐1 MiB拷贝原先可能执行262,144笔逐字提交，现在规划为一笔原生作业；这是提交数量，不是测得的加速比。

共享的 GPUMemoryRanges.h 移至驱动公共 include。Metal 记录拷贝时按实际 strided ranges 检查已知别名，允许互不重叠的交错行；内核针对两个内置拷贝入口检查实际 backing pages 对应的逻辑字节范围，不再仅比较整个包围区间。由此保留同一物理字内互不重叠的字节拷贝，以及同一分配中互不重叠的行。

原生准入读取并校验拷贝及字节填充的小型参数，核对偏移、宽度、行数、跨度、绑定大小与 dispatch 范围，再把参数冻结到作业私有 IB 的只读数据尾。GPU 使用这份已验证的布局；提交后修改客户端参数页不能改变所检查的范围。其余 JIT 继续使用原来的普通 buffer 语义。队列参数资源仍只在真实 end token 成功后复用，完成不确定时停止后续提交。

当前132个 AOT 入口的 registry 为 ecb2551f170a2bb0。Shader36/JIT33和原生 ShaderDispatch 线格式保持不变，内部参数扩为32 bytes，Kext、插件和默认库需配套。现有 gpu-submit 核对规划、原生 PM4/PipelineEncoder 及范围边界；一次无持久脚本的 CPU llvmpipe 检查执行生产前端生成的 GLSL，29组偏移、窄行、同字/交错行别名和填充均逐字节一致，包含源内容、目标边缘、行间 padding 和尾部保护区。CPU仅将 variable-local 声明改为固定64线程，没有运行 A540、iOS Metal 或 IOGPU。证据见 build/gpu-runtime/byte-blit-cpu-check.json 与 byte-blit-offline.json；系统 UI 加速、动画和单笔提交延迟仍待真机验证。

## Metal fence 与编码器同步

Metal0.58.0 实现 MTLDevice.newFence，返回遵循原版 MTLFence 的设备/label 对象，补齐 Blit、Compute 和 Render 的 update/wait 接口。只接受同一设备的 fence；Render 支持 vertex/fragment。按原版 iOS18 SDK，等待放在 encoder 的 load actions 之前，更新放在 draw 和 resolve 之后；同一 encoder 在更新之后再等待同一 fence 会报错。buffer/texture memory barrier 核验 scope、资源归属和阶段，再投递真实的原生 CP Barrier；iOS 不接受 macOS 独有的 RenderTargets scope。

生产者须在消费者之前提交。设备提交锁为每次更新建立独立完成点，等待快照保留所捕获的多个生产者；后一次更新完成不能放行仍依赖前一次更新的消费者。新更新会清理已退休的历史点，原有等待快照保持不变。完成点成功必须携带真实 native queue ID 和已退休 end token；生产者失败会唤醒消费者并传播错误，不能伪造成功。语义参考 [Apple MTLFence](https://developer.apple.com/documentation/metal/mtlfence) 与 [Resource Heaps 中的 fence 规则](https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/ResourceHeaps/ResourceHeaps.html)。这些点由用户态调度器追踪真实 GPU 退休，尚未接硬件并发等待指令。

等待中的 command buffer 保留在其 FIFO 队首，并让出设备 transport lock 与提交 dispatch target；完成回调重新调度同一缓冲区，后续缓冲区不能越过它。MTLEvent/原生 MTLSharedEvent 的等待放到有界后台任务，仍保留原来的5秒错误路径，不占住生产者可能共用的串行提交目标。提交目标配置在任何 commit 后均不可更改。

原生 commitAndWaitUntilSubmitted 不再同步排空整笔 command buffer。它为已进入 ready FIFO 的前缀依次投递无用户资源的真实 Barrier 标记，取得原生准入/结束事件后返回；未提交的 enqueue 保留位置，资源操作仍按 FIFO 执行。这让调用者可以从 QuartzCore 自己的提交目标返回，随后执行等待中的生产者。标记会计入 Native/Compositor 作业总数；仅看到计数上升不能证明系统像素渲染已经使用 GPU，仍需实际 draw/compute、IOSurface/scanout 和动画证据。

共享 MetalSync.h 的完成点和依赖判断沿用现有 gpu-submit 检查，核对禁止无退休凭据的成功、多个生产者、失败传播及 iOS scope/stage 范围；没有新增测试文件或独立脚本。现有 metalctl 新增一个可选 fence-test：两个队列共用串行目标，涵盖 Blit/Compute/Render clear、fence 复用、未来事件唤醒和实际 GPU 数据读回。该命令本轮未运行，不能将交叉编译或主机状态机检查视作它已通过。离线构建记录为 build/gpu-runtime/metal-sync-offline.json。

该同步阶段使用 GPU Kext1.67.0；后续1.68的内核缓存及统计见 [提交缓存与统计](hardware-bringup.md#提交缓存与统计)。132个着色器及 ecb2551f170a2bb0 registry 不变。新同步路径、系统 Metal 加载、UI GPU 合成和动画性能均待真机验证；原有数百毫秒单笔提交延迟也没有新的硬件测量。


## 有界异步提交

GPU1.69 / Metal0.59 在一个设备的队列间共享16笔待完成预算，每个队列以实际 end token 跟踪最多16笔原生作业。命令 shmem 在内核准入返回后已形成不可变 IB，用户态可继续写下一笔；原有逐作业 waitCompletion 和丢弃通知的 drainNotifications 均退出 Metal 执行路径。独立通知消费者使用原版40字节 Family IODataQueue，允许后续 start 先到，但只有时间有效、与队首匹配的 end 记录才能退休；错误、重复或越序 end 均失败。2秒未观察到队首退休进展会报错，时间仍为 CPU 记账而非校准 GPU 时钟。

达到窗口上限时，提交目标暂停队首并让出 device transport lock；真实完成回调释放预算后重试。原生预检查对全局 FIFO、CPU 索引读取、附件快照额度及内部 stamp 换代的排空条件做背压，在 reserveTokens 与原版 Family fence 之前返回 IOKit vendor-specific code0x100（0xe3ff8100），不消耗 token，也不制造失败 fence。用户态只对这个已声明的状态作有界异步重试；其他提交失败不重试、不重放。外部客户端的背压使用2ms独立唤醒、连续5秒未能准入则报资源不足。注册表公布 IOGPUNativeAdmissionBackpressureVersion=1、IOGPUNativeMaxPendingJobs=16 与 IOGPUNativeImmutableBlitParameters；插件要求这些能力，避免与旧内核混用。GPUNativeAdmissionDeferred 记录预检查延后次数，GPUStatistics 的准入统计仍只覆盖实际进入 submitNative 的调用。

内核现在也冻结 blit_fill_words 的 uint4，四个不可变内建 blit 的全部布局均在作业只读 IB 内；内部32字节参数页只需等准入返回即可复用。CPU 索引快照前先排空设备自身的已有作业，内核仍检查其他客户端的物理别名。附件读取的快照/多 IB 作业按现有64 MiB预算串行准入，普通非附件读取的 draw/compute/blit 可继续排队；没有取消平台、DMA、页表或缓存检查。

普通缓冲区全部准入后转入单独的退休 FIFO，保留操作及资源直至其原生队列前缀真实退休，再发 completed 回调。已提前收到 end 的缓冲区和失败缓冲区也进入同一 FIFO，不能越过前一个缓冲区。CPU fence 更新、事件信号及 drawable present 都保留真实退休边界。分批 CP blit 仅在一批已接受后推进行/字节位置，容量唤醒不重新执行已投递的批次。带 drawable 的缓冲区在原版 present 调用结束前阻挡同队列后续像素提交；尚无硬件 display fence 导入，因此没有提前展示或扫描完成声明。

私有 commitAndWaitUntilSubmitted 可准入资源为空的原生标记，并在压力下让出锁等独立回调，返回只说明私有标记已接受。标记不把未满足 fence/事件依赖的缓冲区改为公开 Scheduled；真正开始其正常命令或所有依赖解决后才推进该状态。公开 waitUntilScheduled/waitUntilCompleted 也等待已注册处理块的投递完成。顺序与生命周期参考 [Apple 命令组织模型](https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Cmd-Submiss/Cmd-Submiss.html)；GPU 硬件队列仍由现有单 ring 后端执行，异步准入不代表多引擎并行。

现有 gpu-submit 只补充生产通知窗口、反向/越序记录及背压条件的主机检查。现有 metalctl blit-test 加入32批窄行 CP 拷贝和真实像素读回；fence-test 加入私有准入不伪造 Scheduled、同队列完成处理块顺序检查。它们均未在设备执行。无新增测试文件或独立脚本；构建记录为 build/gpu-runtime/async-submit-offline.json。系统 Metal 发现、真实渲染/合成、故障/超时恢复和动画流畅度仍待真机验证。


## 原生资源预算

GPU1.70 / Metal0.60 的原生 IOGPU 连接使用独立的1024资源命名空间和64资源作业集合，表面写标记为64位。Metal从版本化 IORegistry 数据读取实际窗口对应的资源与工作集上限；当前256 MiB离线窗口策略为128 MiB连接、192 MiB全局预算，单资源至多16 MiB。实际分配仍可能受映射空间及缓存生命周期限制。详见 [原生资源规模与计账](native-iogpu.md#原生资源规模与计账)。

原生资源、IOSurface缓存根及在途作业保持Apple Family生命周期，重复导入的共享backing在私有页表只映射一次。独立内存快照和原有时间统计共用gpuctl入口；未增加新的selector。现有 native-async-test 扩为64资源回读，尚未执行；没有新的系统Metal发现、UI合成或性能数据。


### Buffer 纹理视图

Metal0.61 实现 Apple `MTLBuffer.newTextureWithDescriptor:offset:bytesPerRow:` 和 `MTLDevice.minimumLinearTextureAlignmentForPixelFormat:`，使用原版 descriptor、buffer/texture 协议与签名。支持现有 RGBA/BGRA8 UNorm/sRGB、R8/RG8 UNorm 的线性2D单mip/layer/sample视图；offset和bytesPerRow均按64字节对齐，行跨度至少容纳一行，整幅行跨度范围必须位于逻辑 buffer.length 内。布局仍通过既有 A540 textureLayout 校验，不能借16 KiB分配取整读取buffer声明范围以外的内容。深度、模板、压缩格式、数组、MSAA、mip链、drawable和texture-buffer类型未接入该入口。

视图不创建第二份 IOGPU resource、DMA映射或像素副本。它持有原 buffer，并把其资源ID、显式偏移和pitch送入既有 blit、Compute、采样和渲染路径；shared资源的CPU纹理读写也指向同一存储，private资源继续隐藏CPU地址。storageMode须与来源buffer一致，CPU缓存与hazard属性继承buffer。CPU访问仍由调用者在命令边界同步；同pass内读写别名继续受既有验证限制。

直接创建的视图返回来源 buffer、bufferOffset、bufferBytesPerRow；由它创建的纹理子视图返回 parentTexture，按既有格式/等元素大小与swizzle规则处理。rootResource追溯到原buffer，子视图保留父对象，作业继续保留实际 backing 与原生完成点。现有 blit-test 增加非零offset、13×5像素、128字节行跨度、共享/私有buffer、视图身份、GPU拷贝、Compute改色、采样渲染和完整前后/行间guard回读；未增加独立探针命令。该探针尚未执行，不能据离线构建认定GPU输出正确或UI已加速。

接口依据 [Apple的buffer纹理文档](https://developer.apple.com/documentation/metal/mtlbuffer/maketexture(descriptor:offset:bytesperrow:))及本地iPhoneOS18.6 SDK的MTLBuffer/MTLTexture/MTLDevice声明。完整驱动的剩余格式、同步、语言与系统集成范围仍见本文和README；无Metal family兼容性声明。


### 原生资源heap

Metal0.62 使用原版 MTLHeapDescriptor、MTLHeap/MTLAllocation 和 MTLResource 协议实现 automatic/placement heap，提供设备端 buffer/texture size-and-align 查询及 heap 的 size、allocatedSize、currentAllocatedSize、usedSize、maxAvailableSizeWithAlignment。支持 shared/private，heap按既有原生页大小取整并只创建一份IOGPU buffer；单heap目前受已发布 maxBufferLength 限制，至多16 MiB。子分配按64字节对齐，缓冲区逻辑长度不因取整扩大。纹理大小查询和真正创建共用同一布局初始化，覆盖现有颜色、深度/模板、mip、数组、cube及MSAA存储；创建时仍检查原有格式与用途限制。

自动heap按空闲区间分配。显式 makeAliasable 或实际资源销毁后，该区间才重新可用；每次分配使用不复用的token，旧alias对象随后销毁不会释放新占用者。placement入口允许显式重叠，每个对象保留自己的区间记录；usedSize描述活跃区间的并集，maxAvailableSize表示可放置的连续长度。资源、纹理子视图和编码命令保留allocation/heap/backing引用，原生作业保留真实IOGPU资源直到完成。makeAliasable不等待GPU，调用者须使用已有fence/event保证复用次序。buffer创建的纹理视图调用makeAliasable不释放buffer分配，纹理创建的子视图调用则拒绝；isAliasable沿用基资源状态。

公开buffer偏移和长度继续相对各自子资源。blit/fill、Compute、索引、Vertex/Fragment buffer绑定统一转成原生backing偏移；texture plane保存backing内偏移，嵌套mip/slice视图不重复叠加基址。CPU contents/gpuAddress指向子分配起点，CPU纹理访问使用原生基址加plane偏移。所有已有逻辑长度检查、物理别名、私有页表与原生退休约束继续生效。同heap的不同对象按共同资源ID识别别名；合成辅助入口允许同backing内互不重叠的纹理，继续拒绝采样与输出重叠。

storage/cache模式须符合heap；默认heap hazard模式为untracked，资源默认继承heap，tracked heap允许资源显式退出，untracked heap不能让子资源单独启用tracked。heap/资源属性报告实际分配关系及placement偏移，不把资源对象当成额外原生分配。Sparse、memoryless、volatile/empty purgeability及超过当前单backing上限的heap尚未实现，相关范围不据此声明完整Metal family。

现有gpu-submit增加真实分配器的碎片、空洞复用、token代际、指定位置重叠和边界检查。现有metalctl blit-test复用buffer纹理负载，在placement heap上覆盖共享/私有buffer、17×5纹理、非零vertex绑定、BGRA视图Compute、heap纹理渲染和整个私有backing的guard回读；另核对automatic heap的alias复用及旧对象销毁。这些渲染guard检查使用loadActionDontCare；现有Render load-clear仍填充整个pitch跨度，需要继续补上要求保留行间padding的buffer纹理别名清屏边界。没有新增独立测试命令；目标探针尚未执行。构建和镜像核对记录为 build/gpu-runtime/heaps-offline.json，系统Metal加载和UI GPU加速仍待真机验证。

接口和生命周期依据 [Apple Resource Heaps](https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/ResourceHeaps/ResourceHeaps.html)及本地iPhoneOS18.6 SDK的MTLHeap/MTLResource声明。


### 按像素行清屏

GPU1.71 / Metal0.63 修正Render load-clear填充整个pitch跨度的问题。每个附件按实际plane.width × formatBytes × sampleCount写入，rows为所选mip/slice的高度，保留原pitch和heap/IOSurface backing偏移。R8/RG8重复各自的1/2字节像素值，RGBA/BGRA及D32按32位模式重复；既有sRGB编码、颜色通道顺序、depth/stencil plane和MSAA布局继续使用原有转换。操作仍在原版Render encoder的load阶段记录，只执行GPU填充，没有CPU边缘像素读写。

当前133个AOT入口的registry为a74b60151e63e259。新增blit_fill_rows AOT入口及32字节参数：widthBytes、rows、strideBytes、binding内bias、pattern及3个零保留字段。每行从pattern的低字节开始重复。word对齐的行只为有效字分派线程，末字掩码保留padding；非对齐行由每个线程独占一个全局目标字，一次合并全部有效lane，避免窄行共用同一字时的竞争。原有四个blit入口及其布局未改变。内核通过共享planner复核规范字段、绑定跨度和grid，再把参数冻结进作业只读IB，保留真实结束通知、页表和故障隔离。

IOGPUNativeBlitLayoutVersion为2，Metal同时要求该能力和配套新registry。Shader36/JIT33、Surface metadata2及IOGPU命令线格式不变，新增AOT入口后Kext、插件和默认库需一起打包。现有gpu-submit核对aligned/packed/跨字行、边界、canonical参数和保留字段；现有metalctl blit-test增加RGBA buffer/heap纹理的clear-only回读，并核对窄R8/RG8的整块缓冲区前后、行间和末字guard。目标探针尚未执行，仍需验证A540输出、系统UI和动画；构建记录为 build/gpu-runtime/strided-clear-offline.json。


### 命令缓冲的资源引用

Metal0.64 接入原版 `MTLCommandQueue.commandBufferWithUnretainedReferences` 和 `commandBufferWithDescriptor:` 的 `retainedReferences=NO`。普通 `commandBuffer` 和 descriptor 的默认强引用模式继续保留已编码的资源，直到真实原生队列退休。三种创建入口报告实际 retainedReferences；descriptor 的 errorOptions/logState 仍不支持。接口和调用方责任依据 [Apple commandBufferWithUnretainedReferences](https://developer.apple.com/documentation/metal/mtlcommandqueue/makecommandbufferwithunretainedreferences()) 及本地 iPhoneOS18.6 的 MTLCommandQueue/MTLCommandBuffer 声明。

无保留模式覆盖 Blit 的源/目标及纹理视图、Compute 的 buffer/texture/pipeline/sampler、Render 的附件/resolve/顶点/片元绑定，以及 fence、event 和资源 barrier。外部对象使用归零弱引用，不把原生资源 ID 当成生命周期依据；即使旧 ID 随后被新分配复用，已失效的编码也不能指向新对象。纹理 blit 在 copyRows 记录后补充的视图引用沿用同一模式。Compute/Render 编码器在 endEncoding 清理绑定，调用方继续保留 encoder 对象不会让它持有旧资源。普通模式的实际操作仍持有其所需对象。

commit 及每次准入/背压重试前核对弱引用；缺失对象报告 `MTLCommandBufferErrorInvalidResource`，不能把失效的 copy 源误当成 fill，也不能跳过已经失效的 fence/event。CPU 检查的临时强引用在命令条件锁、队列锁和 transport lock 释放后销毁，包含无效操作中仍存活的部分引用，避免最后一次释放重入原生资源回收而死锁。准入后的内核作业沿用原生资源、DMA 映射和真实结束 token 的生命周期，不靠用户态对象维持硬件安全。调用方仍须按 Apple 契约持有外部资源直至命令完成；提前释放不受支持，弱引用检查也不是对所有并发误用的保证。

`setBytes` 的 Compute/Vertex/Fragment 快照、depth/stencil resolve 参数，以及合成辅助入口内部创建的 pipeline/geometry/sampler 由驱动自行持有，真实退休后释放；调用方没有这些内部对象的持有入口。合成输入/输出仍遵循命令缓冲的外部引用模式。drawable 的显式 presentation 记录仍保留原版 drawable 及纹理，待真实退休后调用原版 present。此次没有实现 volatile/empty 内存回收，也没有改变队列容量耗尽时当前返回 nil 的 bring-up 限制；不能据此声明完整 Metal 兼容性或降低了 CPU 开销。

现有 metalctl blit-test 增加普通引用保活及两个无保留入口的失效拒绝检查；原有 placement-heap 的 GPU 拷贝、clear、Compute/setBytes、纹理采样和整块 guard 回读改用无保留入口，调用方明确持有其资源。探针仅交叉编译，尚未执行。未增加独立测试命令、测试文件或临时脚本。GPU1.71、Shader36/JIT33、133个 AOT 入口和 a74b60151e63e259 registry 不变，本轮只更新用户态插件与诊断工具；记录为 build/gpu-runtime/references-offline.json。系统 UI 加速和动画流畅度仍待真机验证。


### 原生二进制归档

Metal0.65 接入原版 `MTLBinaryArchiveDescriptor`、`MTLBinaryArchive` 和 `MTLDevice.newBinaryArchiveWithDescriptor:error:`。支持将已验证 Compute/Render descriptor 引用的函数加入归档、重复添加、线程安全快照、serializeToURL:error: 的本地原子写入，以及从本地 URL 重新载入。管线及函数 descriptor 的 binaryArchives 使用这些同设备对象；FailOnBinaryArchiveMiss 只有实际命中才成功，包括空列表在内的未命中都会报错，普通未命中走原有创建路径。接口依据 [Apple MTLBinaryArchive](https://developer.apple.com/documentation/metal/mtlbinaryarchive) 和本地 iPhoneOS18.6 SDK 声明。

归档使用有界的 A540 私有容器 QMTLARC1，112字节头，至多256函数、1024管线及16 MiB；一次最多搜索16个归档。完整文件先归零 digest 字段再计算 SHA-256，读取长度本身受限，不仅检查事先查询的文件大小。头中 GPU540、registry、Shader/JIT 版本必须匹配；统一构建另生成 GPUArchiveBuild.h，把适配器、编译前端源码、实际 IR3 编译器 dylib、AOT 头、LLVM SDK ABI、实际构建函数与 Clang 版本的指纹与运行系统版本/build组合，源码或系统更新后旧缓存失效。先验证完整校验和、字段、边界、重复 key、函数阶段及管线引用图，再允许任何原生程序注册。校验和用于检测损坏，既有 JIT 权限和 ISA 隔离范围未扩大。

内建函数只保存当前 AOT catalog 的静态引用，重新核对 name/stage/id 与 registry；不把内建 blit 重新注册成 JIT，保持内核只读参数冻结及范围检查。动态函数保存现有41832字节的无指针 QGPUProgramImage，包含片元 MSAA 变体，进程内注册 ID 不写入文件。重新使用时沿用原生 JIT validator/registerProgram 和作业资源生命周期，按需注册；归档自身不预占全部内核程序槽，已注册对象通过弱缓存复用。

函数 key 包含原库内容、原始入口、解析后的有效函数常量和库线程限制；specializedName/label 不改变代码身份。AIR/MTLB 的函数 descriptor 归档命中仍检查声明和必需常量，但跳过 lowerAIR 与 IR3 编译。MSL 的 newLibraryWithSource 目前仍先生成库，不能据此声称省掉源库编译。普通 pipeline descriptor 仍接收已经解析的函数；归档查找在完整 descriptor 验证之后进行。管线 key 使用这些函数身份及实际支持的线程上限/执行宽度承诺、颜色/深度/模板格式、混合与写掩码、采样/覆盖及顶点布局，不含原生进程句柄。

Tile、Mesh、stitched library、visible/intersection function、Apple GPU 机器码及多设备 FAT 归档仍不支持，相关必需方法返回明确 NSError。反射、动态链接和更完整的延迟库编译仍待完成；未增加 Metal family 声明，也没有新的编译耗时或动画性能测量。此前0.53阶段“所有 archive-only 请求都缺失”的记录属于历史状态，现有行为以此节及 [原生管线记录](../../drivers/abi-22H374/native-pipelines-abi.json) 为准。

仅在现有 gpu-submit 中增加生产 codec 的实际编译程序镜像往返、上下文/版本/边界、重复记录及损坏图检查，主机检查通过；该检查的 digest 为外部输入，不能当成 iOS CommonCrypto 或 Metal 文件接口已运行。现有 metalctl pipeline-test/jit-test 加入归档保存/重载、原生函数/管线严格命中、状态及空归档未命中、普通回退和文件损坏拒绝；JIT探针要求重新注册的句柄不同于仍存活的原函数，并使用恢复后的 Compute/Render 管线读回像素。目标探针仅编译，尚未执行。GPU1.71、133个 AOT 入口及 a74b60151e63e259 registry 不变，系统 UI 和动画仍未验证；构建记录为 build/gpu-runtime/archive-offline.json。


## Metal0.66 真机修正

round43 首次确认原版 Metal.framework 能返回Adreno540设备，但Blit、Compute/Render、JIT和fence探针尚未成功。根据实际失败修正默认Compute stage-input的语义空值判断、Blit私有参数页对齐和完整JIT共享上传；保留Apple原版描述符、IOGPU资源/队列、原生结束通知及IOSurface接口，未换用模拟完成或CPU像素回退。GPU1.72声明64 KiB共享上传能力，Metal在完整程序可上传时才启用JIT。nil函数返回NSError，测试工具保留原始库编译错误。修复已交叉构建，实际像素结果以round44的命令退出状态和GPU回读为准。


round-51 最新真机对照：GPU1.81 / Metal0.66 默认关闭手动供电恢复，可信CP重申64位地址模式后原生CP回读通过，首个Compute仍发生SMMU转换故障。CP导出的HLSQ快照与round49逐字节一致，SP aperture仍不可用。尚无UI合成GPU帧或流畅动画验证；详见[硬件调试记录](hardware-bringup.md)与 `build/gpu-runtime/round-51-evidence.json`。

## 原生UI高编号纹理绑定

Metal0.74 / GPU1.87使用Shader37/JIT34。round63原生合成器的实际fixed_frag_lph_cpf变体将噪声纹理放在Metal索引9，原先的0..7检查拒绝了它。现在AIR顶点/片元的条件接口特化后，先保留启用的低编号槽，再将索引8..15分配到空闲物理槽；本次逻辑3→物理3、逻辑9→物理0。每个特化入口至多启用8个纹理，重复索引、索引16和容量超限仍拒绝。Compute和现有MSL源语言绑定范围保持8。

指针无关的ProgramDescription新增32位textureBindings：0为恒等映射，其他值的8个nibble分别记录物理槽对应的Metal索引。普通/MSAA版本必须一致。描述符2428字节、JIT image41840字节，内核和插件须配套。映射进入AOT/JIT、归档和准入检查；Metal编码器保留16个逻辑纹理引用，在生成既有8槽draw负载时取出对应资源。内核资源检查、IOSurface导入和IOGPU命令队列不变。

同一真实变体还使用常量i8噪声宽度转换及半精度向量clamp。两个LLVM适配器允许经类型核对的标量常量i8/i16输入并保留有符号值；仍不声明一般窄整数算术。LLVM以标量常量对象表示向量splat时，现在保留half向量宽度，避免把向量零界限生成标量。原版AIR及完整59项常量、生产编译检查、两个352字后端程序和代码比对保存在`build/gpu-runtime/ui-air-texture-bindings/`，不等于目标像素通过。临时分析程序已删除。

## 资源的可丢弃状态

round64的backboardd原生异常栈明确经过Core Animation空闲回收定时器和
`QcomMTLBuffer.setPurgeableState:`；旧实现对合法Volatile/Empty请求抛异常，
造成SIGABRT。Metal0.75实现KeepCurrent、NonVolatile、Volatile、Empty的原子状态
转换并返回此前状态。Empty在后续Volatile请求中保持，直到NonVolatile返回Empty，
使调用方知道内容需要重建。Buffer纹理及纹理子视图共用实际backing状态；heap
子资源只反映整个heap的状态，通过MTLHeap控制，不能单独使同heap其他资源失效。

当前原生DMA分配仍保持wired，Volatile只授权未来丢弃；Empty令内容在语义上失效，
不会释放仍可能被原生作业或视图引用的地址，也不会减少已分配字节计账。
尚未接入内存压力下的物理回收/重建。IOSurface导入遵循Apple IOSurfaceRef.h：
高层Metal请求不改变IOSurface的可丢弃状态，控制权保留给IOSurfaceSetPurgeable。
提交时检查普通buffer/texture的状态，拒绝仍为Volatile或Empty的资源。

现有`ca-render-probe`额外执行buffer的Volatile/KeepCurrent/NonVolatile与texture的
Empty/Volatile/NonVolatile，再重写初始内容并沿原生CARenderer和GPU读回路径验证。
没有新增测试脚本；round65的64×64与round66的1080×1920普通纹理、IOSurface探针均已在目标通过。
round65按实际启动UUID筛选的backboardd崩溃报告为0，观察窗口内未再出现该异常；
这不等于内存压力下物理回收或系统UI展示已通过。
接口依据本地iPhoneOS18.6 SDK的MTLResource.h、MTLHeap.h、IOSurfaceRef.h及
[Apple状态定义](https://developer.apple.com/documentation/metal/mtlpurgeablestate)。

### Round88: 提交时序修正后的显示与性能

Metal0.83 的 Scheduled/future-event 回归、原生 CA256×256 八帧透明图像/局部更新和 render-state 均通过；四核在线，主采集无 panic。用户确认当前欢迎文字无闪烁、黑块或叠字，但帧率很低。首次进入 Setup 的重复启动验证仍待完成。证据：`build/gpu-runtime/round-88/evidence.json`。

去掉截图与测试负载后的同一启动窗口以 uptime 差计时，约 197.519 秒共接纳873笔 compositor 作业；这是作业率，不能当作实际显示帧率。每笔地址空间构建平均 19.050 ms、完整接纳 35.489 ms、CPU观测运行 27.062 ms、回收 10.339 ms；阶段可能重叠且包含门锁/轮询延迟。原先目标机 `sleep 30` 不存在，名义30秒窗口无效，已剔除。后续比较由主机等待。

GPU1.94 候选复用 descriptor 保持 prepare/DMA-wired 期间已验证的物理页索引，逐页与保留硬件页表的比对和只读权限检查不变；framebuffer-fetch 每个 primitive 的背景快照仅复制 scissor 覆盖的行，并向外扩到完整2×2 helper quad，保留原纹理 pitch、坐标及完整 scratch 分配。现有 gpu-submit 用 RGBA8/R8/RG8、无MSAA/2×/4×、线与三角形检查实际快照 SSBO 地址和奇数 scissor 边界；主机与内核交叉编译通过。Metal0.83 HFS/trustcache 沿用冻结文件，上机效果另记。

### 原任务收尾与流畅度任务交接

Round89 GPU1.94/Metal0.83 的同步/未来事件、完整 blit/只读权限、原生 CA256×256 八帧和 render-state 全部通过，四核在线，主采集无 panic。实际扫描图显示完整 hello，状态栏及底部提示完整。用户确认“仍然很慢，但无闪烁叠字”，并明确要求原任务完成、流畅度另开任务。证据 `build/gpu-runtime/round-89/evidence.json`；续接入口 `docs/gpu/ui-render-handoff.json`。未实现新的 scratch 缓存或跳过清零；不要把这些排查设想当成现有代码。

### 流畅度续接：GPU1.95 scratch 缓存

GPU1.95 在原生 ShaderDraw 路径复用已退役的 scratch DMA descriptor：只匹配完全相同的长度，最多4个/32 MiB，每次重新使用前完整清零（含进度/时间戳与padding）。仅硬件完成或已证实的停止、私有页表/ASID TLBSYNC退役和资源缓存同步成功后入池；失败/隔离路径不入池，硬件停止后先释放缓存再关闭mapper。原生任务资源、只读权限、逐页实际保留映射核对、Metal0.83 Scheduled/future-event与QuartzCore资源清理语义均保留。

Round90 四核、无主采集panic，同步/未来事件、完整blit含只读输入/拒写、CA256×256同IOSurface八帧和render-state均通过。前1024次scratch请求1016次命中/8次分配，最多观测4次淘汰；所有缓存仍受32 MiB上限约束。用户亮屏观察“仍然很慢，变化不明显”。无截图/读回/测试负载的30.338秒窗口接纳348笔合成作业（11.47 job/s，**不是显示FPS**）；每笔编码2.810 ms、建表18.362 ms、接纳29.024 ms、CPU观测运行21.217 ms、回收5.002 ms。与round88负载混合不完全相同，不能把作业率差异直接宣称为显示提速。证据 `build/gpu-runtime/round-90/evidence.json`，冻结源码/产物 `ui-scratch-cache-source.json`。本轮原生orderly reboot无panic并返回Fastboot。

用户要求同时核对Apple原版GPU实现。只读审计固定SHA的原版kernelcache中IOGPUCommandQueue、IOSurface资源工厂、IODMACommand::prepare、AGXFirmwareResourceStack和AGXGPUMemBlockPool：提交保留共享内存并检查segment范围；AGX资源栈在锁下检查范围/分配/引用计数/重复归还后更新空闲位图；池析构逐项释放保留block再释放元数据。这支持有所有权约束的缓存方向，但不能推出Adreno可省略清零、缓存可见性或ASID/TLB退役。完整地址/反编译与证据在 `build/gpu-runtime/ui-scratch-cache/apple-driver-audit.json` 和 `apple-pool-lifecycle.json`，此前Metal/QuartzCore提交审计继续有效。

GPU1.96 下一候选只优化尚未发布的私有页表构建：每个连续2 MiB L3范围走访一次上级表，逐叶写入；每个source.page仍逐页检查prepared物理页与实际保留内核页表翻译。树发布后保持不可变，ASID/TLB退役不变。既有gpu-submit增加独立实际PTE遍历、跨2 MiB/1 GiB边界、非连续物理页、RO/XN和中途source验证失败检查，主机与内核交叉编译通过。真机结果另记，不能用离线检查代替目标正确性或流畅度结论。

Round91 GPU1.96 四核、主采集无panic，上述完整四项真机回归全部通过；用户再次表示“仍然很慢，变化不明显”。42.485秒无截图/读回/测试窗口接纳753笔合成作业（17.72 job/s，非FPS）。每笔preflight5.576 ms、encoding2.940 ms、address_space14.154 ms、admission_total24.892 ms、run_observed21.196 ms、retire_cleanup4.990 ms。Preflight/encoding/建表等包含在接纳总时间内；阶段可能重叠，不能重复相加或将run_observed当纯GPU时间。GPU IRQ回调2382、合成完成2368，说明并非只靠无中断的10 ms轮询；仍需区分GPU执行与门锁/回调延迟。

性能窗口后单次scanout实际验证 direct=1、mapper_available=1、direct_frames137、quarantined=0，source0xf0004000/IOSurface1；PNG长度/base64偏移/每个chunk CRC/IEND均验证并已查看。该画面处于灰底问候语过渡，状态栏和信息按钮完整，不能从此单帧声称完整hello或测得FPS。显示交接的免拷贝路径已启用且实际使用；framebuffer-fetch仍包含必要的GPU背景快照复制。证据 `build/gpu-runtime/round-91/evidence.json`。本轮之后的orderly reboot又出现既有launchd退出后的initproc namespace2 subcode0xa，再回Fastboot，与无panic的UI运行采集分开记录。

### GPU1.97 只读时钟观测

新增可选gpu-performance-observe=1，每5秒最多64次，在原统计发布处采样。GPU频率经既有ClockController GetRate/GPUCC7读取真实PLL/分频配置；CPU复用既有OSMController LUT验证，固定0x179c0000/16KiB映射仅CPU只读，IO回调没有write/wait/kick。只读现有enable/desired index/计数器control/status，不启动OSM驱动、不启用计数器、不写频率/电压/PMU。GPU1.96渲染、页表、退役和冻结Metal0.83均不变。原display-submit-trace可选启动参数换成此观测参数以满足NVRAM长度限制。

Round92初步运行时GPUCC解码257000097 Hz；两个OSM bank enable均0、counter control0x14/status0，原LUT验证返回NotReady，因此CPU实际频率未知，输出desired-hz=0是无有效读数，绝非CPU为0 Hz。DT中1900000000是声明值，不能用作实际测量。后续完整证据另记；不得为取得数字而猜测LUT、初始化未接管的OSM/CPRh或无依据改电压。

### GPU1.98 CP硬件计时与OPP对照

gpu-performance-observe=1下，单活跃硬件作业在kernel-only fence backing的+8/+16处由CP_REG_TO_MEM记录A540 always-on64位计数器；开始位于私有页表进入前，结束位于返回kernel tree并WAIT_FOR_IDLE/ME之后。普通CACHE_FLUSH_TS IRQ fence和poll验证成功后才读取计时。该区间包含页表切换、IB和屏障，不是单独shader时间；不启用或重置计数器。既有gpu-submit验证未完成作业不能提供计时、诊断槽不允许并发覆盖，以及两IB私有页表下的采样只访问kernel fence。原Scheduled、资源同步、逐页映射核对与ASID退役不变。

Round93的257MHz安静窗口50.535秒/873笔合成作业，CP平均18.786 ms，always-on与CPU时基校准为19199999.86 Hz，invalid0。CPU两端统计每笔admission24.967 ms（含preflight5.579、encoding2.877、建表14.282 ms），run_observed20.910 ms、retire4.969 ms。主机原采集进程在blit过程中以143结束，原始UART及统一工具证据解析器重建的报告明确标注采集不完整；不声称完整blit/CA/render-state通过。后续同boot UUID附着取得上述安静窗口。证据round-93/evidence.json。

Round94在相同GPU1.98/Metal0.83与总线预算下，GPUCC实际读回414000000 Hz，GFX读回896000uV；四核、无UI运行panic，同步/未来事件与CA256×256同IOSurface八帧均通过。49.746秒/952笔合成作业，CP平均13.256 ms，吞吐19.137 job/s；相比257MHz安静样本，CP下降约29%，作业率增约11%，均不是显示FPS。CPU接纳25.046 ms、建表14.431 ms、run_observed15.848 ms、retire4.917 ms，说明CPU阶段仍显著。证据round-94/evidence.json。

频率与电压依据固定MiCode提交3fb35ef1f0fe1f714be3df39724cbc8eb217181e和仓库sagit厂商DTB的实际参数。414MHz在全部8组CPR电压上限中的最大值为880mV，故896mV对照无需升压；596MHz最大上限968mV，CX/MX NOM256支持该档。CRC现在保持257MHz初始化，再关闭唯一gfx3d consumer，用原有GPUCC检查路径重调PLL/root并读回，最后才启动CP。现有power测试覆盖低档CRC和高档恢复后的目标读回。高档不通过活动CP直接改PLL。

AGX原版性能状态断言的只读审计在ui-execution-profile/apple-power-audit.json：通过硬件接口处理状态请求，检查entitlement/platform binary并管理断言所有权及最后释放。该证据不意味着QcomMTL已经实现AGX性能SPI或动态调频；当前工程对照使用设备自己的固定OPP。队列priority与MHz不可等同。

Round95保持414MHz/896mV，仅将GPU峰值预算提高到厂商414MHz最低busindex8（6144MB/s，对应BIMC请求768MHz）；ACK请求不等于DDR实测频率。其后采样遇到原生自动息屏、合成计数停止，已剔除无作业窗口，不以0 job/s计算性能收益。后续唤醒、标称档与最终回归结果另记。/usr/sbin/ioreg虽然存在但独立运行被SIGKILL137，管道末端while曾掩盖失败；没有据此取得显示计数或FPS。

Round95的CADisplayStateControl原生on请求确实达到四个on状态，但只恢复47笔合成后再次空闲；固定UUID/偏移核对的SBSUndimScreen返回也没有恢复持续动画，不把IPC返回当作UI活动。相同前9笔fence用例的CP总时间，414MHz原总线为2.045ms、提高总线后1.005ms；此小用例不等于Welcome吞吐或FPS。用户明确表示不在设备旁，要求自行检测，后续不等待人工流畅度反馈。

Round96使用GPU1.98/Metal0.83的原生代码与冻结HFS，GPUCC稳定读回596000097Hz、GFX读回968000uV、CX/MX NOM256；峰值预算采用厂商596MHz最低busindex9（8136MB/s），BIMC请求1017MHz。保留低档CRC初始化、实际固件GPMU应答、时钟/供电租约和全部健康检查。当前是固定工程OPP，未声称Apple性能SPI到Adreno的动态调频已实现。CPU实际频率仍未知，四核在线。

该启动先采无GPU测试/读图的原生界面工作，再跑回归：剔除起初不足100笔的启动样本和最后息屏的无进展段，34.630秒内1231笔合成作业，CP平均6.573ms、35.547job/s、计时invalid0。统计覆盖全部1474笔界面作业的CPU平均接纳12.445ms（其中preflight4.518、编码1.570、建表4.884ms），CPU观测运行8.569ms、回收2.772ms、发布0.051ms。与round93/94为不同时间窗口，596档也同时调整了核心与总线，不能宣称隔离的单频率收益。

随后同boot UUID37881A5D-0F2D-45ED-95CE-02904FE3D995的完整同步/未来事件、CA256同IOSurface八帧、完整blit/RO拒写/guards和render-state均通过，三段采集均无运行panic。计时窗口后实际scanout读取direct=1、mapper_available=1、direct_frames62、completed/displayed62、quarantined0，pending/active/queued均0。PNG89,354bytes，所有base64偏移、长度、PNG chunkCRC/IEND验证并已查看；仅精确移除已知插入的10条GPU诊断行，原UART/hash保留。图为灰底问候笔画过渡、状态栏和底部文字完整，不作完整hello或光学流畅度证明。

低帧率并未解决：35.547是作业率；直到上述截图，整个启动也仅累计62次显示交换。因此此前34.630秒活跃区间的新增交换率至多62/34.630≈1.79次/秒，这是保守上限，不是逐帧FPS实测。一帧包含许多独立ShaderDraw原生作业，当前每笔仍付出接纳/建表/退役开销；需继续研究保留完整Scheduled边界、依赖、RO和资源隔离的有界批提交，不能靠升频或假完成宣布流畅。证据round-96/evidence.json及ui-opp596-capture/performance-report.json，最终图round-96-final/scanout-1.png。


### GPU1.99 / Metal0.84 有界 draw 批提交与真实显示间隔

协议5允许同一原生提交内最多16个连续draw；原协议4兼容保留。immutable快照、每个draw独立只读索引片区、整组最大scratch和逐页物理地址核对不变；一个私有页表/真实fence覆盖整组。跨IB保留递增flush序号并重置每IB地址重定位。前端在事件、fence、barrier、任务归属及潜在CPU索引生产边界拆组，Scheduled仍只在整个操作列表接纳后发布。主机PM4重定位/序号和真机fence、未来事件、blit/只读/guards、CA同IOSurface八帧像素及render-state全通过。

Display1.0.13在真实displayed swap完成后记录时间，不用VBL或GPU作业数冒充FPS。Round97欢迎动画137个实显样本覆盖52.445秒，2.593fps，p50 350.210ms、p95 783.886ms；2856个shader操作合为885作业，失败0。CPU每批接纳18.497ms（内含preflight4.946、编码6.794、建表5.366）、回收7.822ms；CP执行区间18.829ms。阶段重叠且子阶段包含在总接纳内，不重复相加推导逐帧墙钟时间。

新增原生CA移动白条20秒诊断，持有经过核对的CADisplay on lease，持续合成并读真实实显时间。Round97为25.134fps，round98为25.212fps；两者均未达60fps。这不是Welcome的性能替代。另行取回两张实际scanout，白条x范围从539..1079移动至102..642，34960像素变化；PNG长度/偏移/CRC/IEND全部核验，原始UART保留，抓图不在计时窗口。证据ui-native-batch/performance-report.json与round-97-capture2/scanout-verification.json。

### GPU1.100 只读继承CPU时钟核对

按本机厂商DT只读映射两个APCS PLL/RCGR，重复读取确认元组稳定。通过固定原厂ClockDxe的APCS检测函数0x90e8、source表0x18708及PLL检测0xec70核对：在线0号簇cfg0x301选source105、PLL0x17916000的L0x51，alpha关闭、前后分频1、锁定，配置推导1555.2MHz；非周期实测。另一簇cfg0x403选辅助源，不能把其自身PLL614.4MHz当CPU频率。固件GetClockFrequency可能返回缓存，0号簇查询失败，1号簇报300MHz；CalculateClockFrequency有诊断mux写和无限轮询，未调用。OSM依旧disabled。证据ui-cpu-clock-audit/firmware-clock-audit.json和apcs-clock-decode.json。


### Metal0.85 原生提交辅助函数执行既有FIFO

commitAndWaitUntilSubmitted在既有marker回退前尝试普通queue drain，由既有executionLock tryLock、FIFO和依赖暂停保护；硬件退休/回调仍走独立executor。返回YES仍要求完整操作列表已接纳，marker不冒充Scheduled，未来事件仍延后cleanup。Round100四项完整真机回归通过，无UI运行panic。

相同kernel/HFS/596MHz/968mV/CPU配置的round98开批提交与99关闭批提交对照：原生移动动画25.212 vs16.579fps，约52.1%提升。0.85进一步达29.746fps（598实显样本/20.070秒，p50 33.341ms、p95 33.483ms），空marker增量仅34。CPU每批接纳15.160ms（内含preflight4.500、编码3.947、建表5.244）、回收5.572ms，计782批；这些不能当独立逐帧墙钟相加。Welcome单独测得2.711fps，仍未达到60fps目标。

最终仅在实际quiescent时取得一张扫描输出：direct1、mapper1、directframes1893、quarantine0、displayed1896且pending/active/queued0；完整PNG校验通过，红底白条符合诊断内容。忙时NotReady没有被计为成功，单图不作为0.85持续运动证明。round97两图运动及round100八帧CA像素回归分别保留。证据ui-inline-submit/performance-report.json、round-100-capture/scanout-verification.json；后续继续调查逐图元framebuffer快照、建表及回收成本。

### GPU1.101 页表游标对照及背景快照工作量

RetainedPageTables新增逐次核对实际父描述符和叶描述符的游标，只省去重复寻找已保留表页，绝不缓存物理地址或省略逐页buffer physicalPage核对。跨2MiB边界、父表替换、block/table切换、叶修改/清空、循环、宽度及destroy/re-adopt旧游标均有主机检查；最初block转table的失败已修复，初始失败日志保留。`gpu-cursor=0`恢复原完整查表路径。内核、Metal0.85、Display1.0.13、HFS、596MHz/968mV/总线和CPU配置在对照中相同。

Round101开启游标，原生CA移动动画596个实显样本/20.070秒，29.647fps；round102关闭，598样本/20.220秒，29.525fps。每批建表均值4.135 vs5.340ms，减少22.6%；总接纳13.920 vs15.248ms。批数810 vs781，框架运行不是完全确定的命令重放，因此不把所有时间差都归因于游标；帧率没有显著提升。两轮Welcome分别2.998和2.782fps，启动工作量不同，不能作为严格同负载对照。60fps目标未完成。

Round101 Welcome接纳759批，CPU每批平均总接纳18.988ms（包含preflight4.971、编码7.918、建表4.702），回收8.914ms。真实CP区间平均20.918ms，最大237.630ms，计时invalid0；区间包含切换页表、背景复制、绘制和barrier，尚不能单独归因于复制。CPU/GPU阶段重叠，子阶段已包含在接纳内，不能相加推导逐帧墙钟时间。

新增完成计数只在真实、连贯且成功发布的native作业上累计：Welcome启动工作复制背景payload 3,598,916,608字节、760图元；受控移动动画窗口增量67,961,088字节、14图元。payload只计一份复制大小，读写流量至少两倍，但不是外部DDR事务实测，也不是与每个实显帧一一匹配的计数。显示端IOSurface直接扫描仍启用，无每帧CPU复制；GPU逐图元framebuffer快照仍存在。不能把“直扫免拷贝”误写成整个渲染链路无复制。

Round101四核且无UI运行panic；原有完整fence/未来事件、blit/只读/guards、256x256同IOSurface八帧（每帧65536像素正确、unchanged0）及render-state均通过。102关闭游标的主启动同样四核、无运行panic。源码及构建快照在ui-translation-cursor/source-before、source-after；原始UART及性能比较见ui-translation-cursor/performance-comparison.json。继承复制的候选receipt中旧source_snapshot/artifact字段已改为本候选实际冻结源码和实际SHA；原父receipt保留在parent-source.json。

后续候选审计记录在ui-translation-cursor/next-path-audit.json：可验证A5xx原生2D bit-preserving copy替代compute背景复制，先限制非compositor诊断并核对pattern/重叠/边界像素，再考虑启用原生UI。尚未实现或启用。另需量化NoC读寄存器/请求中嵌套powerReady的重复成本，保留实际硬件健康、IRQ fence、缓存可见性、RO页、ASID/TLBSYNC和故障隔离要求。

Round103恢复开启游标的同一payload，四核、无UI运行panic且附着UUID一致。Welcome再次135个实显样本/48.327秒=2.773fps；749批CPU总接纳19.123ms（编码7.959、建表4.713）、回收8.931ms，CP区间20.866ms，invalid0。完成背景复制3,545,303,680字节/746图元。其后取得实际Welcome结束静帧，iPod/电池状态栏和信息图标完整；直扫direct1、mapper1、directframes137、quarantine0、displayed137，pending/active/queued均0。33115字节PNG的base64偏移/长度、全部chunk CRC和IEND通过并已查看；十条完整已知内核计时行从交错流中精确移除，原始UART保留，不丢弃未知字节。单张静帧不作为运动或流畅度证明。证据ui-translation-cursor/restored-on.json、round-103-capture/scanout-verification.json。

### GPU1.102–1.104 背景复制实验与真机否定证据

逐图元快照时序及原fragment的half/float运算均不改动，仅试验用A5xx线性R8字节复制替换部分compute复制。模式0保持原路径，模式1限制非compositor（1.103起还要求真实administrator任务），模式2须在像素验证后才用于UI。增加实际成功完成后的native/compositor 2D字节计数；失败/隔离作业不计成功字节。所有实际父/叶PTE、buffer physicalPage、RO权限、硬件健康、IRQ fence、coherence、ASID/TLBSYNC和quarantine保留。以下104–108轮先限制诊断；完成全尺寸pattern验证后，109轮才启用UI模式2。

复用原有render-state-test，追加1040x67、行距4352、buffer offset256的pattern目标：透明source-over必须逐字节保持背景；同一个draw中的两个重叠instance配合奇数scissor验证先前图元可见。每阶段检查292096字节，包括行填充和前后guards；只有重叠区域alpha的UNORM舍入允许1单位差，其余严格相等。Round104 GPU1.102模式0四核、无运行panic，fence、blit及扩展后的整个render-state-test均通过。Welcome另测118实显样本/36.826秒=3.177fps，启动工作量不同，不作为优化收益。证据ui-snapshot-blit/baseline-evidence.json。

Round105模式1的第一次实际2D图元触发SMMU translation fault：FAR0x1087c000正好为源保留映射0x10834000+0x48000后的首个guard页，FSR0x80000402。驱动隔离作业，2D成功字节仍0，compositor 2D字节0；不扩大映射、不省略guard。其后的CA命令因GPU已隔离不能运行，不是独立的CA像素结果。根因的具体硬件预读机制尚未证明。证据ui-snapshot-blit/diagnostic-failure.json。

GPU1.103仅复制16行对齐的中部，保留至少64KiB及最后部分tile用原有有界compute复制；小复制使用Mesa的单行buffer形式。Round106四核、无运行panic，fence/blit/原render-state部分通过，pattern第一阶段失败：buffer字节4352（plane offset4096、2D行边界）实际51、期望0。没有该轮mapper fault，实际2D完成196608字节，compositor 2D仍0；不运行后续大CA测试，也不启用UI。主机测试通过不能替代此真机否定证据。见ui-snapshot-blit-interior/pattern-failure.json。

GPU1.104继续核对固定Mesa24.3.4 fd5_blitter：单行buffer特殊ARRAY_PITCH128不能直接当多行纹理的slice stride。多行改用实际矩形slice大小，并按该寄存器16位、64字节单位分割，每块最多63*64KiB，避免8/16MiB截断；保留前述compute尾部。同时匹配fd5 sysmem prep的CCU color/depth invalidate，位于已有真实flush完成之后。更新的主机PM4检查核对地址/范围、slice编码、compute尾部、恢复graphics状态、跨IB回滚及单调flush序号。后续107–108轮完成下述真机验证；主机通过本身不作为复制正确性或帧率证据。

重启后固件曾在Battery profile附近长时间无UART/USB输出，之后Fastboot自行重新枚举；具体原因未知，不能归因于新GPU路径。统一test-bridge新增observe-only模式用于无串口写入、无Bash探测、无Fastboot操作的被动采集，不声明payload身份；PTY测试证明没有写入/分发，11项evidence测试通过。正常boot/attach身份约束保留。传输脚本返回0只说明命令均已返回，必须另查console_results逐项状态及实际像素/计数，不能将传输成功当回归通过。


Round107 GPU1.104模式1，四核且无运行panic。1040x67 pattern两阶段全部通过：累计实际2D589824字节、总快照831232字节，compositor 2D仍0。随后1080x1920原生CA同IOSurface八帧每帧2073600像素正确、unchanged0，但2D计数没有增长：CA用例未触发背景快照，不能当大尺寸2D正确性证明。

因此仅扩展既有metalctl render-state-test的可选尺寸参数，保留内核及Metal0.85，Round108模式1再跑1080x1920 pattern。两阶段各检查8356352字节（含4352行距、offset256、全部padding和前后guards），identity精确不变，双instance重叠/scissor的RGB精确、区域内alpha仅允许1 LSB。实际成功2D24772608字节、总快照25032704字节，compositor 2D0，证明多区域全屏复制确实执行且像素通过。该轮Welcome保持compute控制路径，143实显样本/48.061秒=2.955fps。证据ui-snapshot-blit-fullscreen/full-pattern-evidence.json。

Round109以同一GPU1.104内核、工具HFS及频率，启用模式2；四核、主采集无运行panic。欢迎动画158个真实实显样本/43.410秒=3.617fps，p50 249.919ms、p95 466.975ms、p99 786.843ms、最大1700.577ms。实际成功背景快照3932874240字节/830图元，其中2D3854827520字节，覆盖98.016%；不是只改开关却未进入新路径。805批的CP区间平均17.289ms、最大209.986ms，invalid0。CPU每批接纳19.376ms（含preflight5.009、编码8.199、建表4.777）、回收9.193ms。阶段有重叠，不能重复相加作逐帧墙钟时间；108/109启动负载不同，不能把FPS差全归因于复制。60fps目标仍未完成。证据ui-snapshot-blit-fullscreen/ui-performance-initial.json。

109后续受控动画附着没有取得Bash探测回复，命令尚未执行，没有新的动画FPS。先前GPU stats已实际返回0；之后第一轮仅见64字节内核tx_flush输出、第二轮150秒无UART。没有已观测panic或Fastboot设备，故暂记串口无响应，不能据此确认GPU卡死或复制根因，也不把未运行的用例算失败。保留原始UART和报告，未强制中断GPU工作。

GPU1.105为下一轮计时候选，尚未真机运行：诊断ABI2新增ASID/页表退役、coherence、surface发布、资源释放及池回收分段；原操作及全部失败/隔离路径保留。CP仅读取启动已配置的CP_ALWAYS_COUNT counter0，并与19.2MHz always-on区间配对，在实际fence验证后消费；若selector/enable改变则拒绝该频率样本，不重编程计数器。比率表示该区间的有效CP周期率，受时钟门控影响，不冒充PLL配置或CPU周期测量。


### GPU1.105 实际CP周期率与回收分段

用户恢复设备后，Round110 GPU1.105/Metal0.85启动四核，无主运行panic，真实UUID E91C6B4C-081D-4818-934A-9A9C9CD23365。Welcome150实显样本/44.460秒=3.3513fps。796批的CP执行区间平均17.852ms，CP_ALWAYS_COUNT与XO19.2MHz配对后的有效CP周期率297.970MHz，最长作业的比率上界约297.998MHz，invalid0；GPUCC配置仍读回596000097Hz。该数据证明CP周期率约为配置值一半，但具体时钟域、分频或节流原因仍待核对，不能直接按两倍提高PLL。

CPU每批接纳19.308ms，编码8.137、建表4.754；回收9.138ms中，ASID/页表退役1.425ms、coherence0.0537、surface发布0.0050、resource释放0.0146，而索引/commandBuffers释放及scratch/IB池阶段7.637ms。源码中批内扩展64KiB IB仍逐个allocate、随后随OSArray释放，只有主IB进入16槽池；这是下一步缓冲复用的具体依据。该段也包含indices和scratch，不能把整个7.637ms预先都归因于IB。

同boot原生CA移动白条598实显样本，29.5989fps；每批CPU接纳14.142ms、回收5.668ms，其中pools4.191ms。原始UART在token610中插入一条完整已知scratch-cache行，仅精确移除该行并验证全部748个保留frame index连续；active取index150以后，排除先前静止间隔。token151/152未实显，显示计数和index仍一致，不补造帧。证据ui-retirement-profile/performance.json及round-110-animation/frame-verification.json。

fence/未来事件、blit/RO/guards、1080x1920两阶段pattern通过。CA离屏像素用例在nativeRenderCounts读取GPUStatistics时退出：冻结metalctl仍按ABI1/1424字节读取，而1.105诊断为ABI2/1920字节，发生在绘制前；显式及--system发现方式均如此。该轮CA像素结果未验证，不把传输成功或统计不可用写成像素通过。后续重编译匹配诊断ABI的CLI，Metal0.85 adapter仍冻结。请求重启出现既有initproc namespace2/subcode0xa shutdown panic，随后Fastboot实际恢复；主运行与重启异常分别记录。

### GPU1.106 扩展IB复用候选

仅在真实timestamp、ASID/TLBSYNC退役、coherence成功之后，将commandBuffers中的secondary IB转移到原有16槽/1MiB池；主IB不重复转移，OSArray与池的引用分别持有/释放，失败或隔离作业不进入secondary缓存。下一次使用仍全清64KiB、重新构造immutable PM4，并核对每个实际页及只读映射。`gpu-ib-cache=0`提供同kernel/HFS/时钟的原路径对照；未扩大池容量、映射范围或资源权限。

诊断ABI3额外保存已有RBBM counter0/selector6的真实fence配对busy cycles，并增加只读GPUCC PLL/RCGR/CRC双读元组及现存throttle寄存器读数，继续核对CP有效周期率。RBBM busy比率包含空闲，不能直接称GPU频率；不改counter selector、debugmux、PLL、电压或时钟。原有CA工具随ABI3重新编译，HFS中Metal0.85 adapter和Display保持冻结。此候选尚未真机验证，不能声明帧率提升。
