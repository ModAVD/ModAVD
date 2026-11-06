#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

using Clock = std::chrono::steady_clock;
std::atomic<bool> go{false};
int duration = 10;
struct Worker { int id; unsigned long long bytes = 0, syncs = 0; int error = 0; };
void* run(void* ptr) {
    auto* w = static_cast<Worker*>(ptr);
    char path[128];
    snprintf(path, sizeof(path), "/data/local/tmp/modavd-syncbench/work-%d.bin", w->id);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { w->error = 1; return nullptr; }
    char data[7168]; memset(data, 0x41 + w->id, sizeof(data));
    while (!go.load(std::memory_order_acquire)) usleep(1000);
    auto end = Clock::now() + std::chrono::seconds(duration);
    while (Clock::now() < end) {
        if (write(fd, data, sizeof(data)) != sizeof(data) || fsync(fd) != 0) {
            w->error = 1; break;
        }
        w->bytes += sizeof(data); ++w->syncs;
    }
    if (fsync(fd) != 0) w->error = 1;
    close(fd);
    // Only this probe's own files; never reads or modifies application data.
    unlink(path);
    return nullptr;
}
int main(int argc, char** argv) {
    int count = argc > 1 ? atoi(argv[1]) : 1;
    duration = argc > 2 ? atoi(argv[2]) : 10;
    if (count < 1 || count > 8 || duration < 1 || duration > 120) return 2;
    mkdir("/data/local/tmp/modavd-syncbench", 0700);
    Worker workers[8]; pthread_t threads[8];
    for (int i = 0; i < count; ++i) {
        workers[i].id = i;
        if (pthread_create(&threads[i], nullptr, run, &workers[i]) != 0) return 2;
    }
    auto begin = Clock::now(); go.store(true, std::memory_order_release);
    unsigned long long bytes = 0, syncs = 0; int errors = 0;
    for (int i = 0; i < count; ++i) {
        pthread_join(threads[i], nullptr);
        bytes += workers[i].bytes; syncs += workers[i].syncs; errors += workers[i].error;
    }
    double elapsed = std::chrono::duration<double>(Clock::now()-begin).count();
    printf("{\"threads\":%d,\"seconds\":%.4f,\"mib_s\":%.4f,\"syncs_s\":%.4f,\"errors\":%d}\n",
           count, elapsed, bytes / elapsed / 1048576.0, syncs / elapsed, errors);
    return errors ? 1 : 0;
}
