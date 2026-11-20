# macOS ARM64 打包

这里仅放生产构建与装配工具。管理器来自指定的干净 ModAVDManager 提交，SDK 与核心来自明确选择的本地输入；装配不启动模拟器、不接受许可、不下载组件，也不覆盖已有包。构建和复制期间保留至少 20 GiB 可用空间。Python 3.11+、Go 1.26.5 和 macOS SDK 用于构建；运行包中的组件、诊断和问题包功能直接使用 Go CLI，不需要 Python。

## 构建管理器

见 [manager/README.md](manager/README.md)。必须固定完整的 Manager 提交 SHA，不能使用旧的整体移植补丁或移动分支名。示例中的包版本和构建号应换成本次实际版本；ModAVD 的 `X.Y.Z` 中 `Y` 就是核心构建号。

```sh
python3 ports/macos/manager/build.py \
  --source /path/to/clean/ModAVDManager --revision MANAGER_COMMIT_SHA \
  --version 0.38.2 --build 38 --output /path/to/new/manager-dist
```

## 准备 SDK 输入

装配输入是独立目录，具有 `sdk/` 和 `receipts/`。平台工具固定为 macOS 37.0.1；客体固定为 Android 15 Google APIs ARM64 API35 r9，与 Manager 当前平台表一致。归档与许可指纹见 [runtime/components.lock.json](runtime/components.lock.json)。条款全文见 `runtime/licenses/`，必须保留用户已明确接受相应条款的记录。

每个 `receipts/platform-tools.json`、`receipts/system-image.json` 包含锁表里的 `revision`、`archiveSha1`、实际核对过的 `archiveSha256`，以及 `files`（相对该组件目录的完整普通文件 SHA256 映射，包含 `source.properties`）。不要把未核对的下载或另一个 SDK 的收据复制成自己的来源证明。已有 SDK 只读；验收应使用独立复制目录。

`receipts/android-sdk-license.json` 和 `receipts/android-sdk-arm-dbt-license.json` 分别包含 `id`、`acceptedAt` 和锁定条款的 `sha256`。装配工具只验证这些输入，不自动产生接受记录。SDK 获取、许可展示与确认使用现有 Manager 组件流程；若构建系统另行准备 SDK，须同时保留归档验证及逐文件收据。

## 装配

```sh
python3 ports/macos/package.py assemble \
  --emulator /path/to/emulator --profile custom --core-receipt /path/core-receipt.json \
  --manager /path/to/manager-dist \
  --manager-source /path/to/clean/ModAVDManager --manager-revision MANAGER_COMMIT_SHA \
  --runtime-root /path/to/isolated-runtime \
  --version 0.38.2 --build 38 --output /path/to/new/ModAVD
python3 ports/macos/package.py verify --output /path/to/new/ModAVD
```

`custom` 要求核心自身已声明 `Pkg.BuildId=mas-38`（与示例构建号一致），包含自编核心能力标记。核心收据的 `source` 对象记录真实仓库、提交和构建输入；`files` 覆盖每个非目录项：普通文件可用 SHA256 字符串，或含 `type: file`、`mode`、`sha256` 的对象；符号链接使用 `type: symlink`、`target`、`sha256`（目标 UTF-8 字节的摘要）。收据不代替可复现构建或发布者签名，不得修改 `source.properties` 冒充另一个构建。

没有自编核心时，可明确使用 `--profile official-validation`，选择未改写的 Google 官方 emulator 37.2.13 / build 16740729。该模式逐项核验 Mach-O 签名；官方 SDK 顶层目录不作为自编包的签名单元验收。`custom` 则额外要求整个 emulator 目录通过严格签名验证，与原生更新协议一致。官方验证包可用来进行 SDK 和 Manager 机器验收；它不具备自编核心身份，不能作为生产更新发布。官方输入的签名与内容核对也不等于对全部移植优化完成验收。

IME 和桌面 APK 可用 `--ime-apk /path/modavd-ime.apk`、`--launcher-apk /path/launcher.apk` 显式加入。它们是可选项，仅接受 `components.lock.json` 固定的大小与 SHA256；随选中的桌面一起保存其许可。省略时不会下载，清单记录未包含。

输出含 `.app`、`tools/modavd`、`tools/modavd-manager`、SDK、许可与来源清单。路径统一使用 `macos-arm64`。应用默认使用系统浏览器；此构建不加入旧 WKWebView 辅助程序、Python 维护命令、QA 工具、游戏、用户 AVD 或凭据。

## 完整性与发布边界

装配核对精确 Manager 源码/构建收据、SDK 逐文件摘要、ARM64 Mach-O、代码签名和包清单。拒绝特殊文件、大小写冲突、越出所属目录单元的链接、输入输出嵌套与已有输出路径。文件优先 APFS clone，失败后检查空间再复制；新目录原子发布。`PACKAGE-MANIFEST.json` 核对不可变负载，允许运行时另建 `avd/`、`logs/`、`mas-avd.json`。

代码签名使用输入已有签名和 Manager 本地 ad-hoc 签名，校验的是完整性。Developer ID、公证、下载隔离与远端发布须按发行流程单独完成；装配成功不声明这些验收已通过。机器验收结果、截图和日志保存在仓库外并提交对应 issue。
