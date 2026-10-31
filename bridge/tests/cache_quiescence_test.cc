/* Copyright 2026 jinghero. SPDX-License-Identifier: AGPL-3.0-or-later. */
// Metadata-only integration test against the exact Google API-34 translator.
// It never executes a synthetic guest PC or injects into another process.
#include <sys/wait.h>

#include <climits>

#include "../companion/jni/native_to_guest_companion.cc"
// Frozen ABI: the 16-byte aggregate is passed on the stack after the five
// integer arguments of SetTranslatedAndUnlock. Used only for its Invalidating
// completion branch, which ignores the empty code piece. Never used by
// production.
struct HostCodePiece {
  const void* code;
  uint64_t size;
};
static_assert(sizeof(HostCodePiece) == 16 && alignof(HostCodePiece) == 8);
using AddFn = void* (*)(void*, uintptr_t, unsigned);
using LookupFn = void* (*)(void*, uintptr_t);
using UnsafeFn = void (*)(void*);
using CompleteFn = void (*)(void*, uintptr_t, void*, unsigned, int,
                            HostCodePiece);
using WrapFnProbe = void* (*)(void*, uintptr_t);
CacheState snapshot(const char* label, uintptr_t slot = 0) {
  auto* mutex = static_cast<pthread_mutex_t*>(g_fork_translation_cache);
  pthread_mutex_lock(mutex);
  CacheScanStats stats;
  const long begin = MonoNs();
  const CacheState state = ScanForkCache(stats);
  const long elapsed = MonoNs() - begin;
  const uintptr_t code =
      slot ? __atomic_load_n(reinterpret_cast<const uintptr_t*>(slot),
                             __ATOMIC_RELAXED)
           : 0;
  const uintptr_t inflight =
      CacheWord(reinterpret_cast<uintptr_t>(g_fork_translation_cache) + 0x38);
  pthread_mutex_unlock(mutex);
  CacheScanStats attempt;
  const bool accepted = TryForkLocks(&attempt) == ForkAttempt::kReady;
  if (accepted) ForkRelease();
  if (accepted != (state == CacheState::kQuiescent)) {
    fprintf(stderr, "fork accepted a non-quiescent real cache in %s\n", label);
    syscall(SYS_exit_group, 90);
  }
  printf(
      "{\"phase\":\"%s\",\"state\":%d,\"entries\":%zu,\"visited\":%zu,"
      "\"scanNs\":%ld,\"inflight\":%lu,\"hostEntry\":\"0x%lx\"}\n",
      label, static_cast<int>(state), stats.entries, stats.visited, elapsed,
      inflight, code);
  fflush(stdout);
  return state;
}
void probe_timeout(int) { syscall(SYS_exit_group, 42); }
int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && strcmp(argv[1], "translate") != 0 &&
                   strcmp(argv[1], "wrap") != 0))
    return 1;
  void* handle =
      dlopen("/system/lib64/libndk_translation.so", RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    fprintf(stderr, "probe dlopen: %s\n", dlerror());
    return 2;
  }
  auto get = reinterpret_cast<TranslationCacheInstanceFn>(
      dlsym(handle, "_ZN15ndk_translation16TranslationCache11GetInstanceEv"));
  auto add = reinterpret_cast<AddFn>(dlsym(
      handle,
      "_ZN15ndk_translation16TranslationCache24AddAndLockForTranslationEmj"));
  auto lookup = reinterpret_cast<LookupFn>(dlsym(
      handle,
      "_ZN15ndk_translation16TranslationCache26LookupGuestCodeEntryUnsafeEm"));
  auto invalidate =
      reinterpret_cast<UnsafeFn>(dlsym(handle,
                                       "_ZN15ndk_"
                                       "translation16TranslationCache38Invalida"
                                       "teEntriesBeingTranslatedUnsafeEv"));
  auto complete = reinterpret_cast<CompleteFn>(dlsym(
      handle,
      "_ZN15ndk_translation16TranslationCache22SetTranslatedAndUnlockEmPNS_"
      "14GuestCodeEntryEjNS1_4KindENS_13HostCodePieceE"));
  auto wrap = reinterpret_cast<WrapFnProbe>(
      dlsym(handle,
            "_ZN15ndk_translation16TranslationCache21AddAndLockForWrappingEm"));
  if (!get || !add || !lookup || !invalidate || !complete || !wrap) return 3;
  Dl_info info{};
  dladdr(reinterpret_cast<void*>(get), &info);
  const uintptr_t base = reinterpret_cast<uintptr_t>(info.dli_fbase);
  if (!base || !BuildIdIs(base, kNdkForkLocksBuildId)) return 4;
  g_fork_translation_cache = get();
  // Only the live TranslationCache mutex is cross-checked in this neutral
  // process. The other two production offsets are not initialised here.
  pthread_mutex_t neutral_locks[2] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};
  g_fork_locks[0] = &neutral_locks[0];
  g_fork_locks[kForkTranslationCacheLock] = static_cast<pthread_mutex_t*>(g_fork_translation_cache);
  g_fork_locks[2] = &neutral_locks[1];
  g_fork_safety.store(true);
  if (g_fork_translation_cache !=
      reinterpret_cast<void*>(base +
                              kNdkForkLockOffs[kForkTranslationCacheLock]))
    return 5;
  const char* stub_names[] = {"ndk_translation_entry_Translating",
                              "ndk_translation_entry_Invalidating",
                              "ndk_translation_entry_Wrapping"};
  TranslatorDyn dyn;
  dl_iterate_phdr(&FindTranslatorDynCallback, &dyn);
  if (dyn.base != base ||
      LookupExport(dyn, "_ZN15ndk_translation16TranslationCache11GetInstanceEv") !=
          reinterpret_cast<uintptr_t>(get))
    return 23;
  for (int i = 0; i < 3; ++i) {
    g_fork_transient_entries[i] = LookupExport(dyn, stub_names[i]);
    if (!g_fork_transient_entries[i] ||
        g_fork_transient_entries[i] != reinterpret_cast<uintptr_t>(dlsym(handle, stub_names[i])))
      return 6;
  }
  printf("{\"loadedExactBuild\":true,\"mode\":\"%s\"}\n",
         argc > 1 ? argv[1] : "translate");
  fflush(stdout);
  if (snapshot("empty") != CacheState::kQuiescent) return 7;
  constexpr uintptr_t pc = 0x40001000;
  if (argc > 1 && strcmp(argv[1], "wrap") == 0) {
    void* entry = wrap(g_fork_translation_cache, pc);
    if (!entry) return 8;
    const uintptr_t slot = CacheWord(reinterpret_cast<uintptr_t>(entry));
    auto* mutex = static_cast<pthread_mutex_t*>(g_fork_translation_cache);
    pthread_mutex_lock(mutex);
    const uintptr_t inflight = CacheWord(reinterpret_cast<uintptr_t>(g_fork_translation_cache) + 0x38);
    pthread_mutex_unlock(mutex);
    const bool ok = inflight == 0 &&
        snapshot("wrapping", slot) == CacheState::kBusy &&
        __atomic_load_n(reinterpret_cast<const uintptr_t*>(slot),
                        __ATOMIC_RELAXED) == g_fork_transient_entries[2];
    syscall(SYS_exit_group, ok ? 0 : 9);
    __builtin_unreachable();
  }
  if (add(g_fork_translation_cache, pc, UINT_MAX) != nullptr) return 10;
  auto* mutex = static_cast<pthread_mutex_t*>(g_fork_translation_cache);
  pthread_mutex_lock(mutex);
  void* entry = lookup(g_fork_translation_cache, pc);
  pthread_mutex_unlock(mutex);
  if (!entry) return 11;
  const uintptr_t slot = CacheWord(reinterpret_cast<uintptr_t>(entry));
  if (snapshot("not_translated", slot) != CacheState::kQuiescent) return 12;
  if (add(g_fork_translation_cache, pc, 0) != entry ||
      snapshot("translating", slot) != CacheState::kBusy)
    return 13;
  const long child = syscall(SYS_clone, SIGCHLD, 0L, 0L, 0L, 0L);
  if (child == 0) {
    signal(SIGALRM, &probe_timeout);
    alarm(3);
    pthread_mutex_lock(mutex);
    invalidate(g_fork_translation_cache);
    pthread_mutex_unlock(mutex);
    if (snapshot("child_invalidating", slot) != CacheState::kBusy ||
        __atomic_load_n(reinterpret_cast<const uintptr_t*>(slot),
                        __ATOMIC_RELAXED) != g_fork_transient_entries[1])
      syscall(SYS_exit_group, 14);
    // This original API internally locks the cache and erases the entry.
    // Do not access entry again; the separately saved dispatch slot remains.
    complete(g_fork_translation_cache, pc, entry, 0, 0, HostCodePiece{});
    const bool ok = snapshot("child_completed", slot) == CacheState::kQuiescent;
    syscall(SYS_exit_group, ok ? 0 : 15);
    __builtin_unreachable();
  }
  if (child < 0) return 16;
  int status;
  if (waitpid(static_cast<pid_t>(child), &status, 0) != child ||
      !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return 17;
  if (snapshot("parent_still_translating", slot) != CacheState::kBusy ||
      __atomic_load_n(reinterpret_cast<const uintptr_t*>(slot),
                      __ATOMIC_RELAXED) != g_fork_transient_entries[0])
    return 18;
  pthread_mutex_lock(mutex);
  invalidate(g_fork_translation_cache);
  pthread_mutex_unlock(mutex);
  if (snapshot("parent_invalidating", slot) != CacheState::kBusy) return 19;
  complete(g_fork_translation_cache, pc, entry, 0, 0, HostCodePiece{});
  if (snapshot("parent_completed", slot) != CacheState::kQuiescent) return 20;
  for (uintptr_t i = 0; i < 9664; ++i)
    if (add(g_fork_translation_cache, 0x50000000 + i * 4, UINT_MAX) != nullptr)
      return 21;
  if (snapshot("ready_9664_entries") != CacheState::kQuiescent) return 22;
  syscall(SYS_exit_group, 0);
  __builtin_unreachable();
}
