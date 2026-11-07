/* Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later. */
#include "../../companion/jni/download_listener.h"

#include <atomic>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <initializer_list>
#include <linux/io_uring.h>
#include <new>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {
namespace listener = modavd::download_listener;
enum Operation : int { kNone, kSync, kRename, kUnlink };
struct Probe {
  char directory[512]{};
  char source[544]{};
  char renamed[544]{};
  int file = -1;
  pid_t caller = -1;
  ino_t inode = 0;
  std::atomic<int> operation{kNone};
  std::atomic<int> ticks{0}, tick_tid{0}, callback_tid{0};
  std::atomic<int> syncs{0}, renames{0}, unlinks{0}, inconsistent{0};
};

// The listener cannot be removed. Its callback context must outlive this JNI
// invocation and Instrumentation.finish(), until Android stops the process.
Probe* active_probe;

struct Report {
  char text[7168]{};
  size_t used = 0;
  void Add(const char* format, ...) {
    if (used >= sizeof(text) - 1) return;
    va_list arguments;
    va_start(arguments, format);
    const int count = vsnprintf(text + used, sizeof(text) - used, format, arguments);
    va_end(arguments);
    if (count > 0) used += static_cast<size_t>(count) < sizeof(text) - used
        ? static_cast<size_t>(count) : sizeof(text) - used - 1;
  }
};

bool ReadText(const char* path, char* text, size_t capacity) {
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  ssize_t count;
  do { count = read(fd, text, capacity - 1); } while (count < 0 && errno == EINTR);
  const int error = errno;
  close(fd);
  errno = error;
  if (count < 0) return false;
  text[count] = '\0';
  return true;
}

int Field(const char* status, const char* name) {
  const size_t length = strlen(name);
  for (const char* line = status; line && *line; ) {
    if (strncmp(line, name, length) == 0 && line[length] == ':') return atoi(line + length + 1);
    const char* end = strchr(line, '\n');
    line = end ? end + 1 : nullptr;
  }
  return -1;
}

void Fields(Report& report, const char* prefix, const char* status) {
  report.Add("%s Seccomp=%d Seccomp_filters=%d NoNewPrivs=%d\n", prefix,
             Field(status, "Seccomp"), Field(status, "Seccomp_filters"), Field(status, "NoNewPrivs"));
}

int64_t ClockMs() {
  timespec ts{};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

constexpr long kPidfdOpen = 434;  // x86_64 __NR_pidfd_open
constexpr int kPeerTimeoutMs = 2000;
struct PeerResult {
  int stage, role, pid, peer, uid, dumpable;
  int proc_error, proc_bytes, pidfd_error;
  int live_poll, live_error, live_events;
  int exit_poll, exit_error, exit_events, peer_reaped;
};

long PollFd(int fd, int timeout_ms, int* events) {
  pollfd p{fd, POLLIN, 0};
  timespec timeout{timeout_ms / 1000, static_cast<long>(timeout_ms % 1000) * 1000000};
  const long result = syscall(SYS_ppoll, &p, 1, &timeout, nullptr, sizeof(uint64_t));
  *events = p.revents;
  return result;
}

void PeerStatPath(int pid, char* path) {
  // Raw-fork children must not enter snprintf or other possibly locked code.
  const char prefix[] = "/proc/";
  int length = 0;
  for (char c : prefix) if (c) path[length++] = c;
  char digits[16];
  int count = 0;
  do { digits[count++] = static_cast<char>('0' + pid % 10); pid /= 10; } while (pid);
  while (count) path[length++] = digits[--count];
  const char suffix[] = "/stat";
  for (char c : suffix) path[length++] = c;
}

PeerResult InspectPeer(int role, int peer, int* peer_pidfd) {
  PeerResult result{};
  result.stage = 1;
  result.role = role;
  result.pid = static_cast<int>(syscall(SYS_getpid));
  result.peer = peer;
  result.uid = static_cast<int>(syscall(SYS_getuid));
  const long tightened = syscall(SYS_prctl, PR_SET_DUMPABLE, 0, 0, 0, 0);
  result.dumpable = tightened == 0
      ? static_cast<int>(syscall(SYS_prctl, PR_GET_DUMPABLE, 0, 0, 0, 0)) : -1;
  char path[48], bytes[512];
  PeerStatPath(peer, path);
  const int stat = static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0));
  if (stat < 0) result.proc_error = errno;
  else {
    const long count = syscall(SYS_read, stat, bytes, sizeof(bytes));
    if (count < 0) result.proc_error = errno;
    else result.proc_bytes = static_cast<int>(count);
    syscall(SYS_close, stat);
  }
  *peer_pidfd = static_cast<int>(syscall(kPidfdOpen, peer, 0));
  if (*peer_pidfd < 0) result.pidfd_error = errno;
  else {
    result.live_poll = static_cast<int>(PollFd(*peer_pidfd, 0, &result.live_events));
    if (result.live_poll < 0) result.live_error = errno;
  }
  return result;
}

[[noreturn]] void ExitPeer(int code) {
  syscall(SYS_exit_group, code);
  __builtin_unreachable();
}

[[noreturn]] void Peer(int role, int peer, bool parent_exits, int gate, int output) {
  int peer_pidfd = -1;
  PeerResult result = InspectPeer(role, peer, &peer_pidfd);
  if (syscall(SYS_write, output, &result, sizeof(result)) != sizeof(result)) ExitPeer(110);
  int events = 0;
  char token;
  if (PollFd(gate, kPeerTimeoutMs, &events) != 1 || !(events & POLLIN) ||
      syscall(SYS_read, gate, &token, 1) != 1) ExitPeer(111);
  const bool exiting = parent_exits ? role == 1 : role == 2;
  if (exiting) ExitPeer(0);
  result.stage = 2;
  if (peer_pidfd < 0) result.exit_error = result.pidfd_error;
  else {
    result.exit_poll = static_cast<int>(PollFd(peer_pidfd, kPeerTimeoutMs, &result.exit_events));
    if (result.exit_poll < 0) result.exit_error = errno;
  }
  if (role == 1 && result.exit_poll == 1 && (result.exit_events & POLLIN)) {
    int status = 0;
    result.peer_reaped = syscall(SYS_wait4, peer, &status, 0, nullptr) == peer &&
                         WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }
  if (peer_pidfd >= 0) syscall(SYS_close, peer_pidfd);
  if (syscall(SYS_write, output, &result, sizeof(result)) != sizeof(result)) ExitPeer(112);
  ExitPeer(result.exit_poll == 1 && (result.exit_events & POLLIN) &&
           (role != 1 || result.peer_reaped) ? 0 : 113);
}

bool ReapPeer(pid_t pid) {
  const int64_t deadline = ClockMs() + 500;
  do {
    int status = 0;
    const pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (waited < 0) return false;
    timespec pause{0, 1000000};
    nanosleep(&pause, nullptr);
  } while (ClockMs() < deadline);
  return false;
}

bool PeerRound(Report& report, bool parent_exits) {
  int output[2]{-1, -1}, parent_gate[2]{-1, -1}, child_gate[2]{-1, -1};
  const char* round = parent_exits ? "parent-exits" : "child-exits";
  if (pipe2(output, O_CLOEXEC) != 0 || pipe2(parent_gate, O_CLOEXEC) != 0 ||
      pipe2(child_gate, O_CLOEXEC) != 0) {
    const int error = errno;
    for (int fd : {output[0], output[1], parent_gate[0], parent_gate[1], child_gate[0], child_gate[1]})
      if (fd >= 0) close(fd);
    report.Add("peer round=%s pipe_errno=%d\n", round, error);
    return false;
  }
  const long parent = syscall(SYS_clone, static_cast<unsigned long>(SIGCHLD),
                              nullptr, nullptr, nullptr, 0);
  if (parent == 0) {
    syscall(SYS_close, output[0]);
    syscall(SYS_prctl, PR_SET_DUMPABLE, 0, 0, 0, 0);
    const long child = syscall(SYS_clone, static_cast<unsigned long>(SIGCHLD),
                               nullptr, nullptr, nullptr, 0);
    if (child < 0) ExitPeer(114);
    if (child == 0)
      Peer(2, static_cast<int>(syscall(SYS_getppid)), parent_exits, child_gate[0], output[1]);
    Peer(1, static_cast<int>(child), parent_exits, parent_gate[0], output[1]);
  }
  close(output[1]);
  output[1] = -1;
  bool ok = parent > 0;
  int child_pid = -1, child_pidfd = -1;
  PeerResult ready[2]{};
  bool roles[3]{};
  for (int i = 0; i < 2 && ok; ++i) {
    int events = 0;
    if (PollFd(output[0], kPeerTimeoutMs, &events) != 1 || !(events & POLLIN) ||
        read(output[0], &ready[i], sizeof(ready[i])) != sizeof(ready[i])) { ok = false; break; }
    const auto& r = ready[i];
    report.Add("peer round=%s role=%s pid=%d peer=%d uid=%d dumpable=%d proc_errno=%d proc_bytes=%d "
               "pidfd_errno=%d live_ppoll=%d live_errno=%d live_revents=%d\n", round,
               r.role == 1 ? "parent" : "child", r.pid, r.peer, r.uid, r.dumpable,
               r.proc_error, r.proc_bytes, r.pidfd_error, r.live_poll, r.live_error, r.live_events);
    ok = r.stage == 1 && (r.role == 1 || r.role == 2) && !roles[r.role] &&
         r.uid == static_cast<int>(getuid()) && r.dumpable == 0 && !r.pidfd_error &&
         r.live_poll == 0 && !r.live_error && r.live_events == 0;
    if (r.role == 1 || r.role == 2) roles[r.role] = true;
    if (r.role == 2) child_pid = r.pid;
  }
  if (ok) {
    ok = roles[1] && roles[2] && ready[0].pid == ready[1].peer && ready[1].pid == ready[0].peer;
    child_pidfd = static_cast<int>(syscall(kPidfdOpen, child_pid, 0));
    ok = child_pidfd >= 0 && ok;
  }
  if (ok) {
    // Both peers are blocked at their gates, so neither can exit before both
    // zero-time polls have actually proved peer-alive, including non-dumpable.
    ok = write(parent_gate[1], "G", 1) == 1 && write(child_gate[1], "G", 1) == 1;
  }
  if (ok) {
    PeerResult done{};
    int events = 0;
    ok = PollFd(output[0], kPeerTimeoutMs, &events) == 1 && (events & POLLIN) &&
         read(output[0], &done, sizeof(done)) == sizeof(done);
    report.Add("peer round=%s observer_role=%d exit_ppoll=%d exit_errno=%d exit_revents=%d peer_reaped=%d\n",
               round, done.role, done.exit_poll, done.exit_error, done.exit_events, done.peer_reaped);
    ok = done.stage == 2 && done.role == (parent_exits ? 2 : 1) &&
         done.exit_poll == 1 && !done.exit_error && (done.exit_events & POLLIN) &&
         (parent_exits || done.peer_reaped) && ok;
  }
  if (ok) {
    int events = 0;
    ok = PollFd(child_pidfd, kPeerTimeoutMs, &events) == 1 && (events & POLLIN) &&
         ReapPeer(static_cast<pid_t>(parent));
    // In the parent-exits round the orphan is reaped by Android's reaper;
    // if this process was already a subreaper, collect its adopted child too.
    int status = 0;
    waitpid(child_pid, &status, WNOHANG);
  }
  if (!ok) {
    if (parent > 0) kill(static_cast<pid_t>(parent), SIGKILL);
    if (child_pid > 0) kill(child_pid, SIGKILL);
    if (parent > 0) ReapPeer(static_cast<pid_t>(parent));
  }
  if (child_pidfd >= 0) close(child_pidfd);
  for (int fd : {output[0], parent_gate[0], parent_gate[1], child_gate[0], child_gate[1]}) close(fd);
  report.Add("peer round=%s result=%s\n", round, ok ? "PASS" : "FAIL");
  return ok;
}

bool VisibilityProbe(Report& report) {
  const int initial = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
  const int tightened = initial == 0 ? 0 : prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
  const int current = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
  char own[1024]{};
  const bool own_readable = ReadText("/proc/self/stat", own, sizeof(own));
  unsigned long long start_token = 0;
  const char* token = strrchr(own, ')');
  if (token) {
    ++token;
    for (int field = 3; field <= 22; ++field) {
      while (*token == ' ') ++token;
      if (field == 22) { start_token = strtoull(token, nullptr, 10); break; }
      while (*token && *token != ' ') ++token;
    }
  }
  report.Add("dumpable before=%d tighten_rc=%d after=%d own_stat_readable=%d own_start_token=%llu\n",
             initial, tightened, current, own_readable, start_token);
  if (initial < 0 || tightened != 0 || current != 0 || !own_readable || !start_token) return false;
  // No namespace changes or su proxy: every peer inherits this real app's UID,
  // SELinux domain, mount namespace and original Android app seccomp filter.
  return PeerRound(report, false) && PeerRound(report, true);
}

bool InitialRingInventory(Report& report) {
  DIR* directory = opendir("/proc/self/fd");
  if (!directory) {
    report.Add("initial_fds scan_errno=%d inherited_io_uring=unknown\n", errno);
    return false;
  }
  int scanned = 0, disappeared = 0, errors = 0, rings = 0;
  for (;;) {
    errno = 0;
    dirent* entry = readdir(directory);
    if (!entry) { if (errno) ++errors; break; }
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
    const int fd = atoi(entry->d_name);
    char path[64], target[512];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    const ssize_t length = readlink(path, target, sizeof(target) - 1);
    if (length < 0) {
      if (errno == ENOENT) ++disappeared;
      else { ++errors; report.Add("initial_fd fd=%d readlink_errno=%d\n", fd, errno); }
      continue;
    }
    target[length] = '\0';
    ++scanned;
    if (strncmp(target, "anon_inode:", 11) == 0 && strstr(target, "io_uring")) {
      ++rings;
      report.Add("initial_fd fd=%d target=%s\n", fd, target);
    }
  }
  if (closedir(directory) != 0) ++errors;
  report.Add("initial_fds scanned=%d disappeared=%d errors=%d inherited_io_uring=%d\n",
             scanned, disappeared, errors, rings);
  // An existing ring needs a separate admission decision. Do not hide it behind
  // a successful setup-denial probe or assume that zero means a scan error.
  return errors == 0 && rings == 0;
}

struct UringSetupResult { int returned; int result; int error; int close_error; };

bool IoUringCapability(Report& report) {
  if (!InitialRingInventory(report)) return false;
  int pipe[2];
  if (pipe2(pipe, O_CLOEXEC) != 0) {
    report.Add("io_uring_setup pipe_errno=%d\n", errno);
    return false;
  }
  const long child = syscall(SYS_clone, static_cast<unsigned long>(SIGCHLD),
                             nullptr, nullptr, nullptr, 0);
  if (child == 0) {
    syscall(SYS_close, pipe[0]);
    io_uring_params parameters{};
    errno = 0;
    // The only io_uring syscall in this child. No mmap, enter, register, SQE,
    // IORING_OP_CLOSE, or IO submission occurs even if setup is permitted.
    const long ring = syscall(425, 2U, &parameters);
    UringSetupResult result{1, static_cast<int>(ring), ring < 0 ? errno : 0, 0};
    if (ring >= 0 && syscall(SYS_close, ring) != 0) result.close_error = errno;
    // That is ordinary close of an unused ring fd, not an io_uring close request.
    const long written = syscall(SYS_write, pipe[1], &result, sizeof(result));
    ExitPeer(written == sizeof(result) ? 0 : 115);
  }
  close(pipe[1]);
  if (child < 0) { close(pipe[0]); report.Add("io_uring_setup clone_errno=%d\n", errno); return false; }
  int events = 0;
  UringSetupResult result{};
  const long ready = PollFd(pipe[0], kPeerTimeoutMs, &events);
  const ssize_t count = ready == 1 && (events & (POLLIN | POLLHUP))
      ? read(pipe[0], &result, sizeof(result)) : -1;
  close(pipe[0]);
  int status = 0;
  bool reaped = false;
  const int64_t deadline = ClockMs() + 500;
  do {
    const pid_t waited = waitpid(static_cast<pid_t>(child), &status, WNOHANG);
    if (waited == child) { reaped = true; break; }
    if (waited < 0 && errno != EINTR) break;
    timespec delay{0, 1000000};
    nanosleep(&delay, nullptr);
  } while (ClockMs() < deadline);
  if (!reaped) {
    kill(static_cast<pid_t>(child), SIGKILL);
    ReapPeer(static_cast<pid_t>(child));
  }
  const int signal = reaped && WIFSIGNALED(status) ? WTERMSIG(status) : 0;
  const int exit = reaped && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  report.Add("io_uring_setup entries=2 zero_params=1 returned=%d result=%d errno=%d close_errno=%d "
             "child_signal=%d child_exit=%d reaped=%d no_requests_submitted=1\n",
             count == sizeof(result) ? result.returned : 0, result.result, result.error,
             result.close_error, signal, exit, reaped);
  // Record a genuine app-policy denial or a returned kernel result. Setup
  // availability is evidence, not a reason to relax the system's policy.
  return reaped && ((signal == SIGSYS && count == 0) ||
      (!signal && exit == 0 && count == sizeof(result) && result.returned && !result.close_error));
}

void Tick(void* opaque) {
  auto* probe = static_cast<Probe*>(opaque);
  const int tid = static_cast<int>(syscall(SYS_gettid));
  int empty = 0;
  probe->tick_tid.compare_exchange_strong(empty, tid);
  if (probe->tick_tid.load() != tid) probe->inconsistent.store(1);
  probe->ticks.fetch_add(1);
}

listener::Decision Callback(const listener::Request& request, void* opaque) {
  auto* probe = static_cast<Probe*>(opaque);
  if (request.notification.pid != static_cast<uint32_t>(probe->caller))
    return listener::Decision::Continue();
  const int nr = request.notification.data.nr;
  const auto* args = request.notification.data.args;
  const int operation = probe->operation.load();
  const bool sync = operation == kSync && nr == SYS_fsync &&
                    static_cast<int>(static_cast<uint32_t>(args[0])) == probe->file;
  const bool rename = operation == kRename && nr == SYS_renameat &&
      static_cast<int>(static_cast<uint32_t>(args[0])) == AT_FDCWD &&
      static_cast<int>(static_cast<uint32_t>(args[2])) == AT_FDCWD &&
      args[1] == reinterpret_cast<uintptr_t>(probe->source) &&
      args[3] == reinterpret_cast<uintptr_t>(probe->renamed);
  const bool unlink = operation == kUnlink && nr == SYS_unlinkat &&
      static_cast<int>(static_cast<uint32_t>(args[0])) == AT_FDCWD && args[2] == 0 &&
      args[1] == reinterpret_cast<uintptr_t>(probe->renamed);
  if (!sync && !rename && !unlink) return listener::Decision::Continue();
  probe->callback_tid.store(static_cast<int>(syscall(SYS_gettid)));
  if (!listener::Healthy()) return listener::Decision::Result(0, EIO);
  long result = -1;
  int error = 0;
  if (sync) {
    // A private handle, verified against this fixture's original inode.
    const int owned = fcntl(probe->file, F_DUPFD_CLOEXEC, 0);
    if (owned < 0) return listener::Decision::Result(0, errno);
    struct stat st{};
    if (fstat(owned, &st) == 0 && st.st_ino == probe->inode) {
      result = syscall(SYS_fsync, owned);
      error = result < 0 ? errno : 0;
    } else {
      error = ESTALE;
    }
    close(owned);  // The worker must not trap its own close/fsync.
    if (result == 0) probe->syncs.fetch_add(1);
  } else {
    // Only the fixture's immutable, identity-checked argument pointers enter
    // this branch. The actual syscall uses private copies, not caller buffers.
    char from[sizeof(probe->source)], to[sizeof(probe->renamed)];
    memcpy(from, probe->source, sizeof(from));
    memcpy(to, probe->renamed, sizeof(to));
    result = rename ? syscall(SYS_renameat, AT_FDCWD, from, AT_FDCWD, to)
                    : syscall(SYS_unlinkat, AT_FDCWD, to, 0);
    error = result < 0 ? errno : 0;
    if (result == 0) {
      if (rename) probe->renames.fetch_add(1);
      else probe->unlinks.fetch_add(1);
    }
  }
  return listener::Decision::Result(result, error);
}

jstring Finish(JNIEnv* env, bool passed, const Report& details, const char* stage, int error) {
  Report result;
  result.Add("%s late-jni listener app probe\n", passed ? "PASS" : "FAIL");
  result.Add("%s", details.text);
  result.Add("stage=%s errno=%d\n", stage, error);
  return env->NewStringUTF(result.text);
}
}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_org_modavd_tests_downloadlistener_ProbeInstrumentation_nativeRun(
    JNIEnv* env, jclass, jstring files_directory) {
  Report report;
  const uid_t uid = getuid();
  const pid_t pid = static_cast<pid_t>(syscall(SYS_getpid));
  char domain[256]{}, before[4096]{}, after[4096]{};
  if (!ReadText("/proc/self/attr/current", domain, sizeof(domain)) ||
      !ReadText("/proc/self/status", before, sizeof(before)))
    return Finish(env, false, report, "proc-before", errno);
  domain[strcspn(domain, "\n")] = '\0';
  report.Add("uid=%u pid=%d selinux=%s\n", static_cast<unsigned>(uid), pid, domain);
  Fields(report, "before", before);
  if (uid < 10000 || !strstr(domain, "untrusted_app") || Field(before, "Seccomp") != 2 ||
      Field(before, "Seccomp_filters") < 1)
    return Finish(env, false, report, "original-app-context", EPERM);
  // Observe the initial real-app fd table and original platform policy before
  // installing our own filter. Only an isolated child attempts empty setup.
  if (!IoUringCapability(report)) return Finish(env, false, report, "io-uring-capability", EPROTO);
  // Complete and close every diagnostic pipe/pidfd before installing the
  // production listener, so the probe cannot alter its callback fd-table model.
  if (!VisibilityProbe(report)) return Finish(env, false, report, "pidfd-proc-visibility", EPROTO);
  if (active_probe) return Finish(env, false, report, "repeat-in-same-process", EALREADY);
  auto* probe = new (std::nothrow) Probe;
  if (!probe) return Finish(env, false, report, "allocate", ENOMEM);
  active_probe = probe;
  const char* files = env->GetStringUTFChars(files_directory, nullptr);
  if (!files) return nullptr;
  const int length = snprintf(probe->directory, sizeof(probe->directory), "%s/listener-XXXXXX", files);
  env->ReleaseStringUTFChars(files_directory, files);
  if (length <= 0 || length >= static_cast<int>(sizeof(probe->directory)) || !mkdtemp(probe->directory))
    return Finish(env, false, report, "scratch-directory", errno ? errno : ENAMETOOLONG);
  snprintf(probe->source, sizeof(probe->source), "%s/source", probe->directory);
  snprintf(probe->renamed, sizeof(probe->renamed), "%s/renamed", probe->directory);
  probe->caller = static_cast<pid_t>(syscall(SYS_gettid));
  probe->file = open(probe->source, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
  constexpr char payload[] = "listener app scratch\n";
  struct stat original{};
  if (probe->file < 0 || write(probe->file, payload, sizeof(payload) - 1) != sizeof(payload) - 1 ||
      fstat(probe->file, &original) != 0 || fsync(probe->file) != 0)
    return Finish(env, false, report, "scratch-file", errno);
  probe->inode = original.st_ino;
  const listener::StartResult started = listener::Start(Callback, probe, 2000, Tick);
  report.Add("Start error=%d installed=%d owner=%d caller=%d worker=%d guardian=%d\n",
             started.error, started.filter_installed, pid, probe->caller,
             started.worker_tid, started.guardian_pid);
  if (started.error || !started.filter_installed)
    return Finish(env, false, report, "listener-start", started.error ? started.error : EIO);
  const int first_tick = probe->ticks.load();
  const int64_t deadline = ClockMs() + 1500;
  while (probe->ticks.load() < first_tick + 2 && ClockMs() < deadline) {
    timespec delay{0, 10000000};
    nanosleep(&delay, nullptr);
  }
  if (probe->ticks.load() < first_tick + 2 || !listener::Healthy())
    return Finish(env, false, report, "idle-tick", ETIMEDOUT);
  if (!ReadText("/proc/self/status", after, sizeof(after)))
    return Finish(env, false, report, "proc-after", errno);
  Fields(report, "after", after);
  if (Field(after, "Seccomp") != 2 ||
      Field(after, "Seccomp_filters") != Field(before, "Seccomp_filters") + 1)
    return Finish(env, false, report, "installed-filter-count", EPROTO);
  probe->operation.store(kSync);
  if (syscall(SYS_fsync, probe->file) != 0) return Finish(env, false, report, "emulated-fsync", errno);
  probe->operation.store(kRename);
  if (syscall(SYS_renameat, AT_FDCWD, probe->source, AT_FDCWD, probe->renamed) != 0)
    return Finish(env, false, report, "emulated-renameat", errno);
  struct stat renamed{};
  if (fstatat(AT_FDCWD, probe->renamed, &renamed, 0) != 0 || renamed.st_ino != original.st_ino)
    return Finish(env, false, report, "rename-inode", ESTALE);
  probe->operation.store(kSync);
  if (syscall(SYS_fsync, probe->file) != 0) return Finish(env, false, report, "renamed-fsync", errno);
  probe->operation.store(kUnlink);
  if (syscall(SYS_unlinkat, AT_FDCWD, probe->renamed, 0) != 0)
    return Finish(env, false, report, "emulated-unlinkat", errno);
  probe->operation.store(kNone);
  errno = 0;
  if (fstatat(AT_FDCWD, probe->renamed, &renamed, 0) != -1 || errno != ENOENT)
    return Finish(env, false, report, "unlink-result", EPROTO);
  report.Add("emulated fsync=%d renameat=%d unlinkat=%d ticks=%d tick_tid=%d callback_tid=%d healthy=%d\n",
             probe->syncs.load(), probe->renames.load(), probe->unlinks.load(), probe->ticks.load(),
             probe->tick_tid.load(), probe->callback_tid.load(), listener::Healthy());
  const bool passed = probe->syncs.load() == 2 && probe->renames.load() == 1 && probe->unlinks.load() == 1 &&
      probe->tick_tid.load() == started.worker_tid && probe->callback_tid.load() == started.worker_tid &&
      !probe->inconsistent.load() && listener::Healthy();
  if (close(probe->file) != 0) return Finish(env, false, report, "close-scratch", errno);
  probe->file = -1;
  if (unlinkat(AT_FDCWD, probe->directory, AT_REMOVEDIR) != 0)
    return Finish(env, false, report, "remove-scratch-directory", errno);
  return Finish(env, passed, report, "complete", passed ? 0 : EPROTO);
}
