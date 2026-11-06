/* Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later. */
/* ModAVD#96 hardening item 2 (EINTR), on-device probe. Not part of the product.
 *
 * Arms the REAL fsync-deferral module in this process (modavd_fsync_defer_init, same filter, same
 * supervisor), then keeps one worker thread fsyncing (or committing SQLite transactions) while a
 * second thread hits exactly that worker with SIGUSR1 every few microseconds. It counts what the
 * worker sees: EINTR from fsync/fdatasync, SQLITE_IOERR_* from SQLite. The commit text says: a
 * trapped fsync waits interruptibly, so a signal whose handler lacks SA_RESTART can turn it into
 * EINTR (a real ext4 fsync never does), and SQLite does not retry -> SQLITE_IOERR_FSYNC. With
 * SA_RESTART the kernel restarts the call transparently. Control: the same run without arming.
 *
 *   eintr-probe kernel                       uname + whether SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV
 *                                            (Linux 5.19+) is accepted
 *   eintr-probe <none|arm> <norestart|restart> <fsync|fsync-match|sqlite-wal|sqlite-delete>
 *               <iterations> <signal_gap_us> <scratch_dir>
 *
 * "arm" reads the module's normal properties (modavd.fsync.defer, .mode, .paths); the package name
 * passed is com.hypergryph.arknights, so the default list arms it. Built with -DPROBE_KILLABLE the
 * filter is installed with SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV added (the only change).
 */
#include <linux/seccomp.h>
#ifdef PROBE_KILLABLE
#undef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER ((1UL << 3) | (1UL << 5))
#endif
#include "fsync_defer.cc"

#include <dlfcn.h>
#include <signal.h>
#include <string>
#include <sys/utsname.h>
#include <sys/wait.h>

namespace probe {

std::atomic<bool> g_done{false};
std::atomic<pid_t> g_worker_tid{0};
std::atomic<uint64_t> g_sig_sent{0};
std::atomic<uint64_t> g_sig_handled{0};
long g_gap_us = 20;

void OnSig(int) { g_sig_handled.fetch_add(1, std::memory_order_relaxed); }

void* Signaller(void*) {
  while (g_worker_tid.load() == 0 && !g_done.load()) usleep(100);
  const pid_t pid = getpid();
  const pid_t tid = g_worker_tid.load();
  while (!g_done.load(std::memory_order_relaxed)) {
    if (syscall(__NR_tgkill, pid, tid, SIGUSR1) == 0) g_sig_sent.fetch_add(1, std::memory_order_relaxed);
    if (g_gap_us > 0) usleep(static_cast<useconds_t>(g_gap_us));
  }
  return nullptr;
}

// ---- SQLite through the platform library (dlopen; no NDK headers) -------------------
typedef struct sqlite3 sqlite3;
struct Sql {
  int (*open_v2)(const char*, sqlite3**, int, const char*);
  int (*exec)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
  int (*extended_errcode)(sqlite3*);
  int (*extended_result_codes)(sqlite3*, int);
  const char* (*errmsg)(sqlite3*);
  int (*close)(sqlite3*);
} sql;

bool LoadSqlite() {
  void* h = dlopen("libsqlite.so", RTLD_NOW);
  if (h == nullptr) h = dlopen("/system/lib64/libsqlite.so", RTLD_NOW);
  if (h == nullptr) {
    std::fprintf(stderr, "dlopen libsqlite.so: %s\n", dlerror());
    return false;
  }
  sql.open_v2 = reinterpret_cast<decltype(sql.open_v2)>(dlsym(h, "sqlite3_open_v2"));
  sql.exec = reinterpret_cast<decltype(sql.exec)>(dlsym(h, "sqlite3_exec"));
  sql.extended_errcode = reinterpret_cast<decltype(sql.extended_errcode)>(dlsym(h, "sqlite3_extended_errcode"));
  sql.extended_result_codes =
      reinterpret_cast<decltype(sql.extended_result_codes)>(dlsym(h, "sqlite3_extended_result_codes"));
  sql.errmsg = reinterpret_cast<decltype(sql.errmsg)>(dlsym(h, "sqlite3_errmsg"));
  sql.close = reinterpret_cast<decltype(sql.close)>(dlsym(h, "sqlite3_close"));
  return sql.open_v2 && sql.exec && sql.extended_errcode && sql.extended_result_codes && sql.errmsg && sql.close;
}

struct Result {
  uint64_t ok = 0, eintr = 0, other = 0;
  int other_code = 0;       // last non-EINTR errno / SQLite extended code
  uint64_t sql_ioerr_fsync = 0, sql_ioerr_other = 0, sql_other = 0;
  std::string first_msg;
};

Result g_res;
std::string g_workload, g_dir;
long g_iters = 0;

void* Worker(void*) {
  g_worker_tid.store(static_cast<pid_t>(syscall(__NR_gettid)));
  Result& r = g_res;
  if (g_workload == "fsync" || g_workload == "fsync-match") {
    std::string path = g_dir + "/probe.dat";
    if (g_workload == "fsync-match") {
      // a path the module classifies as a download temp file (default pattern)
      const std::string d = g_dir + "/files/HGGameUpdate/HGDownload/file_1/download";
      for (size_t i = 0; i < d.size(); ++i) {
        if (d[i] == '/' && i > 0) mkdir(d.substr(0, i).c_str(), 0755);
      }
      mkdir(d.c_str(), 0755);
      path = d + "/probe.downloading";
    }
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
      std::fprintf(stderr, "open %s errno=%d\n", path.c_str(), errno);
      g_done.store(true);
      return nullptr;
    }
    char buf[4096];
    memset(buf, 'x', sizeof(buf));
    for (long i = 0; i < g_iters; ++i) {
      if (pwrite(fd, buf, sizeof(buf), (i % 256) * 4096) < 0 && errno != EINTR) break;
      const int rc = (i & 1) ? fsync(fd) : fdatasync(fd);
      if (rc == 0) {
        ++r.ok;
      } else if (errno == EINTR) {
        ++r.eintr;
      } else {
        ++r.other;
        r.other_code = errno;
      }
    }
    close(fd);
  } else {
    sqlite3* db = nullptr;
    const std::string path = g_dir + "/probe.db";
    if (sql.open_v2(path.c_str(), &db, 0x2 | 0x4, nullptr) != 0) {
      std::fprintf(stderr, "sqlite open failed\n");
      g_done.store(true);
      return nullptr;
    }
    sql.extended_result_codes(db, 1);
    const char* jm = (g_workload == "sqlite-wal") ? "PRAGMA journal_mode=WAL;" : "PRAGMA journal_mode=DELETE;";
    sql.exec(db, jm, nullptr, nullptr, nullptr);
    sql.exec(db, "PRAGMA synchronous=FULL; CREATE TABLE IF NOT EXISTS t(x BLOB);", nullptr, nullptr, nullptr);
    for (long i = 0; i < g_iters; ++i) {
      const int rc = sql.exec(db, "BEGIN; INSERT INTO t VALUES(randomblob(512)); COMMIT;", nullptr, nullptr, nullptr);
      if (rc == 0) {
        ++r.ok;
        continue;
      }
      const int ext = sql.extended_errcode(db);
      if (ext == (10 | (4 << 8)) || ext == (10 | (5 << 8)))
        ++r.sql_ioerr_fsync;  // SQLITE_IOERR_FSYNC / SQLITE_IOERR_DIR_FSYNC
      else if ((ext & 0xff) == 10)
        ++r.sql_ioerr_other;
      else
        ++r.sql_other;
      r.other_code = ext;
      if (r.first_msg.empty()) r.first_msg = sql.errmsg(db);
      sql.exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    sql.exec(db, "DELETE FROM t;", nullptr, nullptr, nullptr);
    sql.close(db);
  }
  g_done.store(true);
  return nullptr;
}

int KernelReport() {
  struct utsname u;
  uname(&u);
  std::printf("kernel %s %s\n", u.release, u.version);
  // Probe in a child: a filter cannot be removed, so never in this process.
  auto try_flags = [](unsigned long flags) -> int {
    const pid_t c = fork();
    if (c == 0) {
      prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
      struct sock_filter allow[] = {BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
      struct sock_fprog prog = {1, allow};
      const long ret = syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, flags, &prog);
      _exit(ret >= 0 ? 0 : (errno & 0x7f));
    }
    int st = 0;
    waitpid(c, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  };
  const int listener = try_flags(1UL << 3);
  const int killable = try_flags((1UL << 3) | (1UL << 5));
  std::printf("seccomp NEW_LISTENER: %s\n", listener == 0 ? "accepted" : strerror(listener));
  std::printf("seccomp NEW_LISTENER|WAIT_KILLABLE_RECV: %s\n", killable == 0 ? "accepted" : strerror(killable));
  return 0;
}

}  // namespace probe

int main(int argc, char** argv) {
  using namespace probe;
  if (argc == 2 && strcmp(argv[1], "kernel") == 0) return KernelReport();
  if (argc != 7) {
    std::fprintf(stderr, "usage: %s <none|arm> <norestart|restart> <fsync|fsync-match|sqlite-wal|sqlite-delete> <iters> <gap_us> <dir>\n", argv[0]);
    return 2;
  }
  const bool arm = strcmp(argv[1], "arm") == 0;
  const bool restart = strcmp(argv[2], "restart") == 0;
  g_workload = argv[3];
  g_iters = atol(argv[4]);
  g_gap_us = atol(argv[5]);
  g_dir = argv[6];
  if (g_workload.rfind("sqlite", 0) == 0 && !LoadSqlite()) return 3;

  struct sigaction sa = {};
  sa.sa_handler = OnSig;
  sa.sa_flags = restart ? SA_RESTART : 0;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR1, &sa, nullptr);

  if (arm) {
    modavd_fsync_defer_init("com.hypergryph.arknights");
    if (!g_installed.load()) {
      std::fprintf(stderr, "module did not arm (check modavd.fsync.defer / logcat ModAvdFsync)\n");
      return 4;
    }
  }
  pthread_t w, s;
  const long t0 = NowNs();
  pthread_create(&w, nullptr, Worker, nullptr);
  pthread_create(&s, nullptr, Signaller, nullptr);
  pthread_join(w, nullptr);
  g_done.store(true);
  pthread_join(s, nullptr);
  const double secs = (NowNs() - t0) / 1e9;
  const Result& r = g_res;
#ifdef PROBE_KILLABLE
  const char* variant = "killable";
#else
  const char* variant = "plain";
#endif
  std::printf("RESULT arm=%s variant=%s mode=%s handler=%s workload=%s iters=%ld gap_us=%ld secs=%.1f "
              "ok=%llu eintr=%llu other=%llu sql_ioerr_fsync=%llu sql_ioerr_other=%llu sql_other=%llu last_code=%d "
              "sig_sent=%llu sig_handled=%llu sup_recv=%llu sup_recv_enoent=%llu sup_send_enoent=%llu sup_deferred=%llu sup_stuck=%llu msg=\"%s\"\n",
              arm ? "yes" : "no", variant, arm ? (g_mode == kActive ? "active" : "observe") : "-",
              restart ? "SA_RESTART" : "no-SA_RESTART", g_workload.c_str(), g_iters, g_gap_us, secs,
              (unsigned long long)r.ok, (unsigned long long)r.eintr, (unsigned long long)r.other,
              (unsigned long long)r.sql_ioerr_fsync, (unsigned long long)r.sql_ioerr_other, (unsigned long long)r.sql_other,
              r.other_code, (unsigned long long)g_sig_sent.load(), (unsigned long long)g_sig_handled.load(),
              (unsigned long long)g_c_recv.load(), (unsigned long long)g_c_recv_enoent.load(),
              (unsigned long long)g_c_send_enoent.load(),
              (unsigned long long)g_c_deferred.load(), (unsigned long long)g_c_stuck.load(), r.first_msg.c_str());
  return 0;
}
