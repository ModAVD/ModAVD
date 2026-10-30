# 第三方组件

这是摘要，方便查。逐个文件的完整声明和许可证全文以 Release 主包里的 `LICENSES\NOTICE.txt` 为准；`LICENSES\NOTICE.csv` 是同样内容的表格。

下面的路径都相对主包里的 `sdk\emulator\`。适用版本：`37.2.10-mas30`。

## 我们改过的

| 组件 | 许可证 | 主要在哪些文件里 | 改动 |
|---|---|---|---|
| QEMU / Android Emulator（AOSP `platform/external/qemu`） | QEMU 部分 GPL-2.0-only，Android 部分 Apache-2.0；`qemu-system` 整体按 GPL-2.0-only 发布 | `qemu\windows-x86_64\qemu-system-x86_64*.exe`、`qemu-img.exe`、`emulator.exe`、`libandroid-emu-*.dll` 等 | 55 个补丁，每个按所改文件的许可证：改 QEMU 本体文件和带 GPL-2.0 文件头的按 GPL-2.0，改带 Apache-2.0 文件头的按 Apache-2.0（逐个见 `patches/BASE.md`） |
| gfxstream（AOSP `platform/hardware/google/gfxstream`） | Apache-2.0 | `lib64\libgfxstream_backend.dll` 等 | 22 个补丁，按 Apache-2.0 发布 |
| Crashpad（AOSP `platform/external/crashpad`） | Apache-2.0 | `crashpad_handler.exe` | 1 个补丁，按 Apache-2.0 发布 |

## 要随附源码的（GPL / LGPL）

完整对应源码在同一个 Release 的源码包（`ModAVD-37.2.10-mas30-sources.zip`）里。

| 组件 | 版本 | 许可证 | 怎么用 | 在哪些文件里 |
|---|---|---|---|---|
| QEMU（Android 版） | 基于 2.12 | GPL-2.0-only | 编进程序 | `qemu-system-x86_64*.exe`、`qemu-img.exe` |
| FFmpeg | 4.4.2，`--enable-gpl` | GPL-2.0-or-later | 静态链接 | `qemu-system-x86_64*.exe`、`emulator.exe`、`lib64\libgfxstream_backend.dll`、`qsn.exe` |
| GLib | AOSP `external/glib` | LGPL-2.1 | 动态库，`qemu-system` 和 `qemu-img` 加载 | `libglib2_windows_msvc-x86_64.dll`（根目录和 `lib64\` 各一份） |
| GLib（netsimd 里的） | 2.82.2 上游包 + `platform/build/bazel` 862d0ec3 的 `glib 2.82.2.bcr.5.aemu` 模块 | LGPL-2.1-or-later | 静态链接，供 libslirp 用 | `netsimd.exe`（Google 的预编译件，见下文「netsimd.exe」） |
| libusb | 1.0.24 | LGPL-2.1-or-later | 静态链接 | `emulator.exe`、`qemu-system-x86_64*.exe` |
| dtc / libfdt | AOSP `external/dtc` | GPL-2.0-or-later / BSD-2-Clause | 编进程序 | `qemu-system-x86_64*.exe` |
| Qt（含 Qt WebEngine） | 6.5.3 | LGPL-3.0-only | 动态库，带窗口的 `qemu-system-x86_64.exe` 加载，无头版不加载 | `lib64\qt\` |
| SeaBIOS / SeaVGABIOS | rel-1.11.1（QEMU `roms/seabios` 0551a4be），与 QEMU 2.12.0 发行包里的同名文件逐字节相同 | LGPL-3.0（SeaVGABIOS 是 SeaBIOS 项目的一部分，源码在同一棵树的 `vgasrc/`） | 固件 | `lib\pc-bios\bios*.bin`、`vgabios-*.bin` |
| iPXE | QEMU `roms/ipxe` 0600d3ae | GPL-2.0-or-later | 固件 | `lib\pc-bios\efi-*.rom` |
| QEMU 选项 ROM、键盘映射 | 同 QEMU | GPL-2.0 | 固件和数据 | `lib\pc-bios\*.bin`（上面以外的）、`lib\pc-bios\keymaps\` |

Qt 的预编译文件来自 AOSP，Google 没有记录对应的确切提交，所以源码包里放的是 Qt 6.5.3 官方源码；AOSP 的 Qt 构建脚本 `android/build/python/aemu/prebuilts/qt.py` 在 QEMU 仓库的源码包里。

## 宽松许可证

只要求附上版权声明和许可证，全文都在 `NOTICE.txt` 里。

| 许可证 | 组件 |
|---|---|
| Apache-2.0 | Abseil、gRPC（含 upb）、BoringSSL（按编译时提交 54ed1d5a 的 LICENSE）、Perfetto、Tink、Cuttlefish 的部分代码、ext4_utils、keymaster、Netsim（`netsimd.exe`，见下文）、SwiftShader、FlatBuffers、Mynewt NimBLE、模拟器的配置和接口定义文件 |
| BSD-3-Clause | protobuf、ANGLE（含 `libshadertranslator.dll`）、libvpx 1.12.0、WebRTC（另附专利授权）、RE2、Breakpad、libwebp、GoogleTest |
| BSD-2-Clause 等 | lz4、hostapd（BSD）、regex（Henry Spencer 的许可证） |
| MIT 及类似 | c-ares、curl、nlohmann/json、PicoSHA2、tinyobjloader、tinygltf、dirent、mman-win32、libxml2 2.9.2、pixman 0.34.0、Mesa lavapipe（`lib64\vulkan\libvulkan_lvp.dll`） |
| 各自的宽松许可证 | zlib（Zlib）、libpng（libpng 许可证）、libjpeg 6b（IJG） |
| 公有领域 | libselinux |
| Apache-2.0 / MIT / BSD | Vulkan 运行时（`lib64\vulkan\` 下 LunarG 的文件，见 `VulkanRT-License.txt` 的内容） |
| MPL-2.0 | `lib\ca-bundle.pem`（Mozilla 根证书数据） |
| BSD-3-Clause 及其他 | Chromium 和它的第三方组件（在 Qt WebEngine 里），各自的许可证在 Qt WebEngine 源码里 |

## netsimd.exe

`netsimd.exe` 是 Google 的预编译件，我们原样随附：

- **出处**：AOSP `prebuilts/android-emulator-build/common` 的提交 `beacdbe588c2`「Frontend-disabled Netsim BID 16317532 for emu-37-2」。
- **源码**：由 Google 用 Bazel 从 `platform/tools/netsim` 的 `f113660da2a5`（Netsim 1.0.23）编出。我们随附的是 Google 的 frontend-disabled 版，和公开源码的区别只有一处：`--no-cli-ui`（关掉给命令行工具用的 gRPC 服务）默认为 true。Google 没有公开这一行改动；用公开源码编出的 netsimd 运行时加 `--no-cli-ui`，行为相同。
- **GLib（LGPL-2.1-or-later，静态链接）**：确切是 GLib 2.82.2 上游包，加上 `platform/build/bazel` 提交 `862d0ec34f27` 里的 `registry/modules/glib/2.82.2.bcr.5.aemu` 模块（构建文件和 3 个补丁）。不是源码包里的 `external-glib`（那是 2.77.2，属于 `libglib2_windows_msvc-x86_64.dll`）。
- **随附的对应源码**：`glib-2.82.2.tar.xz`、`build-bazel-862d0ec34f27-glib-2.82.2.bcr.5.aemu.tar.xz`、`build-bazel-862d0ec34f27.tar.xz`、`tools-netsim-f113660da2a5.tar.xz`。用 netsim `f113660d` 加 build/bazel `862d0ec3` 可以重建行为一致的 netsimd，并换上修改过的 GLib 重新链接。
- **其他组件**（按程序里留下的源码路径）：netsim 本身、Rootcanal、Casimir 是 Apache-2.0；gRPC、Abseil、BoringSSL、protobuf 同上表；libslirp（BSD-3-Clause）、{fmt}（MIT）；Rust 标准库（MIT OR Apache-2.0）。
- **57 个 Rust crate**：来自 `platform/external/rust/android-crates-io`（pica 来自 `platform/external/rust/pica`）。版本按 android-crates-io 在 Google 编这一版时的 main（`83e064e564e8`）确定，程序里每个 crate 的源码路径在这些版本里都对得上。多数是 MIT 或 MIT OR Apache-2.0；`grpcio`、`grpcio-sys`、`pica` 是 Apache-2.0；`ring` 0.17.2 是它自己的 LICENSE（ISC 风格 + OpenSSL / SSLeay 许可证）；`icu_collections`、`icu_normalizer` 是 Unicode-3.0；`jiff` 按 MIT 用。

逐个 crate 的版本和各自的许可证原文见 `NOTICE.txt` 的「Netsim」一项。

## 微软的再分发文件

不是开源软件，按微软的再分发条款原样随附：

- Microsoft Visual C++ 2022 运行库 14.42.34433（根目录和 `qemu\windows-x86_64\` 下的 `concrt140`、`msvcp140*`、`vccorlib140`、`vcruntime140*`）；
- `lib64\gles_angle\d3dcompiler_47.dll`（文件版本 10.0.22621.755，Direct3D 着色器编译器，随 AOSP 的 ANGLE 预编译件原样附带）。依据是 Windows SDK 许可条款第 2.a 节「Distributable Code」：列在 REDIST.TXT 清单上的文件可以随自己的程序以目标代码形式分发；清单在许可条款给出的 http://go.microsoft.com/fwlink/?LinkId=524842 。这里只引用条款和清单地址，没有附原文。

## 程序图标（同人创作）

`emulator.exe`、`emulator-check.exe`、`qemu\windows-x86_64\qemu-system-x86_64.exe`、`qemu-system-x86_64-headless.exe` 里编进去的程序图标和窗口图标，是 ModAVD 换上的同人创作图像，画的是《明日方舟》的艾雅法拉，角色版权归上海鹰角网络所有。这些图像**不随 Apache-2.0、GPL 或本包里的任何其他许可证授权**。本项目与鹰角网络无关，也未获其认可。见 `NOTICE.txt` 第 2 部分「ModAVD program icon (fan art)」和仓库 README 的「图标与美术」。

## 包根目录的其他文件

| 文件 | 许可证 |
|---|---|
| `components\de.jrpie.android.launcher_57.apk`（µLauncher 0.2.12，F-Droid 发布的原包） | MIT；按它的构建文件，APK 里还有 AndroidX、Material Components、Kotlin 标准库、kotlinx.serialization，都是 Apache-2.0 |
| `补全组件.bat`、`收集问题信息.bat`、`tools\*.ps1`、`说明.txt`、`LICENSES\NOTICE.*` | Apache-2.0，Copyright 2026 qiyinxi |
| `ModAVD.exe`（管理器）、`tools\modavd.exe`（命令行、MCP），0.36.0 起 | AGPL-3.0-or-later，Copyright 2026 qiyinxi；静态链接的 Go 运行时和标准库是 BSD-3-Clause；`ModAVD.exe` 的程序图标是同人创作，同上文「程序图标」。源码在 [ModAVD/ModAVDManager](https://github.com/ModAVD/ModAVDManager) |
| `components\modavd-ime.apk`（宿主输入法），0.36.0 起 | AGPL-3.0-or-later，Copyright 2026 qiyinxi。源码在 [ModAVD/ModAVDHostIME](https://github.com/ModAVD/ModAVDHostIME) |
| `sdk\emulator\resources\modavd\*`（arm64 执行桥：`libmodavd_bridge.so`、`libmodavd_gate.so`、`modavd_bridge.rc`），0.36.0 起 | AGPL-3.0-or-later，Copyright 2026 qiyinxi；`libmodavd_gate.so` 静态链接的 Android NDK 的 LLVM libc++ / libc++abi 是 Apache-2.0 WITH LLVM-exception。源码见下 |

`补全组件.bat` 那一行是 `37.2.10-mas30` 的情况。0.36.0 起，其中包根的 `.bat` 和 `tools\*.ps1`（源码在本仓库的 [package/](package/)）改按 AGPL-3.0-or-later 发布；见 README 的[许可证与源码](README.md#许可证与源码)。

最后三行是 ModAVD 自己的程序，源码都在同一个 Release 的源码包（`ModAVD-<版本>-sources.zip`）里的 `modavd-src-<提交>.tar.xz`，`LICENSE` 和 `NOTICE` 在归档里；许可证全文是包里的 `LICENSES\AGPL-3.0.txt`。

## 不在包里的

- **adb 和系统镜像**：Google 的软件，受《Android 软件开发套件许可协议》约束，由「补全组件.bat」在你同意协议后从官方渠道下载。
- **官方构建里有、我们拿掉的**：`bin64\`（Cygwin 版 e2fsprogs 及其 DLL）、`drivers\`、`resources\` 里的虚拟场景、宏示例和 Pixel 10 皮肤、FlatBuffers 开发文件、ARM 和 32 位 x86 的 `qemu-system`。
- **x264**：FFmpeg 没有启用它，包里任何文件都没有链接 x264。

## 和 Google 原版 NOTICE 的差别

AOSP 构建会自动生成 `NOTICE.txt` / `NOTICE.csv`，有 62 项，但有错漏。我们在它的基础上改了这些：

- **改正**：
  - `qemu-system` 原来标 Apache-2.0，改为整体 GPL-2.0-only；
  - `ffmpeg` 原来指向 3.4.5、写的是 LGPL-2.1，改为实际链接的 4.4.2、GPL-2.0-or-later，文件列表按实际链接改；
  - `bios` 原来标 LGPL-2.0-or-later、挂在 `emulator.exe` 上，改为 SeaBIOS LGPL-3.0、iPXE GPL-2.0-or-later、QEMU 选项 ROM GPL-2.0，挂在 `lib\pc-bios\` 的固件上；
  - `gtest` 下面原来误放了 QEMU 的许可证，换成 GoogleTest 自己的。
- **补上**：zlib、libusb、BoringSSL、c-ares、WebRTC、RE2、Crashpad、Perfetto、Cuttlefish、Mesa lavapipe、Vulkan 运行时许可证、Chromium（Qt WebEngine）、微软再分发文件、Mozilla 证书包、µLauncher、模拟器数据文件、我们自己的脚本；原表漏列的 `netsimd.exe`、`lib\hostapd.conf`、QEMU 键盘映射等文件。
- **补上（第二批）**：mman-win32（MIT；AOSP 构建把它登记在「regex」名下，生成 NOTICE 时被合并掉了）；`netsimd.exe` 里的组件和 Rust crate。
- **注明**：`qemu`、`Qt 6`、`vulkan-sdk`、`angle-renderer`、`vcredist`、`libvpx`（实际链接 v1.12.0）、`LibXml2`、`pixman-1`（实际链接 0.34.0）、`Netsim` 加了版本、源码或归属说明。
- **删掉**：只对应已拿掉文件的 `e2fs` 一项；各项文件列表里删掉了包里没有的文件。
- **文件名**：原表里不带 `lib` 前缀、不带 `lib64\` 目录的写法，改成包里实际的路径。

## 许可证全文

在 `LICENSES\` 下，也附在 `NOTICE.txt` 末尾：

| 文件 | 取自 |
|---|---|
| `Apache-2.0.txt` | AOSP `external/tink` 的 LICENSE |
| `GPL-2.0.txt` | https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt |
| `LGPL-2.1.txt` | https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt |
| `GPL-3.0.txt` | https://www.gnu.org/licenses/gpl-3.0.txt |
| `LGPL-3.0.txt` | https://www.gnu.org/licenses/lgpl-3.0.txt |
| `MPL-2.0.txt` | https://www.mozilla.org/media/MPL/2.0/index.txt |
| `uLauncher-LICENSE.txt` | µLauncher 0.2.12 源码的 LICENSE |
| `AGPL-3.0.txt`（0.36.0 起） | AUTO-MAS 仓库的 LICENSE（与 https://www.gnu.org/licenses/agpl-3.0.txt 内容相同） |

GPL-2.0、LGPL-2.1、GPL-3.0、LGPL-3.0 和 MPL-2.0 这五份文本是 2026-10-17 从上面的官方地址下载的现行版本。`AGPL-3.0.txt` 是 ModAVD 自己的程序（见[包根目录的其他文件](#包根目录的其他文件)）的许可证，取自 AUTO-MAS 仓库的 LICENSE，内容与本仓库的 [package/LICENSE](package/LICENSE) 相同。这六份放进包里时都只把换行改成了 CRLF；`NOTICE.txt` 第 3 部分记了每份文本（LF 换行）的 SHA-256。

本仓库自己的 [LICENSE](LICENSE) 是 Apache-2.0 全文，与 `Apache-2.0.txt` 内容相同。
