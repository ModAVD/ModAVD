# 升级

升级就是关掉所有实例后，把新的 Release 包整个解压到旧的根目录（例如 `D:\ModAVD`），覆盖同名文件。实例、游戏和登录状态都在 `avd\` 里，新包里没有这个文件夹，不会被动到。

0.36.0 起包根多了 `ModAVD.exe`（管理器）和 `tools\` 里的 `modavd.exe`（命令行）、`modavd-bridge.ps1`，`components\` 里多了宿主输入法 `modavd-ime.apk`。只换 `sdk\emulator\` 会少了它们。

## 哪些会换，哪些不动

| 路径 | 升级时 |
|---|---|
| `sdk\emulator\` | 模拟器本体，用新包里的覆盖。覆盖不会删掉新版拿掉的文件；想顺便清掉，解压前先删掉旧的这个文件夹（见步骤 3） |
| `ModAVD.exe`（管理器，0.36.0 起） | 用新包里的覆盖 |
| `tools\`：`fetch-components.ps1`、`collect-info.ps1`、`report-config.ps1`；0.36.0 起还有 `modavd.exe`（命令行）、`modavd-bridge.ps1`（arm64 执行桥的安装 / 卸载 / 检查） | 用新包里的覆盖 |
| `components\`：`de.jrpie.android.launcher_57.apk`（µLauncher）；0.36.0 起还有 `modavd-ime.apk`（宿主输入法） | 用新包里的覆盖 |
| `补全组件.bat`、`收集问题信息.bat`、`复制配置信息.bat`、`说明.txt`、`LICENSES\`、`modavd-manifest.json`（包里每个文件的大小和 SHA-256） | 用新包里的覆盖 |
| `avd\` | **不动**，新包里没有这个文件夹。实例、游戏和登录状态都在这里 |
| `sdk\platform-tools\`、`sdk\system-images\` | 不动，新包里也没有。新版要求的版本变了时，Release 说明里会写，届时再运行「补全组件.bat」 |
| `mas-avd.json`、`logs\` | 不动。这是 AUTO-MAS 和模拟器自己写的 |

## 步骤

1. 关掉所有实例：AUTO-MAS 里开的，以及 0.36.0 起管理器 `ModAVD.exe` 里开的；开着 `ModAVD.exe` 的话，也把它的窗口关掉。稍等半分钟，模拟器的附属进程退出要十几秒；确认任务管理器里没有 `qemu-system-x86_64` 进程。
2. 下载新的 Release 包。0.32.1 起压缩包里没有外层文件夹，`sdk\`、`tools\` 这些直接在压缩包的根上。
3. （可选）想顺便清掉新版拿掉的旧文件，先删掉旧根目录里的 `sdk\emulator\`。只删这一个文件夹，`avd\` 和 `sdk\` 下的其他文件夹都不要动。
4. 把新包整个解压到旧的根目录，遇到同名文件一律替换。右键「全部解压缩」默认会新建一个和压缩包同名的文件夹，要把目标改成旧的根目录；用 7-Zip 时也是选旧的根目录作目标，不要用「解压到 <包名>\」。覆盖时提示文件被占用，是模拟器的附属进程还没退出，再等一会儿重试。
5. 打开 `sdk\emulator\source.properties`，确认 `Pkg.BuildId` 是新版本号（例如 `mas-36`）。
6. 不用重新下载组件，AUTO-MAS 里也不用重新添加，照常开机即可。

## 从内测包升级

内测包（例如 `ModAVD-37.2.10-mas25`）自带 adb 和系统镜像，和「补全组件.bat」下载的是同一版本，所以：

- 按上面的步骤把新包整个解压到内测包的根目录。内测包里没有的文件（例如「补全组件.bat」、`LICENSES\`，以及 0.36.0 起的 `ModAVD.exe`）会一起放进去；
- 不需要再运行「补全组件.bat」。运行了也没关系，它会显示两样组件都已齐全并跳过。

内测包里的旧实例可以直接用新版开机。

## 版本太旧时

AUTO-MAS 会检查模拟器的版本号（`Pkg.BuildId`），太旧时拒绝开机并提示更新。按上面的步骤换成新版即可。
