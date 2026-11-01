{{MACHINE}}
ModAVD {{VERSION}}，构建号 `{{BUILD_ID}}`，基于 Google AOSP Android Emulator {{UPSTREAM}} 构建。安装、升级和反馈见仓库 README 与 `docs/`。

## 更新内容

{{CHANGES}}

## 怎么更新

{{HOW_TO_UPDATE}}

## AUTO-MAS 版本

{{AUTOMAS}}

## 已知问题

{{KNOWN}}

<!-- modavd:github-only -->

## 发布前回归

{{REGRESSION}}

## 下载

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
{{MAIN_ROW}}
{{BUNDLE_ROW}}

主包包含 `sdk/emulator/`、轻量桌面、补全组件与问题收集工具、说明和第三方许可证。
主包里没有外层文件夹：请用右键「全部解压缩」或 7-Zip「解压到 <包名>\」，会自动建一个同名文件夹。
包的根目录有 `modavd-manifest.json`，列出包里每个文件的大小和 SHA-256。
adb 与 Android 系统镜像由「补全组件.bat」在用户同意许可后下载。

## 源码

源码包只打包不压缩，内含 {{SOURCE_COUNT}} 个源码文件、清单和来源说明，合计 {{SOURCE_BYTES}} 字节。
每个源码文件的 SHA-256 见包内的 `SHA256SUMS.txt`，仓库里有同样的一份：[`release/{{VERSION}}.sources.SHA256SUMS.txt`](https://github.com/ModAVD/ModAVD/blob/v{{VERSION}}/release/{{VERSION}}.sources.SHA256SUMS.txt)。
`SOURCES.txt` 说明附件来源，补丁及许可证见 `patches/BASE.md`。

## 校验与许可证

`SHA256SUMS.txt` 记录上述全部附件的 SHA-256。请核对后再安装。
ModAVD 自己的程序和随包脚本（管理器、命令行、arm64 执行桥、宿主输入法、补全组件和问题收集脚本）按 AGPL-3.0-or-later 发布，源码在 [ModAVDManager](https://github.com/ModAVD/ModAVDManager)（管理器、命令行）、[ModAVDHostIME](https://github.com/ModAVD/ModAVDHostIME)（宿主输入法）和本仓库的 [package/](https://github.com/ModAVD/ModAVD/tree/main/package)（随包脚本）、[bridge/](https://github.com/ModAVD/ModAVD/tree/main/bridge)（执行桥），这一版程序的源码也在源码包的 `modavd-src-*.tar.xz` 里，随包脚本本身就是源码；文档按 Apache-2.0；模拟器和补丁按所用第三方许可证发布。见包内 `LICENSES/` 和仓库 `patches/BASE.md`。
程序图标为《明日方舟》艾雅法拉的同人创作，角色版权归上海鹰角网络所有，不在上述许可证范围内。
本项目与 Google、鹰角网络无关。

[已有 Mirror酱 CDK？前往 Mirror酱 高速下载](https://mirrorchyan.com/zh/projects?rid=ModAVD&source=modavdgh-release)
