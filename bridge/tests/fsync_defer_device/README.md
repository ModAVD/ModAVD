# fsync 延后（#35 / #96）的实机测试工具

配合 [`../../docs/fsync-defer-35.md`](../../docs/fsync-defer-35.md)（交接说明）使用。全部脚本只依赖参数，不依赖任何本机目录。
宿主是 Windows + PowerShell 7（`pwsh`），客体是 ModAVD 的 x86_64 实例（装了执行桥、`adb root` 可用）。

| 文件 | 在哪跑 | 做什么 |
| --- | --- | --- |
| `build.ps1` | 宿主 | 用 NDK r27c 编下面几个客体程序和模块单测 |
| `make-mutants.ps1` | 宿主 | 变异检查：每个变体只改 `fsync_defer.cc` 一处，配原单测编一份；上机跑，每份都应打印 `N FAILURE(S)` |
| `eintr_probe.cc` | 客体 | 在进程里装上**真**过滤器，一个线程不停 fsync / fdatasync 或提交 SQLite 事务，另一个线程对它狂发信号，数 EINTR / `SQLITE_IOERR_*` |
| `zscan.cc` | 客体 | 列出文件里整块（4 KiB）全 0 的区段 |
| `jank_probe.cc` | 客体 | 心跳线程停顿、游戏帧停顿（SurfaceFlinger）、下完的文件、脏页，放在同一条时间线上 |
| `tick.cc` + `tick-host.ps1` | 客体 + 宿主 | 客体每 50 ms 打一行时间、宿主收到时打时间：分清「客体真停了」和「客体时钟跳了」 |
| `dl-measure.sh` | 客体 | 一个下载定窗：等第一个 `*.downloading` 出现，跳过预热，按 10 s 采 `du`、网卡收包、磁盘计数、CPU，按 30 s 存 timestats |
| `px.sh` | 客体 | 取屏幕像素颜色（判断游戏弹的是哪个下载确认框） |
| `grab.sh` | 客体 | `inotifyd` 的处理程序：游戏把下完的 `*.downloading` 改名的那一刻，把文件硬链接留下 |
| `dl-window.ps1` | 宿主 | 一个测速窗口：设模式、清资源、启动游戏、点确认框、跑 `dl-measure.sh` |
| `dl-sum.py` | 宿主 | 把各窗口汇总成表：du / 网络速度、帧数、平均 fps、p99、≥100 ms 的帧、客体 CPU |
| `dl-kill.ps1` | 宿主 | 一轮强杀：下到指定量 → 不带 sync 的 `emu kill` → 重开 → 拷下盘上的半截文件并扫 0 → 重启游戏观察解压错误，并逐字节比对 |
| `dl-killloop.ps1` | 宿主 | 强杀循环，出一次解压错误就停、保留证据 |
| `dl-jank.ps1` + `jank-sum.py` | 宿主 | 卡顿诊断窗口（同时记宿主磁盘写延迟）和分析 |

## 准备

1. 编执行桥载荷：`bridge/scripts/build-companion.ps1 -Ndk <NDK>`、`bridge/scripts/build-nbshim.ps1 -Ndk <NDK>`，连同
   `bridge/nbshim/modavd_bridge.rc` 放进一个目录，用 ModAVD 包里的 `tools\modavd-bridge.ps1 install -Payload <目录> -Reboot`
   装进测试实例（要可写系统镜像；脚本会处理）。
2. `pwsh -File build.ps1 -Ndk <NDK> -Out <bin>`，把 `<bin>` 下的程序 `adb push` 到客体 `/data/local/tmp/t96/`、`chmod 755`。
3. 装明日方舟（游戏若提示客户端过时，会自己下新整包，下完找到 `files/HGGameUpdate/gameupdate/*/…_patch.apk` 用
   `pm install -r` 装上）。第一次启动点「用户协议」同意。资源下载在登录前就开始，不用登录。
4. 模式：`adb root` 后 `setprop modavd.fsync.defer com.hypergryph.arknights`、`setprop modavd.fsync.defer.mode active|observe`；
   `setprop modavd.fsync.defer none` 是完全不装（off）。属性不持久，客体重启后要重设，改完要重启游戏进程才生效。

## 常用命令

```powershell
# 单测（客体）
adb -P <端口> -s emulator-<控制台> shell /data/local/tmp/t96/fsync-defer-test        # 期望 ALL PASS
# 变异检查：编出来 push 到客体逐个跑，每个都应 FAIL
pwsh -File make-mutants.ps1 -Ndk <NDK> -Out <dir>
# 测速窗口（宿主）；off / active / observe 交替各跑几轮，再汇总
pwsh -File dl-window.ps1 -Arm active -Label w1 -AdbPort <端口> -Serial emulator-<控制台> -Out <结果目录> -Adb <adb.exe>
python -I -X utf8 dl-sum.py <结果目录>
# 强杀循环（宿主）：出一次 302 就停
pwsh -File dl-killloop.ps1 -Arm active -Rounds 10 -AdbPort <端口> -Serial emulator-<控制台> -ModavdRoot <ModAVD 根> -Instance <编号> -Out <结果目录> -BinDir <bin> -Adb <adb.exe>
# EINTR 探针（客体）：<none|arm> <norestart|restart> <fsync|fsync-match|sqlite-wal|sqlite-delete> <次数> <信号间隔us> <目录>
adb ... shell /data/local/tmp/t96/eintr-probe kernel
adb ... shell /data/local/tmp/t96/eintr-probe arm norestart sqlite-delete 800 20 /data/local/tmp/t96/w
```

判 302：强杀后重开，游戏界面出「释放更新资源失败，请重试，错误号:302」，logcat 里是
`net.lingala.zip4j.exception.ZipException: Reached end of entry, but crc verification failed for …`（`dl-kill.ps1` 会数）。
坏包可以用 Python 的 `zipfile` 逐项读一遍，找出坏条目和它在文件里的偏移。
