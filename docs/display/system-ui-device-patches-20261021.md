# 启动 Logo 与 OnBoardingKit 真机补丁（2026-10-21）

依据任务[分析 iOS 18 启动 Logo 缺失](codex://threads/01a153b3-9243-70d0-abbe-7882d4d9a848)
和[排查 iOS 18 图标偏小](codex://threads/01a15c26-d0ea-7503-9dc1-c84c0c3e2124)。
目标为项目实际使用的 iPadOS 18.7.10、22H374；设备为 sagit。
工作产物和原始日志保存在 `build/system-ui-patches-20261021/`。

## 已写入的修改

ProgressUI 新增 `apple-logo@3x~iphone.png` 和 `apple-logo-black@3x~iphone.png`。
两份文件逐字节复用固件内对应的 600×600 CgBI PNG，匹配当前 iPhone、scale 3
的原生查找路径；原有六份 iPad 图片保留。未修改 ProgressUI 指令或全局屏幕分类。

OnBoardingKit UUID 为 `c0c3892b9f1538c8baed7401cde5b767`。对
`dyld_shared_cache_arm64.09` 中的 `OBDevice` once 初始化增加 96 字节回退：

1. 原版按设备与屏幕分类完成模板选择后，检查设备 type 为 1 且模板仍为 0。
2. 检查初始化时取得的真实 UIScreen bounds 有限、正值且在合理范围内。
3. 将短边逻辑宽度交给原版 `templateTypeForBoundsWidth:`（`0x19b94f524`），
   再通过原版 setter 写入模板。
4. 继续原版 Home 按钮识别和函数返回流程。

hook 为 `0x19b94f4f4`，实现位于共享缓存已验证的可执行零填充区
`0x19b993880`。360×640 pt 选择模板 8，原生高度表返回 80 pt。
真实分辨率、scale、PPI、MobileGestalt 缓存和 Language.ca 均未修改。
已有有效模板和 iPad 分支保留；没有有效 bounds 时保持原结果。

实现为 `scripts/prepare-system-ui-patches.py`、
`tools/displayctl/onboarding_template_fallback.S`；只读诊断为
`tools/displayctl/uipatchctl.mm`。

## 签名与文件系统验证

设备当前 SystemOS 来源 SHA-256 为
`24069fb7f467a0bf2d5dda35a21a43e76b92b6ab7717759403e6a2283cf159ff`；
此前 keybag、AKS 与 PosterFoundation 兼容补丁均已核对并保留。

补丁重算 `.09` 的原生格式 ad-hoc CodeDirectory，独立复核全部代码页；
更新 Cryptex 文件哈希、祖先节点和 integrity root，再复核派生 seal。
派生 SystemOS SHA-256 为
`4422af822056859424a5094d8d290499d6e50af84cbf10263a582618f1d9feca`，
新增 cdhash 为 `745d50ff60a8c16dd25f6b6fa86a232c7006db93`。
这是本项目工程信任链的派生物，未声称 Apple 认证。

原版和补丁版均在隔离 ARM64 QEMU 中执行 24 组输入，覆盖未知/缺失分类、
正常手机、宽窄屏、旋转、iPad、零值、负值、NaN/无穷值及寄存器保存；全部通过。
这部分使用明确的 MG/UIScreen/ObjC 环境替身，不等同于完整 UI 渲染。
诊断签名的 9 项回归也通过；读取 Setup 所需权限仅绑定到该诊断程序标识。

USB 离线 COW 提交基于设备当前 checkpoint 规划，完整独立 `apfsck -w` 通过。
事务 XID 为 92691，提交 403 块、1,650,688 字节；GPT 未修改。
全部改动块已通过 O_DIRECT 读回，两个新增 PNG、卷身份、fstab 和五个非 System
卷超级块核对通过。完整 3,963,617,280 字节 SystemOS 已从实际设备流式计算哈希，
与派生值一致。

原 `verify-update` 的整文件读取接口不能一次读取 3.96 GB 文件。本轮对资源和卷元数据
使用该检查器，对 SystemOS 使用 APFS CLI 的流式逻辑读取分别验证，未降低块读回或
文件哈希要求。记录见 `device-direct-verified.json`、`device-files-verified.json`、
`device-resources-metadata-verified.json` 和 `cow-validation.json`。

退出导出模式时，首次 status 回包有串口字符错误，重查成功；随后 Linux 存储
载荷在停止 USB gadget 时发生空指针异常，自动重启回 EDK2。主机 fsync 已完成，
未收到设备端刷盘完成回包，不能把 `device_flushed` 记为通过。恢复后重新查询
`edk2-fastboot1` 的产品为 sagit，再启动 XNU；该异常单独保留在
`storage-reboot-retry.log` 和 `storage-recovery.json`。

## 启动载荷

启动测试沿用已通过真机启动的 `hello-card-23` 内核及全部驱动文件，新增补丁信任
缓存和只读诊断工具。内核 SHA-256 保持
`1e4b7724a90fc56441f1d7a04966cce1f832d06e604246169d0330a9852c3a95`。
载荷为 `build/system-ui-patches-20261021/runtime/bridge/XnuBridge.efi`；
后续启动应携带 `prepared/trust-cache.bin`，或直接使用该配套载荷。

## 实机启动结果

系统分区写入后完成两轮 XNU 启动。EFI 入口至已验证的交互式 root Bash 分别为
18.355 秒和 18.238 秒；内核入口至 Bash banner 分别为 14.978 秒和 14.814 秒。
两轮采集均未检测到 XNU panic 或 watchdog 恢复。第二轮之前通过原生 reboot
完成 APFS 同步和卸载，返回 EDK2 后再次启动。

Apple Logo 已由用户在设备上确认可见，实际合成画面也保存了居中的白色 Logo。
原生 ProgressUI 诊断成功加载 600×600 图像；仍有 unsupported screen class 提示，
但不再阻止图片加载。画面为
[boot-logo.png](/home/xxiao/Projects/MI6_iOS/build/system-ui-patches-20261021/boot-logo.png)。

最终语言页中的蓝色地球完整可见。对实际 Setup PID 234 的只读检查确认原生框架
UUID、hook `0x140110e3`、设备 type 1、模板 8、原生图标高度 80 pt。
1080×1920 合成截图中蓝色图形边界为 222×223 像素，按 scale 3 为约 74 pt，
与原生图形在 80 pt 图标区域中的留白一致；这是可见图形尺寸，不等同于布局高度。
最终画面见
[language-final.png](/home/xxiao/Projects/MI6_iOS/build/system-ui-patches-20261021/language-final.png)。
用户也确认地球重新出现。

测试过程中曾出现语言列表已显示、地球暂时不可见的状态；尚未确定原因。
按用户“估计是渲染问题，先不管”的要求，本轮保留现象和截图，不继续排查。
没有为此追加系统补丁。早期一次 PNG 采集和一次显示采样返回 124；第二轮
Setup 尚未初始化时的只读检查返回 2。后续最终模板检查、画面保存及完整导出
均返回 0，不能将早期这些非零结果计为通过，也不能据此归因于本次模板回退。

第二轮载荷仅临时扩展了只读诊断工具，内核、驱动和系统分区补丁与第一轮一致。
临时堆扫描没有产生可信的视图布局测量，不作为结论依据，已从诊断源码移除。
最终源码重新构建后的诊断程序与上文推荐的 `runtime/` 载荷内文件逐字节相同。
没有更改用户的语言选择或推进后续设置流程。

完整结果索引为
[verification.json](/home/xxiao/Projects/MI6_iOS/build/system-ui-patches-20261021/verification.json)，
启动原始报告为 `boot-1/report.json`、`boot-layout/report.json`，最终运行状态与
图像导出记录为 `final-visible/report.json`、`language-final-export.json` 和
`globe-final-measurement.json`。这些结果覆盖本轮补丁与启动、语言页检查，
不代表后续全部 Setup 流程均已测试。

## 回退材料

原始 SystemOS、反向文件块补丁和 APFS 提交前备份均保留在工作目录。
设备启动后 checkpoint 已继续变化；如需回退，应针对届时离线设备状态重新规划
COW 文件更新，不能直接覆盖旧的 APFS 主超级块。
