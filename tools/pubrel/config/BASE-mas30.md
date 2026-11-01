# ModAVD mas-30 补丁

ModAVD 37.2.10-mas30（`Pkg.BuildId=mas-30`）在 AOSP `emu-main-dev` 上改了三个仓库。每个目录是该仓库从 AOSP 基准到编译时分支头的完整 `git format-patch` 输出，按文件名顺序用 `git am --keep-cr` 回放。

| 目录 | AOSP 仓库 | 基准（AOSP 公开提交） | 补丁数 | 回放后的树 = 编译时分支头 | 许可证 |
| --- | --- | --- | --- | --- | --- |
| `qemu/fixq30` | `platform/external/qemu` | `f0c183f1cc7456ecd6f3607f2f47893768ae4334` | 55 | `5aedc70819651ef386cef810afa6c2be3c028467` | 按所改文件，见下文：40 个 GPL-2.0，11 个 Apache-2.0，4 个两者都有；0055 里的图片另见下文 |
| `gfxstream/fixq28` | `platform/hardware/google/gfxstream` | `04d0287b04f72d908c621a919fa2ea5c563b1a66` | 22 | `d027890c4799a8537ce1e981fec784a99a269ade` | Apache-2.0 |
| `crashpad/module-version22` | `platform/external/crashpad` | `dd7db6790d73210abaef36a04f2a2bd52cf22b89` | 1 | `377d2d398368c38d43f4ecbca084c89a68c10530` | Apache-2.0 |

- 其余项目的确切提交见源码包里的 `manifest-aosp-base.xml`（全是 AOSP 公开提交）与 `manifest-mas30.xml`（编译时各项目的 HEAD）。
- 「回放后的树」一栏是分支头提交的编号，它本身不在公开服务器上；回放后每个提交的树（`git rev-parse HEAD^{tree}`）与原分支逐个相同。
- qemu 补丁里有 CRLF 文件（`whp-dispatch.h` 等），`git am` 必须带 `--keep-cr`。
- 标题以 `MAS local:` / `MAS experiment:` 开头的是只给本项目用的改动（诊断日志、零外连、崩溃转储只留本机等），其余多为通用修复。
- 补丁作者统一写作 `qiyinxi <qiyinxi@foxmail.com>`；有 AI 辅助的提交保留 `Assisted-by:` 一行。

## qemu 补丁的许可证

每个补丁按它所改文件的许可证发布，与上游文件头一致：

- 改 QEMU 本体文件的（`target/`、`hw/`、`util/`、`slirp/`、`exec.c`、`cpus.c` 等），按 GPL-2.0，与 QEMU 整体相同。其中几个文件自己带 MIT / BSD 文件头（`util/main-loop.c`、`util/oslib-win32.c`、`os-win32.c`、`cpus.c`、`hw/i386/pc.c`、`slirp/*`），这些文件头保留不变。
- 改 `android/`、`android-qemu2-glue/` 下文件的，跟着该文件的文件头：带 GPL-2.0 文件头的按 GPL-2.0（这两个目录里有不少这样的文件），带 Apache-2.0 文件头的按 Apache License 2.0。没有文件头的文件按仓库 `LICENSE` 的说法，是 GPL-2.0 或更新版本。

| 许可证 | 补丁 |
| --- | --- |
| Apache-2.0 | 0002、0003、0008、0012、0036、0038、0042、0043、0050、0051、0052 |
| 两者都有，按文件分 | 0005、0014（`EmulatorService.cpp` 为 Apache-2.0，`SharedMemory_win32.cpp` 为 GPL-2.0）；0016（`android-qemu2-glue/netsim/libslirp_driver.cpp` 为 Apache-2.0，`util/main-loop.c` 为 GPL-2.0）；0039（`CrashSytemInit.cpp` 为 Apache-2.0，`CrashUploadTool.cpp`、`CrashUploader.cpp` 为 GPL-2.0） |
| GPL-2.0 | 其余 40 个（含 0055 的代码部分，见下） |

这张表是按编译时提交上各文件开头的许可证声明逐个归出来的（0001–0054 按 `fac53907`，0055 按 `5aedc708`）。

**0055（换成 ModAVD 图标）**：改的是顶层 `CMakeLists.txt`（没有文件头，按仓库 `LICENSE` 属 GPL-2.0 或更新版本）和 `android/android-ui/modules/aemu-ui-window/include/android/icons.h`（GPL-2.0 文件头），这些代码改动按 GPL-2.0。补丁里的图片——`android/images/emulator_icon.ico`、`emulator_icon_32/128/256.png`，以及 `icons.h` 里由这些图片转成的数据——是同人创作图像，画的是《明日方舟》的艾雅法拉，角色版权归上海鹰角网络所有；**这些图片不随 GPL-2.0 或 Apache-2.0 授权**，见仓库 README 的「图标与美术」。

回放示例（以 qemu 为例）：

```sh
git clone https://android.googlesource.com/platform/external/qemu
cd qemu
git checkout -b mas30 f0c183f1cc7456ecd6f3607f2f47893768ae4334
git am --keep-cr ../patches/qemu/fixq30/*.patch
```
