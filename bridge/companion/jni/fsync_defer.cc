/*
 * ModAVD execution bridge — fsync-deferral module (x86_64 companion).
 * Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later.
 *
 * WHY (ModAVD#35)
 * ---------------
 * 明日方舟 (Arknights) downloads game resources at only ~8 MB/s on Windows. Its
 * download service (a Java/OkHttp worker that runs natively on x86 — it is NOT
 * translated) calls FileDescriptor.sync() -> fsync() after roughly every 7 KB it
 * writes to a "*.downloading" temp file. Each guest fsync is three block requests
 * (data, journal, commit); the virtual disk tops out near 1200 write+fsync/s, which
 * caps the download around 8 MB/s. With the same writes going to tmpfs (no fsync
 * cost) the download runs at 80–93 MB/s (see the issue). The bottleneck is the
 * per-block fsync, not the network or the translator.
 *
 * WHAT THIS DOES
 * --------------
 * Loaded only in listed packages (default: com.hypergryph.arknights), it defers
 * fsync ONLY for the download temp files and pays the fsync back when the file is
 * closed, renamed, or 5 s after the last real fsync. A temp file is therefore as
 * durable as today the moment it gets its final name; only an in-progress
 * "*.downloading" file can lose its last few seconds of content on a hard kill, and
 * the game re-downloads that on resume. Index files, SQLite, settings and account
 * data keep fsyncing normally.
 *
 * It is INDEPENDENT of the arm64 gate in native_to_guest_companion.cc: Arknights is
 * an x86 app that needs no translation and is not on modavd.gate.target. This module
 * only rewrites the libc-import GOT slots of libjavacore.so / libopenjdk.so — the
 * libraries behind java.io.FileDescriptor — inside the target app process. libc
 * itself and every other process are untouched; a non-target process never patches
 * anything.
 *
 * DEFAULT MODE IS OBSERVE
 * -----------------------
 * In observe mode the hooks are installed but every fsync still runs for real; the
 * module only records each call's path class and the close/rename ordering, so one
 * full download can confirm three things before any relaxation ships:
 *   (a) only "*.downloading" is fsync'd per block (other files fsync as before);
 *   (b) the final file is produced by a Java rename, not a copy;
 *   (c) whether the write fd is already closed by the time rename is called.
 * ACTIVE mode (modavd.fsync.defer.mode=active) is the real data-safety relaxation.
 * It is enabled only after the observe run and with the user's approval; the shipped
 * default is flipped to active at release time once the hard-kill regression passes.
 *
 * PROPERTIES (re-read when a fresh process arms; not persistent)
 *   modavd.fsync.defer       comma list of packages; compiled default below;
 *                            "none" disables the module entirely (kill switch).
 *   modavd.fsync.paths       comma list of glob patterns for the temp files;
 *                            default "*​/HGGameUpdate/HGDownload/*.downloading".
 *   modavd.fsync.defer.mode  "observe" (default) or "active".
 *   modavd.fsync.log         "1" logs every hooked call; default is a 60 s summary.
 */

#include <android/log.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <link.h>
#include <new>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#define FD_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ModAvdFsync", __VA_ARGS__)
#define FD_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ModAvdFsync", __VA_ARGS__)

namespace {

// ---- configuration read from properties at arm time ------------------------

// Packages the module arms in when modavd.fsync.defer is unset. Arknights only for
// now; add a package here (not to the gate list) when its download is measured.
constexpr char kDefaultPackages[] = "com.hypergryph.arknights";
// The download temp files. Plain fnmatch (NOT FNM_PATHNAME): a single '*' spans the
// many path segments between .../files and HGGameUpdate and between HGDownload and
// the uuid. Real path, for reference:
//   /storage/emulated/0/Android/data/com.hypergryph.arknights/files/
//       HGGameUpdate/HGDownload/file_<ts>/download/<uuid>.downloading
constexpr char kDefaultPaths[] = "*/HGGameUpdate/HGDownload/*.downloading";
constexpr long kPaybackAgeNs = 5L * 1000 * 1000 * 1000;  // 5 s
constexpr long kSummaryEveryNs = 60L * 1000 * 1000 * 1000;

enum Mode { kObserve = 0, kActive = 1 };

constexpr int kMaxPatterns = 8;
char g_patterns[kMaxPatterns][160];
int g_pattern_count = 0;
int g_mode = kObserve;
std::atomic<bool> g_log_each{false};
std::atomic<bool> g_installed{false};
char g_pkg[128] = {};

// ---- per-fd tracking --------------------------------------------------------
// Classification is populated the first time a tracked libc call (fsync/fdatasync)
// names an fd, then reused. A plain integer-indexed array keyed by fd: fds are small
// and reused, so this is O(1) with no allocation on the hot path. The common case —
// a close() of an fd we never fsync'd — reads one relaxed atomic (state kUnknown)
// and takes the real path without the lock.
enum FdState : uint8_t { kUnknown = 0, kUnmatched = 1, kMatched = 2 };

struct FdEntry {
  std::atomic<uint8_t> state{kUnknown};
  uint64_t dev = 0;  // from fstat, to detect fd-number reuse
  uint64_t ino = 0;
  long last_real_ns = 0;  // CLOCK_MONOTONIC of the last real fsync on this fd
  bool owed = false;      // a deferred fsync is outstanding (active mode)
};

FdEntry* g_fds = nullptr;
int g_fd_cap = 0;

// A small side list of currently-open matched fds, so rename() can find an open
// writer by its source path without storing a path per fd. Download holds ~2 at a
// time; 32 is generous.
constexpr int kMaxMatched = 32;
struct MatchedFile {
  int fd = -1;
  char path[512] = {};
};
MatchedFile g_matched[kMaxMatched];

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// Counters (summary line every 60 s, or per-call when modavd.fsync.log=1).
std::atomic<uint64_t> g_c_skipped{0};  // deferred (active mode)
std::atomic<uint64_t> g_c_paid{0};     // real fsync paid back on close/rename/age
std::atomic<uint64_t> g_c_real{0};     // matched fsync passed straight through
std::atomic<uint64_t> g_c_other{0};    // unmatched fsync passed through
std::atomic<uint64_t> g_c_renames{0};  // matched renames seen
long g_last_summary_ns = 0;

// ---- saved real libc functions (slot value before we patched) --------------
using FsyncFn = int (*)(int);
using CloseFn = int (*)(int);
using FdsanCloseFn = int (*)(int, uint64_t);
using RenameFn = int (*)(const char*, const char*);
using RenameatFn = int (*)(int, const char*, int, const char*);
using Renameat2Fn = int (*)(int, const char*, int, const char*, unsigned);

std::atomic<FsyncFn> g_real_fsync{nullptr};
std::atomic<FsyncFn> g_real_fdatasync{nullptr};
std::atomic<CloseFn> g_real_close{nullptr};
std::atomic<FdsanCloseFn> g_real_fdsan_close{nullptr};
std::atomic<RenameFn> g_real_rename{nullptr};
std::atomic<RenameatFn> g_real_renameat{nullptr};
std::atomic<Renameat2Fn> g_real_renameat2{nullptr};

long NowNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

// ---- GOT patching (x86_64, self-contained) ---------------------------------
// A compact import-slot locator, deliberately independent of the gate module's
// copy so this feature can be reasoned about and reverted on its own. Finds the
// RELA/REL slot a LOADED module uses for an imported symbol, flips the one page
// holding it, writes the replacement and restores the original protection
// (read-only for a slot already sealed in PT_GNU_RELRO).
struct SlotSearch {
  const char* module_needle = nullptr;
  const char* symbol = nullptr;
  void** slot = nullptr;
  int original_prot = PROT_READ;
};

int ProtForAddress(const dl_phdr_info* info, uintptr_t addr) {
  const uintptr_t base = static_cast<uintptr_t>(info->dlpi_addr);
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const Elf64_Phdr& ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_GNU_RELRO) continue;
    const uintptr_t b = base + ph.p_vaddr, e = b + ph.p_memsz;
    if (addr >= b && addr < e) return PROT_READ;
  }
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const Elf64_Phdr& ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD) continue;
    const uintptr_t b = base + ph.p_vaddr, e = b + ph.p_memsz;
    if (addr >= b && addr < e) {
      int p = 0;
      if (ph.p_flags & PF_R) p |= PROT_READ;
      if (ph.p_flags & PF_W) p |= PROT_WRITE;
      if (ph.p_flags & PF_X) p |= PROT_EXEC;
      return p != 0 ? p : PROT_READ;
    }
  }
  return PROT_READ;
}

int FindSlotCallback(struct dl_phdr_info* info, size_t, void* opaque) {
  auto* out = static_cast<SlotSearch*>(opaque);
  const char* soname = info->dlpi_name;
  if (soname == nullptr || strstr(soname, out->module_needle) == nullptr) return 0;
  const uintptr_t base = static_cast<uintptr_t>(info->dlpi_addr);
  const Elf64_Dyn* dynamic = nullptr;
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
      dynamic = reinterpret_cast<const Elf64_Dyn*>(base + info->dlpi_phdr[i].p_vaddr);
      break;
    }
  }
  if (dynamic == nullptr) return 0;

  const Elf64_Sym* symtab = nullptr;
  const char* strtab = nullptr;
  const Elf64_Rela* jmprel = nullptr;
  size_t jmprel_sz = 0;
  const Elf64_Rela* rela = nullptr;
  size_t rela_sz = 0;
  for (const Elf64_Dyn* d = dynamic; d->d_tag != DT_NULL; ++d) {
    switch (d->d_tag) {
      case DT_SYMTAB: symtab = reinterpret_cast<const Elf64_Sym*>(base + d->d_un.d_ptr); break;
      case DT_STRTAB: strtab = reinterpret_cast<const char*>(base + d->d_un.d_ptr); break;
      case DT_JMPREL: jmprel = reinterpret_cast<const Elf64_Rela*>(base + d->d_un.d_ptr); break;
      case DT_PLTRELSZ: jmprel_sz = d->d_un.d_val; break;
      case DT_RELA: rela = reinterpret_cast<const Elf64_Rela*>(base + d->d_un.d_ptr); break;
      case DT_RELASZ: rela_sz = d->d_un.d_val; break;
      default: break;
    }
  }
  if (symtab == nullptr || strtab == nullptr) return 0;

  auto scan = [&](const Elf64_Rela* r, size_t bytes) -> bool {
    if (r == nullptr) return false;
    for (size_t i = 0, n = bytes / sizeof(Elf64_Rela); i < n; ++i) {
      const unsigned type = ELF64_R_TYPE(r[i].r_info);
      if (type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT) continue;
      const Elf64_Sym& s = symtab[ELF64_R_SYM(r[i].r_info)];
      if (strcmp(strtab + s.st_name, out->symbol) != 0) continue;
      const uintptr_t addr = base + r[i].r_offset;
      out->slot = reinterpret_cast<void**>(addr);
      out->original_prot = ProtForAddress(info, addr);
      return true;
    }
    return false;
  };
  if (scan(jmprel, jmprel_sz)) return 1;
  if (scan(rela, rela_sz)) return 1;
  return 0;
}

// Patch one import slot; returns the previous (real) value through real_out.
bool PatchSlot(const char* module, const char* symbol, void* replacement, void** real_out) {
  SlotSearch found;
  found.module_needle = module;
  found.symbol = symbol;
  if (dl_iterate_phdr(&FindSlotCallback, &found) == 0 || found.slot == nullptr) return false;
  void* current = *found.slot;
  if (current == replacement) {  // already ours (inherited through fork COW)
    if (real_out != nullptr) *real_out = nullptr;
    return true;
  }
  if (current == nullptr) return false;
  const long ps = sysconf(_SC_PAGESIZE);
  if (ps <= 0) return false;
  const uintptr_t slot_addr = reinterpret_cast<uintptr_t>(found.slot);
  const uintptr_t page = slot_addr & ~static_cast<uintptr_t>(ps - 1);
  if ((slot_addr - page) + sizeof(void*) > static_cast<uintptr_t>(ps)) return false;
  if (mprotect(reinterpret_cast<void*>(page), ps, PROT_READ | PROT_WRITE) != 0) {
    FD_LOGE("mprotect(%s!%s) failed errno=%d", module, symbol, errno);
    return false;
  }
  if (real_out != nullptr) *real_out = current;
  __atomic_store_n(found.slot, replacement, __ATOMIC_RELEASE);
  const int restore = found.original_prot != 0 ? found.original_prot : PROT_READ;
  mprotect(reinterpret_cast<void*>(page), ps, restore);
  return true;
}

// ---- classification and the matched side list -------------------------------

bool PathMatches(const char* path) {
  for (int i = 0; i < g_pattern_count; ++i) {
    if (fnmatch(g_patterns[i], path, 0) == 0) return true;
  }
  return false;
}

// Resolve /proc/self/fd/N; returns length or -1.
ssize_t FdPath(int fd, char* buf, size_t cap) {
  char link[64];
  snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
  const ssize_t n = readlink(link, buf, cap - 1);
  if (n < 0) return -1;
  buf[n] = '\0';
  return n;
}

// Lock held. Add/refresh a matched fd in the side list.
void MatchedAdd(int fd, const char* path) {
  for (int i = 0; i < kMaxMatched; ++i) {
    if (g_matched[i].fd == fd) {
      strncpy(g_matched[i].path, path, sizeof(g_matched[i].path) - 1);
      g_matched[i].path[sizeof(g_matched[i].path) - 1] = '\0';
      return;
    }
  }
  for (int i = 0; i < kMaxMatched; ++i) {
    if (g_matched[i].fd < 0) {
      g_matched[i].fd = fd;
      strncpy(g_matched[i].path, path, sizeof(g_matched[i].path) - 1);
      g_matched[i].path[sizeof(g_matched[i].path) - 1] = '\0';
      return;
    }
  }
  // Full (should not happen for a download): leave untracked for rename lookup.
}

void MatchedRemove(int fd) {
  for (int i = 0; i < kMaxMatched; ++i) {
    if (g_matched[i].fd == fd) {
      g_matched[i].fd = -1;
      g_matched[i].path[0] = '\0';
      return;
    }
  }
}

// Lock held. Ensure g_fds[fd] is classified; (re)classify on fd reuse. Returns the
// state. Stores path into out_path (may be nullptr) when it classifies afresh.
uint8_t Classify(int fd, char* out_path, size_t path_cap) {
  if (fd < 0 || fd >= g_fd_cap) return kUnmatched;  // untracked -> behave as today
  FdEntry& e = g_fds[fd];
  struct stat st;
  const bool have_stat = (fstat(fd, &st) == 0);
  const uint8_t cached = e.state.load(std::memory_order_relaxed);
  if (cached != kUnknown && have_stat && e.dev == st.st_dev && e.ino == st.st_ino) {
    return cached;  // same file still on this fd
  }
  char path[512];
  const bool have_path = FdPath(fd, path, sizeof(path)) >= 0;
  const bool matched = have_path && PathMatches(path);
  e.dev = have_stat ? st.st_dev : 0;
  e.ino = have_stat ? st.st_ino : 0;
  e.last_real_ns = 0;
  e.owed = false;
  e.state.store(matched ? kMatched : kUnmatched, std::memory_order_release);
  if (matched) {
    MatchedAdd(fd, path);
    if (out_path != nullptr && have_path) {
      strncpy(out_path, path, path_cap - 1);
      out_path[path_cap - 1] = '\0';
    }
  }
  return matched ? kMatched : kUnmatched;
}

void MaybeSummaryLocked() {
  const long now = NowNs();
  if (now - g_last_summary_ns < kSummaryEveryNs) return;
  g_last_summary_ns = now;
  int open_matched = 0;
  for (int i = 0; i < kMaxMatched; ++i)
    if (g_matched[i].fd >= 0) ++open_matched;
  FD_LOGI("summary pkg=%s mode=%s open_matched=%d skipped=%llu paid=%llu real=%llu other=%llu renames=%llu",
          g_pkg, g_mode == kActive ? "active" : "observe", open_matched,
          (unsigned long long)g_c_skipped.load(), (unsigned long long)g_c_paid.load(),
          (unsigned long long)g_c_real.load(), (unsigned long long)g_c_other.load(),
          (unsigned long long)g_c_renames.load());
}

// ---- wrappers ---------------------------------------------------------------

int HandleSync(int fd, bool datasync) {
  const FsyncFn real =
      datasync ? g_real_fdatasync.load(std::memory_order_acquire) : g_real_fsync.load(std::memory_order_acquire);
  // A non-download fd never reaches the locked section after its first classify.
  if (fd >= 0 && fd < g_fd_cap &&
      g_fds[fd].state.load(std::memory_order_relaxed) == kUnmatched) {
    g_c_other.fetch_add(1, std::memory_order_relaxed);
    return real != nullptr ? real(fd) : 0;
  }

  pthread_mutex_lock(&g_lock);
  char path[512] = {};
  const uint8_t state = Classify(fd, path, sizeof(path));
  const bool log_each = g_log_each.load(std::memory_order_relaxed);
  int rc = 0;
  if (state != kMatched) {
    g_c_other.fetch_add(1, std::memory_order_relaxed);
    MaybeSummaryLocked();
    pthread_mutex_unlock(&g_lock);
    return real != nullptr ? real(fd) : 0;
  }

  FdEntry& e = g_fds[fd];
  const long now = NowNs();
  const bool stale = (e.last_real_ns == 0) || (now - e.last_real_ns >= kPaybackAgeNs);
  if (g_mode == kActive && !stale) {
    e.owed = true;
    g_c_skipped.fetch_add(1, std::memory_order_relaxed);
    if (log_each) FD_LOGI("defer %s fd=%d %s", datasync ? "fdatasync" : "fsync", fd, path);
    rc = 0;  // report success; the write is in the page cache, paid back later
  } else {
    // Observe mode always lands here; active mode lands here once per 5 s.
    if (real != nullptr) rc = real(fd);
    e.owed = false;
    e.last_real_ns = now;
    if (g_mode == kActive)
      g_c_paid.fetch_add(1, std::memory_order_relaxed);
    else
      g_c_real.fetch_add(1, std::memory_order_relaxed);
    if (log_each) FD_LOGI("real %s fd=%d %s rc=%d", datasync ? "fdatasync" : "fsync", fd, path, rc);
  }
  MaybeSummaryLocked();
  pthread_mutex_unlock(&g_lock);
  return rc;
}

int WrapFsync(int fd) { return HandleSync(fd, /*datasync=*/false); }
int WrapFdatasync(int fd) { return HandleSync(fd, /*datasync=*/true); }

// Pay a deferred fsync back before the fd stops existing / before close proceeds.
void PaybackAndForget(int fd) {
  if (fd < 0 || fd >= g_fd_cap) return;
  pthread_mutex_lock(&g_lock);
  FdEntry& e = g_fds[fd];
  if (e.state.load(std::memory_order_relaxed) == kMatched) {
    const bool log_each = g_log_each.load(std::memory_order_relaxed);
    if (g_mode == kActive && e.owed) {
      const FsyncFn real = g_real_fsync.load(std::memory_order_acquire);
      if (real != nullptr) real(fd);
      g_c_paid.fetch_add(1, std::memory_order_relaxed);
      if (log_each) FD_LOGI("payback-on-close fd=%d", fd);
    } else if (log_each) {
      FD_LOGI("close matched fd=%d owed=%d", fd, e.owed ? 1 : 0);
    }
    MatchedRemove(fd);
  }
  // The fd number is about to be free for reuse; drop any stale classification.
  e.state.store(kUnknown, std::memory_order_release);
  e.owed = false;
  e.last_real_ns = 0;
  pthread_mutex_unlock(&g_lock);
}

int WrapClose(int fd) {
  PaybackAndForget(fd);
  const CloseFn real = g_real_close.load(std::memory_order_acquire);
  return real != nullptr ? real(fd) : close(fd);
}

int WrapFdsanClose(int fd, uint64_t tag) {
  PaybackAndForget(fd);
  const FdsanCloseFn real = g_real_fdsan_close.load(std::memory_order_acquire);
  return real != nullptr ? real(fd, tag) : 0;
}

// Pay back any open matched writer whose path == the rename source, and log whether
// the source was still open (answers observe question (c)).
void HandleRenameSource(const char* abs_src) {
  if (abs_src == nullptr || abs_src[0] == '\0') return;
  const bool matched = PathMatches(abs_src);
  if (!matched) return;
  pthread_mutex_lock(&g_lock);
  g_c_renames.fetch_add(1, std::memory_order_relaxed);
  int open_fd = -1;
  for (int i = 0; i < kMaxMatched; ++i) {
    if (g_matched[i].fd >= 0 && strcmp(g_matched[i].path, abs_src) == 0) {
      open_fd = g_matched[i].fd;
      break;
    }
  }
  if (open_fd >= 0 && g_mode == kActive && open_fd < g_fd_cap && g_fds[open_fd].owed) {
    const FsyncFn real = g_real_fsync.load(std::memory_order_acquire);
    if (real != nullptr) real(open_fd);
    g_fds[open_fd].owed = false;
    g_fds[open_fd].last_real_ns = NowNs();
    g_c_paid.fetch_add(1, std::memory_order_relaxed);
  }
  if (g_log_each.load(std::memory_order_relaxed))
    FD_LOGI("rename src=%s still_open_fd=%d", abs_src, open_fd);
  pthread_mutex_unlock(&g_lock);
}

// Build an absolute path for a rename argument (download paths are already absolute;
// handle a dirfd/relative best-effort so the observe log is honest).
void ResolveAt(int dirfd, const char* path, char* out, size_t cap) {
  out[0] = '\0';
  if (path == nullptr) return;
  if (path[0] == '/') {
    strncpy(out, path, cap - 1);
    out[cap - 1] = '\0';
    return;
  }
  char dir[512];
  if (dirfd == AT_FDCWD) {
    if (getcwd(dir, sizeof(dir)) == nullptr) return;
  } else if (FdPath(dirfd, dir, sizeof(dir)) < 0) {
    return;
  }
  snprintf(out, cap, "%s/%s", dir, path);
}

int WrapRename(const char* oldp, const char* newp) {
  char abs[512];
  ResolveAt(AT_FDCWD, oldp, abs, sizeof(abs));
  HandleRenameSource(abs);
  const RenameFn real = g_real_rename.load(std::memory_order_acquire);
  return real != nullptr ? real(oldp, newp) : rename(oldp, newp);
}

int WrapRenameat(int ofd, const char* oldp, int nfd, const char* newp) {
  char abs[512];
  ResolveAt(ofd, oldp, abs, sizeof(abs));
  HandleRenameSource(abs);
  const RenameatFn real = g_real_renameat.load(std::memory_order_acquire);
  return real != nullptr ? real(ofd, oldp, nfd, newp) : renameat(ofd, oldp, nfd, newp);
}

int WrapRenameat2(int ofd, const char* oldp, int nfd, const char* newp, unsigned flags) {
  char abs[512];
  ResolveAt(ofd, oldp, abs, sizeof(abs));
  HandleRenameSource(abs);
  const Renameat2Fn real = g_real_renameat2.load(std::memory_order_acquire);
  if (real != nullptr) return real(ofd, oldp, nfd, newp, flags);
  return renameat2(ofd, oldp, nfd, newp, flags);
}

// ---- fork child: a forked worker inherits the table COW but not its validity ----
void ForgetAfterFork() {
  for (int i = 0; i < g_fd_cap; ++i) {
    g_fds[i].state.store(kUnknown, std::memory_order_relaxed);
    g_fds[i].owed = false;
    g_fds[i].last_real_ns = 0;
  }
  for (int i = 0; i < kMaxMatched; ++i) {
    g_matched[i].fd = -1;
    g_matched[i].path[0] = '\0';
  }
  // The mutex is not held across fork() in any path here; reinit defensively.
  pthread_mutex_t fresh = PTHREAD_MUTEX_INITIALIZER;
  memcpy(&g_lock, &fresh, sizeof(g_lock));
}

// ---- configuration parsing --------------------------------------------------

bool PackageOnList(const char* list, const char* pkg) {
  for (const char* p = list; *p != '\0';) {
    const char* comma = strchr(p, ',');
    const size_t len = comma != nullptr ? static_cast<size_t>(comma - p) : strlen(p);
    if (len != 0 && strncmp(pkg, p, len) == 0) {
      const char c = pkg[len];
      if (c == '\0' || c == ':') return true;  // main process or a :subprocess
    }
    p = comma != nullptr ? comma + 1 : p + len;
  }
  return false;
}

void ParsePatterns(const char* list) {
  g_pattern_count = 0;
  for (const char* p = list; *p != '\0' && g_pattern_count < kMaxPatterns;) {
    const char* comma = strchr(p, ',');
    size_t len = comma != nullptr ? static_cast<size_t>(comma - p) : strlen(p);
    if (len >= sizeof(g_patterns[0])) len = sizeof(g_patterns[0]) - 1;
    if (len != 0) {
      memcpy(g_patterns[g_pattern_count], p, len);
      g_patterns[g_pattern_count][len] = '\0';
      ++g_pattern_count;
    }
    p = comma != nullptr ? comma + 1 : p + len;
  }
}

bool InstallHooks() {
  const char* const kJavacore = "libjavacore.so";
  const char* const kOpenjdk = "libopenjdk.so";
  int patched = 0;

  // One wrapper, possibly two slots per symbol (both libs import it). Save the real
  // value once; the libc target is the same object behind either slot.
  auto patch_both = [&](const char* sym, void* wrapper, auto store_real) {
    void* real = nullptr;
    const bool a = PatchSlot(kJavacore, sym, wrapper, &real);
    if (a && real != nullptr) store_real(real);
    void* real2 = nullptr;
    const bool b = PatchSlot(kOpenjdk, sym, wrapper, &real2);
    if (b && real2 != nullptr) store_real(real2);
    if (a || b) ++patched;
    return a || b;
  };

  patch_both("fsync", reinterpret_cast<void*>(&WrapFsync),
             [](void* r) { g_real_fsync.store(reinterpret_cast<FsyncFn>(r), std::memory_order_release); });
  patch_both("fdatasync", reinterpret_cast<void*>(&WrapFdatasync),
             [](void* r) { g_real_fdatasync.store(reinterpret_cast<FsyncFn>(r), std::memory_order_release); });
  patch_both("close", reinterpret_cast<void*>(&WrapClose),
             [](void* r) { g_real_close.store(reinterpret_cast<CloseFn>(r), std::memory_order_release); });
  patch_both("android_fdsan_close_with_tag", reinterpret_cast<void*>(&WrapFdsanClose),
             [](void* r) { g_real_fdsan_close.store(reinterpret_cast<FdsanCloseFn>(r), std::memory_order_release); });
  patch_both("rename", reinterpret_cast<void*>(&WrapRename),
             [](void* r) { g_real_rename.store(reinterpret_cast<RenameFn>(r), std::memory_order_release); });
  patch_both("renameat", reinterpret_cast<void*>(&WrapRenameat),
             [](void* r) { g_real_renameat.store(reinterpret_cast<RenameatFn>(r), std::memory_order_release); });
  patch_both("renameat2", reinterpret_cast<void*>(&WrapRenameat2),
             [](void* r) { g_real_renameat2.store(reinterpret_cast<Renameat2Fn>(r), std::memory_order_release); });

  // fsync is the one that must be present; without it the module does nothing.
  if (g_real_fsync.load(std::memory_order_acquire) == nullptr) {
    FD_LOGE("fsync import slot not found in libjavacore/libopenjdk; module inert");
    return false;
  }
  FD_LOGI("hooks installed: %d symbols (fsync real=%p)", patched,
          reinterpret_cast<void*>(g_real_fsync.load()));
  return true;
}

}  // namespace

// Called from the shim's initialize() in each freshly specialized app process, with
// the package name. A no-op unless the package is on modavd.fsync.defer (default:
// Arknights). Idempotent per process.
extern "C" __attribute__((visibility("default"))) void modavd_fsync_defer_init(const char* pkg) {
  if (pkg == nullptr || pkg[0] == '\0') return;
  if (g_installed.load(std::memory_order_acquire)) return;

  char list[PROP_VALUE_MAX] = {};
  if (__system_property_get("modavd.fsync.defer", list) <= 0 || list[0] == '\0') {
    strncpy(list, kDefaultPackages, sizeof(list) - 1);
  }
  if (strcmp(list, "none") == 0) return;  // kill switch
  if (!PackageOnList(list, pkg)) return;

  char paths[PROP_VALUE_MAX] = {};
  if (__system_property_get("modavd.fsync.paths", paths) <= 0 || paths[0] == '\0') {
    strncpy(paths, kDefaultPaths, sizeof(paths) - 1);
  }
  ParsePatterns(paths);
  if (g_pattern_count == 0) {
    FD_LOGE("no usable path pattern; not arming for %s", pkg);
    return;
  }

  char mode[PROP_VALUE_MAX] = {};
  __system_property_get("modavd.fsync.defer.mode", mode);
  g_mode = (strcmp(mode, "active") == 0) ? kActive : kObserve;  // observe is the safe default
  char logv[PROP_VALUE_MAX] = {};
  __system_property_get("modavd.fsync.log", logv);
  g_log_each.store(logv[0] == '1', std::memory_order_relaxed);

  // Size the fd table to the process's fd limit, capped so a huge rlimit cannot ask
  // for an unreasonable allocation. Untracked high fds fall back to real fsync.
  struct rlimit rl;
  long cap = 4096;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
    cap = static_cast<long>(rl.rlim_cur);
  }
  if (cap < 1024) cap = 1024;
  if (cap > 65536) cap = 65536;
  g_fds = new (std::nothrow) FdEntry[cap];
  if (g_fds == nullptr) {
    FD_LOGE("fd table alloc failed (cap=%ld); not arming", cap);
    return;
  }
  g_fd_cap = static_cast<int>(cap);
  for (int i = 0; i < kMaxMatched; ++i) g_matched[i].fd = -1;

  strncpy(g_pkg, pkg, sizeof(g_pkg) - 1);
  g_last_summary_ns = NowNs();
  pthread_atfork(nullptr, nullptr, &ForgetAfterFork);

  if (!InstallHooks()) {
    delete[] g_fds;
    g_fds = nullptr;
    g_fd_cap = 0;
    return;
  }
  g_installed.store(true, std::memory_order_release);
  FD_LOGI("armed pkg=%s mode=%s patterns=%d fd_cap=%d log=%d", pkg,
          g_mode == kActive ? "active" : "observe", g_pattern_count, g_fd_cap,
          g_log_each.load() ? 1 : 0);
}
