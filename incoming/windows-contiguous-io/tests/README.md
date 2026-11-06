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
- Original aligned buffers with `FILE_FLAG_NO_BUFFERING`.
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

## Download observations

`measure_download.py` reads counters from a selected running test instance. It
checks the instance path, process executable, executable hash and launch time,
then uses `scriptAdbServerPort` from that root's configuration. It rejects port
5037. The game must already be downloading through its own UI.

```powershell
python measure_download.py --root <test-root> --index <test-index> --package <package-name> --expected-exe-sha256 <tested-qemu-hash> --build-label <tested-build> --seconds 150 --out <result.json>
```

The script reads network counters for the instance, block writes and allocated
space in the application's files directories. It does not install or launch
games, click their UI, change game files, restart the instance or claim that
the candidate patch is installed. A window spanning unpacking or an idle login
page is not a steady download-speed measurement. Record the observed UI phase
with each result. The committed game records use the released emulator;
they are not a baseline/candidate comparison.
