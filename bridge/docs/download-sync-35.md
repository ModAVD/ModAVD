# 明日方舟临时下载同步实验

本实验处理 [#35](https://github.com/ModAVD/ModAVD/issues/35) 中频繁同步未完成下载包的问题。默认关闭，只供独立测试实例使用。启用后，对已接管的 `.downloading` 文件提前返回 `fsync` 成功；发布、链接或删除文件前，由协调线程真正同步此前欠下的写入。

提前返回会降低未完成下载包的断电保证。恢复时丢弃已记录的临时包，由游戏重新下载整个包，不按文件长度猜测安全前缀。恢复权限仅覆盖实验接管的临时下载缓存，不覆盖 APK、已完成资源、账号或配置。

## 开关与启动条件

`modavd.download.sync` 在新进程初始化时读取：

| 值 | 行为 |
| --- | --- |
| 空值、`off` | 保持原来的同步行为；有旧实验记录时仍须先完成受控恢复 |
| `observe` | 安装观察器并继续原系统调用，不提前返回成功 |
| `active` | 对通过准入检查的临时下载缓存合并同步 |

只匹配 `com.hypergryph.arknights`。其他包不安装该实验的过滤器。观察模式也有通知、调度和信号语义的成本，不能当成完全没有拦截的对照。

启用与恢复必须通过 [`download_session.py`](../tests/download_session.py)。该启动器核对独立测试根目录、实际 QEMU、SDK 和客体桥的哈希，先同步并停止整个游戏 UID，确认没有残留进程，再设置新的 32 位十六进制 session 值并启动一次。桥会持久化已使用的 session。应用崩溃后自动重启不能重复使用它执行恢复。

session 是外部启动器已完成排他准备的凭据。桥自身的 `flock` 只协调参与本协议的进程，不能阻止任意游戏进程访问文件。不要绕过启动器，用正在登录主力账号的实例试验，或在恢复过程中从其他入口启动同一包。

启动器要求显式的 `.download-sync-test-root.json`，其中包括 `experiment=download-sync-v2`、`disposable=true`、测试根目录和 SDK、实例编号和标题、Manager/ADB/QEMU/桥文件哈希。文件中的路径由本机测试环境提供，不放进仓库。ADB 端口从该根目录的 `mas-avd.json` 读取，拒绝默认 5037。

```powershell
python bridge/tests/download_session.py --root '<独立测试根目录>' --mode off --evidence '<新的结果文件.json>'
python bridge/tests/download_session.py --root '<同一独立测试根目录>' --mode active --evidence '<另一个新的结果文件.json>'
```

首次启动还未创建 `HGDownload` 时，实验会记录未激活并保持原行为。先完成游戏的首次启动和下载准备，再停止游戏并重新受控启动。运行中改属性不会改变已经安装的过滤器。

## 文件与记录的所有权

只接管以下目录中具有非空文件名和精确 `.downloading` 后缀的普通文件：

```text
/storage/emulated/<用户>/Android/data/com.hypergryph.arknights/files/HGGameUpdate/HGDownload/
```

每次准入先复制实际文件句柄，再检查身份、可写模式、单链接和受限路径。目录逐级拒绝符号链接、路径逃逸和不同设备号。已准入的同一源句柄使用有效映射，避免每几 KB 都重新遍历 FUSE 路径。`close`、`dup2`、`dup3` 会使映射失效；无法保证共享文件表的调用先偿还旧债，并永久停止本进程后续提前确认。

相同 inode 的不同打开实例保留独立句柄和提交状态，不能混淆各自的写回错误。提交票据只确认开始同步前封定的请求代数。真实同步错误、溢出和不确定状态不能推进已提交代数。

持久化记录接管的是精确的临时缓存键，即受限目录中的相对路径。同一路径被游戏重新创建后，恢复仍可以丢弃该键下新的未完成文件。这不依赖 inode 在重启后保持不变，也不需要给游戏文件添加扩展属性。

记录保存在应用私有目录的 `.modavd-download-sync/journal` 中。准入顺序为：

1. 完整写入桥自己的 draft 并同步文件。
2. 原子发布为 intent，拒绝覆盖已有记录。
3. 同步 journal 目录。
4. 记录欠下的同步请求，再提前回复游戏。

启动恢复先校验全部最终记录。未发布的、严格属于本协议的 draft 可以清理；损坏的最终记录保留并报错。恢复只处理 intent 枚举的缓存键，要求目标仍是单链接普通临时文件。删除后先同步其父目录，再持久化移除 intent。中途崩溃可重复恢复，缺失的临时文件也按幂等流程处理。恢复不会扫描并清空整个游戏目录，也不会沿改名后的完成路径寻找文件。

## 发布与并发

协调线程串行处理系统调用。对于同进程的 rename、link、unlink 操作，它先复制路径并捕获目录句柄，停止新准入，同步全部已接管的打开实例，持久化退休记录，再执行一次真正的命名操作并返回实际结果。操作完成后，仍是临时文件的对象可以重新经过完整路径检查并建立新 intent；已经发布的名称不再符合准入条件。

同包后续启动的独立进程加入当前 owner，不重复恢复，也不提前确认同步。helper 的同步和命名操作先请求共享屏障，owner 偿还旧债并暂停准入后授予执行许可；helper 在自己的文件表中执行真实调用，完成后结束屏障。只有同一个 owner worker 可以服务请求或修改提交状态，空闲时由它每 100 ms 检查一次控制页。helper 丢失、超时或无法确认完成时，会话永久转入原始同步路径，不能自行选出新 owner 或开始恢复。

不能把 `SECCOMP_IOCTL_NOTIF_SEND(CONTINUE)` 成功当成 rename 已完成。因此需要继续接管新文件时，命名操作必须得到真实完成结果。继承过滤器的外来进程，其文件表和指针不能按 worker 所在进程解释；相关操作会先触发全会话偿还并永久转入原始同步路径。永久移交使用独立的协议状态，不把 CONTINUE 冒充实际完成。

这里保证的是偿还此前已提前确认的 `fsync`。真实同步之后的普通并发写仍遵循游戏原有的系统调用语义。本实验不保证文件此后不可变，也不额外保证游戏的 rename 已持久化目录项。

## 通知器的寿命与失败

独立 guardian 始终独占 seccomp listener 的接收与回复。应用内 worker 负责识别文件和执行操作。父进程退出或 exec 不会把已经接收的通知遗落给无人服务的队列；工作线程消失或超时会返回有界错误。

过滤器要求内核支持 `WAIT_KILLABLE_RECV`，不支持时拒绝安装。它避免非致命信号在通知被接收后撤回调用，使旧请求在调用者复用句柄或路径后才执行。内部启动清理的 close 具有明确的过滤器旁路，因此交接失败时不依赖 guardian 继续存活才能关闭原 listener。

超时会永久标记会话不健康。迟到回复不会重新答复原请求，也不会把已经可能执行的操作再次 `CONTINUE`。超时不撤销正在执行的内核 I/O；恢复前必须结束整个旧应用会话。真正同步成功并偿还旧债后，记录可以退休；超时本身不能作为同步成功的依据。

guardian 和应用处于同一应用 UID/进程组。强制停止通常会一起终止它们。关闭全部 listener 后，仍带过滤器的调用会得到 `ENOSYS`，不等于透明放行。初始化失败、同包辅助进程的协调问题、永久回退和实际提前确认计数都必须纳入测试结果。

## 验证

构建命令仍使用 [`bridge/README.md`](../README.md#构建) 中的两个 NDK 脚本。将两个库一起放入独立 SDK，由管理器安装并冷启动；运行前核对客体文件哈希和游戏的实际映射。

原生验证分为以下几组：

- [`download_commit_test`](../tests/download_commit_test.md)：句柄所有权、代数、错误与复用。
- [`download_journal_test`](../tests/download_journal_test.md)：准入断点、原子发布、限定路径和幂等恢复。
- [`download_control_test`](../tests/download_control_test.md)：独立进程间的屏障、会话换代、死亡与超时。
- [`download_listener_test`](../tests/download_listener_test.md)：真实内核通知、信号、进程退出和交接失败。
- [`download_batch_test`](../tests/download_batch_test.md)：真实适配回调的句柄映射、发布边界和失败路径。

另有不申请权限的 [`download_listener_app`](../tests/download_listener_app/README.md) Instrumentation APK，在真正的 `untrusted_app` 和原 Android seccomp 策略下验证 listener。它不操作游戏，不使用 root，不修改设备属性。后期 JNI 调用通过仍不能替代游戏早期 NativeBridge 初始化和实际下载。

这些测试不替代已安装补丁的游戏下载。性能比较应交替运行多轮，并同时记录网络接收量、临时文件增长、游戏界面进度和实际提前确认计数。永久回退后的区间不能当成优化仍生效的结果。强杀测试要确认当时确有网络下载、增长中的临时包和持久化 intent；普通关机先同步。最终结果还应说明临时包重下、资源下载完成及名单外游戏的回归情况。

## 当前游戏验证结果

在同一独立测试实例、相同生产库下完成 `off → active → off → active` 四个约 60 秒下载窗口：

| 轮次 | 模式 | 秒数 | MB/s |
| --- | --- | ---: | ---: |
| 1 | off | 60.01 | 12.53019 |
| 2 | active | 59.92 | 10.64618 |
| 3 | off | 60.00 | 12.33819 |
| 4 | active | 59.97 | 12.34040 |

每个采样窗口开始前，均确认实际网络接收量增加，且临时文件新增或增长，原始数字保存在本机 ready 与 summary 证据中。独立截图确认游戏界面下载进度继续推进，但没有保存每个窗口精确的首尾界面读数。

这里的 MB 为十进制单位，指标是客体网卡接收量，不等同于游戏有效资源吞吐。这四轮未显示下载加速，不支持达到 50 MB/s 的结论。

大量 negative hits 仍执行真实 fsync，并承担通知 IPC 成本，不能把提前确认次数直接当成性能收益。已有 TCP 接收队列积压，不能简单归因为 CDN 限速。CPU 探针中的主要活动包括产品自带的保活子循环，脚本以 `SCHED_IDLE` 启动它们，不能凭较高 CPU 占用断言宿主已饱和。

两轮 active 的当前 owner 最后一次日志快照分别为 `earlyACK=11752 real=29383` 和 `earlyACK=39834 real=58931`，均为 `strict=0 flusherr=0 foreignstrict=0`。这些是各自会话累计值，不是 60 秒窗口增量。`real` 包含主动同步尝试和继续真实系统调用的请求，不代表成功同步次数。日志包含更早会话，按当前 owner 区分后读取，不跨会话相加；日志最后一次快照也不代表精确的窗口终值。它们证明发生了提前确认，不能单凭这些计数推导收益。

对应原始证据保存在本机实验输出 `measurements/final-b` 的四组 summary、samples 和 bridge-logcat 中，未提交。客体 gate SHA-256 为 `be5fad119bc4cbe783b405a1432edce4a2b356122e4755141ef7826982c4ba52`，shim 为 `22a9544c93ca0fe9efbb51aa5309f12974fb54e8248db848074893b65c6996a2`。

下载中直接终止已核对身份的 QEMU 进程，未先 sync，用于模拟客体突然断电。重启后、受控恢复启动前，持久化 intent 对应的临时包仍在，大小为 12,910,592 字节、inode 为 909730。使用新 session nonce 受控启动后，观察到旧缓存键和旧 intent 消失，随后游戏在同一键创建新 inode 909411。新文件有 41 次增长采样，最大观察到 382,548,862 字节。这验证了本次记录的临时包被丢弃并由游戏重新下载，没有按原文件长度续接。

恢复后，当前测试构建完成明日方舟 15077 MB 资源流程，选择维持原有语音配置后到达 START，未点击 START 或登录。资源流程结束时仍为 active，`.downloading` 文件数为 0，journal 为空。随后通过实验 controller 先 sync，再停止整个游戏 UID。验证覆盖游戏完成资源流程并到达 START，未单独离线校验每个资源文件。

恢复证据为本机实验输出 `final-b/crash-retry/crash.json` 与 `final-b/recovery` 下的 before、observations 和 summary，未提交。完成下载的证据为 `ui-b/after-confirm.png`、`ark-at-voice-dialog.json` 和 `ark-complete-stop.json`。

阴阳师 2.8.89、arm64-v8a 的名单外下载回归通过。界面进度由 75.83/4270.62 M 推进至 2482.73 和 3546.95 M，最终进度条消失并进入剧情，未登录。60.01 秒客体 eth0 接收 1,778,715,099 字节，即 29.64031 MB/s。实际进程映射和新 bridge 哈希均已核验；虽然实验属性为 active，进程仍为 `NoNewPrivs=0`、`Seccomp_filters=1`，没有安装实验 filter。结束后先 sync，再正常停止游戏。测试环境为 Android 14、API 34、4 vCPU、6 GiB 内存及 #126 QEMU 编译候选 `e51c956b`。原始证据保存在本机实验输出 `final-b/onmyoji`，最终界面截图为 `onmy-latest.png`。

重返未来：1999 4.1.0、x86_64 完成界面所示 17916.27 MB 资源流程，保持默认英文语音后到达手机号和验证码登录页，未输入或登录。60.01 秒客体 eth0 接收 2,733,858,294 字节，即 45.56 MB/s；进程仍为 `NoNewPrivs=0`、`Seccomp_filters=1`。首次版本请求超时后自动重试成功，没有修改代理、路由或游戏文件。原始证据保存在本机实验输出 `final-b/reverse1999`，最终截图为 `reverse-voice-confirm.png`。

两款名单外游戏的速率只描述各自的下载窗口，不是本实验的加速收益。所有游戏下载结束后先 sync、停止游戏，再正常关闭测试实例；管理器确认 `method=graceful`、`synced=true`。
