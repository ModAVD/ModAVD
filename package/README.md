# 随包脚本

Release 主包里自带的批处理和脚本，和包里发出的文件逐字节相同。比的是包里的字节，也就是维护者本地仓库工作区里的文件（那边 `core.autocrlf=true` 检出，`.bat` 和多数 `.ps1` 是 CRLF），不是那边 Git 里存的内容（LF）。三个 `.bat` 放在包的根目录，`modavd-bridge.ps1` 和 `components-sources.txt` 放在包的 `tools\` 里。

三个 `.bat` 都调包里的命令行 `tools\modavd.exe`（源码在 [ModAVDManager](https://github.com/ModAVD/ModAVDManager)），自己不做事；补全组件、收集问题信息、复制配置信息的做法改在命令行里改。0.38.0 及更早的包里，这三件事由 `tools\` 下的 `fetch-components.ps1`、`collect-info.ps1`、`report-config.ps1` 做，现在不再随包发出（ModAVD/ModAVD#45）。

| 文件 | 做什么 | 从哪一版起在包里 |
| --- | --- | --- |
| `补全组件.bat` → `modavd components fetch` | 第一次运行先问从哪里下载（菜单是 `components-sources.txt`，直接回车是自动：Google 中国下载、腾讯云镜像、Google 官方依次试），选的记在 `downloads\source.txt`，删掉它就会再问。之后命令行先显示 Google《Android 软件开发套件许可协议》，同意后下载 adb 和系统镜像，校验后装进 `sdk\`；能断点续传 | 37.2.10-mas30 |
| `收集问题信息.bat` → `modavd collect-info` | 在桌面生成问题包：电脑和模拟器的情况、模拟器日志、实例配置；有实例在运行时还有现场截图，模拟器近 7 天崩溃过的话还有崩溃转储。哪些能公开贴见文档站的[反馈问题](https://modavd.github.io/guide/feedback) | 37.2.10-mas30 |
| `复制配置信息.bat` → `modavd report-config` | 把电脑配置和各实例装的游戏包整理成一段文字，复制到剪贴板，在 [Issues](https://github.com/ModAVD/ModAVD/issues) 新开 issue 反馈时附上；不带用户名、电脑名、完整路径和游戏账号 | 0.35.0 |
| `modavd-bridge.ps1` | 给一台实例装上、卸掉或核对 arm64 执行桥，用法见文件开头的说明；执行桥的源码在 [bridge/](../bridge/) | 0.36.0 |

- 三个 `.bat` 都带 `--root "%~dp0."` 调命令行，用的就是批处理所在的这个文件夹，不受 `MODAVD_ROOT` 影响。退出码原样传出来，结尾 `pause`。
- 给 `.bat` 带参数时，参数原样交给命令行，写法照命令行的来，例如 `补全组件.bat --mirror google`、`收集问题信息.bat --no-explorer`、`复制配置信息.bat --no-clipboard`（全部选项见 `tools\modavd.exe help`）。以前 PowerShell 脚本的写法（`-Mirror`、`-AcceptLicense`）不再认。`补全组件.bat` 带了参数就不问下载源，也不读、不写 `downloads\source.txt`。
- `tools\modavd.exe` 不在时，三个 `.bat` 只提示重新解压整个包，什么也不做。

## 写法上的规矩

- `.bat` 里只用 ASCII，换行用 CRLF：要给用户看的中文放在 UTF-8 的 `.txt` 里，显示时临时切到代码页 65001，读输入前切回来。`.ps1` 要在 Windows 自带的 PowerShell 5.1 上也能跑，存成 UTF-8 带 BOM。`.gitattributes` 让这个目录原样保存，不做换行转换，提 PR 时也请保持文件原来的换行和 BOM。
- 在 `.bat` 里调外部程序时，别让它读到批处理的输入：比如 `chcp` 要带 `<nul`，不然输入从文件重定向进来时会被它吃掉。
- 不硬编码本机路径；adb 只走 ModAVD 根目录 `mas-avd.json` 里的私有端口，不碰 5037。
- 重启、关机前先 `adb shell sync`，再结束模拟器。

## 改这些文件

先开 issue 说一声，再提 PR。合进来以后随下一版 Release 发出，发版说明里会写。许可证是 GNU Affero General Public License 第 3 版或更新版本（AGPL-3.0-or-later），全文见本目录的 [LICENSE](LICENSE)。0.36.0 起随包发出的是这里的 AGPL 版本；0.35.0 及更早的包里的脚本按 Apache License 2.0 发布。
