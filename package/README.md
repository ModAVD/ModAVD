# 随包脚本

Release 主包里自带的脚本，和包里发出的文件逐字节相同。比的是包里的字节，也就是维护者本地仓库工作区里的文件（那边 `core.autocrlf=true` 检出，`.bat` 和多数 `.ps1` 是 CRLF），不是那边 Git 里存的内容（LF）。三个 `.bat` 放在包的根目录，四个 `.ps1` 放在包的 `tools\` 里。

| 文件 | 做什么 | 从哪一版起在包里 |
| --- | --- | --- |
| `补全组件.bat` → `fetch-components.ps1` | 先显示 Google《Android 软件开发套件许可协议》，同意后从官方渠道下载 adb 和系统镜像，校验 SHA-1 后装进 `sdk\`；能断点续传，镜像按顺序回落 | 37.2.10-mas30 |
| `收集问题信息.bat` → `collect-info.ps1` | 在桌面生成问题包：电脑和模拟器的情况、模拟器日志、实例配置；有实例在运行时还有现场截图，模拟器近 7 天崩溃过的话还有崩溃转储。哪些能公开贴见 [docs/feedback.md](../docs/feedback.md) | 37.2.10-mas30 |
| `复制配置信息.bat` → `report-config.ps1` | 把电脑配置和各实例装的游戏包整理成一段文字，复制到剪贴板，贴到「兼容性报告」帖；不带用户名、电脑名、完整路径和游戏账号 | 0.35.0 |
| `modavd-bridge.ps1` | 给一台实例装上、卸掉或核对 arm64 执行桥，用法见文件开头的说明 | 0.36.0 |

## 写法上的规矩

- 要在 Windows 自带的 PowerShell 5.1 上也能跑：`.ps1` 存成 UTF-8 带 BOM；`.bat` 里只用 ASCII，换行用 CRLF。`.gitattributes` 让这个目录原样保存，不做换行转换，提 PR 时也请保持文件原来的换行和 BOM。
- 不硬编码本机路径；adb 只走 ModAVD 根目录 `mas-avd.json` 里的私有端口，不碰 5037。
- 重启、关机前先 `adb shell sync`，再结束模拟器。

## 改这些脚本

先开 issue 说一声，再提 PR。合进来以后随下一版 Release 发出，发版说明里会写。许可证是 GNU Affero General Public License 第 3 版或更新版本（AGPL-3.0-or-later），全文见本目录的 [LICENSE](LICENSE)。0.36.0 起随包发出的是这里的 AGPL 版本；0.35.0 及更早的包里的脚本按 Apache License 2.0 发布。
