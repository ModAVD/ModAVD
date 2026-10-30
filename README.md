# ModAVD（魔改 AVD）

![ModAVD](assets/banner.png)

给 Windows 上的挂机脚本用的安卓模拟器。基于 AOSP Android Emulator 37.2.10，加了一批补丁后自己编译。

现在配合 [AUTO-MAS](https://github.com/AUTO-MAS-Project/AUTO-MAS) 使用：建实例、开关机、接截图和点击都由 AUTO-MAS 完成。0.36.0 起包里另带管理器 `ModAVD.exe` 和命令行 `tools\modavd.exe`。

**文档站：<https://modavd.github.io/>**（安装、升级、反馈、隐私、各组件和参与开发的说明都在那里）

> 这是非官方项目，与 Google 没有关联。

- [这是什么](#这是什么)
- [适用环境](#适用环境)
- [安装](#安装)
- [升级](#升级)
- [许可证与源码](#许可证与源码)
- [反馈](#反馈)
- [隐私](#隐私)
- [免责声明](#免责声明)
- [English](#english)
- [图标与美术](#图标与美术)

## 这是什么

- 一个跑在 Windows 上的安卓模拟器，用 Windows 虚拟机监控程序平台（WHPX）做硬件加速。
- 面向无人值守的挂机：通常不开窗口（无头）运行，由脚本拉起、跑完关掉；需要看画面或手动登录游戏时，也可以带窗口启动。
- 模拟器本体来自 Google 开源的 [Android Emulator](https://android.googlesource.com/platform/external/qemu/)。我们改了其中的 QEMU、gfxstream（图形转发）和 Crashpad（崩溃处理），改动以补丁形式公开（`patches/`）。
- adb 和安卓系统镜像属于 Google，受《Android 软件开发套件许可协议》约束，不能由我们分发，所以不在 Release 里，由包里的「补全组件.bat」在你的电脑上从官方渠道下载。

和官方模拟器相比，主要是：模拟器自己不往外发东西；强行结束不弄坏虚拟磁盘；安卓释放的内存还给 Windows；数据盘可以改大；修了一批图形、网络和 WHPX 的问题；0.36.0 起带 arm64 执行桥，只有 arm64 版本的《阴阳师》也能跑。逐项说明见文档站的[和官方模拟器有什么不同](https://modavd.github.io/guide/changes)，逐个补丁见 `patches/`。

版本号从 `0.32.1` 起写成 `0.<构建号>.<重新打包次数>`：0 表示还在内测，构建号就是 mas-N。当前版本 `0.36.0`（构建号 mas-36）。

这个仓库放的是模拟器补丁、arm64 执行桥（[bridge/](bridge/)）和随包脚本（[package/](package/)）。管理器和命令行在 [ModAVDManager](https://github.com/ModAVD/ModAVDManager)，宿主输入法在 [ModAVDHostIME](https://github.com/ModAVD/ModAVDHostIME)。

## 适用环境

| 项目 | 要求 |
|---|---|
| 系统 | Windows 10 / 11，64 位 |
| CPU | x86_64（Intel / AMD），BIOS 里开着虚拟化（Intel VT-x / AMD SVM） |
| Windows 功能 | 「Windows 虚拟机监控程序平台」（WHPX） |
| 显卡 | 支持 Vulkan 的独立显卡或较新的集成显卡 |
| 内存 | AUTO-MAS 默认每台实例 6 GB，开机时电脑要再空出约 1.5 GB |
| 硬盘 | 放在 NTFS 盘。补全组件时要约 6.5 GB 可用；每台实例第一次开机要 10 GB 以上可用，之后随游戏数据增长，一个游戏通常几十 GB |
| 路径 | 只含英文、数字的路径，例如 `D:\ModAVD` |
| AUTO-MAS | 推荐 `v5.7.0` 或更新。v5.7.0-beta.5 起 MAA 和 MFW 脚本（MaaFramework 项目，如 M9A）可以用，其他脚本暂不支持；各版本的支持情况见 Release 说明 |

## 安装

1. 从 [Releases](../../releases) 下载 `ModAVD-0.36.0-win-x64.zip`。
2. 右键「全部解压缩」（压缩包里没有外层文件夹，不要用「解压到当前文件夹」），把得到的文件夹放到一个只含英文、数字的路径，例如 `D:\ModAVD`。
3. 双击文件夹里的「补全组件.bat」，同意 Google 的许可协议后，它下载 adb 和 Android 14 系统镜像（约 1.5 GB）。
4. 在 AUTO-MAS 的「模拟器管理」里点「管理模拟器」→「添加魔改 AVD（内测）」，选这个文件夹，检查通过后添加。

详细步骤、开启虚拟化和常见问题见文档站的[安装](https://modavd.github.io/guide/install)。

## 升级

关掉所有实例，稍等半分钟，把新包整个解压到旧的根目录、覆盖同名文件。实例、游戏和登录状态都在 `avd\` 里，不会被动到。退回 0.36.0 以前的版本之前，要先卸掉每台实例上的 arm64 执行桥。逐项说明见文档站的[升级](https://modavd.github.io/guide/upgrade)。

## 许可证与源码

- **除 `package/`、`bridge/`、`assets/` 和 `patches/` 下的 `.patch` 补丁以外，本仓库的内容**（README 等文档、`docs/`、`scripts/`、`.github/` 等）：Apache License 2.0，见根目录的 [LICENSE](LICENSE)。Copyright 2026 qiyinxi。`package/` 和 `bridge/` 按 AGPL-3.0-or-later（见下一条），补丁按各自所改文件的许可证（见「补丁」一条），`assets/` 下的图片和程序图标见[图标与美术](#图标与美术)。
- **ModAVD 自己的程序**：0.36.0 起都是 GNU Affero General Public License 第 3 版或更新版本（AGPL-3.0-or-later）。
  - 包里的脚本（补全组件、收集问题信息、复制配置信息、装执行桥）：本仓库的 [package/](package/)，许可证全文见 [package/LICENSE](package/LICENSE)。0.35.0 及更早的包里的脚本按 Apache License 2.0 发布。
  - arm64 执行桥（`sdk\emulator\resources\modavd\`，0.36.0 起）：本仓库的 [bridge/](bridge/)，许可证全文见 [bridge/LICENSE](bridge/LICENSE)；也在同一个 Release 的源码包里（见下面「源码」一条）。
  - 管理器 `ModAVD.exe` 和命令行 `tools\modavd.exe`（0.36.0 起）：在 [ModAVD/ModAVDManager](https://github.com/ModAVD/ModAVDManager) 仓库。
  - 宿主输入法 `components\modavd-ime.apk`（0.36.0 起）：在 [ModAVD/ModAVDHostIME](https://github.com/ModAVD/ModAVDHostIME) 仓库。
- **补丁**：每个补丁按它所改文件的许可证发布，与上游文件头一致：
  - QEMU 仓库（`platform/external/qemu`）里，改 QEMU 本体文件的、改带 GPL-2.0 文件头的文件的，按 GPL-2.0；改带 Apache-2.0 文件头的文件的（`android/`、`android-qemu2-glue/` 下的一部分），按 Apache License 2.0。逐个补丁的许可证见 [patches/BASE.md](patches/BASE.md)。
  - gfxstream 和 Crashpad 的补丁按 Apache License 2.0 发布，与它们本身相同。
- **Release 里的程序**：含有 QEMU（GPL-2.0）、FFmpeg（GPL-2.0 或更新版本，静态链接）、Qt 6.5.3（LGPL-3.0）、GLib 和 libusb（LGPL-2.1）等第三方组件。`qemu-system` 等程序整体按 GPL-2.0 发布。每个组件的许可证见包里的 `LICENSES\NOTICE.txt`，摘要见 [THIRD_PARTY.md](THIRD_PARTY.md)。
- **源码**：同一个 Release 的源码包（`ModAVD-<版本>-sources.zip`）里有完整的对应源码：编进程序的 24 个仓库在编译时的确切版本（每个仓库一个 `.tar.xz`，QEMU 仓库里含构建脚本）、FFmpeg / libvpx / libxml2 / pixman 的源码包、SeaBIOS 和 iPXE 源码、Qt 6.5.3 源码、`netsimd.exe` 静态链接的 GLib 2.82.2 及其构建模块和 netsim 源码、两份仓库清单（本仓库的 [manifests/](manifests/) 里也有）；0.36.0 起还有 `modavd-src-<提交>.tar.xz`，是 ModAVD 自己的程序（管理器和命令行、arm64 执行桥、宿主输入法）的源码，按 AGPL-3.0-or-later 发布，`LICENSE` 和 `NOTICE` 在归档里；以及逐个说明这些文件的 `SOURCES.txt`。补丁在本仓库的 [patches/](patches/)，自己编译见文档站的[从源码构建](https://modavd.github.io/develop/build)。
- **Google 的组件**：adb 和系统镜像不在 Release 里，由「补全组件.bat」从官方渠道下载，受《Android 软件开发套件许可协议》约束，下载前要你同意。

## 反馈

在本仓库的 [Issues](../../issues) 里提。出问题后先别关模拟器，双击文件夹里的「收集问题信息.bat」，它会在桌面生成一个问题包 zip。

Issues 是公开的，谁都能下载附件。问题包里有日志、截图，模拟器崩溃过的话还有崩溃转储（`crash` 文件夹，含本机路径和 Windows 用户名）：

- 公开 Issue 里只贴不含 `crash` 文件夹的部分；
- 带崩溃转储的完整问题包请发邮件到 qiyinxi@foxmail.com，邮件里写上 Issue 编号。

上传前请先看文档站的[反馈问题](https://modavd.github.io/guide/feedback)。想帮忙测试、改文档或提补丁，见 [CONTRIBUTING.md](CONTRIBUTING.md) 和文档站的[参与开发](https://modavd.github.io/develop/)。

## 隐私

- 模拟器程序自己不联网：不上传使用统计和崩溃报告，不检查更新，不下载特性开关。例外：带窗口运行时，如果打开扩展控制里的「位置」页，地图会访问 Google 地图服务。
- 安卓系统和里面的应用（游戏、Google Play 服务等）照常联网，这是它们自己的流量。
- 崩溃转储只存在本机，不会上传。
- 「补全组件.bat」只在你运行时访问下载镜像；「收集问题信息.bat」只在本机打包，不上传。

详见文档站的[隐私](https://modavd.github.io/guide/privacy)。

## 免责声明

- 本项目是非官方项目，与 Google LLC 没有关联，也没有得到 Google 的认可或支持。
- Android 是 Google LLC 的商标。
- 软件按「现状」提供，不提供任何担保，详见各许可证。
- 用模拟器和脚本运行游戏是否符合游戏的用户协议，请自行判断。

## English

ModAVD is a modified build of the AOSP Android Emulator (37.2.10) for running unattended automation scripts on Windows. It is currently used through [AUTO-MAS](https://github.com/AUTO-MAS-Project/AUTO-MAS). Documentation (mostly Chinese, with an English summary): <https://modavd.github.io/en/>.

- The emulator itself does not contact servers on its own: no usage metrics, no crash report upload, no update check, no feature-flag download. Crash dumps stay on your machine. (The only exception is the map on the Location page of the extended controls, if you open it in windowed mode.) The Android system and its apps use the network as usual.
- Requirements: Windows 10/11 64-bit, an x86_64 CPU with virtualization enabled, and the Windows Hypervisor Platform (WHPX) feature.
- Install: download the release zip, extract it to a path with ASCII characters only, run `补全组件.bat` (it shows the Android SDK License Agreement and downloads adb and the Android 14 system image from Google's official channels after you accept), then add the folder in AUTO-MAS.
- Licenses: everything in this repository except `package/`, `bridge/`, `assets/` and the `.patch` files under `patches/` (README and the other documents, `docs/`, `scripts/`, `.github/`, ...) is Apache-2.0 (root `LICENSE`). From 0.36.0, ModAVD's own programs are AGPL-3.0-or-later: the bundled scripts in `package/` (full text in `package/LICENSE`; the scripts in 0.35.0 and earlier packages are Apache-2.0), the arm64 execution bridge in `bridge/` (full text in `bridge/LICENSE`), the manager and CLI (repository ModAVD/ModAVDManager) and the host IME (repository ModAVD/ModAVDHostIME); from 0.36.0 the source bundle also has their code in `modavd-src-<commit>.tar.xz`, with `LICENSE` and `NOTICE`. Each patch is licensed like the files it changes: QEMU patches are GPL-2.0, except changes to files with an Apache-2.0 header, which are Apache-2.0 (see `patches/BASE.md`); gfxstream and Crashpad patches are Apache-2.0. The binaries contain third-party components listed in `LICENSES\NOTICE.txt`; the complete corresponding source code is attached to each release.
- Icon and artwork: the images under `assets/` and the program icon built into the binaries are fan art of Eyjafjalla from Arknights (character © Shanghai Hypergryph). They are not covered by the Apache License 2.0 of this repository or by any other license here. This project is not affiliated with or endorsed by Hypergryph.
- Feedback: open an issue. Do not attach crash dumps (the `crash` folder of the problem report) to public issues; send them by e-mail to qiyinxi@foxmail.com with the issue number.
- This is an unofficial project, not affiliated with or endorsed by Google. Android is a trademark of Google LLC.

## 图标与美术

- `assets/` 下的图片（横幅、头像、社交预览图），以及编进程序里的图标（`emulator.exe`、`emulator-check.exe`、`qemu-system-x86_64*.exe` 的程序图标和窗口图标），都是同人创作，**不在本仓库的 Apache-2.0 许可范围内**，也不随本项目的任何其他许可证授权。
- 画中角色是《明日方舟》的艾雅法拉，角色版权归上海鹰角网络所有。
- 本项目与鹰角网络无关，也未获其认可。
- 如有版权问题，请通过 [Issues](https://github.com/ModAVD/ModAVD/issues) 或邮件 qiyinxi@foxmail.com 联系，我们会立即处理。
