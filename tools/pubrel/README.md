# ModAVD 发版框架

入口以 PowerShell 7 运行，辅助代码只用 Python 3.12 标准库；需要 Git、7-Zip，创建草稿时另需已登录的 `gh`。
发版检查清单见仓库根目录的 [RELEASING.md](../../RELEASING.md)，这里是工具本身的说明。

和本机有关的路径（源码树、临时目录、外部输入）一律是参数；没给的从本机配置取，见 [tools/README.md](../README.md)
和 [tools/local.example.psd1](../local.example.psd1)。两处都没有就报错，不猜默认值。

## 版本号

公开版本号 `X.Y.Z` 用 `-Version` 给，不要求 Y 等于模拟器构建号：模拟器本体重新编译才加 Y，没重编（只换组件或只重新打包）只加 Z，比如 0.40.1 之后只换了管理器和截图库就是 0.40.2。
构建号从 0.42 起是这次重编对应的版本号，参数写 `-Build 0.42`（`0.42.0` 也收），盘上一律三段：`sdk-0.42.0`、`Pkg.BuildId=0.42.0`、补丁目录 `qemu/0.42.0`、`manifest-0.42.0.xml`；`modavd-manifest.json`、公告机读行和 `changelog.json` 每条的 `build` 写字符串 `"0.42.0"`（[#219](https://github.com/ModAVD/ModAVD/issues/219)）。0.41 及以前是 mas-N（`sdk-masN`、`Pkg.BuildId=mas-N`、补丁目录 `qemu/masN`，`build` 写整数 N）：已发的目录、清单和条目都不改名，校验两种都认，`-Build masN` 只留给重编这些历史构建。新写法从 0.42 起，`-Build 0.41` 及以前会被拒。
tag 是 `v<版本>`，标题 `ModAVD 0.41.0`；上游版本（`Pkg.Revision`，如 37.2.10）写进公告第一句「ModAVD 0.40.2，基于 Google AOSP Android Emulator 37.2.10 构建」；构建号只记在清单和公告机读行的 `build` 里，公告正文不写。

0.40 及以前是旧规矩：版本 `0.<N>.<P>` 的 N 就是构建号，P 是只重新打包的次数；不给 `-Version` 时 `release.ps1` 还照这个算（mas30、mas32 的旧名 `v37.2.10-masN` 算作 `0.N.0`，只能带 `-Patch 1` 以上重新打包）。校验照旧认这些旧条目。

## 构建

```powershell
pwsh -NoProfile -File tools/build/build-sdk.ps1 -Build 0.42 -WhatIf
```

去掉 `-WhatIf` 才会实际编译。`-EmuSrc`（本机配置 `EmuSrc`）是 AOSP 模拟器源码树，`-Jobs` 默认 12、范围 1–16；
`-Python` 默认取 `MODAVD_PYTHON`、本机配置 `Python`，再是 PATH 里的 `python`；`-LogRoot` 默认 `<TempRoot>\build-sdk`；
`-MinFreeGB` 默认 120（源码树真正所在的盘，目录联接会跟过去）。
目标 `<EmuSrc>\sdk-0.42.0` 已存在时拒绝执行；不用 clean / removeleftover，不执行删除命令。源码必须提交，编译前后提交快照一致才安装产物。
产物复制到 `sdk-0.42.0/emulator`，`Pkg.BuildId` 写成 `0.42.0`（`-Build mas34` 这种旧写法是 `sdk-mas34`、`mas-34`），旁边保存 `build-info.json` 供发版入口确定对应源码。

arm64 执行桥的三个载荷文件（`emulator/resources/modavd/`）不是 AOSP 构建产物，由 `tools/gate/stage-payload.ps1` 单独放进 SDK 树：
`build-sdk.ps1` 在 `ShouldProcess` 之前先跑一次只读的 `-CheckOnly`（`-WhatIf` 也验），编完 robocopy、写完 `source.properties` 之后再真正放进去。
`-GatePayload <已冻结目录>` 指定载荷来源，不给就按 `stage-payload.ps1` 的顺序找。
载荷哈希固定在 `tools/gate/payload.SHA256SUMS.txt`，重编过载荷要先 `stage-payload.ps1 -UpdateSums` 重新固定，否则打包直接停。机制见 [tools/gate/README.md](../gate/README.md)。

## 外部输入

这些不在仓库里，发版前要备好（括号里是本机配置的键名）：

| 输入 | 是什么、从哪来 |
| --- | --- |
| SDK（`-Sdk`） | `build-sdk.ps1` 编出的 `sdk-0.N.0`（旧构建是 `sdk-masN`），带 `build-info.json` |
| 源码树（`EmuSrc`） | 编这个 SDK 的 AOSP 模拟器源码树。补丁从 `build-info.json` 记的提交导出，变了的源项目从这里 `git archive` |
| 源码基准（`Baseline`） | 没变的源码归档直接复用，逐个核对 `config/mas30.SHA256SUMS.txt` 里冻结的哈希。可以用 mas30 Release 的源码包做：`ModAVD-37.2.10-mas30-sources.zip` 里那个文件夹的内容放进 `<Baseline>\source\`，仓库里的 `release/37.2.10-mas30.sources.SHA256SUMS.txt` 复制成 `<Baseline>\SHA256SUMS-sources.txt`（这个做法按代码推出，还没照着从头做过一遍） |
| 说明和许可证（`LicensePackage`） | 已审核的 `LICENSES\`（`NOTICE.txt`、`NOTICE.csv`、各许可证全文）。预检要求 `LICENSES\AGPL-3.0.txt` 在，且换行统一成 LF 后的 SHA-256 是 `release_tools.py` 的 `AGPL_SHA256`（就是 `package/LICENSE`），否则停。目前只在维护者本机 |
| 桌面 APK（`LauncherApk`） | `components\` 下的桌面，文件名和 SHA-256 固定在 `config/components.SHA256SUMS.txt` |
| 管理器（`-Manager`） | ModAVDManager 的 Release 附件 `ModAVDManager-<版本>-win-x64.zip` |
| 宿主输入法（`-ImeApk`） | ModAVDHostIME 的 Release 附件 `modavd-ime.apk`；不给就不带 |
| 截图库（`-ExtrasDll`） | ModAVDExtras 的 Release 附件 `modavd_extras.dll`，放成 `extras\modavd_extras.dll`；每个包都带，必须给 |
| MuMu 兼容外壳（`-MumuShim`） | 同一版 ModAVDExtras 的 Release 附件 `external_renderer_ipc.dll`，放成 `mumu-shim\shell\sdk\external_renderer_ipc.dll`；和 `-ExtrasDll` 一起，必须给 |
| 三个公开仓库的克隆（`ManagerRepo`、`ImeRepo`、`ExtrasRepo`） | 只读 tag，不管当前检出哪个分支：先 `git fetch upstream --tags` |
| 私有隐私规则（`PrivateRules`） | 维护者自己的 `privacy-private-rules.json`，放在仓库外，绝不提交（见下面「隐私规则」） |

## 发版

公告的「更新内容」不手写。用户看得见的改动，各自的 PR 里带一个说明文件（`changelog.d/<名字>.<feat|fix|change|remove>.md`，一行话，写法见 [changelog.d/README.md](../../changelog.d/README.md)）；发版时把上一版以后新加的文件汇总进 `config/changelog.json`（[#216](https://github.com/ModAVD/ModAVD/issues/216)）：

```powershell
pwsh -NoProfile -File tools/pubrel/collect-changelog.ps1 -Build 0.42 -Version 0.42.0 [-Note "更新前先把实例都关机"] [-MinAutoMas 5.9.0-beta.1]
```

- 读四个仓库的 `changelog.d`：ModAVD 的发版克隆读 HEAD；管理器、截图库、输入法读各自 `upstream` 的 `release/<X>.<Y>` 头（路径取 `-ManagerRepo`、`-ExtrasRepo`、`-ImeRepo` 或本机配置，先 `git fetch upstream --tags`）。没有这条发版分支的组件算这一版没重新打包，不出条目。
- 「新加的」指这个提交有、上一版（往回最近的那个 `v*` tag）没有的文件。已经发过的文件改了内容会报错：以后的改动另起一个文件。
- 文件名里的类型就是分类：`feat` 新增，`fix` 修复，`change` 变更，`remove` 移除。条目是文件里那一行，末尾补上加这个文件的 PR（合并提交标题末尾的编号），写成 `(#N)` 或 `(ModAVD/ModAVDManager#N)`；那一行自己以 issue 或 PR 编号结尾的就不补。
- 文件不合格（名字不对、不止一行、超过 60 字、有 `--`）会一次全报出来，回去改文件再跑。ModAVD 自己的 `changelog.d` 在 CI 的 `checks.py metadata` 里也查。
- 每个仓库还会列出上一版以后没带说明文件的提交，过一眼有没有漏掉用户看得见的改动。
- 「说明」类用 `-Note` 手写，只写这一版要用户动手的事；重新汇总时留着。`-MinAutoMas` 不给就沿用上一版的，`none` 表示没有要求；它只进第一行机读注释，公告正文不写。构建号是 `0.N.0` 的版本，`min_automas` 至少要 `5.9.0-beta.1`：AUTO-MAS 5.8.0 及以前开机前按 `Pkg.BuildId=mas-N` 查内测包版本，认不出 `0.N.0` 就拦住开机，低了汇总和预检都会停。
- 写出来的是 `changelog.json` 最前面本版那一条，`{"version": "v0.42.0", "build": "0.42.0", "min_automas": "5.9.0-beta.1", "changes": {"修复": ["…"]}}`（0.41 及以前的条目是 `"build": 41` 这样的整数，照旧留着），重跑会整条换掉。第一行机读注释带 `changelog.json` 最前面最多 10 条，给 AUTO-MAS 跳版本时合并显示；整行超过 8000 字预检就停，这时只能删掉最老的几条或者缩短旧条目。

公告里除了「更新内容」只有开头一行：基于的上游版本、各组件的版本（本版重新打包的会标出来）和文档站的链接。怎么更新、已知问题、回归怎么测的都不写进公告；这一版有要用户动手的事，用 `-Note` 写成一条「说明」。

管理器、截图库、输入法这一版重新打包了，版本号就取这一版 ModAVD 的版本号，没重新打包的不动；还没打 tag 的组件版本号对不上，预检就停。它们自己仓库的 Release 说明也不手写，在组件的发版分支上生成：

```powershell
python -I tools/pubrel/release_tools.py component-notes --component manager --version 0.41.1 --sums release/0.41.1.SHA256SUMS.txt --notes-output release/0.41.1.md
```

`--component` 是 `manager`、`extras` 或 `ime`；生成的是中英各一句「随 ModAVD 0.41.1 发布」加校验表。

汇总完先看公告长度，不用 SDK、不打包：

```powershell
python -I tools/pubrel/release_tools.py notes-budget --build 0.42 --version 0.42.0 [--sdk <SDK 根目录>]
```

报出机读行（限 8000 字，带 `changelog.json` 最前面最多 10 条的改动，旧版的条目也算在里面）和分隔行以上的公告（限 4000 字）各多长；超了就报错并说该删哪里，过了 90% 会提醒。没给 `--sdk` 时上游版本按 `99.99.99` 估，比真的多一两个字。`release.ps1` 的预检用同一个算法再查一遍，超了在打包之前就停。

再检查本次 NOTICE、组件来源和许可证；在测试机上跑发版前回归（[ModAVDE2E](https://github.com/ModAVD/ModAVDE2E)），结果自己留着，不进公告。

```powershell
pwsh -NoProfile -File tools/pubrel/release.ps1 -Build 0.42 -Version 0.42.0 -Sdk <SDK 根目录> -Manager <下载的 ModAVDManager-0.3.0-win-x64.zip> -ImeApk <下载的 modavd-ime.apk> -ExtrasDll <下载的 modavd_extras.dll> -MumuShim <下载的 external_renderer_ipc.dll>
```

默认只生成本地产物和隐私报告，输出 `<TempRoot>\pubrel\0.42.0-v0.42.0`。

管理器、输入法、截图库、执行桥源码和随包脚本都取公开仓库：管理器、输入法和截图库用各自 Release 的附件（管理器、截图库还没打 tag 时见下面一节），
随包脚本和执行桥源码取发版克隆（`-Repository`，默认就是这份工具所在的克隆）当前提交的 `package/`、`bridge/`。
发版前先把发版克隆更新到 ModAVD 的发版分支 `release/<X>.<Y>`（版本号的前两位，比如 0.41.x 是 `release/0.41`；见 [RELEASING.md](../../RELEASING.md)「发版分支」），`ManagerRepo`、`ImeRepo`、`ExtrasRepo` 三个克隆 `git fetch upstream --tags`。

### 组件还没打 tag 时

管理器和截图库不用先发版。它们的发版准备提交（版本号、`release/<版本>.md`、`release/<版本>.SHA256SUMS.txt`）合进各自仓库的
`release/<X>.<Y>`（这一版 ModAVD 版本号的前两位）以后就能打包：

- `v<版本>` 的 tag 在，就按 tag 核对，和以前一样。
- tag 不在，取那个仓库 `upstream` 的 `release/<X>.<Y>` 的头（先 `git fetch upstream`）：那个提交的版本号要是这一版
  （管理器看 `internal/buildinfo/buildinfo.go`，截图库看 `CMakeLists.txt`），给进来的文件要和那个提交上的
  `release/<版本>.SHA256SUMS.txt` 对上。预检和组包的输出里写着「NOT TAGGED YET」和取的提交。
- `-Manager` 给本机从那个提交编的包（ModAVDManager 的 `build.ps1` 加 `mkzip`，同一份源码编出的字节相同）；
  `-ExtrasDll`、`-MumuShim` 给那个提交的 CI 产物 `windows-libs` 里的两个 DLL（换一个 MSVC 编出的字节就不同，本机编的对不上）。
- 包里记的是版本号和提交，`SOURCES.txt` 和 `modavd-src.tar.xz` 打没打 tag 都一样。回归用的就是这个输出；
  发布时先把 tag 打在记下的提交上，不重新打包。
- `-DraftRelease` 只收组件都打了 tag 的包。

输入法不走这条路：它要本机正式签名，照它自己的流程先发好。

| 参数 | 用途 |
| --- | --- |
| `-Build` / `-Sdk` | 必填，构建号 `0.N`（0.42 起；旧构建 `masNN`，mas32 及更早的只能重新打包）、SDK 根目录；检查 `Pkg.BuildId`（`0.N.0` 或 `mas-NN`），`Pkg.Revision` 记为上游版本 |
| `-Version` | 公开版本号 `X.Y.Z`，要比 `changelog.json` 里已有的都新；输出目录 `<构建号>-v<版本>`（`0.42.0-v0.42.0`、`mas40-v0.41.0`）。给了就不再用 `-Patch` |
| `-Patch` | 不给 `-Version` 时才用（0.40 及以前的旧规矩）：默认 0，只重新打包时加一，版本变成 `0.<N>.<P>`，输出目录 `<构建号>-pP` |
| `-Changelog` | 默认 `config/changelog.json`；试运行时指向临时副本 |
| `-Snapshot` | 默认读 SDK 的 `build-info.json` |
| `-EmuSrc` | 源码树（`EmuSrc`） |
| `-Baseline` | 源码基准（`Baseline`），见「外部输入」 |
| `-Package` / `-Apk` | 已审核的说明/许可证目录（`LicensePackage`）、桌面 APK（`LauncherApk`） |
| `-ImeApk` | 宿主输入法 APK，放成 `components\modavd-ime.apk`；默认空，即不带。包里每个 APK 都要在 `config/components.SHA256SUMS.txt` 固定 SHA-256：没固定的被拒，固定过的漏带或内容不同也被拒（漏带会让增量更新删掉用户机器上的文件）。预检还查 NOTICE.txt 里有它的条目。固定文件每行 `<小写 sha256>  components/<文件名>`（两个空格，不分大小写也不能重名；`Get-FileHash` 的输出要先转小写）。固定是全局的：IME 一旦写进去，重新打包 mas35 以前的构建要临时改回。另外要在 `-ImeRepo` 某个 tag 的 `release/<版本>.SHA256SUMS.txt` 里找得到这个 APK（列成 `modavd-ime.apk`），源码包放的就是那个 tag |
| `-ExtrasDll` | ModAVDExtras 的截图库，放成 `extras\modavd_extras.dll`（0.39.0 起；MAA 这类工具从 ModAVD 根目录的 `extras\` 载入它，路径不变）；每个包都带（已发过的文件下一版不能没有，否则增量更新会删掉用户的文件），命令行没给就停。本仓库不钉它，和管理器一样：版本是 `-ExtrasRepo` 的 `upstream/release/0.<N>` 头上 `CMakeLists.txt` 写的（打了 tag `v<版本>` 就认 tag；没取到这个分支时，找校验表列着这个 DLL 的那个 tag），那个提交的 `release/<版本>.SHA256SUMS.txt` 要把这个 DLL 列成 `modavd_extras.dll`，源码包放的就是那个提交。发版分支在时，旧一版的 DLL 对不上就停。DLL 是 LGPL-3.0-or-later：NOTICE 条目在 `config/notice-additions.*`，预检还查许可证包里有 `LICENSES\LGPL-3.0.txt`、`LICENSES\GPL-3.0.txt`（换行统一成 LF 后的 SHA-256 是 `LGPL_TEXTS`）。截图库发新版，本仓库什么都不用改 |
| `-MumuShim` | ModAVDExtras 的 MuMu 兼容外壳，放成 `mumu-shim\shell\sdk\external_renderer_ipc.dll`（单独装的 MAA 只认 MuMu 的截图接口，会到 MuMu 路径下载入 `shell\sdk\external_renderer_ipc.dll`；用户在 MAA 里把 MuMu 路径填成 `<ModAVD 根目录>\mumu-shim`）；和 `-ExtrasDll` 一起，必须给。和截图库同一版：`-ExtrasRepo` 同一个提交的 `release/<版本>.SHA256SUMS.txt` 要把它列成 `external_renderer_ipc.dll`。NOTICE 条目、许可证全文的检查和截图库相同。这是 MAA、MaaFramework 原生支持 ModAVD 以前的过渡办法；以后从包里拿掉时要记得，增量更新会把用户根目录里的这个文件删掉（AUTO-MAS 往同一路径拷的那份也一样） |
| `-ExtrasRepo` | ModAVDExtras 的克隆（`ExtrasRepo`；不带截图库时可以不给）；只读 tag 和 `upstream` 的发版分支，都没取下来就停 |
| `-Manager` | 必填，`ModAVDManager-<版本>-win-x64.zip`：ModAVDManager 的 Release 附件，或者还没打 tag 时从发版分支编的。包和里面的 `ModAVD.exe`、`tools/modavd.exe` 都要和 `-ManagerRepo` 里 tag `v<版本>`（没有 tag 时是发版分支头）的 `release/<版本>.SHA256SUMS.txt` 一致，包里的两个 exe 就取自它；NOTICE 里的 Go 版本取那个提交的 go.mod |
| `-ManagerRepo` / `-ImeRepo` | 两个公开仓库的克隆（`ManagerRepo`、`ImeRepo`；不带输入法时 `-ImeRepo` 可以不给）；管理器读 tag 和 `upstream` 的发版分支，输入法只读 tag，没取下来就停 |
| `-Repository` | ModAVD 的发版克隆，默认是这份工具所在的克隆。随包脚本（`package/`）和执行桥源码（`bridge/`）取它当前提交，这两处有没提交的改动就停；清单、补丁、Release 说明同步进它 |
| `-OutputRoot` / `-OutputName` | 默认 `<TempRoot>\pubrel`；名称只允许默认名或 `<默认名>-check`。输出根目录和发版克隆都不能在源码树、SDK、本机配置 `ProtectedRoots` 列的目录里，输出根目录也不能在发版克隆里 |
| `-PrivateRules` | 仓库外的私有隐私规则（`PrivateRules`，或环境变量 `MODAVD_PRIVATE_RULES`）；开跑前就查，没有就停 |
| `-PrivacyCache` / `-NoPrivacyCache` | 第 8 步的扫描缓存（`PrivacyCache`；都没给就是 `<OutputRoot>\privacy-cache.json`），只在本机，不能在输出目录和发版克隆里；`-NoPrivacyCache` 全部重扫；带 `-DraftRelease` 的那一轮不读缓存、全部重扫，扫完照样写。见「共享校验与隐私规则」 |
| `-MinFreeGB` | 写之前要求的磁盘空闲，默认 120（`MinFreeGB`） |
| `-Force` | 只重建同名、有匹配所有权标记、没有链接的本脚本输出目录；不会清理别的目录 |
| `-Python` / `-SevenZip` | 指定可执行文件；Python 也可由 `MODAVD_PYTHON` 指定，再是本机配置 `Python`、PATH 里的 `python.exe`；7-Zip 再是本机配置 `SevenZip`、PATH 里的 `7z.exe`、`Program Files\7-Zip` |
| `-DraftRelease` | 全部校验通过后创建 GitHub 草稿（标题 `ModAVD <版本>`，tag 指向 `release/<X>.<Y>`）；上传主包、源码总包、外层校验表这三个附件。有组件还没打 tag 时预检就停 |

### 发版 PR 和 GitHub 上的两个 workflow

打包、私有规则全扫、发布前回归、建草稿传附件照旧在本机；核草稿、关 issue 由 GitHub Actions 做（[#134](https://github.com/ModAVD/ModAVD/issues/134)）。为此发版 PR 要带两样机读的东西，都由 `release-pr.ps1` 从发版清单（维护者本机的 `.psd1`，里面有 `Version`、`Manager`、`Issues`、`ReviewedHead`）生成：

```powershell
# 写 release/<版本>.issues.json，和发版提交一起进发版 PR
pwsh -NoProfile -File tools/pubrel/release-pr.ps1 -PlanFile <清单.psd1> [-Repository <发版克隆>]
# 审过以后把 ReviewedHead 填进清单，给 PR 正文文件写上 Reviewed-Head 那一行，再 gh pr edit <号> --body-file <文件>
pwsh -NoProfile -File tools/pubrel/release-pr.ps1 -PlanFile <清单.psd1> -BodyFile <PR 正文.md>
```

- `release/<版本>.issues.json`：`{"version": "0.42.0", "manager": "0.42.0", "issues": [{"issue": 103, "prs": ["ModAVDManager#54"], "kind": "close", "note": ""}]}`。
  `prs` 写「仓库#号」或「仓库@提交」，模拟器源码补丁没有 PR 就留空、在 `note` 写是哪个补丁；`kind` 是 `close`（关掉、附发布链接）、`comment`（已经关了，补一条「已随发布」）、`keep`（`note` 当评论发、不关）。带 `pending-release` 的 issue 都要写进来。格式由 `release_ci.py` 的 `load_issues` 核对，写错了生成时就报。
- PR 正文里单独一行 `Reviewed-Head: <40 位提交>`，正好一行，写审过的那个头。审过以后 PR 又进了提交，就重新审、重新写这一行。

两个 workflow（核对逻辑都在 `release_ci.py`，单测 `test_release_ci.py` 在 `check.yml` 里跑）：

- `verify-release.yml`：建好草稿以后手动触发 `gh workflow run verify-release.yml --repo ModAVD/ModAVD -f version=<版本>`。在 GitHub 的机器上下载草稿的附件、算 SHA-256，核：附件正好是两个 zip 和 `SHA256SUMS.txt`，和发版提交（草稿的目标，要在 `release/0.<N>` 上；tag 已在的要指向它）上的 `release/<版本>.SHA256SUMS.txt` 对上，GitHub 记的摘要也一样；Release 标题和说明（`release/<版本>.md`）对上；合并成这个提交、合进 `release/0.<N>` 的发版 PR 正好一个，正文的 `Reviewed-Head` 是这个 PR 的提交、树和合并提交相同。过了才发布。草稿只有能写仓库内容的令牌看得见，所以下载那个 job 是 `contents: write`，它不检出、不跑仓库里的代码，只用 `gh` 和 `sha256sum`，把哈希交给只读的核对 job。
- `close-released-issues.yml`：网页上点发布（`release: published`）时自动跑，也可以 `gh workflow run close-released-issues.yml --repo ModAVD/ModAVD -f version=<版本>` 重跑。只用 `GITHUB_TOKEN`（`issues: write`，其余只读）。读 tag 所在提交上的 `release/<版本>.issues.json`，逐条核修它的 PR：ModAVDManager 的 PR 合并提交要在 `v<manager>` 这个 tag 里；ModAVD 的要是发版提交的祖先（GitHub 记的合并提交因为 main 改写过不在 main 上时，改按 main 上标题以「 (#号)」结尾、树相同的那一个提交，不是正好一个就跳过）；ModAVDE2E、ModAVDHostIME 合了就算；「仓库@提交」直接核这个提交。都过了的按 `kind` 关掉或评论（评论附发布页链接），去掉 `pending-release`；核不过的不动、留着标签，列在 job summary 里，下一版再核。重跑只做还没做的：评论里已经有发布链接的不再评论，关或评论失败的不去标签。发布时触发的是 tag 所在提交上的这个文件：发版分支在它合进 main 以前切的，发布时不会跑，要手动触发。

推三个源码仓库（ModAVDQemu、ModAVDGfxstream、ModAVDCrashpad）的 `release/0.<N>` 和 tag、换默认分支要能跨仓库写的凭据，还没上 CI，照旧在本机做。

产物（以 `0.33.0` 为例）：

- 主包 `ModAVD-0.33.0-win-x64.zip`，**没有外层文件夹**，包根就是 ModAVD 根目录：Mirror 酱按路径比文件，带版本号的外层文件夹会让增量等于全量（不再另出 `-update.zip`）。用户用「全部解压缩」或 7-Zip「解压到 <包名>\」会得到同名文件夹。文件名带平台，以后可能有 `linux-x64`；平台写在 `release_tools.py` 的 `PLATFORM` 和 `release.ps1` 里。
- 主包根目录有 `modavd-manifest.json`：版本、构建号、上游版本、平台、最低 AUTO-MAS 版本，和包里其余每个文件的大小、SHA-256。`verify-zip` 核对主包与组包目录逐文件一致（带外层文件夹就报错）、清单与文件一一对应。
- 源码总包 `ModAVD-0.33.0-sources.zip`（和平台无关，不带平台），用 7-Zip `-tzip -mx0 -mcu=on`，外层文件夹同名。内部源码归档保持原文件名和字节；另附内部 `SHA256SUMS.txt`。
- 外层 `SHA256SUMS.txt` 只列主包和源码总包两个 ZIP。
- `release-notes.md`：第一行机读注释，然后是更新公告（一行各组件的版本，和更新内容），再是分隔行 `<!-- modavd:github-only -->`，以下是下载表、源码说明。分隔行以上同步到 Mirror 酱、在 AUTO-MAS 里显示，限 4000 字，不能有哈希。
- `release-assets.json` 记录供仓库 CI 使用的大小与哈希。
- `patches/`：要进发版提交的补丁。`series.json`、`BASE.md` 是发版克隆（`-Repository`）里那两份加上这一版，直接覆盖仓库里的：
  - 和已发布的某个系列仓库、基准、分支头都相同的，沿用那个目录（比如 Crashpad 一直是 `crashpad/module-version22`），不加新目录；它的导出放在 `work/patches-reused/` 给回放和 incoming 核对用，不在 `patches/` 里。
  - 其余的是新目录（`patches/<仓库>/<构建号>/`，0.42 起如 `qemu/0.42.0`，以前是 `qemu/masN`）：`series.json` 最前面加一条；`BASE.md` 版本表加一行（同一构建再发一个版本，比如重新打包，按构建号找到那一行，只补「；<版本> 相同」），目录表在同一仓库的旧行上面加一行，目录表下面的说明加一句（和上一个目录前几个补丁相同、从第几个起是新增的，按 patch-id 算）。
  - 新补丁的许可证、新增的补丁做了什么只能人写，留成「发布前填写」：填完再提交，`checks.py metadata` 见到它就失败。版本表下面的许可证表、回放示例也按需要手改。
  - 发版克隆里已经是加过这一版的（重跑一遍），不会重复加。`logs/patch-dirs.json` 记每个仓库这一版用哪个目录、是不是新的。

`config/sdk-inventory.csv` 是审核过的包装政策：目前 SDK 文件集固定，移除 103 个文件。新 SDK 增减文件会停止，需先审核并更新政策。
其中 `resources/modavd/` 三行是我们自研的 arm64 执行桥载荷（AGPL-3.0-or-later），由 `tools/gate/stage-payload.ps1` 放进 SDK 树；
它们在 `sdk/emulator` 下面，所以内测「只发 `sdk\emulator`」那条规矩不用改。包里的 `tools/modavd-bridge.ps1`（装 / 卸 / 核对执行桥）
按提交从发版克隆的 `package/` 取原字节（那边 `package/** -text`，存的就是包里的字节）。0.38.1 及更早的包里还有三个 .bat 和
`fetch-components.ps1`、`collect-info.ps1`、`report-config.ps1`，0.38.2 起这些事由管理器和命令行做，包里不再有（ModAVD/ModAVD#45）；
用 `modavd update` 升级时，旧清单里有、新包里没有的包根文件会被删掉。

0.36.0 起包里还有 `ModAVD.exe`（管理器）和 `tools/modavd.exe`（命令行、MCP），取自 `-Manager` 那个 Release 包（ModAVDManager 自己的发版流程可复现编译并核对过）。

P3 审核过的 NOTICE 之后新发的、不再发的东西，由 assemble 按锚点改进去：程序图标条目去掉「同人」的说法、改名「ModAVD program icon」（`ICON_NOTICE_EDITS`）、已有条目里拿掉不再发的文件（`NOTICE_GONE_FILES`；0.40.1 起包里不再带 `说明.txt`，用法看管理器和文档站，整包更新会按旧清单把它删掉）、
加上新发的文件（`NOTICE_MORE_FILES`）、Part 2 末尾加 `config/notice-additions.txt` / `.csv` 的条目（执行桥载荷、modavd 两个 exe、Go 运行时的 BSD-3 声明，Go 版本取自管理器 tag 的 go.mod；0.39.0 起还有截图库的 LGPL-3.0-or-later 声明）、
锚点或原文找不到、不止一处就停。
补完两个方向都核对：包根以下（`sdk/` 除外）每个文件、`sdk/emulator/resources/modavd/` 每个文件都要在 NOTICE.txt 里有条目，许可证全文除外；
NOTICE.txt 里每个 `(package root)` 文件都要在包里。以后往包里加文件不补条目，或拿掉文件不改条目，打包都会停。

源码总包多一个 `modavd-src.tar.xz`（0.36.0 时叫 `modavd-src-<提交>.tar.xz`）：各公开仓库的原样 `git archive`，
`ModAVDManager/` 是 `-Manager` 那个 tag，`ModAVDHostIME/` 是 `-ImeApk` 对上的 tag，`ModAVDExtras/` 是 `-ExtrasDll` 对上的那个 tag（管理器、截图库还没打 tag 时是发版分支头上的那个提交），`ModAVD/bridge/` 是发版克隆当前提交的 `bridge/`。
归档根目录另有本仓库 HEAD 的 `LICENSE`（`package/LICENSE`，AGPL-3.0 全文，核对 `AGPL_SHA256`）和 `NOTICE`（`config/own-sources-NOTICE.txt`：哪些是自研的、例外和第三方部分、各部分的公开位置）（`OWN_SOURCES_ROOT`）；
时间戳、属主沿用 `git archive`，根目录两个文件取三个提交里最新的提交时间，可复现。`SOURCES.txt` 第 6 节写明各部分的 tag 和提交。
`LICENSE`、`NOTICE` 有没提交的改动时预检就停。随包脚本本身就是源码，随主包原样发出，不进这个归档。

自研部分（管理器和命令行、执行桥、宿主输入法、随包脚本）按 AGPL-3.0-or-later 发布，公开位置：
管理器 https://github.com/ModAVD/ModAVDManager，输入法 https://github.com/ModAVD/ModAVDHostIME，随包脚本（装执行桥的 `modavd-bridge.ps1`）https://github.com/ModAVD/ModAVD/tree/main/package，
执行桥 https://github.com/ModAVD/ModAVD/tree/main/bridge。
截图库（`extras\modavd_extras.dll`）按 LGPL-3.0-or-later 发布，源码在 https://github.com/ModAVD/ModAVDExtras，`modavd-src` 的 NOTICE 写明这个例外。
补丁按所改文件的许可证；ModAVD 仓库根目录的 LICENSE 仍是 Apache-2.0；图标不在任何许可证范围内。
24 个源项目发生变化时重新 `git archive`；其他 manifest 项目（尤其 prebuilts）发生变化会停止，需重新审核对应源码及 NOTICE。
P3 审核过的说明和 NOTICE 里写的是 `37.2.10-mas30`，组包时换成新版本号，`mas-30` / `mas30` 换成本次构建号（0.42 起两处都是 `0.42.0`，清单文件 `manifest-0.42.0.xml`）。
清单模板是仓库里审核过的 `manifests/manifest-mas30.xml`。

## 共享校验与隐私规则

`checks.py`、`privacy.py` 和白名单 `privacy-allowlist.json` 只有一份，就是仓库的 `scripts/`：公开 CI 和发版入口用的是同一份。

- `checks.py metadata --root <公开仓库>` 校验 Release 表格、两层校验表、大小记录与 `.gitattributes`，0.33.0 起的说明另查机读行、分隔行和公告长度；
  `patches --root <公开仓库> --work <全新临时目录>` 直接从 AOSP 浅取基准，回放三条系列并比对最终源码树。
- `privacy.py <公开仓库>` 扫描 Git 跟踪文件；发版入口用同一模块扫描主包、补丁、源码包全部成员和元数据。
- 仓库里只有通用规则（`PUBLIC_RULES`）。点名私人信息的规则（真名、私人邮箱、主机名、本机目录）在维护者自己的
  `privacy-private-rules.json` 里，**放在仓库外，绝不提交**：用环境变量 `MODAVD_PRIVATE_RULES`、`privacy.py --private-rules <文件>`
  或 `privacy.use_rules(<文件>)` 加载，指了却不存在就报错。公开 CI 不带它跑；发版的第 8 步没加载到就停。

发版第 8 步按文件哈希缓存扫过干净的结果（`release_tools.py` 的 `cached_privacy_scan`，`privacy.py` 本身不变）：

- 和上次扫过的文件内容（SHA-256）相同、位置相同（去掉主包外层的 `ModAVD-<版本>-<平台>/`，所以上一版没变的文件也算）的，不再扫；源码归档按整个文件的哈希算，没变的归档连解压都不用。
- 扫描器的代码、规则（公开的和私有的）、Python 的大版本和 Unicode 版本（哪些字符串算可打印、要查，取决于 Unicode 库）一变，整个缓存作废。白名单不在里面：每条缓存记着它用到的例外，命中时逐条在现在的白名单里再查一遍，撤掉的例外会让用到它的文件重扫、照常报失败。
- 只缓存干净的；有失败的文件每次都扫。报告（`logs/privacy.json`）和不用缓存时一样，多一个 `cache` 记命中、重扫了多少。
- 缓存每次只留这一轮用到的条目，跟着最新一版走；文件坏了就整个丢掉重来，某一条的格式不对就只丢那一条、那个文件重扫。
- 缓存是本机的输入，照格式写一条「干净」的记录就能让一个文件不被扫。所以**真要上传的那一轮（`release.ps1 -DraftRelease`）第 8 步不读缓存**，全部重扫（0.38.0 约 7 分钟），扫完照样写缓存；其余各轮照用。
- 上面每一条 `test_release.py` 都有测试（没变的跳过且报告不变、改了内容 / 换了位置 / 撤了例外 / 换了规则、扫描器、Python 或 Unicode 版本就重扫、有失败的不缓存、格式不对的条目丢掉重扫、上传那一轮不信伪造的「干净」记录、缓存不能放进输出和仓库）。

白名单只允许已核查的第三方作者/词表/测试数据/示例路径、上游预编译件的构建路径及二进制巧合。
每条例外固定「规则 + 完整可打印字符串 SHA-256」，并固定具体源文件路径或整个文件 SHA-256；不按姓氏全局豁免。记录的理由不复制私人字符串进仓库。
新命中要核对来源后人工更新白名单；禁止直接把新扫描报告全部加入白名单。

## 失败路径测试

```powershell
python -I tools/pubrel/test_release.py --work <全新临时目录>
python -I tools/pubrel/test_release_ci.py
```

两条都在 CI（`check.yml`）里跑。第二条是两个发版 workflow 的核对逻辑（`release_ci.py`），用假的 GitHub，不连网：关 issue 的各种核不过（PR 没合、合并提交不在这一版或管理器 tag 里、main 改写后找不到或找到不止一个、不认识的仓库、还开着的 `comment`）、没有 `pending-release` 的不去标签、跳过的不动、重跑不重复评论、关或评论失败时留着标签再跑接着做、Release 没发布或没有 `issues.json` 就不动、`issues.json` 读 tag 所在提交上的那份、`issues.json` 的格式；核草稿的附件哈希、GitHub 摘要、附件多了少了、`SHA256SUMS.txt` 的内容、标题和说明、发版提交不在发版分支上、tag 指向别处、发版 PR 找不到、`Reviewed-Head` 没有或不止一行、不是 PR 的提交、树不同；分页的下一页链接不能离开 API。

不需要维护者的私有规则：涉及私有规则的几项用测试里自造的规则。
覆盖输出所有权、路径越界、incoming 在 `core.autocrlf=true` 的检出里也删得掉、补丁记录在已发布的 `series.json` / `BASE.md` 上追加（沿用目录、重跑不重复、重新打包）、公告长度在打包前就算出来、发版克隆不许落在受保护目录、ASCII / UTF-16 / UTF-8 / tar 隐私拦截、私有规则从仓库外加载（指了不存在就报错、没加载到发版扫描就停）、白名单边界、表格哈希/大小/重复/缺项、ZIP 原始反斜杠/内容错误、主包带了外层文件夹、清单缺漏或内容不符、版本号、公告的机读行/分隔行/长度/哈希、「更新内容」从各仓库 `changelog.d` 汇总（只取上一版以后新加的、补上加文件的 PR 编号、没有发版分支的组件不出条目、发过的文件改了或文件不合格一次报全、重跑留着手写的「说明」、版本号放不进去时不动文件）、公告里的组件版本行、组件仓库的 Release 短页、重新打包的组件版本号要等于这一版、changelog.json 的顺序和构建号（构建号和版本号可以不同）、许可证包缺 AGPL-3.0 全文或内容不对、带截图库时缺 LGPL-3.0 / GPL-3.0 全文或内容不对、固定的组件（APK 没固定的被拒、固定了没给或文件不在、内容不同；`components/` 以外一律不许钉，截图库和外壳也不钉；截图库和外壳要一起给、真打包时必须给）、公开来源的核对（管理器包或其中的 exe 和 tag 上的校验表不符、tag 不在、输入法 APK 没有 tag 列着、截图库的版本按发版分支头的 `CMakeLists.txt` 取、旧一版的 DLL 被拒、没有发版分支时按列着它的 tag 找、和那里的校验表不符或 tag / 校验表不在、发版克隆的 `package/` 有改动）、还没打 tag 的组件（取发版分支头、文件和那里的校验表不符或版本号不是这一版就停、打了 tag 以后认 tag、建草稿的那一轮不收）、随包脚本按原字节取出、`modavd-src` 的布局（根目录 LICENSE / NOTICE，各仓库各占一个文件夹，ModAVD 只有 `bridge/`）和可复现（`package/LICENSE` / `config/own-sources-NOTICE.txt` 有没提交的改动时这一项跳过）。
