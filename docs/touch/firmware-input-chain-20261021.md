# iOS 18 原厂触摸链路核对

## 固定输入

- 原厂内核：`build/ios18-kext-analysis/kernelcache.release.ipad7c.macho`，
  SHA-256 `aa65c36c486a8b5cb1d7a2d565d1d6b3cc228b3c172f8c69c5da664ee19832cf`。
- 原厂用户空间来自本项目 22H374 / iPad iOS 18.7.10 固件；SDK 为 18.6，不能
  用 SDK 版本代替运行固件版本。
- BackBoard：`build/apple-logo-static/root/backboardd`，SHA-256
  `4ec29d4bccb3192f2baf29f4de89e2844e92f1291813274fe39ef98479f1b606`。
- 真机起点为 `hello-card-44`：GPU 1.189.0、Metal 1.52.4；EFI SHA-256
  `ffecbd2659aad0d93691855214ef0faae14713b83bdd060d07cd0a1ed48111bc`。

## 源码和实际机器码的对应

1. 小米固定的 `reference/synaptics_dsx_core.c`：F12 Query5 确定控制寄存器，
   Query8 决定 Data1 偏移；Control28 控制 object / XY / Z / WX / WY 字段。
   本地配置 0xff，对应每个对象 8 字节，finger=1、glove=6。Data15 是原厂读取
   对象数量的优化；本地读取全部已声明对象，尚无证据表明这一差异导致当前故障。
2. 原厂 IOHIDEventDriver `0xfffffff005c91124` 对 input 报告进入
   `0xfffffff005cb17a8`，依据内部 collectionDispatch 选择
   `0xfffffff005cb2518` 聚合，逐个调用 `0xfffffff005cb2e10` 生成子事件。
   其 Tip/InRange/Confidence、ContactIdentifier、XY、Width/Height 分支与固定的
   Apple 参考源码相符。聚合设置 Collection、Touch、Range、EventMask 和重心。
3. 原厂 BackBoard `+[BKHIDDirectTouchEventProcessor digitizerMatchingDictionaries]`
   (`0x10005762c`) 按 Digitizer usage 匹配；`matcher:servicesDidMatch:`
   (`0x100002708`) 将 built-in TouchScreen 识别为主屏 digitizer。
   `processEvent:sender:dispatcher:` (`0x10000a114`) 检查 sender 服务和
   built-in / displayUUID 配对，随后通过 `0x1000056bc` 到
   `0x1000057b8` 解释原厂 digitizer 和子触点。
4. 因此，缺少 AppleMultitouchDriver 本身不能证明根因。通用原厂
   IOHIDEventDriver 已具有并行 Finger collection 路径；应先验证其事件内容和
   BackBoard 所见服务，而非改名冒充 Apple 硬件或直接注入触摸。

## 当前证据的边界

原始真机已发现 S3330、F12、十指、94 字节 HID，原厂 IOHIDEventDriver 也已
匹配。用户触屏期间报告数增长加快，但此前工具只记录成功提交数，未保存原始
触点或原厂事件，所以不能据此宣布物理触摸有效。用户反馈仍无响应。

此前捕获的 1080×1920 logical image 全黑；Display power=1 和 wake-ui 返回
也不足以确认存在可点击界面。需要将输入送达和显示/界面状态一起验证。

## 集中采集

`inputctl registry` 增加 Built-In、DeviceUsagePairs、HIDServiceSupport、
IOUserClientClass、插件、DisplayUUID、原厂 Digitizer 解析结果及 user-client。
驱动保留最后一次有效按下的 RMI/HID 帧和按下/抬起转换计数；trace 模式保存
最近 RMI 状态与数据。这些观测不改变 F12 配置或报告格式。

`inputctl monitor SECONDS` 以只读 Monitor 类型订阅原厂事件，打印原厂事件
description 和用户空间服务属性。仅增加 Apple 工具对应的 event-monitor 与
user-access-service 权限，没有 event-dispatch / filter / admin 权限。
Monitor 类型的枚举与 API 依据 Apple IOHIDEventSystemMonitor.c 及
HIDEventSystemClient.h；来源副本保存在 `build/touch-debug-20261021/apple-reference/`。

采集包由 `build/touch-debug-20261021/package-analysis-capture.py` 构建，
先逐项验证 latest-44 的 24 个 HFS 文件，随后只替换 inputctl。
本地 Kext 只替换触屏观测实现，GPU 使用 latest-44 固定包。此包用于确定故障
层级，尚不能作为触摸修复完成的证据。
