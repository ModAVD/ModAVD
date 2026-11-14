# #208 回归与截图日志

补丁基于 `ModAVDGfxstream` 的 `release/0.40` 提交 `13ca2b2565ef504cb142a9b95bc8e5f9d6d99580`。其源码树与本仓库 `gfxstream/mas38` 的 42 个补丁重放结果相同，树为 `33c4220ef1da2f4772788c6af4d37deb9f34c850`。

## 跑回归

在应用补丁后的 gfxstream 源码上运行：

```text
python incoming/208-screenshot-cleanup/tests/run.py --repo <gfxstream源码目录> --work <仓库外的输出目录>
```

需要 Python 3.9 或更新版本和 C++17 编译器。Windows 默认通过 `vswhere` 查找 Visual Studio C++ 工具，也可用 `CXX` 指定编译器。脚本从源码提取函数体，配上可注入失败的 WGL、EGL 和 GL 接口后编译运行，输出 `results.json`。无需模拟器、Android 镜像、GPU 或第三方 Python 包。

三个探针共 15 项检查：

- WGL：释放失败后保留 DC；1000 次正常使用保持池大小；1000 次释放失败后销毁原生对象；销毁失败保留并重试；失效句柄回收 CPU 记录；原生创建失败；首次获取 DC 失败。
- ColorBuffer：正常释放；多次绑定失败后恢复且只释放一次；1000 个待清理对象恢复；helper 销毁时释放 CPU 对象且不调用错误上下文的 GL；实际 `readPixels` 的容量检查、RGBA 和 RGB 转换路径。
- 日志：普通客体读回与 DMA 各 10000 次；宿主尺寸查询与截图分别计数；八线程统计、汇总节流、资源计数和待清理计数。

`--only wgl` 可用于未修复的基线，预期失败在释放失败后 DC 已被错误清空的断言。其余两个探针需要新加的清理结构和日志头文件。

这些检查验证生产函数体在受控失败下的行为。接口由测试替代，不验证 NVIDIA 或 Intel 驱动，也不验证真实游戏、像素输出和内存增长速度。

## 读日志

`MAS: readback source=...` 按来源输出进程启动以来的累计次数、字节数、失败数、总耗时、最大耗时，以及最后一次尺寸、ColorBuffer 句柄和图形客户端进程标识 `puid`。通常每分钟汇总，关闭 FrameBuffer 时再汇总一次。失败单独立即记录。

| source | 统计入口 |
| --- | --- |
| `guest` | `rcReadColorBuffer`，含客体普通图形缓冲区读回 |
| `guest-dma` | `rcReadColorBufferDMA` |
| `host-screenshot` | 宿主 `getScreenshot` 的像素请求 |
| `host-size-query` | 同一接口的尺寸查询，不把正常的容量查询返回值算成失败 |
| `host-frame-submit` | 宿主 onPost 回调的帧提交尝试，不表示共享内存消费者实际读了几次 |
| `gl-pixels` | `ColorBufferGl::readPixels` |
| `gl-scaled` | `ColorBufferGl::readPixelsScaled` |

客体入口也可被截图以外的图形缓冲区读回调用。一次上层请求可能同时增加 GL 层计数，不能把各来源相加当成截图总次数。客体接口没有传回底层成功状态，`failures` 只统计该层可见的失败；GL 层也不额外调用 `glGetError`，以免消耗调用者的错误状态。

`bytes` 是请求或读回的累计流量，不是内存占用。尺寸查询的字节数是所需容量；GL 缩放路径记录输出字节数，不包含临时 RGBA 转换缓冲区。

`MAS: readback resources` 记录 ColorBuffer 和 RenderThread 的创建、释放累计数，以及延迟清理、恢复清理和放弃 GL 调用的数量。`cbRgbaEstimateBytes` 是仍存活的 ColorBuffer 按宽×高×4 的粗估，`pendingRgbaEstimateBytes` 是已退出但等待原上下文清理的粗估。它们不包含驱动缓存、共享纹理、FBO 或额外副本，也不是实测分配量。`cleanupAbandoned` 表示 helper 销毁时只回收了 CPU 所有权，不能据此宣称驱动内存已释放。

`MAS: native pbuffers` 记录原生 pbuffer 的创建、销毁、使用中、池内空闲、等待销毁和失败次数。`created - destroyed - invalid` 应与 `live + free + pending` 对应。已知失效句柄计入 `invalid`，不计作成功销毁。释放、重新获取 DC 或销毁失败时同时输出原生句柄与 Win32 错误码。原有 Intel 延后销毁时机保留，创建路径不直接销毁旧 pbuffer。

`destroyed` 表示原生销毁调用成功，驱动可能等解除上下文后才回收资源。WGL 返回值和销毁语义依据 [WGL_ARB_pbuffer 规范](https://registry.khronos.org/OpenGL/extensions/ARB/WGL_ARB_pbuffer.txt)。

现场对照同一时间段的进程 Private Bytes 与这些累计量的增量。先看增长时究竟增加了哪条读回计数，再看 `pending`、`cleanupDeferred - cleanupRecovered - cleanupAbandoned` 或资源创建与释放差值是否持续增加。计数稳定而进程内存继续增长时，需要继续采集分配栈，不能用这些日志认定为截图泄漏。
