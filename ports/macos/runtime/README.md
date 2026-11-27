# macOS SDK 装配输入

生产装配使用 `components.lock.json` 中的 macOS platform-tools 37.0.1 和 Android 15 Google APIs ARM64 API35 r9。目录结构是独立根目录下的 `sdk/platform-tools/`、`sdk/system-images/android-35/google_apis/arm64-v8a/` 及 `receipts/`。

条款全文在 `licenses/`。获取组件前必须展示并取得用户对相应条款的明确接受；装配脚本不替用户同意，也不复制其他用户的许可状态。使用 Manager 现有组件下载/导入流程或已验证的本地构建输入，保留官方归档大小、SHA1/SHA256、逐文件摘要及接受记录。收据字段和装配命令见 [打包说明](../README.md)。

这里不要求旧 Python runtime/QA 脚本进入生产包。`official-validation` 核心只是官方 SDK 机器验收用途，不是自编核心发布；现有 SDK、AVD、ADB 密钥与默认服务不会由装配工具启动或修改。
