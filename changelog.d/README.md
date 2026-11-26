# 更新说明 / Release note lines

用户看得见的改动，在同一个 PR 里往这个文件夹加一个文件，写一行给用户看的话。发版时工具把上一版以后新加的文件汇总成公告的「更新内容」，并在行尾补上这个 PR 的编号。不加文件的 PR 不会出现在公告里。

- **文件名**：`<名字>.<类型>.md`。名字用小写字母、数字、`-`、`_`，起一个不会和别的改动重复的（比如 issue 号加几个词：`210-resize-crash.fix.md`）。
- **类型**：`feat` 新增，`fix` 修复，`change` 变更，`remove` 移除。
- **内容**：只有一行，不超过 60 字，不能有 `--`。写用户会看到的结果，不写原因和实现，比如「调高分辨率后开机不再卡死退出」。发版公告是中文的，这一行写中文；不会写中文就用英文写，在 PR 里说一声，由维护者在合并前改成中文。
- 想让公告指向 issue 而不是 PR，就自己在行尾写上，比如 `……不再卡死退出 (#210)`；这样工具不再补 PR 编号。
- 只和开发、测试、CI、文档有关的 PR 不用加。
- 已经发过版的文件不要改内容、也不要重复用它的名字；以后的改动另起一个文件。

模拟器源码补丁（`incoming/` 和 `patches/`）的说明文件也放这里。管理器、截图库、宿主输入法各自的仓库里有同样的文件夹。

---

A change that users can see adds one file here, in the same pull request, with a single line written for users. At release time the tools collect the files added since the previous release into the release notes and append the pull request number. A pull request without a file is not announced.

- **Name**: `<name>.<type>.md`, the name in lowercase letters, digits, `-`, `_`, and unique (for example the issue number and a few words: `210-resize-crash.fix.md`).
- **Type**: `feat` (新增), `fix` (修复), `change` (变更), `remove` (移除).
- **Content**: one line, at most 60 characters, without `--`. Say what users will notice, not the cause or the implementation, for example "Starting no longer hangs after raising the resolution". The release notes are in Chinese, so the line is in Chinese; if you do not write Chinese, write it in English and say so in the pull request, and the maintainer will put it into Chinese before merging.
- To point at an issue instead of the pull request, end the line with it yourself, for example `…… (#210)`.
- Pull requests that only touch development, tests, CI or documentation need no file.
- Do not edit or reuse a file that has already been released; a later change takes a new file.
