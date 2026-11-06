/* Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: GPL-2.0-only.
 * Standalone host verification adapter for extracted QEMU source functions. */
#include <windows.h>
#include <winioctl.h>
#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef intptr_t ssize_t;
typedef int64_t off64_t;
struct iovec { void *iov_base; size_t iov_len; };
typedef struct BlockDriverState { int open_flags; void *opaque; } BlockDriverState;
typedef struct RawWin32AIOData {
    BlockDriverState *bs;
    HANDLE hfile;
    struct iovec *aio_iov;
    int aio_niov;
    size_t aio_nbytes;
    off64_t aio_offset;
    int aio_type;
} RawWin32AIOData;
#define BDRV_O_NOCACHE 1
#define QEMU_AIO_READ 1
#define QEMU_AIO_WRITE 2
#define QEMU_AIO_FLUSH 4
#define QEMU_AIO_TYPE_MASK 255
#define RAW_WIN32_BOUNCE_MAX (64 * 1024)

static uint64_t io_calls, allocations, flushes;
static DWORD forced_error, short_limit;
static bool fail_allocation, fail_flush;
static int failures, cases;

static void reset_hooks(void)
{
    io_calls = allocations = flushes = 0;
    forced_error = short_limit = 0;
    fail_allocation = fail_flush = false;
}

static void *g_try_malloc(size_t n)
{
    allocations++;
    return fail_allocation ? NULL : malloc(n);
}
#define g_free free

static size_t transfer_iov(const struct iovec *iov, int count, size_t offset,
                           void *buf, size_t bytes, bool to_iov)
{
    size_t done = 0;
    for (int i = 0; i < count && done < bytes; i++) {
        if (offset >= iov[i].iov_len) {
            offset -= iov[i].iov_len;
            continue;
        }
        size_t n = iov[i].iov_len - offset;
        if (n > bytes - done) n = bytes - done;
        if (to_iov) memcpy((char *)iov[i].iov_base + offset, (char *)buf + done, n);
        else memcpy((char *)buf + done, (char *)iov[i].iov_base + offset, n);
        done += n;
        offset = 0;
    }
    return done;
}
static size_t iov_to_buf(const struct iovec *v, int n, size_t o, void *b, size_t z)
{ return transfer_iov(v, n, o, b, z, false); }
static size_t iov_from_buf(const struct iovec *v, int n, size_t o, const void *b, size_t z)
{ return transfer_iov(v, n, o, (void *)b, z, true); }
static size_t iov_memset(const struct iovec *v, int count, size_t offset, int fill, size_t bytes)
{
    size_t done = 0;
    for (int i = 0; i < count && done < bytes; i++) {
        if (offset >= v[i].iov_len) { offset -= v[i].iov_len; continue; }
        size_t n = v[i].iov_len - offset;
        if (n > bytes - done) n = bytes - done;
        memset((char *)v[i].iov_base + offset, fill, n);
        done += n;
        offset = 0;
    }
    return done;
}

static BOOL counted_read(HANDLE h, void *buf, DWORD n, DWORD *done, OVERLAPPED *ov)
{
    io_calls++;
    if (forced_error) { *done = 0; SetLastError(forced_error); return FALSE; }
    if (short_limit && n > short_limit) n = short_limit;
    return ReadFile(h, buf, n, done, ov);
}
static BOOL counted_write(HANDLE h, const void *buf, DWORD n, DWORD *done, OVERLAPPED *ov)
{
    io_calls++;
    if (forced_error) { *done = 0; SetLastError(forced_error); return FALSE; }
    if (short_limit && n > short_limit) n = short_limit;
    return WriteFile(h, buf, n, done, ov);
}
static BOOL counted_flush(HANDLE h)
{
    flushes++;
    if (fail_flush) { SetLastError(ERROR_WRITE_FAULT); return FALSE; }
    return FlushFileBuffers(h);
}
#define ReadFile counted_read
#define WriteFile counted_write
#define FlushFileBuffers counted_flush
#include "extracted.h"
#undef ReadFile
#undef WriteFile
#undef FlushFileBuffers

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

typedef struct Buffers {
    char *memory;
    struct iovec *iov;
    int count;
    size_t bytes;
} Buffers;

static Buffers buffers(size_t bytes, bool fragmented)
{
    Buffers b;
    b.count = (int)((bytes + 4095) / 4096);
    b.bytes = bytes;
    size_t stride = fragmented ? 8192 : 4096;
    b.memory = _aligned_malloc(stride * b.count, 4096);
    b.iov = calloc(b.count, sizeof(*b.iov));
    if (!b.memory || !b.iov) exit(3);
    for (int i = 0; i < b.count; i++) {
        b.iov[i].iov_base = b.memory + i * stride;
        b.iov[i].iov_len = bytes - i * 4096;
        if (b.iov[i].iov_len > 4096) b.iov[i].iov_len = 4096;
    }
    return b;
}
static void free_buffers(Buffers *b) { _aligned_free(b->memory); free(b->iov); }
static char pattern(size_t n) { return (char)((n * 131 + 17) % 251); }
static void fill(Buffers *b, bool patterned)
{
    size_t offset = 0;
    for (int i = 0; i < b->count; i++) {
        for (size_t j = 0; j < b->iov[i].iov_len; j++, offset++)
            ((char *)b->iov[i].iov_base)[j] = patterned ? pattern(offset) : (char)0x55;
    }
}
static bool contents(Buffers *b, size_t valid)
{
    size_t offset = 0;
    for (int i = 0; i < b->count; i++)
        for (size_t j = 0; j < b->iov[i].iov_len; j++, offset++)
            if (((char *)b->iov[i].iov_base)[j] != (offset < valid ? pattern(offset) : 0)) return false;
    return true;
}
static HANDLE scratch(const wchar_t *work, bool direct)
{
    wchar_t path[32768];
    swprintf(path, 32768, L"%ls\\%ls", work, direct ? L"direct.bin" : L"cached.bin");
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                          NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL | (direct ? FILE_FLAG_NO_BUFFERING : 0), NULL);
    if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "CreateFile failed: %lu\n", GetLastError()); exit(3); }
    DWORD ignored;
    if (!DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &ignored, NULL)) {
        fprintf(stderr, "Scratch filesystem must support sparse files: %lu\n", GetLastError()); exit(3);
    }
    return h;
}
static int worker(bool candidate, HANDLE h, Buffers *b, int type, int flags, int64_t offset)
{
    BlockDriverState bs = { flags, NULL };
    RawWin32AIOData *req = malloc(sizeof(*req));
    *req = (RawWin32AIOData){ &bs, h, b ? b->iov : NULL, b ? b->count : 0,
                             b ? b->bytes : 0, offset, type };
    return candidate ? candidate_aio_worker(req) : baseline_aio_worker(req);
}
static void real_seed(HANDLE h, size_t bytes)
{
    char *buf = malloc(bytes);
    for (size_t i = 0; i < bytes; i++) buf[i] = pattern(i);
    DWORD written;
    CHECK(WriteFile(h, buf, (DWORD)bytes, &written, NULL) && written == bytes);
    free(buf);
}
static void verify_file(HANDLE h, size_t bytes, int64_t offset)
{
    LARGE_INTEGER length;
    CHECK(GetFileSizeEx(h, &length));
    CHECK(length.QuadPart == offset + bytes);
    char *buf = _aligned_malloc(bytes, 4096);
    if (!buf) exit(3);
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)offset;
    ov.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    DWORD done;
    CHECK(ReadFile(h, buf, (DWORD)bytes, &done, &ov) && done == bytes);
    for (size_t i = 0; i < bytes; i++) {
        if (buf[i] != pattern(i)) { CHECK(false); break; }
    }
    _aligned_free(buf);
}
static void report(const char *name, int before)
{
    cases++;
    printf("%s: %s\n", name, failures == before ? "PASS" : "FAIL");
}

static void roundtrip(const wchar_t *work, size_t bytes, bool fragmented,
                      bool direct, int64_t offset, bool allocation_failure)
{
    int before = failures;
    for (int variant = 0; variant < 2; variant++) {
        Buffers b = buffers(bytes, fragmented);
        HANDLE h = scratch(work, direct);
        fill(&b, true);
        for (int op = QEMU_AIO_WRITE; op >= QEMU_AIO_READ; op--) {
            reset_hooks();
            fail_allocation = allocation_failure;
            if (op == QEMU_AIO_READ) fill(&b, false);
            CHECK(worker(variant, h, &b, op, direct ? BDRV_O_NOCACHE : 0, offset) == 0);
            CHECK(contents(&b, bytes));
            bool linear = variant && !fragmented && b.count > 1;
            bool bounce = !linear && !direct && b.count > 1 && bytes <= RAW_WIN32_BOUNCE_MAX;
            uint64_t expected = (linear || (bounce && !allocation_failure)) ? 1 : b.count;
            CHECK(io_calls == expected);
            CHECK(allocations == (bounce ? 1 : 0));
            if (op == QEMU_AIO_WRITE) verify_file(h, bytes, offset);
        }
        CHECK(FlushFileBuffers(h));
        CloseHandle(h);
        free_buffers(&b);
    }
    char name[160];
    snprintf(name, sizeof(name), "roundtrip bytes=%zu fragmented=%d direct=%d offset=%lld allocation_failure=%d",
             bytes, fragmented, direct, (long long)offset, allocation_failure);
    report(name, before);
}

static void edges(const wchar_t *work)
{
    for (int variant = 0; variant < 2; variant++) {
        for (int edge = 0; edge < 8; edge++) {
            int before = failures;
            HANDLE h = scratch(work, false);
            Buffers b = buffers(8192, false);
            if (edge == 0) real_seed(h, 5000);
            if (edge == 1) real_seed(h, 8192);
            fill(&b, false);
            reset_hooks();
            if (edge == 2) forced_error = ERROR_ACCESS_DENIED;
            if (edge == 3) forced_error = ERROR_HANDLE_EOF;
            if (edge == 4) forced_error = ERROR_DISK_FULL;
            if (edge == 5) short_limit = 1234;
            if (edge == 6) fail_flush = true;
            int op = edge == 4 || edge == 5 ? QEMU_AIO_WRITE : edge >= 6 ? QEMU_AIO_FLUSH : QEMU_AIO_READ;
            int64_t offset = edge == 1 ? 16384 : 0;
            int ret = worker(variant, h, &b, op, 0, offset);
            if (edge <= 1 || edge == 3) {
                CHECK(ret == 0);
                CHECK(contents(&b, edge == 0 ? 5000 : 0));
            } else if (edge == 5) CHECK(ret == -EINVAL);
            else if (edge == 7) CHECK(ret == 0);
            else CHECK(ret == -EIO);
            CHECK(edge >= 6 ? flushes == 1 : io_calls == 1);
            char name[80];
            snprintf(name, sizeof(name), "variant=%d edge=%d", variant, edge);
            report(name, before);
            CloseHandle(h);
            free_buffers(&b);
        }
        int before = failures;
        Buffers b = buffers(8192, false);
        reset_hooks();
        CHECK(worker(variant, INVALID_HANDLE_VALUE, &b, QEMU_AIO_READ, 0, 0) == -EIO);
        free_buffers(&b);
        report(variant ? "candidate real invalid handle" : "baseline real invalid handle", before);
    }
}

static void predicate(void)
{
    int before = failures;
    struct iovec v[3] = { {(void *)(uintptr_t)0x10000, 4096}, {(void *)(uintptr_t)0x11000, 4096} };
    RawWin32AIOData a = { NULL, NULL, v, 2, 8192, 0, 0 };
    CHECK(candidate_raw_win32_iov_is_contiguous(&a));
    v[1].iov_base = (void *)(uintptr_t)0x12000;
    CHECK(!candidate_raw_win32_iov_is_contiguous(&a));
    v[1].iov_base = (void *)(uintptr_t)0x11000;
    a.aio_nbytes = 8191;
    CHECK(!candidate_raw_win32_iov_is_contiguous(&a));
    a.aio_nbytes = 8193;
    CHECK(!candidate_raw_win32_iov_is_contiguous(&a));
    a.aio_nbytes = 0;
    CHECK(!candidate_raw_win32_iov_is_contiguous(&a));
    a.aio_nbytes = 8192; a.aio_niov = 1;
    CHECK(!candidate_raw_win32_iov_is_contiguous(&a));
    a.aio_niov = 0;
    CHECK(!candidate_raw_win32_iov_is_contiguous(&a));
    a.aio_niov = 2; v[0].iov_len = 0;
    CHECK(!candidate_raw_win32_iov_is_contiguous(&a));
    v[0] = (struct iovec){ (void *)(uintptr_t)0x10000, MAXDWORD - 4096ULL };
    v[1] = (struct iovec){ (void *)((uintptr_t)0x10000 + MAXDWORD - 4096ULL), 4096 };
    a.aio_nbytes = MAXDWORD;
    CHECK(candidate_raw_win32_iov_is_contiguous(&a));
    a.aio_nbytes = (size_t)MAXDWORD + 1;
    CHECK(!candidate_raw_win32_iov_is_contiguous(&a));
    v[0] = (struct iovec){ (void *)(UINTPTR_MAX - 2047), 4096 };
    v[1] = (struct iovec){ (void *)(uintptr_t)2048, 4096 };
    a.aio_nbytes = 8192;
    CHECK(!candidate_raw_win32_iov_is_contiguous(&a));
    report("contiguity, size, DWORD and pointer-overflow boundaries", before);
}

static void zero_vector(const wchar_t *work)
{
    int before = failures;
    for (int variant = 0; variant < 2; variant++) {
        HANDLE h = scratch(work, false);
        Buffers b = buffers(8192, false);
        struct iovec v[3] = { b.iov[0], {NULL, 0}, b.iov[1] };
        struct iovec *saved = b.iov;
        b.iov = v; b.count = 3;
        fill(&b, true); reset_hooks();
        CHECK(worker(variant, h, &b, QEMU_AIO_WRITE, 0, 0) == 0);
        CHECK(allocations == 1 && io_calls == 1);
        fill(&b, false); reset_hooks();
        CHECK(worker(variant, h, &b, QEMU_AIO_READ, 0, 0) == 0);
        CHECK(contents(&b, 8192));
        b.iov = saved;
        free_buffers(&b); CloseHandle(h);
    }
    report("zero-length vector keeps existing fallback", before);
}

static int functional(const wchar_t *work)
{
    size_t sizes[] = {4096, 8192, 16384, 65535, 65536, 69632, 1048576};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) roundtrip(work, sizes[i], false, false, 0, false);
    roundtrip(work, 16384, true, false, 0, false);
    roundtrip(work, 1048576, true, false, 0, false);
    roundtrip(work, 8192, false, true, 0, false);
    roundtrip(work, 8192, true, true, 0, false);
    roundtrip(work, 8192, false, false, (1LL << 32) + 4096, false);
    roundtrip(work, 16384, false, false, 0, true);
    roundtrip(work, 16384, true, false, 0, true);
    edges(work); predicate(); zero_vector(work);
    printf("%d cases, %d failed assertions\n", cases, failures);
    return failures ? 1 : 0;
}

static int benchmark(const wchar_t *work, int rounds)
{
    size_t sizes[] = {8192, 16384, 65536, 1048576};
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    printf("case,variant,round,iterations,seconds,io_calls,allocations\n");
    for (int fragmented = 0; fragmented < 2; fragmented++) {
        for (size_t s = 0; s < sizeof(sizes)/sizeof(sizes[0]); s++) {
            Buffers b = buffers(sizes[s], fragmented);
            fill(&b, true);
            HANDLE h = scratch(work, false);
            CHECK(worker(false, h, &b, QEMU_AIO_WRITE, 0, 0) == 0);
            int iterations = (int)((512ULL << 20) / b.bytes);
            if (iterations < 1024) iterations = 1024;
            for (int op = QEMU_AIO_READ; op <= QEMU_AIO_WRITE; op++) {
                for (int round = 0; round < rounds; round++) {
                    for (int turn = 0; turn < 2; turn++) {
                        bool candidate = (round + turn) % 2;
                        for (int warm = 0; warm < 32; warm++) CHECK(worker(candidate, h, &b, op, 0, 0) == 0);
                        reset_hooks();
                        LARGE_INTEGER start, end;
                        QueryPerformanceCounter(&start);
                        for (int i = 0; i < iterations; i++) CHECK(worker(candidate, h, &b, op, 0, 0) == 0);
                        QueryPerformanceCounter(&end);
                        printf("%s_%zu_%s,%s,%d,%d,%.9f,%llu,%llu\n",
                               fragmented ? "fragmented" : "contiguous", b.bytes,
                               op == QEMU_AIO_READ ? "read" : "write",
                               candidate ? "candidate" : "baseline", round, iterations,
                               (double)(end.QuadPart - start.QuadPart) / freq.QuadPart,
                               (unsigned long long)io_calls, (unsigned long long)allocations);
                    }
                }
            }
            CHECK(FlushFileBuffers(h));
            CloseHandle(h); free_buffers(&b);
        }
    }
    return failures ? 1 : 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: harness functional|benchmark WORK [ROUNDS]\n"); return 2; }
    if (!wcscmp(argv[1], L"functional")) return functional(argv[2]);
    if (!wcscmp(argv[1], L"benchmark") && argc == 4) return benchmark(argv[2], _wtoi(argv[3]));
    return 2;
}
