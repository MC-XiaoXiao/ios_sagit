# Setup Hello 背景与 Mercury：离线复核（2026-10-20）

本轮延续任务 `01a14f76-7650-7d43-92f3-37494be51ce4`。最初在设备充电时离线分析，
用户随后连接设备并授权调试。以下各节保留当时证据，真机新发现追加在文末。
当前优先解决启动首屏没有壁纸，尺寸问题延后。以下地址均为原始、未 slide 的虚拟地址。

## 目前能确定的阻塞位置

Round177 有 7003 笔完成作业，全部来自 compositor；没有 Mercury 自身提交。
两份 Mercury 主线程 stackshot 位于运行时 shader 编译中：

| 样本 | 原版 Mercury 返回地址 | 当时正在执行 |
| --- | --- | --- |
| PID424 | `0x100008ff4` | 第一个 `newFunctionWithName:`，IR3 调度 / SSA uses 分配 |
| PID432 | `0x100009654` | fragment 常量专用化，IR3 调试反汇编 `_display` |

两者都经过 `PRRenderer scene:willConnectToSession:options:` 的
`renderer:didInitializeWithEnvironment:` 同步回调（调用 `0x18b82acc0`，
返回地址 `0x18b82acc4`），进入 Mercury 渲染器初始化 `0x10000874c`。
因此这些样本已经越过扩展启动、设备发现和 command queue 创建的早期阶段。
它们是运行中的抽样，不能当成崩溃现场，不能单凭这个位置判断编译器崩溃。

PosterKit 在该回调返回之后才执行 `updateClientSettingsWithBlock:`，
由 block 设置 `pui_setDidFinishInitialization:YES`。宿主
`PRSceneViewController isSceneContentReady`（`0x18b7d4c74`）明确要求：

```
clientSettings.pui_didFinishInitialization == true && scene.contentState == 2
```

宿主收到 client settings 更新时比较初始化标志并通知 readiness 变化
（`0x18b7d68ec` 至 `0x18b7d6908`）。因此扩展已经运行、GPU 合成正在工作，
仍不足以证明背景场景可显示；当前抽样时初始化回执尚未发出。

## Mercury 后续绘制与相关服务

| 环节 | 本地证据 | 尚缺的验证 |
| --- | --- | --- |
| PosterBoard 数据库与扩展发现 | 前序已修复 store migration；发现 Mercury / HelloPoster，活动扩展记录为 Mercury | 重新采集当前 Hello 对应的 scene / provider / host 关联，避免把其他 wallpaper scene 算作可见背景 |
| ExtensionKit / FrontBoard 场景连接 | 原版 scene connect 已进入 Mercury 初始化，相关框架在原生调用栈中 | 初始化回执、场景 contentState 和 renderer 存活必须在同一启动、同一 PID 对齐 |
| FrontBoard watchdog | `FBSProcessWatchdog provision:wasViolatedWithError:` 会根据 process 回调构造 termination request，并走 `_terminateWithRequest:forWatchdog:` | 实际 namespace/code/description；尚未证明本次是 watchdog，也没有证明固定超时秒数 |
| PosterKit HangTracer | 普通 scene 调用 `_startHangTracer`；preview/snapshot 调用 `_stopHangTracer` | 当前环境分支与退出原因。启用 tracer 本身不是它终止了 Mercury 的证据 |
| PosterBoard / RunningBoard 断言 | `PBFRuntimeAssertionManager` 构造原生 RBSAssertion；增强内存分支检查 `com.apple.posterkit.enhanced-memory-limits`，原始 Mercury 已有该 entitlement | 断言申请/生效结果、实际内存限额、退出前 footprint；权限存在不等于运行时断言成功 |
| CAMetalLayer | `WallpaperMetalView` 设置 device、pixelFormat、Display P3、`presentsWithTransaction=YES`；display 回调从 `nextDrawable` 取 drawable | 真正的 drawable 获取、提交、完成及事务显示。单独 CARenderer 像素测试不足以覆盖此路径 |

Mercury 非 snapshot 绘制链为：
`displayLayer` → `nextDrawable` → drawing handler → coordinator → renderer。
`nextDrawable` 返回 nil 时直接跳过。coordinator 在 `0x1000338ac` commit，
普通 drawable 分支在 `0x100033954` 直接调用 drawable `present`；
另一个条件分支会 `waitUntilCompleted` 并使用 IOSurface 内容。它并不统一经过
`MTLCommandBuffer presentDrawable:`，后续呈现验证必须覆盖原版路径。
这属于后续检查点，当前没有证据指向它是本轮最先失败的环节。

`Rendering extension timed-out` 字符串位于 `PRRenderingSession` 的超时日志路径；
不能据此把它解释成当前 scene 初始化的超时。
`Too many corpses being created` 仅说明尸体报告的创建受限，也不提供退出原因。

## 本轮已完成的修改

- 扩展已有 `displayctl native-exception-window`：在 12 秒窗口内观察 Mercury 的
  `EVFILT_PROC` 退出状态、signal、jetsam / codesigning / decrypt 标志，同时按秒
  记录进程年龄、CPU 时间、resident、footprint 和 lifetime max footprint。
  每 250ms 发现新 PID，最多 64 个并发 watch；注册或读取失败明确记为证据缺口。
  原有 XNU 异常记录开关在窗口结束时恢复，并输出恢复结果。
- 根据本地 XNU 和 libproc 实现保留 CPU 原始 Mach ticks，用实际 timebase 转为毫秒；
  不使用前序 `ps` 中全零的 CPU 列作为性能依据。
- 不查询 `PROC_PIDEXITREASONINFO`：本 XNU 要求调用方为父进程或 debugger，
  root 观察进程也不能直接读取。完整 OS_REASON 仍取 XNU 原生异常/退出日志。
  该 XNU 的 `system_event_log` 使用 `printf`，应从 UART 收集。
- 修正已有 `mc --system fluidity-test` 的描述符覆盖：SDF 禁写附件0，附件1
  开启 Add 混合，源 RGB/Alpha=`SourceAlpha(4)`，目标 RGB=`One(1)`、
  Alpha=`DestinationAlpha(8)`；主 fragment 禁写附件1。此前的九条管线探针只
  配置双附件格式，没有完整复现这些状态。新增探针尚未在真机执行。

生产 GPU / Metal / Display 代码没有在本轮变更。候选继承前序 Metal1.13：
运行时编译不再生成随后丢弃的完整 NIR/ISA 文本。前序已证明 11 个真实 Fluidity
variant 的完整 41912 字节程序图像逐字节相同；主机耗时变化不是设备收益证明。
原始应用、场景就绪策略、watchdog 策略和断言权限保持原样。

## 冻结候选与验收范围

候选目录：`build/gpu-runtime/ui-mercury-init-audit/`。
版本：GPU1.148.0 / Metal1.13.0 / Display1.0.22。
父候选：`ui-runtime-compiler-text`。
`source-before/` 与父 receipt 一致，`source-after/` 保存当前全部已记录源码。

`displayctl` 和 `metalctl` 已通过现有 arm64 交叉构建；runner 通过 Bash 语法检查。
HFS 相对父候选只改变 `displayctl`、`metalctl`、`mc`、`ui-background.sh` 四个文件；
所有文件均从生成的 HFS 读回并校验 SHA256。EFI / Image 内实际嵌入的 kernel、
driver image、trust cache 均验证唯一匹配，原 APFS root、GPU/display handoff
保持一致。保留已安装 PosterBoard store 兼容修复对应的 trust cache。

本轮没有启动候选、连接 UART、发 USB 命令或执行设备测试。
没有把交叉编译通过记作运行通过，也没有把退出事件窗口无事件记作背景成功。
全局异常日志开关的恢复以正常返回路径及输出为准；若诊断进程被外部强制终止，
需要在恢复调试时核对该开关。

## 设备恢复后的最短验证路径

1. 使用冻结候选，先运行已有 `ui-background.sh`，收集 native exception window、
   退出事件、资源采样及 `compositor=0` timing；避免同时运行编译探针干扰初始化。
2. 按同一 PID 对齐原始 exception code / OS_REASON、退出 signal/detail 与峰值
   footprint。SIGKILL 本身不能区分 watchdog 和内存终止；没有捕获退出时继续定点
   获取 stackshot 和宿主日志，不能凭空补全原因。
3. 依据原因决定优化编译时间/内存，或修正明确的 API/权限失败。不要放宽 watchdog、
   强制设置 readiness 或伪造提交成功。
4. 初始化确实完成后，再核对原始 scene readiness、Mercury 自身 GPU 作业和
   CAMetalLayer 事务呈现；单独运行更新的 Fluidity 描述符测试。最终以 Hello
   原生背景实际可见和持续渲染验收。

详细反编译、annotated disassembly、构建日志和 SHA256 receipt 均保存在候选目录。
两份原始 stackshot / symbolication 保留在 `ui-poster-failure-delivery/`。

## Round 178 真机与唤醒回归

用户确认首次出现背景图案或动画，但熄屏唤醒后消失，且壁纸尺寸似乎不正确；目标未完成。
最初 UART 端口映射变化，stage/CRC/run-efi 成功而启动日志无效。改用 CH340 稳定路径后，
root Bash 与 Metal1.13 均已实测确认；全程 RAM 启动，没有刷写分区。

两个 Mercury 进程约 5 秒后收到 SIGKILL，原生 OS_REASON namespace7/code3。
原版 libxpc 的表项 `0x1f03b5a60` 数值3对应 `XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT`，
不是根据字符串顺序猜测。kqueue 没有 jetsam/codesigning/decrypt 标志。
PID273/308/315 完成的五次函数编译累计分别约4219/4349/4388毫秒，其中主要是后端；
这支持初始化过慢的调查方向，尚未独立证明去掉编译即可修复全部生命周期问题。

PID273 还报告 render pipeline / render pass 附件格式不匹配，后续空管线和 inline
buffer 错误是同一 command buffer 的失败链。Metal1.14 候选仅增加具体格式、样本数、
extent 和 IOSurface 尺寸不符的错误参数，尚未放宽兼容检查。

更新后的真实 Fluidity 11函数/9管线描述符测试通过；RG32Float clear/write/fetch 和
两次 memoryless pass 的像素、padding、混合检查均通过。它们不代替 Mercury 原生呈现验收。
BackBoardServices、MobileGestalt 和 framebuffer 一致报告1080×1920，scale3；
实际壁纸取景/裁切仍需核对。另捕获 SpringBoard 缺少 `newTextureLayoutWithDescriptor:isHeapOrBufferBacked:`，
暂未证明与背景消失相关。完整证据见 `ui-mercury-init-audit/hardware-evidence.json`。

## Round 179–183：启动纹理为空及原生压缩选择

Round179 的详细错误为主 pass `extent=1080x1920 samples=1/1 depth=0/0
stencil=0/0 color0=80/0`。这里的 0 是主颜色附件为空，不是已存在的纹理尺寸错误。
Round180 的导入错误进一步给出 `format=0x26424741`、pitch1088、alloc8388608。
固定 SDK `CVPixelBuffer.h` 明确定义该 FourCC `&BGA` 为苹果无损压缩 BGRA；
当前内核只接受线性表面，拒绝是正确行为，不能把压缩数据当普通像素放行。

原版 QuartzCore UUID `FAF7D7EA-18F0-3C2D-816C-AC56D16C40A3` 的初始化器
`0x1844454b4` 根据 MobileGestalt `ChipID` 排除若干苹果芯片，然后在
`0x184445588` 读取 `CAEnableIOSurfaceCompression`、current-application 域。
真机 ChipID=292。该分配路径不查询我们的 Metal 压缩支持，因而未知芯片进入压缩分支。
`0x184452910` 在此开关为假时保留线性表面格式。
没有伪造 ChipID、修改苹果二进制、放宽纹理校验或扩展超时。

Round181 的探针错误地降为 mobile 身份，诊断 GPU IOServiceOpen 返回
`0xe00002c1`，因此这一轮不能作为纹理 A/B 结果。验证改为保留诊断工具原有 root
身份，图形偏好操作则使用 mobile 用户。Round182 仅冻结构建，没有启动真机。

Round183 在原驱动 Metal1.15 上的两次独立进程对比：

| 配置 | 原生 CAMetalDrawable | 纹理 | 实际布局 | 结果 |
| --- | --- | --- | --- | --- |
| 默认、偏好不存在 | 非空 | **空** | 导入日志为 `&BGA`，1080×1920，pitch1088 | status1 |
| 仅本进程 volatile argument domain 关闭压缩 | 非空 | **非空** | `BGRA`，1080×1920，pitch4352，alloc8355840 | status0 |

两者都使用原生 CAMetalLayer、framebufferOnly=YES、presentsWithTransaction=YES、
allowsDisplayCompositing=NO。没有提交 GPU 作业或显示内容，此实验只证明原生纹理分配修复。
原始证据：`build/gpu-runtime/round-183/uart.bin`。

当前开发控制台 CFPreferencesSynchronize 保存 mobile 全局偏好失败，不能将其
报告为生效。因此 `displayctl surface-compression-policy linear` 使用原生 plist
读写，原子保存 mobile `.GlobalPreferences.plist` 中同一布尔键并 fsync、全字典读回，
其余键保持原值。修改前的键值记录在
`/private/var/mobile/Library/Preferences/org.darwin.surface-compression-backup.plist`；
`surface-compression-policy restore` 恢复原值。必须重启后检查框架有效读回和原生
Mercury 初始化；文件写入成功本身不是运行中服务已刷新或背景已显示的证据。

## Round 184：尺寸恢复，启动延迟仍在

用户确认“现在背景显示尺寸正常了”，但首次 Hello 仍“先没有背景，稍后才出现”。
本轮原生错误记录未再看到压缩表面导入错误。诊断进程的偏好检查显示磁盘值为
false，但 CFPreferences 的有效值标志仍为0；因此不能声称已经从 Mercury 内部
直接验证了偏好读回。保留该限制及原始 A/B 证据。

五个完成初始化的原生进程仍各花约4.09–4.46秒创建五个着色器函数，约3.96–4.32秒
在编译后端。PID312 主线程已回到原生事件循环，随后由 runningboardd 挂起；
晚期查询显示屏幕已熄灭，这个挂起状态不是编译死锁证据。该进程的 stackshot 完整，
另一次系统全量 stackshot 因输出过大被主动中断，不能作为完整快照解码。

Metal1.16 候选增加通用 AIR 函数磁盘缓存。键包含完整库身份、解析后的常量、
线程限制及精确编译器/驱动/OS上下文；复用现有二进制归档校验和内核 JIT 准入。
缓存位于原生 NSCachesDirectory，每个目录512槽、每槽一个函数；碰撞只导致
重新编译，原子替换避免并发读取半个文件。显式 binary archive 的 fail-on-miss
语义先处理，隐式缓存不会覆盖它。缓存缺失、损坏、版本不匹配或不可写时仍走
原编译流程。首次空缓存仍需要编译，不能把后续缓存命中当作冷启动已修复。
函数计时 detail=1 表示磁盘命中，2表示成功保存，4表示保存失败。

## Round 185–186：缓存实测与剩余拒绝

Round185 首个进程保存4个函数，后续 Mercury 命中这4个并保存第5个，再正常提交
GPU工作。独立 root 诊断读取原始 Fluidity metallib、创建11函数/9管线：冷运行
15018.29ms，独立暖进程60.87ms，11/11缓存命中。将本测试新建的 root 缓存槽
`036.qmtl` 写成无效短文件后，独立进程仍通过；10个命中，1个重新编译并保存。
没有修改 Mercury 的缓存进行破坏测试。

Round186 同一 EFI 重启后，最早观察到的 Mercury PID231 五个函数全部命中缓存，
总计94.56ms，无后端编译事件。首个 commit 在进程启动后1772.07ms，采样到
16.04秒仍存活，无该进程退出。用户曾选择“同时出现”，随后明确更正未看到首次
出现；Round186也未观察到，因此保留“首次光学可见时刻未验证”，不能引用被更正
的回答作为验收证据。用户希望先以运行日志检查为主，避免反复要求目视确认。

后续发现 PID231 一次 render pass 拒绝，在 encoder 与 command-buffer 两阶段
各记录一次；两张附件都非空，1080×1920，分别BGRA8与RG32Float。拒绝时间
5268317095 ticks，早于诊断唤醒命令（12637054505 ticks），不能推断为该命令
引起。SBSUndimScreen虽返回，但状态仍为off，scanout为NotReady，未验证实际
唤醒。SpringBoard另有已知texture-layout选择器未实现，尚未归因到本次背景问题。

Metal1.17只在现有25处render pass拒绝分支记录具体原因，并补齐附件storage、
device身份、mipmap/array和十六进制clear数值；未放宽任何谓词。原版Mercury
`0x10000c754` 在 `0x10000d304`、`0x10000d460` 对两个附件均设置(0,0,0,1)，
静态路径不要求非有限浮点清屏值。详细日志需继续定位这次后续拒绝，不能因
初始缓存与管线测试通过而宣布整个目标完成。

## Round 187：物理唤醒确认，保留当前候选

用户实际按电源键唤醒后确认“唤醒后背景正常”。这是本轮物理唤醒的目视证据，
与此前仅调用 SBSUndimScreen、状态仍为 off 的尝试不同。壁纸尺寸沿用此前用户
确认正常的配置。

四个30秒生命周期采样及用户确认后的最终只读查询，Metal failure count 均为1，
始终是启动时 SpringBoard 的 texture-layout 选择器缺失（timestamp1746084045）。
没有新增 Mercury render-pass 拒绝。最终 native 提交/完成3375/3375，compositor
3332/3332，两类 ticket failure 均为0，GPU recovery attempts/count 均为0。
查询时逻辑显示状态又为off；该晚期状态不否定用户的唤醒观察，也不能单独证明
当前仍在主动显示。原始证据见 `round-187-wake-confirmed/uart.txt`。

Metal1.17 改变精确缓存上下文，因此本轮首个 Mercury PID247 重新保存4个函数，
PID254 命中4个并保存第5个后正常提交。跨重启五个函数全部命中、累计94.56ms
的证据属于 Round186 的 Metal1.16；不能将该耗时错误标注为本轮测量。
空缓存仍需正常编译，Hello与背景首次同时可见仍未目视确认。Round186的晚期
拒绝本轮未复现，原因尚未定位；保留新增的具体拒绝原因诊断。

按用户希望以日志检查为主的反馈，保留当前冻结候选，不再为重复确认进行重启。
本轮结束时当前工作区的五个显示相关文件已不同于候选构建回执，未回滚或合入
这些并行修改；上述结论仅覆盖已核验 SHA256 的冻结 EFI。完整日志哈希、计数
及限制保存在 `ui-render-pass-rejection/hardware-evidence.json`。


## Round 188：空缓存编译改善，晚期失败定位到临时附件分配

原先每个片段着色器的 base/MSAA 版本各重复一遍 GLSL 解析和公共 NIR 降级。
Metal1.18 将公共部分每次编译只运行一次，随后克隆 NIR，各自执行与样本数相关的
降级及 IR3 代码生成；仍独立输出两份 ISA，保留原有边界校验和无 promotion 回退。
98个已有样例及原版 Mercury Fluidity 的11个函数（含8种常量组合），成功/失败、
错误文本及完整41912字节输出均与原实现一致。现有编译器边界/清理回归全部通过。
Fluidity 主机总耗时798.32→443.01ms；这是离线测量，不当作真机耗时。

Round188 新缓存上下文下，最早记录的 Mercury PID235 五个函数均未命中，全部
成功保存并提交3个 command buffer：函数总耗时2893.64ms，后端2617.39ms。
相比此前五个函数约4.09–4.46秒，同步编译耗时下降。PID235的出生及退出时刻
没有被12秒窗口捕获，不能据此声称它通过了check-in超时验收。后续PID255五个
函数全部命中，总计45.51ms，首个commit在进程启动后637.26ms；观察到进程存活
63.88秒。用户反馈“目前背景没问题了似乎”，首次Hello那一帧仍未目视确认。

最终查询复现了晚期失败，Metal1.17加入的原因明确为`memoryless-allocation`：
1080×1920 RG32Float附件，storage3，loadClear/storeDontCare；设备、格式、clear值
均合法。PID255在3738823823 ticks的encoder拒绝与3747916868 ticks的command
buffer报告属于同一条失败链。资源统计显示native大缓冲区46次工厂调用、5次失败；
查询当时全局native占用91,537,408字节、54个资源，不能用晚期占用代替失败瞬间
的峰值，也尚未得到底层分配返回码。证据保存在`ui-cold-compile-shared-nir`。

## Round 189候选：按队列复用临时附件

候选Metal1.19让同一memoryless纹理在同一命令队列上保留一份私有附件存储；
其他队列使用独立存储。每个pass仍记录自己的clear/draw/discard命令，操作按队列
FIFO提交，command buffer仍持有内部资源直到真实GPU退休。公开memoryless纹理
仍不暴露backing，load/store与内核资源限制保持原校验。分配失败时另记录实际返回
码、申请尺寸和设备已分配量，避免继续只看到笼统的render-pass拒绝。

扩展原有RG32诊断，覆盖同队列倒序commit、不同队列的不同clear结果，以及预先
编码9个1080×1920 RG32 pass时只增长一份≤16MiB存储。真机像素、队列隔离、倒序
commit均通过；9个全尺寸pass增长16,596,992字节，低于16MiB上限。此结果只验证
临时附件语义和资源占用，不能代替壁纸验收。候选沿用Round188冻结内核与显示配置，未
合入工作区的其他显示修改。

用户随后明确反馈：首轮壁纸延迟出现，动画卡顿并重复旧帧，唤醒后再次消失；受控
唤醒观察为“壁纸稍后才出现”。因此目标仍未完成。该窗口native提交/完成6506/6506，
Metal错误数仍为1（原有SpringBoard缺少texture-layout selector），没有新增Mercury
错误。逻辑亮屏转换已捕获，但窗口末尾Metal环覆盖了唤醒最初几秒，且120秒采样的
批量UART输出触发诊断超时；不能据此计算首帧延迟。改用短间隔保留快照继续定位。

## Round 190：直接呈现前的真实准入

短间隔窗口捕获了完整亮屏转换。Mercury在targetPower亮起采样后约1.8ms已commit，
而逻辑displayState=on约晚1.67秒。其后多笔commit在CPU执行仍排队时就已返回。
原版coordinator在0x1000338ac commit，随后0x100033954直接drawable.present；
本驱动原先在异步worker的实际准入阶段才建立IOSurface producer访问。这给原生
image queue先观察旧内容留下时序窗口，属于源码支持的候选原因，仍需真机对照。

Metal1.21在独立目录从冻结1.19构建。对无显式present的drawable写入，且前序无
未提交预留、未来事件或其他队列未完成fence时，让普通FIFO执行器在commit返回前
完成整份真实像素操作准入。真实Scheduled、硬件完成、IOSurface fence signal规则
保持原样。纹理view继承drawable属性；执行到目标Scheduled即停止内联推进。
未来事件和显式present保留异步路径，没有用marker替代像素准入。

Apple文档说明command-buffer present会在Scheduled后调用drawable present，见
[原版接口说明](https://developer.apple.com/documentation/metal/mtlcommandbuffer/present(_:attime:))。
该候选保留原版Mercury和QuartzCore代码，补齐其直接呈现路径需要的驱动提交边界。

候选HFS逐文件读回、EFI嵌入与冻结内核SHA验证通过。未合入另一任务的Metal1.20
texture-layout SPI和Display1.0.23改动。用户在Round190明确观察到首次Hello与壁纸
一起出现；首个冷缓存Mercury PID246仍记录namespace7/code3初始化超时退出。
同机旧驱动负对照、独立队列/未来事件回归、替代进程及熄屏唤醒稳定性仍待结果，
不能把本次首次出现反馈直接扩大为完整验收。

同机负对照确认旧1.19实际加载自`QcomMTLBefore.bundle`：占住提交串行target时，
直接drawable测试未到Scheduled即返回；1.21通过8帧真实准入、4096像素回读、
纹理view、预留队列倒序commit及未来事件非阻塞检查。原有两队列/共用串行target
fence测试也通过。原版Mercury替代PID262的21笔drawable commit均以真实
Scheduled(3)返回，最长415ms，说明新路径实际覆盖壁纸进程，而非只覆盖诊断。

`presentation-test`在旧1.19与新1.21均因overlap=0返回1；两者FIFO呈现顺序正确、
完成回调均为2。该测试要求后笔Scheduled早于前笔CPU完成，与保留的保守presentation
barrier不符；不能将其标成通过，像素检查也因前置断言未执行。它没有证明本次新增
回归，亦不据此重新启用早先拒绝的呈现重叠优化。当前累计4376笔GPU作业完成、
0失败，Metal错误仍仅原有SpringBoard texture-layout选择器。唤醒验证仍进行中。

## Round 190 收尾：壁纸范围验收与源码合入

用户明确观察到“Hello 与壁纸同时出现”，随后实际熄屏唤醒反馈
“壁纸没消失，首轮回放BUG还在”。进一步澄清，旧帧回放发生在
**Hello 手写文字动画**，不属于本轮“只先解决壁纸”的剩余问题。
结合此前“现在背景显示尺寸正常了”，本轮壁纸首次出现、尺寸和唤醒保持
均得到正向真机反馈。没有将文字动画或整个图形系统标为修复完成。

对应唤醒窗口确实覆盖逻辑off→on，壁纸PID262的11笔唤醒提交均以实际
Scheduled(3)返回，单笔67–458ms。这是提交边界和延迟证据，不是光学帧率，
也没有证明文字回放的根因。完整时序保存在
`build/gpu-runtime/round-190-wake-detail/uart.txt`。

收尾只读检查中，PID262在进程年龄608.9–620.5秒期间持续存活，
12秒观察窗口exits=0/errors=0，footprint约53.4MB；累计12919笔原生GPU作业
和12643笔compositor作业均完成，失败0、recovery0。Metal仍有此前已记录的
SpringBoard缺失texture-layout选择器1项。首个冷缓存PID246的XPC初始化超时
和presentation-test旧版同样失败的overlap断言仍作为限制保留，未改成通过。

已将本轮经过真机验证的驱动差异和direct-drawable-test合入工作区，保留其他
任务已有的texture-layout代码。合并源码版本标为Metal1.22，与真机仍运行的
冻结Metal1.21区分。1.22驱动及回归工具ARM64编译通过，未部署到设备；
没有覆盖共享build/output。源码合并前后快照及验证记录位于
`build/gpu-runtime/ui-direct-drawable-admission/production-merge-verification.json`。
原生直接呈现回归及fence回归的硬件通过记录对应冻结1.21，不能扩大成合并1.22
的完整硬件验收。

壁纸阶段按本轮用户观察收尾；Hello手写动画首轮卡顿/旧帧回放单独留作后续问题。
