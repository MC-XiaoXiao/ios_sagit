# kernelcache_patcher

## 小米 6 项目中的验证

当前副本来自 `Project_iOS2ARM/tools/kernelcache_patcher`，原始文件校验值及本地修改见
`MI6-origin.json`。从项目根目录运行 `make test-patcher` 构建并测试，
`make analyze-kexts` 导出真实 Kext 依赖，`make analyze-abi` 仅用原生符号检查虚表。
`KERNELCACHE=/path/to/decompressed.macho` 可覆盖默认样本。

已修复实例虚表与相邻 MetaClass 虚表混淆、自动猜测覆盖原生符号，以及虚表长度不同时
仍匹配相同前缀的问题。`analyze --native-only` 不加载 sidecar 或进行自动类命名；
`--details` 同时输出注册实例大小。拒绝信息包含类名、原因、槽位、地址及两侧槽数。
被拒绝的虚表不会作为地址映射输出，但分析命令返回成功只表示存在可匹配虚表，
不表示全部 ABI 兼容。当前固件结论和驱动顺序见
[SDM835 分析](../../docs/sdm835/ios18-kext-analysis.md)。

生成缓存时强制调用本项目 `scripts/verify-kernelcache.py`，需要 Python 3 和同目录的
`scripts/analyze-kernelcache.py`，以及用于常量核对的 XNU 源码。
先写入目标目录的 `.pending.*`，通过结构、依赖、导入跳板、原内核字节和源常量校验后
才原子发布。校验器缺失、未知 ADRP 消费者、冲突页面或校验失败均返回非零，删除临时文件，
保留此前已验证的输出。报告写为 `<输出文件>.verification.json`。
从项目根目录执行 `make kernelcache` 生成默认 11-Kext 平台闭包，
`make verify-kernelcache` 可再次核对。当前离线通过的缓存仍需独立的启动与私有 ABI 验证。

以下为参考项目的原有使用说明和验证记录；其中 iOS 12.4 的结果不等同于本项目的 iOS 18 启动验证。

该工具把已发布 iOS kernelcache 中的 Kext，或磁盘上的独立 Kext，重新链接到自行编译的 XNU 内核并生成传统 prelinked kernelcache。

## 当前能力

- 自动读取本地未剥符号内核的 Mach-O 符号表。
- 自动读取发布内核仍保留的导出符号。
- 若发布内核旁存在 `<kernelcache>.symbols.json`，自动加载地址到符号名的签名；手工 CSV 只作为可选覆盖层。
- 按 `ioskextdump_ios10` 的思路扫描 arm64 `OSMetaClass` 构造调用，从 `x1` 类名字符串、`x2` 父类元对象和 `w3` 实例大小恢复被剥符号类，并由元对象/父类引用定位虚表。
- 对源、目标中同名 C++ 类的虚表进行同槽匹配，把没有独立符号名的继承虚函数地址也加入解析表。
- 读取自行编译 Kext 的外部重定位：由子类虚表的 `superClassE` 反推出发布内核中的父类，再用继承槽重定位给父类虚函数命名。
- 线性索引所有写入虚表地址点的构造函数，并用相邻 `C2/C1/D2/D1/D0` 样板恢复非虚构造/析构符号。
- 可自动使用未剥符号参考 Kext 的相对结构恢复类旁全局量和紧邻构造函数的非虚成员；参考文件只提供名称/相对关系，不复制地址。
- 用已有命名槽校验虚表布局。发现命名槽错位时拒绝整张虚表的无名槽匹配，不静默套用错误布局。
- Mach-O C 符号和 Itanium C++ 符号会建立去前导下划线/反混淆别名。
- 仅当去除 arm64e 高位元数据后的地址确实落在映像段中时，才规范化该指针；不会跨架构复用函数地址。
- 仍支持从依赖 Kext 的段范围重定位私有函数和数据。
- 可输出按类、虚表槽号、目标函数地址和符号名组织的分析报告。

本项目给定的 iOS 12.4 发布内核实测恢复 1,461 个类注册、1,218 张实例虚表和 16,308 个由本地内核命名的虚表槽；包含 3 个自编译 Kext 的完整生成耗时约 3 秒。

## 依赖与构建

需要 `tinyxml2`、`capstone`、`keystone` 和支持 C++17 的编译器。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## 分析 C++ 虚表

```sh
kernelcache_patcher analyze \
  ../../build/output/mach.development.bcm2837.unstripped \
  ../../kernels/ios_kernel/kernel_ipadmini4_124/kernelcache.release.ipad5.out
```

默认自动查找发布内核旁的 `.symbols.json`。指定其他签名文件并输出每个虚函数槽：

```sh
kernelcache_patcher analyze <本地未剥符号内核> <发布内核> <symbols.json> --details
```

`--details` 的输出包含类名、源/目标虚表地址、槽号和本地内核中的函数符号，作用类似 `ipsw kernel cpp`/`iometa` 的虚表结构报告。也可以单独使用：

```sh
ipsw kernel cpp --json --inheritance <kernelcache>
```

## 制作 kernelcache

```sh
kernelcache_patcher \
  <本地未剥符号内核> \
  <发布内核> \
  auto \
  <Kext目录> \
  <输出内核> \
  [发布内核补丁列表]
```

第三个参数使用 `auto` 时会尝试加载发布内核旁的 `<kernelcache>.symbols.json`，随后继续执行自动结构分析；使用 `-` 可明确禁用该 sidecar，只依赖内核/Kext 混合识别。也可以传入旧格式 CSV，作为自动结果的显式覆盖：

```text
# 地址,符号名
0xFFFFFFF006E11000,__ZTV10AppleARMIO
```

发布内核的 JSON 签名格式为一个扁平对象：

```json
{
  "18446744005108056656": "vtable for OSArray"
}
```

### Kext 目录

目录中必须有 `kexts.txt`。每行是磁盘 Kext 名称（不带 `.kext`）或发布 kernelcache 中的 bundle ID：

```text
com.apple.kpi.iokit
com.apple.kpi.libkern
com.apple.driver.AppleARMPlatform
```

所有 `OSBundleLibraries` 依赖也必须列入；工具会验证依赖是否存在。磁盘 Kext 布局保持原约定：`Name.kext/Name` 和 `Name.kext/Info.plist`。

### 未剥符号参考 Kext

工具从当前目录逐级向上自动寻找 `kexts/reference`，并按已选发布 Kext 的 bundle 名加载对应的 `Name.kext/Name`。也可显式指定：

```sh
KERNELCACHE_PATCHER_REFERENCE_DIR=/path/to/reference-kexts \
  kernelcache_patcher <本地内核> <发布内核> - <Kext目录> <输出内核>
```

参考 Kext 可以来自相近 iOS/macOS 版本或 arm64e，但只用于类名、符号名、槽位和相对结构。任何最终地址都重新从当前发布 kernelcache 中计算。没有参考文件时仍可完成类、继承、虚表和宏生成构造/析构分析；无法可靠恢复的外部符号会列出并使程序返回非零状态，且不写出损坏的 kernelcache。

### 发布内核补丁

可选补丁文件继续使用发布内核虚拟地址：

```text
# 目标地址
*0xfffffff001234000:
# 32 位补丁值
+d503201f
```

## 验证状态

测试样本：

- 发布内核：`kernels/ios_kernel/kernel_ipadmini4_124/kernelcache.release.ipad5.out`
- 本地内核：`build/output/mach.development.bcm2837.unstripped`
- Kext 闭包：`AppleARMPlatform` 及其依赖，加上 `BCM2837`、`BCM283xInterruptController`、`BCM2837GPIOController`

端到端生成结果已通过：

- AddressSanitizer 与 UndefinedBehaviorSanitizer；
- `file` 的 arm64 Mach-O 识别；
- `llvm-otool` 的 5 个 prelink 段边界检查；
- `ipsw kernel kexts`，13 个 Kext 均可重新枚举；
- `ipsw kernel cpp`，生成内核中的 IOKit 类仍可解析。

## MI6：Kext 静态 ARMv8.0 降级

MI6 分支默认在打包后、原子发布前执行 `scripts/armv8_compat.py`，目标是
SDM835 的 ARMv8-A + CRC + AES/SHA1/SHA256。需要主机 `libLLVM` AArch64
反汇编器及 `libkeystone`；可用 `KERNELCACHE_PATCHER_LLVM_LIBRARY` 指定 LLVM 动态库。
首次使用先 `make build TARGET=SDM835`，确保内核含有 `__ARMV8_EXEC` 映射支持。

原扩展指令改为 `B`，进入 KC 内核的 RX 辅助代码段，再直接 `B` 返回。
无需异常处理器、非法指令陷阱或运行时修改 Kext。辅助代码保留非输出寄存器、SP
和 NZCV；密码指令使用 144 字节临时栈，LSE 原子操作使用 256 字节临时栈。

已支持 SHA512H/H2/SU0/SU1、EOR3、BCAX、RAX1、XAR；同时支持
CAS、SWP、LDADD/LDCLR/LDEOR/LDSET、LDSMAX/LDSMIN/LDUMAX/LDUMIN
的 8/16/32/64 位及 acquire/release 变体，并把 LDAPR 降为更强排序的 LDAR。
CASP、SM4、FP16、dot product、SVE 等未实现指令遇到即拒绝发布。
arm64e 指针认证 ABI 不在本功能范围内。

`__patches` 内嵌记录用于独立离线检查：重新生成每个辅助代码、验证来回分支、
对照原始固件指令、检查整段 Kext 代码及已确认的常量。未知指令、越界跳转、
损坏记录、缺失内核映射支持或 XNU KCGEN 头排列错误都会失败，旧输出保留。

```sh
make kernelcache TARGET=SDM835
make verify-kernelcache
make test-armv8  # 需要 clang、ld.lld、qemu-system-aarch64
```

详见 `docs/sdm835/ios18-kext-analysis.md` 的 ARMv8 降级记录。

## 限制

- 源、目标类的虚表 ABI 必须兼容。命名槽校验能拒绝已观察到的错位，但没有任何共同命名锚点且 ABI 已改变的私有类仍需要手工确认。
- arm64e 参考文件若仍是 IMG4/IM4P 容器，必须先解包为 Mach-O。arm64e 只用于类名、继承和槽顺序参考，不能向普通 arm64 复制地址。
- macOS KDK 或其他版本 Kext 的符号可作为名称/槽顺序参考；版本差异较大时不会覆盖本地布局校验。
- 目前面向传统 prelinked kernelcache；fileset/kernel collection 不是本工具的输出格式。

## 参考

- [`blacktop/ipsw`](https://github.com/blacktop/ipsw) 的 `ipsw kernel cpp`（C++ 类和继承发现）
- [`Siguza/iometa`](https://github.com/Siguza/iometa)（IOKit 类、继承、虚表和虚方法分析）
- [`blacktop/symbolicator`](https://github.com/blacktop/symbolicator)（内核签名；本仓库 `tools/symbolicator`）
- [`userlandkernel/ios-unstripped-kexts`](https://github.com/userlandkernel/ios-unstripped-kexts/tree/master/kexts/10.3/s8000)（旧版未剥符号 Kext 参考）
- [`cocoahuke/ioskextdump_ios10`](https://github.com/cocoahuke/ioskextdump_ios10)（类名字符串、`OSMetaClass` 参数、继承和虚表恢复方法）
