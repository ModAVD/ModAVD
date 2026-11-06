## 怎么更新

- 这一版只重新打包：模拟器还是 mas-38，没有重编；包里的管理器和命令行升到 0.5.0；执行桥、宿主输入法都和 0.38.1 一样。
- 包根的「补全组件.bat」「收集问题信息.bat」「复制配置信息.bat」和 `tools` 下对应的三个脚本不再随包：在管理器里补全组件、收集问题信息，也可以运行 `tools\modavd.exe components fetch`、`tools\modavd.exe collect-info`。（[#45](https://github.com/ModAVD/ModAVD/issues/45)）
  - 用 `modavd update` 自动换包会删掉这六个旧文件；手动覆盖解压会留下它们，可以自己删掉。
- 要 Play 商店的：在管理器的「补全组件」里勾上「同时补全 Google Play 商店服务」（约 1.5 GB），或点「使用我自己的文件…」导入 Google 官方的镜像压缩包；然后在实例的「更多操作」里点「补全 Google Play 商店…」。登录时提示「设备未经 Play 保护机制认证」的，用同一处的「复制 GSF ID」去 Google 登记。查 Play Integrity 的游戏多半还是过不了。不要商店的什么都不用做。（[#125](https://github.com/ModAVD/ModAVD/issues/125)）
- 先关掉所有实例（在 AUTO-MAS 或管理器里关机）再更新；关掉后稍等半分钟再覆盖。手动更新时用新包整个覆盖解压，`avd` 文件夹不会被动到。不用在 AUTO-MAS 里重新添加。
- 第一次装：主包里没有外层文件夹，请用右键「全部解压缩」或 7-Zip「解压到 <包名>\」。解压后双击 `ModAVD.exe`，点上方提示里的「补全组件」下载 adb 和系统镜像，再新建实例；要接入 AUTO-MAS 的，再到 AUTO-MAS 里添加，详见 `说明.txt`。

## AUTO-MAS 版本

- 推荐 `v5.7.0` 或更新，和 0.38.1 相同。这一版对 AUTO-MAS 没有新要求。

## 已知问题

- 删除游戏或文件后虚拟磁盘不会自动缩小，模拟器的 trim 还不会真正回收空间；要省空间，请在关机后用管理器的「整理磁盘」或命令行 `modavd disk <编号> compact`。（[#2](https://github.com/ModAVD/ModAVD/issues/2)）
- 《阴阳师》冷启动偶尔停在「正在获取音频列表...」，关掉重开即可。（[#36](https://github.com/ModAVD/ModAVD/issues/36)）
- AUTO-MAS 自己起的模拟器（不经过管理器或命令行），多开时 netsimd 还可能卡住，之后所有实例都开不了机；在任务管理器里结束 netsimd 进程再开机就好。用管理器或命令行开机的实例已经不用 netsimd 了。（[#65](https://github.com/ModAVD/ModAVD/issues/65)）
- Windows 注销、重启或关机时，开着的实例还是直接断电，不会先让安卓关机，和以前一样。关电脑前请先在 AUTO-MAS 或管理器里关掉实例。（[#60](https://github.com/ModAVD/ModAVD/issues/60)）
- 《明日方舟》偶尔在启动时报资源管理器初始化失败，随后把约 12.8 GB 的资源整包重新下载一遍；遇到过一次，排查没有发现模拟器丢数据，更像游戏自己把资源索引作废了，等它下完即可。（[#89](https://github.com/ModAVD/ModAVD/issues/89)）
- 用新版 Unity（6000.3）做的部分只有 arm64 版本的游戏，比如 World of Airports 3.7.3，商店里显示不兼容，侧载以后一启动就闪退。（[#131](https://github.com/ModAVD/ModAVD/issues/131)）

## 发布前回归

这一版只重新打包，模拟器和 0.38.1 是同一份，所以只测改了的地方，每项一遍。用的是这一版的候选包：管理器和命令行是 ModAVDManager 0.5.0 发版前同一条主线编的候选版，和后来发布的 v0.5.0 逐字节相同（zip 和两个 exe 的 SHA-256 都对得上）。

| 项目 | 判据 | 结果 |
|---|---|---|
| 补全组件：新根目录从空，命令行 `components fetch`，再跑一遍 ModAVDE2E 的 fetch 项 | 平台工具和系统镜像都装好；包里没有「补全组件.bat」时只跑命令行；不往 `%LOCALAPPDATA%` 写东西 | 通过：命令行 37 秒装好两样（adb 37.0.1、系统镜像 14）；fetch 项 36 秒；`downloads\` 只剩许可协议；`%LOCALAPPDATA%` 没写 |
| Play 商店组件：`components import-playstore` 导入，挪走后 `components fetch --play-store` 下载 | 五个文件装好、记下来源；不留压缩包 | 通过：导入 1 秒；下载 1.41 GB、解出 2.89 GB，32 秒，只留五个文件和 `source.properties`，和导入的逐个 SHA-256 相同 |
| 冒烟：默认配置新建、无头开机、核对执行桥和输入法、关机 | 新建时装好输入法和执行桥；开机正常；正常关机 | 通过：新建 223 秒（6 GB / 6 核 / 64 GB / 1080p）；开机 38 秒；`bridge verify` 全过；宿主输入法是默认；正常关机 6.3 秒 |
| Play 商店：关机时 `playstore install`，冷启动打开商店，`playstore gsfid`，关机后 `playstore uninstall`，再冷启动核对 | 商店能打开到登录页；GSF ID 取得到、两种写法一致；卸掉后回到镜像自带的，执行桥、输入法不受影响 | 通过：install 124 秒、没有警告；开机 47 秒，商店到 Google 登录页（没登录）；gsfid 第一次就取到；uninstall 98 秒；冷启动后占位应用回来了，执行桥 verify 过，输入法还是默认 |
| 从 0.38.1 升级：0.38.1 自带的命令行 `modavd update install` 换上 0.38.2 | 三个 .bat 和 tools 下三个 .ps1 删掉；包根文件和新包相同；旧实例数据都在 | 通过：候选没发布，先把本地包暂存进 `.update`（核对 303 个文件），再 `update install`，5 秒换好，更新记录写了 0.38.1 → 0.38.2；六个文件删了；包根逐字节相同；`avd` 没动。升级后开机 69 秒：关机前写的标记在，三个游戏的数据大小逐 KB 相同，执行桥和输入法照旧 |
| 从 0.38.0 升级：ModAVDE2E 的 upgrade 项，按清单把 0.38.2 覆盖上去 | 旧实例数据都在；升级后照常开机、装好执行桥 | 通过：389 秒；数据在；数据盘 64 → 80 GB 扩满；执行桥自动装上；输入法和新包逐字节相同 |
| 开关机主路径：无头开机、正常关机，`stop --all` 关一台、两台 | 都正常关机；关完没有残留进程 | 通过：开机 69 秒、关机 6 秒；`stop --all` 一台 6.0 秒，两台同时 6.6 秒；关完没有残留进程 |
| 诊断模式：关机时打开再开机，运行中关掉；另一台运行中开、关 | 客体缓冲区和宿主采集按诊断规则切换；关掉后恢复、清理 | 通过：客体缓冲区 32 MiB，宿主采集加了 kernel，开头有 dmesg；运行中关掉 0.4 秒生效，清掉 2 份日志 92 MB |
| 复制配置信息、收集问题信息：命令行 `report-config`、`collect-info` | 没有用户名、电脑名和路径；问题包各节没出错；认出别的根目录里同编号的实例并跳过 | 通过：`report-config` 2.4 秒；`collect-info` 13.7 秒、31.6 MB，各节都没出错，同编号的别处实例跳过了 |
| 包内容：和 0.38.1 比 | 只去掉三个 .bat 和三个 .ps1；模拟器程序文件不变；隐私扫描 0 失败 | 通过：去掉 6 个；变了的是 ModAVD.exe、tools\modavd.exe、tools\modavd-bridge.ps1、说明.txt 和两份 NOTICE；`sdk\emulator` 下的程序没变；三条补丁系列逐提交回放，树都相同；隐私扫描 0 失败 |
| 管理器 0.5.0 | 两次编译逐字节相同；Defender 静态扫描干净；CI 全过 | 通过：两次相同；三个文件扫描干净；CI 五项全过（含 macOS、Ubuntu） |
| macOS 平台层（这一版新进管理器） | Windows 上的行为不变；三个平台都能编 | 通过：Windows 上新建、开关机、升级各项都在上面测过；CI 的 macOS、Ubuntu 两项都过。这一版只发 Windows 包，Mac 上没有实机测 |

- 关机都走 `modavd stop` 的正常关机（先 sync）；这一轮不做硬杀测试。
- 这一轮只走了命令行：管理器界面里的补全组件、Play 商店菜单，带窗口开机，`stop --all` 取消正在新建的实例，Play 商店装到一半失败时的撤回（只有单元测试），都没有在实例上单独测。
