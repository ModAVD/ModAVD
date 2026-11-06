# Contributing to ModAVD

[简体中文](CONTRIBUTING.md) | **English**

<!-- CONTRIBUTING.md and CONTRIBUTING.en.md match section by section: if you change one, change the other in the same PR. -->

The full contributor guide is on the docs site: **[Contributing](https://modavd.github.io/en/develop/)**. For the environment and hardware each area needs, how to verify a change, and which model to pick when you use an AI tool, see [Areas and common requirements](https://modavd.github.io/en/develop/overview). To get a game running, see [Adapting a new game](https://modavd.github.io/en/develop/adapt-game).

What follows are the points that matter most before you open an issue or a PR.

## Before you start

- ModAVD has one maintainer, and replies can take a few days. For anything bigger, open an issue and talk it over first; a large PR that was not discussed may not be merged.
- Say so in the issue before you start work. If an issue is assigned to someone, that person is working on it.
- What helps most is testing on your own computer: install it and run a normal day of tasks. Open an issue in [Issues](https://github.com/ModAVD/ModAVD/issues) and include the output of **复制配置信息** (Copy system info; in the manager under **设置** (Settings) → **组件与问题反馈** (Components and feedback), or `tools\modavd.exe report-config`). See [Game testing and compatibility reports](https://modavd.github.io/en/develop/testing).

## Reporting problems

- Problems with every component go to this repository's [Issues](https://github.com/ModAVD/ModAVD/issues): the emulator itself, and also the manager and CLI, the host IME, the docs site and the pre-release regression. Pick the component when you open the issue.
- Attach the problem report made by **收集问题信息** (Collect a problem report; in the manager under **设置** (Settings) → **组件与问题反馈** (Components and feedback), or `tools\modavd.exe collect-info`). What you may post publicly and how to send crash dumps privately is in [Report a problem](https://modavd.github.io/en/guide/feedback).
- For problems with AUTO-MAS's interface, tasks or scheduling, please go to [AUTO-MAS](https://github.com/AUTO-MAS-Project/AUTO-MAS/issues).
- Do not post security problems in public; e-mail them to qiyinxi@foxmail.com.

## What to change in this repository

- Documentation: the user and developer docs are in the docs site's repository, [modavd.github.io](https://github.com/ModAVD/modavd.github.io), and every page has an "Edit this page on GitHub" link at the bottom. If this repository's README is wrong, open a PR directly.
- Both languages: the README and this guide each come in a Chinese file (`README.md`, `CONTRIBUTING.md`) and an English file (`README.en.md`, `CONTRIBUTING.en.md`) that match section by section; Chinese is the primary language. If you change one, change the other in the same PR. If you only write one of the two languages, say so in the PR and the maintainer will add the other. The English pages of the docs site work the same way; see [Documentation and this site](https://modavd.github.io/en/develop/docs).
- Bundled scripts in `package/`: PRs are welcome and ship with the next release once merged. How to write them is in [package/README.md](package/README.md) (in Chinese). Fetching the components, collecting a problem report and copying the system info are changed in [ModAVDManager](https://github.com/ModAVD/ModAVDManager).
- The arm64 execution bridge in `bridge/`: open an issue first. It decides directly whether a game can start, so every version is regression-tested by running games on an instance that has the bridge installed.
- Emulator patches: open an issue first, then export a `.patch` as described in [The emulator itself](https://modavd.github.io/en/develop/emulator) and attach it to the issue, or open a PR that only adds `incoming/<topic>/*.patch`. A patch should already be in the shape we will take: keep test scripts out of the patch itself (put them in the PR description, or in `incoming/<topic>/tests/`). Once reviewed it is merged, and merged means accepted: it ships unchanged with the next release, with your authorship kept. You do not have to be able to build or run the regression tests.
- `patches/`, `manifests/` and `release/` are exported by the release tools (`tools/pubrel/`): PRs do not change them directly; if something is wrong, open an issue first.
- The release and build tools in `tools/`, and the CI checks in `scripts/` (the release tools use them too): open an issue first. Do not write local paths into the repository; use arguments or the local configuration, see [tools/README.md](tools/README.md) (in Chinese). If you change `tools/pubrel/` or `scripts/`, run `python -I tools/pubrel/test_release.py --work <fresh temp dir>` locally.
- The manager and the CLI are in [ModAVDManager](https://github.com/ModAVD/ModAVDManager), the host IME is in [ModAVDHostIME](https://github.com/ModAVD/ModAVDHostIME), and the pre-release regression (end-to-end tests) is in [ModAVDE2E](https://github.com/ModAVD/ModAVDE2E): PRs for those go to those repositories, but issues still go here.
- Adding or changing a workflow: pin third-party actions to a full 40-character commit SHA, do not use `pull_request_target`, and do not use self-hosted runners. `mirrorchyan_*.yml` is maintained by MirrorChyan and is kept as it is; the only change on our side is `environment: mirrorchyan`, which holds the upload token so that only runs on a `v*` tag can use it.
- A green CI run only means that the static, build or protocol checks that repository declares have passed. It does not mean a full build, a Windows start on real hardware, GPU or game compatibility has been verified, and it does not mean the full AUTO-MAS flow has been verified.
- Changes that cross repositories (protocol, version, or full-package integration) must state, in each repository involved, the baseline, the dependencies and what was actually verified. One repository's CI result cannot stand in for verification in another.
- We do not accept build CI: the source plus build output is about 124 GB, which GitHub's free machines cannot handle, and a self-hosted runner would let external PRs run arbitrary commands on that machine (see #5).

## Credit and licensing

- Open PRs from your own fork. CI for external PRs runs only after a maintainer approves it.
- Use your own name or a pseudonym, and a GitHub noreply address as your e-mail (`<id>+<username>@users.noreply.github.com`); do not add `Co-authored-by`. Your authorship is kept once the change is merged, and the release notes will thank you.
- Your change comes in under the license of the file it changes; see [Licenses and source code](README.en.md#licenses-and-source-code) in the README. Put your own name or pseudonym on the copyright line of new files. No CLA is needed, and `Signed-off-by` is optional.
- You may use AI tools, but the person who commits is still responsible for the change. For code changes that can be built or run on an instance, verify them for real as far as you can, and say in the PR which checks you actually ran and what the known limits are. The rules for AI tools are in [AGENTS.md](AGENTS.md) (in Chinese) at the repository root; Codex, Claude Code and similar tools read it on their own.
- General fixes may later be sent upstream to Google (AOSP, google/gfxstream), and upstream requires the author to sign Google's CLA personally. We will ask you first; if you would rather not, the change stays in ModAVD only.
- For changes to images and icons, open an issue first and state the source and license; fan art and images with an unclear source are not accepted.

## Testing on your own

- Shut instances down from AUTO-MAS or the manager when you can. In your own scripts, run `adb -s <device> shell sync` first and then `adb -s <device> emu kill`: without the sync, files a game just wrote can end up as 0 bytes.
- Do kill tests on a freshly created instance with a test account, not on an instance where your main account is logged in.
- Do not use adb on the default port 5037; take the port from `scriptAdbServerPort` in `mas-avd.json` (default 20049).

If you would like to help sort issues (reproduce, merge duplicates, add labels), ask the maintainer for Triage permission.
