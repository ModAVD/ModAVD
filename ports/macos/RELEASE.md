# macOS ARM64 release packaging

This is a local, explicit release procedure. It does not change GitHub Actions,
upload assets, create tags or publish a release. Stage 13 supplies `package.py`
and the Manager builder; the Manager native updater must already be integrated.

Use a clean, exact Manager commit and the pinned Go toolchain. Build the package
with a ModAVD version `X.Y.Z` and core build `Y`. Its `source.properties` must
declare `Pkg.BuildId=mas-Y`, and the headless QEMU must contain the real
`free-page-reporting` implementation. The assembler's `official-validation`
profile is for local SDK acceptance only; it cannot produce a release update.
Never edit the official SDK's properties to impersonate a custom core.

## Local update archive

Finish all signing before assembly records file digests. Keep the entire package
directory together, including its inventory and provenance. Choose a new output
directory and retain at least 20 GiB free throughout:

```sh
python3 ports/macos/package.py verify --output /absolute/path/ModAVD
python3 ports/macos/release.py \
  --source /absolute/path/ModAVD \
  --output /absolute/path/assets/ModAVD-0.38.2-macos-arm64.zip
shasum -a 256 /absolute/path/assets/ModAVD-0.38.2-macos-arm64.zip
```

The ZIP is the updater payload, without an enclosing directory. It includes the
signed emulator, app, tools, components, licenses and provenance. It excludes
platform-tools, system images, AVDs, logs, settings, credentials and user data.
The root transport manifest is `modavd-macos-update-v1`, platform
`macos-arm64`. Signature attributes are captured only from the supported
`com.apple.cs.*` allowlist. The generator verifies the source inventory and
signatures before and after streaming, enforces updater resource limits and
refuses to overwrite an existing archive.

The default archive has local development signing requirements. Ad hoc signatures
check integrity; they provide no publisher identity or notarization assurance.
Treat it as a local development artifact, not a public distribution.

## Public distribution

Build the Manager with an explicit `--sign-identity` for the release maintainer's
Developer ID Application identity. This enables hardened runtime and secure
timestamps. Add `--notary-profile` only when intending to submit the app to Apple
using that existing keychain profile. Submission, acceptance, stapling and
Gatekeeper checks finish before the build receipt and file hashes are recorded;
without this option the builder makes no notary requests. The default identity
remains ad hoc. No credentials are stored in the receipt.

Sign the custom emulator with the same identity. Preserve the emulator's required
virtualization/JIT entitlements when signing; do not use an indiscriminate deep
resign that replaces nested entitlements. Hardened runtime, secure timestamp and
the complete nested library set must be reviewed for the actual emulator build.
Never commit certificates, private keys, passwords or notary profiles.

The builder's app submission covers the app; submit the complete native payload
to Apple's notary service separately using a maintainer-managed `notarytool`
keychain profile. Keep Apple's acceptance record for that payload. Assemble from those
final signed inputs so stapling/signing cannot invalidate the package inventory.
Generate the update archive with `release.py --distribution` to additionally
require Developer ID, hardened runtime, a valid stapled app ticket and a passing
Gatekeeper assessment. These checks do not submit anything to Apple.

Before uploading, exercise `update prepare`, `update install` and `update recover`
on a disposable copy with the actual archive and its independently recorded
SHA256. Verify that guest data and existing SDK components survive. Repeat
Finder launch, CLI/MCP, real HVF boot, window/headless transitions, daily
operations, disk compaction and report collection on the actual signed build.
Keep regression evidence in the tracking issue, not in the source tree. Do not
label an unrun or resource-blocked check as passing.

The matching ModAVD release tag is `vX.Y.Z`. Upload exactly
`ModAVD-X.Y.Z-macos-arm64.zip`; the Manager's GitHub selector uses this name and
requires its asset size and `digest` field (`sha256:…`). Compare GitHub's digest
and byte size with the local result after upload. Publish the SHA256 through an
independent trusted channel for local `--package` users. Keep Windows assets and
their release process unchanged. A missing macOS asset remains
`update_unavailable` and must never select a Windows package.

Publishing, signing credentials, Apple approval and machine acceptance are
maintainer actions. This procedure and archive creation do not publish a release.
