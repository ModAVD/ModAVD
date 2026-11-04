/* Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later. */
// Native test for the #35 fsync-deferral module (design "B": seccomp + USER_NOTIF). It
// compiles the real implementation directly and drives its internals — Classify,
// ShouldDefer/DecideDefer and PaybackSlotIfDue — with real files, WITHOUT installing a
// seccomp filter (the deferral is a kernel verdict in production; here we check the
// decision logic and the payback I/O). No translator, no game. Run on a test instance:
//   clang++ --target=x86_64-linux-android30 -std=c++17 -O2 -Wall -Wextra -Werror \
//       -fno-exceptions -fno-rtti -static-libstdc++ bridge/tests/fsync_defer_test.cc \
//       -llog -o fsync-defer-test  &&  adb push ... && adb shell .../fsync-defer-test
//
// Covers: path-pattern and package matching; classify + fd-reuse revalidation; observe
// never defers; active defers an already-classified matched fd but continues the first
// (unknown) fsync, an unmatched fd, a foreign (forked-child) pid, and an fd whose file
// changed; payback pays a dirty slot once per cap, is a no-op when paid recently, and
// retires a renamed/deleted file.
#include "../companion/jni/fsync_defer.cc"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
int failures = 0;
#define CHECK(x)                                           \
  do {                                                     \
    if (!(x)) {                                            \
      std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); \
      ++failures;                                          \
    }                                                      \
  } while (0)

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

pid_t MyTid() { return static_cast<pid_t>(syscall(__NR_gettid)); }

// A notification as the supervisor would receive it: fsync on fd, from thread `pid`.
struct seccomp_notif MakeReq(int fd, pid_t pid, uint32_t nr = kNrFsync) {
  struct seccomp_notif req;
  memset(&req, 0, sizeof(req));
  req.pid = static_cast<uint32_t>(pid);
  req.data.nr = static_cast<int>(nr);
  req.data.args[0] = static_cast<uint64_t>(static_cast<uint32_t>(fd));
  return req;
}

// Reset the module's runtime state between cases (no filter is ever installed).
void SetUp(int mode) {
  g_mode = mode;
  g_pattern_count = ParseList(kDefaultPaths, g_patterns);
  if (g_fds == nullptr) {
    g_fd_cap = 4096;
    g_fds = new FdEntry[g_fd_cap];
  }
  for (int i = 0; i < g_fd_cap; ++i) {
    g_fds[i].state.store(kUnknown);
    g_fds[i].dev.store(0);
    g_fds[i].ino.store(0);
    g_fds[i].matched_idx.store(-1);
  }
  for (int i = 0; i < kMaxMatched; ++i) {
    g_matched[i].fd = -1;
    g_matched[i].path[0] = '\0';
    g_matched[i].deferred_ns.store(0);
    g_matched[i].paid_ns.store(0);
  }
  g_c_recv.store(0);
  g_c_real.store(0);
  g_c_other.store(0);
  g_c_foreign.store(0);
  g_c_deferred.store(0);
  g_c_payback.store(0);
  g_c_payback_err.store(0);
}
}  // namespace

int main() {
  char tmpl[] = "/data/local/tmp/fsdeferXXXXXX";
  const char* base = mkdtemp(tmpl);
  if (base == nullptr) base = mkdtemp(strcpy(new char[64], "/tmp/fsdeferXXXXXX"));
  CHECK(base != nullptr);
  const std::string dir = base;
  const std::string match_rel = "files/HGGameUpdate/HGDownload/file_1/download/abc.downloading";
  const std::string other_rel = "files/other.dat";
  const pid_t me = MyTid();

  // --- pure matching ---------------------------------------------------------
  g_pattern_count = ParseList(kDefaultPaths, g_patterns);
  CHECK(PathMatches("/x/y/files/HGGameUpdate/HGDownload/file_9/download/u.downloading"));
  CHECK(!PathMatches("/x/y/files/HGGameUpdate/HGDownload/file_9/download/u.dat"));
  CHECK(!PathMatches("/x/y/Bundles/persistent_res_list.json"));
  CHECK(PackageOnList("com.hypergryph.arknights", "com.hypergryph.arknights"));
  CHECK(PackageOnList("com.hypergryph.arknights", "com.hypergryph.arknights:push"));
  CHECK(!PackageOnList("com.hypergryph.arknights", "com.netease.onmyoji"));
  CHECK(!PackageOnList("a.b,c.d", "e.f"));

  // --- classify + fd-reuse revalidation --------------------------------------
  SetUp(kActive);
  {
    const int fd = OpenUnder(dir, match_rel);
    CHECK(fd >= 0);
    CHECK(Classify(fd, nullptr, 0) == kMatched);
    CHECK(g_fds[fd].state.load() == kMatched);
    CHECK(g_fds[fd].matched_idx.load() >= 0);
    // A matched fd whose cached identity is wiped still reclassifies by path.
    g_fds[fd].dev.store(0);
    g_fds[fd].ino.store(0);
    CHECK(Classify(fd, nullptr, 0) == kMatched);
    close(fd);
    const int fd2 = OpenUnder(dir, other_rel);
    CHECK(fd2 >= 0);
    CHECK(Classify(fd2, nullptr, 0) == kUnmatched);
    CHECK(g_fds[fd2].matched_idx.load() == -1);
    close(fd2);
  }

  // --- observe never defers (even a classified matched fd) -------------------
  SetUp(kObserve);
  {
    const int fd = OpenUnder(dir, match_rel);
    CHECK(fd >= 0);
    CHECK(Classify(fd, nullptr, 0) == kMatched);
    const struct seccomp_notif req = MakeReq(fd, me);
    CHECK(!ShouldDefer(&req));  // observe: always continue
    close(fd);
  }

  // --- active: first (unknown) fsync continues, later ones defer --------------
  SetUp(kActive);
  {
    const int fd = OpenUnder(dir, match_rel);
    CHECK(fd >= 0);
    const struct seccomp_notif req = MakeReq(fd, me);
    CHECK(!ShouldDefer(&req));            // not classified yet -> continue (real)
    CHECK(Classify(fd, nullptr, 0) == kMatched);  // AccountFsync would do this post-SEND
    CHECK(ShouldDefer(&req));             // now matched -> defer
    const int idx = g_fds[fd].matched_idx.load();
    CHECK(idx >= 0);
    CHECK(g_matched[idx].deferred_ns.load() > 0);  // stamped for payback
    CHECK(ShouldDefer(&req));             // and again
    close(fd);
  }

  // --- active continues: unmatched fd, foreign pid, changed file -------------
  SetUp(kActive);
  {
    const int fd = OpenUnder(dir, other_rel);
    CHECK(fd >= 0);
    CHECK(Classify(fd, nullptr, 0) == kUnmatched);
    const struct seccomp_notif req = MakeReq(fd, me);
    CHECK(!ShouldDefer(&req));            // unmatched -> continue
    close(fd);

    const int mfd = OpenUnder(dir, match_rel);
    CHECK(mfd >= 0);
    CHECK(Classify(mfd, nullptr, 0) == kMatched);
    // Foreign thread id (not under /proc/self/task): never defer.
    const struct seccomp_notif foreign = MakeReq(mfd, 0x7fffffff);
    CHECK(!ShouldDefer(&foreign));
    // fd-number reuse: cached matched, but the fd now fstats to a different inode.
    const struct seccomp_notif req2 = MakeReq(mfd, me);
    g_fds[mfd].ino.store(g_fds[mfd].ino.load() ^ 0x1);  // corrupt cached identity
    CHECK(!ShouldDefer(&req2));           // dev/ino mismatch -> continue
    close(mfd);
  }

  // --- payback: due once per cap, no-op when recent, retire a gone file -------
  SetUp(kActive);
  {
    const int fd = OpenUnder(dir, match_rel);
    CHECK(fd >= 0);
    CHECK(Classify(fd, nullptr, 0) == kMatched);
    const int idx = g_fds[fd].matched_idx.load();
    CHECK(idx >= 0);
    const long now = NowNs();
    g_matched[idx].deferred_ns.store(now);   // one unpaid deferred fsync
    g_matched[idx].paid_ns.store(0);
    CHECK(PaybackSlotIfDue(idx, now));       // paid=0 -> due
    CHECK(g_c_payback.load() == 1);          // real fsync ran on the file
    CHECK(g_c_payback_err.load() == 0);
    CHECK(!PaybackSlotIfDue(idx, now));      // deferred <= paid now -> nothing unpaid
    // New defer, but paid too recently -> not due.
    g_matched[idx].deferred_ns.store(now + 1);
    CHECK(!PaybackSlotIfDue(idx, now + 1));
    // Age past the cap -> due again.
    CHECK(PaybackSlotIfDue(idx, now + kPaybackEveryNs + 1));
    CHECK(g_c_payback.load() == 2);
    // Renamed/deleted: open fails -> counted as an error and retired.
    const std::string full = dir + "/" + match_rel;
    CHECK(unlink(full.c_str()) == 0);
    g_matched[idx].deferred_ns.store(NowNs());
    g_matched[idx].paid_ns.store(0);
    CHECK(PaybackSlotIfDue(idx, NowNs()));
    CHECK(g_c_payback_err.load() == 1);
    CHECK(g_c_payback.load() == 2);          // unchanged
    close(fd);
  }

  if (failures == 0)
    std::printf("ALL PASS\n");
  else
    std::printf("%d FAILURE(S)\n", failures);
  return failures == 0 ? 0 : 1;
}
