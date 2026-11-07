/* Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later. */
// Print CLOCK_MONOTONIC and CLOCK_REALTIME (ms) every 50 ms for <secs> seconds, line-buffered, so the host
// can timestamp each line as it arrives and tell a real guest stall (both clocks and the host see the
// same gap) from a guest clock jump (the guest clocks leap while the host saw no gap).
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
static long long Ms(clockid_t c) { timespec t; clock_gettime(c, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  const long long end = Ms(CLOCK_MONOTONIC) + 1000LL * (argc > 1 ? atoi(argv[1]) : 120);
  while (Ms(CLOCK_MONOTONIC) < end) {
    printf("%lld %lld\n", Ms(CLOCK_MONOTONIC), Ms(CLOCK_REALTIME));
    usleep(50000);
  }
  return 0;
}
