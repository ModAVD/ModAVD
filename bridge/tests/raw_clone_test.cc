/* Copyright 2026 jinghero. SPDX-License-Identifier: AGPL-3.0-or-later. */
// Exercise the actual trampoline and guard, including varargs ABI preservation.
#include "../companion/jni/native_to_guest_companion.cc"

#include <cstdarg>
#include <cstddef>
#include <initializer_list>
#include <poll.h>
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
int captured_calls = 0;
long fake_result = 0x123456789abcdef;
int fake_errno = EIO;
bool expect_protected = false;
unsigned externally_locked = 0;
std::atomic<bool> blocker_ready{false};
std::atomic<bool> release_blocker{false};
enum class TranslationEntry : uintptr_t { kReady = 1, kTranslating, kInvalidating, kWrapping };
struct TestCacheNode {
  TestCacheNode* left;
  TestCacheNode* right;
  TestCacheNode* parent;
  uintptr_t unused;
  uintptr_t guest_pc;
  std::atomic<uintptr_t>* slot;
};
static_assert(offsetof(TestCacheNode, slot) == 0x28);
struct TestTranslationCache {
  unsigned char unused[0x38];
  uintptr_t inflight;
  TestCacheNode* begin;
  TestCacheNode* root;
  size_t size;
} translation_cache;
static_assert(offsetof(TestTranslationCache, inflight) == 0x38 &&
              offsetof(TestTranslationCache, root) == 0x48 &&
              offsetof(TestTranslationCache, size) == 0x50);
TestCacheNode cache_nodes[3];
std::atomic<uintptr_t> cache_slots[3];

void ResetTestTranslations(TranslationEntry state = TranslationEntry::kReady) {
  for (int i = 0; i < 3; ++i) {
    cache_slots[i].store(static_cast<uintptr_t>(TranslationEntry::kReady));
    cache_nodes[i] = {nullptr, nullptr, &cache_nodes[1], 0, static_cast<uintptr_t>(i), &cache_slots[i]};
  }
  cache_nodes[1].left = &cache_nodes[0];
  cache_nodes[1].right = &cache_nodes[2];
  cache_nodes[1].parent = reinterpret_cast<TestCacheNode*>(&translation_cache.root);
  cache_slots[1].store(static_cast<uintptr_t>(state));
  translation_cache.inflight = state == TranslationEntry::kTranslating ? 1 : 0;
  translation_cache.begin = &cache_nodes[0];
  translation_cache.root = &cache_nodes[1];
  translation_cache.size = 3;
}

bool TestCacheReady() {
  bool ready = translation_cache.inflight == 0 && translation_cache.size == 3;
  for (const auto& slot : cache_slots)
    ready = ready && slot.load() == static_cast<uintptr_t>(TranslationEntry::kReady);
  return ready;
}

long Capture(long nr, ...) {
  ++captured_calls;
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
  if (expect_protected) CHECK(TestCacheReady());
  errno = fake_errno;
  return fake_result;
}

void CheckUnlocked() {
  for (int i = 0; i < kNumForkLocks; ++i) {
    CHECK(pthread_mutex_trylock(&locks[i]) == 0);
    pthread_mutex_unlock(&locks[i]);
  }
}

int CountOneModule(struct dl_phdr_info*, size_t, void* opaque) {
  ++*static_cast<int*>(opaque);
  return 1;
}

bool CanUseLoader() {
  int modules = 0;
  dl_iterate_phdr(&CountOneModule, &modules);
  void* handle = dlopen("libc.so", RTLD_NOW | RTLD_LOCAL);
  const bool ok = modules == 1 && handle != nullptr;
  if (handle != nullptr) dlclose(handle);
  return ok;
}

bool ChildCanResumeTranslation() {
  bool ok = TestCacheReady() && !g_fork_held.load() && g_fork_owner.load() == 0;
  // The worker must finish before clone. Child continuation must not need to
  // repair an inherited transition, and all mutexes must now be available.
  for (auto& mutex : locks) {
    if (pthread_mutex_trylock(&mutex) != 0)
      ok = false;
    else
      pthread_mutex_unlock(&mutex);
  }
  return CanUseLoader() && ok;
}

void CheckParentAfterChild(long child) {
  CHECK(child > 0);
  int status = 0;
  if (child > 0) {
    CHECK(waitpid(static_cast<pid_t>(child), &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 37);
  }
  // The guard must not rewrite either copy of the cache.
  CHECK(TestCacheReady());
  CheckUnlocked();
}

void* BlockThreadTable(void*) {
  pthread_mutex_lock(&locks[kNumForkLocks - 1]);
  blocker_ready.store(true, std::memory_order_release);
  while (!release_blocker.load(std::memory_order_acquire)) sched_yield();
  pthread_mutex_unlock(&locks[kNumForkLocks - 1]);
  return nullptr;
}

void* FinishCacheTransition(void*) {
  usleep(10000);
  pthread_mutex_lock(&locks[kForkTranslationCacheLock]);
  cache_slots[1].store(static_cast<uintptr_t>(TranslationEntry::kReady));
  translation_cache.inflight = 0;
  pthread_mutex_unlock(&locks[kForkTranslationCacheLock]);
  return nullptr;
}

void CheckCacheTransitions() {
  for (TranslationEntry state : {TranslationEntry::kTranslating, TranslationEntry::kInvalidating,
                                  TranslationEntry::kWrapping}) {
    ResetTestTranslations(state);
    CacheScanStats stats;
    CHECK(ScanForkCache(stats) == CacheState::kBusy);
    pthread_t worker;
    CHECK(pthread_create(&worker, nullptr, &FinishCacheTransition, nullptr) == 0);
    expect_protected = true;
    fake_result = 777;
    const int misses = g_fork_misses.load();
    CHECK(ModavdSyscallWrapper(SYS_clone, static_cast<long>(SIGCHLD), 0L, 11L, 22L, 33L) == 777);
    CHECK(errno == fake_errno && g_fork_misses.load() == misses && TestCacheReady());
    CHECK(pthread_join(worker, nullptr) == 0);
    CheckUnlocked();
  }
  // Busy entries remain untouched even when the existing retry budget expires.
  for (TranslationEntry state : {TranslationEntry::kTranslating, TranslationEntry::kInvalidating,
                                  TranslationEntry::kWrapping}) {
    ResetTestTranslations(state);
    expect_protected = false;
    const int misses = g_fork_misses.load();
    fake_result = 777;
    CHECK(ModavdSyscallWrapper(SYS_clone, static_cast<long>(SIGCHLD), 0L, 11L, 22L, 33L) == 777);
    CHECK(errno == fake_errno && g_fork_misses.load() == misses + 1 &&
          cache_slots[1].load() == static_cast<uintptr_t>(state));
    CHECK(!g_fork_held.load() && g_fork_owner.load() == 0);
    CheckUnlocked();
  }
  ResetTestTranslations();
  // Structural inconsistency must be unknown, not silently considered idle.
  translation_cache.size = 2;
  CacheScanStats stats;
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && !TryForkLocks());
  CHECK(g_fork_unknown_scans.load() != 0);
  ResetTestTranslations();
  cache_nodes[0].left = &cache_nodes[1];  // A cycle must also fail closed.
  stats = {};
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && !TryForkLocks());
  ResetTestTranslations();
  cache_nodes[0].slot = nullptr;
  stats = {};
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && !TryForkLocks());
  ResetTestTranslations();
  translation_cache.begin = &cache_nodes[2];
  stats = {};
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && !TryForkLocks());
  ResetTestTranslations();
  cache_nodes[1].left = cache_nodes[1].right;
  stats = {};
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && !TryForkLocks());
  ResetTestTranslations();
  cache_nodes[1].parent = nullptr;
  stats = {};
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && !TryForkLocks());
  ResetTestTranslations();
  cache_nodes[0].parent = &cache_nodes[2];
  stats = {};
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && !TryForkLocks());
  ResetTestTranslations();
  cache_nodes[0].guest_pc = 2;
  stats = {};
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && !TryForkLocks());
  ResetTestTranslations();
  cache_slots[0].store(0);
  stats = {};
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && !TryForkLocks());
  ResetTestTranslations();
  TestCacheNode deep_nodes[65]{};
  for (int i = 0; i < 65; ++i) {
    deep_nodes[i] = {nullptr, i < 64 ? &deep_nodes[i + 1] : nullptr,
                     i ? &deep_nodes[i - 1] : reinterpret_cast<TestCacheNode*>(&translation_cache.root),
                     0, static_cast<uintptr_t>(i), &cache_slots[0]};
  }
  translation_cache.begin = translation_cache.root = &deep_nodes[0];
  translation_cache.size = 65;
  stats = {};
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && !TryForkLocks());
  ResetTestTranslations();
}

struct InversionTest {
  std::atomic<bool> ready{false}, visit_loader{false}, visiting_loader{false}, done{false};
  bool rejected = false;
  long callback_ns = 0;
};

void* CodePoolThenLoader(void* opaque) {
  auto& test = *static_cast<InversionTest*>(opaque);
  pthread_mutex_lock(&locks[0]);
  test.ready.store(true);
  while (!test.visit_loader.load()) sched_yield();
  test.visiting_loader.store(true);
  int modules = 0;
  dl_iterate_phdr(&CountOneModule, &modules);
  pthread_mutex_unlock(&locks[0]);
  test.done.store(true);
  return nullptr;
}

int TryCloneWithInvertedWaiter(struct dl_phdr_info*, size_t, void* opaque) {
  auto& test = *static_cast<InversionTest*>(opaque);
  test.visit_loader.store(true);
  while (!test.visiting_loader.load()) sched_yield();
  RawCloneCall call{SYS_clone, SIGCHLD, 0, 0, 0, 0};
  const int calls = captured_calls;
  const long begin = MonoNs();
  const int rc = RawCloneUnderLoaderLock(nullptr, 0, &call);
  test.callback_ns = MonoNs() - begin;
  test.rejected = rc != 0 && !call.held && captured_calls == calls;
  CHECK(!test.done.load());  // The waiter needs this callback to exit first.
  return 1;
}

void CheckLockInversion() {
  InversionTest test;
  pthread_t worker;
  const int error = pthread_create(&worker, nullptr, &CodePoolThenLoader, &test);
  CHECK(error == 0);
  if (error != 0) return;
  while (!test.ready.load()) sched_yield();
  // This is the actual production callback, under the actual loader mutex,
  // against a thread that owns CodePool and is now waiting for that loader.
  dl_iterate_phdr(&TryCloneWithInvertedWaiter, &test);
  CHECK(pthread_join(worker, nullptr) == 0);
  CHECK(test.rejected && test.callback_ns < kForkLockWaitNs / 2 && test.done.load());
  CheckUnlocked();
}

struct LoaderProbe {
  std::atomic<bool> go{false}, visiting{false}, done{false};
};

void* VisitLoader(void* opaque) {
  auto& probe = *static_cast<LoaderProbe*>(opaque);
  while (!probe.go.load()) sched_yield();
  probe.visiting.store(true);
  int modules = 0;
  dl_iterate_phdr(&CountOneModule, &modules);
  probe.done.store(true);
  return nullptr;
}

int CloneInsideOuterLoaderScope(struct dl_phdr_info*, size_t, void* opaque) {
  auto& probe = *static_cast<LoaderProbe*>(opaque);
  ResetTestTranslations();
  const long child = ModavdSyscallWrapper(SYS_clone, static_cast<long>(SIGCHLD), 0L, 0L, 0L, 0L);
  if (child == 0) {
    syscall(SYS_exit, ChildCanResumeTranslation() ? 37 : 38);
    __builtin_unreachable();
  }
  CheckParentAfterChild(child);
  probe.go.store(true);
  while (!probe.visiting.load()) sched_yield();
  usleep(20000);
  CHECK(!probe.done.load());  // The caller's original loader level must remain.
  return 1;
}

void CheckRecursiveLoaderScope() {
  LoaderProbe probe;
  pthread_t worker;
  const int error = pthread_create(&worker, nullptr, &VisitLoader, &probe);
  CHECK(error == 0);
  if (error != 0) return;
  dl_iterate_phdr(&CloneInsideOuterLoaderScope, &probe);
  CHECK(pthread_join(worker, nullptr) == 0);
  CHECK(probe.done.load());  // Leaving the caller's level must now release it.
}

int child_notify_fd = -1;
void ChildLoaderTimeout(int) { syscall(SYS_exit, 39); }

long CloneAndNotifyChild(long nr, ...) {
  va_list ap;
  va_start(ap, nr);
  long args[5];
  for (auto& arg : args) arg = va_arg(ap, long);
  va_end(ap);
  const long result = syscall(nr, args[0], args[1], args[2], args[3], args[4]);
  const int saved = errno;
  if (result == 0) {
    signal(SIGALRM, &ChildLoaderTimeout);
    alarm(2);  // Turn a stale inherited loader mutex into a bounded test failure.
    const char byte = 'c';
    write(child_notify_fd, &byte, 1);
  }
  errno = saved;
  return result;
}

struct HeldLoaderTest {
  int notify_fd;
  std::atomic<bool> ready{false};
  int poll_result = -1;
};

int HoldLoaderUntilChildOrTimeout(struct dl_phdr_info*, size_t, void* opaque) {
  auto& test = *static_cast<HeldLoaderTest*>(opaque);
  test.ready.store(true);
  pollfd fd{test.notify_fd, POLLIN, 0};
  test.poll_result = poll(&fd, 1, 250);
  return 1;
}

void* HoldLoader(void* opaque) {
  dl_iterate_phdr(&HoldLoaderUntilChildOrTimeout, opaque);
  return nullptr;
}

void CheckInheritedLoaderMutex() {
  int pipe_fds[2];
  const int pipe_error = pipe(pipe_fds);
  CHECK(pipe_error == 0);
  if (pipe_error != 0) return;
  HeldLoaderTest test{pipe_fds[0]};
  child_notify_fd = pipe_fds[1];
  pthread_t worker;
  const int thread_error = pthread_create(&worker, nullptr, &HoldLoader, &test);
  CHECK(thread_error == 0);
  if (thread_error == 0) {
    while (!test.ready.load()) sched_yield();
    ResetTestTranslations();
    modavd_original_syscall = &CloneAndNotifyChild;
    const int misses = g_fork_misses.load();
    const long child = ModavdSyscallWrapper(
        SYS_clone, static_cast<long>(CLONE_VFORK | SIGCHLD), 0L, 0L, 0L, 0L);
    if (child == 0) {
      const bool ok = ChildCanResumeTranslation();
      alarm(0);
      syscall(SYS_exit, ok ? 37 : 38);
      __builtin_unreachable();
    }
    CheckParentAfterChild(child);
    CHECK(pthread_join(worker, nullptr) == 0);
    // Protected clone cannot begin until this other thread leaves the loader.
    // Without loader synchronisation the child notifies it, then hangs on its
    // own inherited copy even though the parent thread releases the real mutex.
    CHECK(test.poll_result == 0 && g_fork_misses.load() == misses);
    modavd_original_syscall = &syscall;
  }
  close(pipe_fds[0]);
  close(pipe_fds[1]);
  child_notify_fd = -1;
}

}  // namespace

int main() {
  for (int i = 0; i < kNumForkLocks; ++i) {
    pthread_mutex_init(&locks[i], nullptr);
    g_fork_locks[i] = &locks[i];
  }
  g_fork_translation_cache = &translation_cache;
  g_fork_transient_entries[0] = static_cast<uintptr_t>(TranslationEntry::kTranslating);
  g_fork_transient_entries[1] = static_cast<uintptr_t>(TranslationEntry::kInvalidating);
  g_fork_transient_entries[2] = static_cast<uintptr_t>(TranslationEntry::kWrapping);
  ResetTestTranslations();
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
  // These flags can overwrite host TLS or its cached tid. They must tail-jump
  // without entering the C++/loader path, retaining their original arguments.
  for (long flag : {static_cast<long>(CLONE_SETTLS), static_cast<long>(CLONE_PARENT_SETTID),
                    static_cast<long>(CLONE_CHILD_SETTID), static_cast<long>(CLONE_PIDFD)}) {
    CHECK(ModavdSyscallWrapper(SYS_clone, flag | SIGCHLD, 0L, 11L, 22L, 33L) == fake_result);
    CHECK(errno == fake_errno && arguments[0] == (flag | SIGCHLD) && arguments[1] == 0 &&
          arguments[2] == 11 && arguments[3] == 22 && arguments[4] == 33);
  }
  g_fork_safety.store(false);
  CHECK(ModavdSyscallWrapper(SYS_clone, static_cast<long>(SIGCHLD), 0L, 11L, 22L, 33L) ==
        fake_result);
  CHECK(TestCacheReady());
  g_fork_safety.store(true);
  expect_protected = true;
  for (long result : {777L, 0L, -1L}) {
    ResetTestTranslations();
    fake_result = result;
    CHECK(ModavdSyscallWrapper(SYS_clone, static_cast<long>(SIGCHLD), 0L, 11L, 22L, 33L) == result);
    CHECK(errno == fake_errno && arguments[2] == 11 && arguments[3] == 22 && arguments[4] == 33);
    CHECK(TestCacheReady());
    CheckUnlocked();
  }

  // A busy thread-table mutex must release the earlier mutexes,
  // keep the existing bounded fallback, and leave the other thread's lock alone.
  pthread_t blocker;
  CHECK(pthread_create(&blocker, nullptr, &BlockThreadTable, nullptr) == 0);
  while (!blocker_ready.load(std::memory_order_acquire)) sched_yield();
  externally_locked = 1u << (kNumForkLocks - 1);
  expect_protected = false;
  for (long result : {777L, 0L}) {
    ResetTestTranslations();
    fake_result = result;
    const int misses = g_fork_misses.load();
    const long begin = MonoNs();
    CHECK(ModavdSyscallWrapper(SYS_clone, static_cast<long>(SIGCHLD), 0L, 11L, 22L, 33L) ==
          fake_result);
    CHECK(MonoNs() - begin >= kForkLockWaitNs && g_fork_misses.load() == misses + 1);
    CHECK(errno == fake_errno && !g_fork_held.load() && g_fork_owner.load() == 0);
    CHECK(TestCacheReady());
  }
  release_blocker.store(true, std::memory_order_release);
  CHECK(pthread_join(blocker, nullptr) == 0);
  externally_locked = 0;
  CheckUnlocked();

  CheckLockInversion();
  CheckCacheTransitions();

  // Use the real kernel path: private fork and private-memory vfork. The child
  // must inherit an already-quiescent cache and see every mutex unlocked before
  // the translator's reset work begins. Neither cache copy may be rewritten.
  modavd_original_syscall = &syscall;
  for (long flags : {static_cast<long>(SIGCHLD), static_cast<long>(CLONE_VFORK | SIGCHLD)}) {
    ResetTestTranslations();
    const long child = ModavdSyscallWrapper(SYS_clone, flags, 0L, 0L, 0L, 0L);
    if (child == 0) {
      const bool ok = ChildCanResumeTranslation();
      syscall(SYS_exit, ok ? 37 : 38);
      __builtin_unreachable();
    }
    CheckParentAfterChild(child);
  }
  CheckRecursiveLoaderScope();
  CheckInheritedLoaderMutex();
  // The existing Bionic fork path uses the same child recovery handler.
  const int atfork_error = pthread_atfork(&ForkPrepare, &ForkParent, &ForkChild);
  CHECK(atfork_error == 0);
  if (atfork_error == 0) {
    ResetTestTranslations(TranslationEntry::kInvalidating);
    pthread_t worker;
    CHECK(pthread_create(&worker, nullptr, &FinishCacheTransition, nullptr) == 0);
    const pid_t child = fork();
    if (child == 0) {
      syscall(SYS_exit, ChildCanResumeTranslation() ? 37 : 38);
      __builtin_unreachable();
    }
    CheckParentAfterChild(child);
    CHECK(pthread_join(worker, nullptr) == 0);
  }
  for (auto& mutex : locks) pthread_mutex_destroy(&mutex);
  std::printf("raw clone ABI, loader locks and cache quiescence: %s (%d failures)\n",
              failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
