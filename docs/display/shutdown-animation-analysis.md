# iOS 18 关机界面动画与 GPU 合成离线分析

2026-10-20。针对 Setup 中唤出关机界面时入场动画和滑块高光均不可见的现象。
分析使用项目实际采用的 iPad iOS 18.7.10 22H374 arm64 原版固件、当前源码及已有日志；
全程没有访问设备、执行 USB 或 UART 命令或部署。原始分析后继续修改了本地
Metal 源码并完成离线构建，补齐记录见文末。

**现有证据更支持 GPU 纹理准备或合成回归。原版入场动画和循环高光实现完整，
软件渲染曾有动画的用户观察也支持沿加速路径定位。已补齐历史日志中的 Metal
布局接口；继续追踪发现两张高光资源使用 Deepmap2 编码，并在后续补齐独立的
`LA08` IOSurface 导入路径。布局接口报错不能直接作为这两张遮罩的根因。**

初次源码取样为 GPU 1.148.0、Metal 1.19.0、Display 1.0.23；引用的已有设备日志
来自 GPU 1.148.0、Metal 1.18.0、Display 1.0.22 的样本。源码版本和运行版本分别记录，
不能用工作区的新版本证明设备已经运行该版本。机器可读记录见
[evidence.json](../../build/shutdown-animation/20261020/evidence.json)。

**关机界面由 SpringBoard 显示。** Setup 原版导入 `SBSPresentPowerDownUI`。
例如 `BuddyProximityAutomatedDeviceEnrollmentCompletionController`
在 `0x100161880` 调用该入口的本地 stub `0x1001f2654`；这是 Setup 的一个调用点，
不代表用户此次必定通过该页面触发。按键触发也进入 SpringBoard 的关机 overlay。
SpringBoard 的 `SBPowerDownView` 继承共享的 `SBUIPowerDownView`。
证据：[Setup 调用](../../build/shutdown-animation/20261020/setup-request-shutdown.asm)、
[SpringBoard 呈现](../../build/shutdown-animation/20261020/sb-power-presentation.asm)。

| 原版入口 | 未 slide 地址 | 行为 |
| --- | --- | --- |
| `SBMainWorkspace presentPowerDownTransientOverlay` | `0x1d25f4840` | 创建关机控制器，正常呈现调用传入动画标志 1。 |
| `SBPowerDownViewController` 的 presentation block | `0x1d25e3878` | 读取 transition context 的 `isAnimated`，传给 `showAnimated:completion:`。 |
| `SBUIPowerDownView showHideAnimationDuration` | `0x1a60fa18c` | 返回 0.3 秒。 |
| `SBUIPowerDownView showAnimated:completion:` | `0x1a60fa198` | 淡入视图，使用延迟的默认阻尼弹簧展开滑轨；动画标志为 0 时直接执行终态 block。 |
| `_UIActionSlider openTrackAnimated:` | `0x185a74678` | 展开 `trackWidthProportion`，布局滑轨和遮罩，随后显示轨道文字。 |
| `_UIActionSlider _showTrackLabel` | `0x185a75a20` | 高光文字使用 0.15 秒 `fadeInWithDuration:`。 |

0.3 秒是淡入的标称时长，不是整个弹簧过程的固定总时长。
这里没有 Setup 专用的“静态关机界面”实现。呈现 context 的实际运行值仍需对应窗口的数据确认。
证据：[关机视图](../../build/shutdown-animation/20261020/power-view-show.asm)、
[滑轨展开](../../build/shutdown-animation/20261020/slider-open-close.asm)。

**滑块高光由 RenderServer 持续求值。** `_UIActionSlider _makeTrackLabel`
（`0x185a74e18`）在默认 `textStyle=0` 时创建 `_UIGlintyStringView`；
`didMoveToSuperview` 和显示文字的路径会启动它。`startAnimating`
（`0x1863d10fc`）经 `addGlintyAnimations` 把以下两组关键帧交给 CoreAnimation：

| 动画 | 原版创建地址 | 属性与参数 |
| --- | --- | --- |
| `shimmerAnimation` | `0x1863d2928` | `position.x`，2 秒，重复次数为正无穷，`removedOnCompletion=NO`，fill mode 为 `both`。 |
| `reflectionAnimation` | `0x1863d2a94` | 相同属性、周期、重复及保留设置。 |

两组原版 keyTimes 都是 `[0,1]`，位置数组为 `[0,320]` 或 `[320,0]`，方向由原版
view 中的标志选择。因此它是遮罩图片反复横移形成的扫光；Setup 不需要每帧
调用 `CADisplayLink`、更新字符串或重画笔画来维持这个循环。
证据：[原版关键帧](../../build/shutdown-animation/20261020/glinty-text-and-animation.asm)、
[启动及停止](../../build/shutdown-animation/20261020/glinty-start-stop-show.asm)。

**高光需要完整的图片和混合链。** `updateTextWithBounds:`
（`0x1863d1750`）创建图片、文字及模糊效果，明确设置
`linearLightBlendMode`、`multiplyBlendMode`、`gaussianBlur`，并配置透明度、
裁剪、`allowsGroupOpacity` 和 `allowsGroupBlending`。
因此普通实色图层的位置动画通过，不能覆盖这里的效果正确性。

原版 `UIKitCore.framework/Artwork.bundle/Assets.car` 中两张图片均存在：

| 图片 | 原版 rendition | CSI 头信息 |
| --- | --- | --- |
| `UIGlintyStringViewShimmerMask` | `UIGlintyStringViewShimmerMask@2x.png` | 768 × 48，scale 字段 200，FourCC `GA8 `。 |
| `UIGlintyStringViewReflectionMask` | `UIGlintyStringViewReflectionMask@2x.png` | 同上。 |

这确认原版资源并未缺失。CSI 的 `GA8 ` 描述资源存储；它不能直接替代运行时
`MTLTexture.pixelFormat`、swizzle、IOSurface 格式或解码结果。
证据：[资源头及哈希](../../build/shutdown-animation/20261020/asset-evidence.json)、
[原版效果常量](../../build/shutdown-animation/20261020/indirect-effect-constants.json)。

**初轮证实的渲染缺口是纹理布局 SPI。** 已有日志明确记录：

```text
Creator = "pid 32, SpringBoard"
Stage = 7
Unsupported Metal selector:
QcomMTLDevice newTextureLayoutWithDescriptor:isHeapOrBufferBacked:
```

修复前的 `QcomMTLDevice` 没有这个实现。离线追踪确认原版调用点包括 QuartzCore
`0x184417600`、CoreUI `0x1a04824ac` / `0x1a048271c`、ImageIO `0x187dc3d88`。
QuartzCore 所在函数为 `0x184417108`，它处理 `CGImageTextureData`：先获取图片
格式、大小和 mip 数，建立 `MTLTextureDescriptor`，再查询纹理布局对象。
后续立即使用对象的 `size`、`alignment`、`initializeTextureMemory:`、
`copyFromLinearBytes:…` 和 `finalizeTextureMemory:`。

这条路径的缺口发生在纹理准备阶段，可能使相应图片或效果无法进入绘制，
而其余静态界面仍可显示。后续检查确认两张高光图片的 CoreUI 回调不会返回这种
`CGImageTextureData`，应继续查它们的普通解码和 IOSurface 上传，见文末追踪。
但已有报错没有携带高光图片名称、关机窗口、调用栈或相应纹理描述符，
尚未证明这两张遮罩正好走到失败分支。修复应覆盖真实布局和上传契约；
只返回一个占位对象或虚构 size 不能满足这些调用。
证据：[原版完整调用](../../build/shutdown-animation/20261020/quartzcore-texture-layout-full.asm)、
[跨框架调用点](../../build/shutdown-animation/20261020/texture-layout-callers.json)、
[原版 CoreGraphics 导出](../../build/shutdown-animation/20261020/resolved-native-exports.json)、
[已有失败日志](../../build/gpu-runtime/round-188-final/uart.txt)。

GPU 作业失败计数为 0 不能排除这个问题：接口、资源或管线在进入 IOGPU 之前失败，
本来就不会变成一笔失败的 GPU ticket。已经提交并完成的其他作业也不证明高光图片正确。
该样本中 Mercury 还报告过 `memoryless-allocation` 失败；它属于另一个进程，
没有关机窗口关联，不能作为关机高光根因。

**离线编译排除了一个过于宽泛的解释。** 用原版 QuartzCore `default.metallib`
中经 SHA-256 校验的 AIR，冻结本地主机编译器，检查了 11 个与效果需求相关的
模糊及缩放候选：3 个 vertex、8 个 fragment，均成功生成 A540 代码。
包含 half/float 的 downsample、narrow blur、single pass blur 及 upsample。
所以“驱动完全不支持模糊着色器”不符合此次结果。它们是候选检查，
不是关机帧实录；初次检查也没有验证混合专用化、像素、资源绑定或显示结果。
见[检查结果](../../build/shutdown-animation/20261020/shader-probe/report.json)。

**入场丢失和高光持续缺失需要分别判断。** 管线冷编译、纹理准备或排队若占满短暂的
0.3 秒淡入窗口，用户可能只看到终态。这个机制不能单独解释无限重复的 2 秒高光
长期不可见。后者应重点查遮罩是否成功上传、动画位置是否进入每帧数据、
linear light / multiply 合成结果以及对应画面是否真正呈现。
Display 1.0.23 已修复的 VBL 采样顺序问题会影响节奏，但尚无证据把它等同于此处的
整段效果缺失。修改 Hello 的 display link 请求也没有覆盖 Glinty 的服务器动画链。

后续定位应以同一关机窗口中的数据串联以下边界，本轮没有执行这些设备检查：

| 观察结果 | 应继续检查的位置 |
| --- | --- |
| 动画 key 或窗口内的 presentation position 不存在、不变化 | 呈现 context、view 生命周期和动画注册。 |
| position 在变，但纹理或管线创建失败，没有对应 draw | 原生 Metal ABI、两张图片的解码及布局、实际描述符和资源额度。 |
| draw 已提交，逐帧变换不同，目标像素没有对应变化 | 纹理通道与 swizzle、uniform、透明度、遮罩及 linear light / multiply 合成。 |
| 目标 IOSurface 像素在变，屏幕没有变化 | drawable 呈现、surface seed、fence、DPU 锁存及实际扫描。 |

后续实现已补齐这个纹理准备接口，并追加了遮罩及混合分支检查。现有离线证据
仍不足以宣布某条具体 shader 是唯一原因，或证明设备上的动画已经恢复。
全部分析产物位于 [本轮目录](../../build/shutdown-animation/20261020/)。

## 纹理布局接口补齐与构建验证

同日继续离线实现，Metal 插件版本更新为 **1.20.0**。没有更改 UIKit / SpringBoard
的动画时长、关键帧或软件渲染开关。

从原版 Metal / AGX Objective-C 元数据提取了设备入口及 `MTLTextureLayout`
的完整方法签名。原版 `AGXTextureLayout size` 返回纹理数据大小加 16，
`finalizeTextureMemory:` 将 16 字节 watermark 写到数据末尾；`watermark`
返回两个 64 位值。新实现保持这个序列化契约，使用独立的 Adreno 线性布局标识，
不复用 AGX 的平铺格式标识。

`QcomMTLTextureLayout` 支持 size、alignment、copyGranularity、watermark、
初始化、收尾以及双向线性区域拷贝。它借用实际纹理分配器的描述符检查与布局，
但不分配 GPU 资源；拷贝使用相同的 mip 行距、末级 32 行 padding 和 4096 字节切片跨度。
对齐为 64 字节，拷贝粒度为一个 texel。描述符在创建时冻结。
`isHeapOrBufferBacked` 两个值在当前仅有的线性存储模型中得到相同布局。

支持现有十种单采样颜色格式：RGBA/BGRA8（含 sRGB）、R8、RG8、A8、RGBA16Float、
RG32Float、RGBA16Unorm；覆盖受现有分配器支持的 2D、数组和 cube 面。
不支持的 depth/stencil、MSAA、3D、压缩格式和 memoryless 描述符明确返回 nil。
有效调用保留原始字节，不做灰度、alpha 或颜色空间转换；非法区域、行距、整数溢出和
非同位的重叠拷贝在写入前拒绝。原生 SPI 没有线性缓冲长度参数，调用者仍须按契约
提供足够的源/目标空间。

证据：[原版协议及实现元数据](../../build/shutdown-animation/texture-layout/metal-layout-native.json)、
[原版收尾指令](../../build/shutdown-animation/texture-layout/agx-layout-copy.asm)、
[编译后的签名比较](../../build/shutdown-animation/texture-layout/abi-verification.json)。
编译产物中的设备入口及八个布局方法，共 **9 个编码**，与原版逐项相同。
声明使用适配器自己的协议名，不注册重复的 Apple 私有协议。

## 高光遮罩与混合链复核

原版 `updateTextWithBounds:` 的 shimmer UIImageView 使用 `linearLightBlendMode`
和 alpha 0.9（`0x1863d20b8` / `0x1863d20cc`）；reflection UIImageView 使用
`multiplyBlendMode` 和 alpha 0.75（`0x1863d2210` / `0x1863d2220`）。
上层 effectView 另有 linear-light，路径还包括裁剪容器、文字/chevron 和 gaussian blur。
这些不应被压成一笔普通透明贴图。两张名为 Mask 的资源通过 UIImageView 参与合成；
不能仅凭资源名就认定它们一定是运行时的 `CALayer.mask` 或 A8 纹理。

初轮仅依据 CSI `GA8 ` 存储信息，不能确定运行时 Metal 格式。如果解码为 RG8 灰度加 alpha，
则必须保留 `(r,r,r,g)` 这样的通道解释；默认 RG8 采样的 alpha 为 1。现有 A540
描述符代码会先应用逻辑 swizzle，再映射实际存储通道。新增回归用 768×48 RG8
和 `(r,r,r,g)` 检查实际硬件描述符：格式 0x0f、通道 0/0/0/1、行距 1536。
同时检查 A8、R8、RGBA 及 mip/数组拷贝不会把遮罩数据变成常量。
**没有给所有 RG8 强制套用灰度 swizzle，也没有声称已采集到这两张图片的实际描述符。**

原版 AIR 的 blend-function 40 分支，在完全不透明时化简为 `D + 2*S - 1`，
与 linear-light 公式对应；透明情形包含原版的预乘 alpha 项和输出约束。
这是依据原始运算推导的分支身份，不是一次关机帧的函数常量捕获。
multiply 同时检查了分支 15 的直接乘法和分支 28 的带 source-over alpha 组合，
保留两者区别，由真实函数常量决定实际选择。

用原版 `fixed_frag_lph_cph` / `fixed_frag_lph_cpf`，分别检查颜色输入、纹理输入
以及上述三个分支，共 **12 组**。全部经生产 `Compiler::compile` 生成单采样和 MSAA
IR3，并通过内核共用的 JIT 校验。12 组都需要 `framebufferMask=1`，说明该混合链
还依赖目标颜色读取；纹理组保留 texture 3 / sampler 0 的读取依赖。
A540 现有实现通过每个 primitive 的 GPU 快照或已验证的等价路径提供目标颜色，
不能只检查纹理上传而漏掉该依赖。
结果见[混合编译及 JIT 校验](../../build/shutdown-animation/texture-layout/blends/report.json)。
本次没有执行这些 shader 的像素，也没有把编译通过当成目标设备合成正确的证明。

## 验证结果与产物

| 检查 | 结果 |
| --- | --- |
| `test-drivers --test gpu-texture-layout` | 通过；独立字节期望、十种格式、mip/切片、双向区域、padding、watermark、溢出及拒绝路径。 |
| 同一测试 `--sanitize` | ASan / UBSan 通过。 |
| `test-drivers --test gpu-submit` | 通过；包含新增 RG8 灰度/alpha 硬件描述符和既有提交/快照路径。 |
| 原版 AIR 混合候选 | 12/12 经生产编译器及 JIT 校验通过，含两种采样变体。 |
| 原版 ABI 与编译产物对照 | 9/9 签名编码一致；二进制及构建输入哈希一致。 |
| arm64 离线构建 | QcomMTLDriver、metalctl、gpuctl、QcomGPU / QcomSCM 均完成。 |
| `test_gpu_integration.py` | 5/5 通过。 |

验证时发现 Metal plist 仍声明 shader 协议 38 / JIT 35，而当前头文件和实际二进制
已经使用 40 / 36，已同步声明及离线集成断言；没有改变协议实现。
初次全量检查还命中了旧 QcomGPU 1.88.0 和旧 gpuctl 输入哈希，离线重建后全部通过。
构建目标共用 Mesa 缓存造成过一次锁冲突，顺序重建成功。

统一构建生成的 `gpu-rootfs` 只是本地 staging 产物，没有安装或部署。
冻结候选包和机器可读验证摘要位于
[补齐目录](../../build/shutdown-animation/texture-layout/)。
本次可确认布局接口缺口已在源码和构建产物中补齐；实际关机窗口选择的资源分支、
函数常量、逐帧目标像素和屏幕呈现仍未经设备验证。

## Deepmap2 解码、GA8 上传与目标颜色读取的继续追踪

同日继续离线检查。**追踪阶段确认了 `LA08` 导入缺口，当时未修改驱动生产代码或部署。
GA8 的颜色重排由 QuartzCore shader 完成，不能给所有 RG8 纹理强制灰度 swizzle。**

两张 rendition 的 bitmap 编码均为 **11（Deepmap 2.0）**，bitmap flags 为 **2**。
从原始 CAR 提取的 `dmp2` 块包含以下 LZFSE 载荷：

| 图片 | CAR 中 LZFSE 起点 | 压缩字节数 | LZFSE 解开后的字节数 |
| --- | --- | --- | --- |
| ShimmerMask | 922688 | 7323 | 110640 |
| ReflectionMask | 650816 | 3877 | 110640 |

使用本地缓存中的 LZFSE Go 实现、关闭模块网络访问完成解压。这里的 110640 字节是
**Deepmap2 的中间编码数据**，不能当作最终 GA8 texel；最终紧排行应为 768×2=1536
字节，48 行为 73728 字节。没有执行 Apple 的 vImage 代码，也没有声称已经离线
还原了最终像素。载荷长度、头尾和哈希见
[payloads.json](../../build/shutdown-animation/mask-upload/payloads.json)。

原版调用链已经追到解码和上传边界：

1. `_CUIThemePixelRendition _initWithCSIHeader:version:` 创建惰性 `CGImageProvider`；
   `unslicedImage` 返回缓存的 CGImage。压缩编码 11 进入 provider 分支。
2. `copyImageTextureData` 回调 `0x1a045b8b4` **只处理编码 7**；编码 11 返回空。
   因此，这两张原始资源不会经这个回调进入 `newTextureLayoutWithDescriptor:…`。
3. `copyIOSurface`（`0x1a045bb28`）在默认 options 下将 GA8 映射为单平面
   **`LA08`（0x4c413038）**，每像素 2 字节；`copyImageBlockSet`
   （`0x1a04609c0`）也选择灰度加 alpha 的双字节输出。
4. 两条回调均调用 `0x1a045c848`；其编码 11 分支进入 `CUIUncompressDeepmap2ImageData`
   （`0x1a045c3cc`），再由 block `0x1a045c2c0` 调用原版
   `vImageDeepmap2Decode`（`0x18835eb5c`）。传入像素类型 2，原版
   `vImageDeepmap2PixelSize(2)` 的只读表返回 2。超过 1 MiB 的压缩块才走流式 API；
   本次两个载荷均使用普通入口。
5. QuartzCore 的 CGImage 工厂先尝试 texture-data，再尝试 provider IOSurface，之后
   才进入普通 CGImage／bitmap 回退。普通上传使用 `newTextureWithDescriptor:` 和
   `replaceRegion:…withBytes:bytesPerRow:bytesPerImage:`；不能把其他压缩纹理的
   `newTextureWithBytesNoCopy:…` 分支套到这两张资源上。

证据：[CoreUI provider 原始指令](../../build/shutdown-animation/mask-upload/coreui-provider.asm)、
[解码分派](../../build/shutdown-animation/mask-upload/coreui-decoder-dispatch.asm)、
[Deepmap2 调用](../../build/shutdown-animation/mask-upload/coreui-deepmap.asm)、
[原版 vImage 导出](../../build/shutdown-animation/mask-upload/vImage-deepmap-exports.json)、
[普通 Metal 上传](../../build/shutdown-animation/mask-upload/qc-upload-regular.asm)。
原始指令与 compact-unwind 边界互校；部分自动反编译因未知外部调用错误合并或截短
函数，不能单独以这些伪代码判断 provider 创建和 SurfaceRenderImage 的成功分支。

**GA8 → Metal 的正常映射是 RG8Unorm，逻辑 swizzle 保持 identity。**
`0x184418f28` 把 QuartzCore 内部格式 18 映射为 Metal 格式 **30**，图像模式 **31**；
内部格式 19 也映射为 Metal 格式 **30**，图像模式为 **30**。
这里 Metal 枚举 30 与 shader 图像模式 30 是两个不同字段。

| 原版分支 | Metal 存储 | shader 图像模式 | shader 得到的颜色 |
| --- | --- | --- | --- |
| GA8 / LA08 surface | RG8Unorm（30） | 31 | `(r,r,r,g)` |
| 普通灰度 CGImage，预乘 alpha-last，内部格式 18 | RG8Unorm（30） | 31 | `(r,r,r,g)` |
| 普通灰度 CGImage，skip-last，内部格式 19 | RG8Unorm（30） | 30 | `(r,r,r,1)` |

原版 AIR 的 `fc_image_function0`（常量索引 45）明确包含这两个分支：31 对采样结果
进行 `0,0,0,1` 重排，30 复制通道 0 并把 alpha 设为 1。
CoreUI bitmap flags 的 bit 1 为 1 时选择 `kCGImageProviderAlphaIsOne`；两张资源均如此。
CoreGraphics 将其 provider alpha-info 设为 **5（none-skip-last）**。因此，普通 CGImage
灰度直传分支选择模式 30，而非一律读取第二通道作为 alpha。
纹理输出之后仍有 UIImageView alpha、颜色运算和 blend，不能把整个高光当成 alpha-only mask。

当前 A540 对 identity RG8 的硬件通道为 **0/1/4/5**，即 `(r,g,0,1)`；
随后由上述 shader 得到正确灰度颜色。强制纹理 swizzle `(r,r,r,g)` 会使 shader 的
第二通道也变成灰度，模式 31 再次重排后 alpha 就错了。初轮的 `(r,r,r,g)` 描述符
测试只证明驱动支持显式 swizzle，不能作为原版这条链的实际配置。
证据：[格式映射](../../build/shutdown-animation/mask-upload/qc-gray-format.asm)、
[surface 的原版 Metal 分支](../../build/shutdown-animation/mask-upload/qc-surface-upload.asm)、
[provider 属性](../../build/shutdown-animation/mask-upload/provider-properties.json)、
[原版 AIR](../../build/shutdown-animation/mask-upload/fixed_frag_lph_cph.ll)。

**`LA08` 的缺口会阻止相应纹理上传。** 原版 `0x18443e544` 在尺寸、对齐和颜色模型
等条件满足时接受 LA08，创建 SurfaceRenderImage。之后 `0x18446c0fc` 用
`newTextureWithDescriptor:iosurface:plane:` 创建 RG8Unorm 纹理。
当前代码有两道独立拒绝：

- `GPUSurfaceProtocol.h::elementBytes` 仅接受 L008、2C08、RGBA、BGRA；
  `copySurface()` 共享的 `Surface::valid()` 因此拒绝 LA08。
- `QcomMTLTexture` 的 IOSurface 构造函数把双字节纹理的期望 FourCC 固定为 **2C08**，
  再与 surface 的实际 FourCC 做相等比较，即使只放宽内核一层仍会失败。

原版这个 Metal 创建点返回空后进入 `Failed to allocate MTLTexture from IOSurface`
错误分支；该函数内没有再解码一次普通 CGImage 的回退。因此不能以早先工厂存在
bitmap 回退，就假定后续的 Metal 导入拒绝也会自动恢复。是否在实际关机窗口选中
surface 路径仍取决于运行时尺寸／对齐／flags，现有历史日志没有记录这些值。
这是一项已证实的兼容性缺口，尚不是一次设备关机帧的根因实录。

**linear-light / multiply 的目标颜色读取没有在已测配置中出现新的绑定缺口。**
新增 12 组原版 AIR 配置：两种 half/float 颜色变体 × GA 图像模式 30/31 × blend 15/28/40。
全部通过生产编译与单采样／MSAA JIT 校验；采样纹理 3、sampler 0、framebufferMask 1
均保留。进一步用这些真实 fragment 与匹配接口的合成 vertex 生成生产 PipelineEncoder
命令，对 BGRA8、BGRA8_sRGB、RG32Float 的 1×/2×/4×、快照／in-place 请求共
**216 组**核验通过：

- GA8 保持 RG8 identity 描述符，目标读取另绑定硬件纹理槽 0，与源纹理槽 3 分离。
- 每个 triangle 单独绘制，前后有实际 color／UCHE flush、timestamp wait 和 invalidation。
- 快照地址、逐图元拷贝次数与奇数 scissor 覆盖的行匹配；目标 metadata 保留完整坐标、
  pitch、格式和 sample count。sRGB 描述符保留解码标志，MSAA 使用各自变体和样本布局。
- 受支持的 BGRA8 单采样可走显式启用的 in-place；MSAA 和 RG32Float 保持快照。
  默认 boot-arg 未指定时仍为 snapshot；没有改动这些开关。

**本阶段发现的格式边界：RGBA16Float 当时仅能采样，不能作为颜色附件。**
12 组负向检查均在命令生成前按预期拒绝这种 target；Metal 管线和 render-pass 校验
也会拒绝。若原版效果申请 RGBA16Float 中间目标，仍需补齐其渲染能力；这与纹理能否
采样、是否支持目标读取是不同条件。当前没有证据证明关机高光确实申请了这种 target。

结果：[GA shader 编译](../../build/shutdown-animation/mask-upload/ga-shader-verification.json)、
[PM4 与格式拒绝核验](../../build/shutdown-animation/mask-upload/pm4-verification.jsonl)、
[可复现的核验程序](../../build/shutdown-animation/mask-upload/validate_chain.cpp)。
以上验证不执行 shader 像素，不包括实际关机函数常量、完整动画帧、GPU 缓存的设备行为
或屏幕呈现，不能据此宣布动画恢复。

上述追踪阶段新增内容仅为分析报告和 `build/shutdown-animation/mask-upload/` 下的证据／离线程序。
分析期间工作区 Metal 文件和版本另有变化，前后输入哈希单独记录；这里核对的
IOSurface 拒绝逻辑未变，没有覆盖这些并发修改，也没有把初轮 1.20.0 候选当作新版本构建。
完整结果与输入边界见 [verification.json](../../build/shutdown-animation/mask-upload/verification.json)。

## LA08 导入补齐与离线构建

同日继续实现，版本为 **Metal 1.23.0 / QcomGPU 1.149.0**。
内核共享的 IOSurface 校验现在接受 `LA08` 的双字节元素，元数据保留原始 FourCC；
Metal 的格式匹配允许 LA08 与 RG8Unorm 存储对应。单平面、尺寸、行距、边界、
权限及原有 RGBA/BGRA 匹配检查继续生效，未改变元数据 ABI。

上传不重排像素，也不插入灰度 swizzle。RG8 identity 仍生成硬件通道 **0/1/4/5**，
由原版 shader 图像模式 30/31 分别形成 `(r,r,r,1)` / `(r,r,r,g)`。
显式请求的纹理 view swizzle 仍按调用者描述符执行。

针对性验证沿用 `gpu-submit`：增加 LA08 的合法导入、错误元素大小及格式拒绝，
同时检查 768×48 RG8 的 identity 和显式灰度 view 硬件描述符。该测试已通过；
QcomMTLDriver、metalctl、gpuctl 与 QcomGPU 的 arm64 离线构建均已完成。
没有重复前一阶段的 Deepmap2 解码、12 组 shader 或 216 组命令矩阵。
本次构建结果和冻结产物见 [la08-import](../../build/shutdown-animation/la08-import/)。
所有操作均在本地进行；没有连接设备或部署，尚未验证实际关机动画恢复。

## RGBA16Float 渲染目标补齐与离线构建

继续补齐为 **Metal 1.24.0 / QcomGPU 1.150.0**。上文 RGBA16Float 附件拒绝是
补齐前的历史结果；当前共享附件校验已接受该格式及 1×/2×/4× 采样，
A540 的 RB 渲染存储和 SP 输出格式均使用原生 `R16G16B16A16_FLOAT`（`0x62`）。
shader 输出寄存器仍按其自身精度描述，不因附件为半浮点就强制改为 half 寄存器。

本次同时修复只放开校验仍会存在的两处颜色损失：清屏直接将四个 double 分量
舍入成 binary16，以两个 32 位字填充；计算图像读写按每像素 8 字节寻址，
使用 `unpackHalf2x16` / `packHalf2x16`。这些路径保留负值和大于 1 的颜色，
不经过 UNORM 的量化、钳位或 sRGB 转换。共享图像存储也覆盖 MSAA resolve 和
mipmap 写回；GA8/LA08 的 RG8 通道映射保持不变，ABI 和协议版本不变。

linear-light / multiply 的目标颜色读取保持逐图元私有快照，按完整 8 字节样本复制，
再由 `0x62` 纹理读取线性颜色。RGBA16Float 即使请求实验性 in-place 也走快照；
原有 color/UCHE flush、timestamp wait、invalidation 和奇数 scissor helper 行覆盖继续生效。

必要检查已完成：现有 `gpu-submit` 新增半浮点清屏编码、附件、快照和 resolve 提交检查；
沿用上一阶段原版 AIR 编译产物，对 blend 15/28/40 × 1×/2×/4× 的 **9 组**生产
PipelineEncoder 命令检查通过。主机 llvmpipe 执行本次生成的生产 resolve GLSL，
2×/4× 的负值、HDR、alpha 及行尾填充逐字节匹配；仅将可变工作组声明替换为固定 1×1。
没有重复原来的 216 组矩阵。136 个内置 MSL/AIR 条目重新生成 A540 IR3，
QcomMTLDriver、metalctl、gpuctl 和 QcomGPU 的 arm64 离线构建均成功。

记录、补丁和冻结产物见 [rgba16f-target](../../build/shutdown-animation/rgba16f-target/)。
以上不执行 A540 像素或实际关机窗口；仍不能确认关机效果是否使用该中间格式或动画已恢复。
本次没有连接设备或部署。
