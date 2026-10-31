/* Copyright 2026 jinghero. SPDX-License-Identifier: AGPL-3.0-or-later. */
// Exercise the actual trampoline and guard, including varargs ABI preservation.
#include "../companion/jni/native_to_guest_companion.cc"

#include <cstdarg>
#include <initializer_list>
#include <sys/wait.h>

namespace {
int failures = 0;
#define CHECK(x)                                           \
  do {                                                     \
    if (!(x)) {                                            \
      std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); \
      ++failures;                                          \
    }                                                      \
  } while (0)
pthread_mutex_t locks[kNumForkLocks];
int argument_count = 0;
long arguments[6] = {};
long captured_nr = 0;
long fake_result = 0x123456789abcdef;
int fake_errno = EIO;
bool expect_protected = false;
unsigned externally_locked = 0;
std::atomic<bool> blocker_ready{false};
std::atomic<bool> release_blocker{false};

long Capture(long nr, ...) {
  captured_nr = nr;
  va_list ap;
  va_start(ap, nr);
  for (int i = 0; i < argument_count; ++i) arguments[i] = va_arg(ap, long);
  va_end(ap);
  for (int i = 0; i < kNumForkLocks; ++i) {
    const int error = pthread_mutex_trylock(&locks[i]);
    const bool locked = expect_protected || (externally_locked & (1u << i));
    CHECK(locked ? error == EBUSY : error == 0);
    if (error == 0) pthread_mutex_unlock(&locks[i]);
  }
  errno = fake_errno;
  return fake_result;
}

void CheckUnlocked() {
  for (int i = 0; i < kNumForkLocks; ++i) {
    CHECK(pthread_mutex_trylock(&locks[i]) == 0);
    pthread_mutex_unlock(&locks[i]);
  }
}

void* BlockThreadTable(void*) {
  pthread_mutex_lock(&locks[kNumForkLocks - 1]);
  blocker_ready.store(true, std::memory_order_release);
  while (!release_blocker.load(std::memory_order_acquire)) sched_yield();
  pthread_mutex_unlock(&locks[kNumForkLocks - 1]);
  return nullptr;
}

}  // namespace

int main() {
  for (int i = 0; i < kNumForkLocks; ++i) {
    pthread_mutex_init(&locks[i], nullptr);
    g_fork_locks[i] = &locks[i];
  }
  modavd_original_syscall = &Capture;
  g_fork_safety.store(true);
  const long values[] = {0x112233445566778, -2, 0x223344556677889, -4, 0x33445566778899a, -6};
  // Non-clone syscalls must retain all six arguments, including the stack slot.
  for (int count = 0; count <= 6; ++count) {
    argument_count = count;
    expect_protected = false;
    long actual = 0;
    switch (count) {
      case 0:
        actual = ModavdSyscallWrapper(SYS_getpid);
        break;
      case 1:
        actual = ModavdSyscallWrapper(SYS_close, values[0]);
        break;
      case 2:
        actual = ModavdSyscallWrapper(SYS_kill, values[0], values[1]);
        break;
      case 3:
        actual = ModavdSyscallWrapper(SYS_write, values[0], values[1], values[2]);
        break;
      case 4:
        actual =
            ModavdSyscallWrapper(SYS_rt_sigprocmask, values[0], values[1], values[2], values[3]);
        break;
      case 5:
        actual =
            ModavdSyscallWrapper(SYS_mremap, values[0], values[1], values[2], values[3], values[4]);
        break;
      case 6:
        actual = ModavdSyscallWrapper(SYS_mmap, values[0], values[1], values[2], values[3],
                                      values[4], values[5]);
        break;
    }
    CHECK(actual == fake_result && captured_nr != SYS_clone && errno == fake_errno);
    for (int i = 0; i < count; ++i) CHECK(arguments[i] == values[i]);
  }
  argument_count = 5;
  // Shared VM and alternate host stacks retain their original call path.
  CHECK(ModavdSyscallWrapper(SYS_clone, static_cast<long>(CLONE_VM | CLONE_VFORK | SIGCHLD), 0L,
                             11L, 22L, 33L) == fake_result);
  CHECK(arguments[1] == 0 && arguments[2] == 11 && arguments[3] == 22 && arguments[4] == 33);
  CHECK(ModavdSyscallWrapper(SYS_clone, static_cast<long>(SIGCHLD), 99L, 11L, 22L, 33L) ==
        fake_result);
  g_fork_safety.store(false);
  CHECK(ModavdSyscallWrapper(SYS_clone, static_cast<long>(SIGCHLD), 0L, 11L, 22L, 33L) ==
        fake_result);
  g_fork_safety.store(true);
  expect_protected = true;
  for (long result : {777L, 0L, -1L}) {
    fake_result = result;
    CHECK(ModavdSyscallWrapper(SYS_clone, static_cast<long>(SIGCHLD), 0L, 11L, 22L, 33L) == result);
    CHECK(errno == fake_errno && arguments[2] == 11 && arguments[3] == 22 && arguments[4] == 33);
    CheckUnlocked();
  }

  // A busy thread-table mutex must release partially acquired code-pool locks,
  // keep the existing bounded fallback, and leave the other thread's lock alone.
  pthread_t blocker;
  CHECK(pthread_create(&blocker, nullptr, &BlockThreadTable, nullptr) == 0);
  while (!blocker_ready.load(std::memory_order_acquire)) sched_yield();
  externally_locked = 1u << (kNumForkLocks - 1);
  expect_protected = false;
  fake_result = 777;
  const int misses = g_fork_misses.load();
  const long begin = MonoNs();
  CHECK(ModavdSyscallWrapper(SYS_clone, static_cast<long>(SIGCHLD), 0L, 11L, 22L, 33L) ==
        fake_result);
  CHECK(MonoNs() - begin >= kForkLockWaitNs && g_fork_misses.load() == misses + 1);
  CHECK(errno == fake_errno && !g_fork_held.load() && g_fork_owner.load() == 0);
  release_blocker.store(true, std::memory_order_release);
  CHECK(pthread_join(blocker, nullptr) == 0);
  externally_locked = 0;
  CheckUnlocked();

  // Use the real kernel path: private fork and private-memory vfork. The child
  // must see every mutex unlocked before the translator's reset work begins.
  modavd_original_syscall = &syscall;
  for (long flags : {static_cast<long>(SIGCHLD), static_cast<long>(CLONE_VFORK | SIGCHLD)}) {
    const long child = ModavdSyscallWrapper(SYS_clone, flags, 0L, 0L, 0L, 0L);
    if (child == 0) {
      bool ok = true;
      for (int i = 0; i < kNumForkLocks; ++i) {
        if (pthread_mutex_trylock(&locks[i]) != 0)
          ok = false;
        else
          pthread_mutex_unlock(&locks[i]);
      }
      syscall(SYS_exit, ok ? 37 : 38);
      __builtin_unreachable();
    }
    CHECK(child > 0);
    int status = 0;
    if (child > 0) {
      CHECK(waitpid(static_cast<pid_t>(child), &status, 0) == child);
      CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 37);
    }
    CheckUnlocked();
  }
  for (auto& mutex : locks) pthread_mutex_destroy(&mutex);
  std::printf("raw clone ABI and locks: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
