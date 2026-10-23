# iOS 18 主屏触摸投递修复

2026-10-22，sagit / Mi 6，原厂 iPad arm64 iOS 18.7.10 / 22H374。

## 故障与根因

修正 HID 报告后，原厂 IOHIDEventDriver 已产生完整的 Hand/Finger 按下、抬起，
但亮屏语言页点击 English 仍没有变化。BackBoard 的 touchstats 记录将触点
送往 SpringBoard，且每帧出现：

```text
BKDisplayConvertCAScreenLocationToContext: unknown displayUUID:(null)
```

原厂 `processEvent:sender:dispatcher:` (`0x10000a114`) 接受 built-in
TouchScreen 时要求服务的 displayUUID 为空，因此不能给内置触屏补一个
非空 DisplayUUID。进一步核对显示几何后确认：

- `sub_100003CE8` 通过 `sub_100003DFC` 和 `sub_1000341BC` 读取主
  CAWindowServerDisplay 的 uniqueId，与传入的显示 UUID 比较。
- QcomDisplay 原来给内置 framebuffer 发布 `IOMFBUUID`，使主屏
  uniqueId 为 `D60C6B4E-4EDA-4456-9C57-960E7509100D`。
- 内置触屏的 nil UUID 因而不能匹配主屏。`sub_100009A74` 的几何读取
  (`sub_1000053E0`) 进入 CADisplay 的 UUID 查找，得到空对象，屏幕宽高为零。
  归一化坐标失去对应的屏幕位置，后续 context 转换也找不到显示对象。

原始二进制 SHA-256 为
`4ec29d4bccb3192f2baf29f4de89e2844e92f1291813274fe39ef98479f1b606`，
保存的反编译、地址与依赖见
`build/touch-debug-20261022/native-display-routing.json` 及
`native-main-display-2.json`、`native-main-display-3.json`。

## 实现

QcomDisplay 1.0.24 保留 loader 提供的面板 UUID 校验，将诊断属性改为
`QcomPanelUUID`，不再为内置 framebuffer 发布 `IOMFBUUID`。主屏和原生
内置 digitizer 都按 nil UUID 关联；IOHIDEventDriver、BackBoard、Setup
继续使用原厂实现。

本次候选同时包含此前触屏侧修复：无压力轴报告的宽高字段位于正确偏移；
抬起帧保留最后成功坐标。压力字段保留为 RMI 诊断，未声明为 HID 压力。
仅修正 HID 偏移的候选仍失败，显示身份修复后的候选才观察到页面响应。

## 已验证的真机行为

所有页面操作都从设备端连续执行，并记录亮屏状态和点击前后图片。
`hid-tap` 通过 SynapticsTouch 的同一报告编码、IOHIDDevice、原厂
IOHIDEventDriver 与 BackBoard；日志显式标为 `physical_input=0`。

| 操作 | 结果 | 证据目录（相对 build/touch-debug-20261022） |
| --- | --- | --- |
| 语言页 English，(0.5, 0.366) | 进入 Select Your Country or Region | `main-display-language` |
| 地区页左上返回，(0.043, 0.065) | 返回语言列表 | `main-display-menu-contiguous` |
| 语言页二次 Home | 显示 Emergency Call / Accessibility Options / Start Over / Cancel | `main-display-menu-contiguous` |
| 菜单 Cancel，(0.5, 0.944) | 菜单关闭，返回语言列表 | `main-display-menu-cancel` |
| Hello 右下角信息 | 未验收；唤醒后只捕获到壁纸，用户明确允许跳过 | `main-display-hello-awake`、`main-display-hello-power` |

修复前后相同语言页缩略图 SHA-256 为
`eb4ebf6e8f31a68ed8ebce724e5c5edb460d6ed8f8ec95a51e9821e76ab5f6e4`。
修复前点击后仍为该图；修复后进入地区页。
修复后的 touchstats 目的地为 `Setup.261 contextID CB9BFD5`，触点 ID 为 1，
该段日志没有 `unknown displayUUID`。菜单关闭后语言列表的图片亦与上面相同。

`main-display-final-runtime` 中真实 S3330 / F12 / 十指服务仍在线：
`RMILastError=0`、`HIDInputOffline=0`、原厂 IOHIDEventDriver 已匹配。
显示状态为 1080×1920，`Location=Internal`，没有 IOMFBUUID，
`DisplayPowerTransitionError=0`。

后续用相同候选再次启动，Home 仍可从熄屏唤醒并进入语言列表。
用户于本轮明确表示“hello看不见就算了”，因此停止 Hello 显示调查，
不将 Apple 标志、纯壁纸或其上的点击算作 Hello 信息按钮通过。
收尾时设备返回语言列表，证据为
`main-display-start-over/export-3-0.png`；此后允许原生计时自动熄屏。

无效尝试仍保留：`main-display-menu-actions` 最初标作 Cancel 的点击实际
进入德语地区页，已从通过项排除，正式菜单结论采用上表独立的
`main-display-menu-cancel`。`main-display-hello-info` 首图是 Apple 标志，
随后图是壁纸，也已排除。

## 构建与交付

- `make test-touch`：I²C、触摸宿主测试和 9 项 Python 集成测试通过。
- `make test-display`：显示、MIPI DSI、驱动构建检查和显示引导桥测试通过。
- QcomDisplay 交叉构建通过 arm64 Kext / ABI / 导入校验。
- 78 个 Kext 的完整候选 kernelcache 验证通过。
- 真机以 Fastboot stage + CRC32 + run-efi 临时启动，无持久分区刷写。

候选：`build/touch-debug-20261022/main-display-fix/bridge/XnuBridge.efi`。
payload SHA-256：
`28b48d264b541e623062bab7c07ede03e2c29788eba30f096851fdd6a04bc5b8`。
下载 CRC32：`1da210bf`。
内核 SHA-256：
`70581a36374ec92896e7614bd8e4423872282a2da532c3059350214766f72556`。

打包脚本 `build/touch-debug-20261022/package-main-display.py` 从先前运行的
touch-fix 候选开始，只替换 QcomDisplay 的二进制和 plist。GPU、Metal、
用户空间工具及系统镜像保持该候选的版本；完整构建参数、差异、校验和启动
证据都在该目录。

本次汇总为 [validation-main-display-20261022.json](validation-main-display-20261022.json)，
记录候选校验和、源码校验和、逐项截图和验收边界；旧 `validation.json`
是 2026-10-20 的离线历史记录，不代表本次真机状态。

## 验证边界

远程测试验证的是驱动报告经原厂解析和投递后确实驱动了真机页面，不是只看
计数。用户不在设备旁，本轮没有新的物理手指或光学摄像头验收；实际面板
感测、GPIO 电气行为、全屏拖动和多指仍应与这些远程页面结果区分。
Hello 自动熄屏及 Home/电源唤醒保持固件逻辑。唤醒后可能先显示壁纸，不能
凭 display=1 或截图文件名将其判为 Hello。
