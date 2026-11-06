/* Copyright 2026 jinghero. SPDX-License-Identifier: AGPL-3.0-or-later. */
#include "download_listener.h"

#include <atomic>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/futex.h>
#include <new>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef SECCOMP_FILTER_FLAG_TSYNC_ESRCH
#define SECCOMP_FILTER_FLAG_TSYNC_ESRCH (1UL << 4)
#endif
#ifndef SECCOMP_USER_NOTIF_FLAG_CONTINUE
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE (1UL << 0)
#endif
#ifndef SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV
#define SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV (1UL << 5)
#endif

namespace modavd::download_listener {
namespace {
static_assert(SYS_fsync == 74 && SYS_clone == 56, "x86_64 companion only");
constexpr unsigned kCapacity = 32;
constexpr size_t kKernelBuffer = 1024;
constexpr int kStartupTimeoutMs = 2000;
constexpr int kTickIntervalMs = 100;
constexpr uint32_t kProtocol = 0x4d444c31;
constexpr long kCloseRange = 436;
constexpr long kPidfdOpen = 434;
constexpr uint64_t kCloseMagic = 0x4d444c434c4f5345ULL;

int InternalClose(int fd) {
  // close ignores unused arguments. This is an internal recursion/lifecycle
  // bypass, not a security boundary. Cleanup cannot depend on a live guardian.
  return static_cast<int>(syscall(SYS_close, fd, kCloseMagic));
}

int InitialRingError() {
  // This is pre-install admission, before threads/fork. Callers must exclude
  // concurrent ring producers until the new setup/enter/register filter exists.
  DIR* directory = opendir("/proc/self/fd");
  if (!directory) return errno;
  int error = 0;
  for (;;) {
    errno = 0;
    dirent* entry = readdir(directory);
    if (!entry) { error = errno; break; }
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
    char path[64], target[4096];
    const int length = snprintf(path, sizeof(path), "/proc/self/fd/%s", entry->d_name);
    if (length < 0 || length >= static_cast<int>(sizeof(path))) { error = ENAMETOOLONG; break; }
    const ssize_t count = readlink(path, target, sizeof(target) - 1);
    if (count < 0) {
      if (errno == ENOENT) continue;  // An fd closed during enumeration is gone.
      error = errno;
      break;
    }
    if (count >= static_cast<ssize_t>(sizeof(target) - 1)) { error = ENAMETOOLONG; break; }
    target[count] = '\0';
    if (strncmp(target, "anon_inode:", 11) == 0 && strstr(target, "io_uring")) {
      error = ENOTSUP;
      break;
    }
  }
  if (closedir(directory) != 0 && !error) error = errno;
  return error;
}

enum : int { kParked, kRun, kAbort };
enum : uint32_t { kRequest = 1, kReply = 2, kActivate = 3, kActivated = 4 };
struct Packet {
  uint32_t protocol;
  uint32_t kind;
  uint64_t epoch;
  uint64_t id;
  int64_t deadline;
  Request request;
  Decision decision;
};
struct State {
  Callback callback;
  Tick tick;
  void* context;
  int jobs;
  pid_t owner;
  uint64_t epoch;
  std::atomic<int> worker_tid{0};
  std::atomic<int> gate{kParked};
};
std::atomic<bool> g_started{false};
std::atomic<bool> g_installed{false};
static_assert(std::atomic<int>::is_always_lock_free && sizeof(std::atomic<int>) == sizeof(int));
struct Health {
  std::atomic<int> error{0};
#ifdef MODAVD_DOWNLOAD_LISTENER_TEST
  std::atomic<pid_t> last_received_tid{0};
#endif
};
Health* g_health = nullptr;  // Initialized before pthread_create/raw clone.
#ifdef MODAVD_DOWNLOAD_LISTENER_TEST
enum class StartupFault { kNone, kBeforeTransfer, kBadTransferFd, kAfterReady };
StartupFault g_startup_fault = StartupFault::kNone;
void (*g_before_guardian_kill)(int) = nullptr;

void KillStartupGuardian(pid_t guardian) {
  syscall(SYS_kill, guardian, SIGKILL);
  int status;
  while (syscall(SYS_wait4, guardian, &status, 0, nullptr) < 0 && errno == EINTR) {}
}
#endif

void Poison(int error) {
  if (!g_health) return;
  int healthy = 0;
  g_health->error.compare_exchange_strong(healthy, error, std::memory_order_acq_rel);
}

int64_t ClockMs() {
  timespec ts{};
  if (syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts) != 0) return -1;
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

long RawPoll(pollfd* fds, nfds_t count, int timeout_ms) {
  // Android's x86_64 app allowlist admits ppoll, not the legacy poll syscall.
  // Keep this child-after-fork path independent of libc's poll wrapper.
  timespec timeout{};
  if (timeout_ms >= 0) {
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000;
  }
  return syscall(SYS_ppoll, fds, count, timeout_ms < 0 ? nullptr : &timeout,
                 nullptr, sizeof(uint64_t));
}

[[noreturn]] void ExitGuardian(int status) {
  if (status) Poison(EIO);
  syscall(SYS_exit_group, status);
  __builtin_unreachable();
}

void Wake(std::atomic<int>& value) {
  syscall(SYS_futex, reinterpret_cast<int*>(&value), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
}

void ReleaseWorker(State* s, int gate) {
  s->gate.store(gate, std::memory_order_release);
  Wake(s->gate);
}

void* Worker(void* opaque) {
  State* s = static_cast<State*>(opaque);
  s->worker_tid.store(static_cast<int>(syscall(SYS_gettid)), std::memory_order_release);
  Wake(s->worker_tid);
  while (s->gate.load(std::memory_order_acquire) == kParked)
    syscall(SYS_futex, reinterpret_cast<int*>(&s->gate), FUTEX_WAIT_PRIVATE, kParked,
            nullptr, nullptr, 0);
  if (s->gate.load(std::memory_order_acquire) == kRun) {
    int64_t next_tick = ClockMs() + kTickIntervalMs;
    for (;;) {
      const int64_t before_wait = ClockMs();
      if (before_wait < 0) break;
      const int wait_ms = next_tick <= before_wait ? 0 : static_cast<int>(next_tick - before_wait);
      pollfd jobs{s->jobs, POLLIN, 0};
      const long ready = RawPoll(&jobs, 1, wait_ms);
      if (ready < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (ready == 0) {
        if (s->tick) s->tick(s->context);
        const int64_t after_tick = ClockMs();
        if (after_tick < 0) break;
        next_tick = after_tick + kTickIntervalMs;
        continue;
      }
      if (!(jobs.revents & POLLIN)) break;
      Packet packet{};
      ssize_t count;
      do { count = recv(s->jobs, &packet, sizeof(packet), MSG_TRUNC | MSG_DONTWAIT); }
      while (count < 0 && errno == EINTR);
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      if (count != sizeof(packet)) break;
      if (packet.protocol != kProtocol || packet.kind != kRequest ||
          packet.epoch != s->epoch || packet.id != packet.request.notification.id ||
          packet.request.epoch != s->epoch || packet.request.owner_pid != s->owner) break;
      if (s->tick) s->tick(s->context);
      const int64_t now = ClockMs();
      if (now < 0) break;
      next_tick = now + kTickIntervalMs;
      // Expired work still sitting in the IPC socket must never start a new
      // mutation after the guardian has already returned its timeout.
      packet.decision = now < 0 || now >= packet.deadline
          ? Decision::Result(0, ETIMEDOUT) : s->callback(packet.request, s->context);
      packet.kind = kReply;
      do { count = send(s->jobs, &packet, sizeof(packet), MSG_NOSIGNAL); }
      while (count < 0 && errno == EINTR);
      if (count != sizeof(packet)) break;
    }
  }
  if (s->gate.load(std::memory_order_acquire) == kRun) Poison(EIO);
  // No parent-held duplicate remains after Start. A fork child can still hold
  // this endpoint, so the guardian also enforces per-notification deadlines.
  InternalClose(s->jobs);
  return nullptr;
}

bool TransferListener(int channel, int listener) {
  char marker = 'L';
  iovec iov{&marker, 1};
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
  msghdr message{};
  message.msg_iov = &iov;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  cmsghdr* c = CMSG_FIRSTHDR(&message);
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  c->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(c), &listener, sizeof(listener));
  return syscall(SYS_sendmsg, channel, &message, MSG_NOSIGNAL) == 1;
}

int AcceptListener(int channel) {
  char marker;
  iovec iov{&marker, 1};
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
  msghdr message{};
  message.msg_iov = &iov;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  if (syscall(SYS_recvmsg, channel, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT) != 1 ||
      marker != 'L' || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) return -1;
  cmsghdr* c = CMSG_FIRSTHDR(&message);
  if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
      c->cmsg_len != CMSG_LEN(sizeof(int))) return -1;
  int listener;
  memcpy(&listener, CMSG_DATA(c), sizeof(listener));
  return listener;
}

void CloseOthers(int a, int b, unsigned fd_limit) {
  const unsigned low = static_cast<unsigned>(a < b ? a : b);
  const unsigned high = static_cast<unsigned>(a < b ? b : a);
  bool fast = true;
  if (low && syscall(kCloseRange, 0U, low - 1, 0U) != 0) fast = false;
  if (low + 1 < high && syscall(kCloseRange, low + 1, high - 1, 0U) != 0) fast = false;
  if (syscall(kCloseRange, high + 1, ~0U, 0U) != 0) fast = false;
  if (!fast) for (unsigned fd = 0; fd < fd_limit; ++fd)
    if (fd != low && fd != high) syscall(SYS_close, fd);
}

struct Pending {
  bool occupied;
  bool dispatched;
  int64_t deadline;
  Packet packet;
};

bool SendResult(int listener, uint64_t id, Decision result, bool dispatched = false) {
  alignas(8) unsigned char storage[kKernelBuffer]{};
  auto* response = reinterpret_cast<seccomp_notif_resp*>(storage);
  response->id = id;
  if (result.action == Decision::kContinue && !result.value && !result.error) {
    response->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
  } else if (result.action == Decision::kResult && result.error >= 0 && result.error <= 4095) {
    response->val = result.error ? 0 : result.value;
    response->error = -result.error;
  } else {
    response->error = -EPROTO;
    Poison(EPROTO);
  }
  long rc;
  do { rc = syscall(SYS_ioctl, listener, SECCOMP_IOCTL_NOTIF_SEND, response); }
  while (rc < 0 && errno == EINTR);
  if (rc < 0 && errno == ENOENT && dispatched) Poison(ECANCELED);
  if (rc < 0 && errno != ENOENT) Poison(EIO);
  return rc == 0 || errno == ENOENT;
}

// After raw clone this process calls only syscall wrappers and memory functions.
// No callback, allocation, pthread, logger, or game/translator code runs here.
[[noreturn]] void Guardian(int boot, int jobs, unsigned fd_limit, pid_t owner,
                         pid_t worker_tid, pid_t startup_tid, uint64_t epoch, unsigned timeout_ms) {
  CloseOthers(boot, jobs, fd_limit);
  // Keep asynchronous inherited handlers from entering a possibly locked libc
  // or game library. SIGKILL remains effective. Synchronous faults still abort.
  const uint64_t blocked = ~uint64_t{0};
  syscall(SYS_rt_sigprocmask, SIG_SETMASK, &blocked, nullptr, sizeof(blocked));
  const int parent_pidfd = static_cast<int>(syscall(kPidfdOpen, owner, 0));
  pollfd startup{boot, POLLIN, 0};
  if (RawPoll(&startup, 1, kStartupTimeoutMs) != 1 || !(startup.revents & POLLIN))
    ExitGuardian(120);
  const int listener = AcceptListener(boot);
  if (listener < 0) ExitGuardian(121);
  char ack = 'A';
  if (syscall(SYS_sendto, boot, &ack, 1, MSG_NOSIGNAL, nullptr, 0) != 1) ExitGuardian(122);
  syscall(SYS_close, boot);

  Pending pending[kCapacity]{};
  alignas(8) unsigned char buffer[kKernelBuffer];
  bool worker_live = true;
  bool parent_live = true;
  bool activated = false;
  for (;;) {
    const int64_t now = ClockMs();
    if (now < 0) ExitGuardian(123);
    int wait_ms = 100;
    bool unsent = false;
    for (auto& p : pending) {
      if (!p.occupied) continue;
      const int expired = !worker_live || !parent_live ? EIO : now >= p.deadline ? ETIMEDOUT : 0;
      if (expired) {
        if (p.dispatched || expired == EIO) Poison(expired);
        if (!SendResult(listener, p.packet.id, Decision::Result(0, expired), p.dispatched))
          ExitGuardian(124);
        p.occupied = false;
        continue;
      }
      if (activated && !p.dispatched) {
        if (syscall(SYS_ioctl, listener, SECCOMP_IOCTL_NOTIF_ID_VALID, &p.packet.id) != 0) {
          if (errno == ENOENT) { p.occupied = false; continue; }
          if (errno == EINTR) { unsent = true; continue; }
          Poison(EIO);
          ExitGuardian(134);
        }
        const long sent = syscall(SYS_sendto, jobs, &p.packet, sizeof(p.packet),
                                  MSG_NOSIGNAL | MSG_DONTWAIT, nullptr, 0);
        if (sent == sizeof(p.packet)) p.dispatched = true;
        else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
          unsent = true;
        else { worker_live = false; Poison(EIO); }
      }
      const int64_t remaining = p.deadline - now;
      if (remaining < wait_ms) wait_ms = static_cast<int>(remaining);
    }
    pollfd fds[3]{{listener, POLLIN, 0},
                  {worker_live ? jobs : -1, static_cast<short>(POLLIN | (unsent ? POLLOUT : 0)), 0},
                  {parent_live ? parent_pidfd : -1, POLLIN, 0}};
    const long ready = RawPoll(fds, 3, wait_ms);
    if (ready < 0) {
      if (errno == EINTR) continue;
      ExitGuardian(125);
    }
    if (fds[2].revents & (POLLIN | POLLHUP | POLLERR)) { parent_live = false; Poison(EIO); }
    if (fds[1].revents & POLLIN) {
      Packet reply{};
      const long count = syscall(SYS_recvfrom, jobs, &reply, sizeof(reply),
                                 MSG_DONTWAIT | MSG_TRUNC, nullptr, nullptr);
      if (count == sizeof(reply) && reply.protocol == kProtocol && reply.kind == kActivate &&
          reply.epoch == epoch && !activated) {
        activated = true;
        reply.kind = kActivated;
        if (syscall(SYS_sendto, jobs, &reply, sizeof(reply), MSG_NOSIGNAL | MSG_DONTWAIT,
                    nullptr, 0) != sizeof(reply)) ExitGuardian(133);
      } else if (count == sizeof(reply) && reply.protocol == kProtocol && reply.kind == kReply &&
                 reply.epoch == epoch) {
        for (auto& p : pending) {
          if (!p.occupied || !p.dispatched || p.packet.id != reply.id) continue;
          // A queued/late reply is never allowed to publish success past deadline.
          const int64_t current = ClockMs();
          const Decision result = !parent_live || !worker_live ? Decision::Result(0, EIO)
              : current < 0 || current >= p.deadline ? Decision::Result(0, ETIMEDOUT)
              : reply.decision;
          if (current < 0 || current >= p.deadline) Poison(ETIMEDOUT);
          if (!SendResult(listener, reply.id, result, true)) ExitGuardian(126);
          p.occupied = false;
          break;
        }
      } else if (!(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) {
        worker_live = false;
        Poison(EIO);
      }
    }
    if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) { worker_live = false; Poison(EIO); }
    if (fds[0].revents & POLLIN) {
      memset(buffer, 0, sizeof(buffer));
      auto* request = reinterpret_cast<seccomp_notif*>(buffer);
      if (syscall(SYS_ioctl, listener, SECCOMP_IOCTL_NOTIF_RECV, request) < 0) {
        if (errno == EINTR || errno == ENOENT) continue;
        ExitGuardian(127);
      }
#ifdef MODAVD_DOWNLOAD_LISTENER_TEST
      g_health->last_received_tid.store(static_cast<pid_t>(request->pid), std::memory_order_release);
#endif
      // The callback worker must be able to fsync its journal and execute the
      // namespace syscall it emulates without waiting on its own callback.
      if (parent_live && ((worker_live && activated &&
          request->pid == static_cast<uint32_t>(worker_tid)) ||
          (!activated && request->pid == static_cast<uint32_t>(startup_tid)))) {
        if (!SendResult(listener, request->id, Decision::Continue())) ExitGuardian(128);
        continue;
      }
      if (!worker_live || !parent_live) {
        if (!SendResult(listener, request->id, Decision::Result(0, EIO))) ExitGuardian(129);
        continue;
      }
      Pending* slot = nullptr;
      for (auto& p : pending) if (!p.occupied) { slot = &p; break; }
      if (!slot) {
        if (!SendResult(listener, request->id, Decision::Result(0, EBUSY))) ExitGuardian(130);
        continue;
      }
      const int64_t admitted = ClockMs();
      if (admitted < 0) ExitGuardian(131);
      slot->occupied = true;
      slot->dispatched = false;
      slot->deadline = admitted + timeout_ms;
      slot->packet = {};
      slot->packet.protocol = kProtocol;
      slot->packet.kind = kRequest;
      slot->packet.epoch = epoch;
      slot->packet.id = request->id;
      slot->packet.deadline = slot->deadline;
      slot->packet.request = Request{*request, owner, epoch};
    }
    if (fds[0].revents & POLLHUP) ExitGuardian(0);
    if (fds[0].revents & (POLLERR | POLLNVAL)) ExitGuardian(132);
  }
}

int InstallFilter() {
  sock_filter code[]{
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 0, 25),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_fsync, 22, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_rename, 21, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_renameat, 20, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_renameat2, 19, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_link, 18, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_linkat, 17, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_unlink, 16, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_unlinkat, 15, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_close, 10, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 436, 13, 0),  // close_range
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_dup2, 12, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_dup3, 11, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_unshare, 10, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_setns, 9, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 8, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 435, 7, 0),  // clone3
      // A platform that permits io_uring must retire fd-cache optimization
      // before setup/enter/register can expose asynchronous fd-table mutation.
      // A stricter inherited Android action keeps its normal precedence.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 425, 6, 0),  // io_uring_setup
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 426, 5, 0),  // io_uring_enter
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 427, 4, 5),  // register; other non-close -> ALLOW
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[1])),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(kCloseMagic), 0, 2),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[1]) + 4),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(kCloseMagic >> 32), 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  static_assert(sizeof(code) / sizeof(code[0]) == 28, "update the filter jump layout");
  sock_fprog program{static_cast<unsigned short>(sizeof(code) / sizeof(code[0])), code};
  return static_cast<int>(syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
      SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_TSYNC_ESRCH |
          SECCOMP_FILTER_FLAG_NEW_LISTENER | SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV, &program));
}
}  // namespace

StartResult Start(Callback callback, void* context, unsigned timeout_ms, Tick tick) {
  StartResult result{0, false, -1, -1};
  if (!callback || timeout_ms == 0 || timeout_ms > 60000) { result.error = EINVAL; return result; }
  bool unused = false;
  if (!g_started.compare_exchange_strong(unused, true, std::memory_order_acq_rel)) {
    result.error = EALREADY;
    result.filter_installed = g_installed.load(std::memory_order_acquire);
    return result;
  }
  result.error = InitialRingError();
  if (result.error) return result;
  seccomp_notif_sizes sizes{};
  if (syscall(SYS_seccomp, SECCOMP_GET_NOTIF_SIZES, 0, &sizes) != 0) {
    result.error = errno; return result;
  }
  if (sizes.seccomp_notif > kKernelBuffer || sizes.seccomp_notif_resp > kKernelBuffer) {
    result.error = EOVERFLOW; return result;
  }
  rlimit limit{};
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0) { result.error = errno; return result; }
  if (limit.rlim_cur > 1048576) { result.error = EOVERFLOW; return result; }
  void* shared = mmap(nullptr, sizeof(Health), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (shared == MAP_FAILED) { result.error = errno; return result; }
  g_health = new (shared) Health;
  int jobs[2], boot[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, jobs) != 0) {
    result.error = errno; return result;
  }
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, boot) != 0) {
    result.error = errno; InternalClose(jobs[0]); InternalClose(jobs[1]); return result;
  }
  State* state = new (std::nothrow) State;
  if (!state) {
    result.error = ENOMEM; InternalClose(jobs[0]); InternalClose(jobs[1]); InternalClose(boot[0]); InternalClose(boot[1]);
    return result;
  }
  state->callback = callback;
  state->tick = tick;
  state->context = context;
  state->jobs = jobs[0];
  state->owner = static_cast<pid_t>(syscall(SYS_getpid));
  const int64_t started = ClockMs();
  if (started < 0) {
    result.error = errno ? errno : EIO;
    delete state; InternalClose(jobs[0]); InternalClose(jobs[1]); InternalClose(boot[0]); InternalClose(boot[1]); return result;
  }
  state->epoch = (static_cast<uint64_t>(started) << 24) ^ static_cast<uint64_t>(state->owner);
  pthread_t thread;
  result.error = pthread_create(&thread, nullptr, Worker, state);
  if (result.error) {
    delete state; InternalClose(jobs[0]); InternalClose(jobs[1]); InternalClose(boot[0]); InternalClose(boot[1]); return result;
  }
  // The worker publishes its TID before entering an allocation-free parked state.
  while (!state->worker_tid.load(std::memory_order_acquire)) {
    timespec wait{0, 10000000};
    syscall(SYS_futex, reinterpret_cast<int*>(&state->worker_tid), FUTEX_WAIT_PRIVATE,
            0, &wait, nullptr, 0);
    const int64_t now = ClockMs();
    if (now < 0 || now - started > kStartupTimeoutMs) { result.error = ETIMEDOUT; break; }
  }
  result.worker_tid = state->worker_tid.load(std::memory_order_acquire);
  if (!result.error) {
    // The child runs no guest code and deliberately bypasses inherited atfork
    // handlers. Its private address space needs no translator lock repair.
    const pid_t startup_tid = static_cast<pid_t>(syscall(SYS_gettid));
    const long child = syscall(SYS_clone, static_cast<unsigned long>(SIGCHLD),
                               nullptr, nullptr, nullptr, 0);
    if (child == 0) Guardian(boot[1], jobs[1], static_cast<unsigned>(limit.rlim_cur),
                            state->owner, result.worker_tid,
                            startup_tid, state->epoch, timeout_ms);
    if (child < 0) result.error = errno;
    else result.guardian_pid = static_cast<pid_t>(child);
  }
  InternalClose(jobs[1]); InternalClose(boot[1]);
  if (!result.error && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) result.error = errno;
  if (!result.error) {
    const int listener = InstallFilter();
    if (listener < 0) result.error = errno;
    else {
      result.filter_installed = true;
      g_installed.store(true, std::memory_order_release);
      int channel = boot[0];
#ifdef MODAVD_DOWNLOAD_LISTENER_TEST
      if (g_startup_fault == StartupFault::kBeforeTransfer)
        KillStartupGuardian(result.guardian_pid);
      if (g_startup_fault == StartupFault::kBadTransferFd) channel = -1;
#endif
      const bool sent = TransferListener(channel, listener);
      const int send_error = errno;
#ifdef MODAVD_DOWNLOAD_LISTENER_TEST
      if (sent && g_startup_fault == StartupFault::kAfterReady) {
        pollfd ready{boot[0], POLLIN, 0};
        if (RawPoll(&ready, 1, kStartupTimeoutMs) == 1 && (ready.revents & POLLIN)) {
          if (g_before_guardian_kill) g_before_guardian_kill(listener);
          KillStartupGuardian(result.guardian_pid);
        }
      }
#endif
      InternalClose(listener);
      if (!sent) result.error = send_error ? send_error : EIO;
    }
  }
  if (!result.error) {
    pollfd ready{boot[0], POLLIN, 0};
    const int64_t deadline = ClockMs() + kStartupTimeoutMs;
    int polled;
    do {
      const int64_t remaining = deadline - ClockMs();
      if (remaining <= 0) { polled = 0; break; }
      polled = static_cast<int>(RawPoll(&ready, 1, static_cast<int>(remaining)));
    } while (polled < 0 && errno == EINTR);
    char ack = 0;
    if (polled != 1 || !(ready.revents & POLLIN)) result.error = polled < 0 ? errno : ETIMEDOUT;
    else if (recv(boot[0], &ack, 1, MSG_DONTWAIT) != 1 || ack != 'A') result.error = EPROTO;
  }
  InternalClose(boot[0]);
  if (!result.error) {
    Packet activate{};
    activate.protocol = kProtocol;
    activate.kind = kActivate;
    activate.epoch = state->epoch;
    if (send(jobs[0], &activate, sizeof(activate), MSG_NOSIGNAL | MSG_DONTWAIT) != sizeof(activate))
      result.error = errno ? errno : EIO;
    if (!result.error) {
      pollfd active{jobs[0], POLLIN, 0};
      Packet ack{};
      if (RawPoll(&active, 1, kStartupTimeoutMs) != 1 || !(active.revents & POLLIN))
        result.error = ETIMEDOUT;
      else if (recv(jobs[0], &ack, sizeof(ack), MSG_DONTWAIT | MSG_TRUNC) != sizeof(ack) ||
               ack.protocol != kProtocol || ack.kind != kActivated || ack.epoch != state->epoch)
        result.error = EPROTO;
    }
  }
  if (result.error) Poison(result.error);
  ReleaseWorker(state, result.error ? kAbort : kRun);
  // State is deliberately process-lifetime storage. It must never be freed
  // while a parked worker can still wake after a failed startup.
  pthread_detach(thread);
  return result;
}

bool Healthy() {
  return g_health && g_health->error.load(std::memory_order_acquire) == 0 &&
         g_installed.load(std::memory_order_acquire);
}
}  // namespace modavd::download_listener
