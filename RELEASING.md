# ModAVD 发版检查清单

构建和包装入口在本仓库的 [`tools/`](tools/README.md)：`tools/build/build-sdk.ps1`、`tools/pubrel/release.ps1`（说明见 [tools/pubrel/README.md](tools/pubrel/README.md)）；发版前回归在 [ModAVDE2E](https://github.com/ModAVD/ModAVDE2E)。
本机路径（源码树、临时目录、外部输入）用参数或本机配置给，见 [tools/README.md](tools/README.md)「本机配置」；外部构建方法见文档站的[从源码构建](https://modavd.github.io/develop/build)。

## 版本号

版本号 `0.<N>.<P>` 单独往上加（`release.ps1 -Version`）：0 表示内测阶段，比如只重新打包 0.41.0 就发 0.41.1。
模拟器构建号照旧是 mas-N（`source.properties` 的 `Pkg.BuildId=mas-N`），和版本号无关，记在 `modavd-manifest.json` 和公告机读行的 `build` 里，比如 0.41.0 是 mas-40。
0.40.0 带的管理器 0.8.0 装包时还要求版本中间那位等于构建号，所以 0.40.0 之后先发 0.40.1（mas-40，带不再要这个对应的管理器 0.9.0）；装了 0.40.1 以后的根目录才能用「更新」装上版本号和构建号对不上的包。
tag 是 `v0.<N>.<P>`，标题是 `ModAVD 0.<N>.<P>`；Release 说明第一句写明构建号和基于的 Google AOSP Android Emulator 版本。
0.32.1 到 0.40 是旧规矩：N 就是构建号，P 是不重编、只重新打包的次数，标题 `ModAVD mas-<N>（0.<N>.<P>）`。
mas30、mas32 是旧写法 `v37.2.10-masN`，保持不变，算作 `0.N.0`；`0.32.1` 是 mas32 原样重新打包成新格式。

## 发版分支

下面的 `release/0.<N>` 按版本号取（0.41.x 在 `release/0.41`），不按构建号。

发版在单独的分支上做，和 main 上的开发分开（[#175](https://github.com/ModAVD/ModAVD/issues/175)）：

- 冻结时，本仓库和 ModAVDManager、ModAVDExtras 各从 main 切出 `release/0.<N>`（和三个源码仓库的叫法一样）。ModAVDHostIME 很少变，有改动要发时才切。main 照常合 PR。
- 要进这一版的修复先合进 main，再开 PR 挑到 `release/0.<N>`。
- 组件的发版准备提交（版本号、`release/<版本>.md`、`release/<版本>.SHA256SUMS.txt`）落在组件自己的 `release/0.<N>` 上；组件各用各的版本号和 tag。
- 本仓库的钉版本（`EXTRAS_VERSION`、`tools/pubrel/config/components.SHA256SUMS.txt`）、补丁系列、清单、`release/<版本>.*` 落在本仓库的 `release/0.<N>` 上，tag 打在这条分支的头上。
- 打包和回归不用等组件发版：`release.ps1` 取组件发版分支头上的提交。回归通过的那个输出就是发出去的包。
- 发布按顺序做，前一步没过就停：ModAVDExtras → ModAVDHostIME（有变才发）→ ModAVDManager → 三个源码仓库 → 本仓库。每个组件先把 tag 打在包里记的那个提交上，等它的 `release` workflow 出草稿、附件和校验表对上，再发布。没变的组件沿用旧 tag。
- 补丁版 `0.<N>.<P>` 接着在 `release/0.<N>` 上做。
- 发完以后，每个仓库开 PR 把 `release/0.<N>` 并回 main。

## 发版前

- [ ] 确定构建号、已提交的待编译源码、对应 manifest、补丁基准及第三方组件来源。更新许可证声明。
- [ ] 先预览构建命令，再另行安排实际构建：`pwsh -NoProfile -File tools/build/build-sdk.ps1 -Build mas34 -WhatIf`。实际构建去掉 `-WhatIf`，并行度最多 16；目标 SDK 已存在时停止。
- [ ] 确认 SDK 中 `source.properties` 的 `Pkg.BuildId=mas-34`，并保留构建入口生成的 `build-info.json`。
- [ ] **在测试机上跑回归**：启动/关机、WHPX、GLES/GuestAngle、MAA/MaaFW 共享内存截图与点击、已有游戏及相关修复；记录结果。运行器和套件在 [ModAVDE2E](https://github.com/ModAVD/ModAVDE2E)，改了什么跑什么（`-Scope`），先 `-DryRun` 看会跑哪些项；结果表 `results.md` 直接贴进 `changes.md` 的「发布前回归」。计划内关闭实例先执行 `adb -P <独立端口> -s emulator-<实例端口> shell sync`，再 `emu kill`。
- [ ] 回归跑完后填写 `tools/pubrel/config/changes.md`（怎么更新、AUTO-MAS 版本、已知问题、发布前回归四节）和 `tools/pubrel/config/changelog.json`（最前面加本版一条：最低 AUTO-MAS 版本和按分类的一句话改动）。
- [ ] 写的时候就看长度：`python -I tools/pubrel/release_tools.py notes-budget --build mas34 [--sdk <SDK根目录>]` 报出机读行（限 8000 字，带 `changelog.json` 最前面最多 10 版的改动）和分隔行以上的公告（限 4000 字）各多长，超了就报错，过了 90% 会提醒。`release.ps1` 的预检也查这一项，不用等到第 7 步。
- [ ] Release 说明的「已知问题」**每条对应一个本仓库 issue，并在说明中链接过去**；新增问题先建 issue。mas30 已对应 [#1](https://github.com/ModAVD/ModAVD/issues/1)、[#2](https://github.com/ModAVD/ModAVD/issues/2)、[#3](https://github.com/ModAVD/ModAVD/issues/3)、[#4](https://github.com/ModAVD/ModAVD/issues/4)。

## 生成和核对

默认只在本地执行到隐私扫描。SDK 和管理器包通过参数指定，源码树、源码基准、说明和许可证目录等外部输入用参数或本机配置给（见 [tools/pubrel/README.md](tools/pubrel/README.md)「外部输入」）：

```powershell
pwsh -NoProfile -File tools/pubrel/release.ps1 -Build mas34 -Sdk <SDK根目录> -Manager <ModAVDManager-版本-win-x64.zip> -ImeApk <modavd-ime.apk> -ExtrasDll <modavd_extras.dll> -MumuShim <external_renderer_ipc.dll>
```

`-ImeApk`、`-ExtrasDll`、`-MumuShim` 在 `release.ps1` 的参数里是可选的，但宿主输入法 APK、截图库和 MuMu 兼容外壳的哈希都钉在 `tools/pubrel/config/components.SHA256SUMS.txt` 里，钉了没给就停，所以打包这几项都必须给（截图库 0.39.0 起，外壳从钉上它的那一版起）。

0.39.0 起包里有 ModAVDExtras 的截图库 `extras\modavd_extras.dll`（LGPL-3.0-or-later）：`-ExtrasDll` 给它 Release 里的附件，哈希和版本固定在 `tools/pubrel/config/components.SHA256SUMS.txt` 和 `release_tools.py` 的 `EXTRAS_VERSION`，对不上就停。同一版 ModAVDExtras 的 MuMu 兼容外壳 `external_renderer_ipc.dll` 放成 `mumu-shim\shell\sdk\external_renderer_ipc.dll`（`-MumuShim`），给单独装的 MAA 用：在 MAA 里把 MuMu 路径填成 `<ModAVD 根目录>\mumu-shim`；它和截图库要出自 ModAVDExtras 同一个提交的校验表，截图库钉了它没钉就停。管理器、截图库还没打 tag 时，取它们发版分支头上的提交（见上面「发版分支」和 [tools/pubrel/README.md](tools/pubrel/README.md)「组件还没打 tag 时」）。

1. **组包**：依据已审核的文件政策复制 SDK、桌面 APK、随包脚本 `modavd-bridge.ps1`、说明与许可证，最后写 `modavd-manifest.json`（版本、构建号、上游版本、平台、最低 AUTO-MAS 版本，以及其余每个文件的大小和 SHA-256）。SDK 文件集改变时先复核政策；不在原 SDK 上删除文件。
2. **主包 ZIP**：`ModAVD-0.34.0-win-x64.zip`，**没有外层文件夹**，包根就是 ModAVD 根目录（Mirror 酱按路径比文件，外层文件夹带版本号会让增量等于全量）。文件名带平台，以后可能有 `linux-x64`。条目使用 `/`，7-Zip `-mcu=on`，中文文件名带 UTF-8 标志。完成后不再替换 ZIP 内的单个文件；任何改动都重新走完整流程。
3. **验证主包**：逐文件 SHA-256 与组包目录一致，排除文件不在包内，条目直接从包根开始（带外层文件夹就报错），路径和编码全部通过，清单与包里的文件一一对应。
4. **补丁**：从构建快照记录的提交导出，提交说明无联合作者尾注。外部贡献者的补丁保留本人署名（GitHub noreply 地址）；从上游 `git cherry-pick -x` 来的提交（说明里有 `(cherry picked from commit <40 位>)`，作者不是在本地提交它的人）保留上游原作者；其余作者为 `qiyinxi <qiyinxi@foxmail.com>`。`scripts/checks.py` 只认这三种，上游作者只在带 cherry-pick 那一行的提交上算数。在独立临时仓库用 `git am --keep-cr` 回放，每个提交的源码树和作者与原提交相同。已合并的 `incoming/<主题>/*.patch` 必须都在新系列里（patch-id 相同；收录时 rebase 过的，按作者和标题对上），核对后从仓库删掉（按发版克隆自己的换行设置 `git rm`，`core.autocrlf=true` 的检出也行）、进发版提交；留到以后再发的主题用 `-KeepIncoming <主题>` 指明。
   输出的 `patches/series.json`、`patches/BASE.md` 是发版克隆里那两份加上这一版：和已发布的系列基准、分支头都相同的，沿用已发布的目录（导出放到 `work/patches-reused/`，不进 `patches/`）；其余的是新目录，在 `series.json` 最前面加一条，`BASE.md` 的版本表、目录表各加一行、目录表下面加一句说明。新补丁的许可证、做了什么只能人写，留成「发布前填写」，`checks.py metadata` 见到就失败。`logs/patch-dirs.json` 记每个仓库用哪个目录、是不是新的。
5. **源码总包与清单**：没变的源码文件逐个验哈希后复用，只给变化的源项目重新归档。用 `7z a -tzip -mx0 -mcu=on` 生成和平台无关的 `ModAVD-0.34.0-sources.zip`，外层文件夹同名；包括对应源码、`SOURCES.txt`、两份 manifest 和包内的 `SHA256SUMS.txt`。清单同时同步到本仓库 `manifests/`，不再分别上传附件。
6. **两层校验表**：包内那份只列逐个源码文件，排除自身；外层 `SHA256SUMS.txt` **只列主包和源码总包这两行**。
7. **Release 说明**：第一行是机读注释（版本、构建号、上游版本、最低 AUTO-MAS 版本、`changelog.json` 最前面最多 10 版的改动摘要）；接着是更新公告（更新内容、怎么更新、AUTO-MAS 版本、已知问题）；然后一行 `<!-- modavd:github-only -->`；以下是回归结果、两个 ZIP 的字节数和 SHA-256、源码包的文件数和总大小。逐个源码文件的 SHA-256 不进说明，见源码包内和本仓库 `release/` 的 `<版本>.sources.SHA256SUMS.txt`。分隔行以上会同步到 Mirror 酱、在 AUTO-MAS 里显示，限 4000 字，不放哈希。
8. **隐私**：主包、补丁、全部源码成员及发布元数据均须通过，用 `scripts/privacy.py` 的通用规则加上维护者仓库外的私有规则（`-PrivateRules`，没加载到就停）；通用规则也在仓库 CI 中运行。新命中要核对来源，白名单固定具体文件/内容，不按姓氏全局放行。报告必须是 0 失败。上次扫过干净、内容没变的文件按本机缓存跳过（扫描器或规则一变就全部重扫，`-NoPrivacyCache` 强制全扫）；带 `-DraftRelease` 真要上传的那一轮不读缓存、全部重扫，扫完照样写缓存。见 [tools/pubrel/README.md](tools/pubrel/README.md)「共享校验与隐私规则」。

- [ ] 把输出 `patches/` 下的新目录和 `BASE.md`、`series.json`（直接覆盖仓库里的，它们就是在仓库那两份上加的）、manifest，以及生成的 Release 说明纳入待审核的发版提交；填完 `BASE.md` 里的「发布前填写」（新补丁按所改文件归许可证、写清新增了什么）；核对 `series.json` 中的基准、数量和树。
- [ ] 在 `release/` 一起保存 `<版本>.md`、`<版本>.SHA256SUMS.txt`（外层）、`<版本>.sources.SHA256SUMS.txt`（包内）和 `<版本>.assets.json`（字节数与摘要），`<版本>` 不带 `v`，例如 `0.34.0.md`。后者来自包装输出的 `release-assets.json`，不要从旧版手工抄数字。
- [ ] `checks.py`、`privacy.py`、白名单只有 `scripts/` 这一份，CI 和发版工具共用。`privacy-private-rules.json`（带真名、私人邮箱、主机名、本机目录的规则）只放在维护者本机、仓库外面，绝不提交。
- [ ] CI 补丁回放、一致性与隐私检查全部通过。需要本地复验时，使用下一节的同一组命令。

## 草稿与发布

先按「发版分支」的顺序把这一版新发的组件发掉：tag 打在包里记的那个提交上（预检的输出里有，也记在输出目录 `snapshot.json` 的 `own` 里），它们的 Release 发布以后再建本仓库的草稿。

然后显式添加 `-DraftRelease` 重新生成并创建草稿：它只收组件都打了 tag 的包，隐私扫描不读缓存、全部重扫，草稿的 tag 指向 `release/0.<N>`。已有本地输出时可以使用 `-Force`，它只重建有匹配所有权标记的本次输出目录；失败时保留现场。

```powershell
pwsh -NoProfile -File tools/pubrel/release.ps1 -Build mas34 -Sdk <SDK根目录> -Force -DraftRelease
```

- [ ] 草稿附件**恰好三个**：主包 ZIP、源码总包 ZIP、外层 `SHA256SUMS.txt`。
- [ ] **人工过目草稿后再发布**：目标标签/提交、版本号、两层校验表、附件字节数与摘要、源码对应关系、许可证、隐私报告、issue 链接、升级说明和分隔行以上的公告均正确。
- [ ] 经维护者确认后通过 GitHub 页面发布；自动脚本只创建草稿，不发布、不改仓库设置。
- [ ] 发布后要改公告（例如补一条已知问题）就直接编辑 Release；分隔行和第一行机读注释不要删。

## 本地运行与 CI 相同的检查

在本仓库根目录执行，需要 Python 3.12、Git、到 AOSP 的网络连接。`<全新临时目录>` 必须尚不存在。

```powershell
python scripts/checks.py patches --work <全新临时目录>
python scripts/checks.py metadata
python scripts/privacy.py .
python -I tools/pubrel/test_release.py --work <全新临时目录>
```

最后一条是发版工具的失败路径单测，CI 不跑；改了 `tools/pubrel/` 或 `scripts/` 就在本地跑一遍。
维护者另用 `python scripts/privacy.py . --private-rules <仓库外的私有规则>` 加上私有规则扫一遍。

补丁检查会从 `android.googlesource.com` 按 `BASE.md` / `series.json` 浅取公开基准，不依赖维护者的对象库。
CI 使用只读仓库权限、固定提交的 checkout action，无需配置 secret。
