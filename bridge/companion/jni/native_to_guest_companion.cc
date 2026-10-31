/*
 * ModAVD native->guest call bridge — COMPANION library (x86_64).
 * Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later.
 *
 * WHY A COMPANION (not a Berberis source rebuild)
 * -----------------------------------------------
 * The arm64->x86_64 translator in the emulator image (`libndk_translation.so`,
 * Berberis 16.0.0) is a CLOSED Google prebuilt — public AOSP Berberis is
 * riscv64-only, so we cannot rebuild the arm64 translator with a source patch
 * (see BUILD-STATUS.md). BUT that closed .so EXPORTS the machinery we need as
 * dynsyms (confirmed by readelf: WrapGuestFunctionImpl, RunGuestCall,
 * GuestMapShadow::GetInstance/IsExecutable, GuestCall::*, ScopedVirtualGuestCallFrame,
 * MakeTrampolineCallable, ...). So this companion — our own x86_64 lib, built
 * with the standard NDK — loads in the same process, dlsym()s those exported
 * symbols, and implements the native->guest call bridge from OUTSIDE, without
 * any Berberis source.
 *
 * It does NOT touch the game, libunisec, or any anti-tamper: it makes the game's
 * own hook SUCCEED (run with the right ABI and return), exactly what a mature
 * translator (Houdini on LDPlayer, verified by sibling session) does.
 *
 * ROUTE 1 (implemented here — lowest-risk): at a native->guest instruction-fetch
 * fault, redirect host RIP into a WrapGuestFunctionImpl trampoline and return;
 * the trampoline (the translator's OWN code) marshals SysV->AAPCS64 and runs the
 * arm64 hook via ScopedVirtualGuestCallFrame + RunGuestCall, then `ret`s to the
 * caller. Only primitive-typed exported symbols are used (GuestArgumentBuffer is
 * opaque) so we depend on NO internal C++ type layout. One generic integer
 * signature "lllllll" covers every libc slot libunisec hooks (all integer/ptr;
 * the mature translator marshals integers only — sibling finding).
 *
 * STATUS: NDK-buildable; the fault path and symbol use are designed against the
 * confirmed exports, but the end-to-end bridge is NOT yet validated on device.
 * Key unresolved items (see COMPANION-DESIGN.md): signal-handler ordering vs
 * Berberis' own handler, async-signal-safety of the first trampoline build, and
 * early-enough load into the game process. `UNVERIFIED` marks spots to confirm.
 */

#include <android/log.h>
#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <csignal>
#include <ucontext.h>
#include <atomic>
#include <elf.h>
#include <jni.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/system_properties.h>
#include <sched.h>
#include <time.h>
#include <sys/syscall.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ModAvdGate", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ModAvdGate", __VA_ARGS__)

namespace {

// berberis::GuestArgumentBuffer is only ever used as an opaque pointer in the
// exported function-pointer type, so a forward declaration is all we need.
struct GuestArgumentBuffer;

// Exported-symbol function types (from readelf + c++filt on the shipped .so):
//   berberis::WrapGuestFunctionImpl(unsigned long, char const*,
//       void (*)(unsigned long, berberis::GuestArgumentBuffer*), char const*)
//   berberis::RunGuestCall(unsigned long, berberis::GuestArgumentBuffer*)
//   berberis::GuestMapShadow::GetInstance()                       [static]
//   berberis::GuestMapShadow::IsExecutable(unsigned long, unsigned long) const
using RunGuestCallFn = void (*)(uintptr_t, GuestArgumentBuffer*);
using WrapFn = void* (*)(uintptr_t pc, const char* signature, RunGuestCallFn runner,
                         const char* name);
using ShadowInstanceFn = void* (*)();
// const member: `this` is the (hidden) first argument in the Itanium C++ ABI.
using IsExecutableFn = bool (*)(void* self, uintptr_t addr, uintptr_t size);

// Reverse path (guest -> host) exported symbols (from host_function_wrapper_impl.h
// + guest_state_opaque.h + get_cpu_state.cc in the Berberis source):
//   MakeTrampolineCallable(GuestAddr pc, bool is_host_func,
//       void (*trampoline)(HostCode, ThreadState*), HostCode func, const char* name)
//     — when the guest branches to `pc`, run `trampoline(func, state)` instead of
//       faulting; the trampoline reads guest args, calls func, writes the result
//       back to guest state; Berberis handles the return-to-guest (via x30).
//   GetCPUState(ThreadState&) -> CPUState&   (a reference == a pointer in the ABI)
//   GetCpuState(NativeBridgeGuestRegs*, const CPUState*) -> int  (fills regs; needs
//       guest_arch preset to NATIVE_BRIDGE_ARCH_ARM64, else returns an error)
//   SetReturnValueRegister(CPUState&, GuestAddr)  — writes x0
using MarshallFn = void (*)(const void* func, void* thread_state);
using MakeTrampFn = void (*)(uintptr_t pc, bool is_host_func, MarshallFn trampoline,
                             const void* func, const char* name);
using GetCpuStateRefFn = void* (*)(void* thread_state);  // returns CPUState*
using GetCpuRegsFn = int (*)(void* nb_guest_regs, const void* cpu_state);
using SetRetFn = void (*)(void* cpu_state, uint64_t val);

constexpr uint64_t kNativeBridgeArchArm64 = 2;  // accessor.h NATIVE_BRIDGE_ARCH_ARM64
// Layout-compatible with native_bridge_support's NativeBridgeGuestRegs (arm64 arm).
// GetCpuState memcpys the full x[31]/sp/ip/v[32], so we allocate the whole struct
// and read only guest_arch + regs_arm64.x[0..7] (the AAPCS64 integer arg registers).
struct NbGuestRegs {
  uint64_t guest_arch;
  struct {
    uint64_t x[31];
    uint64_t sp;
    uint64_t ip;
    alignas(16) unsigned __int128 v[32];
  } regs_arm64;
};

// Symbol OFFSETS within the shipped libndk_translation.so (Berberis 16.0.0,
// build-id c8d551fd…), from `readelf --dyn-syms` st_value. Why offsets, not
// dlsym: the app linker namespace cannot reach the bridge-namespace instance
// (RTLD_NOLOAD fails; a plain dlopen would load a SEPARATE copy with its own
// empty GuestMapShadow state — useless). So we resolve against the ALREADY-MAPPED
// real instance: base (from /proc/self/maps) + these offsets. BUILD-SPECIFIC —
// re-extract if the translator build changes. Mangled names (for re-extraction):
//   WrapGuestFunctionImpl       _ZN8berberis21WrapGuestFunctionImplEmPKcPFvmPNS_19GuestArgumentBufferEES1_
//   RunGuestCall                _ZN8berberis12RunGuestCallEmPNS_19GuestArgumentBufferE
//   GuestMapShadow::GetInstance _ZN8berberis14GuestMapShadow11GetInstanceEv
//   GuestMapShadow::IsExecutable _ZNK8berberis14GuestMapShadow12IsExecutableEmm
// NOTE: these offsets are for the DT_NEEDED-INJECTED libndk_translation.so — i.e. the
// copy patched (via LIEF) to add `NEEDED libmodavd_gate.so`, which shifted every
// st_value by +0x1000 vs the stock lib. Build-id is preserved (c8d551fd), so the gate
// passes for both; the companion must be deployed ALONGSIDE the patched libndk. (Stock
// offsets were 0x4f8730/0x278900/0x265f60/0x266270.)
constexpr uintptr_t kOffWrap = 0x4f9730;
constexpr uintptr_t kOffRunGuestCall = 0x279900;
constexpr uintptr_t kOffShadowInstance = 0x266f60;
constexpr uintptr_t kOffIsExecutable = 0x267270;
// Reverse path (mangled names for re-extraction):
//   MakeTrampolineCallable    _ZN8berberis22MakeTrampolineCallableEmbPFvPKvPNS_11ThreadStateEES1_PKc
//   GetCPUState(ThreadState&) _ZN8berberis11GetCPUStateERNS_11ThreadStateE
//   GetCpuState               _ZN8berberis11GetCpuStateEP21NativeBridgeGuestRegsPKNS_8CPUStateE
//   SetReturnValueRegister    _ZN8berberis22SetReturnValueRegisterERNS_8CPUStateEm
constexpr uintptr_t kOffMakeTramp = 0x4fa470;
constexpr uintptr_t kOffGetCpuRegs = 0x26bf20;
constexpr uintptr_t kOffGetCpuStateRef = 0x26be40;
constexpr uintptr_t kOffSetRet = 0x26c090;

// Base of the already-mapped /system/lib64/libndk_translation.so (its
// file-offset-0 mapping), from /proc/self/maps. 0 if not mapped yet. `needle`
// = "libndk_translation.so" (does not match the "..._proxy_..." libs).
uintptr_t FindMappedLibBase(const char* needle) {
  FILE* f = fopen("/proc/self/maps", "r");
  if (f == nullptr) return 0;
  char line[512];
  uintptr_t base = 0;
  while (fgets(line, sizeof(line), f) != nullptr) {
    if (strstr(line, needle) == nullptr) continue;
    unsigned long long start = 0, end = 0, off = 0;
    char perms[8] = {};
    if (sscanf(line, "%llx-%llx %7s %llx", &start, &end, perms, &off) == 4 && off == 0) {
      base = static_cast<uintptr_t>(start);
      break;
    }
  }
  fclose(f);
  return base;
}

// Expected libndk_translation.so GNU build-id (16 bytes, from `readelf -n`). The
// hardcoded symbol offsets are valid ONLY for this exact translator build; arming
// against a different build would send every offset to the wrong code and corrupt
// the translator's own TranslationCache/code-pool state. So we refuse to arm unless
// the mapped image's build-id matches.
constexpr unsigned char kExpectedBuildId[16] = {
    0xc8, 0xd5, 0x51, 0xfd, 0x7f, 0x87, 0xf2, 0xdc,
    0x08, 0x07, 0xae, 0x22, 0x2a, 0x71, 0x0c, 0x68,
};

// Confirm the ELF notes of the image mapped at `base` carry NT_GNU_BUILD_ID ==
// `expected`. Reads only mapped memory (the note is in the first r-- PT_LOAD).
bool BuildIdIs(uintptr_t base, const unsigned char (&expected)[16]) {
  const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(base);
  if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) return false;
  const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(base + ehdr->e_phoff);
  for (int i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_NOTE) continue;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(base + phdr[i].p_vaddr);
    const unsigned char* end = p + phdr[i].p_memsz;
    while (p + sizeof(Elf64_Nhdr) <= end) {
      const auto* nh = reinterpret_cast<const Elf64_Nhdr*>(p);
      const char* name = reinterpret_cast<const char*>(p + sizeof(Elf64_Nhdr));
      const unsigned char* desc =
          reinterpret_cast<const unsigned char*>(name + ((nh->n_namesz + 3u) & ~3u));
      if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 && memcmp(name, "GNU", 4) == 0) {
        return nh->n_descsz >= sizeof(expected) && memcmp(desc, expected, sizeof(expected)) == 0;
      }
      p = desc + ((nh->n_descsz + 3u) & ~3u);
    }
  }
  return false;
}

bool VerifyBuildId(uintptr_t base) { return BuildIdIs(base, kExpectedBuildId); }

// --- resolve the translator's exports BY NAME (preferred over the offsets) ----
// The hardcoded offsets above are exact but brittle: they are valid only for one
// translator build, and the LIEF DT_NEEDED patch already shifted every st_value by
// +0x1000 once. Resolving against the mapped image's own dynsym survives both. The
// shipped translator carries DT_GNU_HASH only (no DT_HASH), so we implement the GNU
// hash lookup; a miss falls back to the build-id-gated offsets.
struct TranslatorDyn {
  uintptr_t base = 0;
  const Elf64_Sym* symtab = nullptr;
  const char* strtab = nullptr;
  const uint32_t* gnu_hash = nullptr;
};

int FindTranslatorDynCallback(struct dl_phdr_info* info, size_t, void* opaque) {
  auto* out = static_cast<TranslatorDyn*>(opaque);
  const char* soname = info->dlpi_name;
  // "libndk_translation.so" does not match the "..._proxy_*.so" libraries.
  if (soname == nullptr || strstr(soname, "libndk_translation.so") == nullptr) return 0;
  const uintptr_t base = static_cast<uintptr_t>(info->dlpi_addr);
  const Elf64_Dyn* dynamic = nullptr;
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
      dynamic = reinterpret_cast<const Elf64_Dyn*>(base +
                                                   static_cast<uintptr_t>(info->dlpi_phdr[i].p_vaddr));
      break;
    }
  }
  if (dynamic == nullptr) return 0;
  out->base = base;
  for (const Elf64_Dyn* d = dynamic; d->d_tag != DT_NULL; ++d) {
    switch (d->d_tag) {
      case DT_SYMTAB:
        out->symtab = reinterpret_cast<const Elf64_Sym*>(base + d->d_un.d_ptr);
        break;
      case DT_STRTAB:
        out->strtab = reinterpret_cast<const char*>(base + d->d_un.d_ptr);
        break;
      case DT_GNU_HASH:
        out->gnu_hash = reinterpret_cast<const uint32_t*>(base + d->d_un.d_ptr);
        break;
      default:
        break;
    }
  }
  return 1;
}

uint32_t GnuHashOf(const char* name) {
  uint32_t h = 5381;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p != 0; ++p) {
    h = h * 33 + *p;
  }
  return h;
}

// Standard DT_GNU_HASH lookup: bloom filter, bucket, then the chain whose last
// entry has bit 0 set.
uintptr_t LookupExport(const TranslatorDyn& dyn, const char* name) {
  if (dyn.symtab == nullptr || dyn.strtab == nullptr || dyn.gnu_hash == nullptr) return 0;
  const uint32_t nbuckets = dyn.gnu_hash[0];
  const uint32_t symoffset = dyn.gnu_hash[1];
  const uint32_t bloom_size = dyn.gnu_hash[2];
  const uint32_t bloom_shift = dyn.gnu_hash[3];
  if (nbuckets == 0 || bloom_size == 0) return 0;
  const uint64_t* bloom = reinterpret_cast<const uint64_t*>(dyn.gnu_hash + 4);
  const uint32_t* buckets = reinterpret_cast<const uint32_t*>(bloom + bloom_size);
  const uint32_t* chain = buckets + nbuckets;

  const uint32_t h = GnuHashOf(name);
  const uint64_t word = bloom[(h / 64) % bloom_size];
  const uint64_t mask = (1ULL << (h % 64)) | (1ULL << ((h >> bloom_shift) % 64));
  if ((word & mask) != mask) return 0;

  uint32_t idx = buckets[h % nbuckets];
  if (idx < symoffset) return 0;
  for (;;) {
    const uint32_t hh = chain[idx - symoffset];
    if (((hh | 1u) == (h | 1u)) &&
        strcmp(dyn.strtab + dyn.symtab[idx].st_name, name) == 0) {
      return dyn.symtab[idx].st_value != 0 ? dyn.base + dyn.symtab[idx].st_value : 0;
    }
    if ((hh & 1u) != 0) break;
    ++idx;
  }
  return 0;
}

// Read this process's name (argv[0] from /proc/self/cmdline): the package name for an
// app's main process, or "zygote64"/"zygote" before a zygote fork specializes. Empty on
// error. Used to (a) never arm in the zygote itself (fork-COW would spread into every
// app) and (b) arm ONLY in the configured target package when loaded process-wide via
// zygote preload.
void GetProcName(char* out, size_t cap) {
  if (cap == 0) return;
  out[0] = '\0';
  FILE* f = fopen("/proc/self/cmdline", "r");
  if (f == nullptr) return;
  size_t n = fread(out, 1, cap - 1, f);
  fclose(f);
  out[(n < cap) ? n : (cap - 1)] = '\0';
}

// One generic integer signature: int64 return + 6 int64/pointer args, all in the
// SysV integer registers rdi,rsi,rdx,rcx,r8,r9 -> AAPCS64 x0..x5. Covers every
// libc function libunisec hooks (read/__read_chk/pread/close/fstat/lseek/open/
// mmap — all integer/pointer). See COMPANION-DESIGN.md.
constexpr char kGenericIntegerSignature[] = "lllllll";

WrapFn g_wrap = nullptr;
void* g_run_guest_call = nullptr;  // address of berberis::RunGuestCall
ShadowInstanceFn g_shadow_instance = nullptr;
IsExecutableFn g_is_executable = nullptr;
void* g_shadow = nullptr;  // cached GuestMapShadow* instance
MakeTrampFn g_make_tramp = nullptr;
GetCpuStateRefFn g_get_cpu_state_ref = nullptr;
GetCpuRegsFn g_get_cpu_regs = nullptr;
SetRetFn g_set_ret = nullptr;
struct sigaction g_old_segv = {};
std::atomic<bool> g_ready{false};
// Chain-order probe (prop `modavd.gate.probe=1`): on the native->guest fault,
// log that we were reached and CHAIN (do not bridge), so the libsigchain order
// vs Berberis' own fatal handler can be measured safely before any bridging.
std::atomic<bool> g_probe{false};
// Reverse-branch probe (prop `modavd.gate.revprobe=1`, independent of the forward
// probe): when a guest hook calls back into a saved ORIGINAL host function, the
// guest branches to a host address Berberis cannot execute as guest code -> it
// self-sends SIGSEGV with si_code=SEGV_ACCERR and si_addr=the HOST target (rip is
// inside libndk's syscall, so si_addr != rip). We log it (lib!sym via dladdr) and
// chain. This confirms WHETHER the hook calls back at all (vs issuing svc直接),
// which decides whether the reverse host-function pre-registration is needed.
std::atomic<bool> g_revprobe{false};
// `modavd.gate.slotstrict=1`: make the watched-GOT-slot condition BINDING in the
// discriminator. Default off = advisory (logged, not enforced) so that turning the
// check on can never silently kill a working bridge before it has been confirmed
// to match on this image.
std::atomic<bool> g_slotstrict{false};

// ARMING TRIGGER — why we patch the TRANSLATOR's own import GOT
// -------------------------------------------------------------
// The companion is a DT_NEEDED dependency of our own libndk_translation.so, so
// it is mapped (and its constructor runs) in ZYGOTE, long before any app is
// specialized. A child-atfork thread is both too early and unsafe (a thread
// during specialization makes selinux_android_setcontext() abort -> bootloop),
// so the trigger has to be an ordinary, synchronous, thread-free callback that
// fires inside the already-specialized app process.
//
// The first attempt patched the `android_dlopen_ext` GOT slot of
// libnativeloader.so. DEVICE EVIDENCE says that never fires for an arm64-primary
// app: its classloader namespace is BRIDGED, so libnativeloader routes loads
// through NativeBridgeLoadLibraryExt into the translator's own loader
// (logcat: "nativeloader: Load .../lib/arm64/libisabridge_guest.so ... ok" and
// "berberis: '<app>/libisabridge_caller.so' library was loaded for the host
// platform", with ZERO companion lines in the app process).
//
// So we patch the GOT of libndk_translation.so ITSELF. llvm-readelf confirms the
// shipped translator imports, among others: sigaction, mprotect,
// android_dlopen_ext, dlopen, dlsym, prctl, pthread_create. Three of those give
// us a trigger, installed together because each covers the others' blind spots:
//
//   mprotect   — THE one that actually fires, and it cannot miss: every guest
//                mprotect (the guest linker protecting freshly loaded segments,
//                and later libunisec flipping libartbase's RELRO) is serviced by
//                the translator calling the HOST mprotect through this slot. The
//                only high-frequency caller of that slot is MprotectForGuest, the
//                guest mprotect syscall implementation — so it fires while the
//                guest linker is still loading, long before libunisec, and
//                structurally before any rewritten slot can be called.
//   sigaction  — in an app process this does NOT fire: device evidence shows the
//                translator installs its host handlers (sig 11/4/8/6) in ZYGOTE,
//                once, and every app inherits them through COW. Kept as an
//                observer, and so that ordering is deterministic on any image
//                that ever moves that init post-fork.
//   android_dlopen_ext / dlopen — the translator's host-platform library load
//                path; a second backstop and a useful trace of load order.
//
// All wrappers are inherited by forked children through COW, and all are gated on
// the target package, so a non-target process only pays one relaxed atomic load.
using AndroidDlopenExtFn = void* (*)(const char* filename, int flags,
                                      const void* extinfo);
using DlopenFn = void* (*)(const char* filename, int flags);
using SigactionFn = int (*)(int, const struct sigaction*, struct sigaction*);
using MprotectFn = int (*)(void*, size_t, int);

AndroidDlopenExtFn g_real_ndk_android_dlopen_ext = nullptr;  // libndk slot
DlopenFn g_real_ndk_dlopen = nullptr;
SigactionFn g_real_ndk_sigaction = nullptr;
MprotectFn g_real_ndk_mprotect = nullptr;
std::atomic<bool> g_ndk_hooks_installed{false};
thread_local bool g_in_trigger = false;
// `modavd.gate.logsig=1`: trace every trigger call (sigaction / mprotect / dlopen)
// seen through the translator's GOT, in EVERY process, with the caller module. This
// is the one-run diagnostic that says which trigger fires and when.
std::atomic<bool> g_logtrace{false};
// The handler Berberis installed for SIGSEGV in this process, captured from the
// `oldact` of our own install. Kept separately from g_old_segv only for logging.
std::atomic<void*> g_berberis_segv{nullptr};

// Defined below; the GOT wrapper needs to call it before the real loader call.
extern "C" int modavd_gate_init();

// --- lock-free-ish trampoline cache (guest_pc -> host trampoline) ---
struct TrampEntry {
  std::atomic<uintptr_t> pc;  // 0 = not yet published; published LAST
  void* tramp;
};
constexpr int kMaxTramps = 64;
TrampEntry g_tramps[kMaxTramps];
std::atomic<int> g_tramp_count{0};

// Publish order matters: a nested or concurrent fault must never observe a
// half-written entry. The writer RESERVES a slot with fetch_add, fills `tramp`
// first, then release-stores `pc`; the reader acquire-loads `pc` and only then
// trusts `tramp`. (The previous order — pc, tramp, then a release store of a
// separate count — let a reader see {pcB, trampA} and jump into the wrong guest
// hook's trampoline, silently executing the wrong hook.)
void* LookupTramp(uintptr_t pc) {
  const int n = g_tramp_count.load(std::memory_order_acquire);
  const int lim = n < kMaxTramps ? n : kMaxTramps;
  for (int i = 0; i < lim; ++i) {
    if (g_tramps[i].pc.load(std::memory_order_acquire) == pc) return g_tramps[i].tramp;
  }
  return nullptr;
}

// NOTE(async-signal-safety): WrapGuestFunctionImpl allocates/locks, so the FIRST
// build for a given pc is NOT async-signal-safe (bounded: <= #distinct hooks).
// Production hardening = a deferred builder stub that runs post-sigreturn. See
// COMPANION-DESIGN.md risk #2.
void* GetOrBuildTramp(uintptr_t pc, const char* sym, const char* via) {
  if (void* t = LookupTramp(pc)) return t;
  void* t = g_wrap(pc, kGenericIntegerSignature,
                   reinterpret_cast<RunGuestCallFn>(g_run_guest_call), "modavd_gate");
  // Fires once per distinct guest pc (cache hits skip this): positive evidence
  // the bridge engaged on a real native->guest fault. `sym` names WHICH libc slot
  // was rewritten — that is how we notice if a slot needing a typed shim
  // (open/openat: divergent O_* values; fstat: divergent struct stat layout) has
  // started being hooked, which would corrupt semantics silently rather than crash.
  // `via` names which of our two entry points took the fault (see
  // modavd_gate_segv_first_chance); scripts match only the "bridged: guest pc=" prefix.
  LOGI("bridged: guest pc=%lx sym=%s -> tramp=%p via=%s", (unsigned long)pc,
       sym != nullptr ? sym : "?", t, via);
  if (sym != nullptr && (strcmp(sym, "open") == 0 || strcmp(sym, "openat") == 0 ||
                         strncmp(sym, "fstat", 5) == 0 || strncmp(sym, "__fxstat", 8) == 0)) {
    LOGE("bridged slot '%s' needs a TYPED shim (O_* values / struct stat layout differ "
         "between arm64 and x86_64); the generic integer marshaller will corrupt it",
         sym);
  }
  if (t != nullptr) {
    const int slot = g_tramp_count.fetch_add(1, std::memory_order_acq_rel);
    if (slot < kMaxTramps) {
      g_tramps[slot].tramp = t;                                      // fill first
      g_tramps[slot].pc.store(pc, std::memory_order_release);        // publish last
    } else {
      g_tramp_count.fetch_sub(1, std::memory_order_relaxed);         // table full
    }
  }
  return t;
}

void ChainToPrevious(int sig, siginfo_t* info, void* uc) {
  if (g_old_segv.sa_flags & SA_SIGINFO) {
    if (g_old_segv.sa_sigaction) {
      g_old_segv.sa_sigaction(sig, info, uc);
      return;
    }
  } else if (g_old_segv.sa_handler == SIG_IGN) {
    return;
  } else if (g_old_segv.sa_handler != SIG_DFL && g_old_segv.sa_handler != nullptr) {
    g_old_segv.sa_handler(sig);
    return;
  }
  // Nothing valid to chain to. DO NOT call sigaction(sig, SIG_DFL) here: on a
  // signal libsigchain has claimed, that only writes its stored `action_` and
  // returns 0 WITHOUT touching the kernel — which permanently discards the whole
  // saved chain, the guest's own claimed handler included, so every later SEGV in
  // the process would bypass the game's handler. That would be real signal
  // starvation caused by us. Returning is not an option either: the faulting
  // instruction would re-execute, re-enter us, and spin forever.
  // abort() is async-signal-safe, leaves the chain untouched, and still produces a
  // tombstone whose backtrace names this frame — which is exactly the diagnosis we
  // want, because reaching here at all means our own state is broken.
  abort();
}

// --- watched-GOT-slot table (the discriminator's second, decisive condition) --
// `si_addr == rip && IsExecutable(rip)` alone is NOT sufficient. GuestMapShadow is
// a flat bitmap shared by host and guest: ToHostProt clears the host X bit, and any
// guest mprotect carrying PROT_EXEC marks the range guest-executable, while
// ProtectMappingsFromGuest only shields mappings whose name contains "libc.so". So a
// guest mprotect(PROT_EXEC) over a HOST library's text makes host x86_64 bytes look
// like a guest entry point — and we would feed them to RunGuestCall as arm64.
//
// The in-tree patch (src/guest_os_primitives/native_to_guest_call_bridge.cc:290-305)
// adds the condition that closes this: some WATCHED libc GOT slot must CURRENTLY
// HOLD this pc. Slot addresses are static (only their values get rewritten), so the
// table is built once at arm time by a read-only .rela.plt scan — no runtime trigger
// needed. The fault-time check is a handful of plain loads: no locks, no allocation,
// async-signal-safe.
struct HookedSlot {
  uintptr_t* addr;
  const char* sym;
};
// An app process maps on the order of 150 shared objects and many of them import
// several of the watched symbols, so this has to be generous: measured, 512 was
// already saturated before the scan reached the module that mattered, which would
// have made a STRICT discriminator reject a perfectly good bridge. 4096 entries is
// 64 KiB and the per-fault cost is still just a few thousand pointer compares on a
// path that runs ~17 times per launch.
constexpr int kMaxSlots = 4096;
HookedSlot g_slots[kMaxSlots];
std::atomic<int> g_slot_count{0};

// Returns the symbol name whose GOT slot currently holds `pc`, else nullptr.
const char* MatchHookedSlot(uintptr_t pc) {
  const int n = g_slot_count.load(std::memory_order_acquire);
  const int lim = n < kMaxSlots ? n : kMaxSlots;
  for (int i = 0; i < lim; ++i) {
    if (*reinterpret_cast<volatile uintptr_t*>(g_slots[i].addr) == pc) return g_slots[i].sym;
  }
  return nullptr;
}

// Defined after the ELF helpers; fills g_slots. Returns the number found.
int BuildHookedSlotTable();

// Unregistered reverse-branch accounting (diagnostic only; see SegvHandler).
struct RevEntry {
  uintptr_t addr;
  int n;
};
constexpr int kMaxRev = 16;
RevEntry g_rev[kMaxRev];
std::atomic<int> g_rev_count{0};

// Same-thread reentry detection WITHOUT thread-local storage.
//
// Once we faithfully preserve a guest-requested SA_NODEFER (see
// ReassertSegvHandler), our handler can be entered again from inside itself — and
// the bridging path takes the translator's WrapperCache / CodePool mutexes, so a
// nested entry on the same thread would self-deadlock.
//
// The obvious implementation is a __thread counter, but we must NOT use one:
// this library is pulled in as a DT_NEEDED of a translator that libnativebridge
// dlopen()s, and the model that would make TLS safe in signal context
// (initial-exec) is rejected by the linker in a dlopened library — the whole
// native bridge then fails to load and the device loses arm64 support entirely.
// (Measured: "TLS symbol (null) in dlopened libmodavd_gate.so".) So we key off the
// tid instead: a handful of atomics, no allocation, no __tls_get_addr.
constexpr int kMaxInFlight = 8;
std::atomic<pid_t> g_in_handler[kMaxInFlight];

// Returns -1 if this thread is already inside the handler (caller must not take
// the bridging path), else the claimed slot index, or -2 when the table is full
// (proceed, but nothing to release — losing the guard is better than losing the
// bridge, and 8 concurrent faulting threads is already pathological).
int EnterHandler(pid_t tid) {
  for (int i = 0; i < kMaxInFlight; ++i) {
    if (g_in_handler[i].load(std::memory_order_acquire) == tid) return -1;
  }
  for (int i = 0; i < kMaxInFlight; ++i) {
    pid_t expected = 0;
    if (g_in_handler[i].compare_exchange_strong(expected, tid, std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
      return i;
    }
  }
  return -2;
}

void LeaveHandler(int slot) {
  if (slot >= 0) g_in_handler[slot].store(0, std::memory_order_release);
}

struct HandlerSlotGuard {
  int slot;
  explicit HandlerSlotGuard(int s) : slot(s) {}
  ~HandlerSlotGuard() { LeaveHandler(slot); }
};

// The bridging half of the SIGSEGV path, shared by both ways a fault reaches us: the
// regular sigaction() handler (SegvHandler) and the first-chance handler ART registers
// through the native bridge (modavd_gate_segv_first_chance). Returns true only when
// the fault was our native->guest branch and RIP now points at its trampoline — the
// caller must then return straight to the kernel. Anything else returns false and
// leaves the context untouched.
bool TryBridgeNativeToGuest(int sig, siginfo_t* info, ucontext_t* uc, const char* via) {
#if defined(__x86_64__)
  const uintptr_t rip = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
  if (g_logtrace.load(std::memory_order_relaxed)) {
    // Diagnostic only (not async-signal-safe; enabled by modavd.gate.logsig=1).
    LOGI("segv: sig=%d code=%d si_addr=%p rip=%p ready=%d", sig,
         info != nullptr ? info->si_code : -1,
         info != nullptr ? info->si_addr : nullptr, reinterpret_cast<void*>(rip),
         static_cast<int>(g_ready.load(std::memory_order_relaxed)));
  }
  // Discriminator. An instruction-FETCH fault (si_addr == RIP) with a permission
  // error (SEGV_ACCERR) into a page the translator marks guest-executable, AND a
  // watched libc GOT slot that currently holds exactly this pc = a native caller
  // branched into guest code through a rewritten slot (the libunisec pattern).
  // The slot condition is what rejects the false positive described above; when
  // the table could not be built we fall back to the looser test rather than
  // losing the bridge entirely, and say so at arm time.
  // REG_CSGSFS's low 16 bits are %cs: 0x33 = 64-bit user code. Berberis hands our
  // handler's bare address to the guest as a "previous disposition", so libunisec
  // could invoke us with a guest-built siginfo and an arm64 ucontext; this check
  // refuses to treat such a frame as a host fault.
  // OBSERVE BEFORE ENFORCING. The slot condition can only ever make us bridge
  // FEWER faults, so switching it on blind would silently kill a bridge that is
  // currently working if the scan happens to miss the one slot that matters. So it
  // is advisory by default — we log what it WOULD have decided — and becomes
  // binding only once `modavd.gate.slotstrict=1` says the match has been confirmed
  // on this image.
  // Establish the cheap, allocation-free part of the signature FIRST. Everything
  // below — including walking the slot table, which dereferences addresses inside
  // other modules — must stay off the path of faults that are none of our business
  // (the game's own anti-dump probes deliberately fault a lot).
  const bool host_frame =
      (static_cast<unsigned>(uc->uc_mcontext.gregs[REG_CSGSFS]) & 0xffffu) == 0x33u;
  const bool fetch_fault = sig == SIGSEGV && info != nullptr &&
                           info->si_code == SEGV_ACCERR &&
                           reinterpret_cast<uintptr_t>(info->si_addr) == rip && host_frame;
  const int nslots = fetch_fault ? g_slot_count.load(std::memory_order_acquire) : 0;
  const char* hooked_sym = (fetch_fault && nslots > 0) ? MatchHookedSlot(rip) : nullptr;
  const bool slot_ok =
      !g_slotstrict.load(std::memory_order_relaxed) || nslots == 0 || hooked_sym != nullptr;
  if (g_ready.load(std::memory_order_acquire) && fetch_fault && slot_ok &&
      g_is_executable(g_shadow, rip, 1)) {
    if (g_probe.load(std::memory_order_acquire)) {
      // Chain-order probe: prove we are reached on the native->guest fault
      // (before Berberis' fatal handler) WITHOUT bridging. Log and fall through
      // to the previous handler; it may terminate the process — that is fine,
      // our line in logcat proves we ran first.
      LOGI("PROBE: reached on native->guest fault pc=%lx; chaining (not bridging)",
           (unsigned long)rip);
    } else {
      // Reentry guard scoped to EXACTLY the region that takes the translator's
      // non-recursive WrapperCache / CodePool locks. It must not span
      // ChainToPrevious: a predecessor is free to siglongjmp out of the handler
      // (that is a normal anti-dump idiom, and we now faithfully preserve the
      // SA_NODEFER that makes it work), which would skip the destructor, leak the
      // tid slot, and silently disable bridging on that thread forever.
      const int hslot = EnterHandler(gettid());
      if (hslot != -1) {
        HandlerSlotGuard slot_guard(hslot);
        if (hooked_sym == nullptr && nslots > 0 && LookupTramp(rip) == nullptr) {
          // The decisive datum for turning on modavd.gate.slotstrict: the table
          // was built, yet no watched slot holds this pc. Enforcing now would have
          // REFUSED this (working) bridge, so the scan is missing the slot that
          // matters. Logged once per distinct pc (only before the tramp is cached).
          LOGE("slot check: no watched GOT slot holds pc=%lx although the table has %d "
               "entries — do NOT set modavd.gate.slotstrict=1 until this is explained",
               (unsigned long)rip, nslots);
        }
        void* tramp = GetOrBuildTramp(rip, hooked_sym, via);
        if (tramp != nullptr) {
          // Host is already in "just called" state (SysV args in registers, return
          // address on the stack). Resume into the trampoline; it marshals, runs
          // the guest hook, and `ret`s to the original caller with result in rax.
          // The trampoline runs AFTER sigreturn, in normal context, so releasing
          // the slot here is correct — it must be free again for a fault the
          // trampoline itself provokes.
          uc->uc_mcontext.gregs[REG_RIP] =
              static_cast<greg_t>(reinterpret_cast<uintptr_t>(tramp));
          return true;
        }
      }
      // Nested on this thread, or no trampoline: not bridged; the caller chains.
    }
  }
#else
  (void)sig;
  (void)info;
  (void)uc;
  (void)via;
#endif
  return false;
}

void SegvHandler(int sig, siginfo_t* info, void* ucv) {
  auto* uc = static_cast<ucontext_t*>(ucv);
  if (TryBridgeNativeToGuest(sig, info, uc, "sigaction")) return;
#if defined(__x86_64__)
  const uintptr_t rip = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
  // Reverse branch: guest -> host (si_addr != rip, si_addr not a guest-executable
  // page). This is the guest branching to a HOST address Berberis has no wrapper
  // for. Its untreated failure mode is the nastiest one we can produce:
  // berberis_HandleNoExec re-queues SEGV_ACCERR and the dispatch loop re-reads the
  // same insn_addr, so the process either spins forever or — if the guest masked
  // SIGSEGV — hangs dead with no log and no tombstone. So we ALWAYS account for it
  // (not just under a probe prop) and get loud on repeats, turning a silent hang
  // into a named address someone can pre-register.
  if (g_ready.load(std::memory_order_acquire) && sig == SIGSEGV && info != nullptr &&
      info->si_code == SEGV_ACCERR &&
      reinterpret_cast<uintptr_t>(info->si_addr) != rip &&
      !g_is_executable(g_shadow, reinterpret_cast<uintptr_t>(info->si_addr), 1)) {
    const uintptr_t target = reinterpret_cast<uintptr_t>(info->si_addr);
    int n = 0;
    const int used = g_rev_count.load(std::memory_order_relaxed);
    int i = 0;
    for (; i < used && i < kMaxRev; ++i) {
      if (g_rev[i].addr == target) { n = ++g_rev[i].n; break; }
    }
    if (n == 0 && used < kMaxRev) {
      g_rev[used].addr = target;
      g_rev[used].n = 1;
      n = 1;
      g_rev_count.store(used + 1, std::memory_order_relaxed);
    }
    // The COUNTING above is unconditional and allocation-free — that is what turns
    // a silent hang into something measurable. NAMING the address is not: dladdr
    // takes the linker's global lock and liblog takes its own, and this block's
    // condition is not actually specific to a reverse branch — it matches ANY data
    // fault to a non-guest-executable address, which includes the hardening
    // library's own mprotect(PROT_NONE) probes. Doing that on a thread that was
    // interrupted while holding the linker lock or inside malloc self-deadlocks
    // with no log and no tombstone, which is precisely the class of defect this
    // round was supposed to remove. So names come out only when asked for.
    const bool may_log = g_revprobe.load(std::memory_order_acquire) ||
                         g_logtrace.load(std::memory_order_relaxed);
    if (may_log && (n == 1 || n == 3 || g_revprobe.load(std::memory_order_acquire))) {
      Dl_info da = {};
      const char* alib = (dladdr(info->si_addr, &da) && da.dli_fname) ? da.dli_fname : "?";
      const char* asym = da.dli_sname ? da.dli_sname : "?";
      if (n >= 3) {
        LOGE("revbranch: guest -> host %s!%s si_addr=%lx seen %d times — this address is "
             "NOT pre-registered; expect a NoExec spin or a silent hang. Add it to "
             "RegisterHostLibc().",
             alib, asym, (unsigned long)target, n);
      } else {
        LOGI("revbranch: guest -> host %s!%s si_addr=%lx n=%d (rip=%lx); chaining",
             alib, asym, (unsigned long)target, n, (unsigned long)rip);
      }
    }
  }
#endif
  ChainToPrevious(sig, info, ucv);
}

// Reverse trampoline: guest branched to a pre-registered host function. Read the
// AAPCS64 integer args (x0..x7), call the host callee as a SysV integer function,
// write the result back to x0. Integer/pointer ABI only — matches every libc slot
// libunisec hooks. Runs in NORMAL context (post-dispatch, with a valid ThreadState),
// so the exported state accessors are safe here; Berberis handles the return-to-guest.
void HostIntMarshall(const void* func, void* thread_state) {
  void* cpu = g_get_cpu_state_ref(thread_state);  // &GetCPUState(ts)
  NbGuestRegs regs;
  regs.guest_arch = kNativeBridgeArchArm64;
  int rc = g_get_cpu_regs(&regs, cpu);
  if (rc != 0) {
    LOGE("reverse marshall: GetCpuState rc=%d (arch mismatch?)", rc);
    g_set_ret(cpu, 0);
    return;
  }
  using HostFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t, uint64_t);
  auto fn = reinterpret_cast<HostFn>(const_cast<void*>(func));
  const uint64_t* x = regs.regs_arm64.x;
  uint64_t ret = fn(x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]);
  g_set_ret(cpu, ret);
}

// --- reverse marshaller, legacy (ndk_translation) flavour ------------------
// The older translator predates the `guest_state_opaque.h` accessor layer, so it
// exports no CPU-state accessors at all. It does not need them: there, the
// marshaller's `ProcessState*` IS the `CPUState*` (offset 0) and `x[i]` lives at
// `i*8`, so the guest integer argument registers are just the first eight words.
//
// Established by five independent derivations against the shipped binary
// (build-id 553ba82b76bb5a11fd1d67b2c7e01b75); the decisive one is
// RunKernelSyscall(ProcessState*), which reads the syscall number from +0x40 (x8)
// and args from +0x00..+0x28 (x0..x5) and writes the result back to +0x00 — an
// exact match for the arm64 Linux syscall ABI, which admits no other reading.
//
// DANGER: do NOT reuse the NbGuestRegs-based reader here. That struct has
// `guest_arch` at 0 and the register block at +0x10, because the newer
// translator's GetCpuState() memcpys into nb+0x10. Carrying that +0x10 across
// would read x2..x9 and write the return value into x2 — silent argument
// corruption that would look exactly like the hardening library rejecting us.
using SaveRegsFn = void (*)(const void* process_state, void* buf, int);
SaveRegsFn g_legacy_save_regs = nullptr;
std::atomic<int> g_legacy_layout_checked{0};  // 0=unchecked, 1=ok, 2=failed

// One-shot, zero-risk confirmation of the layout above using only an EXPORTED
// symbol: SaveRegsToJumpBuf walks the same state with the same offsets, so if the
// values it stores for x30 and sp agree with what we read at x[30] and +0x108,
// the "state base + i*8" assumption is confirmed by the translator's own code
// rather than by our reading of a disassembly.
bool LegacyLayoutOk(const uint64_t* x) {
  const int cached = g_legacy_layout_checked.load(std::memory_order_acquire);
  if (cached != 0) return cached == 1;
  if (g_legacy_save_regs == nullptr) {
    g_legacy_layout_checked.store(1, std::memory_order_release);  // cannot check; proceed
    return true;
  }
  uint64_t buf[64] = {};  // the callee writes up to buf+0xf8
  g_legacy_save_regs(x, buf, 0);
  const bool ok = buf[0x70 / 8] == x[30] && buf[0x78 / 8] == x[0x108 / 8];
  g_legacy_layout_checked.store(ok ? 1 : 2, std::memory_order_release);
  if (ok) {
    LOGI("legacy guest-state layout confirmed via SaveRegsToJumpBuf (x30 and sp agree)");
  } else {
    LOGE("legacy guest-state layout CHECK FAILED (x30=%lx vs %lx, sp=%lx vs %lx); refusing "
         "to marshal rather than corrupt the translator's state",
         (unsigned long)buf[0x70 / 8], (unsigned long)x[30],
         (unsigned long)buf[0x78 / 8], (unsigned long)x[0x108 / 8]);
  }
  return ok;
}

void LegacyHostIntMarshall(const void* func, void* process_state) {
  auto* x = reinterpret_cast<uint64_t*>(process_state);
  if (!LegacyLayoutOk(x)) {
    x[0] = static_cast<uint64_t>(-1);
    return;
  }
  using HostFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t, uint64_t);
  auto fn = reinterpret_cast<HostFn>(const_cast<void*>(func));
  // The generated trampoline reads x30 for its return dispatch AFTER we return,
  // so we must leave it alone; we only ever write x0.
  x[0] = fn(x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]);
}

// Which marshaller the registrations use, chosen when the translator generation
// is identified.
MarshallFn g_marshall = nullptr;

// Register a host function so a guest branch INTO it is bridged (instead of a fatal
// NoExec). MakeTrampolineCallable only takes effect if `addr` has no TranslationCache
// entry yet, so this MUST run before the guest first branches there; afterwards it is
// a silent no-op.
void RegisterOneHost(void* addr, const char* name) {
  if (addr == nullptr || g_make_tramp == nullptr || g_marshall == nullptr) return;
  g_make_tramp(reinterpret_cast<uintptr_t>(addr), /*is_host_func=*/true, g_marshall,
               addr, name != nullptr ? name : "host");
}

// The libc functions libunisec is known to hook. Pre-registered at arm time so the
// hook's callback into the saved ORIGINAL (host) function is bridged, not fatal.
void RegisterHostLibc() {
  // LAYOUT-SAFE only: every arg and the return is an integer/pointer whose
  // pointed-to bytes are identical across arm64/x86_64. EXCLUDED on purpose:
  // open/openat (O_* flag values diverge between arm64 and x86_64) and fstat
  // (struct stat layout diverges) — the generic integer marshaller would pass those
  // through raw and silently corrupt semantics; they need a typed, flag-translating
  // shim before they can be bridged. mmap/munmap are safe (PROT_*/MAP_* identical on
  // Linux); read/__read_chk/pread/readv carry raw bytes; close/lseek are pure int.
  static const char* const kNames[] = {
      "read", "__read_chk", "pread", "pread64", "readv",
      "close", "lseek", "lseek64", "mmap", "munmap",
  };
  if (g_marshall == nullptr) {
    LOGE("reverse: no usable marshaller for this translator generation; not registering");
    return;
  }
  int n = 0;
  for (const char* name : kNames) {
    void* a = dlsym(RTLD_DEFAULT, name);
    if (a != nullptr) {
      RegisterOneHost(a, name);
      ++n;
    }
  }
  LOGI("reverse: pre-registered %d host libc functions", n);
}

// Generic GOT-slot locator. Finds the relocation slot that a given LOADED module
// uses for a given IMPORTED symbol, without dlsym (the companion lives in the
// bridge linker namespace and dlsym's namespace rules are deliberately not relied
// on here). Android's x86_64 build uses RELA; REL is handled too so the scanner
// stays useful on a loader variant.
struct GotSearch {
  const char* module_needle = nullptr;  // substring of the module's soname/path
  const char* symbol = nullptr;         // imported symbol name
  void** slot = nullptr;                // the writable pointer-sized slot
  uintptr_t module_base = 0;
  int original_prot = PROT_READ;
};

// Kept for the original libnativeloader hook's call sites.
using DlopenGotSearch = GotSearch;

uintptr_t RelocatedElfAddress(uintptr_t base, Elf64_Addr value) {
  // Dynamic pointers in a loaded shared object are virtual addresses relative
  // to the load bias.  (The add is also correct for the usual PIE layout where
  // the first PT_LOAD has p_vaddr == 0.)
  return base + static_cast<uintptr_t>(value);
}

int ProtectionForAddress(const dl_phdr_info* info, uintptr_t address) {
  const uintptr_t base = static_cast<uintptr_t>(info->dlpi_addr);
  // A slot inside PT_GNU_RELRO has already been sealed read-only by the linker by
  // the time any constructor runs, so restoring the PT_LOAD flags (RW) would
  // silently un-harden the translator's own GOT. Restore read-only instead.
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const Elf64_Phdr& ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_GNU_RELRO) continue;
    const uintptr_t begin = base + static_cast<uintptr_t>(ph.p_vaddr);
    const uintptr_t end = begin + static_cast<uintptr_t>(ph.p_memsz);
    if (address >= begin && address < end) return PROT_READ;
  }
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const Elf64_Phdr& ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD) continue;
    const uintptr_t begin = base + static_cast<uintptr_t>(ph.p_vaddr);
    const uintptr_t end = begin + static_cast<uintptr_t>(ph.p_memsz);
    if (address >= begin && address < end) {
      int prot = 0;
      if (ph.p_flags & PF_R) prot |= PROT_READ;
      if (ph.p_flags & PF_W) prot |= PROT_WRITE;
      if (ph.p_flags & PF_X) prot |= PROT_EXEC;
      return prot != 0 ? prot : PROT_READ;
    }
  }
  return PROT_READ;
}

bool IsWantedRelocation(const Elf64_Sym* symtab, const char* strtab,
                        const Elf64_Xword sym_index, const Elf64_Rela& rela,
                        uintptr_t base, GotSearch* out,
                        const dl_phdr_info* info) {
  const Elf64_Sym& sym = symtab[sym_index];
  const char* name = strtab + sym.st_name;
  if (strcmp(name, out->symbol) != 0) return false;
  const unsigned type = ELF64_R_TYPE(rela.r_info);
  if (type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT) return false;
  const uintptr_t address = RelocatedElfAddress(base, rela.r_offset);
  out->slot = reinterpret_cast<void**>(address);
  out->module_base = base;
  out->original_prot = ProtectionForAddress(info, address);
  return true;
}

int FindGotSlotCallback(struct dl_phdr_info* info, size_t, void* opaque) {
  auto* out = static_cast<GotSearch*>(opaque);
  const char* soname = info->dlpi_name;
  if (soname == nullptr || strstr(soname, out->module_needle) == nullptr) return 0;

  const uintptr_t base = static_cast<uintptr_t>(info->dlpi_addr);
  const Elf64_Dyn* dynamic = nullptr;
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
      dynamic = reinterpret_cast<const Elf64_Dyn*>(
          base + static_cast<uintptr_t>(info->dlpi_phdr[i].p_vaddr));
      break;
    }
  }
  if (dynamic == nullptr) return 0;

  const Elf64_Sym* symtab = nullptr;
  const char* strtab = nullptr;
  const Elf64_Rela* jmprel = nullptr;
  size_t jmprel_size = 0;
  bool jmprel_is_rela = true;
  const Elf64_Rela* rela = nullptr;
  size_t rela_size = 0;
  const Elf64_Rel* rel = nullptr;
  size_t rel_size = 0;
  for (const Elf64_Dyn* d = dynamic; d->d_tag != DT_NULL; ++d) {
    switch (d->d_tag) {
      case DT_SYMTAB:
        symtab = reinterpret_cast<const Elf64_Sym*>(RelocatedElfAddress(base, d->d_un.d_ptr));
        break;
      case DT_STRTAB:
        strtab = reinterpret_cast<const char*>(RelocatedElfAddress(base, d->d_un.d_ptr));
        break;
      case DT_JMPREL:
        jmprel = reinterpret_cast<const Elf64_Rela*>(RelocatedElfAddress(base, d->d_un.d_ptr));
        break;
      case DT_PLTRELSZ:
        jmprel_size = static_cast<size_t>(d->d_un.d_val);
        break;
      case DT_PLTREL:
        jmprel_is_rela = (d->d_un.d_val == DT_RELA);
        break;
      case DT_RELA:
        rela = reinterpret_cast<const Elf64_Rela*>(RelocatedElfAddress(base, d->d_un.d_ptr));
        break;
      case DT_RELASZ:
        rela_size = static_cast<size_t>(d->d_un.d_val);
        break;
      case DT_REL:
        rel = reinterpret_cast<const Elf64_Rel*>(RelocatedElfAddress(base, d->d_un.d_ptr));
        break;
      case DT_RELSZ:
        rel_size = static_cast<size_t>(d->d_un.d_val);
        break;
      default:
        break;
    }
  }
  if (symtab == nullptr || strtab == nullptr) return 0;

  // The x86_64 Android loader is RELA.  A RELA scan also covers the common
  // PLT table; the separate DT_RELA scan catches a GLOB_DAT import if needed.
  auto scan_rela = [&](const Elf64_Rela* entries, size_t bytes) -> bool {
    if (entries == nullptr) return false;
    const size_t count = bytes / sizeof(Elf64_Rela);
    for (size_t i = 0; i < count; ++i) {
      if (IsWantedRelocation(symtab, strtab, ELF64_R_SYM(entries[i].r_info), entries[i],
                             base, out, info)) {
        return true;
      }
    }
    return false;
  };
  if (jmprel_is_rela && scan_rela(jmprel, jmprel_size)) return 1;
  if (scan_rela(rela, rela_size)) return 1;

  // REL is not emitted for the current target, but keep a small compatibility
  // path for a loader built with it.  The field layout differs only by addend.
  if (rel != nullptr) {
    const size_t count = rel_size / sizeof(Elf64_Rel);
    for (size_t i = 0; i < count; ++i) {
      const Elf64_Rel& r = rel[i];
      const Elf64_Sym& sym = symtab[ELF64_R_SYM(r.r_info)];
      if (strcmp(strtab + sym.st_name, out->symbol) != 0) continue;
      const unsigned type = ELF64_R_TYPE(r.r_info);
      if (type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT) continue;
      const uintptr_t address = RelocatedElfAddress(base, r.r_offset);
      out->slot = reinterpret_cast<void**>(address);
      out->module_base = base;
      out->original_prot = ProtectionForAddress(info, address);
      return 1;
    }
  }
  return 0;
}

// Overwrite one GOT slot with `replacement`, handing back the previous value.
// Flips only the single page holding the slot, and restores the original
// protection afterwards — the same technique libunisec uses on libartbase, here
// applied strictly inside our own process on our own runtime's import table.
bool PatchOneGotSlot(const char* module_needle, const char* symbol, void* replacement,
                     void** real_out, const char* tag, bool log = true) {
  GotSearch found;
  found.module_needle = module_needle;
  found.symbol = symbol;
  if (dl_iterate_phdr(&FindGotSlotCallback, &found) == 0 || found.slot == nullptr) {
    return false;
  }
  void* current = *found.slot;
  if (current == replacement) {  // already patched (e.g. inherited through fork)
    return true;
  }
  if (current == nullptr) return false;

  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) return false;
  const uintptr_t slot_address = reinterpret_cast<uintptr_t>(found.slot);
  const uintptr_t page = slot_address & ~static_cast<uintptr_t>(page_size - 1);
  if ((slot_address - page) + sizeof(void*) > static_cast<uintptr_t>(page_size)) {
    LOGE("%s GOT slot crosses a page; refusing to patch", tag);
    return false;
  }
  if (mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size),
               PROT_READ | PROT_WRITE) != 0) {
    LOGE("mprotect(%s GOT) failed errno=%d", tag, errno);
    return false;
  }
  if (real_out != nullptr) *real_out = current;
  __atomic_store_n(found.slot, replacement, __ATOMIC_RELEASE);
  const int restore = found.original_prot != 0 ? found.original_prot : PROT_READ;
  if (mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), restore) != 0) {
    LOGE("restoring %s GOT protection failed errno=%d", tag, errno);
    // The pointer is already valid; keeping the page writable is safer than
    // leaving a half-installed hook with an unexpected protection state.
  }
  if (log) LOGI("patched %s GOT slot=%p real=%p", tag, static_cast<void*>(found.slot), current);
  return true;
}

// --- build the watched-GOT-slot table ------------------------------------
// The libc entry points a dex-protecting hardening library plausibly redirects.
// A slot only matters if it is actually rewritten to a guest address, so listing
// one that never gets hooked costs nothing but a pointer compare per fault.
const char* const kWatchedSyms[] = {
    "read",   "__read_chk", "pread",  "pread64", "readv",  "close",
    "lseek",  "lseek64",    "mmap",   "mmap64",  "munmap", "mprotect",
    "open",   "openat",     "__open_2", "__openat_2",
    "fstat",  "fstat64",    "fstatat", "fstatat64", "stat", "lstat",
    "write",  "pwrite",     "pwrite64", "dup",   "dup2",   "fcntl",
};

bool IsWatchedSym(const char* name) {
  for (const char* w : kWatchedSyms) {
    if (strcmp(name, w) == 0) return true;
  }
  return false;
}

int ScanModuleForWatchedSlots(struct dl_phdr_info* info, size_t, void* opaque) {
  auto* found = static_cast<int*>(opaque);
  const char* soname = info->dlpi_name;
  // Skip ourselves: our own imports are not what the guest rewrites, and matching
  // them would only add noise.
  if (soname != nullptr && strstr(soname, "libmodavd_gate.so") != nullptr) return 0;
  // Only record slots in images that cannot be unloaded underneath us. Both the
  // slot address and the symbol name we keep point INTO the module, so a module
  // that later gets dlclose()d leaves dangling pointers that the fault path would
  // dereference — taking a second SIGSEGV inside the handler, with a tombstone
  // pointing at our scan loop and looking for all the world like an anti-tamper
  // rejection. The slot that actually matters here lives in
  // /apex/com.android.art/lib64/libartbase.so, so nothing of value is lost.
  if (soname == nullptr ||
      (strncmp(soname, "/system/", 8) != 0 && strncmp(soname, "/apex/", 6) != 0 &&
       strncmp(soname, "/vendor/", 8) != 0 && strncmp(soname, "/system_ext/", 12) != 0)) {
    return 0;
  }

  const uintptr_t base = static_cast<uintptr_t>(info->dlpi_addr);
  const Elf64_Dyn* dynamic = nullptr;
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
      dynamic = reinterpret_cast<const Elf64_Dyn*>(
          base + static_cast<uintptr_t>(info->dlpi_phdr[i].p_vaddr));
      break;
    }
  }
  if (dynamic == nullptr) return 0;

  const Elf64_Sym* symtab = nullptr;
  const char* strtab = nullptr;
  const Elf64_Rela* jmprel = nullptr;
  size_t jmprel_size = 0;
  const Elf64_Rela* rela = nullptr;
  size_t rela_size = 0;
  for (const Elf64_Dyn* d = dynamic; d->d_tag != DT_NULL; ++d) {
    switch (d->d_tag) {
      case DT_SYMTAB: symtab = reinterpret_cast<const Elf64_Sym*>(base + d->d_un.d_ptr); break;
      case DT_STRTAB: strtab = reinterpret_cast<const char*>(base + d->d_un.d_ptr); break;
      case DT_JMPREL: jmprel = reinterpret_cast<const Elf64_Rela*>(base + d->d_un.d_ptr); break;
      case DT_PLTRELSZ: jmprel_size = static_cast<size_t>(d->d_un.d_val); break;
      case DT_RELA: rela = reinterpret_cast<const Elf64_Rela*>(base + d->d_un.d_ptr); break;
      case DT_RELASZ: rela_size = static_cast<size_t>(d->d_un.d_val); break;
      default: break;
    }
  }
  if (symtab == nullptr || strtab == nullptr) return 0;

  auto scan = [&](const Elf64_Rela* entries, size_t bytes) {
    if (entries == nullptr) return;
    const size_t count = bytes / sizeof(Elf64_Rela);
    for (size_t i = 0; i < count; ++i) {
      const unsigned type = ELF64_R_TYPE(entries[i].r_info);
      if (type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT) continue;
      const char* name = strtab + symtab[ELF64_R_SYM(entries[i].r_info)].st_name;
      if (!IsWatchedSym(name)) continue;
      const int n = g_slot_count.load(std::memory_order_relaxed);
      if (n >= kMaxSlots) {
        LOGE("slot table saturated at %d entries; the slot that matters may be missing "
             "— the strict discriminator is not trustworthy on this image",
             kMaxSlots);
        return;
      }
      auto* slot = reinterpret_cast<uintptr_t*>(base + entries[i].r_offset);
      // Append-only, deduplicated: a refresh re-walks every module, and a reader
      // may be scanning the table concurrently from a signal handler, so entries
      // must never move or be rewritten.
      bool dup = false;
      for (int k = 0; k < n; ++k) {
        if (g_slots[k].addr == slot) { dup = true; break; }
      }
      if (dup) continue;
      g_slots[n].addr = slot;
      g_slots[n].sym = name;  // points into the module's DT_STRTAB: stable while mapped
      g_slot_count.store(n + 1, std::memory_order_release);
      ++*found;
    }
  };
  scan(jmprel, jmprel_size);
  scan(rela, rela_size);
  return 0;  // keep iterating every module
}

// Read-only scan of the currently mapped images. Slot ADDRESSES are static within
// a module, so a module already mapped never needs rescanning — but a module
// mapped LATER brings slots the table has never seen, and under a strict
// discriminator an unknown slot means a refused (and therefore broken) bridge.
// That is not hypothetical: the neutral harness extracts and System.load()s its
// native caller well after arm time, which is exactly why its real-`read`-slot
// case does not match. So this is also called again from the library-load
// triggers, in normal context, and appends only what is new.
int BuildHookedSlotTable() {
  int found = 0;
  dl_iterate_phdr(&ScanModuleForWatchedSlots, &found);
  return found;
}

// Cheap guard so a burst of dlopens does not re-walk every module each time.
std::atomic<int> g_last_scan_mod_count{0};

int CountModules(struct dl_phdr_info*, size_t, void* opaque) {
  ++*static_cast<int*>(opaque);
  return 0;
}

void RefreshHookedSlotTableIfModulesGrew() {
  int mods = 0;
  dl_iterate_phdr(&CountModules, &mods);
  if (mods <= g_last_scan_mod_count.load(std::memory_order_relaxed)) return;
  g_last_scan_mod_count.store(mods, std::memory_order_relaxed);
  const int before = g_slot_count.load(std::memory_order_relaxed);
  BuildHookedSlotTable();
  const int after = g_slot_count.load(std::memory_order_relaxed);
  if (after != before) {
    LOGI("slot table: +%d new watched slots after a library load (%d total, %d modules)",
         after - before, after, mods);
  }
}

// REMOVED: the libnativeloader `android_dlopen_ext` GOT hook.
//
// It was the original (failed) arming trigger. Device evidence shows it never
// fires for an arm64-primary app — those loads route through
// NativeBridgeLoadLibraryExt into the translator's own loader, never through
// libnativeloader's slot — so it bought us nothing. But it was NOT harmless: the
// patch was applied in zygote and inherited by EVERY forked app through COW, and
// it pointed libnativeloader's slot at a function living inside
// libmodavd_gate.so. For a same-ISA app (and system_server), ART takes
// NativeBridgeAction::kUnload -> UnloadNativeBridge() -> dlclose(libndk); libndk
// carries no DF_1_NODELETE, so our companion can be unmapped with it, leaving
// that app's libnativeloader slot pointing into a hole — and the next native
// library load in a process that has nothing to do with us jumps into it.
// That directly violates the "non-target apps are untouched" guarantee, so the
// whole hook is gone rather than merely gated.

// --- per-process target gate ---------------------------------------------
// Every trigger wrapper is inherited by every forked child through COW, so each
// one must decide cheaply whether THIS process is the configured target. The
// decision (and the trace flag) is cached per process; the fork-child handler
// below invalidates it, so the fast path is a single relaxed atomic load with no
// syscall. The child handler only stores to an atomic — it creates NO thread,
// which is what previously bootlooped the device during zygote specialization.
// Packages the bridge arms for when `modavd.gate.target` is unset. Keep this list
// to titles whose behaviour under the bridge is known: every entry is a process
// that gets a process-wide SIGSEGV handler, and an unknown one is a regression
// waiting to be blamed on something else.
//
// 10-30: every 《阴阳师》 channel package MaaYYs knows (resource_pack\*\pipeline\
// 启动游戏.json) — official/TapTap, the 雷电/MuMu download, bilibili, Huawei, OPPO,
// vivo, 应用宝. They are the same game with the same hardening; only
// com.netease.onmyoji has been measured. Without the bridge none of them starts at
// all, so listing the rest cannot make them worse than stock.
constexpr char kDefaultGateTargets[] =
    "com.netease.onmyoji,com.netease.onmyoji.wyzymnqsd_cps,com.netease.onmyoji.bili,"
    "com.netease.onmyoji.huawei,com.netease.onmyoji.nearme.gamecenter,"
    "com.netease.onmyoji.vivo,com.tencent.tmgp.yys.zqb,com.lingxigames.yuan.cn";
static_assert(sizeof(kDefaultGateTargets) <= 256, "Default targets must fit the gate buffers");

constexpr int kGateUnknown = 0;
constexpr int kGateTarget = 1;
constexpr int kGateOther = 2;
std::atomic<int> g_gate_state{kGateUnknown};
// Set by modavd_gate_detach_unless_target() in a process that is not on the list:
// the translator GOT has its original targets back and nothing may arm here again.
// Inherited through fork, so a non-target app's own children stay detached too.
std::atomic<bool> g_detached{false};

void GateForgetAfterFork() { g_gate_state.store(kGateUnknown, std::memory_order_relaxed); }

// `modavd.gate.target` is a COMMA-SEPARATED allow-list. It is non-persistent, and
// nothing in the product sets it at boot, so an UNSET property falls back to the
// list compiled in above — that is what makes the bridge work out of the box
// after an install. Setting the property overrides the built-in list entirely, so
// `setprop modavd.gate.target none` is the kill switch (no process is named
// "none"). PROP_VALUE_MAX is 92 bytes, i.e. roughly four package names, so the
// property cannot hold the whole built-in list; if an override ever needs more,
// add a second property rather than reading a file (the constructor must not open
// file descriptors in zygote). `target` receives the list that was used.
bool OnGateList(const char* name, char* target, size_t target_cap) {
  if (__system_property_get("modavd.gate.target", target) <= 0 || target[0] == '\0') {
    const size_t n = sizeof(kDefaultGateTargets) < target_cap ? sizeof(kDefaultGateTargets) : target_cap;
    memcpy(target, kDefaultGateTargets, n);
    target[n - 1] = '\0';
  }
  bool match = false;
  // Walk the comma-separated list without allocating. An entry matches the
  // package's MAIN process, any of its sub-processes, and its app-zygote:
  //
  //   <pkg>            the main process
  //   <pkg>:<name>     a sub-process declared with android:process=":name"
  //   <pkg>_zygote     an app-zygote
  //
  // The sub-process form is not an edge case — it is how the hardening behaves.
  // Measured: with only the exact name matched, com.netease.onmyoji ran fine while
  // com.netease.onmyoji:PushService crash-looped with the identical signature
  // (SEGV_ACCERR, same hook offset), because it loads libunisec too and was not
  // armed. A per-package gate has to cover the whole package.
  for (const char* p = target; *p != '\0' && !match;) {
    const char* comma = strchr(p, ',');
    const size_t len = comma != nullptr ? static_cast<size_t>(comma - p) : strlen(p);
    if (len != 0 && strncmp(name, p, len) == 0) {
      const char suffix = name[len];
      match = suffix == '\0' || suffix == ':' || strcmp(name + len, "_zygote") == 0;
    }
    p = comma != nullptr ? comma + 1 : p + len;
  }
  return match;
}

bool IsTargetProcess() {
  if (g_detached.load(std::memory_order_acquire)) return false;
  const int cached = g_gate_state.load(std::memory_order_acquire);
  if (cached != kGateUnknown) return cached == kGateTarget;
  char proc[128];
  GetProcName(proc, sizeof(proc));
  if (proc[0] == '\0') return false;
  // The real zygote keeps renaming itself as children specialize, so its answer
  // must not be cached. An app-zygote ("<pkg>_zygote") does NOT rename, so it is
  // cacheable — and it must also be treated as the target, or an app that starts
  // from its own zygote silently never arms.
  const bool is_real_zygote = strcmp(proc, "zygote") == 0 || strcmp(proc, "zygote64") == 0 ||
                              strcmp(proc, "usap32") == 0 || strcmp(proc, "usap64") == 0;
  if (is_real_zygote) return false;

  char lt[PROP_VALUE_MAX] = {};
  __system_property_get("modavd.gate.logsig", lt);
  g_logtrace.store(lt[0] == '1', std::memory_order_relaxed);
  char target[256] = {};
  const bool match = OnGateList(proc, target, sizeof(target));
  const int state = match ? kGateTarget : kGateOther;
  g_gate_state.store(state, std::memory_order_release);
  if (state == kGateTarget || g_logtrace.load(std::memory_order_relaxed)) {
    LOGI("gate: pid=%d proc=%s target=%s -> %s", getpid(), proc, target,
         state == kGateTarget ? "ARM" : "passthrough");
  }
  return state == kGateTarget;
}

// Install (or re-install) our SIGSEGV handler so it sits on top of whatever the
// translator has installed, and remember what to chain to. Called at arm time and
// again every time the translator (re)installs its own SIGSEGV handler, so our
// position in the chain cannot be silently lost.
bool ReassertSegvHandler() {
  // Take over the DISPOSITION only, never the terms. Whoever currently owns
  // SIGSEGV asked the kernel for a specific sa_mask and sa_flags; when that
  // someone is the guest, Berberis built the host action from the GUEST's own
  // request (guest_signal_action.cc deliberately refuses to blanket-block), so
  // overwriting them with sigemptyset()+our own flags silently takes away things
  // the game asked for. SA_NODEFER is the one that bites: "mprotect PROT_NONE and
  // catch the nested SEGV" is a real anti-dump technique, and without SA_NODEFER
  // the nested fault kills the process inside its own handler. So: copy the
  // incumbent action wholesale and change only the function pointer.
  // Seed with a magic so a sigaction() that silently does nothing is detectable.
  // A process running under DEBUG_IGNORE_APP_SIGNAL_HANDLER has every sigaction
  // turn into a no-op that still returns 0; without this check we would "install"
  // forever, never actually be on top, and end up with a zeroed chain target.
  struct sigaction cur;
  memset(&cur, 0, sizeof(cur));
  const auto kMagic = reinterpret_cast<void (*)(int, siginfo_t*, void*)>(
      static_cast<uintptr_t>(0xA5A5A5A5A5A5A5A5ull));
  cur.sa_sigaction = kMagic;
  if (sigaction(SIGSEGV, nullptr, &cur) != 0) {
    LOGE("sigaction(SIGSEGV, query) failed errno=%d", errno);
    return false;
  }
  if (cur.sa_sigaction == kMagic) {
    LOGE("sigaction() is a silent no-op in this process (signal-hook debuggable?); "
         "disarming instead of spinning on re-install");
    g_ready.store(false, std::memory_order_release);
    return false;
  }
  const bool cur_is_ours =
      (cur.sa_flags & SA_SIGINFO) != 0 && cur.sa_sigaction == SegvHandler;
  if (cur_is_ours) return true;  // already on top; nothing to do

  struct sigaction sa = cur;
  sa.sa_sigaction = SegvHandler;
  sa.sa_flags |= SA_SIGINFO;  // we need the 3-argument form
  // Never hand back a restorer: bionic installs its own, and the translator treats
  // an unrecognised sa_restorer as fatal.
  sa.sa_flags &= ~static_cast<int>(SA_RESTORER);
  sa.sa_restorer = nullptr;

  struct sigaction prev = {};
  if (sigaction(SIGSEGV, &sa, &prev) != 0) {
    LOGE("sigaction(SIGSEGV) failed errno=%d", errno);
    return false;
  }
  const bool prev_is_ours =
      (prev.sa_flags & SA_SIGINFO) != 0 && prev.sa_sigaction == SegvHandler;
  void* h = (prev.sa_flags & SA_SIGINFO) ? reinterpret_cast<void*>(prev.sa_sigaction)
                                         : reinterpret_cast<void*>(prev.sa_handler);
  // A zeroed/!handler predecessor is NOT a usable chain target — storing it would
  // send ChainToPrevious down its abort path on the next fault we decline.
  const bool prev_usable = !prev_is_ours && h != nullptr &&
                           h != reinterpret_cast<void*>(SIG_DFL) &&
                           h != reinterpret_cast<void*>(SIG_IGN);
  if (prev_usable) {
    g_old_segv = prev;
    g_berberis_segv.store(h, std::memory_order_relaxed);
    Dl_info di = {};
    const char* lib = (dladdr(h, &di) && di.dli_fname != nullptr) ? di.dli_fname : "?";
    LOGI("segv handler asserted; chain -> %p (%s, flags=0x%x inherited)", h, lib, prev.sa_flags);
  } else if (!prev_is_ours) {
    // Arming with NO usable chain target is worse than not arming at all: every
    // fault we decline would fall through ChainToPrevious' "nothing to chain to"
    // branch and abort the process — including the faults the game's own handler
    // was going to absorb. If we have no target from an earlier assert either, put
    // the incumbent back and refuse to arm.
    const bool have_chain =
        ((g_old_segv.sa_flags & SA_SIGINFO) != 0 && g_old_segv.sa_sigaction != nullptr) ||
        ((g_old_segv.sa_flags & SA_SIGINFO) == 0 && g_old_segv.sa_handler != nullptr &&
         g_old_segv.sa_handler != SIG_DFL && g_old_segv.sa_handler != SIG_IGN);
    LOGE("segv handler asserted but predecessor is unusable (handler=%p flags=0x%x); %s",
         h, prev.sa_flags,
         have_chain ? "keeping the previous chain target" : "backing out, refusing to arm");
    if (!have_chain) {
      sigaction(SIGSEGV, &prev, nullptr);  // restore the incumbent exactly
      return false;
    }
  }
  return true;
}

// Anything in the process that installs a SIGSEGV handler AFTER us displaces us
// (libsigchain reaches the last-installed handler first): the neutral harness'
// own x86_64 caller does exactly that, and the game's hardening may too. So on
// every trigger we check cheaply whether we are still on top and, if not, get
// back on top and chain to whoever displaced us. A guest mprotect is serviced
// through our trigger, so for the game this re-check lands right before
// libunisec flips libartbase's RELRO and rewrites the slots.
void MaybeReassertSegv() {
  if (!g_ready.load(std::memory_order_acquire)) return;
  ReassertSegvHandler();  // queries first and no-ops when we are already on top
}

// --- translator-GOT trigger wrappers -------------------------------------
// Per-trigger first-fire accounting: answers "did the trigger fire at all, when,
// and which one" from a single run, which is the question every arming failure
// reduces to.
std::atomic<int> g_trigger_hits[4];  // sigaction, mprotect, android_dlopen_ext, dlopen

void TryArmFromTrigger(const char* why, int idx) {
  if (g_ready.load(std::memory_order_acquire)) return;
  if (g_in_trigger) return;
  if (!IsTargetProcess()) return;
  const int seen = g_trigger_hits[idx].fetch_add(1, std::memory_order_relaxed);
  if (seen == 0 && g_logtrace.load(std::memory_order_relaxed)) {
    LOGI("trigger: pid=%d from=%s first=1", getpid(), why);
  }
  g_in_trigger = true;
  const int rc = modavd_gate_init();
  if (rc == 0) {
    LOGI("armed from %s (trigger hits: sigaction=%d mprotect=%d dlopen_ext=%d dlopen=%d)", why,
         g_trigger_hits[0].load(std::memory_order_relaxed),
         g_trigger_hits[1].load(std::memory_order_relaxed),
         g_trigger_hits[2].load(std::memory_order_relaxed),
         g_trigger_hits[3].load(std::memory_order_relaxed));
  } else if (g_logtrace.load(std::memory_order_relaxed)) {
    LOGI("arm deferred from %s (rc=%d)", why, rc);
  }
  g_in_trigger = false;
}

// The translator reaches libc's sigaction through this one slot from several very
// different places, and one of them is its FATAL SIGNAL path: HandleFatalSignal
// restores the saved original disposition through exactly this slot, in signal
// context, on a thread that is already dying. Doing anything non-trivial here —
// opening /proc, reading properties, taking liblog's lock — risks re-entering
// malloc during a heap-corruption SEGV and deadlocking, which costs that app its
// tombstone and turns a clean crash into an ANR. For EVERY bridged app, not just
// ours.
//
// So the first thing this function does is classify the call site, using
// fingerprints that need no hardcoded addresses:
//   oldact == nullptr            -> the fatal-signal restore. Forward and return,
//                                   touching nothing.
//   act == oldact (non-null)     -> the guest claiming/changing its own handler,
//                                   on a thread holding the translator's
//                                   signal-actions mutex. Forward only.
//   oldact != nullptr && != act  -> ordinary init-time installation. The only
//                                   class where we may arm or re-assert.
int NdkSigactionWrapper(int sig, const struct sigaction* act, struct sigaction* oldact) {
  SigactionFn real = g_real_ndk_sigaction;
  // Deliberately NO dlsym(RTLD_DEFAULT, "sigaction") fallback: that could resolve
  // past libsigchain and install straight into the kernel, which breaks ART's
  // implicit null-check handling and gets us silently evicted on the next
  // EnsureFrontOfChain.
  if (real == nullptr) {
    errno = ENOSYS;
    return -1;
  }

  // --- fatal-signal restore: do nothing but forward -----------------------
  if (oldact == nullptr) return real(sig, act, oldact);

  const bool guest_change = (act == oldact);
  const int rc = real(sig, act, oldact);

  // Never let the guest read back OUR address as the previous disposition: it is
  // a reportable anomaly, and if it ever "restores" that action our chain target
  // becomes ourselves and the handler recurses forever. Hand back what we are
  // standing in front of instead. (sizeof(struct sigaction) is fixed; we only
  // rewrite the one pointer field.)
  if (rc == 0 && sig == SIGSEGV && (oldact->sa_flags & SA_SIGINFO) != 0 &&
      oldact->sa_sigaction == SegvHandler) {
    void* saved = g_berberis_segv.load(std::memory_order_relaxed);
    if (saved != nullptr) oldact->sa_sigaction = reinterpret_cast<void (*)(int, siginfo_t*, void*)>(saved);
  }

  if (guest_change) return rc;  // guest-held mutex: forward only, never arm

  if (g_logtrace.load(std::memory_order_relaxed)) {
    void* h = (act != nullptr)
                  ? ((act->sa_flags & SA_SIGINFO) ? reinterpret_cast<void*>(act->sa_sigaction)
                                                  : reinterpret_cast<void*>(act->sa_handler))
                  : nullptr;
    LOGI("sigaction: pid=%d sig=%d cls=INIT act=%p handler=%p flags=0x%x ret=%d",
         getpid(), sig, static_cast<const void*>(act), h, act != nullptr ? act->sa_flags : 0, rc);
  }

  if (rc == 0 && sig == SIGSEGV && act != nullptr && !g_in_trigger && IsTargetProcess()) {
    const int flags = act->sa_flags;
    void* h = (flags & SA_SIGINFO) ? reinterpret_cast<void*>(act->sa_sigaction)
                                   : reinterpret_cast<void*>(act->sa_handler);
    const bool real_handler = h != nullptr && h != reinterpret_cast<void*>(SIG_DFL) &&
                              h != reinterpret_cast<void*>(SIG_IGN);
    if (real_handler) {
      if (g_ready.load(std::memory_order_acquire)) {
        g_in_trigger = true;
        MaybeReassertSegv();
        g_in_trigger = false;
      } else {
        TryArmFromTrigger("libndk!sigaction(SIGSEGV)", 0);
      }
    }
  }
  return rc;
}

// The must-not-miss backstop: every guest mprotect — the guest linker protecting
// freshly loaded segments, and later libunisec flipping libartbase's RELRO — is
// serviced by the translator calling the HOST mprotect through this slot. So this
// fires after the bridge is fully initialised and before the rewritten GOT slot
// can ever be called, in any process, without depending on the load route.
int NdkMprotectWrapper(void* addr, size_t len, int prot) {
  MprotectFn real = g_real_ndk_mprotect;
  if (real == nullptr) {
    errno = ENOSYS;
    return -1;
  }
  const int rc = real(addr, len, prot);
  if (!g_ready.load(std::memory_order_acquire)) {
    TryArmFromTrigger("libndk!mprotect", 1);
  } else if (!g_in_trigger) {
    g_in_trigger = true;
    MaybeReassertSegv();
    g_in_trigger = false;
  }
  return rc;
}

void* NdkAndroidDlopenExtWrapper(const char* filename, int flags, const void* extinfo) {
  AndroidDlopenExtFn real = g_real_ndk_android_dlopen_ext;
  if (real == nullptr) return nullptr;
  if (g_logtrace.load(std::memory_order_relaxed)) {
    LOGI("trace libndk!android_dlopen_ext(%s)", filename != nullptr ? filename : "<null>");
  }
  void* result = real(filename, flags, extinfo);
  if (!g_ready.load(std::memory_order_acquire)) {
    TryArmFromTrigger("libndk!android_dlopen_ext", 2);
  } else if (!g_in_trigger) {
    g_in_trigger = true;
    RefreshHookedSlotTableIfModulesGrew();  // a new module brings new watched slots
    MaybeReassertSegv();
    g_in_trigger = false;
  }
  return result;
}

void* NdkDlopenWrapper(const char* filename, int flags) {
  DlopenFn real = g_real_ndk_dlopen;
  if (real == nullptr) return nullptr;
  if (g_logtrace.load(std::memory_order_relaxed)) {
    LOGI("trace libndk!dlopen(%s)", filename != nullptr ? filename : "<null>");
  }
  void* result = real(filename, flags);
  if (!g_ready.load(std::memory_order_acquire)) {
    TryArmFromTrigger("libndk!dlopen", 3);
  } else if (!g_in_trigger) {
    g_in_trigger = true;
    RefreshHookedSlotTableIfModulesGrew();  // a new module brings new watched slots
    MaybeReassertSegv();
    g_in_trigger = false;
  }
  return result;
}

// Patch the translator's own import table. Runs in the constructor, i.e. while the
// linker is still bringing up libndk_translation.so (we are its DT_NEEDED, so its
// relocations are already applied and its link_map entry already exists, but none
// of its constructors have run). In zygote this patch is inherited by every forked
// app process through COW; the wrappers themselves are target-gated.
using RawSyscallFn = long (*)(long, ...);
static_assert(SYS_clone == 56 && CLONE_VM == 0x100, "The syscall trampoline requires Linux x86_64");
static_assert((CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | CLONE_PIDFD) == 0x1181000,
              "The clone guard requires unchanged host TLS and cached tid");
extern "C" {
RawSyscallFn modavd_original_syscall = nullptr;
__attribute__((used, visibility("hidden"))) long ModavdGuardClone(long nr, long flags, long stack,
                                                                  long ptid, long ctid, long tls);
// Keep all registers and the optional sixth stack argument intact on every
// unsupported syscall. A clone that can change host TLS or write the cached tid
// must not enter C++: the loader's recursive-mutex unlock reads that cached tid.
__attribute__((naked)) long ModavdSyscallWrapper(long, ...) {
  __asm__ volatile(
      "cmp $56, %rdi\n"
      "jne 1f\n"
      "test $0x100, %rsi\n"
      "jnz 1f\n"
      "test $0x1181000, %rsi\n"
      "jnz 1f\n"
      "test %rdx, %rdx\n"
      "jnz 1f\n"
      "jmp ModavdGuardClone\n"
      "1: jmp *modavd_original_syscall(%rip)\n");
}
}

bool InstallTranslatorHooks() {
  if (g_ndk_hooks_installed.load(std::memory_order_acquire)) return true;
  const char* kNdk = "libndk_translation.so";  // does not match ..._proxy_*.so
  int n = 0;
  void* real = nullptr;
  if (PatchOneGotSlot(kNdk, "sigaction", reinterpret_cast<void*>(&NdkSigactionWrapper), &real,
                      "libndk!sigaction")) {
    if (real != nullptr) g_real_ndk_sigaction = reinterpret_cast<SigactionFn>(real);
    ++n;
  }
  real = nullptr;
  if (PatchOneGotSlot(kNdk, "mprotect", reinterpret_cast<void*>(&NdkMprotectWrapper), &real,
                      "libndk!mprotect")) {
    if (real != nullptr) g_real_ndk_mprotect = reinterpret_cast<MprotectFn>(real);
    ++n;
  }
  real = nullptr;
  if (PatchOneGotSlot(kNdk, "android_dlopen_ext",
                      reinterpret_cast<void*>(&NdkAndroidDlopenExtWrapper), &real,
                      "libndk!android_dlopen_ext")) {
    if (real != nullptr) g_real_ndk_android_dlopen_ext = reinterpret_cast<AndroidDlopenExtFn>(real);
    ++n;
  }
  real = nullptr;
  if (PatchOneGotSlot(kNdk, "dlopen", reinterpret_cast<void*>(&NdkDlopenWrapper), &real,
                      "libndk!dlopen")) {
    if (real != nullptr) g_real_ndk_dlopen = reinterpret_cast<DlopenFn>(real);
    ++n;
  }
  GotSearch syscall_slot;
  syscall_slot.module_needle = kNdk;
  syscall_slot.symbol = "syscall";
  if (dl_iterate_phdr(&FindGotSlotCallback, &syscall_slot) && syscall_slot.slot &&
      *syscall_slot.slot) {
    modavd_original_syscall = reinterpret_cast<RawSyscallFn>(*syscall_slot.slot);
    if (PatchOneGotSlot(kNdk, "syscall", reinterpret_cast<void*>(&ModavdSyscallWrapper), nullptr,
                        "libndk!syscall"))
      ++n;
  }
  LOGI("translator GOT triggers installed: %d/5", n);
  if (n > 0) g_ndk_hooks_installed.store(true, std::memory_order_release);
  return n > 0;
}

// Put one translator GOT slot back to the target it had before InstallTranslatorHooks.
int RestoreTranslatorSlot(const char* symbol, void* real) {
  if (real == nullptr) return 0;
  return PatchOneGotSlot("libndk_translation.so", symbol, real, nullptr, symbol, false) ? 1 : 0;
}

}  // namespace

// 分流 (10-30). Called by the shim from the native bridge's initialize(), i.e. in a
// freshly forked app process before the translator is initialised and before ART
// asks the bridge for its signal handlers. The process is still named "zygote64"
// there, so the shim hands over the package name it reads from the app's
// code-cache directory. Returns 1 when the package is on the gate list (nothing
// changes), 0 when it is not: then all five translator GOT slots get their original
// targets back and the gate is off for good in this process. Together with the shim
// handing ART the translator's own callbacks, a non-target app then runs the stock
// translator with nothing of ours left on its path.
extern "C" __attribute__((visibility("default"))) int modavd_gate_detach_unless_target(
    const char* pkg) {
  if (pkg == nullptr || pkg[0] == '\0') return 1;  // unknown: keep today's behaviour
  char target[256] = {};
  if (OnGateList(pkg, target, sizeof(target))) return 1;
  g_detached.store(true, std::memory_order_release);
  g_gate_state.store(kGateOther, std::memory_order_release);
  int n = 0;
  if (g_ndk_hooks_installed.load(std::memory_order_acquire)) {
    n += RestoreTranslatorSlot("sigaction", reinterpret_cast<void*>(g_real_ndk_sigaction));
    n += RestoreTranslatorSlot("mprotect", reinterpret_cast<void*>(g_real_ndk_mprotect));
    n += RestoreTranslatorSlot("android_dlopen_ext", reinterpret_cast<void*>(g_real_ndk_android_dlopen_ext));
    n += RestoreTranslatorSlot("dlopen", reinterpret_cast<void*>(g_real_ndk_dlopen));
    n += RestoreTranslatorSlot("syscall", reinterpret_cast<void*>(modavd_original_syscall));
  }
  LOGI("detach: pid=%d pkg=%s not on the gate list; translator GOT restored %d/5", getpid(), pkg,
       n);
  return 0;
}

namespace {

// (The libnativeloader android_dlopen_ext wrapper that used to live here was
// removed — see the note on InstallAndroidDlopenExtHook's removal above.)

// --- fork safety of the translator (10-29) -----------------------------------
// WHAT HAPPENED: 《阴阳师》 now and then stopped for good at "正在获取音频列表..."
// before the login screen (about one start in five). Read-only look at the stuck
// process (/proc task states, /proc/<pid>/syscall, its stack memory; no ptrace): no
// thread busy, none waiting on the network (60 s of capture: one packet). The game's
// main logic thread sat in pipe_read and another one in kernel_clone, each waiting on a
// child it had forked to run a command. Both children were single-threaded copies of
// the game that never reached exec: blocked in futex_wait on the code-pool or
// translation-cache mutex in libndk_translation's .bss (0x241d78, 0x242fb8 in the
// API-34 build), called from the translator's own run loop. In the parent both
// mutexes were free; in each child its
// copy was locked (2). The classic fork-in-a-threaded-process deadlock: the game's
// arm64 code forks through the translator, the child goes on running translated code,
// and the first time it needs a translator lock that another thread happened to hold
// at the fork, it waits forever. The translator registers no fork handlers of its own.
//
// Fix, touching neither the translator nor the game nor libunisec: in a target
// process, hold those translator mutexes across fork(). The prepare handler takes all
// of them (try-lock; on a miss release what it has, yield and retry, so it cannot
// deadlock against a translator thread holding one and waiting for the other); the
// parent and child handlers release what they took. Before accepting those locks,
// require cache quiescence: no active translations and no dispatch slot marked
// Translating, Invalidating or Wrapping. An inherited marker has no worker in the
// child to finish it. InvalidateEntriesBeingTranslatedUnsafe is NOT recovery: it
// only changes Translating to Invalidating, leaving the same infinite wait.
// If the locks cannot all be had within
// kForkLockWaitNs (a forking thread that holds one itself would be such a case), the
// fork goes on unprotected, exactly as before, and the miss is logged. The offsets are
// valid only for the build-id below; any other translator build is left alone.
constexpr unsigned char kNdkForkLocksBuildId[16] = {
    0x55, 0x3b, 0xa8, 0x2b, 0x76, 0xbb, 0x5a, 0x11,
    0xfd, 0x1d, 0x67, 0xb2, 0xc7, 0xe0, 0x1b, 0x75,
};
constexpr int kForkTranslationCacheLock = 1;
constexpr uintptr_t kNdkForkLockOffs[] = {
    0x241d78,  // Default code pool.
    0x242fb8,  // TranslationCache; its mutex is the first member.
    0x242f00,  // Thread table, also taken by ResetCurrentGuestThreadAfterFork.
};
constexpr int kNumForkLocks = sizeof(kNdkForkLockOffs) / sizeof(kNdkForkLockOffs[0]);
constexpr long kForkLockWaitNs = 200L * 1000 * 1000;
pthread_mutex_t* g_fork_locks[kNumForkLocks] = {};
using TranslationCacheInstanceFn = void* (*)();
void* g_fork_translation_cache = nullptr;
uintptr_t g_fork_transient_entries[3] = {};  // Actual dispatcher stubs, resolved by name.
struct CacheScanStats {
  size_t entries = 0;
  size_t visited = 0;
  long ns = 0;
  uintptr_t first_node = 0;
  uintptr_t previous_key = 0;
  bool has_previous_key = false;
};
enum class CacheState { kQuiescent, kBusy, kUnknown };
std::atomic<bool> g_fork_safety{false};
// Who holds the locks for a fork in progress. Only a prepare handler that got ALL of
// them sets these, and they are guarded by the locks themselves.
std::atomic<bool> g_fork_held{false};
std::atomic<pid_t> g_fork_owner{0};
std::atomic<int> g_fork_misses{0};
std::atomic<int> g_fork_misses_logged{0};
std::atomic<int> g_fork_unknown_scans{0};
std::atomic<int> g_fork_unknown_scans_logged{0};

long MonoNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

uintptr_t CacheWord(uintptr_t address) {
  uintptr_t value;
  memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
  return value;
}

// Read-only API-34 layout, verified against AddAndLockForTranslation and
// InvalidateGuestRange: cache map root/count at +0x48/+0x50; node left/right at
// +0/+8 and GuestCodeEntry at +0x28, whose first word points to the atomic slot.
// The cache mutex must be held throughout. Bound depth and node count so a
// structural mismatch is refused, never mistaken for an idle cache.
CacheState ScanForkCacheNode(uintptr_t node, uintptr_t parent, unsigned depth, CacheScanStats& stats) {
  if (node == 0) return CacheState::kQuiescent;
  if (depth > 64 || stats.visited >= stats.entries || (node & 7) != 0)
    return CacheState::kUnknown;
  if (CacheWord(node + 0x10) != parent) return CacheState::kUnknown;
  ++stats.visited;
  const uintptr_t left_node = CacheWord(node);
  const uintptr_t right_node = CacheWord(node + 8);
  if (left_node != 0 && left_node == right_node) return CacheState::kUnknown;
  const CacheState left = ScanForkCacheNode(left_node, node, depth + 1, stats);
  if (left != CacheState::kQuiescent) return left;
  const uintptr_t key = CacheWord(node + 0x20);
  if (stats.has_previous_key && key <= stats.previous_key) return CacheState::kUnknown;
  if (!stats.has_previous_key) stats.first_node = node;
  stats.previous_key = key;
  stats.has_previous_key = true;
  const uintptr_t slot = CacheWord(node + 0x28);
  if (slot == 0 || (slot & 7) != 0) return CacheState::kUnknown;
  const uintptr_t code = __atomic_load_n(reinterpret_cast<const uintptr_t*>(slot), __ATOMIC_RELAXED);
  if (code == 0) return CacheState::kUnknown;
  for (uintptr_t transient : g_fork_transient_entries) {
    if (code == transient) return CacheState::kBusy;
  }
  return ScanForkCacheNode(right_node, node, depth + 1, stats);
}

CacheState ScanForkCache(CacheScanStats& stats) {
  const uintptr_t cache = reinterpret_cast<uintptr_t>(g_fork_translation_cache);
  if (cache == 0) return CacheState::kUnknown;
  for (uintptr_t transient : g_fork_transient_entries) {
    if (transient == 0) return CacheState::kUnknown;
  }
  stats.entries = CacheWord(cache + 0x50);
  if (CacheWord(cache + 0x38) != 0) return CacheState::kBusy;
  const uintptr_t root = CacheWord(cache + 0x48);
  const uintptr_t begin = CacheWord(cache + 0x40);
  if ((stats.entries == 0) != (root == 0) || root == cache + 0x48 ||
      (stats.entries == 0 ? begin != cache + 0x48 : begin == 0 || begin == cache + 0x48))
    return CacheState::kUnknown;
  const CacheState state = ScanForkCacheNode(root, cache + 0x48, 1, stats);
  if (state != CacheState::kQuiescent) return state;
  return stats.visited == stats.entries &&
                 (stats.entries == 0 || stats.first_node == begin)
             ? CacheState::kQuiescent
             : CacheState::kUnknown;
}

bool TryForkLocks(CacheScanStats* measured = nullptr) {
  int got = 0;
  for (; got < kNumForkLocks; ++got) {
    if (pthread_mutex_trylock(g_fork_locks[got]) != 0) break;
  }
  if (got == kNumForkLocks) {
    CacheScanStats stats;
    const long begin = MonoNs();
    const CacheState state = ScanForkCache(stats);
    stats.ns = MonoNs() - begin;
    if (measured != nullptr) *measured = stats;
    if (state == CacheState::kQuiescent) {
      g_fork_held = true;
      g_fork_owner = gettid();
      return true;
    }
    if (state == CacheState::kUnknown) g_fork_unknown_scans.fetch_add(1, std::memory_order_relaxed);
  }
  while (got > 0) pthread_mutex_unlock(g_fork_locks[--got]);
  return false;
}

void ForkPrepare() {
  if (!g_fork_safety.load(std::memory_order_acquire)) return;
  const long deadline = MonoNs() + kForkLockWaitNs;
  for (;;) {
    if (TryForkLocks()) return;
    if (MonoNs() >= deadline) {
      g_fork_misses.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    sched_yield();
  }
}

void ForkRelease() {
  g_fork_held = false;
  g_fork_owner = 0;
  for (int i = kNumForkLocks; i > 0; --i) pthread_mutex_unlock(g_fork_locks[i - 1]);
}

void ForkParent() {
  if (g_fork_held && g_fork_owner == gettid()) ForkRelease();
  const int misses = g_fork_misses.load(std::memory_order_relaxed);
  if (misses > g_fork_misses_logged.exchange(misses, std::memory_order_relaxed)) {
    LOGE("fork safety: could not take the translator locks with a quiescent cache within the %ld ms "
         "retry budget (%d so far); "
         "that child may hang as before", kForkLockWaitNs / 1000000, misses);
  }
  const int unknown = g_fork_unknown_scans.load(std::memory_order_relaxed);
  if (unknown > g_fork_unknown_scans_logged.exchange(unknown, std::memory_order_relaxed)) {
    LOGE("fork safety: cache layout or dispatcher stubs could not be validated (%d scans); "
         "those attempts were refused", unknown);
  }
}

// In the child only the forking thread exists. If our prepare got the locks, they are
// held by it on behalf of a cache-quiescent fork. Do not mutate inherited cache
// entries; only release the mutexes before the translator resets its thread table.
void ForkChild() {
  if (g_fork_held) ForkRelease();
}

void InstallForkSafety(uintptr_t base) {
  static std::atomic<bool> once{false};
  if (once.exchange(true)) return;
  if (!BuildIdIs(base, kNdkForkLocksBuildId)) {
    LOGI("fork safety: translator build-id differs from the one the lock offsets come from; not installed");
    return;
  }
  for (int i = 0; i < kNumForkLocks; ++i) {
    g_fork_locks[i] = reinterpret_cast<pthread_mutex_t*>(base + kNdkForkLockOffs[i]);
  }
  // Resolve and initialise in ordinary parent context, never in the fork child.
  TranslatorDyn dyn;
  dl_iterate_phdr(&FindTranslatorDynCallback, &dyn);
  const auto get_cache = reinterpret_cast<TranslationCacheInstanceFn>(LookupExport(
      dyn, "_ZN15ndk_translation16TranslationCache11GetInstanceEv"));
  const uintptr_t transient_entries[] = {
      LookupExport(dyn, "ndk_translation_entry_Translating"),
      LookupExport(dyn, "ndk_translation_entry_Invalidating"),
      LookupExport(dyn, "ndk_translation_entry_Wrapping"),
  };
  if (dyn.base != base || get_cache == nullptr || transient_entries[0] == 0 ||
      transient_entries[1] == 0 || transient_entries[2] == 0) {
    LOGE("fork safety: cache quiescence exports missing; not installed");
    return;
  }
  void* cache = get_cache();
  if (cache != g_fork_locks[kForkTranslationCacheLock]) {
    LOGE("fork safety: translation-cache instance differs from the verified mutex; not installed");
    return;
  }
  g_fork_translation_cache = cache;
  memcpy(g_fork_transient_entries, transient_entries, sizeof(transient_entries));
  if (pthread_atfork(&ForkPrepare, &ForkParent, &ForkChild) != 0) {
    LOGE("fork safety: pthread_atfork failed; not installed");
    return;
  }
  g_fork_safety.store(true, std::memory_order_release);
  LOGI("fork safety: holding %d translator locks across fork(); cache quiescence enabled",
       kNumForkLocks);
}

struct RawCloneCall {
  long nr, flags, stack, ptid, ctid, tls;
  bool held = false;
  long prepared = 0;
  long result = -1;
  int saved_errno = 0;
  CacheScanStats cache_scan{};
};

// API 34's dl_iterate_phdr holds the loader's recursive mutex across this call.
// Never wait for a translator mutex here: a translator thread may already hold
// one while waiting to dlopen. A failed attempt must immediately leave the loader
// scope so that thread can finish. The caller retries only after it is outside.
int RawCloneUnderLoaderLock(struct dl_phdr_info*, size_t, void* opaque) {
  auto& call = *static_cast<RawCloneCall*>(opaque);
  if (TryForkLocks(&call.cache_scan)) {
    call.held = true;
    call.prepared = MonoNs();
    call.result = modavd_original_syscall(call.nr, call.flags, call.stack, call.ptid, call.ctid,
                                        call.tls);
    call.saved_errno = errno;
  }
  return 1;  // Stop iteration and release exactly this loader-lock nesting level.
}

extern "C" __attribute__((used, visibility("hidden"))) long ModavdGuardClone(long nr, long flags,
                                                                             long stack, long ptid,
                                                                             long ctid, long tls) {
  if (!g_fork_safety.load(std::memory_order_acquire)) {
    return modavd_original_syscall(nr, flags, stack, ptid, ctid, tls);
  }
  const long begin = MonoNs();
  const long deadline = begin + kForkLockWaitNs;
  RawCloneCall call{nr, flags, stack, ptid, ctid, tls};
  for (;;) {
    // Enter without translator locks. This public loader-lock acquisition itself
    // can block; the 200 ms deadline bounds retries, not the whole loader API.
    dl_iterate_phdr(&RawCloneUnderLoaderLock, &call);
    if (call.held) break;
    if (MonoNs() >= deadline) {
      g_fork_misses.fetch_add(1, std::memory_order_relaxed);
      call.prepared = MonoNs();
      call.result = modavd_original_syscall(nr, flags, stack, ptid, ctid, tls);
      call.saved_errno = errno;
      break;
    }
    sched_yield();
  }
  // The loader scope is now gone in parent and child. The raw-clone subset above
  // leaves the host cached tid unchanged, so its recursive unlock used the same
  // owner in both. Bionic fork's atfork handlers never take this loader scope.
  if (call.result == 0) {
    if (call.held) ForkChild();  // Before ResetCurrentGuestThreadAfterFork takes the table lock.
  } else {
    ForkParent();
    const long released = MonoNs();
    LOGI("raw clone guard: flags=%lx held=%d result=%ld wait_us=%ld protected_us=%ld "
         "cache_entries=%zu scan_nodes=%zu scan_us=%ld", flags, call.held, call.result,
         (call.prepared - begin) / 1000, (released - call.prepared) / 1000,
         call.cache_scan.entries, call.cache_scan.visited, call.cache_scan.ns / 1000);
  }
  errno = call.saved_errno;
  return call.result;
}

}  // namespace

// Idempotent. Returns 0 on success; non-zero if the translator isn't loaded yet
// (1), a symbol is missing (2), sigaction failed (3), the translator build-id does
// not match our offsets (4), or this is the zygote (5). Safe to call again later
// (e.g. once the native bridge is initialised) if it first returns 1.
extern "C" __attribute__((visibility("default"))) int modavd_gate_init() {
  if (g_ready.load(std::memory_order_acquire)) return 0;
  // Detached (not on the gate list): never re-install the translator hooks here.
  if (g_detached.load(std::memory_order_acquire)) return 6;
  // Usually installed by the constructor while the companion is loaded into
  // zygote.  Retrying here also makes a direct System.load() neutral-harness
  // run work if libnativeloader was not visible during the constructor.
  if (!g_ndk_hooks_installed.load(std::memory_order_acquire)) InstallTranslatorHooks();
  char proc[128];
  GetProcName(proc, sizeof(proc));
  if (proc[0] == '\0' || strstr(proc, "zygote") != nullptr) {
    // The zygote itself, or a just-forked child not yet specialized: wait, never arm here.
    return 5;
  }
  // Target gate: arm ONLY in a configured package's process, so a process-wide
  // deployment leaves every other app — and all stock behaviour — untouched. One
  // implementation, in IsTargetProcess(): it owns the allow-list semantics and
  // caches the decision per process.
  if (!IsTargetProcess()) return 6;
  uintptr_t base = FindMappedLibBase("libndk_translation.so");
  if (base == 0) {
    // Translator not mapped in this process yet. Caller retries later.
    return 1;
  }
  // Preferred path: resolve the eight exports from the mapped image's own dynsym.
  // Survives both a different translator build and the +0x1000 st_value shift our
  // LIEF DT_NEEDED patch introduces.
  // The shipped translator comes in two naming generations that are otherwise the
  // same machinery: the older one (the API-34-era image) puts everything in the
  // `ndk_translation` namespace, the newer one (Berberis 16) in `berberis`. The
  // signatures are identical except that the reverse-registration callback's state
  // argument is called ProcessState in the old build and ThreadState in the new —
  // a rename, not a layout change. Resolving both spellings is what lets one
  // companion binary serve both images, which is what keeps the project on a
  // single system image.
  struct NameSet {
    const char* tag;
    const char* wrap;
    const char* run;
    const char* shadow;
    const char* isexec;
    const char* tramp;
    const char* cpuref;
    const char* cpuregs;
    const char* setret;
    const char* saveregs;  // legacy only: used to self-check the state layout
  };
  static const NameSet kNameSets[] = {
      {"berberis",
       "_ZN8berberis21WrapGuestFunctionImplEmPKcPFvmPNS_19GuestArgumentBufferEES1_",
       "_ZN8berberis12RunGuestCallEmPNS_19GuestArgumentBufferE",
       "_ZN8berberis14GuestMapShadow11GetInstanceEv",
       "_ZNK8berberis14GuestMapShadow12IsExecutableEmm",
       "_ZN8berberis22MakeTrampolineCallableEmbPFvPKvPNS_11ThreadStateEES1_PKc",
       "_ZN8berberis11GetCPUStateERNS_11ThreadStateE",
       "_ZN8berberis11GetCpuStateEP21NativeBridgeGuestRegsPKNS_8CPUStateE",
       "_ZN8berberis22SetReturnValueRegisterERNS_8CPUStateEm",
       nullptr},
      {"ndk_translation",
       "_ZN15ndk_translation21WrapGuestFunctionImplEmPKcPFvmPNS_19GuestArgumentBufferEES1_",
       "_ZN15ndk_translation12RunGuestCallEmPNS_19GuestArgumentBufferE",
       "_ZN15ndk_translation14GuestMapShadow11GetInstanceEv",
       "_ZNK15ndk_translation14GuestMapShadow12IsExecutableEmm",
       "_ZN15ndk_translation22MakeTrampolineCallableEmbPFvPKvPNS_12ProcessStateEES1_PKc",
       // The older build exports no CPU-state accessors — it predates that layer
       // entirely. It does not need them: its marshaller receives the CPUState
       // directly (see LegacyHostIntMarshall). SaveRegsToJumpBuf is pulled in only
       // to self-check that layout at runtime against the translator's own code.
       nullptr, nullptr, nullptr,
       "_ZN15ndk_translation17SaveRegsToJumpBufEPKNS_12ProcessStateEPvi"},
  };

  TranslatorDyn dyn;
  const char* how = "by name";
  const char* ns = "?";
  uintptr_t a_wrap = 0, a_run = 0, a_shadow = 0, a_isexec = 0, a_tramp = 0, a_cpuref = 0,
            a_cpuregs = 0, a_setret = 0, a_saveregs = 0;
  if (dl_iterate_phdr(&FindTranslatorDynCallback, &dyn) != 0 && dyn.gnu_hash != nullptr) {
    for (const NameSet& set : kNameSets) {
      const uintptr_t w = LookupExport(dyn, set.wrap);
      const uintptr_t r = LookupExport(dyn, set.run);
      const uintptr_t s = LookupExport(dyn, set.shadow);
      const uintptr_t e = LookupExport(dyn, set.isexec);
      if (!(w && r && s && e)) continue;  // not this generation
      ns = set.tag;
      a_wrap = w; a_run = r; a_shadow = s; a_isexec = e;
      a_tramp = set.tramp != nullptr ? LookupExport(dyn, set.tramp) : 0;
      a_cpuref = set.cpuref != nullptr ? LookupExport(dyn, set.cpuref) : 0;
      a_cpuregs = set.cpuregs != nullptr ? LookupExport(dyn, set.cpuregs) : 0;
      a_setret = set.setret != nullptr ? LookupExport(dyn, set.setret) : 0;
      a_saveregs = set.saveregs != nullptr ? LookupExport(dyn, set.saveregs) : 0;
      break;
    }
  }
  // The forward four are what we resolve a generation by. The reverse accessors are
  // looked up too, but their absence is handled below — NOT by quietly proceeding.
  const bool by_name = a_wrap && a_run && a_shadow && a_isexec;
  if (!by_name) {
    // Fallback: the hardcoded offsets, which are valid ONLY for the one translator
    // build they were extracted from — arming against a different build would send
    // every offset into the wrong code and corrupt the translator's own state.
    if (!VerifyBuildId(base)) {
      LOGE("libndk exports not resolvable by name at base=%p AND build-id mismatch: "
           "refusing to arm",
           (void*)base);
      return 4;
    }
    how = "by offset (build-id gated)";
    ns = "berberis";
    a_wrap = base + kOffWrap;
    a_run = base + kOffRunGuestCall;
    a_shadow = base + kOffShadowInstance;
    a_isexec = base + kOffIsExecutable;
    a_tramp = base + kOffMakeTramp;
    a_cpuref = base + kOffGetCpuStateRef;
    a_cpuregs = base + kOffGetCpuRegs;
    a_setret = base + kOffSetRet;
  }
  g_wrap = reinterpret_cast<WrapFn>(a_wrap);
  g_run_guest_call = reinterpret_cast<void*>(a_run);
  g_shadow_instance = reinterpret_cast<ShadowInstanceFn>(a_shadow);
  g_is_executable = reinterpret_cast<IsExecutableFn>(a_isexec);
  g_make_tramp = reinterpret_cast<MakeTrampFn>(a_tramp);
  g_get_cpu_state_ref = reinterpret_cast<GetCpuStateRefFn>(a_cpuref);
  g_get_cpu_regs = reinterpret_cast<GetCpuRegsFn>(a_cpuregs);
  g_set_ret = reinterpret_cast<SetRetFn>(a_setret);
  g_shadow = g_shadow_instance();
  if (g_shadow == nullptr) {
    // The bridge is not up yet in this process; a later trigger retries.
    g_wrap = nullptr;
    return 1;
  }
  g_legacy_save_regs = reinterpret_cast<SaveRegsFn>(a_saveregs);
  // Pick the reverse marshaller that matches this translator generation. The newer
  // one goes through the exported state accessors; the older one reads the CPUState
  // it is handed directly, because that layer of API did not exist yet.
  const bool accessors =
      g_get_cpu_state_ref != nullptr && g_get_cpu_regs != nullptr && g_set_ret != nullptr;
  if (accessors) {
    g_marshall = &HostIntMarshall;
  } else if (g_make_tramp != nullptr) {
    g_marshall = &LegacyHostIntMarshall;
  } else {
    g_marshall = nullptr;
  }
  const bool reverse_ok = g_make_tramp != nullptr && g_marshall != nullptr;
  LOGI("resolved libndk %s (ns=%s): base=%p wrap=%p run=%p makeTramp=%p shadow=%p reverse=%s(%s)",
       how, ns, (void*)base, (void*)g_wrap, g_run_guest_call, (void*)g_make_tramp, g_shadow,
       reverse_ok ? "available" : "UNAVAILABLE",
       accessors ? "accessors" : (g_marshall != nullptr ? "direct-state" : "none"));
  if (!reverse_ok) {
    // MEASURED, do not "optimise" this away: arming with only the forward half is
    // WORSE than not arming. The hardening library's hook calls back into the
    // saved original host function, and without a pre-registered trampoline for it
    // the guest branches to a host address the translator cannot execute. On the
    // device that turned a clean one-second crash into an immediate death with a
    // `revbranch` diagnostic instead — strictly harder to diagnose and no closer to
    // running. (A/B: reverse registration on -> game reaches its Client activity,
    // 0 crashes; off -> dead, Fatal signal 11, revbranch=1.)
    // So: refuse, unless someone is deliberately experimenting.
    char fo[PROP_VALUE_MAX] = {};
    __system_property_get("modavd.gate.forwardonly", fo);
    if (fo[0] != '1') {
      LOGE("translator exports no CPU-state accessors, so the reverse half of the bridge "
           "cannot be built; refusing to arm (forward-only is measurably worse than stock). "
           "Set modavd.gate.forwardonly=1 to override for an experiment.");
      g_wrap = nullptr;
      return 7;
    }
    LOGE("forward-only arming FORCED by modavd.gate.forwardonly=1 — expect the guest hook's "
         "callback into the saved original host function to die on an unbridged branch");
  }
  char probe_val[PROP_VALUE_MAX] = {};
  __system_property_get("modavd.gate.probe", probe_val);
  g_probe.store(probe_val[0] == '1', std::memory_order_release);
  char revprobe_val[PROP_VALUE_MAX] = {};
  __system_property_get("modavd.gate.revprobe", revprobe_val);
  g_revprobe.store(revprobe_val[0] == '1', std::memory_order_release);
  char slotstrict_val[PROP_VALUE_MAX] = {};
  __system_property_get("modavd.gate.slotstrict", slotstrict_val);
  g_slotstrict.store(slotstrict_val[0] == '1', std::memory_order_release);
  // Reverse pre-registration FIRST — before publishing g_ready and installing the
  // forward handler — so a guest thread already running libunisec cannot take an
  // unbridged reverse branch in the gap. Skipped under a probe mode that wants to
  // observe the raw faults. (Must precede the first reverse branch; MakeTrampolineCallable
  // is a silent no-op once that pc has a TC entry.)
  if (!g_probe.load(std::memory_order_acquire) && !g_revprobe.load(std::memory_order_acquire)) {
    RegisterHostLibc();
  }
  // Build the watched-GOT-slot table BEFORE publishing g_ready: the discriminator
  // consults it on the very first fault. Read-only scan of already-mapped images.
  const int nslots = BuildHookedSlotTable();
  LOGI("slot table: %d watched libc GOT slots (discriminator: %s)", nslots,
       g_slotstrict.load(std::memory_order_acquire) ? "STRICT" : "advisory");
  if (nslots == 0) {
    LOGE("slot table EMPTY — the strict discriminator would be inoperative; a guest "
         "PROT_EXEC mprotect over a host library's text could be mistaken for guest code");
  }
  // On Android this install is intercepted by libsigchain, which reaches the
  // last-installed handler first. The arming triggers call this right after the
  // translator installs its own SIGSEGV handler, and re-assert on every later
  // install of its, so our handler stays on top of its fatal path.
  if (!ReassertSegvHandler()) return 3;
  g_ready.store(true, std::memory_order_release);
  LOGI("modavd_gate armed");
  InstallForkSafety(base);
  return 0;
}

// First-chance SIGSEGV entry for libsigchain's special-handler slot.
//
// WHY: our sigaction() handler only stays in front because every translator-side
// trigger re-asserts it (ReassertSegvHandler). A NATIVE library that installs a
// SIGSEGV handler later is invisible to those triggers. Measured on the API-34 image:
// WebView's crashpad (libmonochrome_64.so) installs one when the game's login SDK
// loads WebView, about 2 s into startup. Whenever our next native->guest fault landed
// before the next re-assert, crashpad took it for a crash, spawned its handler with
// --trace-parent-with-exception, and that handler ptrace-stopped the main thread for
// good: "executing service ...RecordServiceG37" ANR, crash_dump "already traced by
// <pid> (/system/bin/linker64)". Hit or miss on timing — the "won't start, retry
// works" pattern.
//
// The native bridge interface has a dedicated hook for exactly this: ART asks
// NativeBridgeItf::getSignalHandler(sig) for every signal when it sets the bridge up
// in an app process and registers a non-null answer with AddSpecialSignalHandlerFn.
// Special handlers run before ANY sigaction() handler, whoever installs it and
// whenever. The shim (nbshim) hands ART this function for SIGSEGV. It claims only our
// own fault — the discriminator is the same one SegvHandler uses — and returns false
// for everything else, so libsigchain carries on to the regular chain (crashpad, the
// game's breakpad, the translator) exactly as before. The game, libunisec and their
// handlers are not touched; we only stop leaking our private trap to them.
//
// In a process that is not on the allow-list g_ready never becomes true, so this
// returns false at once: stock behaviour.
extern "C" __attribute__((visibility("default"))) bool modavd_gate_segv_first_chance(
    int sig, siginfo_t* info, void* ucv) {
  return TryBridgeNativeToGuest(sig, info, static_cast<ucontext_t*>(ucv), "first-chance");
}

// Pre-register one host function for the reverse bridge (used by the neutral harness
// to register its callback, and available to a game loader). No-op under revprobe so
// the reverse fault can still be observed. Must be called before the guest first
// branches to `addr`.
extern "C" __attribute__((visibility("default"))) void modavd_gate_register_host(
    void* addr, const char* name) {
  LOGI("register_host(addr=%p name=%s) ready=%d revprobe=%d", addr, name ? name : "?",
       (int)g_ready.load(std::memory_order_acquire), (int)g_revprobe.load(std::memory_order_acquire));
  if (!g_ready.load(std::memory_order_acquire)) return;
  if (g_revprobe.load(std::memory_order_acquire)) return;
  RegisterOneHost(addr, name);
}

// JNI entry for the NEUTRAL harness only (System.load'ed companion exports aren't
// visible via dlsym(RTLD_DEFAULT) across the app's load namespaces, but JNI method
// resolution searches all loaded libs). The game does not use this — it auto-registers
// host libc at arm time. Pre-registers one host address before the guest branches to it.
extern "C" __attribute__((visibility("default"))) JNIEXPORT void JNICALL
Java_com_modavd_isabridge_MainActivity_companionRegisterHost(JNIEnv*, jclass, jlong addr) {
  modavd_gate_register_host(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), "harness_cb");
}

// Arming is triggered by the inherited android_dlopen_ext GOT wrapper.  No
// child-atfork thread is used: a thread during zygote specialization causes
// selinux_android_setcontext() to abort and bootloop the device.

// Best-effort auto-arm on load. The constructor runs in zygote when the
// DT_NEEDED dependency is loaded. It only installs the inherited GOT wrapper
// and calls modavd_gate_init once; it never creates a thread. In a specialized
// child, the wrapper invokes modavd_gate_init synchronously immediately before
// the next native-library load. The target property remains the default-off
// safety gate, so non-target apps keep stock behavior.
__attribute__((constructor)) static void ModAvdGateAutoInit() {
  char lt[PROP_VALUE_MAX] = {};
  __system_property_get("modavd.gate.logsig", lt);
  g_logtrace.store(lt[0] == '1', std::memory_order_relaxed);
  // Invalidate the cached per-process gate decision in every forked child. Stores
  // to one atomic only — no thread is created, which is what bootlooped zygote
  // specialization before.
  pthread_atfork(nullptr, nullptr, &GateForgetAfterFork);
  // The trigger that actually fires for an arm64-primary app: the translator's own
  // import table. See the ARMING TRIGGER note above for why libnativeloader's
  // android_dlopen_ext slot (kept below as a secondary) never fires there.
  if (!InstallTranslatorHooks()) {
    LOGE("could not install any libndk_translation GOT trigger at load time");
  }
  int rc = modavd_gate_init();
  if (rc == 1) {
    LOGI("auto-init deferred (translator not mapped yet); dlopen wrapper will retry");
  } else if (rc == 5) {
    LOGI("loaded in zygote/unspecialized; arming is deferred to the dlopen GOT wrapper");
  } else if (rc != 0 && rc != 6) {
    LOGE("auto-init refused (rc=%d)", rc);
  }
}
