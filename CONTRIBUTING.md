# 参与 ModAVD

ModAVD 只有一个维护者，回复可能要几天。大一点的改动请先开 issue 商量，没商量过的大 PR 可能不合。

## 最需要的：在你的电脑上试

维护者只在一台 Windows 11、AMD CPU、NVIDIA 独显的电脑上测过。最缺这些配置的实测：Intel CPU，AMD 或 Intel 显卡，笔记本双显卡，Windows 10，16 GB 内存。

1. 照 [README](README.md) 装好，在 AUTO-MAS 里添加，开机后跑一轮日常任务。
2. 在置顶的[「兼容性报告」帖](https://github.com/ModAVD/ModAVD/issues/8)按模板填写配置与运行结果。当前公开的 0.32.1 包没有「复制配置信息.bat」，请手动填写；这个工具计划从 0.35.0 提供，拿到含它的包后可以双击运行，把输出贴到该帖，补完最后三行，再写一句能不能跑。

这份输出不带用户名、电脑名、完整路径和游戏账号，可以直接公开贴。

## 报问题

- 开 issue，附「收集问题信息.bat」生成的问题包。哪些能公开贴、崩溃转储怎么私下发，见 [docs/feedback.md](docs/feedback.md)。
- 贴日志前，把路径里的 `C:\Users\<你的用户名>` 换成 `%USERPROFILE%`。
- AUTO-MAS 的界面、任务、调度问题，请到 [AUTO-MAS](https://github.com/AUTO-MAS-Project/AUTO-MAS/issues) 提。
- 安全问题不要公开发，请发邮件到 qiyinxi@foxmail.com。

## 改文档和代码

- 动手前在 issue 里说一声；issue 已经指派给谁，就是有人在做。
- README 和 `docs/` 里写错、写得不清楚的地方：直接提 PR。
- `patches/`、`manifests/`、`release/` 是发版工具导出的，`scripts/` 和维护者本地的发版工具逐字节一致：PR 不直接改这些，有问题先开 issue。
- 新加或修改 workflow：第三方 action 钉到 40 位提交 SHA，不用 `pull_request_target`，不用自托管 runner。`mirrorchyan_*.yml` 由 Mirror 酱官方维护，照原样保留。
- 不收编译 CI：源码加编译产物约 124 GB，16 核的机器全量编一次要 1.5 小时左右，GitHub 的免费机器跑不了；自托管 runner 又会让外部 PR 在那台机器上执行任意命令（见 #5）。想帮忙验证构建，照 [docs/build.md](docs/build.md) 在自己的电脑上编，把结果贴到 issue。

### 模拟器补丁

1. 先开 issue，写清要改什么、为什么。
2. 拿源码：在本仓库目录里运行 `python scripts/checks.py patches --work <一个还不存在的目录>`。它从 android.googlesource.com 浅取 AOSP 基准，回放 `patches/` 里当前版本的补丁，每条补丁系列生成一个仓库，如 `qemu-mas35`。
3. 进要改的仓库，**先把提交身份改成你自己的**，因为脚本给这几个仓库设的是维护者的身份：

   ```
   git config user.name "<你的名字或化名>"
   git config user.email "<你的 GitHub noreply 邮箱>"
   ```

4. 改完提交，用 `git format-patch -<提交数>` 导出。把 `.patch` 附在 issue 里，或者开一个只加 `incoming/<主题>/*.patch` 的 PR。这类 PR 不合并：补丁收进下一版后关闭，并写明是哪一版。
5. 不要求你会编译或跑回归。维护者审完会在自己的电脑上编、测。

## 署名与许可

- 署你自己的名字或化名，邮箱用 GitHub 的 noreply 地址（`<编号>+<登录名>@users.noreply.github.com`，在 GitHub 的邮箱设置里能看到）；不写 `Co-authored-by`。收进来后保留你的署名，发版说明里会致谢。
- 你的改动按所改文件的许可证进来，见 README 的[许可证与源码](README.md#许可证与源码)；新文件的版权行写你的名字或化名。不用签 CLA，`Signed-off-by` 可加可不加。
- 可以用 AI 工具，但你要看懂、测过每一处改动，并为它负责。
- 通用的修复以后可能投到 Google 上游（AOSP、google/gfxstream），上游要求作者本人签 Google CLA。到时会先问你，不愿意就只留在 ModAVD。
- 图片、图标类改动先开 issue，写明来源和许可证；同人图和来源说不清的图不收。

## 自己测试时

- 关实例尽量在 AUTO-MAS 里关。自己写脚本时，先 `adb -s <设备> shell sync`，再 `adb -s <设备> emu kill`：不先 sync，游戏刚写的文件可能变成 0 字节。
- 强杀测试用新建的实例和测试账号，不用登着主力账号的实例。
- 不要用默认 5037 端口上的 adb，它会和别的模拟器自带的 adb 抢同一个 server；端口看 `mas-avd.json` 里的 `scriptAdbServerPort`（默认 20049），和 AUTO-MAS 给脚本用的一样。

## 其他

想帮着整理 issue（复现、合并重复、打标签），可以找维护者要 Triage 权限。
