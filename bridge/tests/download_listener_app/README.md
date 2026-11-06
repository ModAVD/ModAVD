<!-- Copyright 2026 Ji Mao Cai -->
# 在 Android app 上下文测试 listener

包名为 `org.modavd.tests.downloadlistener`，只包含 x86_64 JNI 和运行自身的 Instrumentation。没有 Activity、权限声明或 UI 操作。测试不修改属性、系统 seccomp 或 SELinux policy、游戏文件，只在自己的 `filesDir` 创建 scratch。

JNI 编译实际 `bridge/companion/jni/download_listener.cc`。它要求 UID 至少为 10000、SELinux domain 包含 `untrusted_app`，并且 Start 前已有 `Seccomp=2`、`Seccomp_filters>=1`。这能覆盖 root 原生程序没有继承的 Android app filter。

## 离线构建

需要 Python 3.9+、NDK r27c、JDK 21、现有 API 34 或更新的 android.jar、SDK build-tools 35。manifest 固定 minSdk 30、targetSdk 35，代码只使用 API 30 已有接口。所有工具路径显式传入，不使用 Gradle，不下载依赖，也不调用 ADB：

```text
python bridge/tests/download_listener_app/build.py --ndk "<NDK>" --java-home "<JDK>" --android-jar "<platform>/android.jar" --build-tools "<build-tools>" --out "<独立输出目录>"
```

脚本检查签名、ELF64 x86_64、APK 仅有 x86_64 库，并保存 APK、SO、`manifest.json`、`build-log.json`。manifest 记录 probe 与实际 listener 的源文件哈希。每条外部构建命令限时 90 秒。临时 staging 每次新建，不删除已有输出。

本地测试签名 key 保留在输出目录供重建复用。新输出目录可追加 `--keystore "<原输出目录>/probe-debug.keystore"`，复用已有测试签名，继续 `install -r`。该参数只接受已有 key。生成物和 key 不提交。不指定时，新目录生成新 key；签名冲突时只能卸载这个自有测试包后重装。

## 在隔离实例运行

端口取测试根目录 `mas-avd.json` 的 `scriptAdbServerPort`。脚本拒绝 5037，所有参数显式传入：

```text
python bridge/tests/download_listener_app/run.py --adb "<adb>" --port <私有端口> --serial "<测试实例>" --apk "<输出目录>/download-listener-app.apk" --out "<独立结果目录>"
```

脚本执行 `install -r`，停止自有包以保证新进程，再运行：

```text
adb -P <私有端口> -s <测试实例> shell am instrument -w -r org.modavd.tests.downloadlistener/.ProbeInstrumentation
```

安装和进程准备各限时 30 秒，instrument 限时 25 秒。Java 在加载 JNI 前启动 10 秒 watchdog；probe 超时会终止自身。脚本 finally 只停止这个包，清理限时 10 秒，不提权、不授予权限、不更改属性。每步命令、stdout、stderr、退出码或超时保存在 `device-log.json`，instrument 原始 stdout 按字节另存为 `instrumentation.txt`。判读用 `splitlines()` 后逐行 `strip()`，接受 ADB 的 CRCRLF，不改写原始输出。

失败时可能保留自有 scratch，不清理游戏或全局日志。验收需同时出现以下结果：

- `PASS late-jni listener app probe` 和 `INSTRUMENTATION_CODE: -1`。
- 输出真实 UID、SELinux domain、Start 前后的 Seccomp 字段，以及 owner、caller、worker、guardian ID。
- Start 成功，原有 app filter 保留，filter 数增加 1。
- 空闲时 Tick 至少新增两次，`tick_tid` 和 `callback_tid` 都等于 `worker`，Healthy 为真。
- worker 用自己的 dup 真实 fsync 两次，并实际 emulate 一次 renameat 和一次 unlinkat。改名后 inode 一致，删除后原路径不存在。caller 收到 worker 的真实结果。

callback 只对本 fixture 已识别的 fd 和不可变路径参数做 emulation，其他框架请求继续真实 syscall。这验证 listener 的 app policy 兼容性，不替代完整下载 adapter 的身份管理或恢复测试。

在 Start 前还运行两轮父子存活探针。自有进程及同 UID raw-fork 后代均设 `PR_SET_DUMPABLE=0`，不更换 mount、user 或 SELinux namespace，不通过 su 代理。先确认自己的 `/proc/self/stat` 和 start token 可读，再记录双方读取 peer `/proc/<pid>/stat` 的 errno；proc 读失败不算测试失败。

双方必须实际用 `pidfd_open(peer, 0)` 和 `ppoll(..., 0)` 确认对方仍活着。一轮由 child 先退出，parent 观测 pidfd 可读并 wait4 回收；另一轮由 parent 先退出，child 观测 pidfd 可读并经 pipe 回传。外层 JNI driver 回收直接子进程，孤儿后代交给 Android reaper；driver 另用 pidfd 确认后代已退出。所有诊断 pipe、pidfd 在生产 Start 前关闭，不进入 listener 的 callback fd 状态。每个 gate、pipe 和 pidfd 等待限时 2 秒，保留总计 10 秒 watchdog。

io_uring 能力探针同样位于 Start 前，使用原 Android app policy。先扫描自己的初始 `/proc/self/fd`，输出 anon_inode io_uring 匹配、已消失 fd 和扫描错误数。存在继承 ring 或扫描不完整时不会假定安全。然后隔离 raw-fork child，仅调用一次 `io_uring_setup(2, &全零params)`，回传真实返回值、errno，或由父记录 SIGSYS。若建立成功，仅普通 close 清理未使用的 ring fd；不 mmap ring，不调用 enter/register，不创建 SQE，不提交 IO 或 IORING_OP_CLOSE。调用被禁止或内核返回其他结果均保留现场，测试不放宽策略。

生产 Start 也在创建 worker、guardian 和安装 filter 前扫描自己的 fd，发现继承 io_uring 返回 ENOTSUP；扫描失败返回具体 errno，仅关闭 fd 的 ENOENT 可忽略。新 setup、enter、register 则由生产 filter 通知 adapter，先 Drain 并永久退入 strict。调用方须在扫描到安装期间排除并发 ring producer。本 fixture 的晚期 JNI 没有其他 ring producer，不以单次扫描宣称任意晚期 app 的 SQPOLL 安全。

## 已有证据与边界

2026-11-07，Windows 离线构建使用 NDK r27c、JDK 21、API 34 android.jar、build-tools 35，完成签名与 x86_64 ABI 检查。当前 APK SHA-256：

```text
6af4dd0c05a13078a785dd9ea4d0f5d706b94f22c1805fe782f5ac69bd734b47
```

该 APK 复用原测试签名，实际 `install -r`、instrument 和清理均 exit 0。UID 为 10195，SELinux 为 `untrusted_app`；dumpable 初始与最终均为 0，own stat 和 start token 可读。两轮同 UID 父子读取对方 proc/stat 均返回 errno 2，也就是 ENOENT，但 pidfd_open 全部成功。两个方向均在 peer 存活时由 ppoll 返回 0，退出后返回 1、revents 为 POLLIN。child-exits 轮确认父已回收 child。

初始 fd 扫描共 67 项，没有消失的 fd、扫描错误或继承 io_uring。原 Android app policy 下，隔离 child 的 io_uring_setup 被 SIGSYS 31 终止并回收。没有创建 SQE 或提交 IO，也没有放宽策略。该结果与 `su 0` 原生测试中能建立空 ring 并验证 Start 拒绝它的环境不同。

Start 前 `Seccomp=2`、filter 数 1、NNP 0，Start 成功后 filter 数 2、NNP 1。fsync 2 次、renameat 1 次、unlinkat 1 次均由 worker 真实执行。Tick 7 次，Tick 和 callback 都在 worker 21790，Healthy 为真，最终 `INSTRUMENTATION_CODE: -1`。完整日志为本机实验输出 `listener-app-device-04-uring/instrumentation.txt` 与 `device-log.json`，未提交。

原 Android app policy 隐藏了活着的同 UID peer 的 proc/stat，ENOENT 不能独立作为死亡判据。pidfd 能区分本次存活和退出，无需放宽 dumpable 或系统保护。

此入口位于 Instrumentation 的后期 JNI 阶段。它不证明更早的 `NativeBridgeInitialize` 安装时机、完整游戏下载或崩溃恢复已经通过，仍需分别实测。
