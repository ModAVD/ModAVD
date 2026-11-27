# ModAVD 补丁

ModAVD 在 AOSP `emu-main-dev` 上改了三个仓库。每个目录是该仓库从 AOSP 基准到编译时分支头的完整 `git format-patch` 输出，按文件名顺序用 `git am --keep-cr` 回放。各版本用的目录：
`series.json` 的 `platforms` 按补丁文件名排序，一项对应一个补丁；取值是 `windows`、`macos`、`linux` 或 `all`。目录表的「平台」是这一列的汇总。


| 版本 | qemu | gfxstream | Crashpad |
| --- | --- | --- | --- |
| 0.43.0（`Pkg.BuildId=0.43.0`） | qemu/0.43.0 | gfxstream/0.42.0 | crashpad/module-version22 |
| 0.42.0（`Pkg.BuildId=0.42.0`） | qemu/0.42.0 | gfxstream/0.42.0 | crashpad/module-version22 |
| 0.41.0（`Pkg.BuildId=mas-41`） | qemu/mas41 | gfxstream/mas41 | crashpad/module-version22 |
| 0.40.0（`Pkg.BuildId=mas-40`） | qemu/mas40 | gfxstream/mas38 | crashpad/module-version22 |
| 0.39.0（`Pkg.BuildId=mas-39`；0.39.1 相同） | qemu/mas39 | gfxstream/mas38 | crashpad/module-version22 |
| 0.38.0（`Pkg.BuildId=mas-38`；0.38.1、0.38.2 相同） | qemu/mas38 | gfxstream/mas38 | crashpad/module-version22 |
| 0.37.0（`Pkg.BuildId=mas-37`；0.37.1 相同） | qemu/mas37 | gfxstream/mas37 | crashpad/module-version22 |
| 0.36.0（`Pkg.BuildId=mas-36`） | qemu/mas36 | gfxstream/mas35 | crashpad/module-version22 |
| 0.35.0（`Pkg.BuildId=mas-35`） | qemu/mas35 | gfxstream/mas35 | crashpad/module-version22 |
| 37.2.10-mas32（`Pkg.BuildId=mas-32`；0.32.1 相同） | qemu/mas32 | gfxstream/fixq28 | crashpad/module-version22 |
| 37.2.10-mas30（`Pkg.BuildId=mas-30`） | qemu/fixq30 | gfxstream/fixq28 | crashpad/module-version22 |

| 目录 | AOSP 仓库 | 基准（AOSP 公开提交） | 补丁数 | 平台 | 回放后的树 = 编译时分支头 | 许可证 |
| --- | --- | --- | --- | --- | --- | --- |
| `qemu/0.43.0` | `platform/external/qemu` | `f0c183f1cc7456ecd6f3607f2f47893768ae4334` | 116 | windows / all | `30139c404f59c9f312292466c2fc5fcc65efeae0` | 按所改文件，见下文：73 个 GPL-2.0，24 个 Apache-2.0，19 个两者都有；0055 里的图片另见下文 |
| `qemu/0.42.0` | `platform/external/qemu` | `f0c183f1cc7456ecd6f3607f2f47893768ae4334` | 110 | windows / all | `0bb19e599a66c71d0ffbbbdadad52c1c12c9c5b1` | 按所改文件，见下文：73 个 GPL-2.0，23 个 Apache-2.0，14 个两者都有；0055 里的图片另见下文 |
| `qemu/mas41` | `platform/external/qemu` | `f0c183f1cc7456ecd6f3607f2f47893768ae4334` | 105 | windows / all | `60c619f9ecfe01ccde23907b04d92bdb3fb7643e` | 按所改文件，见下文：73 个 GPL-2.0，20 个 Apache-2.0，12 个两者都有；0055 里的图片另见下文 |
| `qemu/mas40` | `platform/external/qemu` | `f0c183f1cc7456ecd6f3607f2f47893768ae4334` | 101 | windows / all | `5d99d72b2b08f35249d66d929ee4277accd533f7` | 按所改文件，见下文：71 个 GPL-2.0，19 个 Apache-2.0，11 个两者都有；0055 里的图片另见下文 |
| `qemu/mas39` | `platform/external/qemu` | `f0c183f1cc7456ecd6f3607f2f47893768ae4334` | 98 | windows / all | `ac8b73bd8bb327cf875f4570d0c644825bc16eef` | 按所改文件，见下文：71 个 GPL-2.0，18 个 Apache-2.0，9 个两者都有；0055 里的图片另见下文 |
| `qemu/mas38` | `platform/external/qemu` | `f0c183f1cc7456ecd6f3607f2f47893768ae4334` | 95 | windows / all | `caab5653c46bdf480a5eb1dac15816bd00014711` | 按所改文件，见下文：68 个 GPL-2.0，18 个 Apache-2.0，9 个两者都有；0055 里的图片另见下文 |
| `qemu/mas37` | `platform/external/qemu` | `f0c183f1cc7456ecd6f3607f2f47893768ae4334` | 80 | windows / all | `3419edeb1ee3f69c4a12c4215aba032d3bee4c57` | 按所改文件，见下文：60 个 GPL-2.0，14 个 Apache-2.0，6 个两者都有；0055 里的图片另见下文 |
| `qemu/mas36` | `platform/external/qemu` | `f0c183f1cc7456ecd6f3607f2f47893768ae4334` | 65 | windows / all | `a46fc099df1403d982c601f2f53329e8332a2f6a` | 按所改文件，见下文：46 个 GPL-2.0，13 个 Apache-2.0，6 个两者都有；0055 里的图片另见下文 |
| `qemu/mas35` | `platform/external/qemu` | `f0c183f1cc7456ecd6f3607f2f47893768ae4334` | 60 | windows / all | `a1e36334f096ce5268a7df2b750f1a4a09b83c52` | 按所改文件，见下文：43 个 GPL-2.0，11 个 Apache-2.0，6 个两者都有；0055 里的图片另见下文 |
| `qemu/mas32` | `platform/external/qemu` | `f0c183f1cc7456ecd6f3607f2f47893768ae4334` | 57 | windows / all | `bc694d86d778732785a75364479fc03fad0250bc` | 按所改文件，见下文：42 个 GPL-2.0，11 个 Apache-2.0，4 个两者都有；0055 里的图片另见下文 |
| `qemu/fixq30` | `platform/external/qemu` | `f0c183f1cc7456ecd6f3607f2f47893768ae4334` | 55 | windows / all | `5aedc70819651ef386cef810afa6c2be3c028467` | 按所改文件，见下文：40 个 GPL-2.0，11 个 Apache-2.0，4 个两者都有；0055 里的图片另见下文 |
| `gfxstream/0.42.0` | `platform/hardware/google/gfxstream` | `04d0287b04f72d908c621a919fa2ea5c563b1a66` | 45 | windows / all | `baf0fd5600360307fe799083118d214dadf1b4be` | Apache-2.0 |
| `gfxstream/mas41` | `platform/hardware/google/gfxstream` | `04d0287b04f72d908c621a919fa2ea5c563b1a66` | 43 | windows / all | `32813e91da98b992967b78496b895f69e96a4bf3` | Apache-2.0 |
| `gfxstream/mas38` | `platform/hardware/google/gfxstream` | `04d0287b04f72d908c621a919fa2ea5c563b1a66` | 42 | windows / all | `eb1565574a7ab64457ae0f6794ad0cb6a8b76726` | Apache-2.0 |
| `gfxstream/mas37` | `platform/hardware/google/gfxstream` | `04d0287b04f72d908c621a919fa2ea5c563b1a66` | 25 | windows / all | `2afcb7b95a3011a3858ce0b813b215e0be21c1be` | Apache-2.0 |
| `gfxstream/mas35` | `platform/hardware/google/gfxstream` | `04d0287b04f72d908c621a919fa2ea5c563b1a66` | 23 | windows / all | `28437bb6c4b3fdef705d1919813467af465a718d` | Apache-2.0 |
| `gfxstream/fixq28` | `platform/hardware/google/gfxstream` | `04d0287b04f72d908c621a919fa2ea5c563b1a66` | 22 | windows / all | `d027890c4799a8537ce1e981fec784a99a269ade` | Apache-2.0 |
| `crashpad/module-version22` | `platform/external/crashpad` | `dd7db6790d73210abaef36a04f2a2bd52cf22b89` | 1 | windows | `377d2d398368c38d43f4ecbca084c89a68c10530` | Apache-2.0 |


## 平台判断和待实测

补丁平台按实际改动的文件标注。Windows 专用改动标为 `windows`，明确跨平台的改动标为 `all`；同一系列同时含两类时，目录表汇总为 `windows / all`。

以下判断仍需在对应平台实测后确认，先保留在清单中：
- QEMU 的 0035（气球空闲页上报）：非 Windows 走 QEMU 原有的 madvise 路径，HVF 下内存是否真的还给系统要测。
- QEMU 的 0060（宿主输入法的窗口文字模式）：本地事件过滤要补别的平台的实现。
- QEMU 的 0042、0043（CUDA 媒体解码）：只在有 CUDA 的平台上有意义。
- gfxstream 中涉及 Vulkan、GuestVulkanOnly 或 Vulkan 内存分配的补丁，先按 `all` 记录；mac 上底层是 MoltenVK，留待对应平台回归。

以后的补丁照现有习惯用标题前缀标平台：Windows 专用的用 `whpx:`、`file-win32:` 这类，mac 专用的用 `hvf:`、`macos:`；`incoming/<主题>` 的主题名也带上平台。

- `qemu/0.43.0` 的 0001–0110 与 `qemu/0.42.0` 是同一批提交（补丁文件头里的序号不同），0111–0116 是 0.43.0 新增的：最大化 / 还原按钮点过以后悬停高亮会消掉（ModAVD/ModAVD#214）（0111），精简窗口的标题栏加旋转按钮，在横屏和竖屏之间切换，窗口跟着转（ModAVD/ModAVD#225）（0112），开机时按 `hw.initialOrientation` 写回客体的 `user_rotation`、点 × 关机前先复位，按钮以初始方向为基准（0113），安卓开机完成前旋转按钮置灰，点击先写客体的 `user_rotation`、写成了才转窗口（0114），实例没有 `skin.name` 时自动生成的同尺寸皮肤保留 config 里的方向，竖屏实例的窗口一开始就是竖的（ModAVD/ModAVD#247）（0115），竖屏实例开机动画期间窗口先横着、动画结束再转成竖的，开机动画不再侧着（0116）。
- 0.43.0 的 gfxstream 补丁和已发布的 `gfxstream/0.42.0` 是同一批提交（基准、分支头都相同），沿用这个目录；0.43.0 清单注释里写的也是这个目录。
- 0.43.0 的 Crashpad 补丁和已发布的 `crashpad/module-version22` 是同一批提交（基准、分支头都相同），沿用这个目录；0.43.0 清单注释里写的也是这个目录。
- `qemu/0.42.0` 的 0001–0105 与 `qemu/mas41` 是同一批提交（补丁文件头里的序号不同），0106–0110 是 0.42.0 新增的：精简窗口的标题栏加截图按钮（存到图片文件夹，ModAVD/ModAVD#231）和音量按钮（只在开了声音时有，ModAVD/ModAVD#226）（0106），找到的 adb 协议版本都一样时不再每发一条命令都去问 adb server（0107），设媒体音量改走 `su`（0108），音量按钮改成唤起安卓系统自己的音量面板、不改音量，去掉 0106 自己画的音量条（0109），标题栏按钮去掉悬停提示、只留无障碍名称（0110）。
- `gfxstream/0.42.0` 的 0001–0043 与 `gfxstream/mas41` 是同一批提交（补丁文件头里的序号不同），0044–0045 是 0.42.0 新增的，来自外部贡献者的 ModAVD/ModAVD#209（收录自 `incoming/208-screenshot-cleanup`，保留原作者）：图形资源创建、释放失败时照样回收并分开记日志，补上截图读回的统计日志（0044），上下文绑定失败时只释放 CPU 侧的对象、不在别的上下文里删 GL 对象（0045）（ModAVD/ModAVD#208）。
- 0.42.0 的 Crashpad 补丁和已发布的 `crashpad/module-version22` 是同一批提交（基准、分支头都相同），沿用这个目录；0.42.0 清单注释里写的也是这个目录。
- `qemu/mas41` 的 0001–0101 与 `qemu/mas40` 是同一批提交（补丁文件头里的序号不同），0102–0105 是 mas-41 新增的：控制台命令 `event key <linux 键码> <0|1>`，按下、抬起模拟器键盘上的一个键（0102），截图共享没开起来时不再去注销没登记过的回调（0103），`screenrecord webrtc start` 失败时控制台回 KO、不让调用方空等（0104），精简窗口的标题栏加最大化 / 还原按钮（0105）。
- `gfxstream/mas41` 的 0001–0042 与 `gfxstream/mas38` 是同一批提交（补丁文件头里的序号不同），0043 是 mas-41 新增的：去掉一个 display 的回读回调时，这个 display 没登记过回调或回读线程没起来就直接返回，不再等一个不会来的应答（0043）。
- 0.41.0 的 Crashpad 补丁和已发布的 `crashpad/module-version22` 是同一批提交（基准、分支头都相同），沿用这个目录；mas-41 清单注释里写的也是这个目录。
- `qemu/mas40` 的 0001–0098 与 `qemu/mas39` 是同一批提交（补丁文件头里的序号不同），0099–0101 是 mas-40 新增的：带窗口的实例点标题栏就激活窗口、回到最前（0099），放开系统的最大化和贴边吸附、比例对不上时画面居中留黑边（0100），无头实例在 Windows 注销、重启、关机时先 sync、再让安卓关机，带窗口和无头共用抽出来的安全关机流程（0101）。
- 0.40.0 的 gfxstream 补丁和已发布的 `gfxstream/mas38` 是同一批提交（基准、分支头都相同），沿用这个目录；mas-40 清单注释里写的也是这个目录。
- 0.40.0 的 Crashpad 补丁和已发布的 `crashpad/module-version22` 是同一批提交（基准、分支头都相同），沿用这个目录；mas-40 清单注释里写的也是这个目录。
- `qemu/mas39` 的 0001–0095 与 `qemu/mas38` 是同一批提交（补丁文件头里的序号不同），0096–0098 是 mas-39 新增的：不用永久 cache 时，临时 cache 的同名 `.qcow2` 写层也登记进退出清理、正常退出时删掉（0096），Windows 上首尾相接的多段读写用原缓冲区一次读写、不复制（0097），`block/file-win32.c` 里那段说明跟着 0097 改写（0098）。
- 0.39.0 的 gfxstream 补丁和已发布的 `gfxstream/mas38` 是同一批提交（基准、分支头都相同），沿用这个目录；mas-39 清单注释里写的也是这个目录。
- 0.39.0 的 Crashpad 补丁和已发布的 `crashpad/module-version22` 是同一批提交（基准、分支头都相同），沿用这个目录；mas-39 清单注释里写的也是这个目录。
- `qemu/mas38` 的 0001–0080 与 `qemu/mas37` 是同一批提交（补丁文件头里的序号不同），0081–0095 是 mas-38 新增的：点 × 走安卓自己的关机，以及 Windows 结束会话、实例暂停时的关机处理（0081–0084），窗口标题用实例名、精简窗口自画标题栏（0085–0088），打开 gfxstream 的 ASTC / ETC2 模拟（0089），WC 内存默认按 cached 分配（0090），FAT32 / exFAT 首次建盘按实际需要检查空间（0091），媒体解码四处修复（0092–0095）。
- `gfxstream/mas38` 的 0001–0025 与 `gfxstream/mas37` 是同一批提交，0026–0042 是 mas-38 新增的上游 gfxstream 同步：按进程回收 GL 对象、死掉的客体进程不再让宿主一直等栅栏和信号量、ASTC / ETC2 在 sync2 屏障和仅传输队列上生效、WC 内存改用上游写法及其前置，以及几处小修复；从上游摘过来的提交保留原作者。
- 0.38.0 的 Crashpad 补丁与 0.37.0 逐字节相同，沿用 `crashpad/module-version22`；mas-38 清单注释里写的也是这个目录。
- `qemu/mas37` 的 0001–0065 与 `qemu/mas36` 是同一批提交（补丁文件头里的序号不同；0061 的邮件头少了 MIME 那几行，回放出来的提交相同），0066–0080 是 mas-37 新增的：`block/file-win32.c` 非缓存读写保持对齐、读出错如实报错（0066），数据盘转 qcow2 失败时保留原来的 ext4 照常开机（0067），宿主输入法一次提交整段送入（0068），精简工具栏和点 × 安全关机（0069–0080）。
- `gfxstream/mas37` 的 0001–0023 与 `gfxstream/mas35` 是同一批提交，0024、0025 是 mas-37 新增的：暂存缓冲按逻辑容量判断够不够用、同步读回画面失败时如实上报。
- 0.37.0 的 Crashpad 补丁与 0.36.0 相同（只差补丁文件头里的序号），沿用 `crashpad/module-version22`；mas-37 清单注释里写的 `patches/crashpad/mas37` 就是这个目录。
- `qemu/mas36` 的 0001–0060 与 `qemu/mas35` 是同一批提交（补丁文件头里的序号不同），0061–0065 是 mas-36 新增的：新建数据盘的 inode 数封顶、有开机标记 `modavd-writable-system` 时按可写系统镜像开机、数据盘改大不清数据、resize inode 按在线扩容的要求排布、`aio-win32` 注销句柄时不再读空指针。
- 0.36.0 的 gfxstream、Crashpad 补丁与 0.35.0 逐字节相同，沿用 `gfxstream/mas35`、`crashpad/module-version22`；两份 mas-36 清单的注释里写的 `patches/gfxstream/mas36`、`patches/crashpad/mas36` 就是这两个目录。
- `qemu/mas35` 的 0001–0057 与 `qemu/mas32` 是同一批提交（补丁文件头里的序号不同），0058–0060 是 mas-35 新增的宿主输入法：goldfish pipe 服务 `modavd_ime`、控制台命令 `ime`、窗口文字模式。
- `gfxstream/mas35` 的 0001–0022 与 `gfxstream/fixq28` 是同一批提交，0023 是 mas-35 新增的：暂存缓冲按需增长。
- `qemu/mas32` 的 0001–0055 与 `qemu/fixq30` 是同一批提交（补丁文件头里的序号不同），0056、0057 是 mas-32 新增的。
- 其余项目的确切提交见源码包里的 `manifest-aosp-base.xml`（全是 AOSP 公开提交）与 `manifest-<版本>.xml`（编译时各项目的 HEAD），本仓库的 `manifests/` 里也有。
- 「回放后的树」一栏是分支头提交的编号，它本身不在公开服务器上；回放后每个提交的树（`git rev-parse HEAD^{tree}`）与原分支逐个相同。
- qemu 补丁里有 CRLF 文件（`whp-dispatch.h` 等），`git am` 必须带 `--keep-cr`。
- 标题以 `MAS local:` / `MAS experiment:` 开头的是只给本项目用的改动（诊断日志、零外连、崩溃转储只留本机等），其余多为通用修复。
- 补丁作者是维护者 `qiyinxi <qiyinxi@foxmail.com>`、外部贡献者的 GitHub noreply 地址，以及从上游 gfxstream 摘过来的提交保留的原作者。

## qemu 补丁的许可证

每个补丁按它所改文件的许可证发布，与上游文件头一致：

- 改 QEMU 本体文件的（`target/`、`hw/`、`util/`、`slirp/`、`block/`、`exec.c`、`cpus.c` 等），按 GPL-2.0，与 QEMU 整体相同。其中几个文件自己带 MIT / BSD 文件头（`util/main-loop.c`、`util/oslib-win32.c`、`os-win32.c`、`cpus.c`、`hw/i386/pc.c`、`block/file-win32.c`、`slirp/*`），这些文件头保留不变。
- 改 `android/`、`android-qemu2-glue/` 下文件的，跟着该文件的文件头：带 GPL-2.0 文件头的按 GPL-2.0（这两个目录里有不少这样的文件），带 Apache-2.0 文件头的按 Apache License 2.0。没有文件头的文件按仓库 `LICENSE` 的说法，是 GPL-2.0 或更新版本。

| 许可证 | 补丁 |
| --- | --- |
| Apache-2.0 | 0002、0003、0008、0012、0036、0038、0042、0043、0050、0051、0052；mas36–0.43.0：0061、0064（只改 `android/third_party/ext4_utils/src/` 下的 `make_ext4fs.c`、`indirect.c`）；mas37–0.43.0：0068（只改 `ModavdImePipe.h`、`ModavdImePipe.cpp`）；mas38–0.43.0：0092–0095（只改 `android/emu/media/src/android/emulation/` 下的 `MediaVpxDecoderGeneric`、`MediaH264Decoder*`、`MediaHevcDecoderDefault`、`MediaTexturePool`、`MediaVpxDecoder`、`MediaSnapshotState`）；mas40–0.43.0：0099（只改 `windows-lean-frame.cpp`）；mas41–0.43.0：0103（只改 `VideoFrameSharer.cpp`、`VideoFrameSharer.h`）；0.42.0–0.43.0：0107（只改 `adb/interface` 下的 `TestAdbInterface.h`、`AdbInterface.cpp`、`AdbInterface_unittest.cpp`），0108（只改 `lean-title-bar-logic.cpp` 及其单元测试），0110（只改 `lean-title-bar.cpp`、`lean-title-bar.h`）；0.43.0：0111（只改 `windows-lean-frame`、`lean-window-frame` 及其单元测试） |
| 两者都有，按文件分 | 0005、0014（`EmulatorService.cpp` 为 Apache-2.0，`SharedMemory_win32.cpp` 为 GPL-2.0）；0016（`android-qemu2-glue/netsim/libslirp_driver.cpp` 为 Apache-2.0，`util/main-loop.c` 为 GPL-2.0）；0039（`CrashSytemInit.cpp` 为 Apache-2.0，`CrashUploadTool.cpp`、`CrashUploader.cpp` 为 GPL-2.0）；mas35–0.43.0：0058（新文件 `ModavdImePipe.h`、`ModavdImePipe.cpp` 为 Apache-2.0，`qemu-setup.cpp` 为 GPL-2.0，`android/emu/hardware/CMakeLists.txt` 没有文件头），0060（`native-event-filter-factory.cpp` 为 Apache-2.0，`emulator-qt-window`、`emulator-container`、`tool-window`、`shortcut-key-store` 为 GPL-2.0）；mas38–0.43.0：0083（`native-event-filter-factory.cpp` 为 Apache-2.0，`emulator-qt-window`、`tool-window` 为 GPL-2.0），0086（新文件 `lean-title-bar`、`windows-lean-frame`、`instance-window-title`、`lean-window-frame` 及其单元测试为 Apache-2.0，`emulator-qt-window`、`emulator-container`、`tool-window`、`windows-native-window`、`winsys-qt.cpp`、`winsys-headless.cpp`、`emulator-window.c`、`skin/window.c`、`skin/winsys.h` 为 GPL-2.0，两个 `CMakeLists.txt` 没有文件头），0088（`emulator-container.cpp` 为 GPL-2.0，`windows-lean-frame`、`lean-window-frame` 及其单元测试为 Apache-2.0）；mas40–0.43.0：0100（`emulator-container`、`emulator-qt-window`、`tool-window.cpp` 为 GPL-2.0，`windows-lean-frame`、`lean-window-frame` 及其单元测试为 Apache-2.0），0101（新文件 `emulator-safe-shutdown`、`headless-session-end.cpp`、`safe-shutdown` 及其单元测试为 Apache-2.0，`winsys-headless.cpp`、`emulator-qt-window` 为 GPL-2.0，三个 `CMakeLists.txt` 没有文件头）；mas41–0.43.0：0105（`emulator-container`、`tool-window.cpp`、`resources.qrc` 为 GPL-2.0，`lean-title-bar`、`windows-lean-frame`、`lean-window-frame` 及其单元测试为 Apache-2.0，两个新图标 `ic_window_maximize_white_48dp.svg`、`ic_window_restore_white_48dp.svg` 没有文件头）；0.42.0–0.43.0：0106（`emulator-qt-window`、`tool-window.cpp`、`aemu-ui-widgets` 的 `common.cpp`、`resources.qrc` 为 GPL-2.0，`lean-title-bar`、新文件 `lean-volume`、`lean-title-bar-logic` 及其单元测试为 Apache-2.0，两个 `CMakeLists.txt` 和新图标 `ic_volume_off_lean_16px.svg` 没有文件头，图标在 0109 又删掉了），0109（`resources.qrc` 为 GPL-2.0，`lean-title-bar`、`lean-volume`、`lean-title-bar-logic` 及其单元测试为 Apache-2.0，另删掉 0106 加的图标）；0.43.0：0112（`emulator-qt-window`、`tool-window` 为 GPL-2.0，`lean-rotate`（新文件）、`lean-title-bar`、`lean-title-bar-logic`、`lean-window-frame` 及其单元测试为 Apache-2.0，`aemu-ui-qt` 的 `CMakeLists.txt` 没有文件头），0113（`emulator-qt-window`、`tool-window.cpp` 为 GPL-2.0，`lean-rotate`、`lean-title-bar-logic`、`safe-shutdown` 及其单元测试为 Apache-2.0），0114（`tool-window` 为 GPL-2.0，`lean-rotate`、`lean-title-bar-logic` 及其单元测试为 Apache-2.0），0115（`skin/user-config.c` 为 GPL-2.0，新文件 `magic-skin-orientation.h` 和 `lean-title-bar-logic` 的单元测试为 Apache-2.0），0116（`emulator-qt-window.cpp`、`skin/user-config.c`、`skin/user-config.h` 为 GPL-2.0，`lean-rotate`、`lean-title-bar-logic`、`magic-skin-orientation.h` 及单元测试为 Apache-2.0） |
| GPL-2.0 | 其余的：qemu/fixq30 有 40 个，qemu/mas32 有 42 个（含 0055 的代码部分，见下；0056、0057 只改 `block/file-win32.c`），qemu/mas35 有 43 个（多出的 0059 只改 `console.cpp`），qemu/mas36 有 46 个（又多出 0062 `main-common.c`、0063 `android-qemu2-glue/main.cpp`、0065 `util/aio-win32.c`），qemu/mas37 有 60 个（又多出 0066 `block/file-win32.c`、0067 `android-qemu2-glue/main.cpp`，以及 0069–0080，只改 `android/android-ui/modules/aemu-ui-qt/src/android/skin/qt/` 下带 GPL-2.0 文件头的 `emulator-qt-window`、`emulator-container`、`tool-window`、`windows-native-window`），qemu/mas38 有 68 个（又多出 0081、0082、0084、0087 只改 `skin/qt/` 下带 GPL-2.0 文件头的 `emulator-qt-window`、`emulator-container`，0085 `emulator-window.c`，0089、0090 `android/android-emu/android/opengles.cpp`，0091 `android-qemu2-glue/main.cpp`），qemu/mas39 有 71 个（又多出 0096 只改 `android-qemu2-glue/main.cpp` 和带 GPL-2.0 文件头的 `android/android-emu-base/android/utils/tempfile.c`、`tempfile.h`，0097、0098 只改 `block/file-win32.c`），qemu/mas40 也是 71 个（新增的 0099–0101 都不只改 GPL-2.0 文件），qemu/mas41 有 73 个（又多出 0102、0104，只改 `android/android-emu/android/console.cpp`），qemu/0.42.0 也是 73 个（新增的 0106–0110 都不只改 GPL-2.0 文件），qemu/0.43.0 也是 73 个（新增的 0111–0116 都不只改 GPL-2.0 文件） |

这张表是按编译时提交上各文件开头的许可证声明逐个归出来的（0001–0054 按 `fac53907`，0055 按 `5aedc708`，0056–0057 按 `bc694d86`，0058–0060 按 `a1e36334`，0061–0065 按 `a46fc099`，0066–0080 按 `3419edeb`，0081–0095 按 `caab5653`，0096–0098 按 `ac8b73bd`，0099–0101 按 `5d99d72b`，0102–0105 按 `60c619f9`，0106–0110 按 `0bb19e59`，0111–0116 按 `30139c40`）。

**0055（换成 ModAVD 图标）**：改的是顶层 `CMakeLists.txt`（没有文件头，按仓库 `LICENSE` 属 GPL-2.0 或更新版本）和 `android/android-ui/modules/aemu-ui-window/include/android/icons.h`（GPL-2.0 文件头），这些代码改动按 GPL-2.0。补丁里的图片——`android/images/emulator_icon.ico`、`emulator_icon_32/128/256.png`，以及 `icons.h` 里由这些图片转成的数据——是 ModAVD 自己的图标；**这些图片不随 GPL-2.0 或 Apache-2.0 授权**，见仓库 README 的「图标与美术」。

回放示例（以 0.43.0 的 qemu 为例）：

```sh
git clone https://android.googlesource.com/platform/external/qemu
cd qemu
git checkout -b 0.43.0 f0c183f1cc7456ecd6f3607f2f47893768ae4334
git am --keep-cr ../patches/qemu/0.43.0/*.patch
```
