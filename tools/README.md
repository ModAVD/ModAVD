# 发版和编译工具

**简体中文** | [English](README.en.md)

<!-- README.md 和 README.en.md 逐节对应：改了其中一份，请在同一个 PR 里把另一份也改了。 -->

维护者和平台负责人用的工具，不随包发出。入口都是 PowerShell 7（`pwsh`），辅助代码只用 Python 3.12 标准库。
发版前回归（端到端测试）在单独的仓库 [ModAVDE2E](https://github.com/ModAVD/ModAVDE2E)：它在装好的整包上测、跨好几个仓库，
要测试机和游戏实例，不跟着这个仓库发版。

| 目录 | 做什么 |
| --- | --- |
| [`build/`](build/) | `build-sdk.ps1`：从 AOSP 模拟器源码树增量编出 `sdk-masN`，记下编译时各仓库的提交，放进执行桥载荷 |
| [`gate/`](gate/README.md) | `stage-payload.ps1`：核对 arm64 执行桥三个载荷文件的固定哈希，放进 SDK 树 |
| [`pubrel/`](pubrel/README.md) | `release.ps1`：组包、导出并回放补丁、源码包、Release 说明、隐私扫描，可选创建 GitHub 草稿 |

CI 用的检查（`checks.py`、`privacy.py`、白名单）在仓库的 [`scripts/`](../scripts/)，发版工具直接用那一份。
发版的检查清单见 [RELEASING.md](../RELEASING.md)。

## 本机配置

仓库里不写死任何本机路径。和这台电脑有关的（源码树在哪、临时文件放哪、外部输入在哪），命令行上给参数；
不想每次都给的，写进本机配置：

1. 把 [`local.example.psd1`](local.example.psd1) 复制成同目录的 `local.psd1`（已在 `.gitignore` 里，不会提交），照注释填；
2. 或者放在仓库外，用环境变量 `MODAVD_TOOLS_CONFIG` 指过去（几个克隆共用一份时方便）。

读取规则在 [`local.ps1`](local.ps1)：参数优先，其次本机配置；两处都没有的路径直接报错，不猜默认值。
`MODAVD_TOOLS_CONFIG` 设了却指向不存在的文件，也报错。

私有隐私规则 `privacy-private-rules.json` 同样放在仓库外，用本机配置的 `PrivateRules` 或环境变量 `MODAVD_PRIVATE_RULES` 指过去，
绝不复制进仓库（`.gitignore` 里也挡着这个文件名）。

## 安全约定

- 输出、日志只写进临时目录（本机配置 `TempRoot`）。发版的输出目录和发版克隆不能落进源码树、SDK，以及本机配置
  `ProtectedRoots` 列出的目录（比如备份、生产环境的根目录）。
- 源码树只读：编译要求源码都已提交，编译前后各仓库的提交一致才装产物；发版只从提交里取东西。
- 关实例一律先 `adb shell sync` 再 `emu kill`，adb 只用私有端口，不碰 5037，也不碰生产根目录的端口。
