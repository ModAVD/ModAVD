# 参与 ModAVD

完整的参与说明在文档站：**[参与开发](https://modavd.github.io/develop/)**。各部分要什么环境和硬件、怎么验证、用 AI 工具时选什么模型，见[分区一览和通用要求](https://modavd.github.io/develop/overview)；想让一个游戏跑起来，见[适配新游戏](https://modavd.github.io/develop/adapt-game)。

下面是提 issue、PR 前最要紧的几条。

## 先说一声

- ModAVD 只有一个维护者，回复可能要几天。大一点的改动请先开 issue 商量，没商量过的大 PR 可能不合。
- 动手前在 issue 里说一声；issue 已经指派给谁，就是有人在做。
- 最需要的是在你的电脑上试：装好跑一轮日常，用「复制配置信息.bat」把配置贴到置顶的「兼容性报告」帖。见[游戏实测和兼容性报告](https://modavd.github.io/develop/testing)。

## 报问题

- 开 issue，附「收集问题信息.bat」生成的问题包。哪些能公开贴、崩溃转储怎么私下发，见[反馈问题](https://modavd.github.io/guide/feedback)。
- AUTO-MAS 的界面、任务、调度问题，请到 [AUTO-MAS](https://github.com/AUTO-MAS-Project/AUTO-MAS/issues) 提。
- 安全问题不要公开发，请发邮件到 qiyinxi@foxmail.com。

## 这个仓库里改什么

- 文档：使用说明和开发说明都在文档站的仓库 [modavd.github.io](https://github.com/ModAVD/modavd.github.io)，每页底部有「在 GitHub 上改这一页」。本仓库的 README 写错了直接提 PR。
- 随包脚本 `package/`：可以提 PR，合进来后随下一版发出。写法见 [package/README.md](package/README.md)。
- arm64 执行桥 `bridge/`：改动先开 issue 商量。它直接影响游戏能不能启动，每一版都要在装了桥的实例上实跑游戏回归。
- 模拟器补丁：先开 issue，再按[模拟器本体](https://modavd.github.io/develop/emulator)导出 `.patch`，附在 issue 里或开一个只加 `incoming/<主题>/*.patch` 的 PR。不要求你会编译或跑回归。
- `patches/`、`manifests/`、`release/` 是发版工具导出的，`scripts/` 和维护者本地的发版工具逐字节一致：PR 不直接改这些，有问题先开 issue。
- 管理器和命令行在 [ModAVDManager](https://github.com/ModAVD/ModAVDManager)，宿主输入法在 [ModAVDHostIME](https://github.com/ModAVD/ModAVDHostIME)：代码问题和 PR 提到那边。
- 新加或修改 workflow：第三方 action 钉到 40 位提交 SHA，不用 `pull_request_target`，不用自托管 runner。`mirrorchyan_*.yml` 由 Mirror 酱官方维护，照原样保留。
- 不收编译 CI：源码加编译产物约 124 GB，GitHub 的免费机器跑不了；自托管 runner 又会让外部 PR 在那台机器上执行任意命令（见 #5）。

## 署名与许可

- 从自己的 fork 开 PR。外部 PR 的 CI 要维护者批准后才会跑。
- 署你自己的名字或化名，邮箱用 GitHub 的 noreply 地址（`<编号>+<登录名>@users.noreply.github.com`）；不写 `Co-authored-by`。收进来后保留你的署名，发版说明里会致谢。
- 你的改动按所改文件的许可证进来，见 README 的[许可证与源码](README.md#许可证与源码)；新文件的版权行写你的名字或化名。不用签 CLA，`Signed-off-by` 可加可不加。
- 可以用 AI 工具，但改动要让 AI 自己测过（编译、跑测试，能开实例的就开实例看结果），在 PR 里写明测了什么，你为它负责。
- 通用的修复以后可能投到 Google 上游（AOSP、google/gfxstream），上游要求作者本人签 Google CLA。到时会先问你，不愿意就只留在 ModAVD。
- 图片、图标类改动先开 issue，写明来源和许可证；同人图和来源说不清的图不收。

## 自己测试时

- 关实例尽量在 AUTO-MAS 或管理器里关。自己写脚本时，先 `adb -s <设备> shell sync`，再 `adb -s <设备> emu kill`：不先 sync，游戏刚写的文件可能变成 0 字节。
- 强杀测试用新建的实例和测试账号，不用登着主力账号的实例。
- 不要用默认 5037 端口上的 adb；端口看 `mas-avd.json` 里的 `scriptAdbServerPort`（默认 20049）。

想帮着整理 issue（复现、合并重复、打标签），可以找维护者要 Triage 权限。
