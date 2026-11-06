/* Copyright 2026 jinghero. SPDX-License-Identifier: AGPL-3.0-or-later. */
#pragma once

#include <linux/seccomp.h>
#include <stdint.h>
#include <sys/types.h>

namespace modavd::download_listener {

struct Request {
  seccomp_notif notification;
  pid_t owner_pid;
  uint64_t epoch;
};

struct Decision {
  enum Action : uint32_t { kContinue, kResult };
  Action action;
  int64_t value;
  int error;  // Positive errno, or zero. The guardian negates it for the kernel.

  static Decision Continue() { return {kContinue, 0, 0}; }
  static Decision Result(int64_t value, int error = 0) { return {kResult, value, error}; }
};

using Callback = Decision (*)(const Request&, void*);
using Tick = void (*)(void*);

struct StartResult {
  int error;  // Positive errno, zero on success.
  bool filter_installed;  // True remains true if a later handshake fails.
  pid_t guardian_pid;
  pid_t worker_tid;
};

// Explicit, once-per-process activation. No Stop: seccomp cannot be removed.
// Pre-install fd inventory rejects inherited io_uring with ENOTSUP; scan errors
// fail admission with their errno. Call before game/library ring producers can
// run, and keep them quiescent until installation completes. One scan cannot
// certify arbitrary late app initialization with a concurrent SQPOLL producer.
// callback and context must remain valid until this process exits. Requests are
// serialized on one worker; that worker's own intercepted syscalls run normally.
// Every other TID, including fork descendants, is passed to callback. The callback
// must treat foreign fd tables/address spaces separately before dereferencing args.
// io_uring setup/enter/register are notified too. An adapter that caches fd
// identities must drain and permanently leave optimization before continuing
// them; never assume an inherited platform filter always rejects these calls.
// Timeout always returns ETIMEDOUT, never retries a possibly emulated syscall.
// WAIT_KILLABLE_RECV is mandatory. Unsupported kernels reject installation;
// no fallback permits nonfatal signals to cancel already-received mutations.
// Even error + filter_installed==false may leave NO_NEW_PRIVS enabled. If
// filter_installed==true and error!=0, the caller must terminate initialization.
// Optional tick uses the same context and worker, before each request and every
// 100ms while idle. It never supplies a kernel response. Calls are serialized;
// a running callback or tick can delay the next idle tick.
StartResult Start(Callback callback, void* context, unsigned timeout_ms = 10000, Tick tick = nullptr);

// False before Start, and permanently false after dispatched work times out,
// IPC fails, or a dispatched request disappears before its reply. Shared with
// the guardian across fork. Check on callback entry and before publishing new
// optimized state; an unhealthy adapter must drain and remain on its strict path.
bool Healthy();

}  // namespace modavd::download_listener
