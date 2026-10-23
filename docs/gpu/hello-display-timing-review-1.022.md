# Display 1.0.20–1.0.22：冷编译、显示相位与映射复用

2026-10-19。本轮一次采集 GPU、Metal CPU 阶段、硬 IRQ、显示 workloop、原生读 fence 和映射生命周期；没有降低原生完成条件。

## 结论

- 冷启动的 25 次函数专用化在三轮均重新发生，平均每次约 531–534 ms。AIR lowering 约 133 ms，后端编译约 391 ms；两者嵌套在函数时间内，不能重复相加。编译锁等待约 1 µs，不是主要原因。当前尚未实现透明持久化编译缓存。
- 显示映射复用已经真机命中。177→245 帧之间，命中175→242，映射创建保持3，淘汰0；原生读 fence 每次仍重新同步。
- 准备阶段中位数由121版3.773 ms降至122版0.084 ms；旧映射处理由3.485 ms降至0.006 ms。这是具体CPU路径收益，不能据此宣称60 FPS。
- Display122的实显间隔中位数仍约33.35 ms。多数启动发生在读计数7–9，等待约16.5 ms到下一次TE，随后传输约14.3 ms；提交相位与原生VBL调度仍须结合分析。
- GPU真实计数约596 MHz；没有测得运行时CPU频率。UEFI调试时钟不是CPU运行频率。

## 原实现与设备参数

实际sagit DTB的JDI r63452 command面板为1080×1920、60Hz、RGB888、4 lanes。MDSS流寄存器也匹配3241-byte命令行与1080×1920。公共固件显示时钟测量返回0，属于不可用结果。

固件autorefresh配置原为start=5、read/write初值0。Display121按本地固定版本Linux DPU原实现设置read/start=1920、read IRQ=1921、write init=1925，保留已知synthetic周期1927、阈值和external TE选择。没有盲开外部TE或修改DSI频率。参考`docs/display/reference/dpu_encoder_phys_cmd.c`、`dpu_hw_pingpong.c`和`sources.json`。

AppleDisplayPipe原实现使用池化IODMACommand；原生prepare/read fence与完成所有权保持。Display122仅保留最多两个已经结束硬件读取的闲置映射，匹配同一个保留IOSurface，复用后仍按新的原生读访问同步。销毁和淘汰走既有回收路径，SMMU故障中断保持。没有提前报告PP_DONE，没有自动重复扫描。

## 真机证据与限制

- Round166，GPU144/Metal1.6/Display120：原生fence通过，CARenderer八帧每帧4096像素正确；硬IRQ至worker中位数0.078 ms。准备、旧映射回收各约3.5 ms。被动亮屏采集初始超时，保留失败状态。
- Round167，Display121：主采集4.73秒无新GPU作业且已熄屏，不能作性能对照。此前33次物理传输正常、无native失败；同一Metal二进制仍重新编译25个函数。
- Round168，Display122：runner退出0，四核，控制命令状态[0,1,0]，无内核panic。ASan/UBSan显示所有权测试通过；GPU/Metal未改，沿用166原生正确性证据。180次完整物理传输，硬IRQ至worker中位数0.062 ms，PP_DONE后回调中位数0.080 ms；没有看到驱动失败。
- Round168的4.10秒窗口145个GPU作业完成、失败0，但最后约0.44秒逻辑熄屏。power_status=1，整个窗口不能作为持续亮屏FPS验收；原始数据和失败状态均保留。
- 独立全屏画面内容顺序和光学闪烁尚未验收；单调swap token不能证明没有语义帧回退。

证据：`build/gpu-runtime/round-{166,167,168}/analysis.json`，`ui-display-map-reuse/hardware-evidence.json`、`embedded-verification.json`。下一步同时推进原生Mercury初始化修复和冷编译缓存；不重复不变的GPU全套对照。
