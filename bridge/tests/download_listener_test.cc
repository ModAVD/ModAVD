/* Copyright 2026 jinghero. SPDX-License-Identifier: AGPL-3.0-or-later. */
// Standalone x86_64 Android/Linux seccomp lifecycle fixture. No game files.
// The guardian is the sole RECV/SEND owner. Its only policy is CONTINUE after
// a fixed deadline. This is NOT a timeout policy for emulated mutations: once
// an operation might have executed elsewhere, CONTINUE could execute it twice.
// Run with --scratch-dir DIR if the platform's temporary directory differs.
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/io_uring.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Production code, not a reimplementation of the callback IPC path.
#define MODAVD_DOWNLOAD_LISTENER_TEST 1
#include "../companion/jni/download_listener.cc"
#undef MODAVD_DOWNLOAD_LISTENER_TEST

#ifndef SECCOMP_FILTER_FLAG_TSYNC_ESRCH
#define SECCOMP_FILTER_FLAG_TSYNC_ESRCH (1UL << 4)
#endif
#ifndef SECCOMP_USER_NOTIF_FLAG_CONTINUE
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE (1UL << 0)
#endif

static_assert(SYS_fsync == 74 && SYS_clone == 56, "x86_64 fixture only");

namespace {
constexpr int kCaseTimeoutMs = 6000;
constexpr int kReplyDelayMs = 250;
constexpr int kPendingCapacity = 8;
constexpr size_t kBufferCapacity = 1024;

enum EventKind : int {
  kGuardianReady = 1, kAppReady, kExecReady, kWorkerReady,
  kReceived, kReplied, kResult, kGuardianDone, kError, kParentDeathObserved,
  kNamespaceStep, kNamespaceMask,
  kSkipped,
};
struct Event {
  int kind;
  int pid;
  int value;
  int extra;
  uint64_t id;
};
enum class Case { kExit, kKill, kExec, kRawClone, kConsumedThenKill, kGuardianKill };

int64_t NowMs() {
  timespec ts{};
  if (syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts) != 0) _exit(90);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool Report(int fd, int kind, int value = 0, int extra = 0, uint64_t id = 0) {
  const Event e{kind, static_cast<int>(syscall(SYS_getpid)), value, extra, id};
  return syscall(SYS_sendto, fd, &e, sizeof(e), MSG_NOSIGNAL, nullptr, 0) == sizeof(e);
}

[[noreturn]] void ChildError(int fd, int stage) {
  const int e = errno;
  Report(fd, kError, stage, e);
  _exit(91);
}

bool WaitReadable(int fd, int ms) {
  pollfd p{fd, POLLIN, 0};
  int rc;
  do { rc = static_cast<int>(syscall(SYS_poll, &p, 1, ms)); } while (rc < 0 && errno == EINTR);
  return rc == 1 && (p.revents & POLLIN);
}

bool SendFd(int socket, int fd) {
  char payload = 'L';
  iovec iov{&payload, 1};
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  cmsghdr* c = CMSG_FIRSTHDR(&msg);
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  c->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(c), &fd, sizeof(fd));
  return syscall(SYS_sendmsg, socket, &msg, MSG_NOSIGNAL) == 1;
}

int ReceiveFd(int socket) {
  char payload = 0;
  iovec iov{&payload, 1};
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  if (syscall(SYS_recvmsg, socket, &msg, MSG_CMSG_CLOEXEC) != 1 || payload != 'L' ||
      (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) return -1;
  cmsghdr* c = CMSG_FIRSTHDR(&msg);
  if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
      c->cmsg_len != CMSG_LEN(sizeof(int))) return -1;
  int fd;
  memcpy(&fd, CMSG_DATA(c), sizeof(fd));
  return fd;
}

void CloseInherited(int keep_a, int keep_b, int limit) {
  // The child has its own fd table. Never use CLONE_FILES here.
  const unsigned low = static_cast<unsigned>(keep_a < keep_b ? keep_a : keep_b);
  const unsigned high = static_cast<unsigned>(keep_a < keep_b ? keep_b : keep_a);
  // close_range arrived in Linux 5.9; old Android kernels use the bounded loop.
  constexpr long kCloseRange = 436;  // x86_64 __NR_close_range
  bool fast = true;
  if (low > 0 && syscall(kCloseRange, 0U, low - 1, 0U) != 0) fast = false;
  if (low + 1 < high && syscall(kCloseRange, low + 1, high - 1, 0U) != 0) fast = false;
  if (syscall(kCloseRange, high + 1, ~0U, 0U) != 0) fast = false;
  if (fast) return;
  for (int fd = 0; fd < limit; ++fd) {
    if (fd != keep_a && fd != keep_b) syscall(SYS_close, fd);
  }
}

struct Pending { uint64_t id; int64_t deadline; bool occupied; };

[[noreturn]] void Guardian(int channel, int events, int fd_limit, bool death_barrier) {
  CloseInherited(channel, events, fd_limit);
  // Test-only barrier: the consumed-notification case must prove the parent is
  // dead before SEND, even if the driver was descheduled for longer than 250ms.
  constexpr long kPidfdOpen = 434;  // x86_64 __NR_pidfd_open
  int parent_pidfd = -1;
  if (death_barrier) {
    parent_pidfd = static_cast<int>(syscall(kPidfdOpen, syscall(SYS_getppid), 0));
    if (parent_pidfd < 0) ChildError(events, 18);
  }
  if (!WaitReadable(channel, 2000)) ChildError(events, 10);
  const int listener = ReceiveFd(channel);
  if (listener < 0) ChildError(events, 11);
  // Publish readiness before waking the app, so the driver's event order is fixed.
  Report(events, kGuardianReady);
  char ack = 'A';
  if (syscall(SYS_sendto, channel, &ack, 1, MSG_NOSIGNAL, nullptr, 0) != 1)
    ChildError(events, 12);
  Pending pending[kPendingCapacity]{};
  alignas(8) unsigned char req_buffer[kBufferCapacity];
  alignas(8) unsigned char resp_buffer[kBufferCapacity];
  int received = 0;
  int replied = 0;
  bool channel_open = true;
  bool parent_death_observed = false;
  int64_t death_deadline = 0;
  for (;;) {
    int wait_ms = 100;
    int64_t now = NowMs();
    for (auto& p : pending) {
      if (!p.occupied) continue;
      if (p.deadline <= now) {
        if (death_barrier && !parent_death_observed) {
          if (!death_deadline) death_deadline = now + 2000;
          pollfd parent{parent_pidfd, POLLIN, 0};
          const long ready = syscall(SYS_poll, &parent, 1, 0);
          if (ready == 1 && (parent.revents & POLLIN)) {
            parent_death_observed = true;
            Report(events, kParentDeathObserved, 0, 0, p.id);
          } else {
            if (now >= death_deadline) { errno = ETIMEDOUT; ChildError(events, 19); }
            p.deadline = now + 10;
            wait_ms = 10;
            continue;
          }
        }
        memset(resp_buffer, 0, sizeof(resp_buffer));
        auto* resp = reinterpret_cast<seccomp_notif_resp*>(resp_buffer);
        resp->id = p.id;
        resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
        // Sample before SEND wakes the worker, which may immediately exit.
        pollfd peer{channel, POLLIN, 0};
        syscall(SYS_poll, &peer, 1, 0);
        const bool inherited_channel = channel_open && !(peer.revents & POLLHUP);
        int rc;
        do { rc = static_cast<int>(syscall(SYS_ioctl, listener, SECCOMP_IOCTL_NOTIF_SEND, resp)); }
        while (rc < 0 && errno == EINTR);
        if (rc < 0 && errno != ENOENT) ChildError(events, 13);
        ++replied;
        // extra==1 means a forked worker still holds the original app IPC end.
        Report(events, kReplied, rc == 0 ? 0 : ENOENT,
               inherited_channel, p.id);
        p.occupied = false;
      } else {
        const int remaining = static_cast<int>(p.deadline - now);
        if (remaining < wait_ms) wait_ms = remaining;
      }
    }
    // POLLOUT is deliberately absent: SENT notifications make it continuously ready.
    pollfd fds[2]{{listener, POLLIN, 0}, {channel_open ? channel : -1, POLLIN, 0}};
    int rc = static_cast<int>(syscall(SYS_poll, fds, 2, wait_ms));
    if (rc < 0) {
      if (errno == EINTR) continue;
      ChildError(events, 14);
    }
    if (fds[1].revents & (POLLHUP | POLLERR)) {
      // Parent exec/exit is not a reason to stop serving inherited filters.
      channel_open = false;
    }
    if (fds[0].revents & POLLIN) {
      memset(req_buffer, 0, sizeof(req_buffer));
      auto* req = reinterpret_cast<seccomp_notif*>(req_buffer);
      if (syscall(SYS_ioctl, listener, SECCOMP_IOCTL_NOTIF_RECV, req) < 0) {
        if (errno == EINTR || errno == ENOENT) continue;
        ChildError(events, 15);
      }
      ++received;
      Report(events, kReceived, static_cast<int>(req->pid), 0, req->id);
      Pending* slot = nullptr;
      for (auto& p : pending) if (!p.occupied) { slot = &p; break; }
      if (!slot) {
        // This fixture never emulates any operation. Queue-full CONTINUE is safe
        // only under that narrow policy, not after mutation admission.
        memset(resp_buffer, 0, sizeof(resp_buffer));
        auto* resp = reinterpret_cast<seccomp_notif_resp*>(resp_buffer);
        resp->id = req->id;
        resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
        if (syscall(SYS_ioctl, listener, SECCOMP_IOCTL_NOTIF_SEND, resp) < 0 && errno != ENOENT)
          ChildError(events, 16);
        ++replied;
        Report(events, kReplied, 0, 0, req->id);
      } else {
        *slot = Pending{req->id, NowMs() + kReplyDelayMs, true};
      }
    }
    if (fds[0].revents & POLLHUP) {
      Report(events, kGuardianDone, received, replied);
      syscall(SYS_close, listener);
      _exit(received == replied ? 0 : 92);
    }
    if (fds[0].revents & (POLLERR | POLLNVAL)) ChildError(events, 17);
  }
}

int Install() {
  seccomp_notif_sizes sizes{};
  if (syscall(SYS_seccomp, SECCOMP_GET_NOTIF_SIZES, 0, &sizes) != 0) return -1;
  if (sizes.seccomp_notif > kBufferCapacity || sizes.seccomp_notif_resp > kBufferCapacity) {
    errno = EOVERFLOW;
    return -1;
  }
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
  sock_filter code[]{
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 0, 3),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_fsync, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  sock_fprog prog{static_cast<unsigned short>(sizeof(code) / sizeof(code[0])), code};
  return static_cast<int>(syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
      SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_TSYNC_ESRCH |
          SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog));
}

[[noreturn]] void ScratchSync(int file, int events, int gate) {
  char token;
  if (syscall(SYS_read, gate, &token, 1) != 1) ChildError(events, 20);
  const int64_t start = NowMs();
  errno = 0;
  const long rc = syscall(SYS_fsync, file);
  const int e = rc == 0 ? 0 : errno;
  Report(events, kResult, e, static_cast<int>(NowMs() - start));
  _exit(rc == 0 || e == ENOSYS ? 0 : 93);
}

[[noreturn]] void App(Case test, int control, int gate, int file, int fd_limit) {
  if (setpgid(0, 0) != 0) ChildError(control, 30);
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0)
    ChildError(control, 31);
  const pid_t guardian = fork();
  if (guardian < 0) ChildError(control, 32);
  if (guardian == 0)
    Guardian(pair[1], control, fd_limit, test == Case::kConsumedThenKill);
  close(pair[1]);
  const int listener = Install();
  if (listener < 0) ChildError(control, 33);
  if (!SendFd(pair[0], listener)) ChildError(control, 34);
  // No post-install game child may inherit a listener. The guardian is its owner.
  close(listener);
  if (!WaitReadable(pair[0], 2000)) ChildError(control, 35);
  char ack;
  if (read(pair[0], &ack, 1) != 1 || ack != 'A') ChildError(control, 36);
  Report(control, kAppReady, guardian);
  if (test == Case::kExec || test == Case::kGuardianKill) {
    char command;
    if (read(control, &command, 1) != 1) ChildError(control, 37);
    if (test == Case::kGuardianKill) {
      int status = 0;
      if (waitpid(guardian, &status, 0) != guardian || !WIFSIGNALED(status) ||
          WTERMSIG(status) != SIGKILL) ChildError(control, 38);
      ScratchSync(file, control, gate);
    }
    char file_arg[24], control_arg[24], gate_arg[24];
    snprintf(file_arg, sizeof(file_arg), "%d", file);
    snprintf(control_arg, sizeof(control_arg), "%d", control);
    snprintf(gate_arg, sizeof(gate_arg), "%d", gate);
    if (fcntl(control, F_SETFD, 0) != 0 || fcntl(gate, F_SETFD, 0) != 0 ||
        fcntl(file, F_SETFD, 0) != 0) ChildError(control, 39);
    execl("/proc/self/exe", "download-listener-test", "--exec-sync", file_arg,
          control_arg, gate_arg, static_cast<char*>(nullptr));
    ChildError(control, 40);
  }
  // A true raw clone bypasses every pthread_atfork callback. No CLONE_FILES or VM.
  const long worker = syscall(SYS_clone, static_cast<unsigned long>(SIGCHLD),
                              nullptr, nullptr, nullptr, 0);
  if (worker < 0) ChildError(control, 41);
  if (worker == 0) ScratchSync(file, control, gate);
  Report(control, kWorkerReady, static_cast<int>(worker));
  char command;
  if (read(control, &command, 1) != 1) ChildError(control, 42);
  _exit(command == 'E' ? 0 : 94);
}

bool SendByte(int fd, char value) {
  return send(fd, &value, 1, MSG_NOSIGNAL) == 1;
}

bool ReapLeader(pid_t app, bool expect_kill) {
  const int64_t deadline = NowMs() + 1000;
  do {
    int status = 0;
    const pid_t r = waitpid(app, &status, WNOHANG);
    if (r == app) return expect_kill ? WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL
                                   : WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (r < 0) return false;
    poll(nullptr, 0, 5);
  } while (NowMs() < deadline);
  return false;
}

bool Cleanup(pid_t group) {
  // Each case creates a private process group; the driver is a subreaper. All
  // case descendants are killed/reaped on failure as well as after success.
  kill(-group, SIGKILL);
  const int64_t deadline = NowMs() + 1500;
  do {
    int status;
    const pid_t r = waitpid(-group, &status, WNOHANG);
    if (r < 0 && errno == ECHILD) return true;
    if (r < 0 && errno != EINTR) return false;
    if (r == 0) poll(nullptr, 0, 5);
  } while (NowMs() < deadline);
  return false;
}

bool Run(Case test, const char* name, const char* scratch, int fd_limit) {
  int control[2], gate[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, control) != 0) return false;
  if (pipe2(gate, O_CLOEXEC) != 0) { close(control[0]); close(control[1]); return false; }
  char path[512];
  const int n = snprintf(path, sizeof(path), "%s/modavd-listener-XXXXXX", scratch);
  int file = n > 0 && n < static_cast<int>(sizeof(path)) ? mkstemp(path) : -1;
  if (file < 0) {
    close(control[0]); close(control[1]); close(gate[0]); close(gate[1]);
    return false;
  }
  // An unlinked, still-open scratch inode is the only file any case syncs.
  const bool prepared = unlink(path) == 0 && write(file, "scratch\n", 8) == 8;
  if (!prepared) {
    close(file); close(control[0]); close(control[1]); close(gate[0]); close(gate[1]);
    return false;
  }
  const pid_t app = fork();
  if (app == 0) {
    close(control[0]); close(gate[1]);
    App(test, control[1], gate[0], file, fd_limit);
  }
  close(control[1]); close(gate[0]); close(file);
  if (app < 0) { close(control[0]); close(gate[1]); return false; }
  // Both sides set the group to avoid a race with timeout cleanup.
  if (setpgid(app, app) < 0 && errno != EACCES && errno != ESRCH) {
    kill(app, SIGKILL); Cleanup(app); close(control[0]); close(gate[1]); return false;
  }
  bool ok = true, finished = false, app_reaped = false, worker_reaped = false;
  int guardian = -1, worker = -1, received = 0, replied = 0, results = 0;
  bool guardian_done = false, ready = false, exec_ready = false, released = false;
  bool inherited_channel = false;
  bool death_observed = false;
  uint64_t received_id = 0, replied_id = 0;
  Event trace[32]{};
  int trace_count = 0;
  const int64_t deadline = NowMs() + kCaseTimeoutMs;
  while (ok && !finished && NowMs() < deadline) {
    if (!WaitReadable(control[0], 100)) continue;
    Event e{};
    const ssize_t count = recv(control[0], &e, sizeof(e), 0);
    if (count != sizeof(e)) { ok = false; break; }
    if (trace_count < 32) trace[trace_count++] = e;
    switch (e.kind) {
      case kGuardianReady:
        guardian = e.pid;
        break;
      case kAppReady:
        ready = e.pid == app && e.value == guardian;
        ok = ready;
        if (test == Case::kExec) ok = ok && SendByte(control[0], 'X');
        if (test == Case::kGuardianKill) {
          ok = ok && kill(guardian, SIGKILL) == 0 && SendByte(control[0], 'G');
          if (ok) { ok = write(gate[1], "G", 1) == 1; released = true; }
        }
        break;
      case kExecReady:
        exec_ready = e.pid == app;
        ok = exec_ready && write(gate[1], "G", 1) == 1;
        released = true;
        break;
      case kWorkerReady:
        worker = e.value;
        ok = ready && worker > 0;
        if (test == Case::kExit) {
          ok = ok && SendByte(control[0], 'E') && ReapLeader(app, false);
          app_reaped = ok;
        } else if (test == Case::kKill) {
          ok = ok && kill(app, SIGKILL) == 0 && ReapLeader(app, true);
          app_reaped = ok;
        }
        if (ok) { ok = write(gate[1], "G", 1) == 1; released = true; }
        break;
      case kReceived:
        ++received;
        received_id = e.id;
        ok = released && e.pid == guardian && e.id != 0 &&
             e.value == (test == Case::kExec ? app : worker);
        if (ok && test == Case::kConsumedThenKill) {
          ok = kill(app, SIGKILL) == 0 && ReapLeader(app, true);
          app_reaped = ok;
        }
        break;
      case kReplied:
        ++replied;
        replied_id = e.id;
        inherited_channel = e.extra == 1;
        ok = e.pid == guardian && e.value == 0 && e.id == received_id;
        if (test == Case::kConsumedThenKill) ok = ok && death_observed && app_reaped;
        break;
      case kParentDeathObserved:
        death_observed = test == Case::kConsumedThenKill && app_reaped &&
                         e.pid == guardian && e.id == received_id;
        ok = death_observed;
        break;
      case kResult:
        ++results;
        ok = e.value == (test == Case::kGuardianKill ? ENOSYS : 0);
        if (test != Case::kGuardianKill)
          ok = ok && e.pid == (test == Case::kExec ? app : worker) && e.extra >= kReplyDelayMs - 10;
        if (ok && test == Case::kRawClone) ok = SendByte(control[0], 'E');
        // A zombie can retain its seccomp filter until release_task at reap.
        // Reap filtered tasks before waiting for listener POLLHUP; reversing
        // this order makes the test driver itself keep the guardian alive.
        if (ok && !app_reaped) {
          app_reaped = ReapLeader(app, false);
          ok = app_reaped;
        }
        if (ok && worker > 0) {
          worker_reaped = ReapLeader(worker, false);
          ok = worker_reaped;
        }
        if (test == Case::kGuardianKill) finished = true;
        break;
      case kGuardianDone:
        guardian_done = e.pid == guardian && e.value == 1 && e.extra == 1;
        ok = guardian_done;
        break;
      default:
        ok = false;
        break;
    }
    if (test != Case::kGuardianKill && guardian_done && results == 1 && replied == 1)
      finished = true;
  }
  ok = ok && finished && ready && results == 1;
  if (test != Case::kGuardianKill)
    ok = ok && received == 1 && replied == 1 && received_id == replied_id && guardian_done;
  else
    ok = ok && received == 0 && replied == 0;
  if (test == Case::kExec) ok = ok && exec_ready;
  if (test == Case::kConsumedThenKill) ok = ok && death_observed;
  if (test == Case::kExit || test == Case::kKill || test == Case::kConsumedThenKill)
    ok = ok && app_reaped && inherited_channel;
  // Verify natural leader exit before cleanup can conceal it.
  if (!app_reaped && finished) ok = ReapLeader(app, false) && ok;
  if (finished && test != Case::kGuardianKill) {
    ok = ReapLeader(guardian, false) && ok;
    if (worker > 0) ok = worker_reaped && ok;
  }
  const bool cleaned = Cleanup(app);
  close(control[0]); close(gate[1]);
  ok = ok && cleaned;
  printf("%s %s recv=%d reply=%d result=%d cleanup=%s\n", ok ? "PASS" : "FAIL",
         name, received, replied, results, cleaned ? "ok" : "failed");
  if (!ok) {
    for (int i = 0; i < trace_count; ++i) {
      const auto& e = trace[i];
      fprintf(stderr, "  event=%d pid=%d value=%d extra=%d id=%llu\n", e.kind,
              e.pid, e.value, e.extra, static_cast<unsigned long long>(e.id));
    }
  }
  return ok;
}

enum class ProductionCase {
  kContinue, kError, kBypass, kTimeout, kNamespace, kForeign, kParentKill,
  kSignalQueue, kFaultBeforeTransfer, kFaultTransfer, kFaultAfterReady,
  kAppLikePollDenied,
  kIdleTick,
  kIoUringRoute,
  kInitialRingRefusal,
};
int production_skips = 0;
struct ProductionContext {
  ProductionCase test;
  int file;
  int events;
  std::atomic<int> sync_calls{0};
  std::atomic<unsigned> namespace_mask{0};
  std::atomic<int> tick_calls{0}, tick_tid{0}, handler_active{0}, overlap{0}, tick_baseline{0};
  std::atomic<unsigned> uring_mask{0};
};

void ProductionTick(void* opaque) {
  auto* context = static_cast<ProductionContext*>(opaque);
  if (context->handler_active.exchange(1)) context->overlap.store(1);
  const int tid = static_cast<int>(syscall(SYS_gettid));
  int unset = 0;
  context->tick_tid.compare_exchange_strong(unset, tid);
  if (context->tick_tid.load() != tid) context->overlap.store(1);
  context->tick_calls.fetch_add(1);
  context->handler_active.store(0);
}

modavd::download_listener::Decision ProductionCallback(
    const modavd::download_listener::Request& request, void* opaque) {
  using modavd::download_listener::Decision;
  auto* context = static_cast<ProductionContext*>(opaque);
  const int nr = request.notification.data.nr;
  if (context->test == ProductionCase::kIoUringRoute && nr >= 425 && nr <= 427) {
    context->uring_mask.fetch_or(1U << (nr - 425));
    Report(context->events, kReceived, static_cast<int>(request.notification.pid), nr,
           request.notification.id);
    // No kernel ring creation or IO submission: prove the callback owns all
    // three decisions by returning a synthetic ENOSYS before kernel execution.
    return Decision::Result(0, ENOSYS);
  }
  unsigned bit = 0;
  if (nr == SYS_rename) bit = 1;
  if (nr == SYS_renameat) bit = 2;
  if (nr == SYS_renameat2) bit = 4;
  if (nr == SYS_link) bit = 8;
  if (nr == SYS_linkat) bit = 16;
  if (nr == SYS_unlink) bit = 32;
  if (nr == SYS_unlinkat) bit = 64;
  if (bit) context->namespace_mask.fetch_or(bit);
  if (nr != SYS_fsync) return Decision::Continue();
  const int count = context->sync_calls.fetch_add(1) + 1;
  Report(context->events, kReceived, static_cast<int>(request.notification.pid), nr,
         request.notification.id);
  if (context->test == ProductionCase::kIdleTick) {
    if (context->handler_active.exchange(1)) context->overlap.store(1);
    const int ticks = context->tick_calls.load();
    const bool same_worker = context->tick_tid.load() == static_cast<int>(syscall(SYS_gettid));
    const bool before_request = ticks > context->tick_baseline.load();
    // Longer than one idle interval: another tick thread would overlap here.
    timespec pause{0, 150000000};
    nanosleep(&pause, nullptr);
    const bool serialized = context->tick_calls.load() == ticks && !context->overlap.load();
    context->handler_active.store(0);
    return same_worker && before_request && serialized ? Decision::Continue() : Decision::Result(0, EPROTO);
  }
  if (context->test == ProductionCase::kError) return Decision::Result(0, ENOSPC);
  if (context->test == ProductionCase::kForeign) {
    return Decision::Result(0, request.notification.pid != static_cast<uint32_t>(request.owner_pid)
        ? EACCES : EPROTO);
  }
  if (context->test == ProductionCase::kTimeout) {
    if (count == 1) {
      timespec pause{0, 400000000};
      nanosleep(&pause, nullptr);
      // This success is intentionally late. The caller must retain ETIMEDOUT.
      return Decision::Result(0);
    }
    return Decision::Result(0, modavd::download_listener::Healthy() ? EPROTO : EIO);
  }
  if (context->test == ProductionCase::kBypass) {
    if (syscall(SYS_fsync, context->file) != 0) return Decision::Result(0, errno);
    errno = 0;
    if (syscall(SYS_unlinkat, -1, "unreachable-scratch-name", 0) != -1 || errno != EBADF)
      return Decision::Result(0, EPROTO);
    return Decision::Result(0);
  }
  return Decision::Continue();
}

volatile sig_atomic_t received_signals = 0;
void NonRestartingSignal(int) { ++received_signals; }

struct SignalContext {
  int file, replacement;
  const char* original;
  const char* replacement_path;
  const char* destination;
  char source[544];
  ino_t original_ino;
  std::atomic<int> actor_tid[2]{};
  std::atomic<int> actor_gate[2]{};
  std::atomic<int> actor_result[2]{};
  std::atomic<int> first_entered{0}, release{0}, second_returned{0}, identity_ok{0}, reused{0};
};
struct SignalActor { SignalContext* context; int index; };

void* QueuedSignalActor(void* opaque) {
  auto* actor = static_cast<SignalActor*>(opaque);
  SignalContext* s = actor->context;
  const int index = actor->index;
  s->actor_tid[index].store(static_cast<int>(syscall(SYS_gettid)));
  while (!s->actor_gate[index].load()) {
    syscall(SYS_futex, reinterpret_cast<int*>(&s->actor_gate[index]), FUTEX_WAIT_PRIVATE,
            0, nullptr, nullptr, 0);
  }
  errno = 0;
  const long rc = index == 0 ? syscall(SYS_fsync, s->file)
      : syscall(SYS_rename, s->source, s->destination);
  const int error = rc == 0 ? 0 : errno;
  s->actor_result[index].store(error);
  if (index == 1) {
    s->second_returned.store(1);
    // If an EINTR incorrectly escapes while queued, the old callback can now
    // see a reused pathname/fd. With WAIT_KILLABLE this happens only after reply.
    strcpy(s->source, s->replacement_path);
    s->reused.store(syscall(SYS_dup2, s->replacement, s->file) == s->file);
  }
  return nullptr;
}

modavd::download_listener::Decision QueuedSignalCallback(
    const modavd::download_listener::Request& request, void* opaque) {
  using modavd::download_listener::Decision;
  auto* s = static_cast<SignalContext*>(opaque);
  const int nr = request.notification.data.nr;
  if (nr == SYS_fsync && request.notification.pid == static_cast<uint32_t>(s->actor_tid[0].load())) {
    s->first_entered.store(1);
    while (!s->release.load()) {
      syscall(SYS_futex, reinterpret_cast<int*>(&s->release), FUTEX_WAIT_PRIVATE,
              0, nullptr, nullptr, 0);
    }
    const long rc = syscall(SYS_fsync, s->file);
    return Decision::Result(rc, rc < 0 ? errno : 0);
  }
  if (nr == SYS_rename && request.notification.pid == static_cast<uint32_t>(s->actor_tid[1].load())) {
    struct stat st{};
    const bool same = fstat(s->file, &st) == 0 && st.st_ino == s->original_ino &&
                      strcmp(s->source, s->original) == 0 && !s->second_returned.load();
    s->identity_ok.store(same);
    if (!same) return Decision::Result(0, ESTALE);
    const long rc = syscall(SYS_rename, s->source, s->destination);
    return Decision::Result(rc, rc < 0 ? errno : 0);
  }
  return Decision::Continue();
}

template <typename Predicate> bool WaitCondition(Predicate predicate, int ms = 1500) {
  const int64_t deadline = NowMs() + ms;
  while (!predicate() && NowMs() < deadline) poll(nullptr, 0, 1);
  return predicate();
}

[[noreturn]] void SignalApp(int events, int file, const char* a, const char* b, const char* c) {
  using namespace modavd::download_listener;
  SignalContext context{};
  context.file = file;
  context.original = a;
  context.replacement_path = c;
  context.destination = b;
  strcpy(context.source, a);
  context.replacement = open(c, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  struct stat original{};
  if (context.replacement < 0 || fstat(file, &original) != 0) ChildError(events, 70);
  context.original_ino = original.st_ino;
  struct sigaction signal{};
  signal.sa_handler = NonRestartingSignal;
  sigemptyset(&signal.sa_mask);
  signal.sa_flags = 0;  // Deliberately no SA_RESTART.
  if (sigaction(SIGUSR1, &signal, nullptr) != 0) ChildError(events, 71);
  received_signals = 0;
  pthread_t threads[2];
  SignalActor actors[2]{{&context, 0}, {&context, 1}};
  for (int i = 0; i < 2; ++i)
    if (pthread_create(&threads[i], nullptr, QueuedSignalActor, &actors[i]) != 0)
      ChildError(events, 72);
  if (!WaitCondition([&] { return context.actor_tid[0].load() && context.actor_tid[1].load(); }))
    ChildError(events, 73);
  const StartResult started = Start(QueuedSignalCallback, &context, 3000);
  if (started.error || !started.filter_installed) { errno = started.error; ChildError(events, 74); }
  Report(events, kAppReady, started.guardian_pid, started.worker_tid);
  context.actor_gate[0].store(1);
  syscall(SYS_futex, reinterpret_cast<int*>(&context.actor_gate[0]), FUTEX_WAKE_PRIVATE, 1);
  bool ok = WaitCondition([&] { return context.first_entered.load() == 1; });
  context.actor_gate[1].store(1);
  syscall(SYS_futex, reinterpret_cast<int*>(&context.actor_gate[1]), FUTEX_WAKE_PRIVATE, 1);
  // This test-only shared atomic is written immediately AFTER the actual kernel
  // RECV. It avoids guessing when the queued notification becomes SENT.
  ok = WaitCondition([&] {
    return modavd::download_listener::g_health->last_received_tid.load() == context.actor_tid[1].load();
  }) && ok;
  for (int i = 0; i < 20; ++i) {
    ok = syscall(SYS_tgkill, syscall(SYS_getpid), context.actor_tid[1].load(), SIGUSR1) == 0 && ok;
    poll(nullptr, 0, 2);
  }
  ok = ok && !context.second_returned.load() && !context.reused.load() && received_signals == 0;
  context.release.store(1);
  syscall(SYS_futex, reinterpret_cast<int*>(&context.release), FUTEX_WAKE_PRIVATE, 1);
  for (auto thread : threads) ok = pthread_join(thread, nullptr) == 0 && ok;
  struct stat moved{}, replacement{}, reused{};
  ok = context.actor_result[0].load() == 0 && context.actor_result[1].load() == 0 &&
       context.identity_ok.load() && context.reused.load() && received_signals > 0 &&
       stat(b, &moved) == 0 && moved.st_ino == original.st_ino && stat(c, &replacement) == 0 &&
       replacement.st_ino != original.st_ino && fstat(file, &reused) == 0 &&
       reused.st_ino == replacement.st_ino && Healthy() && ok;
  Report(events, kResult, ok ? 0 : EPROTO, static_cast<int>(received_signals));
  _exit(ok ? 0 : 97);
}

struct FaultChildContext { int file; int ready[2]; int gate[2]; pid_t child; };
FaultChildContext* fault_child_context;
void CreateFaultChild(int listener) {
  auto* context = fault_child_context;
  const long child = syscall(SYS_clone, static_cast<unsigned long>(SIGCHLD),
                             nullptr, nullptr, nullptr, 0);
  if (child < 0) _exit(98);
  if (child == 0) {
    modavd::download_listener::InternalClose(listener);
    char byte = 'A';
    if (syscall(SYS_write, context->ready[1], &byte, 1) != 1 ||
        syscall(SYS_read, context->gate[0], &byte, 1) != 1) _exit(98);
    errno = 0;
    const long rc = syscall(SYS_fsync, context->file);
    _exit(rc == -1 && errno == ENOSYS ? 0 : 98);
  }
  context->child = static_cast<pid_t>(child);
  char byte;
  if (syscall(SYS_read, context->ready[0], &byte, 1) != 1) _exit(98);
}

[[noreturn]] void FaultApp(ProductionCase test, int events, int file) {
  using namespace modavd::download_listener;
  ProductionContext callback{test, file, events};
  FaultChildContext child{};
  child.file = file;
  if (test == ProductionCase::kFaultAfterReady) {
    if (pipe2(child.ready, O_CLOEXEC) != 0 || pipe2(child.gate, O_CLOEXEC) != 0)
      ChildError(events, 80);
    fault_child_context = &child;
    g_before_guardian_kill = CreateFaultChild;
  }
  g_startup_fault = test == ProductionCase::kFaultBeforeTransfer ? StartupFault::kBeforeTransfer
      : test == ProductionCase::kFaultTransfer ? StartupFault::kBadTransferFd : StartupFault::kAfterReady;
  const int64_t before = NowMs();
  const StartResult started = Start(ProductionCallback, &callback, 1000);
  bool ok = started.error != 0 && started.filter_installed && !Healthy() && NowMs() - before < 3000;
  Report(events, kAppReady, started.guardian_pid, started.worker_tid);
  errno = 0;
  ok = syscall(SYS_fsync, file) == -1 && errno == ENOSYS && ok;
  if (test == ProductionCase::kFaultAfterReady) {
    ok = child.child > 0 && syscall(SYS_write, child.gate[1], "G", 1) == 1 && ok;
    int status = 0;
    ok = waitpid(child.child, &status, 0) == child.child && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0 && ok;
  }
  Report(events, kResult, ok ? 0 : EPROTO, started.error);
  _exit(ok ? 0 : 99);
}

[[noreturn]] void InitialRingApp(int events) {
  // A separate setup child makes original-policy SIGSYS an explicit skip, not
  // a false PASS or a fatal signal in the driver. No IO requests are submitted.
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0) ChildError(events, 100);
  struct Failure { char marker; int error; };
  const pid_t child = fork();
  if (child < 0) ChildError(events, 101);
  if (child == 0) {
    close(pair[0]);
    io_uring_params parameters{};
    const long ring = syscall(425, 2U, &parameters);
    if (ring >= 0) {
      const bool sent = SendFd(pair[1], static_cast<int>(ring));
      close(static_cast<int>(ring));
      _exit(sent ? 0 : 101);
    }
    const Failure failure{'E', errno};
    _exit(send(pair[1], &failure, sizeof(failure), MSG_NOSIGNAL) == sizeof(failure) ? 0 : 102);
  }
  close(pair[1]);
  pollfd channel{pair[0], POLLIN, 0};
  const int polled = poll(&channel, 1, 2000);
  int ring = -1, error = 0;
  if (polled == 1 && (channel.revents & POLLIN)) {
    char marker = 0;
    if (recv(pair[0], &marker, 1, MSG_PEEK | MSG_DONTWAIT) == 1 && marker == 'L') {
      ring = ReceiveFd(pair[0]);
    } else if (marker == 'E') {
      Failure failure{};
      if (recv(pair[0], &failure, sizeof(failure), MSG_DONTWAIT) == sizeof(failure)) error = failure.error;
    }
  }
  close(pair[0]);
  if (polled <= 0) kill(child, SIGKILL);
  int status = 0;
  if (waitpid(child, &status, 0) != child) ChildError(events, 102);
  const int signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
  if (ring < 0 && (error || signal == SIGSYS)) {
    Report(events, kSkipped, error, signal);
    _exit(0);
  }
  if (ring < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) ChildError(events, 103);
  const int before = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
  auto callback = [](const modavd::download_listener::Request&, void*) {
    return modavd::download_listener::Decision::Continue();
  };
  const auto started = modavd::download_listener::Start(callback, nullptr, 1000);
  const bool ok = started.error == ENOTSUP && !started.filter_installed &&
      started.guardian_pid == -1 && started.worker_tid == -1 &&
      prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == before;
  close(ring);
  Report(events, kAppReady, started.guardian_pid, started.worker_tid);
  Report(events, kResult, ok ? 0 : EPROTO, started.error);
  _exit(ok ? 0 : 103);
}

[[noreturn]] void ProductionApp(ProductionCase test, int events, int gate, int file,
                              const char* a, const char* b, const char* c) {
  using namespace modavd::download_listener;
  if (setpgid(0, 0) != 0) ChildError(events, 60);
  if (test == ProductionCase::kInitialRingRefusal) InitialRingApp(events);
  if (test == ProductionCase::kAppLikePollDenied || test == ProductionCase::kIdleTick) {
    // Reproduce the app-zygote allowlist difference missed by root-only tests:
    // raw poll kills x86_64 tasks, while ppoll remains available. This is a
    // deliberately narrow regression filter, not a full Android app sandbox.
    sock_filter code[]{
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_poll, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog filter{static_cast<unsigned short>(sizeof(code) / sizeof(code[0])), code};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &filter) != 0)
      ChildError(events, 90);
    const pid_t probe = fork();
    if (probe < 0) ChildError(events, 91);
    if (probe == 0) {
      syscall(SYS_poll, nullptr, 0, 0);
      _exit(100);  // The inherited filter must kill this probe first.
    }
    int status = 0;
    if (waitpid(probe, &status, 0) != probe || !WIFSIGNALED(status) ||
        WTERMSIG(status) != SIGSYS) ChildError(events, 92);
  }
  if (test == ProductionCase::kSignalQueue) SignalApp(events, file, a, b, c);
  if (test == ProductionCase::kFaultBeforeTransfer || test == ProductionCase::kFaultTransfer ||
      test == ProductionCase::kFaultAfterReady) FaultApp(test, events, file);
  ProductionContext context{test, file, events};
  const StartResult started = Start(ProductionCallback, &context,
                                    test == ProductionCase::kTimeout ? 100 : 1000,
                                    test == ProductionCase::kIdleTick ? ProductionTick : nullptr);
  if (started.error || !started.filter_installed || !Healthy()) {
    errno = started.error ? started.error : EPROTO;
    ChildError(events, started.filter_installed ? 61 : 62);
  }
  Report(events, kAppReady, started.guardian_pid, started.worker_tid);
  bool ok = true;
  if (test == ProductionCase::kIdleTick) {
    ok = WaitCondition([&] { return context.tick_calls.load() >= 2; });
    ok = context.sync_calls.load() == 0 && context.tick_tid.load() == started.worker_tid && ok;
    context.tick_baseline.store(context.tick_calls.load());
  }
  if (test == ProductionCase::kParentKill || test == ProductionCase::kForeign) {
    const long child = syscall(SYS_clone, static_cast<unsigned long>(SIGCHLD),
                               nullptr, nullptr, nullptr, 0);
    if (child < 0) ChildError(events, 63);
    if (child == 0) {
      if (test == ProductionCase::kParentKill) {
        char command;
        if (read(gate, &command, 1) != 1) ChildError(events, 64);
      }
      errno = 0;
      const long rc = syscall(SYS_fsync, file);
      const int expected = test == ProductionCase::kParentKill ? EIO : EACCES;
      const bool passed = rc == -1 && errno == expected;
      if (test == ProductionCase::kParentKill) Report(events, kResult, passed ? 0 : errno, expected);
      _exit(passed ? 0 : 95);
    }
    if (test == ProductionCase::kParentKill) {
      Report(events, kWorkerReady, static_cast<int>(child));
      for (;;) pause();  // The driver's bounded SIGKILL case owns this process.
    }
    int status = 0;
    ok = waitpid(static_cast<pid_t>(child), &status, 0) == child && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0 && context.sync_calls.load() == 1;
  } else if (test == ProductionCase::kIoUringRoute) {
    errno = 0;
    ok = syscall(425, 2U, nullptr) == -1 && errno == ENOSYS;
    errno = 0;
    ok = syscall(426, -1, 0U, 0U, 0U, nullptr, 0U) == -1 && errno == ENOSYS && ok;
    errno = 0;
    ok = syscall(427, -1, 0U, nullptr, 0U) == -1 && errno == ENOSYS && ok;
    ok = context.uring_mask.load() == 7 && Healthy() && ok;
  } else if (test == ProductionCase::kNamespace) {
    for (int step = 1; step <= 9 && ok; ++step) {
      errno = 0;
      long rc = -1;
      switch (step) {
        case 1: case 7: rc = syscall(SYS_link, a, b); break;
        case 2: rc = syscall(SYS_rename, b, c); break;
        case 3: case 9: rc = syscall(SYS_unlink, c); break;
        case 4: rc = syscall(SYS_linkat, AT_FDCWD, a, AT_FDCWD, b, 0); break;
        case 5: rc = syscall(SYS_renameat, AT_FDCWD, b, AT_FDCWD, c); break;
        case 6: rc = syscall(SYS_unlinkat, AT_FDCWD, c, 0); break;
        case 8: rc = syscall(SYS_renameat2, AT_FDCWD, b, AT_FDCWD, c, 0); break;
      }
      const int error = rc == 0 ? 0 : errno;
      Report(events, kNamespaceStep, step, error);
      ok = rc == 0;
    }
    Report(events, kNamespaceMask, static_cast<int>(context.namespace_mask.load()), 127);
    ok = ok && context.namespace_mask.load() == 127;
  } else {
    const int before = context.sync_calls.load();
    ok = syscall(SYS_fdatasync, file) == 0 && context.sync_calls.load() == before;
    errno = 0;
    const long rc = syscall(SYS_fsync, file);
    const int error = errno;
    if (test == ProductionCase::kTimeout) {
      ok = ok && rc == -1 && error == ETIMEDOUT && !Healthy();
      timespec settle{0, 500000000};
      nanosleep(&settle, nullptr);
      errno = 0;
      ok = syscall(SYS_fsync, file) == -1 && errno == EIO && !Healthy() && ok;
      ok = ok && context.sync_calls.load() == 2;
    } else if (test == ProductionCase::kError) {
      ok = ok && rc == -1 && error == ENOSPC && Healthy();
    } else {
      ok = ok && rc == 0 && context.sync_calls.load() == 1 && Healthy();
      if (test == ProductionCase::kIdleTick)
        ok = context.tick_calls.load() >= 3 && context.tick_tid.load() == started.worker_tid &&
             !context.overlap.load() && ok;
    }
  }
  Report(events, kResult, ok ? 0 : EPROTO, context.sync_calls.load());
  _exit(ok ? 0 : 96);
}

bool RunProduction(ProductionCase test, const char* name, const char* scratch) {
  int events[2], gate[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, events) != 0) return false;
  if (pipe2(gate, O_CLOEXEC) != 0) { close(events[0]); close(events[1]); return false; }
  char directory[512], a[544], b[544], c[544];
  const int n = snprintf(directory, sizeof(directory), "%s/modavd-listener-production-XXXXXX", scratch);
  if (n <= 0 || n >= static_cast<int>(sizeof(directory)) || !mkdtemp(directory)) {
    close(events[0]); close(events[1]); close(gate[0]); close(gate[1]); return false;
  }
  snprintf(a, sizeof(a), "%s/a", directory);
  snprintf(b, sizeof(b), "%s/b", directory);
  snprintf(c, sizeof(c), "%s/c", directory);
  int file = open(a, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (file < 0 || write(file, "scratch\n", 8) != 8) {
    if (file >= 0) close(file);
    unlink(a); rmdir(directory);
    close(events[0]); close(events[1]); close(gate[0]); close(gate[1]); return false;
  }
  const pid_t app = fork();
  if (app == 0) {
    close(events[0]); close(gate[1]);
    ProductionApp(test, events[1], gate[0], file, a, b, c);
  }
  close(events[1]); close(gate[0]); close(file);
  if (app < 0) {
    close(events[0]); close(gate[1]); unlink(a); rmdir(directory); return false;
  }
  setpgid(app, app);
  bool ok = true, ready = false, complete = false, app_reaped = false, skipped = false;
  int skip_errno = 0, skip_signal = 0;
  int guardian = -1, worker = -1, callbacks = 0;
  Event trace[32]{};
  int traces = 0;
  const int64_t deadline = NowMs() + kCaseTimeoutMs;
  while (ok && !complete && NowMs() < deadline) {
    if (!WaitReadable(events[0], 100)) continue;
    Event event{};
    if (recv(events[0], &event, sizeof(event), 0) != sizeof(event)) { ok = false; break; }
    if (traces < 32) trace[traces++] = event;
    if (event.kind == kAppReady) {
      guardian = event.value;
      ready = event.pid == app && (test == ProductionCase::kInitialRingRefusal
          ? guardian == -1 && event.extra == -1 : guardian > 0 && event.extra > 0);
      ok = ready;
    } else if (event.kind == kSkipped && test == ProductionCase::kInitialRingRefusal) {
      skipped = true;
      complete = true;
      skip_errno = event.value;
      skip_signal = event.extra;
    } else if (event.kind == kWorkerReady && test == ProductionCase::kParentKill) {
      worker = event.value;
      ok = ready && kill(app, SIGKILL) == 0 && ReapLeader(app, true);
      app_reaped = ok;
      if (ok) ok = write(gate[1], "G", 1) == 1;
    } else if (event.kind == kReceived) {
      ++callbacks;
    } else if ((event.kind == kNamespaceStep || event.kind == kNamespaceMask) &&
               test == ProductionCase::kNamespace) {
      // Keep per-syscall errno and the actually intercepted mask in diagnostics.
    } else if (event.kind == kResult) {
      ok = ready && event.value == 0;
      complete = true;
    } else {
      ok = false;
    }
  }
  ok = ok && (ready || skipped) && complete;
  if (complete && !app_reaped) ok = ReapLeader(app, false) && ok;
  if (complete && worker > 0) ok = ReapLeader(worker, false) && ok;
  if (complete && guardian > 0 && test != ProductionCase::kFaultBeforeTransfer &&
      test != ProductionCase::kFaultTransfer && test != ProductionCase::kFaultAfterReady)
    ok = ReapLeader(guardian, false) && ok;
  const bool cleaned = Cleanup(app);
  ok = cleaned && ok;
  close(events[0]); close(gate[1]);
  unlink(a); unlink(b); unlink(c); rmdir(directory);
  if (ok && skipped) {
    ++production_skips;
    printf("SKIP production-%s setup_errno=%d setup_signal=%d cleanup=%s\n", name,
           skip_errno, skip_signal, cleaned ? "ok" : "failed");
  } else {
    printf("%s production-%s callbacks=%d cleanup=%s\n", ok ? "PASS" : "FAIL", name,
           callbacks, cleaned ? "ok" : "failed");
  }
  if (!ok) for (int i = 0; i < traces; ++i)
    fprintf(stderr, "  event=%d pid=%d value=%d extra=%d\n", trace[i].kind, trace[i].pid,
            trace[i].value, trace[i].extra);
  return ok;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 5 && strcmp(argv[1], "--exec-sync") == 0) {
    const int file = atoi(argv[2]), events = atoi(argv[3]), gate = atoi(argv[4]);
    Report(events, kExecReady);
    ScratchSync(file, events, gate);
  }
#ifdef __ANDROID__
  const char* scratch = "/data/local/tmp";
#else
  const char* scratch = "/tmp";
#endif
  if (argc == 3 && strcmp(argv[1], "--scratch-dir") == 0) scratch = argv[2];
  else if (argc != 1) {
    fprintf(stderr, "usage: %s [--scratch-dir DIR]\n", argv[0]);
    return 2;
  }
  if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0) {
    perror("PR_SET_CHILD_SUBREAPER");
    return 2;
  }
  signal(SIGPIPE, SIG_IGN);
  rlimit limit{};
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur > 1048576) {
    fprintf(stderr, "unusable fd limit\n");
    return 2;
  }
  int failures = 0;
  const struct { Case test; const char* name; } tests[]{
      {Case::kExit, "parent-exit"}, {Case::kKill, "parent-sigkill"},
      {Case::kExec, "parent-exec"}, {Case::kRawClone, "raw-clone-inherits-filter-and-ipc"},
      {Case::kConsumedThenKill, "parent-dies-after-recv"},
      {Case::kGuardianKill, "guardian-death-enosys"},
  };
  for (const auto& t : tests) if (!Run(t.test, t.name, scratch, static_cast<int>(limit.rlim_cur))) ++failures;
  const struct { ProductionCase test; const char* name; } production[]{
      {ProductionCase::kContinue, "continue-fdatasync-bypass"},
      {ProductionCase::kError, "callback-errno"},
      {ProductionCase::kBypass, "worker-fsync-unlink-bypass"},
      {ProductionCase::kTimeout, "deadline-poison-late-reply"},
      {ProductionCase::kNamespace, "namespace-syscalls"},
      {ProductionCase::kForeign, "raw-clone-foreign-callback"},
      {ProductionCase::kParentKill, "parent-death-eio"},
      {ProductionCase::kSignalQueue, "queued-signal-fd-path-reuse"},
      {ProductionCase::kFaultBeforeTransfer, "guardian-death-before-transfer"},
      {ProductionCase::kFaultTransfer, "scm-rights-failure"},
      {ProductionCase::kFaultAfterReady, "guardian-death-before-local-close"},
      {ProductionCase::kAppLikePollDenied, "app-like-deny-raw-poll"},
      {ProductionCase::kIdleTick, "idle-tick-worker-serialization"},
      {ProductionCase::kIoUringRoute, "io-uring-callback-routes"},
      {ProductionCase::kInitialRingRefusal, "initial-io-uring-refusal"},
  };
  for (const auto& t : production) if (!RunProduction(t.test, t.name, scratch)) ++failures;
  if (production_skips) printf("%s (%d failures, %d explicit skips)\n",
      failures ? "FAILED" : "PASS WITH SKIPS", failures, production_skips);
  else printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
  return failures ? 1 : 0;
}
