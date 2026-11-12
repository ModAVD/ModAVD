# ModAVD

[简体中文](README.md) | **English**

<!-- README.md and README.en.md match section by section: if you change one, change the other in the same PR. -->

![ModAVD](assets/banner.png)

ModAVD is an Android emulator built from the AOSP Android Emulator 37.2.10 plus a set of patches. It runs on Windows today; support for more platforms is planned (see [#27](https://github.com/ModAVD/ModAVD/issues/27)).

The package includes a manager (`ModAVD.exe`) and a CLI (`tools\modavd.exe`) for creating instances and starting and stopping them. Automation tools such as [AUTO-MAS](https://github.com/AUTO-MAS-Project/AUTO-MAS) can drive it too and handle instance orchestration and task flows.

**Documentation: <https://modavd.github.io/en/>** (installation, upgrade, feedback, privacy, the components and how to contribute are all there)

> **Version note (current Release vs. `main`)**: this document describes the latest released package, `v0.39.0`. `main` may contain changes that have not been released yet. When you use a downloaded package, go by the notes of that Release and the version inside the package.

- [What is ModAVD](#what-is-modavd)
- [Tested games](#tested-games)
- [Requirements](#requirements)
- [Install](#install)
- [Upgrade](#upgrade)
- [Licenses and source code](#licenses-and-source-code)
- [Feedback](#feedback)
- [Privacy](#privacy)
- [Disclaimer](#disclaimer)
- [Icon and artwork](#icon-and-artwork)

## What is ModAVD

- An Android emulator that runs on Windows today and uses the Windows Hypervisor Platform (WHPX) for hardware acceleration. More platforms are planned.
- It can run in a window like any emulator, or headless (no window), started and stopped by the manager, the CLI or a script.
- The emulator itself comes from Google's open-source [Android Emulator](https://android.googlesource.com/platform/external/qemu/). We changed parts of QEMU, gfxstream (graphics streaming) and Crashpad (crash handling), and the changes are published as patches (`patches/`).
- adb and the Android system images belong to Google and are covered by the Android Software Development Kit License Agreement, so we cannot distribute them. They are not in the Release; **补全组件** (Fetch components) in the manager, or the CLI command `tools\modavd.exe components fetch`, downloads them from the official channels on your computer.

Compared with the official emulator, the published 0.39.0 build no longer uploads usage statistics or crash reports to Google, and no longer checks Google for updates or downloads feature flags. Write-through lowers the risk of qcow2 metadata corruption when an instance is force-killed, but you should still run `sync` first. Memory that Android frees goes back to Windows. The data disk can be enlarged. A batch of graphics, networking and WHPX problems are fixed. From 0.36.0 the package includes the arm64 execution bridge, which lets some games that ship only for arm64 run; any other arm64-only game still has to be verified one by one. For the games we have tested, see [Tested games](#tested-games). For the item-by-item differences see [How ModAVD differs from the official emulator](https://modavd.github.io/en/guide/changes) on the docs site; for each patch see `patches/`.

Version numbers are written as `0.<Y>.<Z>`; 0 means it is still in beta. From `0.32.1` to 0.40, Y was the emulator build number mas-N and Z the repackage count; since 0.41 the version counts up on its own, and the build number is given in the first line of the release notes and in `modavd-manifest.json` in the package.

This repository holds the emulator patches, the arm64 execution bridge ([bridge/](bridge/README.en.md)) and the bundled scripts ([package/](package/README.en.md)). The manager and the CLI are in [ModAVDManager](https://github.com/ModAVD/ModAVDManager), and the host IME is in [ModAVDHostIME](https://github.com/ModAVD/ModAVDHostIME).

## Tested games

These are the games the maintainer has actually run in the pre-release regression, all on one Windows 11 computer with an AMD CPU and an NVIDIA discrete GPU. "Last tested" is the ModAVD version of the most recent test, with the game version in parentheses. A game that is not listed may well run; we just have not tested it. Reports are welcome, see [Testing on real games and compatibility reports](https://modavd.github.io/en/develop/testing).

| Game | Last tested | What was tested | Notes |
|---|---|---|---|
| *Arknights* | 0.39.0 | Downloaded all 12.8 GB of resources from scratch; MAA started the game and ran stage 1-7; 59.8 fps on the main screen and in the base (the game caps at 60) | Resources download at only about 9 MB/s ([#35](https://github.com/ModAVD/ModAVD/issues/35)); now and then resource initialization fails at launch and the whole package is downloaded again ([#89](https://github.com/ModAVD/ModAVD/issues/89)) |
| *Reverse: 1999* | 0.39.0 (4.0.0) | Cold start to the main screen; on 0.38.0, M9A ran a regular stage at 54.7–59.1 fps in battle | — |
| *Honkai: Star Rail* | 0.39.0 (4.5.0) | Cold start; on 0.38.0, StarRailCopilot ran Simulated Universe for 15 minutes at a median of 42.8 fps | Use the normal (Vulkan) mode: the compatibility (GLES) mode the game picks on a first install crashes at the splash screen. A cold start takes about 3 minutes ([#42](https://github.com/ModAVD/ModAVD/issues/42)) |
| *Arknights: Endfield* | 0.38.0 | MaaEnd visited friends and ran the base, at a median of 34.7 fps | A cold start to the title screen takes over a minute ([#42](https://github.com/ModAVD/ModAVD/issues/42)) |
| *Onmyoji* (official server) | 0.38.0 | With the arm64 execution bridge; MaaYYs entered the courtyard and fought 3 Soul battles: 59.7 fps in the courtyard, about 58 fps in battle | A cold start now and then hangs at the audio list; close the game and open it again ([#36](https://github.com/ModAVD/ModAVD/issues/36)). The other channel builds are on the bridge's list but were not tested one by one |
| *Ruyuan* (China server) | 0.37.0 | With the arm64 execution bridge; 20 cold starts in a row all worked, and MaaYuan's daily tasks run | Host memory grows when MaaYuan drives it ([#156](https://github.com/ModAVD/ModAVD/issues/156)) |
| *Honkai Impact 3rd* | 0.39.0 (9.1.0) | Only a cold start to the "download resources" prompt; did not go in | — |
| *World of Airports* | 0.39.0 (3.7.3) | With the arm64 execution bridge and the Google Play Store, and "report GLES 3.2" turned on; played 13 minutes at an airport at 30 fps (the game caps at 30) | For the setup steps see the [0.39.0 release notes](https://github.com/ModAVD/ModAVD/releases/tag/v0.39.0) ([#131](https://github.com/ModAVD/ModAVD/issues/131)) |

## Requirements

| Item | Requirement |
|---|---|
| OS | Windows 10 / 11, 64-bit |
| CPU | x86_64 (Intel / AMD) with virtualization enabled in the BIOS (Intel VT-x / AMD SVM) |
| Windows feature | Windows Hypervisor Platform (WHPX) |
| GPU | A discrete GPU, or a recent integrated GPU, with Vulkan support |
| Memory | Each instance gets 6 GB by default (3–6 GB can be chosen); the PC needs about 1.5 GB more free when an instance starts |
| Disk | An NTFS drive. Fetching the components needs about 6.5 GB free; the first start of each instance needs more than 10 GB free, and usage then grows with game data, usually tens of GB per game |
| Path | A path with only ASCII letters and digits, for example `D:\ModAVD` |
| AUTO-MAS | `v5.7.0` or newer is recommended. From v5.7.0-beta.5 the MAA and MFW scripts (MaaFramework projects such as M9A) work; other scripts are not supported yet. See the Release notes for what each version supports |

## Install

1. Download `ModAVD-0.39.0-win-x64.zip` from [Releases](../../releases).
2. Right-click it and choose Extract All (the zip has no outer folder, so do not use "extract here"), and put the resulting folder in a path with only ASCII letters and digits, for example `D:\ModAVD`.
3. Double-click `ModAVD.exe` (the manager) in the folder and click **补全组件** (Fetch components) in the notice at the top. Once you pick a download source and accept Google's license agreement, it downloads adb and the Android 14 system image (about 1.5 GB). You can also run `tools\modavd.exe components fetch` in the folder. In 0.38.1 and earlier packages you double-click `补全组件.bat` instead.
4. In AUTO-MAS, open **模拟器管理** (Emulator Management), click **管理模拟器** (Manage emulators) → **添加魔改 AVD（内测）** (Add ModAVD (beta)), choose this folder, and add it once the check passes. You can also point the bundled `ModAVD.exe` or `tools\modavd.exe` at this root folder directly.

For detailed steps, how to turn on virtualization and common problems, see [Install](https://modavd.github.io/en/guide/install) on the docs site.

## Upgrade

Shut down all instances, wait about half a minute, then extract the new package over the old root folder and overwrite files with the same names. Instances, games and logins are all in `avd\` and are not touched. Before you go back to a version older than 0.36.0, uninstall the arm64 execution bridge from every instance first. For the details see [Upgrade](https://modavd.github.io/en/guide/upgrade) on the docs site.

## Licenses and source code

- **Everything in this repository except `package/`, `bridge/`, `assets/` and the `.patch` files under `patches/`** (README and the other documents, `docs/`, `scripts/`, the release and build tools in `tools/`, `.github/`, and so on): Apache License 2.0, see [LICENSE](LICENSE) at the root. Copyright 2026 qiyinxi. Files whose header names another license follow their header (`tools/gate/stage-payload.ps1` is AGPL-3.0-or-later). `package/` and `bridge/` are AGPL-3.0-or-later (see the next item), patches follow the license of the file they change (see the "Patches" item), and for the images and program icons under `assets/` see [Icon and artwork](#icon-and-artwork).
- **ModAVD's own programs**: from 0.36.0 they are all under the GNU Affero General Public License, version 3 or later (AGPL-3.0-or-later).
  - The script in the package that installs the bridge, `tools\modavd-bridge.ps1`: [package/](package/) in this repository; the full license text is in [package/LICENSE](package/LICENSE). Fetching the components, collecting a problem report and copying the system info are in the manager and the CLI (see the ModAVDManager item below); the `.bat` files and scripts that did them in 0.38.1 and earlier packages also shipped from `package/`. The scripts in the 0.35.0 and earlier packages were published under Apache License 2.0.
  - The arm64 execution bridge (`sdk\emulator\resources\modavd\`, from 0.36.0): [bridge/](bridge/) in this repository; the full license text is in [bridge/LICENSE](bridge/LICENSE). It is also in the source bundle of the same Release (see the "Source code" item below).
  - The manager `ModAVD.exe` and the CLI `tools\modavd.exe` (from 0.36.0): in the [ModAVD/ModAVDManager](https://github.com/ModAVD/ModAVDManager) repository.
  - The host IME `components\modavd-ime.apk` (from 0.36.0): in the [ModAVD/ModAVDHostIME](https://github.com/ModAVD/ModAVDHostIME) repository.
- **The capture library** `extras\modavd_extras.dll` (from 0.39.0): written by ModAVD, but not AGPL: it is published under the GNU Lesser General Public License, version 3 or later (LGPL-3.0-or-later). The source is in the [ModAVD/ModAVDExtras](https://github.com/ModAVD/ModAVDExtras) repository and in the source bundle of the same Release.
- **Patches**: each patch is published under the license of the file it changes, matching the upstream file header:
  - In the QEMU repository (`platform/external/qemu`), patches to QEMU's own files and to files with a GPL-2.0 header are GPL-2.0; patches to files with an Apache-2.0 header (some files under `android/` and `android-qemu2-glue/`) are Apache License 2.0. The license of each patch is listed in [patches/BASE.md](patches/BASE.md).
  - The gfxstream and Crashpad patches are published under Apache License 2.0, like the projects themselves.
- **Programs in the Release**: they contain third-party components such as QEMU (GPL-2.0), FFmpeg (GPL-2.0 or later, statically linked), Qt 6.5.3 (LGPL-3.0), and GLib and libusb (LGPL-2.1). Programs such as `qemu-system` are distributed as a whole under GPL-2.0. The license of each component is in `LICENSES\NOTICE.txt` in the package, and a summary is in [THIRD_PARTY.md](THIRD_PARTY.md).
- **Source code**: the source bundle of the same Release (`ModAVD-<version>-sources.zip`) holds the complete corresponding source: the exact build-time version of each of the 24 repositories compiled into the programs (one `.tar.xz` per repository; the QEMU one includes the build scripts), the source packages of FFmpeg / libvpx / libxml2 / pixman, the SeaBIOS and iPXE sources, the Qt 6.5.3 source, GLib 2.82.2 (statically linked into `netsimd.exe`) with its build modules and the netsim source, two repository manifests (also in [manifests/](manifests/) in this repository), and a `SOURCES.txt` that explains each file. From 0.36.0 it also has `modavd-src.tar.xz` (called `modavd-src-<commit>.tar.xz` in 0.36.0), the source of ModAVD's own programs (the manager and CLI, the arm64 execution bridge, the host IME, and from 0.39.0 the capture library), published under AGPL-3.0-or-later (the capture library under LGPL-3.0-or-later), with `LICENSE` and `NOTICE` inside the archive. The patches are in [patches/](patches/) in this repository; to build it yourself see [Building from source](https://modavd.github.io/en/develop/build) on the docs site. The patched source is also in three repositories, [ModAVDQemu](https://github.com/ModAVD/ModAVDQemu), [ModAVDGfxstream](https://github.com/ModAVD/ModAVDGfxstream) and [ModAVDCrashpad](https://github.com/ModAVD/ModAVDCrashpad): the AOSP history plus ModAVD's patches, with one `release/0.<N>` branch and one `v0.<N>.<P>` tag per version (0.36.0 is `release/0.36` and `v0.36.0`). The source tree is the same as the one you get by replaying `patches/`.
- **Google components**: adb and the system image are not in the Release. **补全组件** (Fetch components) in the manager or `tools\modavd.exe components fetch` downloads them from the official channels; they are covered by the Android Software Development Kit License Agreement, which you must accept before downloading.

## Feedback

Open an issue in this repository's [Issues](../../issues); problems with the manager, the host IME and the docs site go here too. After a problem happens, do not close the emulator yet: in the manager, open **设置** (Settings) → **组件与问题反馈** (Components and feedback) and click **收集问题信息** (Collect a problem report), or run `tools\modavd.exe collect-info`. It creates a problem report zip on your desktop.

Issues are public and anyone can download attachments. The report contains logs and screenshots, and, if the emulator has crashed, crash dumps (the `crash` folder, which has local paths and your Windows user name):

- In a public issue, post only the parts without the `crash` folder.
- Send the full report with crash dumps by e-mail to qiyinxi@foxmail.com, and put the issue number in the mail.

Please read [Report a problem](https://modavd.github.io/en/guide/feedback) on the docs site before you upload anything. To help with testing, documentation or patches, see [CONTRIBUTING.en.md](CONTRIBUTING.en.md) and [Contributing](https://modavd.github.io/en/develop/) on the docs site.

## Privacy

- The emulator program in the current release package does not upload usage statistics or crash reports to Google on its own, does not check Google for updates, and does not download feature flags from Google. Exception: when it runs in a window and you open the Location page of the extended controls, the map contacts Google Maps.
- The Android system and the apps in it (games, Google Play services and so on) use the network as usual; that traffic is theirs.
- In the current version, crash dumps stay on your computer, and the emulator does not upload them to Google.
- **补全组件** (Fetch components) only contacts the download mirrors after you click it (or run `components fetch`), and **收集问题信息** (Collect a problem report) only packs files locally and uploads nothing.
- An update check or download that you trigger explicitly in the manager or the CLI is component behavior started by you, and it is not the same as the emulator's own network activity.

For details see [Privacy](https://modavd.github.io/en/guide/privacy) on the docs site.

## Disclaimer

- Android is a trademark of Google LLC.
- The software is provided "as is", without warranty of any kind; see the licenses.
- Whether running a game with an emulator and scripts complies with the game's terms of use is for you to judge.

## Icon and artwork

- The images under `assets/` (banner, avatar, social preview image) and the icons built into the programs (the program icons and window icons of `emulator.exe`, `emulator-check.exe` and `qemu-system-x86_64*.exe`) are ModAVD's own icon and artwork. **They are not covered by this repository's Apache-2.0 license**, and no other license of this project covers them.
- If you have a copyright concern, contact us through [Issues](https://github.com/ModAVD/ModAVD/issues) or by e-mail at qiyinxi@foxmail.com, and we will deal with it right away.
