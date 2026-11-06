/* Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later. */
/* ModAVD#96 jank diagnosis, guest side (x86_64, root). Not part of the product.
 * For <secs> seconds, on one timeline (CLOCK_MONOTONIC ms since start):
 *   - heartbeat: a thread sleeping 5 ms in a loop; every wake-up gap > 50 ms is printed ("HB gap") --
 *     if the whole guest (all vCPUs) stalls, this shows it; if only the game's frames stall, it does not;
 *   - downloads: every 200 ms the set of *.downloading names under <dir> is compared with the previous
 *     one; a name that disappears is printed ("DL done", the game finished that file);
 *   - memory: every 1 s Dirty / Writeback from /proc/meminfo, and procs_blocked from /proc/stat
 *     (printed only when Dirty+Writeback changed by > 32 MB or blocked > 0);
 *   - frames: every 1 s `dumpsys SurfaceFlinger --latency <layer>` is read and new frames' actual
 *     present times are merged; every present-to-present gap > 100 ms is printed ("FRAME gap").
 * usage: jank-probe <secs> <dir> <layer-name>
 */
#include <dirent.h>
#include <pthread.h>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>

static long long NowNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static long long g_t0;
static volatile bool g_done = false;
static double Ms(long long ns) { return (ns - g_t0) / 1e6; }

static void* Heartbeat(void*) {
  long long last = NowNs();
  while (!g_done) {
    usleep(5000);
    const long long now = NowNs();
    if (now - last > 50000000LL) printf("%9.0f HB gap %.0f ms\n", Ms(last), (now - last) / 1e6);
    last = now;
  }
  return nullptr;
}

static void Scan(const std::string& d, std::set<std::string>* out, int depth) {
  DIR* dir = opendir(d.c_str());
  if (!dir) return;
  while (dirent* e = readdir(dir)) {
    if (e->d_name[0] == '.') continue;
    const std::string p = d + "/" + e->d_name;
    if (e->d_type == DT_DIR) {
      if (depth < 6) Scan(p, out, depth + 1);
    } else if (strstr(e->d_name, ".downloading")) {
      out->insert(p);
    }
  }
  closedir(dir);
}

static void* Downloads(void* arg) {
  const std::string root = static_cast<const char*>(arg);
  std::set<std::string> prev;
  Scan(root, &prev, 0);
  while (!g_done) {
    usleep(200000);
    std::set<std::string> cur;
    Scan(root, &cur, 0);
    for (const auto& p : prev)
      if (!cur.count(p)) printf("%9.0f DL done %s\n", Ms(NowNs()), p.substr(p.rfind('/') + 1).c_str());
    for (const auto& p : cur)
      if (!prev.count(p)) printf("%9.0f DL new  %s\n", Ms(NowNs()), p.substr(p.rfind('/') + 1).c_str());
    prev.swap(cur);
  }
  return nullptr;
}

static long Meminfo(const char* key) {
  FILE* f = fopen("/proc/meminfo", "r");
  if (!f) return -1;
  char line[256];
  long v = -1;
  const size_t kl = strlen(key);
  while (fgets(line, sizeof(line), f))
    if (!strncmp(line, key, kl) && line[kl] == ':') { v = atol(line + kl + 1); break; }
  fclose(f);
  return v;
}
static long Blocked() {
  FILE* f = fopen("/proc/stat", "r");
  if (!f) return -1;
  char line[512];
  long v = -1;
  while (fgets(line, sizeof(line), f))
    if (!strncmp(line, "procs_blocked ", 14)) { v = atol(line + 14); break; }
  fclose(f);
  return v;
}

int main(int argc, char** argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s <secs> <dir> <layer>\n", argv[0]);
    return 2;
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  const int secs = atoi(argv[1]);
  const std::string layer = argv[3];
  g_t0 = NowNs();
  pthread_t hb, dl;
  pthread_create(&hb, nullptr, Heartbeat, nullptr);
  pthread_create(&dl, nullptr, Downloads, argv[2]);
  long long last_present = 0;  // SurfaceFlinger's clock is CLOCK_MONOTONIC (ns)
  long last_dw = -1;
  int frames = 0;
  const std::string cmd = "dumpsys SurfaceFlinger --latency '" + layer + "' 2>/dev/null";
  while (Ms(NowNs()) < secs * 1000.0) {
    sleep(1);
    FILE* p = popen(cmd.c_str(), "r");
    if (p) {
      char line[256];
      bool first = true;
      std::vector<long long> pres;
      while (fgets(line, sizeof(line), p)) {
        if (first) { first = false; continue; }  // refresh period
        long long a = 0, b = 0, c = 0;
        if (sscanf(line, "%lld %lld %lld", &a, &b, &c) == 3 && b > 0 && b < (1LL << 62)) pres.push_back(b);
      }
      pclose(p);
      for (long long t : pres) {
        if (t <= last_present) continue;
        if (last_present && t - last_present > 100000000LL)
          printf("%9.0f FRAME gap %.0f ms\n", Ms(last_present), (t - last_present) / 1e6);
        last_present = t;
        ++frames;
      }
    }
    const long dirty = Meminfo("Dirty"), wb = Meminfo("Writeback"), bl = Blocked();
    if (last_dw < 0 || labs(dirty + wb - last_dw) > 32 * 1024 || bl > 0) {
      printf("%9.0f MEM dirty=%ldMB writeback=%ldMB blocked=%ld\n", Ms(NowNs()), dirty / 1024, wb / 1024, bl);
      last_dw = dirty + wb;
    }
  }
  g_done = true;
  pthread_join(hb, nullptr);
  pthread_join(dl, nullptr);
  printf("%9.0f END frames=%d\n", Ms(NowNs()), frames);
  return 0;
}
