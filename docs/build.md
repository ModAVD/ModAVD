# 从源码构建 ModAVD

下面以 main 中准备中的 `0.35.0`（构建号 mas-35）源码为例，固定本仓库提交 `b8c5b85fe4a85dee7c78f92fce950c63801e5a50`。它尚无公开 Release、标签或对应源码总包，下面直接检出这个已存在的提交，并按固定清单同步源码。

当前公开版本是 [0.32.1 Release](https://github.com/ModAVD/ModAVD/releases/tag/v0.32.1)。要复现该发行版，请使用 [v0.32.1 标签下的构建说明](https://github.com/ModAVD/ModAVD/blob/v0.32.1/docs/build.md)及同一个 Release 的源码总包；包内 `SOURCES.txt` 说明各源码文件的来源和用途。其他版本也要使用对应版本的清单和补丁，不能把不同版本的源码和二进制混用。
从 0.32.1 起版本号写成 `0.<N>.<P>`（tag `v0.32.1`），N 就是构建号 mas-N：清单仍叫 `manifest-masN.xml`，补丁目录仍按 masN 命名。

## 环境

- Windows x86_64；Visual Studio 2022 生成工具 **17.14**，安装 C++ 桌面生成工具、ATL、MFC、Windows 10 SDK 19041 / Windows 11 SDK 22621。
- 开启 Windows 开发人员模式，以便 Git 和 repo 创建符号链接。
- Python 3.12、Git for Windows、[repo 工具](https://gerrit.googlesource.com/git-repo/)。把 Python、Git、repo 放到终端 PATH 中。
- 使用较短的源码根目录，保留足够空间给全部 AOSP 项目、预编译库、构建中间文件和产物。下面用 `$src` 表示自行选择的源码目录，用 `$modavd` 表示本仓库的位置。

下面的命令在 PowerShell 7 中运行。repo 在 Windows 上可通过调用其 Python 启动脚本的 `repo.cmd` 使用；上游说明见源码的 `external/qemu/android/docs/WINDOWS-DEV.md`。
准备好自己的 Git 提交身份。关闭换行转换、开启符号链接和长路径；以下设置只影响当前终端及其子进程：

```powershell
$env:GIT_CONFIG_COUNT = '3'
$env:GIT_CONFIG_KEY_0 = 'core.autocrlf'
$env:GIT_CONFIG_VALUE_0 = 'false'
$env:GIT_CONFIG_KEY_1 = 'core.symlinks'
$env:GIT_CONFIG_VALUE_1 = 'true'
$env:GIT_CONFIG_KEY_2 = 'core.longpaths'
$env:GIT_CONFIG_VALUE_2 = 'true'
```

## 获取确切版本

先克隆本仓库并检出下面固定的 mas35 提交。构建已发布版本时改用它实际存在的 Release 标签。mas30 的标签早于 `manifests/`，构建 mas30 时可以检出 mas32 的标签，里面同样有 mas30 的清单和补丁。记下本仓库目录的绝对位置：

```powershell
git clone https://github.com/ModAVD/ModAVD.git
Set-Location ModAVD
git checkout b8c5b85fe4a85dee7c78f92fce950c63801e5a50
$modavd = (Get-Location).Path
```

清单在 `manifests/`，源码总包中也有相同文件：

- `manifest-mas35.xml` 记录编译时的完整目标提交（每个版本一份 `manifest-<版本>.xml`）。
- `manifest-aosp-base.xml` 把三个补丁仓库换成 AOSP 公开基准，其余项目保持相同。

**不能直接对目标清单做 repo sync**：其中 qemu、gfxstream、Crashpad 的三个本地提交不在 AOSP 服务器上。
应以目标清单为版本依据，用配套的公开基准清单完成 `repo init` / `repo sync`，再回放补丁得到目标源码树。

在自己选择的短路径源码目录中运行（先将 `$src` 设为这个目录的绝对路径）：

```powershell
New-Item -ItemType Directory -Path $src
Set-Location $src
repo init -u https://android.googlesource.com/platform/manifest -b emu-main-dev --depth=1
if ($LASTEXITCODE) { throw 'repo init failed' }
Copy-Item "$modavd/manifests/manifest-aosp-base.xml" .repo/manifests/manifest-aosp-base.xml
repo init -u https://android.googlesource.com/platform/manifest -b emu-main-dev -m manifest-aosp-base.xml --depth=1
if ($LASTEXITCODE) { throw 'pinned repo init failed' }
repo sync -c --no-clone-bundle -j4
if ($LASTEXITCODE) { throw 'repo sync failed' }
```

清单中的预编译工具和库也是构建输入，应一并同步。不要用移动中的分支头替代固定 revision。

## 回放补丁

基准、目录、补丁数和各补丁许可证见 [patches/BASE.md](../patches/BASE.md)。按文件名排序，使用 `git am --keep-cr`，保留补丁中本来就有的 CR 字节：

```powershell
$series = @(
    @{ Repo = 'external/qemu'; Patches = 'qemu/mas35' },
    @{ Repo = 'hardware/google/gfxstream'; Patches = 'gfxstream/mas35' },
    @{ Repo = 'external/crashpad'; Patches = 'crashpad/module-version22' }
)
foreach ($s in $series) {
    $repo = Join-Path $src $s.Repo
    git -C $repo switch -c modavd-mas35
    if ($LASTEXITCODE) { throw "branch failed: $($s.Repo)" }
    $patches = @(Get-ChildItem (Join-Path "$modavd/patches" $s.Patches) -Filter '*.patch' |
        Sort-Object Name | ForEach-Object FullName)
    git -C $repo am --keep-cr @patches
    if ($LASTEXITCODE) { throw "patch replay failed: $($s.Repo)" }
}
```

补丁的作者已统一。回放会产生不同的提交编号，应比较源码树，而不是要求本地提交编号等于目标清单里的私有提交。
`git -C <仓库> rev-parse 'HEAD^{tree}'` 应与下表相同（当前仓库的 `patches/series.json` 也记录这些树）：

| 仓库 | mas35 目标树 |
| --- | --- |
| qemu | `08d6404e25008bb19caadf4185af10cbad91bc50` |
| gfxstream | `51288846bdcb9054d899ffc8458d6744d6206e32` |
| Crashpad | `0a3c056231f5257034514ae8d83192de388351f0` |

其余项目仍应处于目标清单记录的 revision。

## 构建

从同步好的源码根目录开始，使用本版本的预编译库和工具。下面跳过清理、重编预编译库及测试任务，进行 release 构建，并保留增量构建目录。

```powershell
$env:PYTHONUTF8 = '1'
$env:CMAKE_BUILD_PARALLEL_LEVEL = '12' # 不超过 16；按可用内存调低
$env:PATH = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer;$env:PATH"
$out = Join-Path $src 'objs'
$dist = Join-Path $src 'dist'
Set-Location (Join-Path $src 'external/qemu')
python android/build/python/cmake.py --out $out --dist $dist `
  --task-disable clean --task-disable removeleftover --task-disable prebuilts `
  --task-disable ctest --task-disable emugentest --task-disable genentriestest `
  --task-disable coveragereport --task-disable packagesamples `
  --task-disable zipintegrationtests --task-disable integrationtest
if ($LASTEXITCODE) { throw 'build failed' }
```

可直接使用的模拟器目录位于 `$out/distribution/emulator/`，发行归档位于 `$dist/`。
安装成自己的 SDK 时，完整复制 `emulator/`，并将其中 `source.properties` 的 `Pkg.BuildId` 设为相应构建号，例如 `mas-35`。
这里构建的是模拟器程序；adb、系统镜像、实例和游戏数据分别管理。

这套流程使用 manifest 固定的 AOSP 预编译依赖。需要修改、重编或重新链接 Qt、GLib 等第三方库时，源码总包中有对应源码、补丁和构建文件，按 `SOURCES.txt` 以及主包 `LICENSES/NOTICE.txt` 的项目说明处理。
