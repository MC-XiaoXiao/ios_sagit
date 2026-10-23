# iOS 18 原版磁盘缓存对 A540 shader 的复用

可以让 QcomMTL 使用原版 `libCoreFSCache.dylib` 存储编译后的 A540 shader。该库是 Metal 自动编译缓存使用的底层键值存储，已核对的插入、查找和读取路径处理任意字节载荷，没有解析 AGX shader 格式。接入时保留现有 A540 缓存格式和校验，只替换存储后端。

接口依据来自 iOS 18.7.10 / 22H374 的静态分析。Metal 1.48 已接入原版 CoreFSCache，底层存储和 QMTLARC1 恢复已通过真机检查，原版系统 shader 也已成功写入；具体范围见文末。自动 shader 缓存不再使用旧的 512 文件槽。

## 已确认的接口与行为

原版 Metal 的 `MTLCompilerFSCache` 调用 `/System/Library/Frameworks/OpenGLES.framework/libCoreFSCache.dylib`；本地 iPhoneOS18.6 SDK 的 [链接桩](../../tools/sdk/iPhoneOS18.6.sdk/System/Library/Frameworks/OpenGLES.framework/libCoreFSCache.tbd) 也包含对应 C 导出。

| 接口 | 此样本确认的职责 |
| --- | --- |
| `fscache_open_with_key` | 接收缓存路径、选项、输出句柄及独立的 32 字节缓存身份 |
| `fscache_find_and_retain` | 按完整 32 字节条目键查找并保留条目；未命中返回 203 |
| `fscache_insert_and_retain` | 接收键、数据地址、长度及输出参数；按容量淘汰可回收条目 |
| `fscache_element_get_data` | 返回条目数据地址和长度；数据属于被保留的条目 |
| `fscache_sync_element` | 请求同步条目的映射数据 |
| `fscache_release`、`fscache_close` | 释放条目引用、关闭缓存 |

原版 Metal 调用方提供外部锁。接入方应串行化缓存操作，在持有条目引用时复制载荷，随后释放引用，再校验自有副本；不能把返回的数据指针当成长期拥有的内存。条目是否可回收受引用和内部状态约束，不能假设容量淘汰总能成功。

`open` 的选项包含容量，以及偏移 8 的 16 位接口版本和偏移 10 的 16 位键长度。本样本接受版本 1、2，键长度要求为 32。它们属于待绑定具体系统版本的私有 ABI。

## QcomMTL 的接入范围

当前数据路径为：

```text
MSL / AIR → 现有 A540 编译器 → QMTLARC1 记录
                                  ↓
                         原版 libCoreFSCache
                                  ↓ 命中后复制载荷
                    现有解码、身份校验与内核 JIT 注册
```

存储使用项目自己的缓存目录，例如 `org.darwin.QcomMTL`，条目键沿用 `functionBinaryKey`，缓存身份使用 `archiveContext`。保存完整的现有归档记录，使机器码和必要元数据一起经过长度、摘要、芯片、编译器构建身份和协议版本校验。相关实现位于 [QcomMTL.mm](../../drivers/gpu/QcomMTL/QcomMTL.mm) 和 [MetalArchive.h](../../drivers/gpu/QcomMTL/MetalArchive.h)。

原版 `MTLCompilerFSCache` 在 CoreFSCache 载荷外另加摘要和键，并由上层解释编译对象。A540 可以沿用自己的归档校验；无需把机器码包装为 AGX 编译对象，也无需与 `com.apple.metal` 混用缓存目录。Apple 的显式 `MTLBinaryArchive` 面向 Metal 编译对象，不能因为底层存储可复用，就推定其原版读取器能解释项目的 A540 记录。

适配器 [CoreFSCache.h](../../drivers/gpu/QcomMTL/CoreFSCache.h) 动态解析原版 C 导出，仅在已核对的 iOS 18 ABI 上启用。库、接口或沙盒目录不可用，以及缓存读取、校验、插入或容量失败时，正常编译 shader。显式 `MTLBinaryArchive` 的 URL 文件接口保持原行为。

自动缓存优先放在 `NSCachesDirectory/<主 bundle ID>/org.darwin.QcomMTL/corefs`；没有 bundle ID 时省略该级。沙盒不允许时使用进程的 `NSTemporaryDirectory()/org.darwin.QcomMTL/corefs`。每个原生存储容量为 32 MiB；遇到另一个进程持有的锁（206），按原版 Metal 的后缀策略尝试 `functions-0` 至 `functions-7`。同一进程串行访问原生存储，复制数据并释放条目后，才在锁外解码和注册 JIT。

载荷仍是完整 `QMTLARC1`，未修改其格式、摘要算法或内核准入。`archiveContext` 包含编译器/适配器/协议源码构建身份及运行系统版本；它同时作为原生存储身份和归档身份，条目键继续使用完整的 `functionBinaryKey`。Function 诊断的 detail 新增 `0x200` 表示原生存储，保留原有命中/写入位。

## 跨重启与失效

本样本按主 bundle 的 `com.apple.` 前缀决定是否比较 boot session，因此不能概括为所有进程都在重启后保留或丢弃缓存。调用方提供的 32 字节身份也参与正常读取时的匹配。

`FS_CACHE_REUSE_ON_REBOOT=1` 在该实现中会跳过调用方身份和 boot session 的比较；它不适合直接作为接入开关。应继续依赖项目自己的构建、系统和协议身份校验，并在目标系统进程中验证跨重启行为。

冷编译与跨进程热命中、并行存储拥有者、损坏载荷和身份失效采用小范围验证。跨重启行为需要在实际系统进程中单独确认，不能用命令行进程替代。该迁移减少缓存存储层的维护，不会自动修复 A540 编译或绘制错误。

## 二进制证据

原版 Metal 调用链见 [shader 磁盘缓存证据](../../build/gpu-scheduling-analysis/20261021/shader-disk-cache-evidence.json)。CoreFSCache 的 12 项反编译与指令记录、接口约束及分析边界见 [复用证据](../../build/gpu-scheduling-analysis/20261021/corefscache-reuse-evidence.json)。

[提取脚本](../../build/gpu-scheduling-analysis/20261021/corefscache/extract.py) 只读固件的映射字节，导出来自原始 export trie。输出 Mach-O 为分析用途，保留原始 VM 地址并补充导出符号，不能安装或执行。原始 `__TEXT` 的 SHA-256 为 `97291f91f716060f3f8d15cde9d900fde7326a25b60e6740dfa04c43379a4ff4`；重建分析文件的摘要另见证据，不能将两者混同为独立原版 dylib 的摘要。

## Metal 1.48 候选与验证状态

`hello-card-21` 的 GPU 1.173 内核与候选 20 字节一致，144 项 shader registry 也一致。Metal 1.48 已交叉编译成功，驱动镜像全部文件回读摘要一致；EFI SHA-256 为 `74d340c9d4749661612c2a3697113cb8beef9a2a7d96308f0c522eda4c29ad85`。

UART 在用户修复连接后恢复正常。21 的临时探针因打包缺少执行位返回 126，未执行；22 修正诊断程序权限，并让 shader 探针使用已有 `mc` 诊断入口，生产 Metal 二进制、内核和启动参数均未改变。22 EFI SHA-256 为 `12a7b2747df855739a151e343c240f297802adb5950393b55a66434c82f0978d`。

22 的原生存储探针通过：首次未命中、保留期间复制、限长、不同键未命中、另一进程占用后的独立存储、跨进程读取、身份失效及容量失败。`air_add` 探针在两个独立进程中通过 QMTLARC1 显式恢复、摘要损坏拒绝及 64 个真实 GPU 输出值核对。注意：该 raw AIR 探针没有进入 lazy MTLB 自动缓存路径，不能把两次执行当作自动机器码缓存命中。

实际 Hello 场景记录中，backboardd 和壁纸进程共 36 个不同函数键前缀出现 `0x282` / `0x302`，确认自动缓存使用原生存储并成功写入。首次转场仍有 4 次冷编译（约 191 / 64 / 267 / 202ms），零 GPU 失败/恢复，不能据此判断热缓存流畅度。详细凭据为 `build/setup-animation/hello-card/cache-validation-22.json` 和 `hello-card-22-hot-transition-data/`。

23 恢复正常 `mc`、移除临时探针，保持 Metal 1.48、GPU 1.173 和缓存身份不变，用于同一目标场景的跨重启缓存验证；EFI SHA-256 为 `d26d8693f4d493a7d52a7319a5709eee0077a5fb002e37e49767b96724227dec`。

23 实际跨重启记录为 35 次原生写入、0 次命中，函数键前缀与 22 相同。目标转场仍有约 208 / 322 / 205ms 的冷函数创建，零 GPU 失败/恢复。这与原库对 `com.apple.` 主 bundle 比较 boot session 的分支相符；未启用跳过身份检查的环境变量。因此不能声称本接入已经消除跨启动冷编译。原始记录见 `hello-card-23-hot-transition-data/`，摘要见 `hello-card/cache-validation-23.json`。用户随后人工确认：预热后的转场没有明显改善，动画目标仍未完成。

构建和启动凭据保存在各候选目录。21 / 22 / 23 的打包中间文件分别已清理 466,945,145 / 466,676,227 / 466,922,287 字节；清理清单在 `build/setup-animation/hello-card/cleanup.json`，当前候选及必要回退载荷保留。

同一次启动的两个原版 `mc ca-render-probe` 进程（638 / 640）均退出 0。第二个进程的 5 个相同函数键均为 `0x201` 原生缓存命中，耗时依次从 245.768 / 189.923 / 162.225 / 202.688 / 163.346ms 降到 95.003 / 7.741 / 3.717 / 3.042 / 3.609ms。该探针经过原版 CARenderer 的 lazy MTLB 路径，覆盖自动 QMTLARC1 恢复、JIT 注册和实际像素检查。UART 再次间歇乱码，主机仅采信至少两份逐字相同的短记录及成功标记；完整重定向文件仍在设备上，未声称其已通过整文件 CRC 回传。凭据见 `hello-card/ca-cache-23-summary.json` 和 `cache-validation-23.json`。


2026-10-21 离线候选 Metal 1.49 补充 compute uint4 store 的完整范围向量路径，AOT registry 为 `5929f35a948802cd`。缓存文件格式与 CoreFSCache 接入不变；编译器二进制和 registry 已包含在既有 build identity 内，所以 1.48 机器码不能作为 1.49 命中。新版本尚未真机运行，缓存验证结论仍以 1.48 的上述原始记录为准。


## Metal 1.52.2：收紧编译缓存身份（离线已集成）

38的Hello转场仍出现4个不同请求键的现场编译，共约1194.9ms；同一个函数名哈希不代表同一个变体。冷编译任务确认原构建身份还包含整个QcomMTL.mm、Info.plist及原型测试，因此纯GMEM/版本修改也会使身份变化。新构建辅助[scripts/gpu_archive_context.py](../../scripts/gpu_archive_context.py)只保留实际前端代码及头文件、编译器二进制、shader/JIT/归档ABI、常量和程序身份逻辑、AOT registry与编译参数；目录扩容、渲染开关与版本号不再改变该身份。运行时OS身份和QMTLARC1完整校验、内核注册保持原流程。

采用新schema会正常失效一次旧记录；这只避免不必要的构建身份变化，**不改变上文CoreFSCache的com.apple进程boot-session策略，也不保证跨重启命中**。不使用跳过身份检查的环境变量。

实际进入JIT时，诊断模式将已有AIR hash、入口、有效常量、目录匹配结果和NIR回退原因异步写入进程临时目录的qcom-shader-miss-*.json，最多64个请求。无需额外执行GLSL capture。拿到记录后再补准确的离线目录配置，不从短哈希猜测常量。

协作交付的8个缓存身份定向检查通过，日志补丁语法检查通过；统一整包交叉构建也已成功，生成context为364e9562940c79480602c392dbfb12fae6399266a680ac7bf340c6a4bdb1c9c3，与交付预览一致。候选40配GPU1.186，尚未真机启动。候选39保留旧Metal1.52.1，以便独立验证GMEM布局/同步改动。交付清单：[handoff.json](../../build/gpu-cache-review-1.52.2/handoff.json)。


### 1.52.2 真机未命中记录

40已加载并验证身份。确认熄屏后的首轮Hello转场有4次实际JIT，约174.10/215.01/639.22/181.36ms；补取进程专用临时目录`/private/var/mobile/tmp/com.apple.backboardd/`后得到27条完整JSON（23fragment、4vertex），全部为已知AIR的variant-miss。639ms条目明确为`AIR JIT: NIR frontend unsupported intrinsic air.fwidth.f32`，随后GLSL回退成功。现有证据足以用精确常量离线固化，无需猜配置或放宽缓存身份。

同一启动内的热转场121个作业无失败，没有JIT（仅两个约2.5ms的c00目录命中）；显示完成仍有450/250/183/133ms间隔，证明补目录只能解决其中的冷编译部分。目标4个argument为`2db3223efa51d5ef`、`0e5bc46e54a0c07e`、`b24d5d4dda2ca91f`、`894a2eb006a452e7`。完整数据已交冷编译任务生成隔离目录候选1.52.3，编译器与ABI先保持不变。

证据：[精确未命中JSON](../../build/setup-animation/hello-card-40-cache-locations/cache-misses.json)、[校验清单](../../build/setup-animation/hello-card-40-cache-locations/fetch-manifest.json)、[热转场](../../build/setup-animation/hello-card/warm-40.json)。


### 1.52.3 精确目录真机命中

补齐23fragment和4vertex变体，目录共200entry/148variant，原121个JIT image逐字节保留。41的4个目标请求全部出现0x800预编译二进制标志，耗时3.223229/3.373906/3.969219/3.192135ms，合计13.758490ms；原对应4次Function累计1209.698229ms。累计减少约98.9%，不是单帧节省，也不是整段动画提升比例。窗口内没有AIRLower/Backend或compiler-lock事件，热转场没有Function事件。

本次采集的miss文件为0字节，SHA/长度已校验；不扩展为全启动、全系统零miss或跨重启CoreFSCache命中的结论。生产编译器、registry、schema2context保持原值。首次683.482ms显示间隔早于首个目标Function，热转场仍有483.395ms间隔，剩余长帧需要另行定位。

[目录冻结交付](../../build/gpu-catalog-1.52.3/handoff.json)、[首次转场Metal记录](../../build/setup-animation/hello-card-41-transition-data/1-metal.txt)、[集成身份](../../build/setup-animation/hello-card/integration-523.json)、[热转场摘要](../../build/setup-animation/hello-card/warm-41.json)。

### 1.52.4 片元程序整数拆装优化候选

在保持 CoreFSCache、QMTLARC1、校验及真实内核 JIT 注册路径不变的前提下，与编译任务配合加入两个精确的整数 SSA 等价化规则。目标 fluidity profile 的原机器码 hash `9a85ca61f842d0fb` 变为 `bb0b559fcaef1830`，完整常量窗口变体 1376→1312 words，full6/half48 不变；只去掉重复字节重建和对应搬运，不更换 16 位内存读取，不删除原 word load/边界保护，也不改变浮点精度。

交接包 `build/gpu-candidate-1.52.4/final/` 共 148 个 profile 全部通过完整恢复和 JIT 结构校验；140 个完整 image 逐字节不变，变化仅为 fluidity 的 8 个 Boolean 配置。27 个真机捕获过的 CoreAnimation miss 请求仍全部回放命中。四个目标变体的内存读取及递归地址表达式相同；局部正/反例、1690 次真实 NIR 前后比较和 393216 个位模式检查通过。非目标配置仅完成结构/字节与元数据验证，不冒称已做全部像素验证。

实际编译器 dylib SHA 为 `d4d38d5d6a4447b49dcacfaba973f0912588dc1722fa7aad528c4cb1ee636f6b`；schema2 build context 为 `bf981c6baf433e7f59fe71d50162e02dda40f2ab0b239f865be485b299571611`。改变的身份输入是实际编译器库及记录了 GPU1.188 CPU 索引扫描优化的 GPUShaderProtocol.h；目录 registry 仍为 `28f66944d0b9aab4`，身份策略未放宽，旧 context 继续被拒绝。该候选尚在完整 Metal 适配器构建/上机流程，离线指令减少不是已测 GPU 毫秒收益。

#### 1.52.4 上机结果与热点定位纠正

候选 43（同一 GPU1.188 kernel）已启动，真实原生 metallib 的 11 个函数、9 个管线创建检查通过；这不是像素比较。目标转场 139 个作业无失败、无新 JIT，仍有 650.111/200.007/133.481 ms 显示完成间隔。

此前把全启动期聚合 bucket 中的 `9a85ca61f842d0fb`（1376 words、half48）当作卡片转场热点不准确。按每次目标 HOME 后 3 秒的实际操作筛选，候选 42/43 使用的主程序是 `aa94109af25ad76c` → `a7aba4cb1419480f`，均为 800 words、full6/half23、4 个 constant windows 全开（0xf），6 顶点、1080×1920 全屏裁剪。两版实际片元操作中位分别 28.339661 ms（42 热窗口，28 次）和 28.339583 ms（43 首次窗口，21 次），未测到 GPU 耗时收益。补丁确实覆盖这个配置且实际加载了新机器码，但不能用另一个配置的静态缩小冒称本动画加速。

当前有实测收益的是 GPU1.188 的索引扫描（约减少 0.95 ms/背景三绘制作业）；这次整数 SSA 修复仅确认代码生成和功能路径有效。核心转场长帧仍未解决。完整证据 `hello-card/fluidity-524-hardware.json`、`hello-card-43-transition-data/`，后续仅围绕实际时间窗命中的 shader/纹理与提交路径继续分析。
