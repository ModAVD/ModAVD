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
- 脚本把载荷顶层的名字用 `|` 隔开，作为 `/DPayloadTopNames` 传给 ISCC（卸载时「连数据一起删」靠它认哪些是程序，见「卸载」）。顶层名字只许 ASCII 字母、数字和 `._-` 空格，别的不编。
- 0.43.0 的包（302 MB 的 zip、756 MB 解压后）编出来约 211 MB，用时一分半左右。
- `-SelfTest`：编一个不带载荷的自检版，这时不用给 `-Source`：

  ```powershell
  pwsh -NoProfile -File tools/installer/build-installer.ps1 -SelfTest -Version 0.43.0 -OutDir <输出目录> -Iscc <Inno Setup 目录>\ISCC.exe
  <输出目录>\ModAVD-0.43.0-win-x64-setup-selftest.exe /VERYSILENT /SUPPRESSMSGBOXES /LANG=en /LOG=<文件>
  ```

  日志里逐条写 `SELFTEST PASS` / `FAIL` / `SKIP`，最后一行 `SELFTEST DONE failures=<个数>`，什么都不装（退出码是 1，因为它在开头就退出）。中文再跑一遍用 `/LANG=zh_cn`（拦下的原因要逐字对上当前语言的提示）。核对的是默认目录、目录校验（含目录联接、8.3 短名，以及同一个系统目录带 `.`、`..`、`/`、重复的 `\`、`\\?\` 前缀、大小写不同、结尾带空格或句点的各种写法，保留设备名，不许有的字符）、卸载时认哪些顶层名字、删数据前的保险（拦下时核对是哪一道拦的），以及 `DelTree` 碰到联接时的行为。要在盘上建目录的那些用例，工作目录建在自检程序所在盘的根下（`<盘>:\mst-<随机数>`，路径短，不会碰到 80 个字符的上限），核完无论成败都删掉。建不出目录联接（`mklink /J`）、或者那个盘关了 8.3 短名时，对应的用例记 `SKIP` 和原因，不算失败。

## 安装程序做什么

- 不要管理员权限：卸载项写在 HKCU，快捷方式放在当前用户的开始菜单和桌面（桌面那个可以取消勾选）。
- 推荐目录：有 D 盘就 `D:\ModAVD`，没有 D 有 E 就 `E:\ModAVD`，都没有就 `C:\ModAVD`。只算本地固定硬盘（`GetDriveType` 是 3），U 盘、光驱、网络盘不算。
- 选目录页写明程序和实例、游戏数据都放在这里，并显示所选盘还剩多少空间。
- 下面这些目录直接拦住，说清原因：`Program Files`、`Program Files (x86)`、`Windows`、`ProgramData` 之下；盘的根目录；路径里有非 ASCII 字符；路径超过 80 个字符；不是本地固定硬盘；OneDrive 同步目录（环境变量 `OneDrive`、`OneDriveConsumer`、`OneDriveCommercial`）之下；目录里已经有一份 ModAVD（`modavd-manifest.json`，或 `mas-avd.json` 加 `ModAVD.exe`），或者只剩以前留下的数据（`mas-avd.json`）；已经存在、里面放着别的东西的文件夹（要选空文件夹或新文件夹，免得卸载时「连数据一起删」把别的东西也删掉）；路径上已经存在的某一级是目录联接、符号链接或挂载的盘（重解析点，实际会装到它指向的地方）；用了 8.3 短名（比如 `C:\PROGRA~1`，已经存在的那几级用 `GetLongPathName` 换回长名，和写的不一样就拦，否则会绕过系统目录的检查）。
  - 比之前先把写的路径规整成 Windows 实际会用的那条：`/` 换成 `\`，用 `GetFullPathName`（Inno 的 `ExpandFileName`）展开 `.` 和 `..`、合并重复的 `\`，上面各项都看规整以后的路径（`C:\不存在\..\Program Files\ModAVD` 就是 `C:\Program Files\ModAVD`，照样拦）。某一级以空格或句点结尾的（`C:\Program Files.\ModAVD`）直接拦：Windows 会悄悄去掉它们，写的和实际用的对不上（被后面的 `..` 退掉的那几级不算，`E:\foo.\..\ModAVD` 就是 `E:\ModAVD`）。某一级是 Windows 保留的设备名（`CON`、`PRN`、`AUX`、`NUL`、`COM0`–`COM9`、`LPT0`–`LPT9`，带扩展名也算）的拦，这种文件夹建不出来。盘符后面再有 `: * ? " < > |` 的拦（`E:\ModAVD:x` 是备用数据流的写法，不是文件夹）。`\\?\`、`\\.\` 开头的和网络路径一样不算本机盘符路径。卸载时「连数据一起删」的保险也先这样规整再判断。
  - 校验是一个函数 `ValidateInstallDir`：交互时在选目录页点「下一步」时调，静默安装时也会调，`PrepareToInstall` 里再调一次兜底；不合格就失败，原因写进日志，都在写文件之前。
- 装完可以勾「运行 ModAVD」；静默安装一律不启动。

## 不做什么

- 不联网、不下载组件。adb 和系统镜像照旧由管理器的「补全组件」下载。
- 不接管以后的文件变化：更新照旧是管理器自己整包替换根目录。已经装过的目录不覆盖，提示去管理器里更新。
- 不删 `%LOCALAPPDATA%\ModAVD`（管理器窗口和更新设置，同一个 Windows 用户的所有根目录共用）。

## 卸载

默认只删程序，不删用户数据。交互卸载时问一句要不要连实例和游戏数据一起删，默认「否」（在 Inno 自己的「确定要卸载吗」之后问；卸载程序没有只跳过那一句确认的参数，`/SILENT` 会连这一问也跳过）。选「是」也还要过一道保险才删整个目录：

- 目录不是盘根、不在系统目录下；
- 目录本身和它上面的每一级都不是重解析点（目录联接、符号链接）；
- 里面已经空了，或者还留着 `mas-avd.json` 或 `avd`；
- 顶层每一项都不是重解析点，而且都是 ModAVD 自己的名字：这个安装程序载荷顶层的名字（编译时从载荷读出来，`PayloadTopNames`），加上管理器运行时建的用户数据 `avd`、`mas-avd.json`、`mas-avd.json.lock`、`logs`、`shader-cache`、`guest-patches`、`downloads`、`.update`，加上 Inno 建的卸载程序（只认 `unins` 加三位数字再加 `.exe` / `.dat`，比如 `unins000.exe`、`unins000.dat`）。

有一项不对就整个不删，原因写进卸载日志。更深处的联接不用另外拦：Inno 帮助里 `DelTree` 一节写明，它删掉是重解析点的目录本身，但不进去删里面的东西（6.7.3 的帮助；自检里有一条实测）。静默卸载（`/SILENT`、`/VERYSILENT`）一律不删数据。

管理器自更新以后会多出安装时没有的程序文件，所以卸载时按目录删程序部分（`[UninstallDelete]`）：

| 归类 | 顶层名字 | 谁写的 |
|---|---|---|
| 程序（卸载时删） | `ModAVD.exe`、`modavd-manifest.json`、`sdk\emulator\`、`tools\`、`components\`、`extras\`、`mumu-shim\`、`LICENSES\` | 发版包里的，管理器更新时整包替换（ModAVDManager `internal/update` 只许包里出现这些，不许出现下面的） |
| 用户数据（留着） | `avd\`、`mas-avd.json`（和 `.lock`） | 实例和根目录配置（`internal/root`） |
| 留着 | `logs\`、`shader-cache\`、`guest-patches\`、`downloads\`、`.update\` | 管理器运行时写的日志、着色器缓存、客体补丁、下载缓存、更新器的状态和回滚备份；更新时也不许覆盖 |
| 留着 | `sdk\` 下除 `emulator` 以外的（`platform-tools`、`system-images`、`playstore` 等） | 「补全组件」下载的 Google 组件，实例要用 |

`[UninstallDelete]` 这份程序目录清单还是写死在 `modavd.iss` 里。安装时放进去的文件不靠它，Inno 按自己的记录删；它管的是管理器自更新以后多出来的程序文件。所以发版包顶层新加程序目录时，还要在 `[UninstallDelete]` 加一行，不然以后自更新放进这个目录的文件卸载时会留下（安装程序当初放进去的照样删掉）。「连数据一起删」的那道保险不用跟着改：它认的程序名字从载荷读出来。

还没顾到的：只有以后的自更新才带来、这个安装程序的载荷里没有的顶层名字，「连数据一起删」时算认不出来的一项，整个目录不删、留在原处（往安全的方向错）。

## 路径长度

上限 80 个字符。依据：Windows 没开长路径时整条路径最多 259 个字符。用过的根目录里最深的是 `sdk\system-images\android-34\google_apis\x86_64\data\misc\modem_simulator\etc\modem_simulator\files\numeric_operator.xml`，相对根目录 120 个字符；实例目录里最深的 76 个，着色器缓存 91 个，发版包里最深的 66 个。80 + 1 + 120 = 201，给以后更深的目录和更新时的临时目录留了约 58 个字符。

## 界面语言

按系统界面语言自动选，不弹选择框：中文系统用简体中文，其余用英文。繁体中文系统（zh-TW、zh-HK、zh-MO）也用简体中文：Inno 自动选语言时不会把简体中文分给繁体系统，所以 `[Languages]` 里另有一项 `zh_tw`，用同样的简体中文文字，语言号设成 `$0404`。静默安装可以用 `/LANG=en`、`/LANG=zh_cn` 指定。

- 向导自带的标准文字：英文是 Inno 自带的 `Default.isl`；简体中文是 [ChineseSimplified.isl](ChineseSimplified.isl)，原样取自 Inno Setup 官方仓库 [jrsoftware/issrc](https://github.com/jrsoftware/issrc) 的 `Files/Languages/ChineseSimplified.isl`，提交 `6ef32198e`（文件头写着适用 6.5.0+），不改内容（`.gitattributes` 里按原样保存）。和 6.7.3 的 `Default.isl` 比，281 条标准消息、12 条自定义消息一条不差。
- 我们自己的提示在 `messages-en.isl`、`messages-zh.isl`，两份逐条对应。

## 图标

`modavd.ico` 原样取自 [ModAVDManager](https://github.com/ModAVD/ModAVDManager) 的 `cmd/modavd-gui/modavd.ico`，和 `ModAVD.exe` 的图标相同。

以后加一种语言：加一个 Inno 的 `.isl`、一份 `messages-<语言>.isl`，再在 `modavd.iss` 的 `[Languages]` 加一行。
