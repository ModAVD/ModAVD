# gate：把 arm64 执行桥的载荷放进 SDK

执行桥是什么、为什么要它、怎么装进实例，见 [bridge/README.md](../../bridge/README.md)。这里只管发版时的一步：
把桥的三个载荷文件放进 SDK 树，让它们随 `sdk\emulator` 一起进包。

| 文件 | 来源 | 装到实例的 | SELinux 标签 |
| --- | --- | --- | --- |
| `libmodavd_bridge.so` | `bridge\nbshim\`，`bridge\scripts\build-nbshim.ps1` 编 | `/system/lib64/` | `u:object_r:system_lib_file:s0` |
| `libmodavd_gate.so` | `bridge\companion\`，`bridge\scripts\build-companion.ps1` 编 | `/system/lib64/` | `u:object_r:system_lib_file:s0` |
| `modavd_bridge.rc` | `bridge\nbshim\modavd_bridge.rc` 原文件 | `/system/etc/init/` | `u:object_r:system_file:s0` |

另外往 `/vendor/build.prop` 追加一行 `ro.dalvik.vm.native.bridge=libmodavd_bridge.so`。三个文件都是 `chmod 644`。

## 包里在哪里

```
<ModAVD 根>\sdk\emulator\resources\modavd\libmodavd_bridge.so
<ModAVD 根>\sdk\emulator\resources\modavd\libmodavd_gate.so
<ModAVD 根>\sdk\emulator\resources\modavd\modavd_bridge.rc
<ModAVD 根>\tools\modavd-bridge.ps1                            装 / 卸 / 核对（源码在 package\）
```

放在 `sdk\emulator` 下面：内测阶段「只换 `sdk\emulator` 文件夹」的升级方式会自动带上它们，更新器也已经整个托管
`sdk/emulator/**`，增量更新不用加规则；载荷和模拟器锁在同一个构建号上，不会出现「新模拟器 + 旧伴生库」。

> 没放进 `components\`（宿主输入法的位置）：那里的规则只认 `*.apk`，要改更新器、`release_tools.py` 的 `component_apks()`、
> `release.ps1` 的参数和单测，而且两个文件分开更新会错配。

## 怎么进包

1. **编载荷，固定哈希。** 载荷不进 git（源码在仓库里，产物不进）。要 Android NDK r27c：

   ```powershell
   $env:ANDROID_NDK_ROOT = '<NDK r27c 的目录>'
   pwsh -File bridge\scripts\build-nbshim.ps1 [-OutDir <目录>]
   pwsh -File bridge\scripts\build-companion.ps1 [-OutDir <目录>]
   ```

   `stage-payload.ps1` 按这个顺序找载荷：`-Payload <已冻结的目录>`；`-Bridge` / `-Gate`；本机配置的 `GatePayload`，
   再是 `BridgeSo` / `GateSo`（`tools\local.example.psd1`）；最后是上面两个脚本不带 `-OutDir` 时的输出位置。
   `.rc` 默认取本仓库的 `bridge\nbshim\modavd_bridge.rc`。
   同一份源码放在不同目录编，产物只有 `.note.gnu.build-id` 不同，所以哈希跟着编译目录走：换了目录或重编过，
   就跑一次 `-UpdateSums` 重新固定，这次换二进制会出现在 git diff 里。
2. **放进 SDK 树。** `tools\build\build-sdk.ps1` 编完会自动做；也可以对一棵已有的 SDK 树单独跑。

```powershell
# 只核对载荷（不需要 SDK 树）
pwsh -NoProfile -File tools\gate\stage-payload.ps1 -CheckOnly

# 重编过载荷：重新固定哈希
pwsh -NoProfile -File tools\gate\stage-payload.ps1 -UpdateSums

# 放进 SDK 树（build-sdk.ps1 自动调用；-Payload 可以指一个已冻结的目录）
pwsh -NoProfile -File tools\gate\stage-payload.ps1 -Sdk <SDK 根目录>
```

- 哈希固定在 `payload.SHA256SUMS.txt`，格式和 `tools\pubrel\config\components.SHA256SUMS.txt` 一样
  （`<小写 sha256><两个空格><resources/modavd/文件名>`，可以直接 `sha256sum -c`）。**哈希对不上就不放、不打包。**
- `build-sdk.ps1` 在 `ShouldProcess` **之前**就跑一次 `-CheckOnly`（只读），`-WhatIf` 也会验：
  免得编一个多小时之后才发现载荷没编或没重新固定。
- 发版 `assemble` 对 `tools\pubrel\config\sdk-inventory.csv` 是**全等判定**：少一个文件就 `SDK inventory changed`
  直接停，所以漏放载荷不会静默发出去，但 CSV 里那三行也必须在。

## 诊断

日志 tag：shim 是 `ModAvdNB`，伴生库是 `ModAvdGate`。冷启动后从零抓最准：

```powershell
adb -P <私有 adb 端口> -s emulator-<端口> logcat -b all -d -s ModAvdNB:V ModAvdGate:V
```

shim 的关键行：

- `shim-ctor OK version=<6|8> wrapped=...`：转发已就绪（`modavd-bridge.ps1 verify` 就看这一行）。
- `shim-ctor real itf=... version=<n> copy_bytes=<144|160|168>`：找到真结构体，版本 n。
- `shim-ctor companion libmodavd_gate.so loaded handle=...`：伴生库也起来了。
- `shim-ctor companion dlopen(...) failed -> ... (bridge still up, gate absent)`：伴生库不在。桥的入口在、执行桥不工作，
  游戏照旧崩，但 arm64 还是好的。
- `shim-ctor FAILED` / `shim-ctor INERT`：真翻译层没打开。这时 `version` 保持 0，`libnativebridge` 在 ART 调 `initialize`
  之前干净地放弃：设备正常开机、x86_64 全好，只是没有 arm64。

可选属性（都不设也能跑，都不持久）：

| 属性 | 默认 | 作用 |
| --- | --- | --- |
| `modavd.nb.log` | 开 | 设 `0` / `false` / `off` 关掉 shim 的逐调用日志；构造函数的日志永远打 |
| `modavd.nb.real` | `libndk_translation.so` | 真翻译层的库名或路径 |
| `modavd.nb.companion` | `libmodavd_gate.so` | 设 `0` / `off` 不加载伴生库（只留转发） |
| `modavd.gate.target` | 编进伴生库的名单 | 设了就整体替换名单；`none` 等于关掉 |
| `modavd.gate.slotstrict` | `0` | 槽校验改成强制。只作诊断，别当修法 |
| `modavd.gate.logsig` / `probe` / `revprobe` | `0` | 信号路径逐点日志 / 只观察不桥接 |
