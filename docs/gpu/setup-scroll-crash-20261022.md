# Setup 列表滑动崩溃与黑屏调查（2026-10-22）

用户报告：语言页、地区页尝试滑动后崩溃；地区页继续后出现黑屏和状态栏。
本次首先处理该交互故障，冷启动与首次转场的 GPU 长帧仍需后续测量。

## 已确认的崩溃

前一启动 UUID 为 `2FC4F1CC-8E66-4083-915D-39619FD758F4`。
CrashReporter 中同一启动的 SpringBoard PID 32、422 均因主线程未捕获异常
`SIGABRT / EXC_CRASH` 退出。两次异常回溯都经过：

```
UIScrollView _scrollViewWillBeginDragging
  -> _UIQOSManagedCommitsBegin
  -> work_interval_create(handle, WORK_INTERVAL_TYPE_CA_CLIENT)
  -> NSInternalInconsistencyException: Failed to create work interval
```

原厂 UIKitCore UUID `150b91d9-f37e-359f-ad9d-9526a43b81aa`。
原始地址 `0x1854f0d60` 调用 `work_interval_create`，flags 为 `0x30000000`。
失败分支 `0x1854f0e38` 读取 errno，放进异常 userInfo；
`0x1854f0ec8` 抛出异常。CrashReporter 将其返回地址归到相邻的
`__getSTKPrewarmingViewControllerClass_block_invoke`，该符号名不表示业务根因。

原厂 libsystem 使用 syscall 499 / CREATE2(4)，参数结构大小 16 字节，与当前
XNU ABI 一致。

诊断启动 `CB32AA5F-FF11-4AE5-B498-396E3ACD9A1D` 中，用户再次滑动后，
Setup PID 231 在相同路径 SIGABRT，incident 为
`E6D012EA-20E5-4B19-9B34-6C5245ADF571`，uptime 170 秒。UART 明确记录：

```
WI_CREATE2 pid=231 stage=privilege flags=0x30000000 errno=1 kret=0
```

这里的 `kret=0` 是尚未调用后端时的初值，不表示创建成功。
失败点是 syscall 的 `PRIV_WORK_INTERVAL` 检查，errno 为 `EPERM`。
本次崩溃进程是 Setup，不能与前一启动的 SpringBoard 崩溃混称。

诊断证据位于 `work-interval-trace/launch-setup/uart.bin` 和
`work-interval-trace/user-scroll-crash/export-1-0.txt`；路径均相对于
`build/setup-startup-20261022/`。

## 原厂权限规则与修复

当前固定原厂 provider 的 sysent 499 指向 `0xfffffff0076ec6d8`。
CREATE2 在 `0xfffffff0076eca3c` 读取 flags；`0xfffffff0076eca48` 到
`0xfffffff0076eca50` 判断 CA_CLIENT 并跳过权限检查，进入真实创建后端。
其他受限类型仍调用 `priv_check_cred` (`0xfffffff0076a0654`)。
UNRESTRICTED 分支 `0xfffffff0076ecb44` 要求 GROUP，且仅接受 DEFAULT、
COREAUDIO、CA_CLIENT，否则返回 EINVAL。

项目导入的公开 XNU 11417.140.69 源码无条件检查权限，与实际 iOS 18
内核存在差异。修复在 `kernel/bsd/kern/sys_work_interval.c` 恢复上述
原厂规则，持久补丁为 `tools/patches/xnu-work-interval-ios18.patch`。
后端 `kern_work_interval_create` 未改变，继续检查 CA_CLIENT 应用类型、
一个 task 只能创建一个 CA_CLIENT、禁止 CA_CLIENT GROUP 和线程已有区间。
完整原厂 syscall 反汇编保存为 `work-interval-trace/native-work-interval-ctl.asm`。

候选在 `work-interval-fix/`；生产源码和候选诊断源码均已编译，真机验证结果
待补。当前不能据此宣称滑动、黑屏或启动长帧已经解决。

原始证据位于 `build/setup-startup-20261022/current-sb-crashes/`；
`scroll-crash-find/command-1.txt` 记录报告与当前 boot UUID 的匹配。
UIKit 和 libsystem 的只读反汇编产物也保存在该调查目录。

## 黑屏现场

SpringBoard 重启后为 PID 682，Setup 为 PID 420。
`taskinfo --verbose --threads --dq` 显示 SpringBoard 是 APP_DEFAULT / 前台应用；
Setup 是 APP_DEFAULT / 非 UI 应用。Setup 线程在正常事件循环，未观察到 GPU 等待。
该现场支持继续检查崩溃后的场景恢复，尚不能证明黑屏的全部因果链。

`scroll-task-policy/export-1-0.txt` 的 GPU 统计仍在前进，未观察到本次滑动后新增
原生提交失败。不能将本次 UIKit 异常视为 A540 硬件完成或 fence 卡死。

## 确认缺少的能力

`/usr/sbin/wifid` 和 launchd plist 均存在，plist 有设备树匹配限制
`wlan` / `marconi-wifi`。当前没有 wifid 进程，日志持续报告 WiFiManager
server bootstrap lookup 失败。项目 WLAN 尚未实现可用扫描、关联及数据收发；
见 `docs/wifi/README.md`。这是后续 Setup 网络流程的实际缺口，尚未证明它是
本次滑动崩溃原因。地区页之后也不能直接假设必定是 Wi-Fi 页。

## 诊断载荷约束

父载荷为 `build/touch-debug-20261022/main-display-fix`，保留 GPU 1.189.0、
Metal 1.52.4、Display 1.0.24 和最新触摸修复。
父 EFI SHA256 `28b48d264b541e623062bab7c07ede03e2c29788eba30f096851fdd6a04bc5b8`。

诊断仅在 CREATE2 的 CA_CLIENT 路径记录权限检查及创建返回，最多 64 条，
由临时启动参数 `wi=1` 启用。不修改返回值、权限授予、调度、GPU 完成或 fence。
仅重新编译该 syscall 对象，使用与父 kernel 输入 SHA256 一致的旧构建目录
其余对象重链接，并保留父 collection 的所有 kext 与 userspace 镜像。
原始诊断源、补丁、命令和日志在
`build/setup-startup-20261022/work-interval-trace/`。
修复候选沿用相同对象替换方式，其他 collection 输入保持一致。
