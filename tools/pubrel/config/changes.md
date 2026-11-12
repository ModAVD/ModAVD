## 怎么更新

- 模拟器重新编译（mas-40）；包里的管理器和命令行升到 0.8.0（改动见 [它的 Release](https://github.com/ModAVD/ModAVDManager/releases/tag/v0.8.0)），截图库升到 [v0.1.2](https://github.com/ModAVD/ModAVDExtras/releases/tag/v0.1.2)；执行桥、宿主输入法和 0.39.1 一样。
- 更新以后，已有实例下次开机前数据盘会自动扩到 256 GB（开机时在线扩，游戏数据不动，电脑上多占不到 1 GB，之后按实际写入增长）。不想要的，在管理器的实例设置里改成「指定大小」，或运行 `tools\modavd.exe set <编号> --data-gb <大小>`。（[#153](https://github.com/ModAVD/ModAVD/issues/153)）
- 从 0.39.1 起可以在管理器「设置」里点「更新」；也可以先关掉所有实例（在 AUTO-MAS 或管理器里关机），稍等半分钟，再用新包整个覆盖解压，`avd` 文件夹不会被动到。不用在 AUTO-MAS 里重新添加。
- 关掉管理器窗口以后管理器留在托盘里；手动覆盖更新前，请在托盘图标的右键菜单里点「退出」。
- 第一次装：主包里没有外层文件夹，请用右键「全部解压缩」或 7-Zip「解压到 <包名>\」。解压后双击 `ModAVD.exe`，点上方提示里的「补全组件」下载 adb 和系统镜像，再新建实例；要接入 AUTO-MAS 的，再到 AUTO-MAS 里添加，详见 `说明.txt`。

## AUTO-MAS 版本

- 推荐 `v5.7.0` 或更新，和 0.39.1 相同。这一版对 AUTO-MAS 没有新要求：管理器 0.8.0 的接口版本（`protocol`）还是 2，只加了参数和字段。新建实例不给数据盘大小时是 256 GB（自动扩盘），还在 AUTO-MAS 允许的 16–512 GB 里。
- AUTO-MAS 5.8.0 在组件不全时还会提示双击「补全组件.bat」，0.38.2 起包里没有这个文件：请在管理器里点「补全组件」，或运行 `tools\modavd.exe components fetch`，再回 AUTO-MAS 点「检查」。
- `modavd update` 只下变了的文件（[#171](https://github.com/ModAVD/ModAVD/issues/171)）要等 AUTO-MAS 改成调 `modavd update` 的版本发布后才用得上；现在的 AUTO-MAS 照旧用它自己的更新器。

## 已知问题

- 删除游戏或文件后虚拟磁盘不会自动缩小，模拟器的 trim 还不会真正回收空间；要省空间，请在关机后用管理器的「整理磁盘」或命令行 `modavd disk <编号> compact`。（[#2](https://github.com/ModAVD/ModAVD/issues/2)）
- 《阴阳师》冷启动偶尔停在「正在获取音频列表...」，关掉重开即可。（[#36](https://github.com/ModAVD/ModAVD/issues/36)）
- AUTO-MAS 自己起的模拟器（不经过管理器或命令行），多开时 netsimd 还可能卡住，之后所有实例都开不了机；在任务管理器里结束 netsimd 进程再开机就好。用管理器或命令行开机的实例已经不用 netsimd 了。（[#65](https://github.com/ModAVD/ModAVD/issues/65)）
- 《明日方舟》偶尔在启动时报资源管理器初始化失败，随后把约 12.8 GB 的资源整包重新下载一遍；遇到过两次，排查都没有发现模拟器丢数据，更像游戏自己把资源索引作废了，等它下完即可。（[#89](https://github.com/ModAVD/ModAVD/issues/89)）
## 发布前回归

用的是这一版的候选包：模拟器就是要发的 mas-40；管理器和命令行是 ModAVDManager `release/0.40` 头上的 0.8.0（还没打 tag，发布时 tag 打在这个提交上，两次编译逐字节相同）；截图库是 v0.1.2 的 Release 附件；宿主输入法、执行桥和 0.39.1 相同。

| 项目 | 判据 | 结果 |
|---|---|---|
| 从 0.39.1 升级：`modavd update` 只下变了的文件（[#171](https://github.com/ModAVD/ModAVD/issues/171)） | 下载量明显小于整包；按清单核对；装好后能开机 | 通过：候选没发布，用 0.8.0 的更新代码对着本地支持分段下载的假 GitHub 从 0.39.1 准备：取回 20 个文件、16 次请求共 113 MB（整包 301 MB 的 37.7%），其余 284 个从本地复制，按清单核对 304 个文件，记成按区间；再用 0.39.1 自带的 `update install` 1 秒换好，包根 303 个文件和新包逐个相同；随后照常开机 |
| 从 0.39.1 升级：0.39.1 自带的命令行 `update install`（暂存整包） | 包根文件和新包相同 | 通过：1.2 秒换好，303 个文件逐个相同 |
| 从 0.39.1 手动覆盖升级：ModAVDE2E 的 upgrade 项 | 旧实例照常开机、数据都在；旧盘扩满；装桥；输入法跟着包过来 | 通过：约 5 分钟；数据在；0.39.1 建的 64 GB 盘是自动扩盘、已扩到 256 GB（/data 255.5 GB）；升级后开机自动装上执行桥；客体里的输入法和包里的逐字节相同、是默认。这一项原来还要把旧盘改成 80 GB，自动扩盘以后改小会被拒，项目已跟着改（[ModAVDE2E](https://github.com/ModAVD/ModAVDE2E) 的 PR） |
| 开关机主路径：无头、带窗口开机，正常关机 | 开机正常；正常关机、不留进程 | 通过：无头开机 12–14 秒、合计 28–37 秒，带窗口开机 17–19 秒、合计 37–45 秒；正常关机 4.7–10 秒 |
| 带窗口的实例：出画面、点触 | 窗口里有画面；点哪儿客体就在哪儿收到 | 通过：窗口截图有桌面；给窗口发鼠标消息点「设置」图标，客体收到的坐标（1198, 592）和按窗口比例算的（1197.5, 592.5）对得上，设置打开了；WoA 里用同样的办法点进了机场 |
| 带窗口的实例点 ×（[#64](https://github.com/ModAVD/ModAVD/issues/64) 把安全关机抽成公用的一块） | 照旧先 sync、再让安卓关机，然后退出 | 通过：给窗口发 `WM_CLOSE`，日志照旧是「syncing the guest… guest synced; asking Android to power off … Guest shutdown command accepted」，5–6 秒退出，和 0.39 上的日志逐行一样；关之前写的文件再开机还在 |
| 无头实例收到 Windows 结束会话的消息（[#64](https://github.com/ModAVD/ModAVD/issues/64)） | 先 sync、再让安卓关机 | 通过（没有真的注销）：给无头实例的隐藏窗口发 `WM_QUERYENDSESSION`、`WM_ENDSESSION`，日志「Safe headless shutdown: Windows is ending the session … the guest is synced … asking Android to power off」，6 秒退出；之前写的文件再开机还在 |
| 老板键（[#101](https://github.com/ModAVD/ModAVD/issues/101)） | 藏 / 显、托盘叫回、退出前显示，位置不变 | 通过（用 #132 之前的 0.8.0 开发版，老板键的代码相同）：管理器只开托盘，注册 Ctrl+Alt+Shift+1；给托盘窗口发 `WM_HOTKEY` 藏起、再发叫回，左键点托盘图标叫回，窗口位置大小（100,100–953,612）都没变；托盘菜单选「退出」时先把藏着的窗口显示出来再退出 |
| 帧率 1–60（[#180](https://github.com/ModAVD/ModAVD/issues/180)） | 设 45 开机，实际帧率对得上 | 通过：`set --fps 45` 后开机，客体 vsync 45、屏幕 45 Hz；明日方舟标题页电脑端收到 45.0 帧/秒、游戏 45.8 fps；`--fps 61` 被拒 |
| 明日方舟下载盘（[#35](https://github.com/ModAVD/ModAVD/issues/35)） | 新装游戏首次下载就在下载盘上、明显变快；下完能进游戏 | 通过：新建的实例装上 2.7.91，没启动前几秒内就挂好下载盘；首次启动下整包 15317 MB，网卡约 96 MB/s（约 91 MiB/s），下完到配音选择和 START；正常关机再开机，盘照原样挂回 |
| 数据盘自动扩盘 / 指定大小（[#153](https://github.com/ModAVD/ModAVD/issues/153)） | 旧实例升级后下次开机扩到 256 GB、数据在；新建默认自动扩盘、电脑上只按实际写入占；指定大小不自动扩 | 通过：旧实例 96 / 64 / 16 GB 三台升级后第一次开机前都扩到 256 GB、开机在线扩 0.2–0.5 秒，量了两台，电脑上只多 0.03、0.15 GB；带明日方舟的那台直接到 START、再开机不再扩。新建不给大小是自动扩盘 256 GB，电脑上占 5.5 GB，和指定 32 GB 新建的 5.3 GB 差不多。指定 32 GB 的实例塞到剩 3.7 GB，开机后提醒快满并给出 `--data-gb auto` 和改大的命令，改小被拒；改成自动扩盘后开机扩到 256 GB，提醒消失，原来的文件 md5 不变 |
| 游戏：World of Airports、明日方舟、重返未来：1999、崩坏：星穹铁道、崩坏3 | 能进到主界面或标题页；没有崩溃、设备丢失、黑帧；帧率不比上一版明显差 | 通过：WoA 带窗口进机场 29.1 fps（锁 30）；明日方舟标题页 60.6 fps；1999 标题页 61.6 fps；星铁（普通模式）到版本更新提示 30.4 fps；崩坏3 到资源下载确认 53.9 fps；设备丢失、Vulkan 报错、崩溃、黑帧都是 0 |
| 截图库 v0.1.2：MaaCore（ModAVD 连接配置）`root` 直接指测试根 | 测速选中 ModAvdExtras；日常任务完成 | 通过：3.8 秒连上，ModAvdExtras 1 毫秒；开始唤醒、1-7、基建、信用访友和购物、领奖励五项都完成（11.1 分钟），截图 0 失败 |
| 执行桥：ModAVDE2E 的 bridge-default、bridge（PowerShell 5.1） | 新建默认装桥；装 / 卸全流程 | 通过：413 秒、259 秒，各项核对全过 |
| 新建数据盘与扩容：ModAVDE2E 的 datadisk | inode 1048576；16 → 32 GB 在线扩满 | 通过：112 秒；/data 31.3 GB |
| 宿主输入法：ModAVDE2E 的 ime-pkg（包里的正式签名 APK） | 新建自动装好、设为默认；通道 20 项；开发签名换回正式签名 | 通过：271 秒；通道 PASS 20、FAIL 0 |
| 补全组件：ModAVDE2E 的 fetch 项 | 新根目录从空装好 adb 和系统镜像 | 通过：命令行 21 秒装好两样（用缓存的压缩包） |
| 包内容和隐私：和 0.39.1 比 | 只有该变的文件变；隐私扫描 0 失败 | 通过：同样 303 个文件，没增没删；变了 17 个：模拟器程序（10 个）和 `source.properties`、两处 NOTICE 的 txt 和 csv、说明.txt、管理器两个 exe、截图库；qemu 补丁 101 个逐个回放树都相同，gfxstream、Crashpad 沿用已发布的目录；隐私扫描 380,866 个文件和成员，0 失败 |

- 带窗口实例的最大化、贴边吸附、黑边和真实鼠标拖动（[#161](https://github.com/ModAVD/ModAVD/issues/161)）、点标题栏激活（[#160](https://github.com/ModAVD/ModAVD/issues/160)）由维护者另外实机测。
- 没测：真实的 Windows 注销、重启、关机；经 AUTO-MAS 开机的实例走一遍自动扩盘；从 GitHub、Mirror 酱真实下载这一版（发布后补）；管理器窗口本身（这一轮只开了托盘）。《明日方舟：终末地》《阴阳师》测试机上没装。
- 模拟器的 GLES / GuestAngle、MaaFW 截图这些和 gfxstream、共享内存有关的没有重跑：gfxstream 补丁和 0.39 相同，见 0.39.0 的发布前回归。
- 每次开机系统自带的蓝牙进程会崩几次（`com.google.android.bluetooth`），0.39.1 上一样，和这一版无关。
- 关机都走正常关机（`modavd stop`，或 sync 后关机）；这一轮不做硬杀测试。
