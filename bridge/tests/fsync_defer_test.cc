/* Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later. */
// Native test for the #35 fsync-deferral module. Compiles the real implementation
// directly and drives its internals with a counting fake in place of libc fsync, so
// no translator, no game and no GOT patching are needed. Run on a test instance:
//   clang++ --target=x86_64-linux-android30 -std=c++17 -O2 -Wall -Wextra -Werror \
//       -fno-exceptions -fno-rtti -static-libstdc++ bridge/tests/fsync_defer_test.cc \
//       -llog -o fsync-defer-test  &&  adb push ... && adb shell .../fsync-defer-test
//
// Covers: path-pattern and package matching, classify + fd-reuse revalidation,
// observe never defers, active defers within 5 s and pays back on the age cap, on
// close, and on rename of an open writer; non-matching fds always fsync for real.
#include "../companion/jni/fsync_defer.cc"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

namespace {
int failures = 0;
#define CHECK(x)                                           \
  do {                                                     \
    if (!(x)) {                                            \
      std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); \
      ++failures;                                          \
    }                                                      \
  } while (0)

int g_fake_fsync_calls = 0;
int g_fake_close_calls = 0;
int FakeFsync(int) {
  ++g_fake_fsync_calls;
  return 0;
}
int FakeClose(int fd) {
  ++g_fake_close_calls;
  return close(fd);
}

// mkdir -p for a directory tree under the test's scratch dir.
void MakeDirs(const std::string& path) {
  for (size_t i = 1; i < path.size(); ++i) {
    if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
  }
  mkdir(path.c_str(), 0755);
}

int OpenUnder(const std::string& dir, const std::string& rel) {
  const std::string full = dir + "/" + rel;
  const size_t slash = full.find_last_of('/');
  MakeDirs(full.substr(0, slash));
  const int fd = open(full.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) std::fprintf(stderr, "open(%s) errno=%d\n", full.c_str(), errno);
  return fd;
}

// Reset the module's runtime state between cases.
void SetUp(int mode) {
  g_mode = mode;
  g_fake_fsync_calls = 0;
  g_fake_close_calls = 0;
  g_real_fsync.store(&FakeFsync);
  g_real_fdatasync.store(&FakeFsync);
  g_real_close.store(&FakeClose);
  ParsePatterns(kDefaultPaths);
  if (g_fds == nullptr) {
    g_fd_cap = 4096;
    g_fds = new FdEntry[g_fd_cap];
  }
  for (int i = 0; i < g_fd_cap; ++i) {
    g_fds[i].state.store(kUnknown);
    g_fds[i].owed = false;
    g_fds[i].last_real_ns = 0;
  }
  for (int i = 0; i < kMaxMatched; ++i) g_matched[i].fd = -1;
  g_c_skipped = g_c_paid = g_c_real = g_c_other = g_c_renames = 0;
  g_last_summary_ns = NowNs();
}
}  // namespace

int main() {
  char tmpl[] = "/data/local/tmp/fsdeferXXXXXX";
  const char* base = mkdtemp(tmpl);
  if (base == nullptr) {
    base = mkdtemp(strcpy(new char[64], "/tmp/fsdeferXXXXXX"));
  }
  CHECK(base != nullptr);
  const std::string dir = base;
  const std::string match_rel = "files/HGGameUpdate/HGDownload/file_1/download/abc.downloading";
  const std::string other_rel = "files/other.dat";

  // --- pure matching ---------------------------------------------------------
  ParsePatterns(kDefaultPaths);
  CHECK(PathMatches("/x/y/files/HGGameUpdate/HGDownload/file_9/download/u.downloading"));
  CHECK(!PathMatches("/x/y/files/HGGameUpdate/HGDownload/file_9/download/u.dat"));
  CHECK(!PathMatches("/x/y/Bundles/persistent_res_list.json"));
  CHECK(PackageOnList("com.hypergryph.arknights", "com.hypergryph.arknights"));
  CHECK(PackageOnList("com.hypergryph.arknights", "com.hypergryph.arknights:push"));
  CHECK(!PackageOnList("com.hypergryph.arknights", "com.netease.onmyoji"));
  CHECK(!PackageOnList("a.b,c.d", "e.f"));

  // --- observe mode never defers --------------------------------------------
  SetUp(kObserve);
  {
    const int fd = OpenUnder(dir, match_rel);
    CHECK(fd >= 0);
    CHECK(WrapFsync(fd) == 0);
    CHECK(WrapFsync(fd) == 0);
    CHECK(WrapFsync(fd) == 0);
    CHECK(g_fds[fd].state.load() == kMatched);  // classified as a download temp
    CHECK(g_fake_fsync_calls == 3);             // but all three ran for real
    CHECK(g_c_skipped.load() == 0);
    CHECK(g_c_real.load() == 3);
    WrapClose(fd);
    CHECK(g_fds[fd].state.load() == kUnknown);  // forgotten on close
  }

  // --- active mode defers within 5 s, real on first and on age cap ----------
  SetUp(kActive);
  int active_fd = -1;
  {
    const int fd = OpenUnder(dir, match_rel);
    CHECK(fd >= 0);
    active_fd = fd;
    CHECK(WrapFsync(fd) == 0);         // first: stale (last==0) -> real
    CHECK(g_fake_fsync_calls == 1);
    CHECK(WrapFsync(fd) == 0);         // within 5 s -> deferred
    CHECK(WrapFsync(fd) == 0);         // still deferred
    CHECK(g_fake_fsync_calls == 1);
    CHECK(g_c_skipped.load() == 2);
    CHECK(g_fds[fd].owed);
    // Age the last real fsync past the cap: next call must pay.
    g_fds[fd].last_real_ns = NowNs() - (kPaybackAgeNs + 1);
    CHECK(WrapFsync(fd) == 0);
    CHECK(g_fake_fsync_calls == 2);
    CHECK(!g_fds[fd].owed);
  }

  // --- payback on close ------------------------------------------------------
  {
    const int fd = active_fd;
    g_fake_fsync_calls = 0;
    CHECK(WrapFsync(fd) == 0);  // deferred again (recent real above)
    CHECK(g_fds[fd].owed);
    CHECK(g_fake_fsync_calls == 0);
    WrapClose(fd);
    CHECK(g_fake_fsync_calls == 1);  // paid back before the real close
    CHECK(g_fake_close_calls >= 1);
  }

  // --- non-matching fd always real ------------------------------------------
  SetUp(kActive);
  {
    const int fd = OpenUnder(dir, other_rel);
    CHECK(fd >= 0);
    CHECK(WrapFsync(fd) == 0);
    CHECK(WrapFsync(fd) == 0);
    CHECK(g_fds[fd].state.load() == kUnmatched);
    CHECK(g_fake_fsync_calls == 2);
    CHECK(g_c_other.load() == 2);
    CHECK(g_c_skipped.load() == 0);
    WrapClose(fd);
  }

  // --- payback on rename of an open writer ----------------------------------
  SetUp(kActive);
  {
    const int fd = OpenUnder(dir, match_rel);
    CHECK(fd >= 0);
    CHECK(WrapFsync(fd) == 0);  // first real
    CHECK(WrapFsync(fd) == 0);  // deferred, owed
    CHECK(g_fds[fd].owed);
    const long before = g_fake_fsync_calls;
    char linkpath[512];
    CHECK(FdPath(fd, linkpath, sizeof(linkpath)) >= 0);
    HandleRenameSource(linkpath);  // source still open -> pay back
    CHECK(g_fake_fsync_calls == before + 1);
    CHECK(!g_fds[fd].owed);
    CHECK(g_c_renames.load() == 1);
    WrapClose(fd);
  }

  // --- fd reuse revalidation: a matched entry whose file changed reclassifies -
  SetUp(kActive);
  {
    const int fd = OpenUnder(dir, match_rel);
    CHECK(fd >= 0);
    CHECK(WrapFsync(fd) == 0);
    CHECK(g_fds[fd].state.load() == kMatched);
    // Simulate reuse: same fd number, different file (clear our record, reopen a
    // non-matching path onto a fresh fd, then force the classifier to re-stat).
    g_fds[fd].dev = 0;
    g_fds[fd].ino = 0;  // make the cached identity impossible to match
    CHECK(Classify(fd, nullptr, 0) == kMatched);  // path still matches -> matched
    close(fd);
    const int fd2 = OpenUnder(dir, other_rel);
    CHECK(fd2 >= 0);
    CHECK(Classify(fd2, nullptr, 0) == kUnmatched);
    close(fd2);
  }

  if (failures == 0)
    std::printf("ALL PASS\n");
  else
    std::printf("%d FAILURE(S)\n", failures);
  return failures == 0 ? 0 : 1;
}
