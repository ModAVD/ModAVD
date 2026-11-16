# #208 回归与截图日志

补丁基于 `ModAVDGfxstream` 的 `release/0.41` 提交 `090f307d7e4aa8d98b90323d5b50c322f47982f7`。其源码树与本仓库 `gfxstream/mas41` 的 43 个补丁重放结果相同，树为 `864ade0e26d1bd9380ce2b49210eec665837c6b3`。

## 跑回归

在应用补丁后的 gfxstream 源码上运行：

```text
python incoming/208-screenshot-cleanup/tests/run.py --repo <gfxstream源码目录> --work <仓库外的输出目录>
```

需要 Python 3.9 或更新版本和 C++17 编译器。Windows 默认通过 `vswhere` 查找 Visual Studio C++ 工具，也可用 `CXX` 指定编译器。脚本从源码提取函数体，配上可注入失败的 WGL、EGL 和 GL 接口后编译运行，输出 `results.json`。无需模拟器、Android 镜像、GPU 或第三方 Python 包。

三个探针共 37 项检查：

- WGL 共 17 项：释放状态、正常池复用、失败回收、空池与失效句柄；大池单次刷新失败和补池首次取 DC 失败仍能创建；队列满时健康空闲对象可复用；轮转与延时；HRESULT 包装错误码；重试上限、无存活对象时的过期恢复、队列溢出记账；按原因限速和只输出变化。
- 清理共 11 项：ColorBuffer 正常、恢复和终止清理；空 DisplaySurface 析构不绑定，有积压时恢复；同 helper 和不同 helper 的嵌套状态；读回容量与 RGB 转换；实际延后清理日志限速且计数不丢。
- 读回与日志共九项：普通和 DMA 调用、FrameBuffer 后端失败与容量传递、宿主查询与截图、并发统计、失败详情限速、只输出变化、回调重入、保留 DMA 旧告警及清理事件的独立额度。

`--only wgl` 可用于未修复的基线，预期失败在释放失败后 DC 已被错误清空的断言。其余两个探针需要新加的清理结构和日志头文件。复审发现的两处创建回归可单独运行 `--only wgl --case pool-fallback` 和 `--only wgl --case initial-dc`；它们在 `4dc6ffd` 版本上分别失败，在修订版本上通过。

清理探针也提取实际的 DisplaySurface 析构函数，诊断探针提取两层 FrameBuffer 读回函数。使用可控时钟验证分钟窗口和一秒重试延时，不靠等待或机器速度判断。原 PR 版本在“空析构不应绑定”的新断言上失败；只移除嵌套保护时，外层 GL 状态回归也会失败。

这些检查验证生产函数体在受控失败下的行为。接口由测试替代，不验证 NVIDIA 或 Intel 驱动，也不验证真实游戏、像素输出和内存增长速度。

## 读日志

`MAS: readback source=...` 按来源输出进程启动以来的累计次数、字节数、失败数、总耗时、最大耗时，以及最后一次尺寸、ColorBuffer 句柄和图形客户端进程标识 `puid`。每分钟只汇总发生变化的来源和资源计数，关闭 FrameBuffer 时刷新尚未输出的变化。各来源每分钟的前三次失败立即记录详细信息，其余失败保留在累计计数中。ColorBuffer 延后清理、恢复和放弃分别有每分钟三条额度；DMA 的旧 `framebuffer cannot be found!` 告警保留并单独限速。失败详情、清理日志和汇总均在释放统计锁后输出。

| source | 统计入口 |
| --- | --- |
| `guest` | `rcReadColorBuffer`，含客体普通图形缓冲区读回 |
| `guest-dma` | `rcReadColorBufferDMA` |
| `host-screenshot` | 宿主 `getScreenshot` 的像素请求 |
| `host-size-query` | 同一接口的尺寸查询，不把正常的容量查询返回值算成失败 |
| `host-frame-submit` | 宿主 onPost 回调的帧提交尝试，不表示共享内存消费者实际读了几次 |
| `gl-pixels` | `ColorBufferGl::readPixels` |
| `gl-scaled` | `ColorBufferGl::readPixelsScaled` |

客体入口也可被截图以外的图形缓冲区读回调用。一次上层请求可能同时增加 GL 层计数，不能把各来源相加当成截图总次数。FrameBuffer 的内部 C++ 读回接口返回后端结果，并保留调用者给出的输出容量；客体普通与 DMA 的 `failures` 包含格式错误、失效句柄、容量不足和后端失败。客体协议和 DMA 现有返回约定保持原样。GL 层不额外调用 `glGetError`，以免消耗调用者的错误状态。

`bytes` 是请求或读回的累计流量，不是内存占用。尺寸查询的字节数是所需容量；GL 缩放路径记录输出字节数，不包含临时 RGBA 转换缓冲区。

`MAS: readback resources` 记录 ColorBuffer 和 RenderThread 的创建、释放累计数，以及延迟清理、恢复清理和放弃 GL 调用的数量。`cbRgbaEstimateBytes` 是仍存活的 ColorBuffer 按宽×高×4 的粗估，`pendingRgbaEstimateBytes` 是已退出但等待原上下文清理的粗估。它们不包含驱动缓存、共享纹理、FBO 或额外副本，也不是实测分配量。`cleanupAbandoned` 表示 helper 销毁时只回收了 CPU 所有权，不能据此宣称驱动内存已释放。

`MAS: native pbuffers` 记录原生 pbuffer 的创建、销毁、使用中、池内空闲、待销毁、失效和放弃数量。`created - destroyed - invalid - abandoned` 应与 `live + free + pending` 对应。`abandoned` 是只释放了 CPU 记录、未确认销毁的对象数，可能对应驱动资源泄漏，不计作成功销毁。原生取 DC、释放 DC、创建、销毁、分配暂停和放弃清理各有独立的每分钟三条详情额度，销毁失败不会挤掉分配暂停的详情。汇总只输出变化。

创建先尝试池内的所有空闲对象，耗尽后才补池。一次请求最多进行一批补池尝试；原生分配失败才终止，首次取 DC 失败则继续下一项。待销毁对象不计入正常存活或空闲对象的分配预算。只有待销毁队列达到 64 项时才暂停新增原生分配，健康空闲对象仍可复用。`allocationDenials` 记录这类暂停次数，`pendingLimit` 为队列上限。

每次释放最多检查八个待销毁对象，轮转处理，失败后至少一秒再重试。按 [HRESULT_CODE](https://learn.microsoft.com/en-us/windows/win32/api/winerror/nf-winerror-hresult_code) 提取错误码，裸值 6 和 `0xC0070006` 都按失效句柄处理。单个对象最多尝试八次、最多等待 60 秒；达到上限或队列溢出时，回收 CPU 记录并增加 `abandoned`，日志注明可能的原生泄漏。无存活 surface 再触发释放时，创建路径也能清除已过期的 CPU 记录，使恢复后的驱动重新分配；这一过程不调用原生销毁。

原有 Intel 延后销毁时机保留，创建路径不直接销毁旧 pbuffer。计数上限与超时不等于驱动资源已经释放，需要结合进程内存和分配栈判断。

`destroyed` 表示原生销毁调用成功，驱动可能等解除上下文后才回收资源。WGL 返回值和销毁语义依据 [WGL_ARB_pbuffer 规范](https://registry.khronos.org/OpenGL/extensions/ARB/WGL_ARB_pbuffer.txt)。

现场对照同一时间段的进程 Private Bytes 与这些累计量的增量。先看增长时究竟增加了哪条读回计数，再看 `pending`、`cleanupDeferred - cleanupRecovered - cleanupAbandoned` 或资源创建与释放差值是否持续增加。计数稳定而进程内存继续增长时，需要继续采集分配栈，不能用这些日志认定为截图泄漏。
