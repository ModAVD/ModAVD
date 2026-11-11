# 随包脚本

**简体中文** | [English](README.en.md)

<!-- README.md 和 README.en.md 逐节对应：改了其中一份，请在同一个 PR 里把另一份也改了。 -->

Release 主包里自带的脚本，和包里发出的文件逐字节相同。比的是包里的字节，也就是维护者本地仓库工作区里的文件（那边 `core.autocrlf=true` 检出，`.ps1` 是 CRLF），不是那边 Git 里存的内容（LF）。现在只有一个 `modavd-bridge.ps1`，放在包的 `tools\` 里。

| 文件 | 做什么 | 从哪一版起在包里 |
| --- | --- | --- |
| `modavd-bridge.ps1` | 给一台实例装上、卸掉或核对 arm64 执行桥，用法见文件开头的说明；执行桥的源码在 [bridge/](../bridge/) | 0.36.0 |

补全组件、收集问题信息、复制配置信息在管理器 `ModAVD.exe` 和命令行 `tools\modavd.exe` 里，源码在 [ModAVDManager](https://github.com/ModAVD/ModAVDManager)，要改就到那边提：

| 要做的事 | 管理器里 | 命令行 |
| --- | --- | --- |
| 补全组件：先选下载源、显示 Google《Android 软件开发套件许可协议》，同意后下载 adb 和系统镜像，校验后装进 `sdk\`；能断点续传 | 缺组件时上方提示里的「补全组件」，或「电脑检查」的「组件」一项下面的「补全组件」 | `tools\modavd.exe components fetch` |
| 收集问题信息：在桌面生成问题包。哪些能公开贴见文档站的[反馈问题](https://modavd.github.io/guide/feedback) | 「设置」→「组件与问题反馈」→「收集问题信息」 | `tools\modavd.exe collect-info` |
| 复制配置信息：把电脑配置和各实例装的游戏包整理成一段文字，复制到剪贴板，在 [Issues](https://github.com/ModAVD/ModAVD/issues) 新开 issue 时附上 | 「设置」→「组件与问题反馈」→「复制配置信息」 | `tools\modavd.exe report-config` |

0.38.1 及更早的包里，这三件事由包根的 `补全组件.bat`、`收集问题信息.bat`、`复制配置信息.bat` 和 `tools\` 下的 `fetch-components.ps1`、`collect-info.ps1`、`report-config.ps1` 做，它们以前也放在这个目录里，0.38.2 起不再随包发出（ModAVD/ModAVD#45）。用 `modavd update` 升级时它们会被删掉；手动覆盖升级的会留着，不再用到，可以自己删。

## 写法上的规矩

- `.ps1` 要在 Windows 自带的 PowerShell 5.1 上也能跑，存成 UTF-8 带 BOM。`.gitattributes` 让这个目录原样保存，不做换行转换，提 PR 时也请保持文件原来的换行和 BOM。
- 不硬编码本机路径；adb 只走 ModAVD 根目录 `mas-avd.json` 里的私有端口，不碰 5037。
- 重启、关机前先 `adb shell sync`，再结束模拟器。

## 改这些文件

先开 issue 说一声，再提 PR。合进来以后随下一版 Release 发出，发版说明里会写。许可证是 GNU Affero General Public License 第 3 版或更新版本（AGPL-3.0-or-later），全文见本目录的 [LICENSE](LICENSE)。0.36.0 起随包发出的是这里的 AGPL 版本；0.35.0 及更早的包里的脚本按 Apache License 2.0 发布。
