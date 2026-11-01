// Copyright 2026 Ji Mao Cai
// Own ARM64 JNI fixture: never load into a game or return to ART in a child.
#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <initializer_list>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {
volatile sig_atomic_t prepares = 0, parents = 0, children = 0;
pthread_once_t registration_once = PTHREAD_ONCE_INIT;
int registration_result = -1;
void Prepare() { prepares = prepares + 1; }
void Parent() { parents = parents + 1; }
void Child() { children = children + 1; }
void Register() { registration_result = pthread_atfork(Prepare, Parent, Child); }
int64_t Millis() {
  timespec time{};
  clock_gettime(CLOCK_MONOTONIC, &time);
  return static_cast<int64_t>(time.tv_sec) * 1000 + time.tv_nsec / 1000000;
}
struct Marker { int magic, pid, tid, libc_pid, libc_tid, child_callback, child_tid; };
struct TidPages {
  size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  void* mapping = mmap(nullptr, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  int* values = nullptr;
  TidPages() {
    if (mapping != MAP_FAILED &&
        mprotect(static_cast<char*>(mapping) + page, page, PROT_READ | PROT_WRITE) == 0) {
      values = reinterpret_cast<int*>(static_cast<char*>(mapping) + page);
      values[0] = values[1] = -1;
    }
  }
  ~TidPages() { if (mapping != MAP_FAILED) munmap(mapping, page * 3); }
};

enum class Kind { kImportedFork, kRawChildTid, kRawBothTid };
const char* Name(Kind kind) {
  return kind == Kind::kImportedFork ? "imported-libc-fork" :
         kind == Kind::kRawChildTid ? "raw-bionic-flags" : "raw-parent-child-settid";
}
int Case(Kind kind) {
  const bool raw_tid = kind != Kind::kImportedFork;
  const bool parent_settid = kind == Kind::kRawBothTid;
  int descriptors[2];
  if (pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) != 0) return 1;
  TidPages tid;
  if (!tid.values) { close(descriptors[0]); close(descriptors[1]); return 9; }
  int& parent_tid = tid.values[0];
  int& child_tid = tid.values[1];
  const pid_t parent_pid = getpid(), parent_thread = gettid();
  const int prepare_before = prepares, parent_before = parents;
  const long flags = CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | SIGCHLD |
                     (parent_settid ? CLONE_PARENT_SETTID : 0);
  // aarch64 clone order: flags, stack, ptid, tls, ctid. libc fork is a separate case.
  const pid_t child = raw_tid ? static_cast<pid_t>(syscall(SYS_clone, flags, 0L,
                                      parent_settid ? reinterpret_cast<long>(&parent_tid) : 0L,
                                      0L, &child_tid)) : fork();
  if (child == 0) {
    syscall(SYS_close, descriptors[0]);
    Marker marker{0x47464b31, static_cast<int>(syscall(SYS_getpid)),
                  static_cast<int>(syscall(SYS_gettid)), 0, 0,
                  static_cast<int>(children), child_tid};
    if (!raw_tid) { marker.libc_pid = getpid(); marker.libc_tid = gettid(); }
    const long written = syscall(SYS_write, descriptors[1], &marker, sizeof(marker));
    syscall(SYS_close, descriptors[1]);
    syscall(SYS_exit_group, written == static_cast<long>(sizeof(marker)) ? 0 : 93);
    __builtin_unreachable();
  }
  close(descriptors[1]);
  if (child < 0) { close(descriptors[0]); return 2; }
  pollfd waiting{descriptors[0], POLLIN, 0};
  int polled = 0;
  const int64_t poll_deadline = Millis() + 5000;
  while (Millis() < poll_deadline) {
    const int remaining = static_cast<int>(poll_deadline - Millis());
    if (remaining <= 0) break;
    polled = poll(&waiting, 1, remaining);
    if (polled >= 0 || errno != EINTR) break;
  }
  Marker marker{};
  ssize_t received = -1;
  if (polled > 0) {
    do { received = read(descriptors[0], &marker, sizeof(marker)); }
    while (received < 0 && errno == EINTR && Millis() < poll_deadline);
  }
  close(descriptors[0]);
  int status = 0;
  pid_t reaped = 0;
  const int64_t wait_deadline = Millis() + 5000;
  while (Millis() < wait_deadline) {
    reaped = waitpid(child, &status, WNOHANG);
    if (reaped < 0 && errno == EINTR) { reaped = 0; continue; }
    if (reaped != 0) break;
    usleep(1000);
  }
  if (reaped == 0) {
    kill(child, SIGKILL);
    const int64_t reap_deadline = Millis() + 1000;
    while (Millis() < reap_deadline && waitpid(child, &status, WNOHANG) == 0) usleep(1000);
    return 3;
  }
  if (reaped != child || received != static_cast<ssize_t>(sizeof(marker)) || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 4;
  if (marker.magic != 0x47464b31 || marker.pid != child || marker.tid != child) return 5;
  if (getpid() != parent_pid || gettid() != parent_thread) return 6;
  if (raw_tid) {
    if (parent_tid != (parent_settid ? child : -1) || marker.child_tid != child || child_tid != -1 ||
        prepares != prepare_before || parents != parent_before || marker.child_callback != 0) return 7;
  } else if (marker.libc_pid != child || marker.libc_tid != child || marker.child_callback != 1 ||
             prepares != prepare_before + 1 || parents != parent_before + 1) return 8;
  return 0;
}
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_modavd_tests_guestfork_ProbeActivity_runProbe(JNIEnv* env, jclass) {
  prepares = parents = children = 0;
  pthread_once(&registration_once, Register);
  if (registration_result != 0) return env->NewStringUTF("FAIL atfork registration");
  Dl_info info{};
  const int located = dladdr(reinterpret_cast<void*>(&fork), &info);
  __android_log_print(ANDROID_LOG_INFO, "ModavdGuestForkProbe", "fork import: located=%d file=%s symbol=%s address=%p",
                      located, info.dli_fname ? info.dli_fname : "unknown",
                      info.dli_sname ? info.dli_sname : "unknown", reinterpret_cast<void*>(&fork));
  for (int round = 0; round < 3; ++round) {
    for (Kind kind : {Kind::kImportedFork, Kind::kRawChildTid, Kind::kRawBothTid}) {
      const int result = Case(kind);
      __android_log_print(ANDROID_LOG_INFO, "ModavdGuestForkProbe", "case=%s round=%d result=%d pid=%d",
                          Name(kind), round, result, getpid());
      if (result != 0) {
        char text[100];
        snprintf(text, sizeof(text), "FAIL case=%s round=%d code=%d", Name(kind), round, result);
        return env->NewStringUTF(text);
      }
    }
  }
  return env->NewStringUTF("PASS ARM64 imported fork and raw 1200011/1300011, 3 rounds each");
}
