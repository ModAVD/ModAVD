# 发版前回归（e2e 模板）

```powershell
pwsh -NoProfile -File tools\regress\regress.ps1 -Suite tools\regress\suites\release.psd1 -Scope emulator -Sdk <新 SDK> -Baseline <上一个发布版的 SDK>
pwsh -NoProfile -File tools\regress\regress.ps1 -Suite tools\regress\suites\release.psd1 -Scope manager,ime -Sdk <SDK>
pwsh -NoProfile -File tools\regress\regress.ps1 -Suite tools\regress\suites\release.psd1 -Sdk <SDK> -Only yys,datadisk
pwsh -NoProfile -File tools\regress\regress.ps1 -Suite tools\regress\suites\release.psd1 -Scope emulator -Sdk <SDK> -DryRun
```

**改了什么跑什么**：套件顶上的 `Scopes` 列出改动范围（`emulator`、`bridge`、`manager`、`ime`、
`scripts`、`repack`），每项的 `Scopes` 写它管哪些范围；`-Scope` 跑和它有交集的项。套件有范围时必须给 `-Scope` 或 `-Only`。
`PostPackage` 列打完包要用组包目录补跑的（升级、`ime-pkg`），运行器最后按范围提示。

**游戏项帧率**：`game-real.ps1` 默认只跑装桥那一轮，帧率中位数和 `baselines\fps.json`（上一个生产版的数，每个游戏有容差）比；
同机别的模拟器多于 2 个时只记录不判。每次发版回归通过后，把这一版的数写回 `fps.json`。

输出默认在 `<TempRoot>\regress-<构建>-<时间>\`（本机配置有 `TempRoot` 时 `-Out` 也必须在它下面）：每项的控制台 `<项>.console.txt`、
项目自己的输出 `<项>\`、总日志 `regress.log`、`results.json`，以及可以直接贴进 `tools\pubrel\config\changes.md`「发布前回归」的表格
`results.md`。有一项不是「通过」，退出码就是 1。

`-Manager` 是放着 `tools\modavd.exe` 和 `ModAVD.exe` 的目录（ModAVDManager 的 `build.ps1 -Out`，或解开的 Release 包；本机配置
`Regress.Manager`），发版回归用要随包发的那一版；`-PackageScripts` 默认本仓库的 `package\`，发版回归用发版克隆里的那份。

## 本机的东西：本机配置的 Regress 一节

`suites\release.psd1` 是维护者 Windows 测试机上的套件：测试用的 AVD、登着测试号的游戏实例、各游戏的自动化驱动都在仓库外。
它们的位置写在本机配置（[tools/README.md](../README.md)「本机配置」）的 `Regress` 一节，套件里用 `{Local:<键名>}` 引用，各项脚本单独跑时也从这里取默认值。
没配的键：`-DryRun` 会列出来，真跑时用到它的项记「跳过」。

| 键 | 是什么 | 谁用 |
| --- | --- | --- |
| `Manager` | 放着 `tools\modavd.exe`、`ModAVD.exe` 的目录 | 运行器、各项 |
| `TestRoot` | 测试用的 ModAVD 根目录（私有 adb 端口；0 号已装桥、登着阴阳师测试号，其余编号临时建、跑完删） | bridge、bridge-default、datadisk、yys |
| `PrevPackage` | 上一个生产版的组包目录（固定副本） | upgrade |
| `OfficialSdk` | 官方 SDK（`platform-tools`、`system-images`），新搭的根目录链到它，代替「补全组件」的下载 | upgrade、ime-pkg |
| `Launcher` | 启动器 `avd-p0.ps1`（无头 / 带窗口开 AVD，带 `-AdbPort`、`-VulkanFix` 等参数） | game-real、ime |
| `Monitor` | 采样脚本 `emu_monitor.py`（30 秒一行：帧率、重启、崩溃、ANR、Vulkan 报错） | game-real |
| `VulkanFixLib` | 客体 Vulkan 修复库 | game-real |
| `GameAvdHome` / `SrAvdHome` / `ImeAvdHome` | 游戏实例 `mas_6`、`mas_7`、`endfield`，星铁实例 `gfx_t2`，输入法冒烟机 `imesmoke` 的 `ANDROID_AVD_HOME` | game-real、bench、ime |
| `Kit` | 明日方舟（MAA）、1999（M9A）的驱动目录 | ark、r1999 |
| `SrcDir` / `SrcKit` | StarRailCopilot 和它的压测驱动 `stress.py` 所在目录 | sr |
| `MaaEndDir` | 终末地 MaaEnd 驱动 `maaend-run.ps1` 所在目录 | ef |
| `MaaYYs` | MaaYYs 的运行目录（`interface.json`、`maafw\`、`tasks\`） | yys |
| `Coldprof` | 终末地冷启动采样脚本 `coldprof.ps1` | ef-ab |
| `Bench` / `ShimDll` | 体检台 `bench.ps1` 和它用的截图垫片 dll | bench |
| `ImeTestApk` / `DevImeApk` / `BuildTools` | 输入法测试 APK、开发签名的同一个输入法、Android build-tools 36.1.0（`apksigner`） | ime-pkg |

别的平台的负责人照这个套件写自己的：运行器（`regress.ps1`）和 `items\common.ps1` 不绑平台，项目脚本和测试机按自己的环境写，
本机的东西同样放进本机配置，不写进仓库。

## 运行器做什么

- 单线程，一项一项跑。每项开跑前等宿主空闲物理内存 ≥ `NeedFreeGB`（默认 8）、提交余量 ≥ `NeedCommitGB`（默认 12），
  每分钟查一次，最多等 `MaxWaitHours`（默认 3）小时。生产和别的会话也在用这台机器。
- `Avoid` 里的时段（`'HH:mm-HH:mm'`）不跑，记「跳过」。例如测试号生产每天 06:00 / 17:50 在用。
- 每项在子进程里跑，输出强制 UTF-8。限时 `TimeoutMin`（默认 40）。超时就停在这里，后面的项都不跑：超时的那项可能还占着实例。
- 跑期间系统日志里的显卡驱动重置（nvlddmkm）计数记进结果，算不算失败由人看。
- 最后看套件 `Avds` 里列的测试机有没有 qemu 留着。

## 套件里每项的字段

| 字段 | 意思 |
| --- | --- |
| `Id` / `Title` / `Criterion` | 编号；表格里的「项目」和「判据」两列 |
| `Script` / `Args` | 要跑的脚本和参数。占位符 `{Sdk}` `{Baseline}` `{Label}` `{Out}` `{ItemOut}` `{Repo}`，`{Local:<键名>}` 取本机配置的 `Regress.<键名>` |
| `TimeoutMin` `NeedFreeGB` `NeedCommitGB` `MaxWaitHours` `Avoid` | 见上 |
| `NeedsBaseline` | 要对照版（A/B），没给 `-Baseline` 就跳过 |
| `ExitCode` | 期望的退出码，默认 0；`'any'` 表示不看退出码、只看 `Pass` / `Fail` |
| `Pass` / `Fail` | 正则：`Pass` 每条都要在输出里出现，`Fail` 一条都不能出现 |
| `Summary` | 正则：结果栏的摘要取最后一次匹配（有分组就取分组） |

## 写一个新项

脚本最后打印一行结果，运行器就不看 `Pass` / `Fail`：

```
REGRESS-RESULT {"pass":true,"summary":"到登录页 41 秒；进游戏后 52.7 fps / 平均 21.1 ms / 最慢 33 ms"}
```

`items\common.ps1` 给新项用：`Set-RootSdk` 把测试根的 `mas-avd.json` 临时指到被测 SDK、跑完换回；`M` 调测试根的 `modavd.exe`，
开机后的设置和产品一样，关机先正常关机、不成再 sync；`A` / `Su` 走测试根的私有 adb；`Result` 打结果行；`Use-Local` 取本机配置。
规矩：只用私有 adb 端口（不碰 5037，也不碰生产根目录的 20049 / 20050）；关机先 sync；临时数据放 `TempRoot` 下面；不碰生产根目录。
游戏界面只做「拉起」和「点进入游戏」这类固定操作，需要认画面时由脚本自己读像素（`yys.ps1` 认登录页左下角的适龄角标，两个位置都认），不交给模型看图。

## 现有的项（`suites\release.psd1`）

| 项 | 脚本 | 说明 |
| --- | --- | --- |
| bench | `Regress.Bench` | 体检；宿主没开测试服务器时 net 的 ERROR 不算 |
| ime | `items\ime-reg.ps1` | 宿主输入法通道 20 项；`-Attach` 那组参数（`-AdbPort` `-Serial` `-EmulatorLog`，可选 `-AdbExe` `-TestApk`）给 ime-pkg 对开着的实例跑，不带时开 `imesmoke` 自己跑 |
| bridge-default | `items\bridge-default.ps1` | 新建实例默认装执行桥，`modavd bridge` 卸 / 装，新建 7 号实例跑完删掉 |
| ark / r1999 / sr / ef | `items\game-real.ps1` | 各游戏自己的自动化脚本实跑 15 分钟（装桥那轮），帧率和 `baselines\fps.json` 比 |
| bridge | `items\bridge.ps1` | 执行桥装 / 卸全流程（Windows PowerShell 5.1），新建 6 号实例跑完删掉 |
| datadisk | `items\datadisk.ps1` | 新盘 inode 封顶、`set --data-gb` 扩容，新建 3 号实例跑完删掉 |
| upgrade | `items\upgrade.ps1` | 用上一版正式包搭根目录、建实例写数据，照「怎么更新」覆盖新包后核对开机、数据、扩容、照常开机自动装桥；`-Next` 是组包目录（带 `components\modavd-ime.apk`）时还核对宿主输入法跟着包过来、装上的是正式签名那份并设为默认（`-Next` 是 SDK 时不查）；根目录跑完删掉 |
| yys | `items\yys.ps1` | 阴阳师冷启动进游戏测帧（测试根的 0 号，已装桥、已登录），再用 MaaYYs（`items\maayys_run.py`）真打御魂 |
| ef-ab | `items\ef-ab.ps1` | 终末地冷启动新旧交替，等宿主空闲才测 |

不在套件里、打完包再单独跑的：

| 项 | 脚本 | 说明 |
| --- | --- | --- |
| ime-pkg | `items\ime-pkg.ps1` | `-Package <组包目录>`（`release\ModAVD-<版本>-win-x64`，只读）：复制成根目录、新建 9 号实例，核对正式签名的输入法开机自动装好、设为默认，带窗口跑通道 20 项，再核对开发签名的换成正式签名、稳态不变；根目录跑完删掉，日志留在 `-Work` 下 |
