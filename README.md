# ModAVD（魔改 AVD）

![ModAVD](assets/banner.png)

给 Windows 上的挂机脚本用的安卓模拟器。基于 AOSP Android Emulator 37.2.10，加了一批补丁后自己编译。

现在配合 [AUTO-MAS](https://github.com/AUTO-MAS-Project/AUTO-MAS) 使用：建实例、开关机、接截图和点击都由 AUTO-MAS 完成。以后打算做成可以单独使用的产品。

> 这是非官方项目，与 Google 没有关联。

- [这是什么](#这是什么)
- [和官方模拟器有什么不同](#和官方模拟器有什么不同)
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
- 模拟器本体来自 Google 开源的 [Android Emulator](https://android.googlesource.com/platform/external/qemu/)。我们改了其中的 QEMU、gfxstream（图形转发）和 Crashpad（崩溃处理），改动以补丁形式公开。
- adb 和安卓系统镜像属于 Google，受《Android 软件开发套件许可协议》约束，不能由我们分发，所以不在 Release 里，由包里的「补全组件.bat」在你的电脑上从官方渠道下载。

## 和官方模拟器有什么不同

下面是当前版本（`0.36.0`，构建号 mas-36，基于 Google AOSP Android Emulator 37.2.10）里对使用者有影响的改动，逐个补丁见 `patches/`。

版本号从 `0.32.1` 起写成 `0.<构建号>.<重新打包次数>`：0 表示还在内测，构建号就是 mas-N。`0.32.1` 的模拟器和之前发布的 `37.2.10-mas32` 相同，只是换成了新的包格式。

**不往外发东西**

- 不上传使用统计（`-metrics-collection` 按 `-no-metrics` 处理）。
- 不上传崩溃报告，也不弹窗询问；崩溃转储只存在本机，保留最新 5 份。
- 开机不检查更新，不下载特性开关（用本地缓存或包里自带的那一份）。

详见 [docs/privacy.md](docs/privacy.md)。

**扛得住强行结束**

- 数据盘以直写（write-through）方式交给安卓，强行结束模拟器进程不会弄坏虚拟磁盘（qcow2）的元数据，我们的硬杀测试里没有出现过损坏。关机前最后几秒还没写到盘上的文件仍可能丢失。
- 修了崩溃处理程序（crashpad_handler）随机退出的问题。原来它退出以后，模拟器再崩溃时会一直卡住、也不留转储。

**少占资源**

- 支持内存气球的空闲页回报：安卓里释放的内存会还给 Windows。省的是实际占用的物理内存，不是提交量，效果看负载。
- 新建实例第一次开机时，不再临时占满数据盘上限的空间。数据盘所在的盘是 NTFS 或 ReFS 时，首次建盘只要求 10 GB 可用（官方版要求上限的 1.2 倍，64 GB 上限要 77 GB）。
- 不指定 `-logcat` 时，不再把安卓日志转发到电脑这边。
- 图形暂存缓冲从 16 MB 起按需增长（上限仍是 128 MB），不再一开机就分配 128 MB。NVIDIA 显卡上每台实例静置时少占约 140 MB 内存。

**磁盘**

- 安卓一次提交的多段小块读写（合计不超过 64 KiB）先拼成一块再交给 Windows，每次写盘的系统调用从约 4 次降到 1 次。
- 数据盘可以改大，游戏数据不动：配置的数据盘比现有的大时，开机前只把虚拟磁盘扩大（从不缩小），安卓开机后在线扩满。官方版这时要么不生效，要么清空整个实例。
- 新建数据盘时 inode 最多 1,048,576 个（16 GB 分区的默认值），64 GB 数据盘的 inode 表从 1 GiB 降到 256 MiB，新实例少占约 0.7 GB 磁盘。已有的盘不变。
- 新建数据盘的 resize inode 按内核在线扩容的要求排布，以后按 16 GB 建的盘可以完整地改大（以前只能扩到约 30 GB）。

**Windows 虚拟机监控程序平台（WHPX）**

- 修了暂停、恢复、读快照前后客体时钟（TSC）不同步的几处问题。
- 本地 APIC 默认交给 Windows 虚拟机监控程序模拟。
- 虚拟 CPU 上限从 6 个提到电脑逻辑处理器数的一半。

**网络**

- 每个 TCP 连接的缓冲区从 8 KiB 提到 128 KiB。
- 在 Windows 上轮询全部网络连接，原来只看前 128 个。
- 电脑这边的连接加了短保活，对端失联时能更快断开。
- 修了注销一个没登记过的连接时读空指针、模拟器整个退出的问题（实例重启后 adb 重连时遇到过；官方代码里也有）。

**图形（gfxstream）**

- 修了 NVIDIA 显卡上约 2%–5% 的画面读回成全黑的问题（闪屏、截图偶发全黑）。已提交上游。
- 修了渲染线程争用 OpenGL 上下文的问题。原来游戏切换视频时，可能因为 YUV 转换着色器编译失败，整个模拟器直接退出。
- 安卓里的程序在 Vulkan 查询里带上电脑这边不认识的扩展结构（光线追踪、网格着色器等）时，不再直接退出。《明日方舟：终末地》原来一建 Vulkan 设备，模拟器就退出。
- 修了几处 Vulkan 描述符模板更新和 OpenGL ES 计算着色器的兼容问题，以及视频硬解（CUDA）路径的一处崩溃和几处显存泄漏。
- 加了两个默认关闭的环境变量开关：`ANDROID_EMU_VK_HIDE_REBAR=1` 把电脑上「显存且主机可见」的内存（NVIDIA ReBAR）对安卓只报成显存；`ANDROID_EMU_VK_ASTC_BC3=1` 在模拟 ASTC 纹理时解压成 BC3（实验性）。

**界面与日志**

- 侧边工具栏精简为电源、音量加减、返回、主页（另有最小化和关闭）。设环境变量 `ANDROID_EMU_FULL_TOOLBAR=1` 可以恢复完整工具栏。
- 日志每行立即写出，标准错误输出不再整块缓冲，崩溃前的最后几行不会丢。
- 程序图标和窗口图标换成了 ModAVD 自己的图标（同人创作，见[图标与美术](#图标与美术)）。

**输入法**

- 加了宿主输入法通道（goldfish pipe 服务 `modavd_ime`）和控制台命令 `ime`，配合安卓里的客体输入法，可以在模拟器窗口里直接用 Windows 输入法给安卓打中文等文字。0.36.0 起客体输入法随包提供（`components\modavd-ime.apk`），用管理器或命令行开机时自动装上；没装时模拟器的行为和之前一样。

**arm64 执行桥**

- 实例目录里有开机标记 `modavd-writable-system` 时，不管由谁启动，都按可写系统镜像开机。0.36.0 起的 arm64 执行桥（见 [bridge/](bridge/)）装进实例时放上这个标记、卸掉时删掉，所以装过桥的实例照常用 AUTO-MAS 开机，桥也在。没有标记的实例和之前一样。

**拿掉的文件**

和官方构建装出来的文件相比，Release 包里没有这些（挂机用不到）：`bin64\`（Cygwin 版 e2fsprogs）、`drivers\`（USB 直通驱动）、虚拟相机场景、宏示例和 Pixel 10 皮肤、FlatBuffers 开发文件，以及 ARM 和 32 位 x86 的 `qemu-system`。所以只能运行 x86_64 系统镜像，也没有 `-usb-passthrough` 和虚拟场景相机。

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
2. 压缩包里没有外层文件夹，请右键「全部解压缩」，或用 7-Zip「解压到 ModAVD-0.36.0-win-x64\」，得到 `ModAVD-0.36.0-win-x64` 文件夹。不要用「解压到当前文件夹」，否则文件会直接散在压缩包所在的目录里。把这个文件夹放到一个只含英文、数字的路径，可以改名，例如 `D:\ModAVD`。以后所有实例的数据都存在这个文件夹里，挑空间大的盘；不要放进 `C:\Program Files` 这类要管理员权限的目录。
3. 双击文件夹里的「补全组件.bat」。它会先显示 Google《Android 软件开发套件许可协议》全文，输入 `y` 同意后，下载 adb 和 Android 14 系统镜像（约 1.5 GB），校验后装进 `sdk\`。中途断了再运行一次就会接着下。
4. 在 AUTO-MAS 里添加：打开「模拟器管理」，在类型为「自动识别（推荐）」的模拟器配置里（没有就点「手动添加模拟器」新建一个）点「管理模拟器」→「添加魔改 AVD（内测）」，选这个文件夹，点「检查」，全部通过后点「添加」。

详细步骤和常见问题见 [docs/install.md](docs/install.md)。

## 升级

先在 AUTO-MAS 里关掉所有魔改 AVD 实例，稍等半分钟，再把新包整个解压到旧的根目录、覆盖同名文件（覆盖时提示文件被占用，是模拟器的附属进程还没退出，再等一会儿重试）。0.36.0 起包根多了 `ModAVD.exe`（管理器）和 `tools\` 里的 `modavd.exe`（命令行）、`modavd-bridge.ps1`，`components\` 里多了宿主输入法 `modavd-ime.apk`，只换 `sdk\emulator` 会少了它们。实例、游戏和登录状态都在 `avd\` 里，新包里没有这个文件夹，不会被动到；AUTO-MAS 里也不用重新添加。想顺便清掉新版拿掉的旧文件，就先删掉旧的 `sdk\emulator\` 再解压。逐项说明和从内测包升级见 [docs/upgrade.md](docs/upgrade.md)。

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
- **源码**：同一个 Release 的源码包（`ModAVD-<版本>-sources.zip`）里有完整的对应源码：编进程序的 24 个仓库在编译时的确切版本（每个仓库一个 `.tar.xz`，QEMU 仓库里含构建脚本）、FFmpeg / libvpx / libxml2 / pixman 的源码包、SeaBIOS 和 iPXE 源码、Qt 6.5.3 源码、`netsimd.exe` 静态链接的 GLib 2.82.2 及其构建模块和 netsim 源码、两份仓库清单（本仓库的 [manifests/](manifests/) 里也有）；0.36.0 起还有 `modavd-src-<提交>.tar.xz`，是 ModAVD 自己的程序（管理器和命令行、arm64 执行桥、宿主输入法）的源码，按 AGPL-3.0-or-later 发布，`LICENSE` 和 `NOTICE` 在归档里；以及逐个说明这些文件的 `SOURCES.txt`。补丁在本仓库的 [patches/](patches/)。
- **Google 的组件**：adb 和系统镜像不在 Release 里，由「补全组件.bat」从官方渠道下载，受《Android 软件开发套件许可协议》约束，下载前要你同意。

## 反馈

在本仓库的 [Issues](../../issues) 里提。出问题后先别关模拟器，双击文件夹里的「收集问题信息.bat」，它会在桌面生成一个问题包 zip。

Issues 是公开的，谁都能下载附件。问题包里有日志、截图，模拟器崩溃过的话还有崩溃转储（`crash` 文件夹，含本机路径和 Windows 用户名）：

- 公开 Issue 里只贴不含 `crash` 文件夹的部分；
- 带崩溃转储的完整问题包请发邮件到 qiyinxi@foxmail.com，邮件里写上 Issue 编号。

上传前请先看 [docs/feedback.md](docs/feedback.md)。

想帮忙测试、改文档或提补丁，见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 隐私

- 模拟器程序自己不联网：不上传使用统计和崩溃报告，不检查更新，不下载特性开关。例外：带窗口运行时，如果打开扩展控制里的「位置」页，地图会访问 Google 地图服务。
- 安卓系统和里面的应用（游戏、Google Play 服务等）照常联网，这是它们自己的流量。
- 崩溃转储只存在本机，不会上传。
- 「补全组件.bat」只在你运行时访问下载镜像；「收集问题信息.bat」只在本机打包，不上传。

详见 [docs/privacy.md](docs/privacy.md)。

## 免责声明

- 本项目是非官方项目，与 Google LLC 没有关联，也没有得到 Google 的认可或支持。
- Android 是 Google LLC 的商标。
- 软件按「现状」提供，不提供任何担保，详见各许可证。
- 用模拟器和脚本运行游戏是否符合游戏的用户协议，请自行判断。

## English

ModAVD is a modified build of the AOSP Android Emulator (37.2.10) for running unattended automation scripts on Windows. It is currently used through [AUTO-MAS](https://github.com/AUTO-MAS-Project/AUTO-MAS).

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
