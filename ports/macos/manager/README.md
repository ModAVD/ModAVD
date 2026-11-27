# 构建 macOS 管理器

`build.py` 为 Apple Silicon 构建当前 Go CLI 与管理器 `.app`。Python 3.11+、Git、Go 启动器和 macOS SDK 是构建依赖；Go 工具链固定为 `go1.26.5`，最低 macOS 版本为 13.0。不依赖 Swift/WKWebView，不修改系统的 Developer Directory。

```sh
ports/macos/manager/build.sh \
  --source /path/to/clean/ModAVDManager --revision FULL_MANAGER_COMMIT_SHA \
  --version 0.38.2 --build 38 --output /path/to/new/manager-dist
```

`--revision` 必须是精确的 40 位提交，`--source` 必须是该提交的干净 checkout。缺失源码时可克隆到一个新目录；已有 checkout 只读验证，不切分支、不打补丁、不清理修改。未跟踪或会影响构建的额外 ignored 文件会被拒绝；两份已存在的客体 gzip 会被临时 overlay 完整替换。构建输出必须是新的目录，且不能与源码互相包含。

构建先生成 Linux ARM64、amd64 两份 `modavd-proxyd`，核对真实 ELF 架构，确定性 gzip 后通过仓外 Go overlay 嵌入。不会把生成文件写回源码。CLI 和 `.app` 使用同一个 overlay；默认执行现有 `go test ./...`、`go vet ./...`，已独立完成同提交检查时可显式 `--skip-checks`，收据会记录这一选择。此选项不会改 CI。

输出如下：

```text
manager-dist/
  tools/modavd
  tools/modavd-manager
  ModAVD.app/Contents/Info.plist
  ModAVD.app/Contents/MacOS/ModAVD
  LICENSE
  .modavd-manager-build.json
  SHA256SUMS.json
```

Manager 自身版本与协议取自该提交的 `internal/buildinfo`，不以 `-X` 改写。`CFBundleShortVersionString` 与该 Manager 版本一致；`CFBundleVersion` 使用显式包构建号。`--version X.Y.Z --build Y` 声明整个 ModAVD 包的版本，两者分别记录在 schema 2 构建收据中；不能混同 Manager 版本和模拟器构建号。

每个入口采用本地 ad-hoc 签名，`.app` 做严格 deep 校验。独立 Manager 入口和 `.app` 来自同一个构建二进制，分别按各自上下文签名；应用签名绑定其 Info.plist，签名后的字节可以不同。构建直接检查两种入口都嵌入本次生成的双 ABI gzip。收据包含签名前二进制摘要、精确提交/tree、工具链、包版本、双 ABI 客体摘要与最终文件摘要。构建前后再次确认源码状态，生成物只在仓外临时目录存在，成功后原子发布到新输出目录。检查失败会保留输入，不覆盖用户目录。

完整 SDK 装配见 [上一级说明](../README.md)。实际浏览器交互、实例生命周期、磁盘维护和更新恢复验收另行在隔离包上进行；构建收据不表示这些功能已通过实机验收，也不表示已完成 Developer ID 或公证。
