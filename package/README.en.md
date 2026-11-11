# Bundled scripts

[简体中文](README.md) | **English**

<!-- README.md and README.en.md match section by section: if you change one, change the other in the same PR. -->

The scripts that ship inside the main Release package, byte-for-byte identical to the files shipped in the package. What is compared is the bytes in the package, that is, the files in the maintainer's local repository working tree (checked out there with `core.autocrlf=true`, so `.ps1` files are CRLF), not what is stored in Git there (LF). At the moment there is only one, `modavd-bridge.ps1`, which lives in the package's `tools\` folder.

| File | What it does | In the package since |
| --- | --- | --- |
| `modavd-bridge.ps1` | Installs, removes or verifies the arm64 execution bridge on one instance; see the notes at the top of the file for usage. The bridge source is in [bridge/](../bridge/README.en.md) | 0.36.0 |

Fetching missing components, collecting problem information and copying the config information are in the manager `ModAVD.exe` and the command line `tools\modavd.exe`. The source is in [ModAVDManager](https://github.com/ModAVD/ModAVDManager); to change them, raise it there:

| What to do | In the manager | Command line |
| --- | --- | --- |
| Fetch missing components: first pick a download source, show Google's "Android Software Development Kit License Agreement", and after you agree, download adb and the system image, verify them and install them into `sdk\`; downloads can resume | "补全组件" (Fetch components) in the notice at the top when a component is missing, or "补全组件" under the "组件" (Components) item of "电脑检查" (PC check) | `tools\modavd.exe components fetch` |
| Collect problem information: generates a problem package on the desktop. What may be posted publicly is described in [Reporting issues](https://modavd.github.io/guide/feedback) on the docs site | "设置" (Settings) → "组件与问题反馈" (Components and issue reporting) → "收集问题信息" (Collect problem information) | `tools\modavd.exe collect-info` |
| Copy config information: puts the PC configuration and the game packages installed on each instance into a block of text and copies it to the clipboard, to attach when you open a new issue in [Issues](https://github.com/ModAVD/ModAVD/issues) | "设置" (Settings) → "组件与问题反馈" (Components and issue reporting) → "复制配置信息" (Copy config information) | `tools\modavd.exe report-config` |

In packages 0.38.1 and earlier, these three things were done by `补全组件.bat`, `收集问题信息.bat` and `复制配置信息.bat` in the package root, together with `fetch-components.ps1`, `collect-info.ps1` and `report-config.ps1` under `tools\`. They used to live in this directory too, and since 0.38.2 they are no longer shipped in the package (ModAVD/ModAVD#45). When you upgrade with `modavd update` they are deleted; if you upgrade by copying over manually they are left in place, no longer used, and you can delete them yourself.

## Rules for writing them

- `.ps1` files must also run on the Windows built-in PowerShell 5.1 and are saved as UTF-8 with BOM. `.gitattributes` makes this directory store files as they are, with no line-ending conversion; when you submit a PR, please keep each file's original line endings and BOM.
- Do not hard-code local paths; adb goes only through the private port in `mas-avd.json` in the ModAVD root, and never touches 5037.
- Before a restart or shutdown, run `adb shell sync` and then end the emulator.

## Changing these files

Open an issue first to let us know, then submit a PR. Once merged, it ships with the next Release and the release notes will mention it. The license is the GNU Affero General Public License version 3 or later (AGPL-3.0-or-later); the full text is in [LICENSE](LICENSE) in this directory. What ships in the package from 0.36.0 on is the AGPL version here; the scripts in packages 0.35.0 and earlier were published under Apache License 2.0.
