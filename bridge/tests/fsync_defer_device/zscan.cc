/* Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later. */
// zscan <file>...: size, and every run of all-zero 4 KiB blocks (offset, length) -- in a compressed
// lpack (zip) long zero runs mean data that never reached the disk (a hole), not real content.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char** argv) {
  static char buf[4096];
  static const char zero[4096] = {};
  for (int a = 1; a < argc; ++a) {
    const int fd = open(argv[a], O_RDONLY);
    if (fd < 0) { printf("%s: cannot open\n", argv[a]); continue; }
    long long off = 0, run_start = -1, zero_blocks = 0, runs = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
      const bool z = (n == (ssize_t)sizeof(buf)) && memcmp(buf, zero, sizeof(buf)) == 0;
      if (z) { ++zero_blocks; if (run_start < 0) run_start = off; }
      if (!z && run_start >= 0) {
        if (off - run_start >= 16 * 4096) printf("  zero run at %lld len %lld\n", run_start, off - run_start);
        ++runs; run_start = -1;
      }
      off += n;
    }
    if (run_start >= 0) { printf("  zero run at %lld len %lld (to EOF)\n", run_start, off - run_start); ++runs; }
    printf("%s: size %lld, zero 4K blocks %lld in %lld runs\n", argv[a], off, zero_blocks, runs);
    close(fd);
  }
  return 0;
}
