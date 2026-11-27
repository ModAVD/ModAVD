# gate: put the arm64 execution bridge payload into the SDK

[简体中文](README.md) | **English**

<!-- README.md and README.en.md match section by section: if you change one, change the other in the same PR. -->

What the execution bridge is, why it is needed and how it is installed into an instance: see [bridge/README.en.md](../../bridge/README.en.md). This page covers only the one step at release time: putting the bridge's three payload files into the SDK tree so that they go into the package together with `sdk\emulator`.

| File | Comes from | Installed into the instance at | SELinux label |
| --- | --- | --- | --- |
| `libmodavd_bridge.so` | `bridge\nbshim\`, built by `bridge\scripts\build-nbshim.ps1` | `/system/lib64/` | `u:object_r:system_lib_file:s0` |
| `libmodavd_gate.so` | `bridge\companion\`, built by `bridge\scripts\build-companion.ps1` | `/system/lib64/` | `u:object_r:system_lib_file:s0` |
| `modavd_bridge.rc` | `bridge\nbshim\modavd_bridge.rc`, the file as it is | `/system/etc/init/` | `u:object_r:system_file:s0` |

A line `ro.dalvik.vm.native.bridge=libmodavd_bridge.so` is also appended to `/vendor/build.prop`. All three files are `chmod 644`.

## Where they are in the package

```
<ModAVD root>\sdk\emulator\resources\modavd\libmodavd_bridge.so
<ModAVD root>\sdk\emulator\resources\modavd\libmodavd_gate.so
<ModAVD root>\sdk\emulator\resources\modavd\modavd_bridge.rc
<ModAVD root>\tools\modavd-bridge.ps1                            install / uninstall / verify (source in package\)
```

They sit under `sdk\emulator`: the "replace only the `sdk\emulator` folder" way of upgrading during the internal test brings them along automatically, and the updater already manages all of `sdk/emulator/**`, so an incremental update needs no new rule. The payload and the emulator are locked to the same build number, so there is no "new emulator + old companion library".

> They are not in `components\` (where the host IME lives): the rule there accepts only `*.apk`, which would mean changing the updater, `component_apks()` in `release_tools.py`, the parameters of `release.ps1` and the unit tests, and updating two files separately would mismatch them.

## How it gets into the package

1. **Build the payload and pin the hashes.** The payload is not in git (the source is in the repository, the output is not). You need Android NDK r27c:

   ```powershell
   $env:ANDROID_NDK_ROOT = '<NDK r27c directory>'
   pwsh -File bridge\scripts\build-nbshim.ps1 [-OutDir <directory>]
   pwsh -File bridge\scripts\build-companion.ps1 [-OutDir <directory>]
   ```

   `stage-payload.ps1` looks for the payload in this order: `-Payload <a frozen directory>`; `-Bridge` / `-Gate`; `GatePayload` in the local config, then `BridgeSo` / `GateSo` (`tools\local.example.psd1`); last, the output location of the two scripts above when run without `-OutDir`. The `.rc` is by default `bridge\nbshim\modavd_bridge.rc` in this repository. Building the same source in different directories gives outputs that differ only in `.note.gnu.build-id`, so the hash follows the build directory: after you change the directory or rebuild, run `-UpdateSums` once to pin again, and this binary change then shows up in git diff.
2. **Put it into the SDK tree.** `tools\build\build-sdk.ps1` does this by itself after the build; you can also run it alone on an existing SDK tree.

```powershell
# Check the payload only (no SDK tree needed)
pwsh -NoProfile -File tools\gate\stage-payload.ps1 -CheckOnly

# After rebuilding the payload: pin the hashes again
pwsh -NoProfile -File tools\gate\stage-payload.ps1 -UpdateSums

# Put it into the SDK tree (build-sdk.ps1 calls this itself; -Payload can point at a frozen directory)
pwsh -NoProfile -File tools\gate\stage-payload.ps1 -Sdk <SDK root>
```

- The hashes are pinned in `payload.SHA256SUMS.txt`, in the same format as `tools\pubrel\config\components.SHA256SUMS.txt` (`<lowercase sha256><two spaces><resources/modavd/file name>`, usable directly with `sha256sum -c`). **If a hash does not match, nothing is put in place and nothing is packaged.**
- `build-sdk.ps1` runs a read-only `-CheckOnly` **before** `ShouldProcess`, so `-WhatIf` checks too: this avoids finding out only after a build of more than an hour that the payload was not built or not pinned again.
- The release `assemble` step compares `tools\pubrel\config\sdk-inventory.csv` for **exact equality**: one missing file stops it with `SDK inventory changed`, so a forgotten payload cannot be shipped silently, but those three lines in the CSV must be there too.

## Diagnostics

Log tags: `ModAvdNB` for the shim, `ModAvdGate` for the companion library. It is most accurate to capture from zero after a cold boot:

```powershell
adb -P <private adb port> -s emulator-<port> logcat -b all -d -s ModAvdNB:V ModAvdGate:V
```

The key lines of the shim:

- `shim-ctor OK version=<6|8> wrapped=...`: forwarding is ready (`modavd-bridge.ps1 verify` looks at this line).
- `shim-ctor real itf=... version=<n> copy_bytes=<144|160|168>`: the real structure was found, version n.
- `shim-ctor companion libmodavd_gate.so loaded handle=...`: the companion library is up as well.
- `shim-ctor companion dlopen(...) failed -> ... (bridge still up, gate absent)`: the companion library is missing. The bridge entry is there but the execution bridge does not work; the game still crashes as before, but arm64 itself is still fine.
- `shim-ctor FAILED` / `shim-ctor INERT`: the real translation layer was not opened. `version` then stays 0, and `libnativebridge` cleanly gives up before ART calls `initialize`: the device boots normally and x86_64 is fine, there is just no arm64.

Optional properties (it runs without any of them, and none is persistent):

| Property | Default | What it does |
| --- | --- | --- |
| `modavd.nb.log` | on | Set `0` / `false` / `off` to turn off the shim's per-call log; the constructor's log is always printed |
| `modavd.nb.real` | `libndk_translation.so` | Library name or path of the real translation layer |
| `modavd.nb.companion` | `libmodavd_gate.so` | Set `0` / `off` to not load the companion library (forwarding only) |
| `modavd.gate.target` | the list compiled into the companion library | If set, it replaces the list as a whole; `none` means off |
| `modavd.gate.slotstrict` | `0` | Make the slot check strict. For diagnosis only, not a fix |
| `modavd.gate.logsig` / `probe` / `revprobe` | `0` | Per-point log of the signal path / observe only, no bridging |
