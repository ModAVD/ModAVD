## 怎么更新

- 这一版重新编译了模拟器（mas-39）；包里的管理器和命令行升到 0.6.0（改动见 [它的 Release](https://github.com/ModAVD/ModAVDManager/releases/tag/v0.6.0)），还多了截图库 `extras\modavd_extras.dll`（[ModAVDExtras v0.1.1](https://github.com/ModAVD/ModAVDExtras/releases/tag/v0.1.1)）；执行桥、宿主输入法都和 0.38.2 一样。
- 截图库是给 MAA 这类自动化工具用的，接口和用法见 [ModAVDExtras](https://github.com/ModAVD/ModAVDExtras)。MAA、AUTO-MAS 还没有接入它，什么都不用改：AUTO-MAS 现在的截图方式照旧能用。
- 要玩 World of Airports 这类游戏的：实例要有执行桥，装着 Google Play 商店并登录拥有这个游戏的账号（游戏启动时会做 Play 许可检查）。在实例的「更多操作」里开启「报告 GLES 3.2（实验）」（会冷启动一次，要几分钟），再到实例里 设置 → 应用 → Google Play 商店 → 存储 清除数据（账号不会退出），商店就能装了。两个客体库补丁是管理器或命令行开机时挂上的。（[#131](https://github.com/ModAVD/ModAVD/issues/131)）
- 先关掉所有实例（在 AUTO-MAS 或管理器里关机）再更新；关掉后稍等半分钟再覆盖。手动更新时用新包整个覆盖解压，`avd` 文件夹不会被动到。不用在 AUTO-MAS 里重新添加。
- 第一次装：主包里没有外层文件夹，请用右键「全部解压缩」或 7-Zip「解压到 <包名>\」。解压后双击 `ModAVD.exe`，点上方提示里的「补全组件」下载 adb 和系统镜像，再新建实例；要接入 AUTO-MAS 的，再到 AUTO-MAS 里添加，详见 `说明.txt`。

## AUTO-MAS 版本

- 推荐 `v5.7.0` 或更新，和 0.38.2 相同。这一版对 AUTO-MAS 没有新要求：管理器 0.6.0 只加了命令和字段，接口版本（`protocol`）还是 2。
- AUTO-MAS 5.8.0 测试版在组件不全时还会提示双击「补全组件.bat」，0.38.2 起包里没有这个文件：请在管理器里点「补全组件」，或运行 `tools\modavd.exe components fetch`，再回 AUTO-MAS 点「检查」。

## 已知问题

- 删除游戏或文件后虚拟磁盘不会自动缩小，模拟器的 trim 还不会真正回收空间；要省空间，请在关机后用管理器的「整理磁盘」或命令行 `modavd disk <编号> compact`。（[#2](https://github.com/ModAVD/ModAVD/issues/2)）
- 《阴阳师》冷启动偶尔停在「正在获取音频列表...」，关掉重开即可。（[#36](https://github.com/ModAVD/ModAVD/issues/36)）
- AUTO-MAS 自己起的模拟器（不经过管理器或命令行），多开时 netsimd 还可能卡住，之后所有实例都开不了机；在任务管理器里结束 netsimd 进程再开机就好。用管理器或命令行开机的实例已经不用 netsimd 了。（[#65](https://github.com/ModAVD/ModAVD/issues/65)）
- Windows 注销、重启或关机时，没有窗口的实例（命令行或 AUTO-MAS 在后台开的）还是直接断电，不会先让安卓关机。关电脑前请先在 AUTO-MAS 或管理器里关掉实例。（[#64](https://github.com/ModAVD/ModAVD/issues/64)）
- 《明日方舟》偶尔在启动时报资源管理器初始化失败，随后把约 12.8 GB 的资源整包重新下载一遍；遇到过两次，排查都没有发现模拟器丢数据，更像游戏自己把资源索引作废了，等它下完即可。（[#89](https://github.com/ModAVD/ModAVD/issues/89)）

## 发布前回归

用的是这一版的候选包：模拟器就是要发的 mas-39；管理器和命令行是 ModAVDManager 0.6.0 发版 PR 的头编出的候选，两个 exe 的 SHA-256 和 v0.6.0 记的相同；截图库是本机从 ModAVDExtras v0.1.1 同一份源码编的候选，正式包里是 v0.1.1 Release 的附件（CI 编的，字节不同）。宿主输入法和 0.38.2 是同一份。

| 项目 | 判据 | 结果 |
|---|---|---|
| 从 0.38.2 升级：0.38.2 自带的命令行 `update install` | 包根文件和新包相同；旧实例数据都在 | 通过：候选没发布，先用 0.38.2 自己的更新代码把本地包暂存进 `.update`，按清单核对了 304 个文件（含清单），再 `update install`，1.2 秒换好，更新记录 0.38.2 → 0.39.0；包根 303 个文件逐个相同，`extras\modavd_extras.dll` 到位；升级后开机，标记在，三个游戏数据大小逐 KB 相同 |
| 从 0.38.1 升级 | 同上 | 通过：0.38.1 自带的命令行没装暂存的本地包，而是从 GitHub 真下了 0.38.2 装上（4 秒，三个 .bat 和三个旧脚本删了），再按上一行升到 0.39.0；WoA、Play 商店账号、gles32 开关都在 |
| 从 0.38.0 升级：ModAVDE2E 的 upgrade 项，按清单把新包覆盖上去 | 旧实例数据都在；升级后照常开机、装好执行桥 | 通过：386 秒；数据在；数据盘 64 → 80 GB 扩满；执行桥自动装上；输入法和新包逐字节相同、是默认 |
| 开关机主路径：无头、带窗口开机，正常关机 | WHPX；开机正常；正常关机、不留进程 | 通过：WHPX；0.39.0 上无头开机合计 42–48 秒、带窗口 44 秒；正常关机 5–8 秒（明日方舟下完 12.8 GB 后那次 12 秒）；关完没有残留 qemu |
| 磁盘读写数据核对（#126，ModAVD#138）：客体写 1 GiB 固定花纹（每次 4 KiB–1 MiB，普通写和 `O_DIRECT` 混着），sync、正常关机、冷启动后回读比对 | 一个字节都不错 | 通过：两份文件（768 MiB 混写、256 MiB 全 `O_DIRECT`）冷启动后换一种切法回读，0 个错字节，SHA-256 和按花纹重算的一致；`O_DIRECT` 在不加密的目录里确实绕过了页缓存（fsync 前数据盘已写满 256 MiB），在文件级加密的目录里被 ext4 退回普通写，两种都对 |
| 游戏整包下载（#126）：明日方舟资源目录换成空的，从零下完 | 下完；能正常进游戏 | 通过（游戏多重下了一遍，见下）：12.8 GB 用 27 分钟下完（8.9 MB/s，和 0.36.0 时量的 7.7–8.6 MB/s 一样）；下完后 11,325 个带 md5 的资源文件（15.6 GB，清缓存后从盘上回读）md5 全部和游戏清单一致。第一次在同一进程里由 MAA 拉起、点开始时，游戏报 `[ResourceManager] Init failed` 又整包重下一遍（已知问题 [#89](https://github.com/ModAVD/ModAVD/issues/89)，盘上数据是对的）；第二遍下完后直接点开始，进了登录页，MAA 0.6 分钟进到主界面 |
| 临时 cache 写层清理（#123）：`disk.cachePartition=no` 和 `-no-cache` 各开一次机、正常关机 | 临时目录里不留 `.tmp`、`.tmp.qcow2` | 通过：两种都在运行中建了 `TMPxxxx.tmp`（66 MB）和同名 `.qcow2`，正常关机后都删了 |
| World of Airports 3.7.3（#131）：带窗口开机，进主界面，进因斯布鲁克机场 | 开机挂上两个客体库补丁；游戏能进机场、不闪退 | 通过：ARM 转译层补丁（`3efa8977…`）和客体 Vulkan 驱动补丁（`9e5ba745…`）一次挂上、zygote 重启一次；约 60 秒到主界面，进机场画面正常（修之前这里 4 次 4 次 `VK_ERROR_DEVICE_LOST`）；模拟器日志 DEVICE_LOST / VK_ERROR 0 条，显卡驱动事件 0 条；机场里 30 fps（游戏锁 30），1% low 27.8 |
| 报告 GLES 3.2 开关：`gles32 on / off / status --json` | 冷启动后报的版本和开关一致 | 通过：开着的实例 off → 196609（53 秒）、on → 196610（50 秒），status 一致；没开机时 status 报 `unsupported`（退出码 4）；`info --json` 带 `gles32` 字段 |
| MAA 截图和点击（AUTO-MAS 现在的接法）：`MuMuEmulator12` + 假 MuMu 目录里的兼容外壳，实例编号填控制台端口；明日方舟 StartUp、1-7 | MaaCore 测速选中 MumuExtras；任务完成 | 通过：MumuExtras 3 ms（RawWithGzip 176、Encode 402）；两项任务都完成，截图 0 失败 |
| 截图库 `extras\modavd_extras.dll`：本机编的 MaaCore 开发版（加了还没提交给 MAA 的 `ModAVD` 连接配置），`root` 直接指测试根 | 载入包里的 DLL；测速选中 ModAvdExtras；日常任务完成 | 通过：载入的是 `<根目录>\extras\modavd_extras.dll`（哈希和候选包里的一致）；ModAvdExtras 1 ms；开始唤醒、1-7、基建、信用访友和购物、领奖励五项都完成（9.8 分钟），截图 0 失败 |
| MaaFW 截图和点击（AVDExtras）：照 AUTO-MAS 给 MaaFW 的配置，只给 EmulatorExtras | 读共享内存截图；点击生效 | 通过：MaaFW 5.13.0 读 `SHM_videmulator<端口>`，测速选中 AVDExtras 2 ms；30 张截图 0 失败、中位数 0.9 ms；Maatouch 点开、关上游戏菜单 |
| 帧率 | 新版不比上一版明显差、没有卡顿掉帧 | 通过：明日方舟主界面和基建 59.8 fps（锁 60）；WoA 机场 30.4 fps（锁 30），1% low 27.8；崩溃、ANR、Vulkan 报错、设备丢失都是 0 |
| GLES / GuestAngle：默认 GLES；直接起模拟器加 `-feature GuestAngle` 冒烟 | 都能开到桌面 | 通过：默认是宿主 GLES 翻译（OpenGL ES 3.1）；GuestAngle 时渲染器是 `ANGLE (… Vulkan 1.3.0 …)`、OpenGL ES 3.2，桌面正常 |
| 执行桥：ModAVDE2E 的 bridge-default、bridge（PowerShell 5.1） | 新建默认装桥；装 / 卸全流程 | 通过：590 秒、345 秒，各项核对全过 |
| 新建数据盘与扩容：ModAVDE2E 的 datadisk | inode 1048576；16 → 32 GB 在线扩满 | 通过：178 秒；/data 31.3 GB；另外老实例 64 → 96 GB 开机在线扩满（0.4 秒） |
| 整理磁盘：老实例 96 GB 盘关机后 `disk compact`（qemu-img 也是这次重编的） | 校验通过才替换；前后数据一样 | 通过：677 秒，覆盖层 96.9 → 89.9 GB，check 和 compare 都过；前后三个游戏数据大小逐 KB 相同，之后明日方舟照常进主界面。`disk trim` 照旧报「客体数据盘没有 discard」（已知问题 [#2](https://github.com/ModAVD/ModAVD/issues/2)） |
| 宿主输入法：ModAVDE2E 的 ime-pkg（包里的正式签名 APK） | 新建自动装好、设为默认；通道 20 项；开发签名换回正式签名 | 通过：379 秒；通道 PASS 20、FAIL 0 |
| 补全组件：ModAVDE2E 的 fetch 项 | 新根目录从空装好 adb 和系统镜像 | 通过：命令行 24 秒装好两样（用缓存的压缩包，没有真下载）；包里没有批处理 |
| 包内容和隐私 | 和 0.38.2 比只多截图库；隐私扫描 0 失败 | 通过：多了 `extras/modavd_extras.dll`，没删文件；变了 26 个（不算清单本身：模拟器程序、管理器两个 exe、`tools\modavd-bridge.ps1`、说明和 NOTICE）；三条补丁系列逐提交回放，树都相同；两个 incoming 都在新系列里；隐私扫描 380,861 个文件和成员，0 失败 |

- 关机都走正常关机（`modavd stop`，或 sync 后 `emu kill`）；这一轮不做硬杀测试。
- 管理器界面这一轮没开，新命令只用命令行 `--json` 测；Mac 相关的改动 Windows 上不测，只看 CI。用 `modavd update` 从 GitHub、Mirror 酱真实下载 0.39.0，要发布以后才能测。
- 0.38.1 自带的命令行碰上「本地暂存的包比 GitHub 最新的还新」时会改装 GitHub 最新的那版，只有回归塞本地包才会遇到，真实升级不受影响。
