// libmodavd_bridge.so — ModAVD native-bridge shim, PHASE 1 (observation only).
//
// WHAT THIS IS
// ------------
// Android's native bridge has an official entry point: the property
// ro.dalvik.vm.native.bridge names a library, libnativebridge dlopen()s it in the
// "system" linker namespace and dlsym()s the symbol "NativeBridgeItf", a
// NativeBridgeCallbacks struct.  This file is OUR OWN library that exports its own
// NativeBridgeItf, loads the REAL translator (Google's closed
// libndk_translation.so, Berberis 16.0.0) itself, copies the real struct into ours
// and forwards every callback to it.  A handful of callbacks are wrapped so we get
// control at well-defined moments; phase 1 only LOGS at those moments.
//
// It replaces the old injection method (patching Google's translator with LIEF to
// add DT_NEEDED libmodavd_gate.so).  Nothing here modifies a Google binary.
//
// STRUCT LAYOUT — why the definition below is the right one
// ---------------------------------------------------------
// On our image (sdk_gphone64_x86_64, Android 16 / API 36.1, build BE4B.251210.005)
// the translator exports NativeBridgeItf as a 168-byte GLOBAL OBJECT, and the
// bytes in the file say version == 8:
//
//   $ llvm-readelf --dyn-syms libndk_translation.so | grep NativeBridgeItf
//     3207: 0000000000590088   168 OBJECT GLOBAL DEFAULT 24 NativeBridgeItf
//   file offset 0x590088: version=8, padding=0, then 20 non-null pointers
//   (relocations R_X86_64_RELATIVE at 0x590090 .. 0x590128 inclusive — all 20).
//
// 168 = 4 (uint32_t version) + 4 (padding to 8) + 20 * 8 (function pointers).
// That is exactly AOSP main / android-16's NativeBridgeCallbacks, whose last
// member isNativeBridgeFunctionPointer is tagged "Introduced in: version 8".
// For reference, the same struct is 19 pointers = 160 bytes in android15-release
// (version 7) and 17 pointers = 144 bytes in android14-release (version 6), so the
// 168-byte size and version==8 agree and pin the layout uniquely.
//
// Even so, we do not trust the layout more than we have to: the constructor
// memcpy()s the real struct VERBATIM into ours and then overwrites only the slots
// we deliberately wrap.  Every slot we do not wrap is byte-identical to the real
// translator's, so a subtly wrong type in the declaration below cannot hurt a
// forwarded call.
//
// NO THREAD-LOCAL STORAGE.  This library is dlopen()ed; a dlopened library
// carrying initial-exec TLS relocations is rejected by the Bionic linker and the
// whole native bridge then fails to load, which costs the device all arm64
// support.  Do not add -ftls-model=initial-exec and do not add thread_local.
// Verify with: llvm-readelf -r libmodavd_bridge.so | grep TPOFF64  (must be empty)

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <unistd.h>

#define LOG_TAG "ModAvdNB"

// Deliberately no <atomic>, no STL, no function-local statics: APP_STL is "none",
// so there is no libc++ to provide __cxa_guard_acquire, and every global below is
// POD with a constant initializer.  The only dynamic initialization is our
// explicit constructor.

// ---------------------------------------------------------------------------
// AOSP main / android-16 NativeBridgeCallbacks (native bridge interface v8).
// Kept at global scope on purpose: it types the extern "C" NativeBridgeItf object
// below, and giving an external-linkage object a type with internal linkage (i.e.
// one declared in an anonymous namespace) is ill-formed.
// Pointer types are spelled out for documentation; opaque types that we never
// dereference are void*.
// ---------------------------------------------------------------------------

using NativeBridgeSignalHandlerFn = bool (*)(int, siginfo_t*, void*);

// enum JNICallType { kJNICallTypeRegular = 1, kJNICallTypeCriticalNative = 2 };
// A C enum of those values is int-sized and passed in a 32-bit register slot, so
// `int` here is ABI-identical.
using JNICallTypeInt = int;

struct NativeBridgeCallbacks {
  uint32_t version;
  bool (*initialize)(const void* runtime_cbs, const char* private_dir,
                     const char* instruction_set);
  void* (*loadLibrary)(const char* libpath, int flag);
  void* (*getTrampoline)(void* handle, const char* name, const char* shorty, uint32_t len);
  bool (*isSupported)(const char* libpath);
  const void* (*getAppEnv)(const char* instruction_set);
  bool (*isCompatibleWith)(uint32_t bridge_version);
  NativeBridgeSignalHandlerFn (*getSignalHandler)(int signal);
  int (*unloadLibrary)(void* handle);
  const char* (*getError)();
  bool (*isPathSupported)(const char* library_path);
  bool (*unused_initAnonymousNamespace)(const char* public_ns_sonames,
                                        const char* anon_ns_library_path);
  void* (*createNamespace)(const char* name, const char* ld_library_path,
                           const char* default_library_path, uint64_t type,
                           const char* permitted_when_isolated_path, void* parent_ns);
  bool (*linkNamespaces)(void* from, void* to, const char* shared_libs_sonames);
  void* (*loadLibraryExt)(const char* libpath, int flag, void* ns);
  void* (*getVendorNamespace)();
  void* (*getExportedNamespace)(const char* name);
  void (*preZygoteFork)();
  void* (*getTrampolineWithJNICallType)(void* handle, const char* name, const char* shorty,
                                        uint32_t len, JNICallTypeInt jni_call_type);
  void* (*getTrampolineForFunctionPointer)(const void* method, const char* shorty,
                                           uint32_t len, JNICallTypeInt jni_call_type);
  bool (*isNativeBridgeFunctionPointer)(const void* method);
};

static_assert(sizeof(NativeBridgeCallbacks) == 168,
              "NativeBridgeCallbacks must be 168 bytes (native bridge v8, LP64) — "
              "that is the size of the translator's exported NativeBridgeItf object");
static_assert(offsetof(NativeBridgeCallbacks, initialize) == 8, "layout");
static_assert(offsetof(NativeBridgeCallbacks, isSupported) == 32, "layout");
static_assert(offsetof(NativeBridgeCallbacks, getAppEnv) == 40, "layout");
static_assert(offsetof(NativeBridgeCallbacks, getSignalHandler) == 56, "layout");
static_assert(offsetof(NativeBridgeCallbacks, isPathSupported) == 80, "layout");
static_assert(offsetof(NativeBridgeCallbacks, loadLibraryExt) == 112, "layout");
static_assert(offsetof(NativeBridgeCallbacks, preZygoteFork) == 136, "layout");

// Defined (and statically initialised) further down; ShimInitialize rewrites it.
extern "C" __attribute__((visibility("default"))) NativeBridgeCallbacks NativeBridgeItf;

// How many bytes of the real struct are safe to copy, as a function of the version
// it reports.  Copying 168 bytes out of a 160-byte object would read past the end
// of the symbol, so the length follows the version rather than our sizeof().
//   v6 (android-14): 17 pointers -> 144 bytes, last member preZygoteFork
//   v7 (android-15): 19 pointers -> 160 bytes
//   v8 (android-16): 20 pointers -> 168 bytes
// Every slot we wrap lives inside the first 144 bytes, so the wrapping is the same
// for v6, v7 and v8.  Below v6 we do not model the struct at all and refuse.
namespace {

constexpr uint32_t kMinSupportedVersion = 6;

size_t RealStructBytesForVersion(uint32_t version) {
  if (version >= 8) return 168;
  if (version == 7) return 160;
  if (version == 6) return 144;
  return 0;
}

// ---------------------------------------------------------------------------
// State.  Plain globals, accessed with the compiler's atomic builtins (no TLS,
// no locks, no allocation).
// ---------------------------------------------------------------------------

const NativeBridgeCallbacks* g_real = nullptr;  // the real translator's struct
void* g_real_handle = nullptr;                  // never dlclose()d: see README
int g_ready = 0;                                // 1 once g_real is usable
size_t g_real_bytes = 0;                        // how much of *g_real the ctor copied

// The companion's modavd_gate_detach_unless_target (see ShimInitialize).
using GateDetachFn = int (*)(const char* pkg);
GateDetachFn g_companion_detach = nullptr;

// Diagnostic logging gate (property modavd.nb.log, default ON for this build),
// cached per process.  Cached alongside the pid it was read in so that a value
// inherited through fork() is re-read in the child.
int g_log_on = 1;
int g_log_pid = 0;

// Per-callback invocation counters.  These are inherited across fork(), which is
// exactly what makes them useful: if zygote already ran a callback the child
// starts at a non-zero count.
unsigned g_n_initialize = 0;
unsigned g_n_is_supported = 0;
unsigned g_n_is_path_supported = 0;
unsigned g_n_load_library = 0;
unsigned g_n_load_library_ext = 0;
unsigned g_n_get_signal_handler = 0;
unsigned g_n_pre_zygote_fork = 0;
unsigned g_n_get_app_env = 0;

// SIGSEGV first-chance composition (see ShimGetSignalHandler).  Written in normal
// context — the companion's pointer by the constructor, the translator's by
// getSignalHandler — before ART can register the composed handler; only read in
// signal context afterwards.
NativeBridgeSignalHandlerFn g_companion_segv = nullptr;  // modavd_gate_segv_first_chance
NativeBridgeSignalHandlerFn g_real_segv = nullptr;       // the translator's own, if any

inline bool Ready() { return __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) != 0; }

unsigned Bump(unsigned* counter) {
  return __atomic_add_fetch(counter, 1u, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------------------
// Logging helpers.
// ---------------------------------------------------------------------------

// /proc/self/cmdline, first NUL-separated token.  Read fresh every time: the
// process name changes after the zygote forks and the app renames itself, so a
// cached value would be wrong exactly where it matters.  The fd is opened
// O_CLOEXEC and closed inside this function, so no descriptor is ever live across
// a fork we do not control (a stray fd makes zygote's fd checks kill the process).
void ProcName(char* out, size_t cap) {
  if (cap == 0) return;
  out[0] = '\0';
  const int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    strncpy(out, "?", cap - 1);
    out[cap - 1] = '\0';
    return;
  }
  ssize_t n = read(fd, out, cap - 1);
  close(fd);
  if (n <= 0) {
    strncpy(out, "?", cap - 1);
    out[cap - 1] = '\0';
    return;
  }
  out[n] = '\0';  // cmdline is NUL-separated; this also terminates the first token
  if (out[0] == '\0') {
    strncpy(out, "?", cap - 1);
    out[cap - 1] = '\0';
  }
}

pid_t Tid() { return static_cast<pid_t>(syscall(__NR_gettid)); }

bool LogOn() {
  const int pid = static_cast<int>(getpid());
  if (__atomic_load_n(&g_log_pid, __ATOMIC_RELAXED) != pid) {
    char buf[PROP_VALUE_MAX] = {0};
    const int n = __system_property_get("modavd.nb.log", buf);
    // Default ON: this is a diagnostic build.  Only an explicit 0/false/off
    // silences it.
    int on = 1;
    if (n > 0 && (strcmp(buf, "0") == 0 || strcmp(buf, "false") == 0 ||
                  strcmp(buf, "off") == 0)) {
      on = 0;
    }
    __atomic_store_n(&g_log_on, on, __ATOMIC_RELAXED);
    __atomic_store_n(&g_log_pid, pid, __ATOMIC_RELAXED);
    return on != 0;
  }
  return __atomic_load_n(&g_log_on, __ATOMIC_RELAXED) != 0;
}

#define NB_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define NB_LOGF(...) __android_log_print(ANDROID_LOG_FATAL, LOG_TAG, __VA_ARGS__)
#define NB_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// One line per wrapped callback.  Stable, greppable shape:
//   cb=<name> n=<count in this process> pid=<pid> tid=<tid> proc=<cmdline> <extra>
#define NB_TRACE(name, counter, fmt, ...)                                             \
  do {                                                                                \
    const unsigned nb_n_ = Bump(&(counter));                                          \
    if (LogOn()) {                                                                    \
      char nb_proc_[192];                                                             \
      ProcName(nb_proc_, sizeof(nb_proc_));                                           \
      NB_LOGI("cb=" name " n=%u pid=%d tid=%d proc=%s " fmt, nb_n_,                   \
              static_cast<int>(getpid()), static_cast<int>(Tid()), nb_proc_,           \
              __VA_ARGS__);                                                           \
    }                                                                                 \
  } while (0)

const char* Str(const char* s) { return s != nullptr ? s : "(null)"; }

// ---------------------------------------------------------------------------
// Wrapped callbacks.  Each one logs, then forwards.  Each one is null-safe: if
// the real translator was never resolved, or the real slot is null, it returns a
// conservative "not supported / nothing here" answer and never dereferences
// anything.  That matters because these addresses are baked into our exported
// struct at static-initialization time, so they are reachable even if the
// constructor failed.
// ---------------------------------------------------------------------------

// The package name out of the app's code-cache directory, which is what ART passes
// to initialize(): /data/user/<n>/<pkg>/code_cache.  False for anything that does
// not look like that; the caller then keeps today's behaviour.
constexpr char kCodeCache[] = "/code_cache";

bool PackageFromPrivateDir(const char* dir, char* out, size_t cap) {
  if (dir == nullptr || cap == 0) return false;
  size_t end = strlen(dir);
  while (end > 0 && dir[end - 1] == '/') --end;
  const size_t kc = sizeof(kCodeCache) - 1;
  if (end < kc || strncmp(dir + end - kc, kCodeCache, kc) != 0) return false;
  end -= kc;
  size_t start = end;
  while (start > 0 && dir[start - 1] != '/') --start;
  const size_t len = end - start;
  if (len == 0 || len >= cap) return false;
  bool dot = false;
  for (size_t i = start; i < end; ++i) {
    const char c = dir[i];
    if (c == '.') {
      dot = true;
    } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '_')) {
      return false;
    }
  }
  if (!dot) return false;
  memcpy(out, dir + start, len);
  out[len] = '\0';
  return true;
}

bool ShimInitialize(const void* runtime_cbs, const char* private_dir,
                    const char* instruction_set) {
  // THE question phase 1 exists to answer: is this called once in zygote, or once
  // per app process?  runtime_cbs is an ART-owned pointer we never dereference.
  NB_TRACE("initialize", g_n_initialize, "isa=%s private_dir=%s runtime_cbs=%p ppid=%d",
           Str(instruction_set), Str(private_dir), runtime_cbs,
           static_cast<int>(getppid()));
  if (!Ready() || g_real->initialize == nullptr) {
    NB_LOGF("cb=initialize REFUSED: no real translator — arm64 support is off");
    return false;
  }
  // 分流 (10-30).  This is the first callback in a freshly forked app process, ahead
  // of the translator's own initialisation and of ART asking for signal handlers.
  // For a package that is not on the companion's gate list, hand libnativebridge the
  // translator's own struct back (it reads the callbacks through our exported
  // object on every call, and after fork that object is this process's private
  // copy) and let the companion restore the translator GOT.  From here on the app
  // runs the stock translator: no wrapped callbacks, no first-chance SIGSEGV
  // handler, no GOT triggers, no logging.
  const GateDetachFn detach = __atomic_load_n(&g_companion_detach, __ATOMIC_ACQUIRE);
  char pkg[128];
  if (detach != nullptr && PackageFromPrivateDir(private_dir, pkg, sizeof(pkg)) &&
      detach(pkg) == 0) {
    memcpy(&NativeBridgeItf, g_real, g_real_bytes);
    __atomic_store_n(&g_log_on, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_log_pid, static_cast<int>(getpid()), __ATOMIC_RELAXED);
    NB_LOGI("cb=initialize pkg=%s not on the gate list: stock translator from here on pid=%d",
            pkg, static_cast<int>(getpid()));
    return g_real->initialize(runtime_cbs, private_dir, instruction_set);
  }
  const bool ok = g_real->initialize(runtime_cbs, private_dir, instruction_set);
  if (LogOn()) {
    NB_LOGI("cb=initialize/ret ok=%d pid=%d", ok ? 1 : 0, static_cast<int>(getpid()));
  }
  return ok;
}

bool ShimIsSupported(const char* libpath) {
  NB_TRACE("isSupported", g_n_is_supported, "libpath=%s", Str(libpath));
  if (!Ready() || g_real->isSupported == nullptr) return false;
  return g_real->isSupported(libpath);
}

bool ShimIsPathSupported(const char* library_path) {
  NB_TRACE("isPathSupported", g_n_is_path_supported, "library_path=%s", Str(library_path));
  if (!Ready() || g_real->isPathSupported == nullptr) return false;
  return g_real->isPathSupported(library_path);
}

void* ShimLoadLibrary(const char* libpath, int flag) {
  NB_TRACE("loadLibrary", g_n_load_library, "libpath=%s flag=0x%x", Str(libpath), flag);
  if (!Ready() || g_real->loadLibrary == nullptr) return nullptr;
  void* h = g_real->loadLibrary(libpath, flag);
  if (LogOn()) NB_LOGI("cb=loadLibrary/ret handle=%p libpath=%s", h, Str(libpath));
  return h;
}

void* ShimLoadLibraryExt(const char* libpath, int flag, void* ns) {
  NB_TRACE("loadLibraryExt", g_n_load_library_ext, "libpath=%s flag=0x%x ns=%p",
           Str(libpath), flag, ns);
  if (!Ready() || g_real->loadLibraryExt == nullptr) return nullptr;
  void* h = g_real->loadLibraryExt(libpath, flag, ns);
  if (LogOn()) NB_LOGI("cb=loadLibraryExt/ret handle=%p libpath=%s", h, Str(libpath));
  return h;
}

// libsigchain special handler for SIGSEGV: the companion gets the first look (it
// claims only its own native->guest fault), then the translator's own special
// handler if it has one.  Signal context: two atomic loads and calls, nothing else.
bool ShimSegvFirstChance(int sig, siginfo_t* info, void* uc) {
  const NativeBridgeSignalHandlerFn ours = __atomic_load_n(&g_companion_segv, __ATOMIC_ACQUIRE);
  if (ours != nullptr && ours(sig, info, uc)) return true;
  const NativeBridgeSignalHandlerFn real = __atomic_load_n(&g_real_segv, __ATOMIC_ACQUIRE);
  return real != nullptr && real(sig, info, uc);
}

// modavd.nb.firstchance=0 forwards the translator's answer unchanged, so the old
// sigaction()-only behaviour can be A/B-tested without a rebuild.  Default on.
bool FirstChanceOn() {
  char buf[PROP_VALUE_MAX] = {0};
  if (__system_property_get("modavd.nb.firstchance", buf) <= 0) return true;
  return !(strcmp(buf, "0") == 0 || strcmp(buf, "false") == 0 || strcmp(buf, "off") == 0);
}

// ART calls this for every signal when it sets the bridge up in an app process and
// registers each non-null answer with libsigchain's AddSpecialSignalHandlerFn.  Special
// handlers run before every handler installed with sigaction(), so this is the one
// place where the companion's private SIGSEGV trap cannot be pre-empted by a native
// library that installs a handler later (WebView's crashpad did exactly that and turned
// the trap into a ptrace-held main thread; see modavd_gate_segv_first_chance).
NativeBridgeSignalHandlerFn ShimGetSignalHandler(int signal) {
  NB_TRACE("getSignalHandler", g_n_get_signal_handler, "signal=%d", signal);
  if (!Ready() || g_real->getSignalHandler == nullptr) return nullptr;
  // Forward the POINTER only.  Never call it: it runs in signal context.
  NativeBridgeSignalHandlerFn fn = g_real->getSignalHandler(signal);
  if (signal == SIGSEGV && __atomic_load_n(&g_companion_segv, __ATOMIC_ACQUIRE) != nullptr &&
      FirstChanceOn()) {
    __atomic_store_n(&g_real_segv, fn, __ATOMIC_RELEASE);
    NB_LOGI("cb=getSignalHandler SIGSEGV -> first-chance companion handler in front of the "
            "translator's (%p) pid=%d",
            reinterpret_cast<void*>(fn), static_cast<int>(getpid()));
    fn = ShimSegvFirstChance;
  }
  if (LogOn()) NB_LOGI("cb=getSignalHandler/ret signal=%d handler=%p", signal,
                       reinterpret_cast<void*>(fn));
  return fn;
}

void ShimPreZygoteFork() {
  // Runs in an app-zygote immediately before fork().  Keep it allocation-free and
  // fd-free; the trace macro only reads /proc/self/cmdline and closes the fd.
  NB_TRACE("preZygoteFork", g_n_pre_zygote_fork, "ppid=%d", static_cast<int>(getppid()));
  if (!Ready() || g_real->preZygoteFork == nullptr) return;
  g_real->preZygoteFork();
}

const void* ShimGetAppEnv(const char* instruction_set) {
  NB_TRACE("getAppEnv", g_n_get_app_env, "isa=%s", Str(instruction_set));
  if (!Ready() || g_real->getAppEnv == nullptr) return nullptr;
  const void* env = g_real->getAppEnv(instruction_set);
  if (LogOn()) NB_LOGI("cb=getAppEnv/ret values=%p isa=%s", env, Str(instruction_set));
  return env;
}

// ---------------------------------------------------------------------------
// Inert stubs for the slots we do NOT wrap.
//
// In the normal path the constructor's memcpy replaces every one of these with the
// real translator's pointer, so they never run. They exist only for the inert path
// (real translator missing): then the exported struct keeps version 0, which makes
// libnativebridge reject the bridge in LoadNativeBridge() before it calls anything
// — but if some libnativebridge variant ignored version 0, a struct full of null
// pointers would be a null call (i.e. a crash in zygote) instead of a clean "not
// supported". A stub that returns nothing is strictly safer than a null pointer in
// the boot path.
// ---------------------------------------------------------------------------

void* InertGetTrampoline(void*, const char*, const char*, uint32_t) { return nullptr; }
bool InertIsCompatibleWith(uint32_t) { return false; }
int InertUnloadLibrary(void*) { return 0; }
const char* InertGetError() { return "modavd nbshim: no real translator"; }
bool InertInitAnonymousNamespace(const char*, const char*) { return false; }
void* InertCreateNamespace(const char*, const char*, const char*, uint64_t, const char*,
                           void*) {
  return nullptr;
}
bool InertLinkNamespaces(void*, void*, const char*) { return false; }
void* InertGetVendorNamespace() { return nullptr; }
void* InertGetExportedNamespace(const char*) { return nullptr; }
void* InertGetTrampolineWithJNICallType(void*, const char*, const char*, uint32_t,
                                        JNICallTypeInt) {
  return nullptr;
}
void* InertGetTrampolineForFunctionPointer(const void*, const char*, uint32_t,
                                           JNICallTypeInt) {
  return nullptr;
}
bool InertIsNativeBridgeFunctionPointer(const void*) { return false; }

}  // namespace

// ---------------------------------------------------------------------------
// The exported interface.
//
// Statically initialized with our own thunks and version 0, so the object is
// never a field of null pointers even if the constructor below never runs or
// fails.  The constructor then memcpy()s the real struct over it (which picks up
// the real version and all 20 real pointers) and re-installs our 8 thunks.
//
// It must live in writable .data (the constructor writes it), so it is not const.
// -fvisibility=hidden is on, hence the explicit default visibility: libnativebridge
// finds it with dlsym(handle, "NativeBridgeItf").
// ---------------------------------------------------------------------------
extern "C" __attribute__((visibility("default"))) NativeBridgeCallbacks NativeBridgeItf = {
    /* version                        */ 0,
    /* initialize                     */ ShimInitialize,
    /* loadLibrary                    */ ShimLoadLibrary,
    /* getTrampoline                  */ InertGetTrampoline,
    /* isSupported                    */ ShimIsSupported,
    /* getAppEnv                      */ ShimGetAppEnv,
    /* isCompatibleWith               */ InertIsCompatibleWith,
    /* getSignalHandler               */ ShimGetSignalHandler,
    /* unloadLibrary                  */ InertUnloadLibrary,
    /* getError                       */ InertGetError,
    /* isPathSupported                */ ShimIsPathSupported,
    /* unused_initAnonymousNamespace  */ InertInitAnonymousNamespace,
    /* createNamespace                */ InertCreateNamespace,
    /* linkNamespaces                 */ InertLinkNamespaces,
    /* loadLibraryExt                 */ ShimLoadLibraryExt,
    /* getVendorNamespace             */ InertGetVendorNamespace,
    /* getExportedNamespace           */ InertGetExportedNamespace,
    /* preZygoteFork                  */ ShimPreZygoteFork,
    /* getTrampolineWithJNICallType   */ InertGetTrampolineWithJNICallType,
    /* getTrampolineForFunctionPointer*/ InertGetTrampolineForFunctionPointer,
    /* isNativeBridgeFunctionPointer  */ InertIsNativeBridgeFunctionPointer,
};

namespace {

// Re-install our thunks over the slots the memcpy just overwrote with the real
// translator's pointers.  Everything NOT listed here stays byte-identical to the
// real struct, including the hot trampoline slots.
void InstallWrappers() {
  NativeBridgeItf.initialize = ShimInitialize;
  NativeBridgeItf.loadLibrary = ShimLoadLibrary;
  NativeBridgeItf.isSupported = ShimIsSupported;
  NativeBridgeItf.getAppEnv = ShimGetAppEnv;
  NativeBridgeItf.getSignalHandler = ShimGetSignalHandler;
  NativeBridgeItf.isPathSupported = ShimIsPathSupported;
  NativeBridgeItf.loadLibraryExt = ShimLoadLibraryExt;
  NativeBridgeItf.preZygoteFork = ShimPreZygoteFork;
  // NOT wrapped, deliberately: isCompatibleWith (libnativebridge calls it during
  // LoadNativeBridge, before anything else — forwarding the real pointer adds no
  // risk at the most fragile moment), getTrampoline*, unloadLibrary, getError,
  // createNamespace, linkNamespaces, getVendorNamespace, getExportedNamespace,
  // unused_initAnonymousNamespace, isNativeBridgeFunctionPointer.
}

// ---------------------------------------------------------------------------
// Constructor.
//
// WHY A CONSTRUCTOR, and why it is the only option: libnativebridge's
// LoadNativeBridge() does android_dlopen_ext() then dlsym("NativeBridgeItf") and
// then immediately reads callbacks->version and calls callbacks->isCompatibleWith
// (with NAMESPACE_VERSION = 3).  version is a plain data field, so there is no
// lazy path that could fill it in on first read — it has to be correct before
// dlsym() returns.  An ELF constructor runs inside android_dlopen_ext(), i.e.
// before dlsym() is even called, so it is the latest point that is still early
// enough and the earliest point at which we can dlopen() anything.
//
// Bionic's loader lock (g_dl_mutex) is a recursive mutex, so calling dlopen() from
// .init_array is supported.
//
// Nothing here starts a thread, and the only fd it opens (for the process name) is
// closed before it returns — both matter because this runs in zygote, whose fork
// checks kill the process over a stray descriptor.
// ---------------------------------------------------------------------------
__attribute__((constructor)) void ShimCtor() {
  char proc[192];
  ProcName(proc, sizeof(proc));

  char real_path[PROP_VALUE_MAX] = {0};
  if (__system_property_get("modavd.nb.real", real_path) <= 0 || real_path[0] == '\0') {
    strncpy(real_path, "libndk_translation.so", sizeof(real_path) - 1);
    real_path[sizeof(real_path) - 1] = '\0';
  }

  // Deliberately no __DATE__/__TIME__: this project wants reproducible artifact
  // hashes, and the md5 recorded in README.md already identifies the build.
  NB_LOGI("shim-ctor pid=%d tid=%d proc=%s real=%s itf=%p",
          static_cast<int>(getpid()), static_cast<int>(Tid()), proc, real_path,
          static_cast<void*>(&NativeBridgeItf));

  // We were loaded into the "system" linker namespace by libnativebridge, so a
  // plain dlopen() resolves against that namespace — the same one the translator
  // used to be loaded into.  RTLD_LAZY matches what libnativebridge itself used.
  void* handle = dlopen(real_path, RTLD_LAZY);
  if (handle == nullptr) {
    NB_LOGF("shim-ctor FAILED: dlopen(%s) -> %s", real_path, Str(dlerror()));
    NB_LOGF("shim-ctor INERT: version stays 0, libnativebridge will reject this "
            "bridge and the device boots with NO arm64 support");
    return;
  }

  void* sym = dlsym(handle, "NativeBridgeItf");
  if (sym == nullptr) {
    NB_LOGF("shim-ctor FAILED: %s has no NativeBridgeItf -> %s", real_path,
            Str(dlerror()));
    NB_LOGF("shim-ctor INERT: version stays 0, libnativebridge will reject this "
            "bridge and the device boots with NO arm64 support");
    // Deliberately not dlclose()d: the translator's constructors have already run
    // and may have installed signal handlers.  Unmapping it now would leave those
    // handlers pointing at nothing.
    return;
  }

  const auto* real = static_cast<const NativeBridgeCallbacks*>(sym);
  const uint32_t version = real->version;
  const size_t copy_bytes = RealStructBytesForVersion(version);

  NB_LOGI("shim-ctor real itf=%p version=%u copy_bytes=%zu", sym,
          static_cast<unsigned>(version), copy_bytes);

  if (version < kMinSupportedVersion || copy_bytes == 0) {
    NB_LOGF("shim-ctor FAILED: real translator reports native bridge version %u; "
            "this shim only models v%u and up",
            static_cast<unsigned>(version), static_cast<unsigned>(kMinSupportedVersion));
    NB_LOGF("shim-ctor INERT: version stays 0, libnativebridge will reject this "
            "bridge and the device boots with NO arm64 support");
    return;
  }

  // Copy the real struct VERBATIM (only as many bytes as its version guarantees
  // exist), then put our thunks back.  Order matters: the struct must be complete
  // before anybody can read it, and the release store below pairs with the acquire
  // load in Ready().
  g_real_handle = handle;
  g_real = real;
  g_real_bytes = copy_bytes;
  memcpy(&NativeBridgeItf, real, copy_bytes);
  InstallWrappers();
  __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);

  NB_LOGI("shim-ctor OK version=%u wrapped=initialize,loadLibrary,loadLibraryExt,"
          "isSupported,isPathSupported,getAppEnv,getSignalHandler,preZygoteFork",
          static_cast<unsigned>(NativeBridgeItf.version));

  // PHASE 2 — bring up the execution-bridge companion.
  //
  // This is what replaces the old injection method (LIEF-patching a DT_NEEDED into
  // Google's translator). Loading it HERE is what makes the two equivalent: the
  // companion's constructor patches the translator's own import GOT, so it must
  // run with the translator already mapped, in zygote, before any app is
  // specialised — which is exactly where we are. From the companion's point of
  // view nothing has changed; from the product's, no Google binary is modified any
  // more, there is no build-id to track, and the whole thing is redistributable.
  //
  // Failure is deliberately non-fatal: the bridge keeps working, the device keeps
  // arm64, and the only loss is the companion's gate — i.e. stock behaviour, which
  // is what an unset modavd.gate.target gives anyway.
  char companion[PROP_VALUE_MAX] = {0};
  if (__system_property_get("modavd.nb.companion", companion) <= 0 || companion[0] == '\0') {
    strncpy(companion, "libmodavd_gate.so", sizeof(companion) - 1);
  }
  if (strcmp(companion, "0") == 0 || strcmp(companion, "off") == 0) {
    NB_LOGI("shim-ctor companion disabled by modavd.nb.companion");
    return;
  }
  // Never dlclose()d, for the same reason as the translator: its constructor
  // installs GOT wrappers and (later, per app) a signal handler.
  void* ch = dlopen(companion, RTLD_NOW);
  if (ch == nullptr) {
    NB_LOGF("shim-ctor companion dlopen(%s) failed -> %s (bridge still up, gate absent)",
            companion, Str(dlerror()));
  } else {
    // Optional export: an older companion without it keeps the sigaction()-only path.
    const auto fc =
        reinterpret_cast<NativeBridgeSignalHandlerFn>(dlsym(ch, "modavd_gate_segv_first_chance"));
    __atomic_store_n(&g_companion_segv, fc, __ATOMIC_RELEASE);
    // Optional export too: an older companion without it leaves every app on the shim.
    const auto dt = reinterpret_cast<GateDetachFn>(dlsym(ch, "modavd_gate_detach_unless_target"));
    __atomic_store_n(&g_companion_detach, dt, __ATOMIC_RELEASE);
    NB_LOGI("shim-ctor companion %s loaded handle=%p first-chance=%p detach=%p", companion, ch,
            reinterpret_cast<void*>(fc), reinterpret_cast<void*>(dt));
  }
}

}  // namespace
