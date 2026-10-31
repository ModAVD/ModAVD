# arm64 执行桥

让只有 arm64 版本、带加固的游戏（《阴阳师》各渠道包、《如鸢》国服）能在 ModAVD 的 x86_64 系统镜像上跑起来。执行桥从 0.36.0 起随 Release 主包提供，装在 `sdk\emulator\resources\modavd\`；用 ModAVD 管理器或命令行开机时自动装进实例。《如鸢》的 raw clone 保护是本次新增，0.36.0 的原版伴生库不含这项修复。

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

- 名单是逗号分隔的包名，编进伴生库（`companion/jni/native_to_guest_companion.cc` 的 `kDefaultGateTargets`），包含《阴阳师》的 7 个渠道包和《如鸢》国服 `com.lingxigames.yuan.cn`。
- 属性 `modavd.gate.target` 设了就整体替换名单；`setprop modavd.gate.target none` 等于关掉。
- 名单外的应用在进程派生后把转发回调和伴生库改过的槽位都撤回原样，行为和没装桥时一样。

## API 34 的 raw clone 保护

《如鸢》国服的启动路径会经过旧版转译层的 `CloneGuestThread`：它把空栈的 `CLONE_VM | CLONE_VFORK` 请求转成独立地址空间的 raw clone，直接经 `syscall` 调用宿主内核。这个调用不经过 Bionic `fork()`，原来的 `pthread_atfork` 回调不会运行；子进程会继承其他父线程持有的转译层互斥锁，父线程则停在 `kernel_clone` 等子进程退出。调查见 [Issue #20](https://github.com/ModAVD/ModAVD/issues/20)。

伴生库现在也接管转译层自己的 `syscall` GOT 槽。只有已启用 fork 保护、不带 `CLONE_VM`、宿主栈为空，且不带 `CLONE_SETTLS` / `CLONE_PARENT_SETTID` / `CLONE_CHILD_SETTID` / `CLONE_PIDFD` 的 `SYS_clone` 才进入保护函数。这些标志可能改变宿主 TLS，或通过 tid / pidfd 输出地址覆盖缓存的 tid，必须在汇编入口直接透传。保护的三把转译锁分别属于默认 code pool、TranslationCache 和子进程重置时需要的 thread table。锁偏移只对经过 build-id 核对的 API 34 转译层启用，未知版本不使用这些偏移。

只解锁还不够：fork 快照中可以有其他线程尚未完成的缓存转换，子进程里没有那个 worker，便一直停在 `Translating`、`Invalidating` 或 `Wrapping` 入口。`InvalidateEntriesBeingTranslatedUnsafe` 只把 `Translating` 改成 `Invalidating` 并清空进行中集合，仍依赖原 worker 收尾，不能作为 fork 子进程恢复。

保护现在先在三锁下确认缓存静止：进行中计数必须为 0，主 map 的每个实际 dispatch slot 都不能是上述三种入口。不满足就释放本次取得的三锁、退出 loader callback，再让原 worker 前进后重试；成功才 fork。父子进程都不改写缓存。主 map 的只读布局和 stub 符号由精确 API 34 build-id `553ba82b76bb5a11fd1d67b2c7e01b75` 限定，实例地址与缓存锁地址也交叉检查；符号缺失不启用，结构无法判定则拒绝该次保护并记录。普通 Bionic `fork()` 的 prepare 使用同一个完整条件。

另一轮冷启动还复现了子进程在 `ResetAllExecRegions` 的 `dlopen` 中等待继承的 linker 互斥锁。raw clone 保护因此先进入公开的 `dl_iterate_phdr`，利用它在 callback 期间持有的 linker 递归锁，再在 callback 内仅尝试一次三把转译锁及缓存检查。失败就立即释放已取得的转译锁、退出 callback，让等待 loader 的转译线程可以继续，随后在外层重试；成功才发出原 raw clone。父子进程都先退出 callback，正常释放本次新增的一层 linker 锁，再执行 `ForkParent` / `ForkChild`。这个子集保留原宿主缓存 tid，满足 linker 递归解锁的 owner 检查；不会清空或重建 linker 的内部锁。普通 Bionic `fork()` 仍用原 `pthread_atfork` 路径，不套这个 callback。

其余系统调用经 x86_64 汇编尾跳转原样转发，保留零到六个可变参数以及返回值；名单外应用撤回全部五个 GOT 槽。没有修改 Google 二进制或游戏文件，也没有改变 clone 标志和参数。父线程在 `CLONE_VFORK` 等待子进程期间仍持有 linker 和三把转译锁。原 200 ms 预算限制取锁或缓存繁忙后的重试，超时仍告警并执行未保护的原调用，原挂起风险仍然存在；`dl_iterate_phdr` 等待 linker 本身是阻塞的，所以整个保护函数没有 200 ms 的硬上限。静止检查为 O(n) 主 map 扫描，日志 `cache_entries`、`scan_nodes`、`scan_us` 记录本次规模与成本；`wait_us` 包含 loader 等待和缓存检查，`protected_us` 包含 vfork 等待，不能当作补丁净额外耗时。

原生测试直接编译实际保护实现，覆盖 syscall 参数与 TLS/tid 变更标志透传、目标开关、共享 VM / 独立宿主栈透传、父子和错误返回、锁竞争。三种模拟转换态都必须等 worker 完成才能 fork；持续繁忙则测试原调用透传、errno 和不改写状态。真实内核 fork / vfork 验证父子 cache 保持不变、子进程可再次 `dl_iterate_phdr` / `dlopen`、loader 递归层级及反向锁序退让。编译后在测试实例运行：

```powershell
& '<NDK>\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe' --target=x86_64-linux-android30 -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -static-libstdc++ bridge\tests\raw_clone_test.cc -llog -ldl -o '<输出>\raw-clone-test'
& '<adb>' -P <私有端口> -s <测试实例> push '<输出>\raw-clone-test' /data/local/tmp/modavd-raw-clone-test
& '<adb>' -P <私有端口> -s <测试实例> shell chmod 755 /data/local/tmp/modavd-raw-clone-test
& '<adb>' -P <私有端口> -s <测试实例> shell /data/local/tmp/modavd-raw-clone-test
```

独立的 `cache_quiescence_test.cc` 还在自己的原生进程里加载真实 Google 库，仅构造缓存元数据、不执行合成 guest PC。它验证 `NotTranslated → Translating → Invalidating → NotTranslated` 的真实两阶段行为、fork 的 COW 隔离、`Wrapping`（进行中计数也为 0）及同一生产取锁入口的接受/拒绝，另记录 9,664 个 ready 节点的扫描时间。先确认 `/system/lib64/libndk_translation.so` SHA-256 为 `a2086fbc807f697f9b91222065c9d9000ca9727748699c0d9b751319258cbb3e`；测试本身也拒绝不同 build-id。完整收尾 API 只用于该独立测试，生产路径不调用它。

```powershell
& '<NDK>\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe' --target=x86_64-linux-android30 -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -static-libstdc++ bridge\tests\cache_quiescence_test.cc -llog -ldl -o '<输出>\cache-quiescence-test'
& '<adb>' -P <私有端口> -s <测试实例> push '<输出>\cache-quiescence-test' /data/local/tmp/cache-quiescence-test
& '<adb>' -P <私有端口> -s <测试实例> shell chmod 755 /data/local/tmp/cache-quiescence-test
& '<adb>' -P <私有端口> -s <测试实例> shell /data/local/tmp/cache-quiescence-test translate
& '<adb>' -P <私有端口> -s <测试实例> shell /data/local/tmp/cache-quiescence-test wrap
```

原生测试通过还需要真实游戏验证：如鸢冷启动到资源更新提示、名单内旧游戏的运行、名单外应用撤回 `5/5`。协议页或资源更新提示只能证明该阶段启动成功，不能代替登录、庭院、战斗和长期运行回归。

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
