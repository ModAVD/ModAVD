# 下载 listener 原生测试

`download_listener_test.cc` 是独立的 x86_64 Android 程序。前 6 项验证 listener 生命周期，后 15 项直接包含 `download_listener.cc`，验证生产实现。测试只使用自建 scratch 文件，不读写游戏资源。

## 构建与运行

在仓库根目录使用 NDK r27c 构建。替换命令里的路径、设备和私有 ADB 端口。端口取测试根目录 `mas-avd.json` 的 `scriptAdbServerPort`，不要使用 5037。

```powershell
& '<NDK>\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe' --target=x86_64-linux-android30 -O2 -std=c++17 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -static-libstdc++ bridge\tests\download_listener_test.cc -o '<输出>\download-listener-test'
Get-FileHash '<输出>\download-listener-test' -Algorithm SHA256
& '<adb>' -P <私有端口> -s <测试实例> push '<输出>\download-listener-test' /data/local/tmp/modavd-download-listener-test
& '<adb>' -P <私有端口> -s <测试实例> shell chmod 755 /data/local/tmp/modavd-download-listener-test
& '<adb>' -P <私有端口> -s <测试实例> shell 'su 0 sh -c "id; getenforce; uname -r; sha256sum /data/local/tmp/modavd-download-listener-test; /data/local/tmp/modavd-download-listener-test"'
```

默认 scratch 目录为 `/data/local/tmp`，可用 `--scratch-dir DIR` 指定。每项测试创建独立进程组，driver 是 subreaper。每项等待事件的期限为 6 秒，失败后杀掉并回收该组；自然退出先于最终清理核验。

## 已运行结果

2026-11-07，在隔离的 ModAVD x86_64 测试实例、Linux 6.1、`su 0`、SELinux `Enforcing` 条件下，21 项全部通过，输出 `ALL PASS (0 failures)`，没有跳过。设备端 SHA-256 与本机构建一致：

```text
04255cbbbe4798dda83ede618cda730c76550274f9169d62600a2e8d4f37ae05
```

原始日志保存在本机实验输出的 `listener-test-uring-06.log`，未纳入仓库。此前普通 shell UID 2000 的 hardlink fixture 被 SELinux 拒绝，未通过完整套件；本次完整结果来自 `su 0`，没有修改 SELinux policy 或 Enforcing 状态。

生命周期测试覆盖父进程正常退出、SIGKILL、自身 exec、raw clone 继承 filter 和 IPC、RECV 后父进程死亡，以及全部 listener 关闭后的 ENOSYS。Linux 6.1 上需先回收 filtered task 的僵尸，再等待 listener POLLHUP；测试已按此顺序核验。

生产实现测试覆盖以下行为：

- CONTINUE 执行真实 fsync，fdatasync 不进入 callback。
- callback errno 返回、worker 自身 fsync 和 unlinkat 放行。
- deadline 返回 ETIMEDOUT，Healthy 永久为 false，迟到成功回复丢弃。
- rename、renameat、renameat2、link、linkat、unlink、unlinkat 实际执行。
- raw clone 的 foreign 请求进入 callback，父进程死亡后返回 EIO。
- 第一条 callback 阻塞、第二条 rename 已由真实 RECV 接收后，20 次不带 SA_RESTART 的 SIGUSR1 不能让调用者提前返回或复用 fd、路径。释放 callback 后核验原 inode 被改名、信号送达，随后 fd 和路径复用成功。
- guardian 在交接前死亡、SCM_RIGHTS 的 sendmsg 真实失败、guardian ready 后但本地 listener 关闭前死亡。Start 有限时间返回错误并保留 `filter_installed=true`；最后一项的 raw clone 后代关闭自己的 listener 副本后，fsync 返回 ENOSYS。

fixture 用编译期测试钩子确认真实 RECV 时序并注入上述故障。正常构建不包含这些钩子。生产安装强制使用 `SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV`，不支持该 flag 的内核直接拒绝安装，不降级。

第 18 项先安装拒绝 raw `SYS_poll` 的 filter，并通过子进程被 SIGSYS 杀死核验该限制，再运行生产 Start 和真实 fsync 通知。worker 与 guardian 继承同一 filter。该项只复现 Android x86_64 app allowlist 对 poll 的限制，不等于完整 app sandbox。生产等待改用直接 `SYS_ppoll`，毫秒转换为 timespec，不在 fork 子进程调用 libc poll 包装。本次运行确认限制探针被 SIGSYS 终止，随后生产 Start 与 fsync 成功。

第 19 项也拒绝 raw poll，再用可选 `Tick` 核验空闲时至少两次唤醒、处理下一条请求前调用 Tick，以及 Tick 和 callback 的 TID 都等于 `worker_tid`。callback 阻塞 150ms 期间 Tick 计数不变，用于核验两者串行执行。接口为 `Start(callback, context, timeout_ms, tick)`，既有三参数调用保持兼容。空闲间隔为 100ms，不新增线程；运行中的 callback 或 Tick 可延迟下一次调用。本次运行通过该项串行性检查。

第 20 项确认 x86_64 的 io_uring_setup、io_uring_enter、io_uring_register，也就是 nr 425、426、427，均由生产 filter 通知 callback。callback 对三项返回 ENOSYS，核验收到三项且 bitmask 为 7；不在内核建立 ring，不提交 IO 或 IORING_OP_CLOSE。已有 Android filter 若更严格，仍保持其优先级；允许这些入口的平台由 adapter 在继续前 Drain 并永久退入 strict。该项只证明路由，原 app policy 的能力边界由 [app probe](download_listener_app/README.md) 单独测试。

第 21 项仅在原平台允许建立空 ring 时核验 Start 拒绝初始 ring。隔离子进程 setup 两个 entries、零 params，经 SCM_RIGHTS 交出未使用的 fd，再核验 Start 返回 ENOTSUP、没有 worker 或 guardian、没有安装 filter、没有改变 NNP。没有 IO submission。若原策略以 SIGSYS 或 syscall errno 禁止 setup，则明确 SKIP 并记录原始原因，汇总显示 `PASS WITH SKIPS`，不冒充该分支通过。本次 `su 0` 运行实际建立空 ring，并验证 Start 拒绝它；该项通过，没有 SKIP。

Start 的 fd inventory 在创建实验线程、guardian 和安装 filter 前进行，继承 io_uring 返回 ENOTSUP，扫描错误返回具体 errno，关闭 fd 的 ENOENT 视为已消失。调用方须保证 ring producer 在 scan 到安装期间静止；早期 NativeBridgeInitialize 在游戏代码运行前调用，晚期任意 app 的并发 SQPOLL producer 不能靠一次扫描排除。

这些结果验证原生 listener 与 callback 通道。游戏 payload 安装、游戏下载、崩溃恢复和性能需另外实测。WAIT_KILLABLE_RECV 约束通知已接收后的非致命信号，不替代 callback 对私有参数、文件句柄和超时后状态的管理。
