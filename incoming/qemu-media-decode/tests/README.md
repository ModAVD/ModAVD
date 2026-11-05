# mas37 媒体解码验证

QEMU `release/0.37` 基线为 `f3bfb151cd25bda723e9d7efad30a767f089c926`。它的树 `7a669c8b6f6c4cab7a0fdb5413977e8dd55a32cc` 与主仓 mas37 的 80 个补丁回放结果相同，已含 0050–0052。四个 incoming 补丁用 `git am --keep-cr` 打上后，树为 `a2303de43e7714259e468cedfa6640ebca77b63a`。

## 定向对照

在 Windows x64 的 VS 2022 17.14 开发环境运行。两个版本共用 mas37 清单的 AEMU、gfxstream、Abseil、libvpx、FFmpeg、Clang 和 Windows SDK。由 FFmpeg 7.1 生成一次素材，两个程序读取同一目录。

```powershell
python fixtures.py $clips --ffmpeg $ffmpeg
python run.py --src $src --qemu $original --out $before (Join-Path $clips sample.ivf)
python run.py --src $src --qemu $patched --out $after (Join-Path $clips sample.ivf)
```

测试调用实际 codec manager 的 `InitContext`、`DecodeImage`、`GetImage`、`save` 和 `load`。CUDA helper 和 GL 回调由测试替换，以便强制初始化失败、计数纹理的创建与删除。软件解码分别执行 mas37 的 libvpx 和 FFmpeg。

| 用例 | 基线 | 补丁后 |
| --- | --- | --- |
| 默认 VP9 软件解码 | 24 帧，FNV-1a `8ce6a0f3` | 相同 |
| H.264 软件解码，驱动探测返回不可用 | 24 帧，FNV-1a `878198d7` | 相同 |
| VP9 协议 200，host GLES，可用驱动，CUDA helper 的 `init()` 返回 false | 访问异常 `0xc0000005` | 软解 24 帧，PTS 按序，像素与默认 VP9 相同 |
| 快照 VP8 / VP9 各保存 1 / 3 个包 | 分别提交 2 / 6 次 | 1 / 3 次，每个 PTS 都与回放队列一致 |
| VP9、H.264、HEVC 各以未取帧的会话保存，替换并恢复 10 次，最后销毁 | 每种遗留 11 个 helper、176 个纹理名 | 每种均为 0 |
| 依次保存纹理帧与 I420 字节帧，再读回 | 无像素的旧纹理帧排在队首 | 首帧为有效 I420，字节与 PTS 正确，取完队列为空 |

## Windows 实机

本轮完整构建在配置阶段缺少 mas37 Qt 的 `windows_msvc-x86_64/bin/uic.exe`。以下均为 **修复前发行版**，补丁后的完整模拟器仍需按同样方法复测。

Windows 11、RTX 4060 Ti（驱动 610.88）、WHPX、独立 SDK 与 Android 34 Google APIs x86_64 rev.14 AVD，`-gpu host`。素材为 128×96、12 fps、24 帧的同一段 `testsrc2`。VP9 IVF 的 SHA-256 为 `2dcf8292a276390e6b5ae476228c6a5335fc14fda28ac0db845c3043641ba358`，H.264 Annex B 为 `a4c9228113a2a8464d96c4728862c60373035de368b53e19ba802c7c00010bd3`。客体读取无重编码封装的 WebM、MP4。

| 配置 | 实际结果 |
| --- | --- |
| 默认冷启动，`-no-snapshot`，不设 CUDA VPX 开关 | `c2.goldfish.h264.decoder` 和 `c2.goldfish.vp9.decoder` 均输出 24 帧至 EOS。Y/U/V 可见像素累计 CRC32 分别为 `83b1ee5f`、`50ec843b` |
| 开 CUDA VPX，进程环境设 `CUDA_VISIBLE_DEVICES=-1` | 驱动探针返回 `cuInit=100`，模拟器记录 `Failed to call cuInit`，VP9 仍输出 24 帧且 CRC32 相同。该发行版实机路径没有复现访问异常。上表的定向测试单独强制到了 helper 初始化失败且纹理偏好已开启的路径 |
| 开 CUDA VPX，允许快照，同一 VP9 每帧等待 800 ms | 不加载时 24 帧至 EOS，CRC32 为 `50ec843b`。在第 2 帧时请求保存，第 4 帧时请求加载，恢复后仅 4 帧便报告 EOS。复现了恢复后丢失后续输出，第 4 项所报的持续挂起在这次表现为提前结束，原因仍待定位 |
| 对同一活动解码快照连续加载 10 次 | 先做上行的一次恢复，再采第 0 组。进程 GPU Local Usage 的组三次采样中位数从 167.18 MiB 到第 1 次 275.07 MiB、第 10 次 282.93 MiB。中间有升有降，本次未复现每次增长数百 MB |

`mas37-local-usage.csv` 保存每个采样点的字节数。基线与补丁均应使用相同驱动、素材、启动参数、帧延时、预热与采样方法。运行时检查其他 QEMU、游戏启动和 GPU 压测，遇到竞争则另起一轮。

### 复测方法

1. 对每个构建使用各自的 SDK、AVD 和独立 adb 端口。以 `fixtures.py` 生成的同一份素材运行两版。用 Android SDK 的 `javac --release 8 -cp <android.jar> -d <classes> DecodeClip.java` 和 `d8 --lib <android.jar> --min-api 26 --output clip.jar <classes>/DecodeClip.class` 生成客体测试程序。
2. 用私有 adb 端口和明确的 serial 把 `clip.jar`、`sample.mp4` 和 `sample.webm` 推到新 AVD 的 `/data/local/tmp/m43/`，将 jar 设为只读。冷启动时，在客体 shell 运行下列命令。比较每一帧的 PTS、CRC 和最后的 EOS。
   ```sh
   cd /data/local/tmp/m43
   CLASSPATH=$PWD/clip.jar app_process / DecodeClip "$PWD/sample.mp4" c2.goldfish.h264.decoder
   CLASSPATH=$PWD/clip.jar app_process / DecodeClip "$PWD/sample.webm" c2.goldfish.vp9.decoder
   ```
3. 验证 CUDA 回落时，在新模拟器进程设置 `ANDROID_EMU_MEDIA_DECODER_CUDA_VPX=1`。用调试器确认 VPX parser 为 200、`mUseGpuTexture` 为 true，强制 `MediaCudaVideoHelper::init()` 返回 false，核实 `getImage()` 将带像素的帧逐帧输出到 EOS。
4. 验证快照时恢复正常驱动，开启 CUDA VPX，改用 `-no-snapshot-load -no-snapshot-save` 允许手动快照。在客体中后台运行同一 VP9 命令，加最后一个参数 `800` 并把输出重定向到客体文件，保证 adb 重连后仍能读取进度。以私有 adb 执行 `emu avd snapshot save <name>`，预热加载一次，检查返回 `OK` 及客体输出。
5. 用 `sample-local-usage.ps1 -Adb $adb -ServerPort $port -Serial $serial -QemuPid $qemuPid -Snapshot $name -Csv $newCsv` 采第 0 组及 10 次加载。每组等待 3 秒，再以 1 秒间隔读取目标 PID 的三次 WDDM `GPUProcessMemory.LocalUsage`，比较中位数和完整曲线。该脚本适用于该进程只有一个 GPU adapter 的配置。记录恢复后是否继续出帧，最后先在客体 `sync` 再用私有 adb `emu kill`。

目前不能保证加载快照后正在解码的客体会话能播到片尾。
