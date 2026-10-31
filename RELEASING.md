# ModAVD 发版检查清单

维护者的自动构建和包装入口位于维护仓库的 `tools/build/build-sdk.ps1`、`tools/pubrel/release.ps1`。
公开仓库保留可复现的源码清单、补丁、Release 说明及校验代码；外部构建方法见文档站的[从源码构建](https://modavd.github.io/develop/build)。

## 版本号

从 0.32.1 起，版本号是 `0.<N>.<P>`：0 表示内测阶段，N 是构建号（`source.properties` 的 `Pkg.BuildId=mas-N`），P 是不重编、只重新打包的次数。
tag 是 `v0.<N>.<P>`，标题是 `ModAVD mas-<N>（0.<N>.<P>）`；Release 说明第一句写明基于的 Google AOSP Android Emulator 版本。
mas30、mas32 是旧写法 `v37.2.10-masN`，保持不变，算作 `0.N.0`；`0.32.1` 是 mas32 原样重新打包成新格式。

## 发版前

- [ ] 确定构建号、已提交的待编译源码、对应 manifest、补丁基准及第三方组件来源。更新许可证声明。
- [ ] 先预览构建命令，再另行安排实际构建：`pwsh -NoProfile -File tools/build/build-sdk.ps1 -Build mas34 -WhatIf`。实际构建去掉 `-WhatIf`，并行度最多 16；目标 SDK 已存在时停止。
- [ ] 确认 SDK 中 `source.properties` 的 `Pkg.BuildId=mas-34`，并保留构建入口生成的 `build-info.json`。
- [ ] **在本机跑回归**：启动/关机、WHPX、GLES/GuestAngle、MAA/MaaFW 共享内存截图与点击、已有游戏及相关修复；记录结果。计划内关闭实例先执行 `adb -P <独立端口> -s emulator-<实例端口> shell sync`，再 `emu kill`。
- [ ] 回归跑完后填写维护仓库的 `config/changes.md`（怎么更新、AUTO-MAS 版本、已知问题、发布前回归四节）和 `config/changelog.json`（最前面加本版一条：最低 AUTO-MAS 版本和按分类的一句话改动）。
- [ ] Release 说明的「已知问题」**每条对应一个本仓库 issue，并在说明中链接过去**；新增问题先建 issue。mas30 已对应 [#1](https://github.com/ModAVD/ModAVD/issues/1)、[#2](https://github.com/ModAVD/ModAVD/issues/2)、[#3](https://github.com/ModAVD/ModAVD/issues/3)、[#4](https://github.com/ModAVD/ModAVD/issues/4)。

## 生成和核对

默认只在本地执行到隐私扫描，源码根目录和 SDK 路径通过参数指定：

```powershell
pwsh -NoProfile -File tools/pubrel/release.ps1 -Build mas34 -Sdk <SDK根目录>
```

1. **组包**：依据已审核的文件政策复制 SDK、桌面 APK、组件下载/问题收集脚本、说明与许可证，最后写 `modavd-manifest.json`（版本、构建号、上游版本、平台、最低 AUTO-MAS 版本，以及其余每个文件的大小和 SHA-256）。SDK 文件集改变时先复核政策；不在原 SDK 上删除文件。
2. **主包 ZIP**：`ModAVD-0.34.0-win-x64.zip`，**没有外层文件夹**，包根就是 ModAVD 根目录（Mirror 酱按路径比文件，外层文件夹带版本号会让增量等于全量）。文件名带平台，以后可能有 `linux-x64`。条目使用 `/`，7-Zip `-mcu=on`，中文文件名带 UTF-8 标志。完成后不再替换 ZIP 内的单个文件；任何改动都重新走完整流程。
3. **验证主包**：逐文件 SHA-256 与组包目录一致，排除文件不在包内，条目直接从包根开始（带外层文件夹就报错），路径和编码全部通过，清单与包里的文件一一对应。
4. **补丁**：从构建快照记录的提交导出，提交说明无联合作者尾注。外部贡献者的补丁保留本人署名（GitHub noreply 地址），其余作者为 `qiyinxi <qiyinxi@foxmail.com>`；`scripts/checks.py` 只认这两种。在独立临时仓库用 `git am --keep-cr` 回放，每个提交的源码树和作者与原提交相同。已合并的 `incoming/<主题>/*.patch` 必须都在新系列里（patch-id 相同；收录时 rebase 过的，按作者和标题对上），核对后从仓库删掉、进发版提交；留到以后再发的主题用 `-KeepIncoming <主题>` 指明。
5. **源码总包与清单**：没变的源码文件逐个验哈希后复用，只给变化的源项目重新归档。用 `7z a -tzip -mx0 -mcu=on` 生成和平台无关的 `ModAVD-0.34.0-sources.zip`，外层文件夹同名；包括对应源码、`SOURCES.txt`、两份 manifest 和包内的 `SHA256SUMS.txt`。清单同时同步到本仓库 `manifests/`，不再分别上传附件。
6. **两层校验表**：包内那份只列逐个源码文件，排除自身；外层 `SHA256SUMS.txt` **只列主包和源码总包这两行**。
7. **Release 说明**：第一行是机读注释（版本、构建号、上游版本、最低 AUTO-MAS 版本、最近 10 版的改动摘要）；接着是更新公告（更新内容、怎么更新、AUTO-MAS 版本、已知问题）；然后一行 `<!-- modavd:github-only -->`；以下是回归结果、两个 ZIP 的字节数和 SHA-256、源码包的文件数和总大小。逐个源码文件的 SHA-256 不进说明，见源码包内和本仓库 `release/` 的 `<版本>.sources.SHA256SUMS.txt`。分隔行以上会同步到 Mirror 酱、在 AUTO-MAS 里显示，限 4000 字，不放哈希。
8. **隐私**：主包、补丁、全部源码成员及发布元数据均须通过；同一规则也在仓库 CI 中运行。新命中要核对来源，白名单固定具体文件/内容，不按姓氏全局放行。报告必须是 0 失败。

- [ ] 将本次补丁系列和 `BASE.md`、`series.json`、manifest，以及生成的 Release 说明纳入待审核的发版提交；核对 `series.json` 中的基准、数量和树。
- [ ] 在 `release/` 一起保存 `<版本>.md`、`<版本>.SHA256SUMS.txt`（外层）、`<版本>.sources.SHA256SUMS.txt`（包内）和 `<版本>.assets.json`（字节数与摘要），`<版本>` 不带 `v`，例如 `0.34.0.md`。后者来自包装输出的 `release-assets.json`，不要从旧版手工抄数字。
- [ ] 在维护仓库修改共享的 `checks.py`、`privacy.py`、白名单时，同步公开仓库 `scripts/` 的同名文件。`privacy-private-rules.json`（带真名、私人邮箱、主机名、本机目录的规则）只留在维护仓库，绝不复制过来。
- [ ] CI 补丁回放、一致性与隐私检查全部通过。需要本地复验时，使用下一节的同一组命令。

## 草稿与发布

核对完成后，显式添加 `-DraftRelease` 重新生成并创建草稿。已有本地输出时可以使用 `-Force`，它只重建有匹配所有权标记的本次输出目录；失败时保留现场。

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
```

补丁检查会从 `android.googlesource.com` 按 `BASE.md` / `series.json` 浅取公开基准，不依赖维护者的对象库。
CI 使用只读仓库权限、固定提交的 checkout action，无需配置 secret。
