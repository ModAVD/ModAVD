# #208 回归与截图日志

补丁基于 `ModAVDGfxstream` 的 `release/0.40` 提交 `13ca2b2565ef504cb142a9b95bc8e5f9d6d99580`。其源码树与本仓库 `gfxstream/mas38` 的 42 个补丁重放结果相同，树为 `33c4220ef1da2f4772788c6af4d37deb9f34c850`。

## 跑回归

在应用补丁后的 gfxstream 源码上运行：

```text
python incoming/208-screenshot-cleanup/tests/run.py --repo <gfxstream源码目录> --work <仓库外的输出目录>
```

需要 Python 3.9 或更新版本和 C++17 编译器。Windows 默认通过 `vswhere` 查找 Visual Studio C++ 工具，也可用 `CXX` 指定编译器。脚本从源码提取函数体，配上可注入失败的 WGL、EGL 和 GL 接口后编译运行，输出 `results.json`。无需模拟器、Android 镜像、GPU 或第三方 Python 包。

三个探针共 29 项检查：

- WGL 共 12 项：原有七项释放和创建失败回归；持续销毁失败下的分配预算、轮转重试、延时和日志限速；首次获取 DC 持续失败的积压上限；已有大量存活对象时不丢失所有权；汇总只输出变化。
- 清理共 10 项：原有五项 ColorBuffer 回归；空 DisplaySurface 析构不绑定；有积压时只绑定一次并先清理再销毁；绑定失败后只回收 CPU 对象；同 helper 和不同 helper 的嵌套绑定均不扰乱外层 GL 状态。
- 读回与日志共七项：普通与 DMA 各 10000 次；实际 FrameBuffer 函数链传递格式、句柄、容量和后端失败；宿主查询与截图分别计数；八线程统计；两万次失败限速且不丢计数；未变更汇总保持安静；日志回调可重入统计接口。

`--only wgl` 可用于未修复的基线，预期失败在释放失败后 DC 已被错误清空的断言。其余两个探针需要新加的清理结构和日志头文件。

清理探针也提取实际的 DisplaySurface 析构函数，诊断探针提取两层 FrameBuffer 读回函数。使用可控时钟验证分钟窗口和一秒重试延时，不靠等待或机器速度判断。原 PR 版本在“空析构不应绑定”的新断言上失败；只移除嵌套保护时，外层 GL 状态回归也会失败。

这些检查验证生产函数体在受控失败下的行为。接口由测试替代，不验证 NVIDIA 或 Intel 驱动，也不验证真实游戏、像素输出和内存增长速度。

## 读日志

`MAS: readback source=...` 按来源输出进程启动以来的累计次数、字节数、失败数、总耗时、最大耗时，以及最后一次尺寸、ColorBuffer 句柄和图形客户端进程标识 `puid`。每分钟只汇总发生变化的来源和资源计数，关闭 FrameBuffer 时刷新尚未输出的变化。各来源每分钟的前三次失败立即记录详细信息，其余失败保留在累计计数中。失败详情和汇总均在释放统计锁后输出。

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

`MAS: native pbuffers` 记录原生 pbuffer 的创建、销毁、使用中、池内空闲、等待销毁和失败次数。`created - destroyed - invalid` 应与 `live + free + pending` 对应。已知失效句柄计入 `invalid`，不计作成功销毁。原生 WGL 失败详情合计每分钟最多三条，保留原生句柄与 Win32 错误码，之后通过累计量和最后的销毁错误判断变化。WGL 汇总也只输出变化。原有 Intel 延后销毁时机保留，创建路径不直接销毁旧 pbuffer。

每次释放最多检查八个待销毁对象，轮转处理，销毁失败后至少一秒才重试该对象。一旦出现待销毁对象，就把允许保有的原生对象总数冻结为 `max(64, 当时已存在的对象数)`；预算耗尽时拒绝新增原生分配，健康的空闲对象仍可复用。`allocationDenials` 是因预算拒绝的原生分配尝试数，包含预热池扩容被拒绝的尝试；`allocationLimit` 是当前预算，待销毁队列清空后变回 0 并解除限制。已有存活对象以后退出也计入这份预算，不丢弃句柄来凑上限。若驱动持续失败且没有存活 surface 再触发安全释放，可能需要重启渲染器恢复创建。

`destroyed` 表示原生销毁调用成功，驱动可能等解除上下文后才回收资源。WGL 返回值和销毁语义依据 [WGL_ARB_pbuffer 规范](https://registry.khronos.org/OpenGL/extensions/ARB/WGL_ARB_pbuffer.txt)。

现场对照同一时间段的进程 Private Bytes 与这些累计量的增量。先看增长时究竟增加了哪条读回计数，再看 `pending`、`cleanupDeferred - cleanupRecovered - cleanupAbandoned` 或资源创建与释放差值是否持续增加。计数稳定而进程内存继续增长时，需要继续采集分配栈，不能用这些日志认定为截图泄漏。
