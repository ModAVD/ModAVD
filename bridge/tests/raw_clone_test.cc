/* Copyright 2026 jinghero. SPDX-License-Identifier: AGPL-3.0-or-later. */
// Exercise the actual trampoline and guard, including varargs ABI preservation.
#include "../companion/jni/native_to_guest_companion.cc"

#include <cstdarg>
#include <cstddef>
#include <initializer_list>
#include <new>
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

extern "C" __attribute__((used, visibility("hidden"))) long Capture(long nr, ...) {
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

extern "C" {
unsigned char captured_al = 0;
__attribute__((naked)) long CaptureAl(long, ...) {
  __asm__ volatile("movb %al, captured_al(%rip)\n jmp Capture\n");
}
__attribute__((naked)) long InvokeWithAl(long, ...) {
  __asm__ volatile("movb $7, %al\n jmp ModavdSyscallWrapper\n");
}
}

bool RejectUnknownForkCache() {
  g_fork_safety.store(true);
  const bool refused = TryForkLocks() == ForkAttempt::kUnknown;
  CHECK(!g_fork_safety.load());
  const int unknown = g_fork_unknown_scans.load();
  CHECK(TryForkLocks() == ForkAttempt::kDisabled && g_fork_unknown_scans.load() == unknown);
  return refused;
}

void CheckTidRanges() {
  const HostRange tp{0x1000, 0x2000}, thread{0x3000, 0x4000};
  const long p = CLONE_PARENT_SETTID, c = CLONE_CHILD_SETTID, clear = CLONE_CHILD_CLEARTID;
  CHECK(TidOutputsOutsideHostRanges(p, 0, UINTPTR_MAX, tp, thread));
  CHECK(TidOutputsOutsideHostRanges(c, UINTPTR_MAX, 0, tp, thread));
  CHECK(TidOutputsOutsideHostRanges(p | c, 0x5000, 0x5000, tp, thread));
  CHECK(TidOutputsOutsideHostRanges(p, 0xffc, 0, tp, thread));
  CHECK(TidOutputsOutsideHostRanges(c, 0, 0x2000, tp, thread));
  CHECK(!TidOutputsOutsideHostRanges(p, 0xffe, 0, tp, thread));
  CHECK(!TidOutputsOutsideHostRanges(c, 0, 0x2ffe, tp, thread));
  CHECK(!TidOutputsOutsideHostRanges(clear, 0, 0x3000, tp, thread));
  CHECK(!TidOutputsOutsideHostRanges(p, UINTPTR_MAX - 2, 0, tp, thread));
  CHECK(!TidOutputsOutsideHostRanges(c, 0, UINTPTR_MAX, tp, thread));
  CHECK(!TidOutputsOutsideHostRanges(p, 0x5000, 0, {}, thread));
  CHECK(!TidOutputsOutsideHostRanges(c, 0, 0x5000, tp, {}));
  CHECK(!TidOutputsOutsideHostRanges(p, 0x5ffe, 0, tp, thread, {0x6000, 0x7000}));
  HostRange real_tp, real_thread;
  CHECK(FindHostThreadRanges(real_tp, real_thread));
  CHECK(!ModavdCloneTidOutputsSafe(p, reinterpret_cast<long>(__builtin_thread_pointer()), 0));
  CHECK(!ModavdCloneTidOutputsSafe(c, 0, static_cast<long>(pthread_self())));
  int stack_tid = 0;
  CHECK(!ModavdCloneTidOutputsSafe(p, reinterpret_cast<long>(&stack_tid), 0));
  CHECK(!ModavdCloneTidOutputsSafe(p, static_cast<long>(real_tp.begin - 2), 0));
  const int saved = ERANGE;
  errno = saved;
  CHECK(ModavdCloneTidOutputsSafe(p, 0, -1) && errno == saved);
  errno = saved;
  CHECK(!CloneTidOutputsSafeWithMaps(p, 0, 0, "/proc/self/modavd-missing-maps") && errno == saved);
  errno = saved;
  CHECK(!ModavdCloneTidOutputsSafe(p, reinterpret_cast<long>(&stack_tid), 0) && errno == saved);

  // A guard page on each side prevents coalescing this guest-like tid mapping
  // with the current host pthread/stack VMA. This is not an ARM guest test.
  const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  void* region = mmap(nullptr, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(region != MAP_FAILED);
  if (region != MAP_FAILED) {
    auto* tid = static_cast<char*>(region) + page;
    CHECK(mprotect(tid, page, PROT_READ | PROT_WRITE) == 0);
    CHECK(ModavdCloneTidOutputsSafe(p | c, reinterpret_cast<long>(tid), reinterpret_cast<long>(tid)));
    modavd_original_syscall = &CaptureAl;
    const long extra = 0x123456789abcd;
    for (bool unsafe : {false, true}) {
      argument_count = unsafe ? 6 : 5;
      expect_protected = !unsafe;
      const long output = unsafe ? reinterpret_cast<long>(__builtin_thread_pointer())
                                 : reinterpret_cast<long>(tid);
      for (long flags : {p | SIGCHLD, c | clear | SIGCHLD, p | c | SIGCHLD}) {
        captured_al = 0;
        CHECK(InvokeWithAl(SYS_clone, flags, 0L, output, output, 0x5678L, extra) == fake_result);
        CHECK((!unsafe || captured_al == 7) && errno == fake_errno && arguments[0] == flags &&
              arguments[1] == 0 && arguments[2] == output && arguments[3] == output &&
              arguments[4] == 0x5678 && (!unsafe || arguments[5] == extra));
      }
    }
    expect_protected = false;
    argument_count = 5;
    modavd_original_syscall = &Capture;
    munmap(region, page * 3);
  }
}

std::atomic<int> worker_errors{0};
void* CheckWorkerTls(void*) {
  HostRange tp, thread;
  int stack_tid = 0;
  if (!FindHostThreadRanges(tp, thread) ||
      ModavdCloneTidOutputsSafe(CLONE_PARENT_SETTID,
                               reinterpret_cast<long>(__builtin_thread_pointer()), 0) ||
      ModavdCloneTidOutputsSafe(CLONE_CHILD_SETTID, 0, static_cast<long>(pthread_self())) ||
      ModavdCloneTidOutputsSafe(CLONE_CHILD_SETTID, 0, reinterpret_cast<long>(&stack_tid)))
    worker_errors.fetch_add(1);
  return nullptr;
}

void* RememberOriginalWorker(void*) {
  for (int i = 0; i < 1000; ++i) {
    if (!RememberSyscallOriginal(&Capture) || RememberSyscallOriginal(&ModavdSyscallWrapper))
      worker_errors.fetch_add(1);
  }
  return nullptr;
}

void CheckInstallerAndDiagnostics() {
  modavd_original_syscall = nullptr;
  CHECK(!RememberSyscallOriginal(nullptr) && !RememberSyscallOriginal(&ModavdSyscallWrapper));
  pthread_t threads[4];
  for (auto& thread : threads) CHECK(pthread_create(&thread, nullptr, &RememberOriginalWorker, nullptr) == 0);
  for (auto& thread : threads) CHECK(pthread_join(thread, nullptr) == 0);
  CHECK(worker_errors.load() == 0 && modavd_original_syscall.load() == &Capture);
  CHECK(!RememberSyscallOriginal(&syscall) && modavd_original_syscall.load() == &Capture);
  const bool installed = g_ndk_hooks_installed.load();
  g_ndk_hooks_installed.store(false);
  g_ndk_hooks_install_owner.store(gettid());
  CHECK(InstallTranslatorHooksLocked(nullptr, 0, nullptr) == 1);
  CHECK(!g_ndk_hooks_installed.load() && g_ndk_hooks_install_owner.load() == gettid());
  g_ndk_hooks_install_owner.store(0);
  g_detached.store(true);
  CHECK(!InstallTranslatorHooks());
  g_detached.store(false);
  g_ndk_hooks_installed.store(installed);
  g_fork_diagnostics.store(false);
  CHECK(!ShouldLogRawClone(true, true, 15000000, 35000000));
  CHECK(!ShouldLogRawClone(false, false, 0, 0));
  CHECK(ShouldLogRawClone(true, false, 0, 0));
  CHECK(ShouldLogRawClone(true, true, 60000000, 40000000));
  g_fork_diagnostics.store(true);
  CHECK(ShouldLogRawClone(true, true, 0, 0) && ShouldLogRawClone(false, false, 0, 0));
  g_fork_diagnostics.store(false);
}

void CheckHostAtforkParent() {
  g_fork_diagnostics.store(true);
  ForkPrepare();
  CHECK(g_fork_held.load());
  errno = E2BIG;
  ForkParentAtFork();
  CHECK(errno == E2BIG && !g_fork_held.load() && g_fork_owner.load() == 0);
  for (auto& mutex : locks) {
    CHECK(pthread_mutex_trylock(&mutex) == 0);
    pthread_mutex_unlock(&mutex);
  }
  errno = ERANGE;
  ForkParentAtFork();
  CHECK(errno == ERANGE && !g_fork_held.load());
  g_fork_diagnostics.store(false);
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

void CheckRealTidOutputs() {
  const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  void* region = mmap(nullptr, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(region != MAP_FAILED);
  if (region == MAP_FAILED) return;
  auto* output = reinterpret_cast<int*>(static_cast<char*>(region) + page);
  CHECK(mprotect(output, page, PROT_READ | PROT_WRITE) == 0);
  modavd_original_syscall = &syscall;
  for (long flags : {static_cast<long>(CLONE_PARENT_SETTID | SIGCHLD),
                    static_cast<long>(CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | SIGCHLD),
                    static_cast<long>(CLONE_PARENT_SETTID | CLONE_CHILD_SETTID |
                                      CLONE_CHILD_CLEARTID | SIGCHLD)}) {
    for (bool alias : {false, true}) {
      output[0] = output[1] = -7;
      int* ctid = alias ? output : output + 1;
      CHECK(ModavdCloneTidOutputsSafe(flags, reinterpret_cast<long>(output),
                                    reinterpret_cast<long>(ctid)));
      const long child = ModavdSyscallWrapper(SYS_clone, flags, 0L, output, ctid, 0L);
      if (child == 0) {
        const bool written = !(flags & CLONE_CHILD_SETTID) ||
                             *ctid == static_cast<int>(syscall(SYS_gettid));
        syscall(SYS_exit, written && ChildCanResumeTranslation() ? 37 : 38);
        __builtin_unreachable();
      }
      if (flags & CLONE_PARENT_SETTID) CHECK(output[0] == child);
      else CHECK(output[0] == -7);
      if (!alias) CHECK(output[1] == -7);  // Child writes stay in its COW copy.
      CheckParentAfterChild(child);
    }
  }
  munmap(region, page * 3);
  modavd_original_syscall = &Capture;
}

std::atomic<bool> holder_ready{false}, holder_release{false};
std::atomic<pid_t> holder_tid{0};
void* HoldPreparedFork(void*) {
  ForkPrepare();
  holder_tid.store(gettid());
  holder_ready.store(true, std::memory_order_release);
  while (!holder_release.load(std::memory_order_acquire)) sched_yield();
  ForkParent();
  return nullptr;
}

void CheckFallbackInheritedLocks() {
  ResetTestTranslations();
  holder_ready.store(false);
  holder_release.store(false);
  pthread_t holder;
  CHECK(pthread_create(&holder, nullptr, &HoldPreparedFork, nullptr) == 0);
  while (!holder_ready.load(std::memory_order_acquire)) sched_yield();
  CHECK(g_fork_held.load() && g_fork_owner.load() == holder_tid.load());
  modavd_original_syscall = &syscall;
  const int misses = g_fork_misses.load();
  const long child = ModavdSyscallWrapper(SYS_clone, static_cast<long>(SIGCHLD), 0L, 0L, 0L, 0L);
  if (child == 0) {
    syscall(SYS_exit, ChildCanResumeTranslation() ? 37 : 38);
    __builtin_unreachable();
  }
  CHECK(child > 0 && g_fork_misses.load() == misses + 1);
  int status = 0;
  CHECK(waitpid(static_cast<pid_t>(child), &status, 0) == child);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 37);
  // The fallback child releases inherited metadata; its parent cannot release
  // the other thread's live prepare or erase that owner's state.
  CHECK(g_fork_held.load() && g_fork_owner.load() == holder_tid.load());
  holder_release.store(true, std::memory_order_release);
  CHECK(pthread_join(holder, nullptr) == 0);
  CHECK(!g_fork_held.load() && g_fork_owner.load() == 0);
  CheckUnlocked();
  modavd_original_syscall = &Capture;
}

void* ChurnForkOwner(void*) {
  for (int i = 0; i < 500; ++i) {
    const long deadline = MonoNs() + 2000000000L;
    ForkAttempt result;
    do {
      result = TryForkLocks();
      if (result != ForkAttempt::kReady) sched_yield();
    } while (result != ForkAttempt::kReady && MonoNs() < deadline);
    if (result != ForkAttempt::kReady) { worker_errors.fetch_add(1); return nullptr; }
    if (!g_fork_held.load() || g_fork_owner.load() != gettid()) worker_errors.fetch_add(1);
    ForkRelease();
  }
  return nullptr;
}

void* RaceUnknown(void*) {
  for (int i = 0; i < 10000; ++i) {
    const ForkAttempt result = TryForkLocks();
    if (result == ForkAttempt::kUnknown || result == ForkAttempt::kDisabled) return nullptr;
    if (result == ForkAttempt::kReady) { worker_errors.fetch_add(1); ForkRelease(); return nullptr; }
    sched_yield();
  }
  worker_errors.fetch_add(1);
  return nullptr;
}

void CheckUnknownAndOwnerConcurrency() {
  pthread_t threads[2];
  ResetTestTranslations();
  for (auto& thread : threads) CHECK(pthread_create(&thread, nullptr, &ChurnForkOwner, nullptr) == 0);
  for (auto& thread : threads) CHECK(pthread_join(thread, nullptr) == 0);
  CHECK(worker_errors.load() == 0 && !g_fork_held.load() && g_fork_owner.load() == 0);
  translation_cache.size = 2;
  expect_protected = false;
  fake_result = 777;
  g_fork_unknown_logged.store(false);
  const int unknown = g_fork_unknown_scans.load();
  const long begin = MonoNs();
  CHECK(ModavdSyscallWrapper(SYS_clone, static_cast<long>(SIGCHLD), 0L, 0L, 0L, 0L) == 777);
  CHECK(errno == fake_errno && MonoNs() - begin < kForkLockWaitNs);
  CHECK(!g_fork_safety.load() && g_fork_unknown_scans.load() == unknown + 1 &&
        g_fork_unknown_logged.load());
  CHECK(ModavdSyscallWrapper(SYS_clone, static_cast<long>(SIGCHLD), 0L, 0L, 0L, 0L) == 777);
  CHECK(g_fork_unknown_scans.load() == unknown + 1);
  g_fork_safety.store(true);  // Explicit test reset; production never re-enables.
  for (auto& thread : threads) CHECK(pthread_create(&thread, nullptr, &RaceUnknown, nullptr) == 0);
  for (auto& thread : threads) CHECK(pthread_join(thread, nullptr) == 0);
  CHECK(worker_errors.load() == 0 && g_fork_unknown_scans.load() == unknown + 2);
  CHECK(TryForkLocks() == ForkAttempt::kDisabled);
  ResetTestTranslations();
  g_fork_safety.store(true);
  const long sleeping = MonoNs();
  YieldForkAttempt(ForkAttempt::kCacheBusy);
  CHECK(MonoNs() - sleeping >= 900000);
  CheckUnlocked();
}

constexpr size_t kStressEntries = 370000;
std::atomic<uintptr_t>* stress_slots = nullptr;
int stress_calls = 0;
TestCacheNode* BuildStressTree(TestCacheNode* nodes, size_t first, size_t end, TestCacheNode* parent) {
  if (first == end) return nullptr;
  const size_t middle = first + (end - first) / 2;
  TestCacheNode* node = &nodes[middle];
  *node = {nullptr, nullptr, parent, 0, middle * 4, &stress_slots[middle]};
  node->left = BuildStressTree(nodes, first, middle, node);
  node->right = BuildStressTree(nodes, middle + 1, end, node);
  return node;
}

long CaptureStress(long nr, ...) {
  CHECK(nr == SYS_clone && g_fork_held.load() && g_fork_owner.load() == gettid());
  CacheScanStats stats;
  CHECK(ScanForkCache(stats) == CacheState::kQuiescent &&
        stats.entries == kStressEntries && stats.visited == kStressEntries);
  ++stress_calls;
  errno = fake_errno;
  return 777;
}

void* FinishLargeCacheTransition(void*) {
  usleep(10000);
  pthread_mutex_lock(&locks[kForkTranslationCacheLock]);
  stress_slots[kStressEntries - 1].store(static_cast<uintptr_t>(TranslationEntry::kReady));
  pthread_mutex_unlock(&locks[kForkTranslationCacheLock]);
  return nullptr;
}

void CheckLargeCacheBurst() {
  auto* nodes = new (std::nothrow) TestCacheNode[kStressEntries];
  stress_slots = new (std::nothrow) std::atomic<uintptr_t>[kStressEntries];
  CHECK(nodes != nullptr && stress_slots != nullptr);
  if (nodes == nullptr || stress_slots == nullptr) {
    delete[] nodes;
    delete[] stress_slots;
    return;
  }
  for (size_t i = 0; i < kStressEntries; ++i)
    stress_slots[i].store(static_cast<uintptr_t>(TranslationEntry::kReady));
  translation_cache.inflight = 0;
  translation_cache.size = kStressEntries;
  translation_cache.begin = &nodes[0];
  translation_cache.root = BuildStressTree(
      nodes, 0, kStressEntries, reinterpret_cast<TestCacheNode*>(&translation_cache.root));
  modavd_original_syscall = &CaptureStress;
  const int misses = g_fork_misses.load(), unknown = g_fork_unknown_scans.load();
  const long begin = MonoNs();
  for (int i = 0; i < 15; ++i) {
    // A last-node Invalidating with inflight==0 forces complete-tree work
    // before Busy. The worker needs the released cache mutex to finish.
    stress_slots[kStressEntries - 1].store(static_cast<uintptr_t>(TranslationEntry::kInvalidating));
    pthread_t worker;
    CHECK(pthread_create(&worker, nullptr, &FinishLargeCacheTransition, nullptr) == 0);
    CHECK(ModavdSyscallWrapper(SYS_clone, static_cast<long>(CLONE_VFORK | SIGCHLD),
                              0L, 0L, 0L, 0L) == 777);
    CHECK(errno == fake_errno && stress_calls == i + 1);
    CHECK(pthread_join(worker, nullptr) == 0);
    CHECK(!g_fork_held.load() && g_fork_owner.load() == 0);
  }
  CHECK(g_fork_misses.load() == misses && g_fork_unknown_scans.load() == unknown);
  std::printf("synthetic cache burst: entries=%zu calls=%d elapsed_ms=%ld (not game performance)\n",
              kStressEntries, stress_calls, (MonoNs() - begin) / 1000000);
  ResetTestTranslations();
  modavd_original_syscall = &Capture;
  delete[] nodes;
  delete[] stress_slots;
  stress_slots = nullptr;
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
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && RejectUnknownForkCache());
  CHECK(g_fork_unknown_scans.load() != 0);
  ResetTestTranslations();
  cache_nodes[0].left = &cache_nodes[1];  // A cycle must also fail closed.
  stats = {};
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && RejectUnknownForkCache());
  ResetTestTranslations();
  cache_nodes[0].slot = nullptr;
  stats = {};
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && RejectUnknownForkCache());
  ResetTestTranslations();
  translation_cache.begin = &cache_nodes[2];
  stats = {};
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && RejectUnknownForkCache());
  ResetTestTranslations();
  cache_nodes[1].left = cache_nodes[1].right;
  stats = {};
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && RejectUnknownForkCache());
  ResetTestTranslations();
  cache_nodes[1].parent = nullptr;
  stats = {};
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && RejectUnknownForkCache());
  ResetTestTranslations();
  cache_nodes[0].parent = &cache_nodes[2];
  stats = {};
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && RejectUnknownForkCache());
  ResetTestTranslations();
  cache_nodes[0].guest_pc = 2;
  stats = {};
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && RejectUnknownForkCache());
  ResetTestTranslations();
  cache_slots[0].store(0);
  stats = {};
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && RejectUnknownForkCache());
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
  CHECK(ScanForkCache(stats) == CacheState::kUnknown && RejectUnknownForkCache());
  ResetTestTranslations();
  g_fork_safety.store(true);
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
  CheckInstallerAndDiagnostics();
  CheckHostAtforkParent();
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
  // SETTLS/PIDFD always tail-jump; SETTID is covered separately below.
  // They must tail-jump
  // without entering the C++/loader path, retaining their original arguments.
  for (long flag : {static_cast<long>(CLONE_SETTLS), static_cast<long>(CLONE_PIDFD)}) {
    CHECK(ModavdSyscallWrapper(SYS_clone, flag | SIGCHLD, 0L, 11L, 22L, 33L) == fake_result);
    CHECK(errno == fake_errno && arguments[0] == (flag | SIGCHLD) && arguments[1] == 0 &&
          arguments[2] == 11 && arguments[3] == 22 && arguments[4] == 33);
  }
  CheckTidRanges();
  pthread_t tls_worker;
  CHECK(pthread_create(&tls_worker, nullptr, &CheckWorkerTls, nullptr) == 0);
  CHECK(pthread_join(tls_worker, nullptr) == 0 && worker_errors.load() == 0);
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
  CheckUnknownAndOwnerConcurrency();
  CheckFallbackInheritedLocks();
  CheckRealTidOutputs();
  CheckLargeCacheBurst();

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
  const int atfork_error = pthread_atfork(&ForkPrepare, &ForkParentAtFork, &ForkChild);
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
