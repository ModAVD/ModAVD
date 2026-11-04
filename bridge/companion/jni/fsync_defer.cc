/*
 * ModAVD execution bridge — fsync-deferral module (x86_64 companion).
 * Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later.
 *
 * WHY (ModAVD#35)
 * ---------------
 * 明日方舟 (Arknights) downloads game resources at only ~6–8 MB/s on Windows (measured
 * 6.27 MB/s on an idle machine). It ships ONLY arm64 code and runs under NDK Translation;
 * its download worker fsyncs each "*.downloading" temp file roughly every few KB it
 * writes. Each guest fsync is several block requests (data, journal, commit); the virtual
 * disk caps the download there. With the same writes going to tmpfs (no fsync cost) the
 * download runs at 80–93 MB/s (see the issue). The bottleneck is the per-block fsync, not
 * the network or the translator. The fsync is a NATIVE arm64 call (not Java
 * FileDescriptor.sync), which is why the earlier Java-level GOT hook saw none of them.
 *
 * WHERE IT HOOKS (design v3, "B": seccomp + USER_NOTIF)
 * -----------------------------------------------------
 * Arknights ships only arm64 libraries, so the whole game runs under the NDK
 * Translation layer. The translator issues the final host x86_64 fsync with an INLINE
 * syscall instruction, not through the libc syscall() it imports; patching the GOT of
 * the translator ("C'", the previous step) was measured to catch nothing. So this
 * module stays out of userspace plumbing and filters the one thing that cannot be
 * bypassed: the host x86_64 fsync(74)/fdatasync(75) system call itself, with a classic
 * seccomp-BPF filter that answers SECCOMP_RET_USER_NOTIF for those two numbers and
 * SECCOMP_RET_ALLOW for everything else (close/rename/etc. are NOT filtered). A
 * supervisor thread in the same process reads the notifications from the filter's
 * listener fd. The game's arm64 libraries are never modified.
 *
 * It is INDEPENDENT of the arm64 gate in native_to_guest_companion.cc (separate
 * state). A non-target process never installs anything.
 *
 * MODES
 * -----
 * observe (default): every notification is answered at once with
 * SECCOMP_USER_NOTIF_FLAG_CONTINUE, i.e. the kernel runs the fsync exactly as if no
 * filter existed. Only AFTER that reply does the supervisor classify the fd (readlink
 * /proc/self/fd/N against modavd.fsync.paths) and count it. It confirms seccomp really
 * sees the guest's fsync before any relaxation is built on top.
 *
 * active (modavd.fsync.defer.mode=active): for a notification on an fd ALREADY classified
 * as a matched .downloading file (same dev/ino, this process, with a payback slot), the
 * supervisor answers val=0/error=0 WITHOUT CONTINUE — the kernel returns success to the
 * guest and skips the fsync (deferred) — and stamps the file's slot. A scanner thread
 * pays the file back out of band (open + magic fsync + close, at most once per
 * kPaybackEveryNs), bounding a hard-kill's loss for an in-flight file to that window.
 * The FIRST fsync on each fd, an fd whose identity cannot be confirmed, a foreign
 * (forked-child) pid, or a full matched list all CONTINUE — the safe direction. close and
 * rename are not filtered: a renamed/deleted file just drops from the table (its data
 * reaches disk via normal writeback; a crash in the last window re-fetches that file's
 * tail, which is the downloader's normal resume). If the scanner cannot start, active
 * falls back to observe so nothing is ever deferred without a payer.
 *
 * SAFETY STRUCTURE (design §5, §9)
 * --------------------------------
 *  1. A seccomp filter cannot be removed once installed. So the supervisor thread is
 *     created FIRST and parked on a condition variable; only then is the filter
 *     installed; then the listener fd is handed to the supervisor. If any earlier step
 *     fails (NO_NEW_PRIVS, thread create, seccomp()), nothing is installed and fsync
 *     behaves exactly as today (module inert). The bad state "filter without supervisor"
 *     can therefore not arise from our own error paths.
 *  2. The notification path never blocks: the only work before the reply is the active
 *     defer decision (atomic fd-state reads plus one fstat/access — no lock, no readlink,
 *     no open, no allocation, no real fsync), then SEND. All classification (readlink),
 *     payback (open+fsync) and logging happen AFTER the reply or on other threads. Any
 *     per-notification error is counted and swallowed; the loop never exits. (A
 *     persistent error on the listener fd only slows the loop with a short sleep so
 *     it cannot spin.) A separate watchdog thread reports a supervisor that is stuck
 *     or gone, and prints the periodic summary, so the supervisor never logs on a
 *     timer.
 *  3. The listener fd lives in the game's fd table, so a stray close() by the game (or
 *     fd-number reuse) would make every later fsync fail with ENOSYS. It is moved to a
 *     high number (>= 900, above where games allocate) with F_DUPFD_CLOEXEC and given
 *     an fdsan owner tag, so an accidental close is reported by fdsan with our tag.
 *  4. fork: a child inherits the filter AND the listener fd (the supervisor thread does
 *     not exist in the child, but notifications are delivered to the parent's listener
 *     file, so the parent's supervisor still answers them). The supervisor therefore
 *     only classifies notifications whose pid is its own; foreign pids are just
 *     CONTINUEd. If the parent process dies while a forked child is alive, the child's
 *     fsync would fail with ENOSYS — hence the module is armed ONLY for packages whose
 *     whole lifetime was verified not to fork+exec (design §9 point 1). After exec the
 *     listener (CLOEXEC) is closed in the child; the parent still holds the filter's
 *     notifier open, so that case keeps working as long as the parent lives.
 *
 * PROPERTIES (re-read when a fresh process arms; not persistent)
 *   modavd.fsync.defer       comma list of packages; compiled default below;
 *                            "none" disables the module entirely (kill switch).
 *   modavd.fsync.paths       comma list of glob patterns for the temp files;
 *                            default "<any>/HGGameUpdate/HGDownload/<any>.downloading".
 *   modavd.fsync.defer.mode  "observe" (default) or "active" (defer + pay back, see MODES).
 *   modavd.fsync.log         "1" logs every notification; default is a 60 s summary.
 */

#include <android/fdsan.h>
#include <android/log.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fnmatch.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <new>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#define FD_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ModAvdFsync", __VA_ARGS__)
#define FD_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ModAvdFsync", __VA_ARGS__)

// ---- kernel ABI pieces, with fallbacks in case a header is older -----------------
// (NDK r27c has all of these; the fallbacks follow include/uapi/linux/seccomp.h.)
#ifndef SECCOMP_SET_MODE_FILTER
#define SECCOMP_SET_MODE_FILTER 1
#endif
#ifndef SECCOMP_GET_NOTIF_SIZES
#define SECCOMP_GET_NOTIF_SIZES 3
#endif
#ifndef SECCOMP_FILTER_FLAG_TSYNC
#define SECCOMP_FILTER_FLAG_TSYNC (1UL << 0)
#endif
#ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
#endif
#ifndef SECCOMP_FILTER_FLAG_TSYNC_ESRCH
#define SECCOMP_FILTER_FLAG_TSYNC_ESRCH (1UL << 4)
#endif
#ifndef SECCOMP_RET_USER_NOTIF
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#endif
#ifndef SECCOMP_USER_NOTIF_FLAG_CONTINUE
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE (1UL << 0)
#endif
// ioctl numbers: SECCOMP_IOC_MAGIC is '!'; RECV=IOWR(0, seccomp_notif),
// SEND=IOWR(1, seccomp_notif_resp), ID_VALID=IOW(2, __u64).
#ifndef SECCOMP_IOCTL_NOTIF_RECV
#define SECCOMP_IOCTL_NOTIF_RECV _IOWR('!', 0, struct seccomp_notif)
#endif
#ifndef SECCOMP_IOCTL_NOTIF_SEND
#define SECCOMP_IOCTL_NOTIF_SEND _IOWR('!', 1, struct seccomp_notif_resp)
#endif

namespace {

// x86_64 syscall numbers the filter keys on (the numbers the host kernel sees AFTER
// the translator has mapped the guest's arm64 fsync).
constexpr uint32_t kNrFsync = 74;
constexpr uint32_t kNrFdatasync = 75;
static_assert(__NR_fsync == 74 && __NR_fdatasync == 75, "x86_64 only");

// Magic second argument that marks an fsync issued by our own (future) payback code:
// the filter lets such a call through without a notification. fsync(2) only uses its
// first argument, so the kernel ignores args[1]; a value both non-zero and in the low
// 32 bits (high 32 bits must be 0) is checked. 0x4D4F4446 = "MODF" (ModAVD fsync).
// An accidental collision just means that one guest fsync runs for real, which is the
// safe direction. The scanner's payback fsync (active mode) passes it; observe never does.
constexpr uint32_t kMagic = 0x4D4F4446u;
static_assert(kMagic != 0, "magic must be non-zero");

// The supervisor's listener fd is moved to at least this number (above where a game
// normally allocates fds) and tagged with this fdsan owner tag value.
constexpr int kListenerMinFd = 900;
constexpr uint64_t kFdsanTagValue = 0x4D4F44465359ull;  // "MODFSY"

constexpr long kSummaryEveryNs = 60L * 1000 * 1000 * 1000;
constexpr long kStuckAfterNs = 2L * 1000 * 1000 * 1000;
// (active) a dirty matched file is paid back (one open+fsync+close) at most this often;
// bounds the data a hard-kill can lose for a file still being written to this window.
constexpr long kPaybackEveryNs = 5L * 1000 * 1000 * 1000;

enum Mode { kObserve = 0, kActive = 1 };

constexpr char kDefaultPackages[] = "com.hypergryph.arknights";
// Plain fnmatch (NOT FNM_PATHNAME): a single '*' spans the many path segments between
// .../files and HGGameUpdate and between HGDownload and the uuid. Real path:
//   /storage/emulated/0/Android/data/com.hypergryph.arknights/files/
//       HGGameUpdate/HGDownload/file_<ts>/download/<uuid>.downloading
constexpr char kDefaultPaths[] = "*/HGGameUpdate/HGDownload/*.downloading";

constexpr int kMaxPatterns = 8;
char g_patterns[kMaxPatterns][160];
int g_pattern_count = 0;
int g_mode = kObserve;
std::atomic<bool> g_log_each{false};
std::atomic<bool> g_installed{false};
char g_pkg[128] = {};
pid_t g_pid = 0;

// ---- per-fd tracking (supervisor-thread only in this build) ---------------------
enum FdState : uint8_t { kUnknown = 0, kUnmatched = 1, kMatched = 2 };

// All fields are atomic so the supervisor's pre-SEND defer decision (active mode) can
// read them without taking g_lock; the classifier (post-SEND, under g_lock) is the only
// writer. dev/ino detect fd-number reuse; matched_idx points at the g_matched slot.
struct FdEntry {
  std::atomic<uint8_t> state{kUnknown};
  std::atomic<uint64_t> dev{0};
  std::atomic<uint64_t> ino{0};
  std::atomic<int32_t> matched_idx{-1};  // slot in g_matched, or -1
};

FdEntry* g_fds = nullptr;
int g_fd_cap = 0;

// Currently-tracked matched files with their paths, and (active mode) the payback
// bookkeeping. A deferred fsync bumps deferred_ns (atomic, from the notify path, no
// lock); the scanner thread pays back files whose deferred_ns is newer than paid_ns and
// pays at most once per kPaybackEveryNs, by path, with a magic fsync.
constexpr int kMaxMatched = 32;
struct MatchedFile {
  int fd = -1;
  char path[512] = {};
  std::atomic<long> deferred_ns{0};  // time of most recent deferred fsync (0 = none)
  std::atomic<long> paid_ns{0};      // time of most recent payback (0 = none)
};
MatchedFile g_matched[kMaxMatched];

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;  // guards fd table + matched list

// Counters.
std::atomic<uint64_t> g_c_recv{0};      // notifications received
std::atomic<uint64_t> g_c_real{0};      // matched (.downloading) fsync/fdatasync seen
std::atomic<uint64_t> g_c_other{0};     // unmatched fsync/fdatasync seen
std::atomic<uint64_t> g_c_foreign{0};   // notification from another pid (forked child)
std::atomic<uint64_t> g_c_recv_err{0};  // RECV failures other than EINTR/ENOENT
std::atomic<uint64_t> g_c_send_err{0};  // SEND failures other than ENOENT
std::atomic<uint64_t> g_c_send_enoent{0};  // SEND found the notification already gone
std::atomic<uint64_t> g_c_stuck{0};     // watchdog stuck reports
std::atomic<uint64_t> g_c_deferred{0};     // (active) fsync answered val=0, real call skipped
std::atomic<uint64_t> g_c_payback{0};      // (active) payback fsyncs issued by the scanner
std::atomic<uint64_t> g_c_payback_err{0};  // (active) payback open/fsync failures (file gone)

// Supervisor progress, for the watchdog.
std::atomic<bool> g_sup_alive{false};
std::atomic<long> g_last_recv_ns{0};       // when the last notification was received
std::atomic<long> g_handling_since_ns{0};  // 0 = idle (blocked in RECV); else start time
std::atomic<int> g_handling_phase{0};      // 0 = before SEND, 1 = after SEND

// Start-up handshake between install() and the threads.
enum StartState { kWait = 0, kGo = 1, kAbort = 2 };
pthread_mutex_t g_start_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_start_cond = PTHREAD_COND_INITIALIZER;
int g_start_state = kWait;
int g_listener = -1;
size_t g_notif_size = 0;
size_t g_resp_size = 0;

long NowNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000L + ts.tv_nsec;
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

// Lock held. Add/refresh a matched fd in the side list; returns its slot index, or -1 if
// the list is full (then the fd simply goes untracked and is never deferred).
int MatchedAdd(int fd, const char* path) {
  for (int i = 0; i < kMaxMatched; ++i) {
    if (g_matched[i].fd == fd) {
      strncpy(g_matched[i].path, path, sizeof(g_matched[i].path) - 1);
      g_matched[i].path[sizeof(g_matched[i].path) - 1] = '\0';
      return i;
    }
  }
  for (int i = 0; i < kMaxMatched; ++i) {
    if (g_matched[i].fd < 0) {
      g_matched[i].fd = fd;
      strncpy(g_matched[i].path, path, sizeof(g_matched[i].path) - 1);
      g_matched[i].path[sizeof(g_matched[i].path) - 1] = '\0';
      g_matched[i].deferred_ns.store(0, std::memory_order_relaxed);
      g_matched[i].paid_ns.store(0, std::memory_order_relaxed);
      return i;
    }
  }
  // Full (should not happen for a download): leave untracked.
  return -1;
}

// Lock held.
void MatchedRemove(int fd) {
  for (int i = 0; i < kMaxMatched; ++i) {
    if (g_matched[i].fd == fd) {
      g_matched[i].fd = -1;
      g_matched[i].path[0] = '\0';
      return;
    }
  }
}

// Lock held. Ensure g_fds[fd] is classified; (re)classify on fd reuse (the same fd
// number now naming a different dev/ino). Returns the state. Stores the path into
// out_path (may be nullptr) when it classifies afresh as matched. A read failure
// (the fd was already closed — we classify after the kernel has run the call) is
// treated as unmatched, never as matched.
uint8_t Classify(int fd, char* out_path, size_t path_cap) {
  if (fd < 0 || fd >= g_fd_cap) return kUnmatched;  // untracked -> behave as today
  FdEntry& e = g_fds[fd];
  struct stat st;
  const bool have_stat = (fstat(fd, &st) == 0);
  const uint8_t cached = e.state.load(std::memory_order_relaxed);
  if (cached != kUnknown && have_stat &&
      e.dev.load(std::memory_order_relaxed) == st.st_dev &&
      e.ino.load(std::memory_order_relaxed) == st.st_ino) {
    return cached;  // same file still on this fd
  }
  char path[512];
  const bool have_path = FdPath(fd, path, sizeof(path)) >= 0;
  const bool matched = have_stat && have_path && PathMatches(path);
  // Reclassifying (fresh fd or fd-number reuse): drop any stale matched slot first.
  MatchedRemove(fd);
  e.matched_idx.store(-1, std::memory_order_relaxed);
  e.dev.store(have_stat ? st.st_dev : 0, std::memory_order_relaxed);
  e.ino.store(have_stat ? st.st_ino : 0, std::memory_order_relaxed);
  if (matched) {
    const int idx = MatchedAdd(fd, path);
    e.matched_idx.store(idx, std::memory_order_relaxed);
    if (out_path != nullptr) {
      strncpy(out_path, path, path_cap - 1);
      out_path[path_cap - 1] = '\0';
    }
  }
  // Publish state last (release): a reader that sees kMatched then sees the dev/ino and
  // matched_idx written above.
  e.state.store(matched ? kMatched : kUnmatched, std::memory_order_release);
  return matched ? kMatched : kUnmatched;
}

// Post-reply bookkeeping for one notification: classify the fd, count, optionally log.
// Runs AFTER SECCOMP_IOCTL_NOTIF_SEND, so it is off the guest's critical path.
void AccountFsync(const struct seccomp_notif* req) {
  const bool datasync = (req->data.nr == static_cast<int>(kNrFdatasync));
  const int fd = static_cast<int>(static_cast<uint32_t>(req->data.args[0]));
  // req->pid is the TID of the triggering thread (kernel task_pid_vnr), NOT the TGID, so
  // it differs from getpid() for every worker thread. Our own threads share our fd table;
  // a forked child's do not. A TID of one of our threads has /proc/self/task/<tid>; a
  // forked child's does not -- only then would /proc/self/fd/<fd> name the wrong file.
  char taskp[64];
  snprintf(taskp, sizeof(taskp), "/proc/self/task/%d", static_cast<int>(req->pid));
  if (access(taskp, F_OK) != 0) {  // forked child (or already gone): don't trust /proc/self/fd
    g_c_foreign.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // Fast path: an fd already known as unmatched with the same file needs no readlink,
  // but Classify() validates identity with one fstat; that is cheap.
  char path[512] = {};
  pthread_mutex_lock(&g_lock);
  const uint8_t state = Classify(fd, path, sizeof(path));
  pthread_mutex_unlock(&g_lock);
  if (state == kMatched) {
    g_c_real.fetch_add(1, std::memory_order_relaxed);
    if (g_log_each.load(std::memory_order_relaxed)) {
      // path is only filled on the first sighting of the fd; fetch again if empty.
      if (path[0] == '\0') FdPath(fd, path, sizeof(path));
      FD_LOGI("notif %s fd=%d %s", datasync ? "fdatasync" : "fsync", fd, path);
    }
  } else {
    g_c_other.fetch_add(1, std::memory_order_relaxed);
  }
}

// ---- active mode: the pre-SEND defer decision ---------------------------------------
// Runs on the notify path BEFORE SECCOMP_IOCTL_NOTIF_SEND, so it must not block: it takes
// no lock, does no readlink and no open -- only atomic reads plus one fstat/access. It
// returns true to DEFER (answer the guest's fsync with val=0 and skip the real call), and
// false to let the kernel run the call for real (CONTINUE). Only a confirmed-matched,
// same-file, non-foreign fd is deferred; everything uncertain runs for real (safe side).
// On a defer it stamps the matched slot's deferred_ns so the scanner pays it back by path.
bool DecideDefer(const struct seccomp_notif* req) {
  const int fd = static_cast<int>(static_cast<uint32_t>(req->data.args[0]));
  if (fd < 0 || fd >= g_fd_cap) return false;
  FdEntry& e = g_fds[fd];
  if (e.state.load(std::memory_order_acquire) != kMatched) return false;  // unknown/unmatched
  // A forked child's fd table is not ours; its TID is not under /proc/self/task. Never
  // defer such a call -- let the kernel run it (ForgetAfterFork also wipes a child's cache).
  char taskp[64];
  snprintf(taskp, sizeof(taskp), "/proc/self/task/%d", static_cast<int>(req->pid));
  if (access(taskp, F_OK) != 0) return false;
  // fd-number reuse: confirm this fd still names the file we classified as matched.
  struct stat st;
  if (fstat(fd, &st) != 0) return false;
  if (e.dev.load(std::memory_order_relaxed) != st.st_dev ||
      e.ino.load(std::memory_order_relaxed) != st.st_ino)
    return false;
  const int idx = e.matched_idx.load(std::memory_order_relaxed);
  if (idx < 0 || idx >= kMaxMatched) return false;  // no payback slot -> run it for real
  g_matched[idx].deferred_ns.store(NowNs(), std::memory_order_release);
  return true;
}

// The notify path's full decision: observe never defers; active defers only what
// DecideDefer approves. One function so the call site and the unit test share it.
bool ShouldDefer(const struct seccomp_notif* req) {
  return (g_mode == kActive) && DecideDefer(req);
}

// ---- supervisor thread --------------------------------------------------------

void* SupervisorMain(void*) {
  // Park until install() has either handed us the listener or given up.
  pthread_mutex_lock(&g_start_lock);
  while (g_start_state == kWait) pthread_cond_wait(&g_start_cond, &g_start_lock);
  const int state = g_start_state;
  const int listener = g_listener;
  const size_t notif_size = g_notif_size;
  const size_t resp_size = g_resp_size;
  pthread_mutex_unlock(&g_start_lock);
  if (state != kGo) return nullptr;  // inert: install failed, no filter exists

  // Buffers sized from the kernel's own sizes (it requires exactly-zeroed request
  // space of that size); allocated once, before the loop, so the loop never allocates.
  auto* req = static_cast<struct seccomp_notif*>(calloc(1, notif_size));
  auto* resp = static_cast<struct seccomp_notif_resp*>(calloc(1, resp_size));
  if (req == nullptr || resp == nullptr) {
    // The filter is already installed; with no buffers we cannot answer. Fall back to
    // stack buffers of the compiled-in size (kernel 6.1 sizes == sizeof).
    FD_LOGE("supervisor buffer alloc failed; using stack buffers");
  }
  struct seccomp_notif sreq;
  struct seccomp_notif_resp sresp;
  const bool use_stack = (req == nullptr || resp == nullptr);
  if (use_stack) {
    req = &sreq;
    resp = &sresp;
  }

  g_sup_alive.store(true, std::memory_order_release);
  g_last_recv_ns.store(NowNs(), std::memory_order_relaxed);

  for (;;) {  // never exits
    // ---- notification path: RECV -> (cheap defer decision) -> SEND ------------
    if (!use_stack) {
      memset(req, 0, notif_size);
    } else {
      memset(&sreq, 0, sizeof(sreq));
    }
    if (ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, req) != 0) {
      const int e = errno;
      if (e == EINTR || e == ENOENT) continue;  // transient: retry at once
      // Anything else (listener closed/invalid, ...): count, log sparsely, back off a
      // little so a persistent error cannot spin, and keep looping.
      const uint64_t n = g_c_recv_err.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 3 || n % 1000 == 0) FD_LOGE("NOTIF_RECV failed errno=%d (count=%llu)", e, (unsigned long long)n);
      struct timespec ts = {0, 10 * 1000 * 1000};
      nanosleep(&ts, nullptr);
      continue;
    }
    const long t0 = NowNs();
    g_last_recv_ns.store(t0, std::memory_order_relaxed);
    g_handling_phase.store(0, std::memory_order_relaxed);
    g_handling_since_ns.store(t0, std::memory_order_release);
    g_c_recv.fetch_add(1, std::memory_order_relaxed);

    // Decide (active) whether to defer; observe always continues. This is the ONLY work
    // before SEND and is deliberately cheap so the guest thread is never held up.
    const bool defer = ShouldDefer(req);
    // defer: answer the guest with val=0/error=0 and no CONTINUE -> the kernel returns 0
    // to the guest WITHOUT running fsync (deferred). Otherwise CONTINUE -> run it for real.
    memset(resp, 0, resp_size);
    resp->id = req->id;
    resp->val = 0;
    resp->error = 0;
    resp->flags = defer ? 0 : SECCOMP_USER_NOTIF_FLAG_CONTINUE;
    if (defer) g_c_deferred.fetch_add(1, std::memory_order_relaxed);
    if (ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, resp) != 0) {
      // ENOENT: the target died / was interrupted by a signal and the id is gone — the
      // kernel completes that call by itself; nothing to do. Anything else is a bug:
      // count it. There is nothing better to do than carry on.
      if (errno == ENOENT) {
        g_c_send_enoent.fetch_add(1, std::memory_order_relaxed);
      } else {
        const int e = errno;
        const uint64_t n = g_c_send_err.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3 || n % 1000 == 0) FD_LOGE("NOTIF_SEND failed errno=%d (count=%llu)", e, (unsigned long long)n);
      }
    }
    g_handling_phase.store(1, std::memory_order_relaxed);

    // ---- off the critical path: the guest has already been released ------------
    AccountFsync(req);
    g_handling_since_ns.store(0, std::memory_order_release);
  }
  return nullptr;
}

// ---- watchdog thread --------------------------------------------------------
// Prints the periodic summary (so the supervisor never logs on a timer) and reports a
// supervisor that is not making progress. It cannot remove the filter (impossible), it
// only makes the cause of a frozen game visible in logcat.
void* WatchdogMain(void*) {
  pthread_mutex_lock(&g_start_lock);
  while (g_start_state == kWait) pthread_cond_wait(&g_start_cond, &g_start_lock);
  const int state = g_start_state;
  pthread_mutex_unlock(&g_start_lock);
  if (state != kGo) return nullptr;

  long last_summary = NowNs();
  long reported_since = 0;
  bool reported_dead = false;
  for (;;) {
    struct timespec ts = {1, 0};
    nanosleep(&ts, nullptr);
    const long now = NowNs();
    const long since = g_handling_since_ns.load(std::memory_order_acquire);
    if (since != 0 && now - since > kStuckAfterNs && since != reported_since) {
      reported_since = since;
      g_c_stuck.fetch_add(1, std::memory_order_relaxed);
      FD_LOGE("supervisor stuck: handling one notification for %ld ms (phase=%s, recv=%llu)",
              (now - since) / 1000000L, g_handling_phase.load() == 0 ? "before-send" : "after-send",
              (unsigned long long)g_c_recv.load());
    }
    if (!g_sup_alive.load(std::memory_order_acquire) && !reported_dead && now - last_summary > 5L * 1000 * 1000 * 1000) {
      reported_dead = true;
      FD_LOGE("supervisor thread is not running; filtered fsync will hang/ENOSYS");
    }
    if (now - last_summary >= kSummaryEveryNs) {
      last_summary = now;
      FD_LOGI("summary pkg=%s mode=%s recv=%llu real=%llu other=%llu foreign=%llu deferred=%llu "
              "payback=%llu payback_err=%llu recv_err=%llu send_err=%llu send_enoent=%llu stuck=%llu idle_ms=%ld",
              g_pkg, g_mode == kActive ? "active" : "observe",
              (unsigned long long)g_c_recv.load(), (unsigned long long)g_c_real.load(),
              (unsigned long long)g_c_other.load(), (unsigned long long)g_c_foreign.load(),
              (unsigned long long)g_c_deferred.load(), (unsigned long long)g_c_payback.load(),
              (unsigned long long)g_c_payback_err.load(), (unsigned long long)g_c_recv_err.load(),
              (unsigned long long)g_c_send_err.load(), (unsigned long long)g_c_send_enoent.load(),
              (unsigned long long)g_c_stuck.load(),
              (now - g_last_recv_ns.load(std::memory_order_relaxed)) / 1000000L);
    }
  }
  return nullptr;
}

// ---- scanner / payback thread (active mode only) -----------------------------------
// Off the notify path. Once a second it pays back files that have unpaid deferred fsyncs:
// one open(path, O_RDONLY) + magic fsync + close per file, at most once per
// kPaybackEveryNs. That bounds a hard-kill's loss for each in-flight .downloading file to
// about that window (its data is then on disk). If the file was renamed/deleted (open
// fails) there is nothing left to flush at that path, so the slot is retired; the
// downloader re-fetches any tail it cannot verify, which is its normal resume path.
// Pay back one matched slot if it has unpaid deferred fsyncs and was last paid longer ago
// than kPaybackEveryNs. Returns true if a payback was due (and attempted). Off the notify
// path: the lock is held only to copy the path, never across the open/fsync/close I/O.
bool PaybackSlotIfDue(int i, long now) {
  const long deferred = g_matched[i].deferred_ns.load(std::memory_order_acquire);
  const long paid = g_matched[i].paid_ns.load(std::memory_order_relaxed);
  if (deferred == 0 || deferred <= paid) return false;  // nothing unpaid
  if (now - paid < kPaybackEveryNs) return false;        // paid recently enough
  char path[512];
  pthread_mutex_lock(&g_lock);  // only to copy the path; no I/O under the lock
  strncpy(path, g_matched[i].path, sizeof(path) - 1);
  path[sizeof(path) - 1] = '\0';
  pthread_mutex_unlock(&g_lock);
  if (path[0] == '\0') {
    g_matched[i].paid_ns.store(now, std::memory_order_relaxed);
    return true;
  }
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {  // renamed/deleted: nothing to flush here
    g_matched[i].paid_ns.store(now, std::memory_order_relaxed);
    g_c_payback_err.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  // Magic fsync: the filter lets args[1]==kMagic through without a notification.
  const long r = syscall(__NR_fsync, fd, static_cast<long>(kMagic));
  close(fd);
  g_matched[i].paid_ns.store(now, std::memory_order_relaxed);
  if (r == 0)
    g_c_payback.fetch_add(1, std::memory_order_relaxed);
  else
    g_c_payback_err.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void* ScannerMain(void*) {
  pthread_mutex_lock(&g_start_lock);
  while (g_start_state == kWait) pthread_cond_wait(&g_start_cond, &g_start_lock);
  const int state = g_start_state;
  pthread_mutex_unlock(&g_start_lock);
  if (state != kGo) return nullptr;

  for (;;) {
    struct timespec ts = {1, 0};
    nanosleep(&ts, nullptr);
    const long now = NowNs();
    for (int i = 0; i < kMaxMatched; ++i) PaybackSlotIfDue(i, now);
  }
  return nullptr;
}

// Tell both threads to go (listener >= 0) or to give up (listener < 0).
void ReleaseThreads(int listener, size_t notif_size, size_t resp_size) {
  pthread_mutex_lock(&g_start_lock);
  g_listener = listener;
  g_notif_size = notif_size;
  g_resp_size = resp_size;
  g_start_state = (listener >= 0) ? kGo : kAbort;
  pthread_cond_broadcast(&g_start_cond);
  pthread_mutex_unlock(&g_start_lock);
}

bool StartThread(void* (*fn)(void*)) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 128 * 1024);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t th;
  const int rc = pthread_create(&th, &attr, fn, nullptr);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    FD_LOGE("pthread_create failed rc=%d", rc);
    return false;
  }
  return true;
}

// ---- the seccomp filter --------------------------------------------------------
// Classic BPF over struct seccomp_data { int nr @0; u32 arch @4; u64 ip @8; u64 args[6]
// @16 }. Little-endian x86_64: args[1] low 32 bits at 24, high 32 bits at 28. Jump
// offsets are relative to the NEXT instruction and forward only, so each verdict is
// repeated where needed.
//
//  0 ld  arch
//  1 jeq AUDIT_ARCH_X86_64 ? next : ALLOW(6)        (32-bit compat / other arch: allow)
//  2 ld  nr
//  3 jeq fsync(74)         ? fsync-check(7) : next
//  4 jeq fdatasync(75)     ? next(5) : ALLOW(6)
//  5 ret USER_NOTIF                                  (fdatasync)
//  6 ret ALLOW                                       (everything else)
//  7 ld  args[1] high
//  8 jeq 0                 ? next(9) : NOTIF(11)     (high != 0 -> not our magic)
//  9 ld  args[1] low
// 10 jeq kMagic            ? ALLOW(12) : next(11)
// 11 ret USER_NOTIF                                  (ordinary guest fsync)
// 12 ret ALLOW                                       (our own payback fsync)
struct sock_filter g_filter[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 4),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 0, 4),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kNrFsync, 3, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kNrFdatasync, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 28),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 2),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 24),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kMagic, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
};
static_assert(sizeof(g_filter) / sizeof(g_filter[0]) == 13, "filter layout comment is out of date");

// Install the filter on every thread and return the listener fd, or -1 (nothing
// installed). On TSYNC_ESRCH failure the kernel returns -1/ESRCH instead of a tid; both
// <0 results mean "nothing was installed".
int InstallFilter() {
  struct sock_fprog prog = {};
  prog.len = static_cast<unsigned short>(sizeof(g_filter) / sizeof(g_filter[0]));
  prog.filter = g_filter;
  const long ret = syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
                           SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_TSYNC_ESRCH |
                               SECCOMP_FILTER_FLAG_NEW_LISTENER,
                           &prog);
  if (ret < 0) {
    FD_LOGE("seccomp(SET_MODE_FILTER) failed errno=%d; module inert", errno);
    return -1;
  }
  return static_cast<int>(ret);
}

// Move the listener to a high fd number and tag it with fdsan. Returns the fd to use
// (the original one if the move fails: the filter is installed already, so we must
// keep a working listener rather than lose it).
int ProtectListener(int listener) {
  int fd = listener;
  const int high = fcntl(listener, F_DUPFD_CLOEXEC, kListenerMinFd);
  if (high >= 0) {
    close(listener);
    fd = high;
  } else {
    FD_LOGE("F_DUPFD_CLOEXEC(>=%d) failed errno=%d; keeping listener fd %d", kListenerMinFd, errno, listener);
  }
  if (android_fdsan_create_owner_tag != nullptr && android_fdsan_exchange_owner_tag != nullptr) {
    const uint64_t tag = android_fdsan_create_owner_tag(ANDROID_FDSAN_OWNER_TYPE_GENERIC_00, kFdsanTagValue);
    android_fdsan_exchange_owner_tag(fd, 0, tag);
  }
  return fd;
}

// ---- fork child: a forked worker inherits the table COW but not its validity ----
void ForgetAfterFork() {
  for (int i = 0; i < g_fd_cap; ++i) {
    g_fds[i].state.store(kUnknown, std::memory_order_relaxed);
    g_fds[i].matched_idx.store(-1, std::memory_order_relaxed);
  }
  for (int i = 0; i < kMaxMatched; ++i) {
    g_matched[i].fd = -1;
    g_matched[i].path[0] = '\0';
    g_matched[i].deferred_ns.store(0, std::memory_order_relaxed);
    g_matched[i].paid_ns.store(0, std::memory_order_relaxed);
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

// Split a comma list into fixed-size slots; returns the number stored.
template <size_t N, size_t M>
int ParseList(const char* list, char (&out)[N][M]) {
  int count = 0;
  for (const char* p = list; *p != '\0' && count < static_cast<int>(N);) {
    const char* comma = strchr(p, ',');
    size_t len = comma != nullptr ? static_cast<size_t>(comma - p) : strlen(p);
    if (len >= M) len = M - 1;
    if (len != 0) {
      memcpy(out[count], p, len);
      out[count][len] = '\0';
      ++count;
    }
    p = comma != nullptr ? comma + 1 : p + len;
  }
  return count;
}

// Whole install sequence. Order matters (a filter cannot be removed):
//   NO_NEW_PRIVS -> kernel sizes -> supervisor thread (parked) -> [watchdog] ->
//   seccomp() -> move/tag the listener -> release the threads.
// Returns the listener fd in use, or -1 with NOTHING installed.
int InstallSeccompObserver() {
  // seccomp() needs NO_NEW_PRIVS (or CAP_SYS_ADMIN). App processes normally have it
  // already; set it if not. Failing to set it means seccomp() would fail anyway: inert.
  if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) {
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
      FD_LOGE("PR_SET_NO_NEW_PRIVS failed errno=%d; module inert", errno);
      return -1;
    }
  }

  struct seccomp_notif_sizes sizes = {};
  size_t notif_size = sizeof(struct seccomp_notif);
  size_t resp_size = sizeof(struct seccomp_notif_resp);
  if (syscall(__NR_seccomp, SECCOMP_GET_NOTIF_SIZES, 0, &sizes) == 0) {
    if (sizes.seccomp_notif > notif_size) notif_size = sizes.seccomp_notif;
    if (sizes.seccomp_notif_resp > resp_size) resp_size = sizes.seccomp_notif_resp;
  } else {
    FD_LOGE("SECCOMP_GET_NOTIF_SIZES failed errno=%d; module inert", errno);
    return -1;
  }

  // 1. supervisor thread FIRST. No thread -> no filter.
  if (!StartThread(&SupervisorMain)) return -1;
  // The watchdog is optional; observe still works without it.
  if (!StartThread(&WatchdogMain)) FD_LOGE("watchdog thread not started; continuing without it");
  // The scanner pays back deferred fsyncs (active only). If it cannot start, fall back to
  // observe so nothing is ever deferred without a payer -> never risk unflushed data.
  if (g_mode == kActive && !StartThread(&ScannerMain)) {
    FD_LOGE("scanner thread not started; falling back to observe (no deferral) for data safety");
    g_mode = kObserve;
  }

  // 2. the filter.
  const int listener = InstallFilter();
  if (listener < 0) {
    ReleaseThreads(-1, 0, 0);  // threads exit; nothing was installed
    return -1;
  }

  // 3. protect the listener fd, then 4. hand it over.
  const int fd = ProtectListener(listener);
  ReleaseThreads(fd, notif_size, resp_size);
  return fd;
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
  g_pattern_count = ParseList(paths, g_patterns);
  if (g_pattern_count == 0) {
    FD_LOGE("no usable path pattern; not arming for %s", pkg);
    return;
  }

  char mode[PROP_VALUE_MAX] = {};
  __system_property_get("modavd.fsync.defer.mode", mode);
  // observe (default): count only, every fsync still runs. active: defer matched fsyncs
  // and pay them back from the scanner thread.
  g_mode = (strcmp(mode, "active") == 0) ? kActive : kObserve;
  char logv[PROP_VALUE_MAX] = {};
  __system_property_get("modavd.fsync.log", logv);
  g_log_each.store(logv[0] == '1', std::memory_order_relaxed);

  // Size the fd table to the process's fd limit, capped so a huge rlimit cannot ask
  // for an unreasonable allocation. Untracked high fds count as unmatched.
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
  g_pid = getpid();
  pthread_atfork(nullptr, nullptr, &ForgetAfterFork);

  // Any failure inside leaves NOTHING installed. g_fds is deliberately not freed on
  // failure: a parked thread may still be waking up to see the abort.
  const int listener = InstallSeccompObserver();
  if (listener < 0) return;
  g_installed.store(true, std::memory_order_release);
  FD_LOGI("armed pkg=%s mode=%s(seccomp-user-notif) listener_fd=%d patterns=%d (first=%s) fd_cap=%d log=%d",
          pkg, g_mode == kActive ? "active" : "observe", listener, g_pattern_count, g_patterns[0],
          g_fd_cap, g_log_each.load() ? 1 : 0);
}
