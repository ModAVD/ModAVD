# 首次建盘空间检查

补丁基于 ModAVDQemu `release/0.37` 的 `f3bfb151cd25bda723e9d7efad30a767f089c926`，源码树与公开仓库 `patches/qemu/mas37` 回放结果相同。只修改卷信息识别和首次建盘检查；测试脚本留在这里，不进入模拟器补丁。

旧逻辑通过 `FILE_SUPPORTS_SPARSE_FILES` 选择 10 GiB 门槛。FAT32 和 exFAT 没有这个能力，所以沿用上限的 1.2 倍。这原本是为了给临时 raw 镜像留空间：`libsparse/src/output_file.c` 会尝试 `FSCTL_SET_SPARSE`，最后用 `SetEndOfFile` 设置完整长度。保持创建方式不变，就仍须给这两个文件系统预留完整 raw 容量。

补丁按下面的规则检查可用空间：

| 卷类型 | 首次建盘门槛 |
| --- | --- |
| 支持稀疏文件，包括原来识别到的 NTFS、ReFS | 保持 `min(1.2 × 上限, 10 GiB)` |
| FAT32、exFAT | raw 容量 + 初始数据 + `max(512 MiB, raw 容量 / 64)` |
| 其他类型、卷信息查询失败 | 保持 `1.2 × 上限` |

初始数据取初始化目录递归文件大小与初始化镜像逻辑大小的较大值，兼顾目录复制和创建失败后的镜像复制回退。转换之前临时数据目录已被删除，因此这两份初始数据不相加。余量覆盖 ext4、qcow2 元数据和分配对齐，依据当前构建器的 inode 表约 256 MiB 上限、128 MiB 日志上限，至少保留 512 MiB，并随大分区增长。这是保守预算，尚未通过真实 FAT32、exFAT 测量校准。

例如，64 GiB exFAT 数据盘的门槛为 65 GiB 加初始数据，原来是 76.8 GiB。空间判断使用逐项比较和减法，避免加法溢出及转换为 MB 后的精度损失。

FAT32 还会提前拒绝超过 `4 GiB - 1 byte` 的临时 raw 文件，并报告文件大小限制。它不会因此支持默认的 64 GiB 数据盘；API 24 及以上原有的 6 GiB 最小数据盘也超过该限制。补丁不修改这个最小值、文件格式、创建过程或扩容过程。微软的[文件系统功能对照](https://learn.microsoft.com/en-us/windows/win32/fileio/filesystem-functionality-comparison)说明了这两种文件系统的稀疏文件支持情况与 FAT32 文件大小限制。

在 Visual Studio 的 x64 开发者命令行中运行，需要 Python 3 和 C++17 编译器。先应用补丁，然后指定 QEMU 源码目录及一个尚不存在的测试输出目录：

```powershell
python incoming/issue-3-first-disk-space/tests/run.py --source $qemu --work $work --baseline f3bfb151cd25bda723e9d7efad30a767f089c926
```

`run.py` 从实际 `main.cpp` 提取两个修改区段，用受控的 Win32 卷信息、可用空间、初始化文件信息替身运行，分别编译 Windows 与非 Windows 分支。它还比较基线，确认创建、转换、扩容及其余代码未改。非 Windows 分支是在 Windows 编译器上关闭对应条件宏后验证，不等于在 Linux 或 macOS 上构建。

2026-11-04 本地结果，MSVC 19.51、Python 3.12.10：

- Windows 分支 79/79 断言通过，非 Windows 分支 25/25 通过；均以 C++17、`/W4 /WX` 编译。
- 同一套测试在原始 mas37 源码上为 Windows 59/79、非 Windows 25/25，新增用例能检出原问题。
- 覆盖 NTFS、ReFS、FAT32、exFAT、大小写、未知类型、卷查询失败、空间查询失败、初始化目录与镜像回退、1 字节边界、超大数值及溢出、FAT32 单文件限制、已有数据盘与扩容绕过首次检查。
- `main.cpp` 修改区段以外的源码与基线一致。
- 导出的补丁在全新检出上用 `git am --keep-cr` 回放成功，源码树与开发检出一致，随后重跑上述 104 项断言全部通过。
- `python scripts/checks.py patches` 的全部 9 条系列回放及源码树核对通过；发布元数据、隐私扫描另在仓库根目录运行。
- 完整模拟器构建未完成。CMake 配置在缺少配套 AOSP `hardware/google/aemu/windows` 和 PIXMAN 预编译依赖时停止。上述小范围 C++ 编译不能替代完整构建。
- 未进行真实 FAT32、exFAT、ReFS 建盘测试。当前进程没有挂载及格式化虚拟磁盘所需的管理员权限；没有格式化、挂载或修改任何用户磁盘，也未使用游戏账号。
