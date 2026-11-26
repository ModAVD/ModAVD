# 安装程序

把发版 zip 编成一个离线安装程序 `ModAVD-<版本>-win-x64-setup.exe`（Inno Setup 6）。zip 照旧发，安装程序是另一种拿到同样文件的方式。

## 怎么编

要 PowerShell 7 和 Inno Setup 6（在 6.7.3 上编过）。不联网。

```powershell
pwsh -NoProfile -File tools/installer/build-installer.ps1 `
  -Source <目录>\ModAVD-0.43.0-win-x64.zip -Work <空目录> `
  -Version 0.43.0 -OutDir <输出目录> -Iscc <Inno Setup 目录>\ISCC.exe
```

- `-Source` 也可以直接给解压好的目录（顶层就是 `ModAVD.exe`），这时不用 `-Work`。
- 脚本先核对载荷：有 `ModAVD.exe`、`modavd-manifest.json`、`sdk\emulator`；没有 `avd`、`mas-avd.json` 这些用户数据；`sdk\` 下只有 `emulator`；清单里的版本和 `-Version` 一致。不对就不编。
- 0.43.0 的包（302 MB 的 zip、756 MB 解压后）编出来约 211 MB，用时一分半左右。
- `-SelfTest`：编一个不带载荷的自检版，`/VERYSILENT /LOG=<文件>` 运行后，日志里逐条写 `SELFTEST PASS` / `FAIL`，核对默认目录和目录校验的逻辑，什么都不装。

## 安装程序做什么

- 不要管理员权限：卸载项写在 HKCU，快捷方式放在当前用户的开始菜单和桌面（桌面那个可以取消勾选）。
- 推荐目录：有 D 盘就 `D:\ModAVD`，没有 D 有 E 就 `E:\ModAVD`，都没有就 `C:\ModAVD`。只算本地固定硬盘（`GetDriveType` 是 3），U 盘、光驱、网络盘不算。
- 选目录页写明程序和实例、游戏数据都放在这里，并显示所选盘还剩多少空间。
- 下面这些目录直接拦住，说清原因：`Program Files`、`Program Files (x86)`、`Windows`、`ProgramData` 之下；盘的根目录；路径里有非 ASCII 字符；路径超过 80 个字符；不是本地固定硬盘；OneDrive 同步目录（环境变量 `OneDrive`、`OneDriveConsumer`、`OneDriveCommercial`）之下；目录里已经有一份 ModAVD（`modavd-manifest.json`，或 `mas-avd.json` 加 `ModAVD.exe`），或者只剩以前留下的数据（`mas-avd.json`）。
  - 校验是一个函数 `ValidateInstallDir`：交互时在选目录页点「下一步」时调，静默安装时也会调，`PrepareToInstall` 里再调一次兜底；不合格就失败，原因写进日志，都在写文件之前。
- 装完可以勾「运行 ModAVD」；静默安装一律不启动。

## 不做什么

- 不联网、不下载组件。adb 和系统镜像照旧由管理器的「补全组件」下载。
- 不接管以后的文件变化：更新照旧是管理器自己整包替换根目录。已经装过的目录不覆盖，提示去管理器里更新。
- 不删 `%LOCALAPPDATA%\ModAVD`（管理器窗口和更新设置，同一个 Windows 用户的所有根目录共用）。

## 卸载

默认只删程序，不删用户数据。交互卸载时问一句要不要连实例和游戏数据一起删，默认「否」；选「是」才删整个目录。静默卸载（`/SILENT`、`/VERYSILENT`）一律不删数据。

管理器自更新以后会多出安装时没有的程序文件，所以卸载时按目录删程序部分（`[UninstallDelete]`）：

| 归类 | 顶层名字 | 谁写的 |
|---|---|---|
| 程序（卸载时删） | `ModAVD.exe`、`modavd-manifest.json`、`sdk\emulator\`、`tools\`、`components\`、`extras\`、`mumu-shim\`、`LICENSES\` | 发版包里的，管理器更新时整包替换（ModAVDManager `internal/update` 只许包里出现这些，不许出现下面的） |
| 用户数据（留着） | `avd\`、`mas-avd.json`（和 `.lock`） | 实例和根目录配置（`internal/root`） |
| 留着 | `logs\`、`shader-cache\`、`guest-patches\`、`downloads\`、`.update\` | 管理器运行时写的日志、着色器缓存、客体补丁、下载缓存、更新器的状态和回滚备份；更新时也不许覆盖 |
| 留着 | `sdk\` 下除 `emulator` 以外的（`platform-tools`、`system-images`、`playstore` 等） | 「补全组件」下载的 Google 组件，实例要用 |

不在表里的名字（以后的版本新加的）卸载时不删。

## 路径长度

上限 80 个字符。依据：Windows 没开长路径时整条路径最多 259 个字符。用过的根目录里最深的是 `sdk\system-images\android-34\google_apis\x86_64\data\misc\modem_simulator\etc\modem_simulator\files\numeric_operator.xml`，相对根目录 120 个字符；实例目录里最深的 76 个，着色器缓存 91 个，发版包里最深的 66 个。80 + 1 + 120 = 201，给以后更深的目录和更新时的临时目录留了约 58 个字符。

## 界面语言

按系统界面语言自动选，不弹选择框：中文系统用简体中文，其余用英文。繁体中文系统（zh-TW、zh-HK、zh-MO）也用简体中文：Inno 自动选语言时不会把简体中文分给繁体系统，所以 `[Languages]` 里另有一项 `zh_tw`，用同样的简体中文文字，语言号设成 `$0404`。静默安装可以用 `/LANG=en`、`/LANG=zh_cn` 指定。

- 向导自带的标准文字：英文是 Inno 自带的 `Default.isl`；简体中文是 [ChineseSimplified.isl](ChineseSimplified.isl)，原样取自 Inno Setup 官方仓库 [jrsoftware/issrc](https://github.com/jrsoftware/issrc) 的 `Files/Languages/ChineseSimplified.isl`，提交 `6ef32198e`（文件头写着适用 6.5.0+），不改内容（`.gitattributes` 里按原样保存）。和 6.7.3 的 `Default.isl` 比，281 条标准消息、12 条自定义消息一条不差。
- 我们自己的提示在 `messages-en.isl`、`messages-zh.isl`，两份逐条对应。

以后加一种语言：加一个 Inno 的 `.isl`、一份 `messages-<语言>.isl`，再在 `modavd.iss` 的 `[Languages]` 加一行。
