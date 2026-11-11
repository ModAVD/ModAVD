# Release and build tools

[简体中文](README.md) | **English**

<!-- README.md and README.en.md match section by section: if you change one, change the other in the same PR. -->

Tools for the maintainer and platform leads; they do not ship in the package. The entry points are all PowerShell 7 (`pwsh`), and helper code uses only the Python 3.12 standard library.
The pre-release regression (end-to-end tests) lives in a separate repository, [ModAVDE2E](https://github.com/ModAVD/ModAVDE2E): it tests the installed full package, spans several repositories, needs a test machine and game instances, and is not released together with this repository.

| Directory | What it does |
| --- | --- |
| [`build/`](build/) | `build-sdk.ps1`: incrementally builds `sdk-masN` from the AOSP emulator source tree, records the commit of each repository at build time, and puts it into the execution bridge payload |
| [`gate/`](gate/README.md) | `stage-payload.ps1`: checks the pinned hashes of the three arm64 execution bridge payload files and puts them into the SDK tree |
| [`pubrel/`](pubrel/README.md) | `release.ps1`: assembles the package, exports and replays patches, builds the source bundle and the Release notes, runs the privacy scan, and can optionally create a GitHub draft |

The checks used by CI (`checks.py`, `privacy.py`, the allowlist) are in [`scripts/`](../scripts/) in this repository, and the release tools use that same copy directly.
The release checklist is in [RELEASING.md](../RELEASING.md).

## Local configuration

The repository hard-codes no local paths. Anything specific to this computer (where the source tree is, where temporary files go, where external inputs are) is given as an argument on the command line; if you do not want to pass it every time, put it in the local configuration:

1. Copy [`local.example.psd1`](local.example.psd1) to `local.psd1` in the same directory (it is already in `.gitignore` and will not be committed) and fill it in following the comments;
2. Or keep it outside the repository and point to it with the environment variable `MODAVD_TOOLS_CONFIG` (handy when several clones share one copy).

The reading rules are in [`local.ps1`](local.ps1): arguments take priority, then the local configuration; a path that is in neither is an error, and no default is guessed.
If `MODAVD_TOOLS_CONFIG` is set but points to a file that does not exist, that is also an error.

The private privacy rules `privacy-private-rules.json` are likewise kept outside the repository and pointed to with `PrivateRules` in the local configuration or the environment variable `MODAVD_PRIVATE_RULES`;
never copy them into the repository (this file name is also blocked in `.gitignore`).

## Safety conventions

- Output and logs are written only to the temporary directory (`TempRoot` in the local configuration). The release output directory and the release clone must not land inside the source tree, the SDK, or the directories listed in `ProtectedRoots` in the local configuration (for example backups or the production root directory).
- The source tree is read-only: a build requires all source to be committed, and the artifacts are installed only if the commits of each repository are the same before and after the build; a release takes things only from commits.
- Always run `adb shell sync` and then `emu kill` when shutting down an instance; adb uses only the private port, never touches 5037, and never touches the port of the production root directory.
