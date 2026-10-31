# arm64 执行桥

让只有 arm64 版本、带加固的游戏（《阴阳师》各渠道包、《如鸢》国服）能在 ModAVD 的 x86_64 系统镜像上跑起来。执行桥从 0.36.0 起随 Release 主包提供，装在 `sdk\emulator\resources\modavd\`；用 ModAVD 管理器或命令行开机时自动装进实例。《如鸢》的 raw clone 保护由本 PR 新增，以合并后首次包含该改动的 Release 为发布起点；这里不预告尚未发布的版本号。

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

- 名单是逗号分隔的包名，编进伴生库（`companion/jni/native_to_guest_companion.cc` 的 `kDefaultGateTargets`），包含《阴阳师》的 7 个渠道包和《如鸢》国服 `com.lingxigames.yuan.cn`；如鸢加入名单是为了启用下述 fork 兼容保护。
- 属性 `modavd.gate.target` 设了就整体替换名单；`setprop modavd.gate.target none` 等于关掉。
- 名单外的应用在进程派生后把转发回调和伴生库改过的槽位都撤回原样，行为和没装桥时一样。

## API 34 的 raw clone 保护

《如鸢》国服的启动路径会经过旧版转译层的 `CloneGuestThread`：它把空栈的 `CLONE_VM | CLONE_VFORK` 请求转成独立地址空间的 raw clone，直接经 `syscall` 调用宿主内核。这个调用不经过 Bionic `fork()`，原来的 `pthread_atfork` 回调不会运行；子进程会继承其他父线程持有的转译层互斥锁，父线程则停在 `kernel_clone` 等子进程退出。调查见 [Issue #20](https://github.com/ModAVD/ModAVD/issues/20)。

伴生库也接管转译层自己的 `syscall` GOT 槽。已启用 fork 保护、不带 `CLONE_VM`、宿主栈为空的 `SYS_clone` 才可能进入保护；`CLONE_SETTLS` 和 `CLONE_PIDFD` 始终在汇编入口尾跳原调用。`CLONE_PARENT_SETTID`、`CLONE_CHILD_SETTID` 及 `CLONE_CHILD_CLEARTID` 可以进入保护：在原 clone 前、loader/三把转译锁之外，检查活动输出的完整 4 字节区间，不得与当前宿主 TP、`pthread_self()` 或栈标记所在 VMA 相交。区间溢出、maps 不可读或无法识别上述 VMA，都保守尾跳；不猜宿主 pthread 的私有尺寸。该检查会拒绝同 VMA 中一些本来有效的宿主地址，客体输出也必须以实际映射分类，不能因它名为 guest 指针就省略检查。非活动 tid 参数不参与判断，原 flags 和指针不改写。

预检保存和恢复入口 errno；naked 入口保存六个 GP 参数和 RAX/AL，检查返回后还原原 RSP。unsafe 分支随后直接尾跳原 syscall，原 clone 返回时不经过伴生库的 C++ 尾声。关闭保护时也直接尾跳。

本 API 34 镜像实测区分两路。普通 ARM64 guest libc `fork()` 的公共符号归属 `/system/lib64/arm64/libc.so`，其内部 `__clone_for_fork` 经 Google `libndk_translation_proxy_libc.so` 的 thunk 调宿主 Bionic `fork()`，子进程再重置当前 guest thread。精确 proxy 的静态调用链与独立 host-atfork 父回调标记一致；这条路使用 `pthread_atfork` 的 prepare/parent/child，不套 raw clone 的 loader callback。公共 fork 的 dladdr 归属本身不能证明内部分流。显式 guest `SYS_clone` 则可走转译层 syscall GOT：本次增加的是该路对安全 tid 输出的 SETTID 覆盖，包括 `0x1200011`（CHILD_SETTID、CHILD_CLEARTID、SIGCHLD）及带 PARENT_SETTID 的 `0x1300011`。不能把普通导入 fork 的成功计为 raw GOT 覆盖，其他镜像或输出地址也不能仅由本次结果推断。

保护的三把转译锁分别属于默认 code pool、TranslationCache 和子进程重置需要的 thread table。偏移只对经过 build-id 核对的 API 34 转译层启用，未知版本不使用这些偏移。GOT 安装在公开 loader 作用域内序列化，重入、detach 和已安装状态再次检查；syscall 原值先于 wrapper 发布，并明确拒绝将 wrapper 自身保存为原值。

只解锁还不够：fork 快照中可以有其他线程尚未完成的缓存转换，子进程里没有那个 worker，便一直停在 `Translating`、`Invalidating` 或 `Wrapping` 入口。`InvalidateEntriesBeingTranslatedUnsafe` 只把 `Translating` 改成 `Invalidating` 并清空进行中集合，仍依赖原 worker 收尾，不能作为 fork 子进程恢复。

保护先在三锁下确认缓存静止：进行中计数必须为 0，主 map 的每个实际 dispatch slot 都不能是上述三种入口。取锁忙和 cache Busy 分开返回；Busy 时先释放本次三锁、退出 loader callback，再短暂 nanosleep 给 worker 收尾，然后重试。结构或 stub 无法判定是 Unknown：持三锁时永久关闭本进程的新 guard admission，退出锁和 loader 后明确告警，本次立即执行未保护原调用，不再重复扫描 Unknown，也不自动重新启用。已持锁者的 owner/held 和已初始化指针不会被禁用动作改写。主 map 的只读布局及 stub 符号由精确 API 34 build-id `553ba82b76bb5a11fd1d67b2c7e01b75` 限定，实例地址与 TranslationCache mutex 交叉核对；符号缺失不启用。宿主 Bionic fork prepare 使用相同完整条件。父子进程都不改写缓存；子进程包括超时回退分支都执行 ForkChild，释放它继承的 guard 锁，父分支只释放本线程取得的锁。释放时在 unlock 前清 held/owner，避免清掉下一位持锁者的新状态。

另一轮冷启动还复现了子进程在 `ResetAllExecRegions` 的 `dlopen` 中等待继承的 linker 互斥锁。raw clone 保护因此先进入公开的 `dl_iterate_phdr`，利用它在 callback 期间持有的 linker 递归锁，再在 callback 内仅尝试一次三把转译锁及缓存检查。失败就立即释放已取得的转译锁、退出 callback，让等待 loader 的转译线程可以继续，随后在外层重试；成功才发出原 raw clone。父子进程都先退出 callback，正常释放本次新增的一层 linker 锁，再执行 `ForkParent` / `ForkChild`。这个子集保留原宿主缓存 tid，满足 linker 递归解锁的 owner 检查；不会清空或重建 linker 的内部锁。普通 Bionic `fork()` 仍用原 `pthread_atfork` 路径，不套这个 callback。

其余系统调用经 x86_64 汇编尾跳原样转发，保留零到六个可变参数、64 位返回值和 errno；受保护 clone 使用其原始五个参数。名单外应用撤回全部五个 GOT 槽。没有修改 Google 二进制或游戏文件，也没有改变 clone 标志和参数。父线程在 `CLONE_VFORK` 等待子进程期间仍持有 linker 和三把转译锁，需要这些锁的父线程会暂停；子进程若卡在其他未覆盖的资源上，可能扩大到整个进程停顿。原 200 ms 预算限制取锁或 cache Busy 的重试，超时告警并执行未保护原调用，原挂起风险仍在；公开 `dl_iterate_phdr` 等待 linker 本身是阻塞的，所以整个保护函数没有 200 ms 硬上限。

静止检查为 O(n) 主 map 扫描。如鸢实测约 28 万项时 scan 为 15–25 ms，不能用独立 9,664 个 NotTranslated 节点的几十微秒代表游戏成本。默认只记录 miss、Unknown 和准备加 protected 达到 100 ms 的慢调用；目标进程启用保护前设置 `setprop modavd.gate.forkdiag 1` 可开启逐条诊断，默认 0，改属性后需新进程重新读取。详细日志含 flags、SETTID 地址与宿主 VMA 分类、`cache_entries`、`scan_nodes`、`scan_us`、`wait_us` 和 `protected_us`，用于确认显式 guest clone 实际走槽；宿主回调另以 `host fork atfork parent: held=...` 单列，在解锁后、仅父分支记录并保留 errno，raw guard 不产生该标记。scan_us 仅最后一次扫描；wait_us 含 loader/取锁/扫描/重试。protected_us 从成功扫描后到父分支释放为止，包含 vfork 子进程运行/等待，但不含之前已经持锁的扫描时间，因此既不是四锁全部持有时间，也不是补丁净额外耗时。

修订 f6662a1 的如鸢 Default 与 6-24 观测合计有 202 条**完成**的 vfork guard，均 held=1；protected_us 的 P50 为 28.820 ms、P95 为 35.4846 ms、最大 61.880 ms（按 (n−1)×p 线性插值，窗口包含准备/空闲）。这些完成记录不能约束未完成调用或未来子进程挂起。Default 最大呈现间隔约 9.131 s，估算 guest UTC 为 05:02:47.442–05:02:56.573；其中确有两条短 guard：05:02:49.259 的 wait/protected 为 9.182/21.547 ms，05:02:49.317 为 9.005/23.879 ms。它们的估算准备加保护合计约 63.613 ms，没有一条已完成 guard 持续 9.13 s；但日志在释放后产生、调度/日志延迟未知，时钟配对在同一 boot 的较晚时刻取得，也未独立排除更早的时钟校准，所以不能宣称这次长间隔与 guard 完全无关，或排除间接影响、未记录/未完成调用。

维护者对 **f6662a1** 另做了阴阳师官服同场景回归：MaaYYs 冷启、庭院 60 s、御魂壹层 3 场通过，战斗 57.2/57.7/57.1 fps，对照官方 55.8–57.4 fps 未见明显退化。点进入游戏后 1 s 内出现 15 次 vfork，cache 36.5–37 万项；scan P50 20 ms/最大 28 ms、wait P50 21 ms/P95 74 ms/最大 155 ms、protected P50 25 ms/最大 45 ms，全部 held=1、miss/Unknown 为 0，战斗期间无 guard。这是维护者的外部旧修订回归，不能冒充本次评审修复的设备结果；15 次冷启动当时仍在进行。见[维护者记录](https://github.com/ModAVD/ModAVD/pull/25#issuecomment-6542251111)。

原生测试直接编译实际实现，覆盖零到六参数尾跳、64 位返回值/errno、unsafe tid 预检后的 AL/原 RSP/第六栈参、SETTLS/PIDFD 透传、共享 VM/非空宿主栈、fork-safety 开关（实际切换 g_fork_safety，不是完整包名门控）。SETTID 覆盖活动/非活动指针、NULL、别名、4 字节跨 VMA 边界/溢出、maps 缺失时的 errno、主线程/worker 的宿主 TLS/pthread/stack，以及独立带 guard page 的 guest-like tid 页。真实内核 SETTID fork 验证输出和父子 COW；原生的 guest-like 页与下面 ARM fixture 的独立 mmap 都只证明各自输出地址，不代表所有 guest pthread/tid 地址均会通过保守 VMA 检查。

同时覆盖超时子进程释放另一线程 prepare 的继承锁、父进程保留那位 owner、owner 并发换手、Unknown 禁用且不重扫、Busy 退让、原 syscall 自钩拒绝/并发原值发布、安装重入/detach、默认与详细诊断判据。三种模拟转换态须等 worker 完成才能 clone，持续 Busy 测原调用/errno/不改写；新增 37 万合成节点、15 次串行 burst，每次最后节点为 Invalidating 且 inflight=0，worker 需得到释放后的 cache 锁才能收尾，clone 处再次完整检查全树。这是 synthetic 功能/退让压力，包含额外核验且不是游戏性能或 1 秒内真实 vfork 复现。真实内核 fork/vfork 另核父子 cache 不变、子进程再用 dliterate/dlopen、loader 递归层级和反向锁序。构建通过不代表这些运行测试已经通过；编译后在测试实例执行：

```powershell
& '<NDK>\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe' --target=x86_64-linux-android30 -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -static-libstdc++ bridge\tests\raw_clone_test.cc -llog -ldl -o '<输出>\raw-clone-test'
& '<adb>' -P <私有端口> -s <测试实例> push '<输出>\raw-clone-test' /data/local/tmp/modavd-raw-clone-test
& '<adb>' -P <私有端口> -s <测试实例> shell chmod 755 /data/local/tmp/modavd-raw-clone-test
& '<adb>' -P <私有端口> -s <测试实例> shell /data/local/tmp/modavd-raw-clone-test
```

独立 `cache_quiescence_test.cc` 在自己的原生进程加载真实 Google 库，仅构造 metadata、不执行合成 guest PC。它验证真实两阶段 `NotTranslated → Translating → Invalidating → NotTranslated`、fork COW、Wrapping，并明确断言 Wrapping 的 inflight==0，以及同一生产入口的接受/拒绝。它只交叉确认 GetInstance 对应 TranslationCache mutex `0x242fb8`；另外两个实际锁偏移在 neutral 进程未初始化，测试使用两把本地初始化 mutex 代替，不声称核验了 code pool/thread table 的运行状态。另记 9,664 ready 节点时间仅作该布局的 probe 数据。先确认 `/system/lib64/libndk_translation.so` SHA-256 为 `a2086fbc807f697f9b91222065c9d9000ca9727748699c0d9b751319258cbb3e`；测试还拒绝不同 build-id。完整收尾 API 只在该独立测试使用，生产路径不调用。

```powershell
& '<NDK>\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe' --target=x86_64-linux-android30 -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -static-libstdc++ bridge\tests\cache_quiescence_test.cc -llog -ldl -o '<输出>\cache-quiescence-test'
& '<adb>' -P <私有端口> -s <测试实例> push '<输出>\cache-quiescence-test' /data/local/tmp/cache-quiescence-test
& '<adb>' -P <私有端口> -s <测试实例> shell chmod 755 /data/local/tmp/cache-quiescence-test
& '<adb>' -P <私有端口> -s <测试实例> shell /data/local/tmp/cache-quiescence-test translate
& '<adb>' -P <私有端口> -s <测试实例> shell /data/local/tmp/cache-quiescence-test wrap
```

自有 ARM64-only fixture 在上述 API 34 镜像、候选 `0015a574`（2026-10-31，PID 22824）已完成一次严格三路联合验收，各路 3 轮功能检查通过。raw 两路的 ptid/ctid 位于两侧 PROT_NONE 分隔的独立 mmap VMA；日志实际确认 safe=1，不能用普通导入 fork 的结果补算这些计数。

可公开复现的文本在 [`tests/guest_fork/`](tests/guest_fork/README.md)：包含原样 C++/Java、仅加版权注释的 manifest，以及无 Gradle 的通用构建 CLI。其 README 给出显式 NDK/JDK/SDK 路径、外部 timeout、同 PID 三路逐轮验收、仅临时覆盖自有测试包且 finally 恢复属性的独立 ADB 示例。后续 3 次严格重复也通过，合计 4 次启动、每次 3×3，共 36 个 case；这些设备结果来自旧签名 APK `cac94012`，不能改标成通用脚本刚生成的新签名 APK。fixture 默认不进入 Release 构建或运行。

| ARM64 fixture 调用 | 观测到的保护路径 | 本次计数 |
| --- | --- | ---: |
| 公共导入 libc `fork()` | proxy → 宿主 Bionic → 独立 host-atfork parent held=1 | 3 |
| 显式 guest `SYS_clone(0x1200011)` | tid checker safe=1 → raw GOT guard held=1 | 3 |
| 显式 guest `SYS_clone(0x1300011)` | tid checker safe=1 → raw GOT guard held=1 | 3 |

首次 `am start -W` 曾发生一次 process attach timeout，该窗口没有 fixture START/guard；后续非 `-W` 诊断启动及严格重试通过。起因尚未定位，不能宣称与本改动绝对无关，也不能由这个失败归因为 guard。上述是自有 fixture 的功能/路由结果，不代替如鸢或长期回归。

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
