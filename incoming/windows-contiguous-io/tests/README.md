# Windows contiguous-vector I/O verification

Run on Windows with Python 3 and an installed Visual Studio C/C++ toolchain:

```powershell
python run.py --repo <QEMU-checkout> --baseline <baseline-commit> --vcvars <VC-Auxiliary-Build-vcvars64.bat> --work <new-scratch-directory> --benchmark --rounds 6
```

The candidate must contain the contiguous-vector change and differ from the
baseline. The script extracts the actual old and new `handle_aiocb_rw_bounce`,
`handle_aiocb_rw`, and `aio_worker` functions, plus the candidate's predicate. It
compiles them as C11 with MSVC. Lightweight adapters provide QEMU types, iovec
copy operations and allocation accounting. The tests use real `ReadFile`,
`WriteFile`, and `FlushFileBuffers` calls. Explicit error and allocation cases
inject their stated fault before the real API call.

The work directory receives generated source, compiler output, `checks.txt`,
`benchmark.csv`, `results.json`, and two scratch files. It must be on a writable
Windows filesystem supporting sparse files; the large-offset test gives one
file a logical length above 4 GiB. The script requires a new work directory. It
does not remove directories or write files outside that directory.

Functional coverage includes:

- Contiguous and fragmented reads/writes around 64 KiB, and 1 MiB requests.
- Independent readback and file-length checks, including offsets above 4 GiB.
- Original aligned buffers with `FILE_FLAG_NO_BUFFERING`, tested at 8 KiB.
- Real EOF and invalid handles; injected access, EOF, disk-full, short-write,
  and flush failures.
- Bounce-allocation failure, zero-length vector fallback, size mismatch,
  `DWORD` limits, and pointer arithmetic overflow.
- Expected Windows call and allocation counts, including unchanged fragmented
  fallback and unchanged real flush calls.

The benchmark alternates baseline/candidate order every round after warmup and
records every sample. It repeatedly reads/writes the same range of a cached
scratch file. It includes request allocation and the extracted synchronous
worker; it excludes QEMU's thread pool, qcow2 metadata, guest filesystem,
network, game downloader, and per-request durability barriers. The final flush
of each benchmark case is outside the timed sample. Results measure this host
function only. They do not establish emulator throughput, game compatibility,
power-loss durability, or the frequency of contiguous requests in real games.
Real I/O cases cover requests up to 1 MiB. The `DWORD` and pointer-overflow
boundaries are predicate checks without corresponding large I/O operations.

## Complete emulator and guest writes

The complete Windows headless builds and their source, object and executable
hashes are recorded in [full-emulator-build.json](results/full-emulator-build.json).
Use the same dependencies and configuration for both versions. For an
incremental build, verify that the final executable is actually relinked:
the current build passes `qemu2-common` through raw `wholearchive` flags, so
rebuilding that archive alone can leave an unchanged executable. A successful
compiler exit or changed object file is not enough.

`syncbench.cc` is an Android x86_64 probe. Each worker appends 7,168 bytes and
calls real `fsync` on every iteration. Arguments select 1–8 workers and 1–120
seconds; defaults are one worker and ten seconds. Build it with the Android NDK
C++ compiler and static libc++. Run only on a disposable test instance after
checking that `/data/local/tmp/modavd-syncbench` is a real, empty directory and
that no other probe or game is running. It creates and removes only its own
`work-<id>.bin` files in that directory. A failed run must be investigated before
reusing the directory.

Alternate the two compiled emulator versions, preserve all samples, and verify
the running executable hash before each measurement. This probe measures guest
write-plus-fsync throughput; it does not prove power-loss durability or a game
download speedup. The three formal alternating pairs and all retained samples
are in [native-fsync.json](results/native-fsync.json).

## Download observations

`measure_download.py` reads counters from a selected running test instance. It
checks the instance path, process executable, executable hash and launch time,
then uses `scriptAdbServerPort` from that root's configuration. It follows the
Manager's `sdkRoot` override: relative values resolve against the selected root,
absolute values select that SDK, and an empty or absent value uses `<root>/sdk`.
Both QEMU and adb must come from the selected SDK. The script accepts its GUI
or headless QEMU executable, rejects port 5037, and records the guest boot ID
and game PIDs with each sample. It stops if either changes during the window.
The game must already be downloading through its own UI. This path was also
smoke-tested for ten seconds on the compiled candidate headless build while
Arknights was downloading.

```powershell
python measure_download.py --root <test-root> --index <test-index> --package <package-name> --expected-exe-sha256 <tested-qemu-hash> --build-label <tested-build> --seconds 150 --out <result.json>
```

The script reads network counters for the instance, block writes and allocated
space in the application's files directories. It does not install or launch
games, click their UI, change game files, or restart the instance. The executable
hash identifies the observed binary; record source-to-binary provenance
separately. A window spanning unpacking or an idle login page is not a steady
download-speed measurement. Record the observed UI phase with each result.

Candidate game observations are in [compiled-games.json](results/compiled-games.json).
The older released-build observations remain in [games.json](results/games.json)
and the individual released-build window files. Neither set is a controlled
baseline/candidate speed comparison. [RESULTS.md](RESULTS.md) distinguishes
the builds, completed observations, and limits.
