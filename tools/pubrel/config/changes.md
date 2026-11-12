## 怎么更新

- 这一版只重新打包：模拟器还是 mas-40，没有重编；包里的管理器和命令行升到 0.9.0（改动见 [它的 Release](https://github.com/ModAVD/ModAVDManager/releases/tag/v0.9.0)），截图库升到 [v0.1.3](https://github.com/ModAVD/ModAVDExtras/releases/tag/v0.1.3)；执行桥、宿主输入法和 0.40.0 一样。
- 从这一版起版本号和模拟器构建号分开：0.41.0 用的还是 mas-40 的模拟器。
- 包里多了 MuMu 兼容外壳 `mumu-shim\shell\sdk\external_renderer_ipc.dll`，给单独装的 MAA 用：在 MAA 的连接设置里把 MuMu 路径填成 `<ModAVD 根目录>\mumu-shim`，实例编号直接填 ModAVD 的编号，就能用 ModAVD 的共享内存截图。用 AUTO-MAS 的不用管它。
- 包里不再带 `说明.txt`，用法看管理器和[文档站](https://modavd.github.io/)。用「更新」升级会自动删掉旧的那份，手动覆盖时会留着，可以自己删。
- 在管理器「设置」里点「更新」，或者先关掉所有实例（在 AUTO-MAS 或管理器里关机）、稍等半分钟，再用新包整个覆盖解压，`avd` 文件夹不会被动到。不用在 AUTO-MAS 里重新添加。
- 更新前请先关掉 MAA 这类载入了根目录里截图库或外壳的程序；没关的话，「更新」会提示是哪个文件被占着，什么都不动，关掉再点一次就行。
- 关掉管理器窗口以后管理器留在托盘里；手动覆盖更新前，请在托盘图标的右键菜单里点「退出」。
- 第一次装：主包里没有外层文件夹，请用右键「全部解压缩」或 7-Zip「解压到 <包名>\」。解压后双击 `ModAVD.exe`，点上方提示里的「补全组件」下载 adb 和系统镜像，再新建实例；要接入 AUTO-MAS 的，再到 AUTO-MAS 里添加。

## AUTO-MAS 版本

- 推荐 `v5.7.0` 或更新，和 0.40.0 相同。这一版对 AUTO-MAS 没有新要求：管理器 0.9.0 的接口版本（`protocol`）还是 2，`--json` 的字段没有变。
- AUTO-MAS 5.8.0 在组件不全时还会提示双击「补全组件.bat」，0.38.2 起包里没有这个文件：请在管理器里点「补全组件」，或运行 `tools\modavd.exe components fetch`，再回 AUTO-MAS 点「检查」。
## 已知问题

- 删除游戏或文件后虚拟磁盘不会自动缩小，模拟器的 trim 还不会真正回收空间；要省空间，请在关机后用管理器的「整理磁盘」或命令行 `modavd disk <编号> compact`。（[#2](https://github.com/ModAVD/ModAVD/issues/2)）
- 《阴阳师》冷启动偶尔停在「正在获取音频列表...」，关掉重开即可。（[#36](https://github.com/ModAVD/ModAVD/issues/36)）
- AUTO-MAS 自己起的模拟器（不经过管理器或命令行），多开时 netsimd 还可能卡住，之后所有实例都开不了机；在任务管理器里结束 netsimd 进程再开机就好。用管理器或命令行开机的实例已经不用 netsimd 了。（[#65](https://github.com/ModAVD/ModAVD/issues/65)）
- 《明日方舟》偶尔在启动时报资源管理器初始化失败，随后把约 12.8 GB 的资源整包重新下载一遍；遇到过两次，排查都没有发现模拟器丢数据，更像游戏自己把资源索引作废了，等它下完即可。（[#89](https://github.com/ModAVD/ModAVD/issues/89)）
## 发布前回归

（回归做完再填）
