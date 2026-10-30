# arm64 执行桥

让只有 arm64 版本、带加固的游戏（《阴阳师》各渠道包）能在 ModAVD 的 x86_64 系统镜像上跑起来。0.36.0 起随 Release 主包提供，装在 `sdk\emulator\resources\modavd\`；用 ModAVD 管理器或命令行开机时自动装进实例。

**不修改、不打补丁、不绕过游戏或它的加固库，也不干预任何完整性或防篡改校验。** 这是转译层的兼容性工程：让游戏自己装的钩子正确执行并返回。Intel Houdini 本来就是这么做的，同一个游戏在 Houdini 上就能跑。Google 的转译层二进制我们一个字节都不改。

## 为什么会崩

Google 的 native bridge 让 ART 和系统库保持原生 x86_64，只转译应用自带的 arm64 库，一个进程于是被劈成两半。

游戏的加固库（arm64，被转译）为了盯住 dex 读取，把宿主 `libartbase.so` 的 GNU_RELRO 改成可写，把 `read` / `mmap` 等函数的 GOT 槽改写成它自己的 arm64 钩子地址，再改回只读。真机上两边都是 arm64，到这里一切正常。

在 x86_64 镜像上，原生 x86 的 ART 执行 `call [GOT 槽]`，会跳进 arm64 代码页；转译层在宿主侧把客体代码页映射成不可执行，于是取指出错（`SIGSEGV`、`SEGV_ACCERR`，出错地址就是 RIP）。转译层自己的信号处理函数没有「原生代码跳进客体页」这条分支，直接判成致命错误。

## 两个库

| 目录 | 产物 | 做什么 |
| --- | --- | --- |
| `nbshim/` | `libmodavd_bridge.so` | 我们自己的 native bridge 入口。`ro.dalvik.vm.native.bridge` 指向它，`libnativebridge` 就打开它并取 `NativeBridgeItf`。它打开真正的转译层，把真结构体按版本长度（v6 = 144、v7 = 160、v8 = 168 字节）逐字节拷过来，逐个转发，再把伴生库带起来 |
| `companion/` | `libmodavd_gate.so` | 执行桥本体。用转译层自己导出的接口（`WrapGuestFunctionImpl`、`RunGuestCall`、`GuestMapShadow`、`MakeTrampolineCallable`），从外部补上「原生 x86 经被改写的 GOT 槽调用被转译的 arm64 函数」这条路 |
| `nbshim/modavd_bridge.rc` | — | 对新的属性值重新注册 arm64 的 `binfmt_misc`（镜像自带的那份只认 `libndk_translation.so`，属性一改，arm64 可执行文件就静默跑不起来）；remount 过的实例开机补载 zram |

支持两代转译层：API 34 镜像的 ndk_translation 0.2.3（ModAVD 用的就是这个）和 API 36.1 的 Berberis 16。符号按名字从已映射的镜像里解析，不绑 build-id。

## 只对名单里的应用生效

- 名单是逗号分隔的包名，编进伴生库（`companion/jni/native_to_guest_companion.cc` 的 `kDefaultGateTargets`），现在是《阴阳师》的 7 个渠道包。
- 属性 `modavd.gate.target` 设了就整体替换名单；`setprop modavd.gate.target none` 等于关掉。
- 名单外的应用在进程派生后把转发回调和伴生库改过的槽位都撤回原样，行为和没装桥时一样。

## 信号顺序

执行桥靠 SIGSEGV 工作：原生代码经被改写的 GOT 槽跳进客体页时触发的那次取指错误，就是桥的入口。这个错误必须第一个到我们手里，否则会被别的处理函数当成真崩溃，比如 WebView 的 crashpad。

shim 通过 native bridge 接口的 `getSignalHandler(SIGSEGV)` 把伴生库的判断函数交给 ART。ART 用 libsigchain 把它注册成特殊处理函数，排在所有 `sigaction()` 处理函数之前。它只认我们自己的陷阱，其余一律放行。

## 构建

要 Android NDK r27c。

```powershell
$env:ANDROID_NDK_ROOT = '<NDK r27c 的目录>'
pwsh -File bridge\scripts\build-companion.ps1 [-OutDir <目录>]
pwsh -File bridge\scripts\build-nbshim.ps1 [-OutDir <目录>]
```

- 产物默认在 `%TEMP%\modavd-nbbridge\` 下。
- **绝不要加 `-ftls-model=initial-exec`。** 这两个库都是被 dlopen 的，带 initial-exec TLS 重定位的库会被 Bionic 链接器拒绝，整个 native bridge 加载失败，设备丢掉全部 arm64 支持。`build-nbshim.ps1` 编完会检查，有这类重定位就报错。
- 同一份源码放在不同目录编，产物只有 `.note.gnu.build-id` 不同（build-id 里带了源码路径），去掉这一节后逐字节相同。所以自己编出来的哈希和 Release 里的对不上是正常的；Release 里三个文件的 SHA-256 写在主包根目录的 `modavd-manifest.json` 里。

## 装进实例

- **0.36.0 起不用自己装**：用 ModAVD 管理器 `ModAVD.exe` 或命令行 `tools\modavd.exe start` 开机时，新建的和已有的实例都会自动带好桥：开机前放上开机标记，开机流程做完后桥还没装就装上，没生效就关机冷启动（不在客体里热重启），最多三轮。第一次开机要多 1–2 分钟。装好以后照常用 AUTO-MAS 开机，桥一直在。
- 只用 AUTO-MAS 开机、从没用管理器开过的实例，这一版还不会自动装：用管理器把它开一次机就行。
- 不想要桥：新建时用 `tools\modavd.exe create --no-bridge`，或者对已有实例运行 `tools\modavd.exe bridge <编号> uninstall`，之后开机都不再自动装（再 `install` 就恢复）；`start --no-bridge` 只管这一次。只读检查用 `bridge <编号> verify`（实例要开着）。
- **退回 0.36.0 以前的版本之前，先在每一台实例上运行一次 `tools\modavd.exe bridge <编号> uninstall`**，看到结果是「没装」再换包。不先卸就退版，那台实例可能变成系统属性指着一个已经不存在的库，所有只有 arm64 版本的应用都跑不起来，界面上也看不出来。
- 真正干活的是包里的 `tools\modavd-bridge.ps1`（源码在 [package/](../package/)），上面这些都是调用它。

它负责：

- 实例要按可写系统镜像开机。0.36.0 起的模拟器看到实例目录里的开机标记 `modavd-writable-system` 就会这样开。
- 两个 `.so` 放进 `/system/lib64`（shim 必须在那儿：`libnativebridge` 在链接器的 "system" 命名空间里打开它），`.rc` 放进 `/system/etc/init/`。
- 往 `/vendor/build.prop` 追加 `ro.dalvik.vm.native.bridge=libmodavd_bridge.so`。改之前先备份，写完先核对再重启，重启前先 `sync`。
- 装的顺序是先文件、最后属性，卸的顺序是先属性、再文件。属性是唯一的开关。

这些都写在实例自己的 system / vendor 覆盖层里，不动 Google 的原始镜像。

## 许可证

GNU Affero General Public License 第 3 版或更新版本（AGPL-3.0-or-later），全文见本目录的 [LICENSE](LICENSE)。Copyright 2026 qiyinxi。

`nbshim/jni/native_bridge_shim.cc` 里的 `NativeBridgeCallbacks` 结构体按 AOSP `libnativebridge` 的 `native_bridge.h`（Apache License 2.0）的布局声明，这样 `libnativebridge` 和转译层才认得它。
