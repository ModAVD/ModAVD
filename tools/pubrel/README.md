# ModAVD 发版框架

入口以 PowerShell 7 运行，辅助代码只用 Python 3.12 标准库；需要 Git、7-Zip，创建草稿时另需已登录的 `gh`。
发版检查清单见仓库根目录的 [RELEASING.md](../../RELEASING.md)，这里是工具本身的说明。

和本机有关的路径（源码树、临时目录、外部输入）一律是参数；没给的从本机配置取，见 [tools/README.md](../README.md)
和 [tools/local.example.psd1](../local.example.psd1)。两处都没有就报错，不猜默认值。

## 版本号

公开版本号是 `0.<N>.<P>`：0 表示内测阶段，N 是构建号（`sdk-masN`、`Pkg.BuildId=mas-N`），P 是不重编、只重新打包的次数，默认 0。
mas33 发布成 tag `v0.33.0`，标题 `ModAVD mas-33（0.33.0）`；上游版本（`Pkg.Revision`，如 37.2.10）只写进公告第一句「基于 Google AOSP Android Emulator 37.2.10 构建」和清单。
已发布的 mas30、mas32 保留旧名 `v37.2.10-masN`，算作 `0.N.0`，所以这两个构建只能带 `-Patch 1` 以上重新打包，例如 mas32 → `0.32.1`。

## 构建

```powershell
pwsh -NoProfile -File tools/build/build-sdk.ps1 -Build mas34 -WhatIf
```

去掉 `-WhatIf` 才会实际编译。`-EmuSrc`（本机配置 `EmuSrc`）是 AOSP 模拟器源码树，`-Jobs` 默认 12、范围 1–16；
`-Python` 默认取 `MODAVD_PYTHON`、本机配置 `Python`，再是 PATH 里的 `python`；`-LogRoot` 默认 `<TempRoot>\build-sdk`；
`-MinFreeGB` 默认 120（源码树真正所在的盘，目录联接会跟过去）。
目标 `<EmuSrc>\sdk-mas34` 已存在时拒绝执行；不用 clean / removeleftover，不执行删除命令。源码必须提交，编译前后提交快照一致才安装产物。
产物复制到 `sdk-mas34/emulator`，`Pkg.BuildId` 写成 `mas-34`，旁边保存 `build-info.json` 供发版入口确定对应源码。

arm64 执行桥的三个载荷文件（`emulator/resources/modavd/`）不是 AOSP 构建产物，由 `tools/gate/stage-payload.ps1` 单独放进 SDK 树：
`build-sdk.ps1` 在 `ShouldProcess` 之前先跑一次只读的 `-CheckOnly`（`-WhatIf` 也验），编完 robocopy、写完 `source.properties` 之后再真正放进去。
`-GatePayload <已冻结目录>` 指定载荷来源，不给就按 `stage-payload.ps1` 的顺序找。
载荷哈希固定在 `tools/gate/payload.SHA256SUMS.txt`，重编过载荷要先 `stage-payload.ps1 -UpdateSums` 重新固定，否则打包直接停。机制见 [tools/gate/README.md](../gate/README.md)。

## 外部输入

这些不在仓库里，发版前要备好（括号里是本机配置的键名）：

| 输入 | 是什么、从哪来 |
| --- | --- |
| SDK（`-Sdk`） | `build-sdk.ps1` 编出的 `sdk-masN`，带 `build-info.json` |
| 源码树（`EmuSrc`） | 编这个 SDK 的 AOSP 模拟器源码树。补丁从 `build-info.json` 记的提交导出，变了的源项目从这里 `git archive` |
| 源码基准（`Baseline`） | 没变的源码归档直接复用，逐个核对 `config/mas30.SHA256SUMS.txt` 里冻结的哈希。可以用 mas30 Release 的源码包做：`ModAVD-37.2.10-mas30-sources.zip` 里那个文件夹的内容放进 `<Baseline>\source\`，仓库里的 `release/37.2.10-mas30.sources.SHA256SUMS.txt` 复制成 `<Baseline>\SHA256SUMS-sources.txt`（这个做法按代码推出，还没照着从头做过一遍） |
| 说明和许可证（`LicensePackage`） | 已审核的 `说明.txt` 和 `LICENSES\`（`NOTICE.txt`、`NOTICE.csv`、各许可证全文）。预检要求 `LICENSES\AGPL-3.0.txt` 在，且换行统一成 LF 后的 SHA-256 是 `release_tools.py` 的 `AGPL_SHA256`（就是 `package/LICENSE`），否则停。目前只在维护者本机 |
| 桌面 APK（`LauncherApk`） | `components\` 下的桌面，文件名和 SHA-256 固定在 `config/components.SHA256SUMS.txt` |
| 管理器（`-Manager`） | ModAVDManager 的 Release 附件 `ModAVDManager-<版本>-win-x64.zip` |
| 宿主输入法（`-ImeApk`） | ModAVDHostIME 的 Release 附件 `modavd-ime.apk`；不给就不带 |
| 两个公开仓库的克隆（`ManagerRepo`、`ImeRepo`） | 只读 tag，不管当前检出哪个分支：先 `git fetch upstream --tags` |
| 私有隐私规则（`PrivateRules`） | 维护者自己的 `privacy-private-rules.json`，放在仓库外，绝不提交（见下面「隐私规则」） |

## 发版

回归跑完后填两个文件（发版时读取，不进构建快照）：

- `config/changes.md`：`## 怎么更新`、`## AUTO-MAS 版本`、`## 已知问题`、`## 发布前回归` 四节，一节都不能少，不能留「发布前填写」占位。
- `config/changelog.json`：在最前面加本版一条，`{"version": "v0.34.0", "build": 34, "min_automas": "5.7.0", "changes": {"修复": ["…"]}}`。分类只用 `新增`、`变更`、`修复`、`移除`、`说明`；条目一行一句，不能有 `--`。公告的「更新内容」就是本版这条按分类排出来的；第一行机读注释带最近 10 版，给 AUTO-MAS 跳版本时合并显示。`v0.32.0` 一条是 mas32 的补记。

这两个文件写完（或写的过程中）先看公告长度，不用 SDK、不打包：

```powershell
python -I tools/pubrel/release_tools.py notes-budget --build mas34 [--sdk <SDK 根目录>]
```

报出机读行（限 8000 字，带最近 10 版的改动，旧版的条目也算在里面）和分隔行以上的公告（限 4000 字）各多长；超了就报错并说该删哪里，过了 90% 会提醒。没给 `--sdk` 时上游版本按 `99.99.99` 估，比真的多一两个字。`release.ps1` 的预检用同一个算法再查一遍，超了在打包之前就停。

再检查本次 NOTICE、组件来源和许可证；在测试机上跑发版前回归（[ModAVDE2E](https://github.com/ModAVD/ModAVDE2E)），结果表贴进 `config/changes.md` 的「发布前回归」。

```powershell
pwsh -NoProfile -File tools/pubrel/release.ps1 -Build mas34 -Sdk <SDK 根目录> -Manager <下载的 ModAVDManager-0.3.0-win-x64.zip> -ImeApk <下载的 modavd-ime.apk>
```

默认只生成本地产物和隐私报告，输出 `<TempRoot>\pubrel\mas34`。

管理器、输入法、执行桥源码和随包脚本都取公开仓库：管理器和输入法用各自已发布的 Release 附件，
随包脚本和执行桥源码取发版克隆（`-Repository`，默认就是这份工具所在的克隆）当前提交的 `package/`、`bridge/`。
发版前先把发版克隆更新到 ModAVD 的 main，`ManagerRepo`、`ImeRepo` 两个克隆 `git fetch upstream --tags`。

| 参数 | 用途 |
| --- | --- |
| `-Build` / `-Sdk` | 必填，构建号 `masNN`（mas32 及更早的只能重新打包）、SDK 根目录；检查 `Pkg.BuildId`，`Pkg.Revision` 记为上游版本 |
| `-Patch` | 默认 0；只重新打包时加一，版本变成 `0.<N>.<P>`，输出目录 `masNN-pP` |
| `-Changes` / `-Changelog` | 默认 `config/changes.md`、`config/changelog.json`；试运行时指向临时副本 |
| `-Snapshot` | 默认读 SDK 的 `build-info.json` |
| `-EmuSrc` | 源码树（`EmuSrc`） |
| `-Baseline` | 源码基准（`Baseline`），见「外部输入」 |
| `-Package` / `-Apk` | 已审核的说明/许可证目录（`LicensePackage`）、桌面 APK（`LauncherApk`） |
| `-ImeApk` | 宿主输入法 APK，放成 `components\modavd-ime.apk`；默认空，即不带。包里每个 APK 都要在 `config/components.SHA256SUMS.txt` 固定 SHA-256：没固定的被拒，固定过的漏带或内容不同也被拒（漏带会让增量更新删掉用户机器上的文件）。预检还查 NOTICE.txt 里有它的条目。固定文件每行 `<小写 sha256>  components/<文件名>`（两个空格，不分大小写也不能重名；`Get-FileHash` 的输出要先转小写）。固定是全局的：IME 一旦写进去，重新打包 mas35 以前的构建要临时改回。另外要在 `-ImeRepo` 某个 tag 的 `release/<版本>.SHA256SUMS.txt` 里找得到这个 APK（列成 `modavd-ime.apk`），源码包放的就是那个 tag |
| `-Manager` | 必填，ModAVDManager 的 Release 附件 `ModAVDManager-<版本>-win-x64.zip`。包和里面的 `ModAVD.exe`、`tools/modavd.exe` 都要和 `-ManagerRepo` 里 tag `v<版本>` 的 `release/<版本>.SHA256SUMS.txt` 一致，包里的两个 exe 就取自它；NOTICE 里的 Go 版本取那个 tag 的 go.mod |
| `-ManagerRepo` / `-ImeRepo` | 两个公开仓库的克隆（`ManagerRepo`、`ImeRepo`；不带输入法时 `-ImeRepo` 可以不给）；只读 tag，tag 没取下来就停 |
| `-Repository` | ModAVD 的发版克隆，默认是这份工具所在的克隆。随包脚本（`package/`）和执行桥源码（`bridge/`）取它当前提交，这两处有没提交的改动就停；清单、补丁、Release 说明同步进它 |
| `-OutputRoot` / `-OutputName` | 默认 `<TempRoot>\pubrel`；名称只允许默认名或 `<默认名>-check`。输出根目录和发版克隆都不能在源码树、SDK、本机配置 `ProtectedRoots` 列的目录里，输出根目录也不能在发版克隆里 |
| `-PrivateRules` | 仓库外的私有隐私规则（`PrivateRules`，或环境变量 `MODAVD_PRIVATE_RULES`）；开跑前就查，没有就停 |
| `-MinFreeGB` | 写之前要求的磁盘空闲，默认 120（`MinFreeGB`） |
| `-Force` | 只重建同名、有匹配所有权标记、没有链接的本脚本输出目录；不会清理别的目录 |
| `-Python` / `-SevenZip` | 指定可执行文件；Python 也可由 `MODAVD_PYTHON` 指定，再是本机配置 `Python`、PATH 里的 `python.exe`；7-Zip 再是本机配置 `SevenZip`、PATH 里的 `7z.exe`、`Program Files\7-Zip` |
| `-DraftRelease` | 全部校验通过后创建 GitHub 草稿；上传主包、源码总包、外层校验表这三个附件 |

产物（以 `0.33.0` 为例）：

- 主包 `ModAVD-0.33.0-win-x64.zip`，**没有外层文件夹**，包根就是 ModAVD 根目录：Mirror 酱按路径比文件，带版本号的外层文件夹会让增量等于全量（不再另出 `-update.zip`）。用户用「全部解压缩」或 7-Zip「解压到 <包名>\」会得到同名文件夹。文件名带平台，以后可能有 `linux-x64`；平台写在 `release_tools.py` 的 `PLATFORM` 和 `release.ps1` 里。
- 主包根目录有 `modavd-manifest.json`：版本、构建号、上游版本、平台、最低 AUTO-MAS 版本，和包里其余每个文件的大小、SHA-256。`verify-zip` 核对主包与组包目录逐文件一致（带外层文件夹就报错）、清单与文件一一对应。
- 源码总包 `ModAVD-0.33.0-sources.zip`（和平台无关，不带平台），用 7-Zip `-tzip -mx0 -mcu=on`，外层文件夹同名。内部源码归档保持原文件名和字节；另附内部 `SHA256SUMS.txt`。
- 外层 `SHA256SUMS.txt` 只列主包和源码总包两个 ZIP。
- `release-notes.md`：第一行机读注释，然后是更新公告（更新内容、怎么更新、AUTO-MAS 版本、已知问题），再是分隔行 `<!-- modavd:github-only -->`，以下是回归、下载表、源码说明。分隔行以上同步到 Mirror 酱、在 AUTO-MAS 里显示，限 4000 字，不能有哈希。
- `release-assets.json` 记录供仓库 CI 使用的大小与哈希。
- `patches/`：要进发版提交的补丁。`series.json`、`BASE.md` 是发版克隆（`-Repository`）里那两份加上这一版，直接覆盖仓库里的：
  - 和已发布的某个系列仓库、基准、分支头都相同的，沿用那个目录（比如 Crashpad 一直是 `crashpad/module-version22`），不加新目录；它的导出放在 `work/patches-reused/` 给回放和 incoming 核对用，不在 `patches/` 里。
  - 其余的是新目录（`patches/<仓库>/masN/`）：`series.json` 最前面加一条；`BASE.md` 版本表加一行（重新打包同一构建时只在那一行补「；0.N.P 相同」），目录表在同一仓库的旧行上面加一行，目录表下面的说明加一句（和上一个目录前几个补丁相同、从第几个起是新增的，按 patch-id 算）。
  - 新补丁的许可证、新增的补丁做了什么只能人写，留成「发布前填写」：填完再提交，`checks.py metadata` 见到它就失败。版本表下面的许可证表、回放示例也按需要手改。
  - 发版克隆里已经是加过这一版的（重跑一遍），不会重复加。`logs/patch-dirs.json` 记每个仓库这一版用哪个目录、是不是新的。

`config/sdk-inventory.csv` 是审核过的包装政策：目前 SDK 文件集固定，移除 103 个文件。新 SDK 增减文件会停止，需先审核并更新政策。
其中 `resources/modavd/` 三行是我们自研的 arm64 执行桥载荷（AGPL-3.0-or-later），由 `tools/gate/stage-payload.ps1` 放进 SDK 树；
它们在 `sdk/emulator` 下面，所以内测「只发 `sdk\emulator`」那条规矩不用改。包里的 `tools/modavd-bridge.ps1`（装 / 卸 / 核对执行桥）
按提交从发版克隆的 `package/` 取原字节（那边 `package/** -text`，存的就是包里的字节）。0.38.1 及更早的包里还有三个 .bat 和
`fetch-components.ps1`、`collect-info.ps1`、`report-config.ps1`，0.38.2 起这些事由管理器和命令行做，包里不再有（ModAVD/ModAVD#45）；
用 `modavd update` 升级时，旧清单里有、新包里没有的包根文件会被删掉。

0.36.0 起包里还有 `ModAVD.exe`（管理器）和 `tools/modavd.exe`（命令行、MCP），取自 `-Manager` 那个 Release 包（ModAVDManager 自己的发版流程可复现编译并核对过）。

P3 审核过的 NOTICE 和 `说明.txt` 之后新发的、不再发的东西，由 assemble 按锚点改进去：已有条目里拿掉不再发的文件（`NOTICE_GONE_FILES`）、
加上新发的文件（`NOTICE_MORE_FILES`）、Part 2 末尾加 `config/notice-additions.txt` / `.csv` 的条目（执行桥载荷、modavd 两个 exe、Go 运行时的 BSD-3 声明，Go 版本取自管理器 tag 的 go.mod）、
`说明.txt` 里讲批处理的几段换成管理器和命令行的说法（`README_EDITS`），在「出问题怎么办」前加 `config/readme-tools.txt`。锚点或原文找不到、不止一处就停。
补完两个方向都核对：包根以下（`sdk/` 除外）每个文件、`sdk/emulator/resources/modavd/` 每个文件都要在 NOTICE.txt 里有条目，许可证全文除外；
NOTICE.txt 里每个 `(package root)` 文件都要在包里。以后往包里加文件不补条目，或拿掉文件不改条目，打包都会停。

源码总包多一个 `modavd-src.tar.xz`（0.36.0 时叫 `modavd-src-<提交>.tar.xz`）：各公开仓库的原样 `git archive`，
`ModAVDManager/` 是 `-Manager` 那个 tag，`ModAVDHostIME/` 是 `-ImeApk` 对上的 tag，`ModAVD/bridge/` 是发版克隆当前提交的 `bridge/`。
归档根目录另有本仓库 HEAD 的 `LICENSE`（`package/LICENSE`，AGPL-3.0 全文，核对 `AGPL_SHA256`）和 `NOTICE`（`config/own-sources-NOTICE.txt`：哪些是自研的、例外和第三方部分、各部分的公开位置）（`OWN_SOURCES_ROOT`）；
时间戳、属主沿用 `git archive`，根目录两个文件取三个提交里最新的提交时间，可复现。`SOURCES.txt` 第 6 节写明各部分的 tag 和提交。
`LICENSE`、`NOTICE` 有没提交的改动时预检就停。随包脚本本身就是源码，随主包原样发出，不进这个归档。

自研部分（管理器和命令行、执行桥、宿主输入法、随包脚本）按 AGPL-3.0-or-later 发布，公开位置：
管理器 https://github.com/ModAVD/ModAVDManager，输入法 https://github.com/ModAVD/ModAVDHostIME，随包脚本（装执行桥的 `modavd-bridge.ps1`）https://github.com/ModAVD/ModAVD/tree/main/package，
执行桥 https://github.com/ModAVD/ModAVD/tree/main/bridge。
补丁按所改文件的许可证；ModAVD 仓库根目录的 LICENSE 仍是 Apache-2.0；图标是同人创作，不在任何许可证范围内。
24 个源项目发生变化时重新 `git archive`；其他 manifest 项目（尤其 prebuilts）发生变化会停止，需重新审核对应源码及 NOTICE。
P3 审核过的说明和 NOTICE 里写的是 `37.2.10-mas30`，组包时换成新版本号，`mas-30` / `mas30` 换成本次构建号。
清单模板是仓库里审核过的 `manifests/manifest-mas30.xml`。

## 共享校验与隐私规则

`checks.py`、`privacy.py` 和白名单 `privacy-allowlist.json` 只有一份，就是仓库的 `scripts/`：公开 CI 和发版入口用的是同一份。

- `checks.py metadata --root <公开仓库>` 校验 Release 表格、两层校验表、大小记录与 `.gitattributes`，0.33.0 起的说明另查机读行、分隔行和公告长度；
  `patches --root <公开仓库> --work <全新临时目录>` 直接从 AOSP 浅取基准，回放三条系列并比对最终源码树。
- `privacy.py <公开仓库>` 扫描 Git 跟踪文件；发版入口用同一模块扫描主包、补丁、源码包全部成员和元数据。
- 仓库里只有通用规则（`PUBLIC_RULES`）。点名私人信息的规则（真名、私人邮箱、主机名、本机目录）在维护者自己的
  `privacy-private-rules.json` 里，**放在仓库外，绝不提交**：用环境变量 `MODAVD_PRIVATE_RULES`、`privacy.py --private-rules <文件>`
  或 `privacy.use_rules(<文件>)` 加载，指了却不存在就报错。公开 CI 不带它跑；发版的第 8 步没加载到就停。

白名单只允许已核查的第三方作者/词表/测试数据/示例路径、上游预编译件的构建路径及二进制巧合。
每条例外固定「规则 + 完整可打印字符串 SHA-256」，并固定具体源文件路径或整个文件 SHA-256；不按姓氏全局豁免。记录的理由不复制私人字符串进仓库。
新命中要核对来源后人工更新白名单；禁止直接把新扫描报告全部加入白名单。

## 失败路径测试

```powershell
python -I tools/pubrel/test_release.py --work <全新临时目录>
```

不需要维护者的私有规则：涉及私有规则的几项用测试里自造的规则。
覆盖输出所有权、路径越界、incoming 在 `core.autocrlf=true` 的检出里也删得掉、补丁记录在已发布的 `series.json` / `BASE.md` 上追加（沿用目录、重跑不重复、重新打包）、公告长度在打包前就算出来、发版克隆不许落在受保护目录、ASCII / UTF-16 / UTF-8 / tar 隐私拦截、私有规则从仓库外加载（指了不存在就报错、没加载到发版扫描就停）、白名单边界、表格哈希/大小/重复/缺项、ZIP 原始反斜杠/内容错误、主包带了外层文件夹、清单缺漏或内容不符、版本号、公告的机读行/分隔行/长度/哈希、changes.md 的节和占位、changelog.json 的顺序和构建号、许可证包缺 AGPL-3.0 全文或内容不对、公开来源的核对（管理器包或其中的 exe 和 tag 上的校验表不符、tag 不在、输入法 APK 没有 tag 列着、发版克隆的 `package/` 有改动）、随包脚本按原字节取出、`modavd-src` 的布局（根目录 LICENSE / NOTICE，各仓库各占一个文件夹，ModAVD 只有 `bridge/`）和可复现（`package/LICENSE` / `config/own-sources-NOTICE.txt` 有没提交的改动时这一项跳过）。
