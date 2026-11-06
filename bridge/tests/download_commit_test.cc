// Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later.
// Tests the real commit core through its public API. It does not certify a
// downloader's writer tracking, publication, or crash recovery.
#include "../companion/jni/download_commit.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
using namespace modavd::download;
int failures = 0;
int cases = 0;

#define CHECK(expression)                                                    \
    do {                                                                     \
        if (!(expression)) {                                                 \
            std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__,           \
                         #expression);                                       \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

bool SameToken(Token a, Token b) {
    return a.slot == b.slot && a.epoch == b.epoch;
}

struct FakeHandles {
    struct Handle {
        bool open = false;
        FileIdentity identity{};
    };
    std::array<Handle, 4096> handles{};
    int next_handle = 100;
    int captures = 0;
    int closes = 0;
    int capture_error = 0;
    int close_error = 0;
    bool fail_after_dup = false;
    bool unknown_identity = false;

    void Source(int fd, uint64_t inode) {
        handles[static_cast<size_t>(fd)] = {true, {7, inode}};
    }

    static int Capture(void* context, int source, CapturedHandle* output) {
        auto& self = *static_cast<FakeHandles*>(context);
        ++self.captures;
        if (self.capture_error && !self.fail_after_dup)
            return self.capture_error;
        if (source < 0 || static_cast<size_t>(source) >= self.handles.size() ||
            !self.handles[static_cast<size_t>(source)].open)
            return EBADF;
        const int duplicate = self.next_handle++;
        if (static_cast<size_t>(duplicate) >= self.handles.size())
            std::abort();
        self.handles[static_cast<size_t>(duplicate)] =
            self.handles[static_cast<size_t>(source)];
        *output = {duplicate, self.handles[static_cast<size_t>(duplicate)].identity,
                   !self.unknown_identity};
        return self.capture_error;
    }

    static int Close(void* context, int handle) {
        auto& self = *static_cast<FakeHandles*>(context);
        ++self.closes;
        CHECK(handle >= 100);
        CHECK(static_cast<size_t>(handle) < self.handles.size());
        if (handle < 0 || static_cast<size_t>(handle) >= self.handles.size())
            return EBADF;
        CHECK(self.handles[static_cast<size_t>(handle)].open);
        self.handles[static_cast<size_t>(handle)].open = false;
        return self.close_error;
    }

    HandleOps Ops() { return {this, Capture, Close}; }
};

Snapshot Read(CommitCore& core, Token token) {
    Snapshot snapshot;
    CHECK(core.Inspect(token, &snapshot).status == Status::Ok);
    return snapshot;
}

void CaptureFailures() {
    FakeHandles handles;
    handles.Source(3, 100);
    CommitCore core(handles.Ops());
    Token token;
    handles.capture_error = EMFILE;
    const Result failure = core.Track(3, &token);
    CHECK(failure.status == Status::CaptureFailed && failure.error == EMFILE);
    Snapshot rejected;
    CHECK(core.Inspect(token, &rejected).status == Status::Stale);
    CHECK(handles.closes == 0);

    handles.fail_after_dup = true;
    handles.capture_error = EIO;
    CHECK(core.Track(3, &token).status == Status::CaptureFailed);
    CHECK(handles.closes == 1);
    CHECK(handles.handles[3].open);

    handles.capture_error = 0;
    handles.unknown_identity = true;
    CHECK(core.Track(3, &token).status == Status::CaptureFailed);
    CHECK(handles.closes == 2);
    handles.unknown_identity = false;
    CHECK(core.Track(3, &token).status == Status::Ok);
    CHECK(core.CleanShutdown().status == Status::Ok);
    CHECK(handles.closes == 3);
}

void OwnedCaptureReusesSourceNumber() {
    FakeHandles handles;
    handles.Source(100, 100);
    // Model the capture contract's permitted result when source close races
    // with atomic dup. The callback returns an owned reference at the old fd
    // number; the core cannot infer ownership from numerical inequality.
    handles.next_handle = 100;
    CommitCore core(handles.Ops());
    Token token;
    CHECK(core.Track(100, &token).status == Status::Ok);
    CHECK(core.MarkDirty(token).status == Status::Ok);
    CommitTicket ticket;
    CHECK(core.BeginCommit(token, &ticket).status == Status::Ok);
    CHECK(ticket.handle == 100 && handles.closes == 0);
    CHECK(core.CompleteCommit(ticket, 0).status == Status::Ok);
    CHECK(core.Retire(token).status == Status::Ok);
    CHECK(handles.closes == 1 && !handles.handles[100].open);
}

void AliasCapture() {
    FakeHandles handles;
    handles.Source(3, 100);
    handles.Source(4, 100);
    {
        CommitCore core(handles.Ops());
        Token first, alias;
        CHECK(core.Track(3, &first).status == Status::Ok);
        CHECK(core.Track(4, &alias).status == Status::Ok);
        CHECK(!SameToken(first, alias));
        CHECK(core.MarkDirty(first).status == Status::Ok);
        CHECK(core.MarkDirty(alias).status == Status::Ok);
        CommitTicket first_ticket, alias_ticket;
        CHECK(core.BeginCommit(first, &first_ticket).status == Status::Ok);
        CHECK(core.BeginCommit(alias, &alias_ticket).status == Status::Ok);
        CHECK(first_ticket.handle != alias_ticket.handle);
        CHECK(core.CompleteCommit(first_ticket, EIO).status == Status::Failed);
        CHECK(Read(core, alias).state == State::Committing);
        CHECK(core.CompleteCommit(alias_ticket, 0).status == Status::Ok);
        CHECK(core.Retire(alias).status == Status::Ok);
        CHECK(core.Retire(first).status == Status::Failed);
        CHECK(handles.closes == 1);
        CHECK(handles.handles[static_cast<size_t>(first_ticket.handle)].open);
    }
    CHECK(handles.closes == 2);
    CHECK(handles.handles[3].open && handles.handles[4].open);
}

void DirtyDuringCommit() {
    FakeHandles handles;
    handles.Source(3, 100);
    CommitCore core(handles.Ops());
    Token token;
    CHECK(core.Track(3, &token).status == Status::Ok);
    CHECK(core.MarkDirty(token).status == Status::Ok);
    CommitTicket first;
    CHECK(core.BeginCommit(token, &first).status == Status::Ok);
    CommitTicket wrong = first;
    ++wrong.sealed_generation;
    CHECK(core.CompleteCommit(wrong, 0).status == Status::Stale);
    wrong = first;
    ++wrong.handle;
    CHECK(core.CompleteCommit(wrong, 0).status == Status::Stale);
    CHECK(core.Retire(token).status == Status::Busy);
    CHECK(core.CleanShutdown().status == Status::Busy);
    CHECK(handles.closes == 0);
    CHECK(core.MarkDirty(token).status == Status::Ok);
    CommitTicket blocked;
    CHECK(core.BeginCommit(token, &blocked).status == Status::Busy);
    CHECK(core.CompleteCommit(first, -EIO).status == Status::Invalid);
    CHECK(Read(core, token).committed_generation == 0);
    CHECK(core.CompleteCommit(first, 0).status == Status::Ok);
    const Snapshot still_dirty = Read(core, token);
    CHECK(still_dirty.state == State::Dirty);
    CHECK(still_dirty.dirty_generation > still_dirty.committed_generation);
    CHECK(still_dirty.committed_generation == first.sealed_generation);
    CHECK(core.Retire(token).status == Status::Unpaid);

    CommitTicket second;
    CHECK(core.BeginCommit(token, &second).status == Status::Ok);
    CHECK(core.CompleteCommit(first, 0).status == Status::Stale);
    CHECK(Read(core, token).state == State::Committing);
    CHECK(core.CompleteCommit(second, 0).status == Status::Ok);
    CHECK(core.CompleteCommit(second, 0).status == Status::Stale);
    CHECK(Read(core, token).state == State::Clean);
    CHECK(handles.closes == 0);
    CHECK(core.Retire(token).status == Status::Ok);
    CHECK(handles.closes == 1);
}

void ReusedSourceAndSlot() {
    FakeHandles handles;
    handles.Source(3, 100);
    CommitCore core(handles.Ops());
    Token old, replacement;
    CHECK(core.Track(3, &old).status == Status::Ok);
    CHECK(core.MarkDirty(old).status == Status::Ok);
    CommitTicket old_ticket;
    CHECK(core.BeginCommit(old, &old_ticket).status == Status::Ok);
    handles.Source(3, 200);  // The caller closed/reused its borrowed descriptor.
    CHECK(core.Track(3, &replacement).status == Status::Ok);
    CHECK(!SameToken(old, replacement));
    CHECK(Read(core, old).identity.inode == 100);
    CHECK(Read(core, replacement).identity.inode == 200);
    CHECK(handles.handles[static_cast<size_t>(old_ticket.handle)].identity.inode ==
          100);
    CHECK(core.MarkDirty(replacement).status == Status::Ok);
    CHECK(core.CompleteCommit(old_ticket, 0).status == Status::Ok);
    CHECK(Read(core, replacement).committed_generation == 0);
    CHECK(core.Retire(old).status == Status::Ok);

    handles.Source(4, 300);
    Token reused;
    CHECK(core.Track(4, &reused).status == Status::Ok);
    CHECK(reused.slot == old.slot && reused.epoch != old.epoch);
    CHECK(core.MarkDirty(old).status == Status::Stale);
    CHECK(core.CompleteCommit(old_ticket, 0).status == Status::Stale);
    CHECK(Read(core, reused).state == State::Clean);
    CommitTicket replacement_ticket;
    CHECK(core.BeginCommit(replacement, &replacement_ticket).status == Status::Ok);
    CHECK(core.CompleteCommit(replacement_ticket, 0).status == Status::Ok);
    CHECK(core.CleanShutdown().status == Status::Ok);
}

void FlushFailure(int error) {
    FakeHandles handles;
    handles.Source(3, 100);
    {
        CommitCore core(handles.Ops());
        Token token;
        CHECK(core.Track(3, &token).status == Status::Ok);
        CHECK(core.MarkDirty(token).status == Status::Ok);
        CommitTicket paid;
        CHECK(core.BeginCommit(token, &paid).status == Status::Ok);
        CHECK(core.CompleteCommit(paid, 0).status == Status::Ok);
        CHECK(core.MarkDirty(token).status == Status::Ok);
        CommitTicket failed;
        CHECK(core.BeginCommit(token, &failed).status == Status::Ok);
        const Result result = core.CompleteCommit(failed, error);
        CHECK(result.status == Status::Failed && result.error == error);
        const Snapshot snapshot = Read(core, token);
        CHECK(snapshot.state == State::NeedsRecovery);
        CHECK(snapshot.committed_generation == paid.sealed_generation);
        CHECK(snapshot.committed_generation < snapshot.dirty_generation);
        CHECK(snapshot.last_error == error && snapshot.owns_handle);
        CHECK(core.CompleteCommit(failed, 0).status == Status::Stale);
        CHECK(core.MarkDirty(token).status == Status::Failed);
        CHECK(core.BeginCommit(token, &paid).status == Status::Failed);
        CHECK(core.Retire(token).status == Status::Failed);
        CHECK(core.CleanShutdown().status == Status::Failed);
        CHECK(handles.closes == 0);
    }
    CHECK(handles.closes == 1);  // Destructor releases debt without claiming clean.
}

void Capacity() {
    FakeHandles handles;
    CommitCore core(handles.Ops());
    for (size_t i = 0; i < CommitCore::kCapacity; ++i) {
        handles.Source(static_cast<int>(i), 100 + i);
        Token token;
        CHECK(core.Track(static_cast<int>(i), &token).status == Status::Ok);
    }
    const int captures = handles.captures;
    Token rejected;
    CHECK(core.Track(40, &rejected).status == Status::Capacity);
    CHECK(handles.captures == captures);
    CHECK(handles.closes == 0);
    CHECK(core.CleanShutdown().status == Status::Ok);
    CHECK(handles.closes == static_cast<int>(CommitCore::kCapacity));
    handles.Source(40, 999);
    CHECK(core.Track(40, &rejected).status == Status::Ok);
    CHECK(core.CleanShutdown().status == Status::Ok);
}

void GenerationOverflow() {
    FakeHandles handles;
    handles.Source(3, 100);
    CommitCore core(handles.Ops(), 2);
    Token token;
    CHECK(core.Track(3, &token).status == Status::Ok);
    CHECK(core.MarkDirty(token).status == Status::Ok);
    CHECK(core.MarkDirty(token).status == Status::Ok);
    CommitTicket ticket;
    CHECK(core.BeginCommit(token, &ticket).status == Status::Ok);
    const Result overflow = core.MarkDirty(token);
    CHECK(overflow.status == Status::Overflow && overflow.error == EOVERFLOW);
    CHECK(Read(core, token).commit_inflight);
    CHECK(core.Retire(token).status == Status::Busy);
    CHECK(core.CleanShutdown().status == Status::Busy);
    const Result returned = core.CompleteCommit(ticket, 0);
    CHECK(returned.status == Status::Failed && returned.error == EOVERFLOW);
    CHECK(!Read(core, token).commit_inflight);
    CHECK(core.CompleteCommit(ticket, 0).status == Status::Stale);
    CHECK(Read(core, token).committed_generation == 0);
    CHECK(Read(core, token).state != State::Clean);
    CHECK(core.CleanShutdown().status != Status::Ok);
    CHECK(handles.closes == 0);
}

void EpochOverflow() {
    FakeHandles handles;
    handles.Source(3, 100);
    CommitCore core(handles.Ops(), UINT64_MAX, 1);
    Token oldest;
    for (size_t i = 0; i < CommitCore::kCapacity; ++i) {
        Token token;
        CHECK(core.Track(3, &token).status == Status::Ok);
        if (i == 0) oldest = token;
        CHECK(core.Retire(token).status == Status::Ok);
    }
    const int captures = handles.captures;
    Token rejected;
    CHECK(core.Track(3, &rejected).status == Status::Overflow);
    CHECK(handles.captures == captures);
    CHECK(core.MarkDirty(oldest).status == Status::Stale);
    CHECK(core.CleanShutdown().status == Status::Ok);
}

void ShutdownPreflightAndCloseFailure() {
    FakeHandles handles;
    handles.Source(3, 100);
    handles.Source(4, 200);
    CommitCore core(handles.Ops());
    Token clean, dirty;
    CHECK(core.Track(3, &clean).status == Status::Ok);
    CHECK(core.Track(4, &dirty).status == Status::Ok);
    CHECK(core.MarkDirty(dirty).status == Status::Ok);
    CHECK(core.CleanShutdown().status == Status::Unpaid);
    CHECK(handles.closes == 0 && Read(core, clean).owns_handle);
    CommitTicket ticket;
    CHECK(core.BeginCommit(dirty, &ticket).status == Status::Ok);
    CHECK(core.CompleteCommit(ticket, 0).status == Status::Ok);
    handles.close_error = EIO;
    const Result failed_close = core.Retire(clean);
    CHECK(failed_close.status == Status::HandleError && failed_close.error == EIO);
    CHECK(Read(core, clean).state == State::Failed);
    CHECK(!Read(core, clean).owns_handle);
    const int closes = handles.closes;
    CHECK(core.Retire(clean).status == Status::Failed);
    CHECK(handles.closes == closes);  // A consumed close is never retried.
    CHECK(core.CleanShutdown().status == Status::Failed);
}

#ifndef _WIN32
struct PosixHandles {
    int captures = 0;
    int closes = 0;
    static int Capture(void* context, int source, CapturedHandle* output) {
        auto& self = *static_cast<PosixHandles*>(context);
        ++self.captures;
        const int owned = fcntl(source, F_DUPFD_CLOEXEC, 0);
        if (owned == -1) return errno;
        output->handle = owned;
        struct stat info {};
        if (fstat(owned, &info) == -1) return errno;
        output->identity = {static_cast<uint64_t>(info.st_dev),
                            static_cast<uint64_t>(info.st_ino)};
        output->identity_valid = S_ISREG(info.st_mode);
        return output->identity_valid ? 0 : EINVAL;
    }
    static int Close(void* context, int handle) {
        ++static_cast<PosixHandles*>(context)->closes;
        return close(handle) == 0 ? 0 : errno;
    }
    HandleOps Ops() { return {this, Capture, Close}; }
};

// Only these paths, freshly created by this process, are written or removed.
struct Scratch {
    char directory[128]{};
    char first[160]{};
    char second[160]{};
    Scratch() {
#ifdef __ANDROID__
        const char* base = "/data/local/tmp";
#else
        const char* base = "/tmp";
#endif
        std::snprintf(directory, sizeof(directory), "%s/modavd-commit-XXXXXX", base);
        if (!mkdtemp(directory)) {
            std::perror("mkdtemp");
            std::exit(2);
        }
        std::snprintf(first, sizeof(first), "%s/first", directory);
        std::snprintf(second, sizeof(second), "%s/second", directory);
    }
    ~Scratch() {
        CHECK(unlink(first) == 0 || errno == ENOENT);
        CHECK(unlink(second) == 0 || errno == ENOENT);
        CHECK(rmdir(directory) == 0);
    }
};

int SyncResult(const CommitTicket& ticket, int injected_error = 0) {
    // Inject the worker's result, not corruption or failure in another process.
    if (injected_error) return injected_error;
    return fsync(ticket.handle) == 0 ? 0 : errno;
}

void PosixReuseAndReadback() {
    Scratch scratch;
    PosixHandles handles;
    CommitCore core(handles.Ops());
    const int original = open(scratch.first, O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(original >= 0);
    if (original < 0) return;
    CHECK(write(original, "first", 5) == 5);
    Token first;
    CHECK(core.Track(original, &first).status == Status::Ok);
    CHECK(core.MarkDirty(first).status == Status::Ok);
    CommitTicket first_ticket;
    CHECK(core.BeginCommit(first, &first_ticket).status == Status::Ok);
    CHECK(first_ticket.handle != original);
    CHECK(close(original) == 0);

    int replacement = open(scratch.second, O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(replacement >= 0);
    if (replacement < 0) return;
    if (replacement != original) {
        CHECK(dup2(replacement, original) == original);
        CHECK(close(replacement) == 0);
        replacement = original;
    }
    CHECK(write(replacement, "other", 5) == 5);
    Token second;
    CHECK(core.Track(replacement, &second).status == Status::Ok);
    CHECK(core.MarkDirty(second).status == Status::Ok);
    CHECK(!SameToken(first, second));
    CHECK(Read(core, first).identity.inode != Read(core, second).identity.inode);
    CHECK(core.CompleteCommit(first_ticket, SyncResult(first_ticket)).status ==
          Status::Ok);
    CHECK(Read(core, second).committed_generation == 0);
    char bytes[6]{};
    CHECK(pread(first_ticket.handle, bytes, 5, 0) == 5);
    CHECK(std::strcmp(bytes, "first") == 0);
    CHECK(handles.closes == 0);
    CHECK(core.Retire(first).status == Status::Ok);
    CHECK(fcntl(first_ticket.handle, F_GETFD) == -1 && errno == EBADF);
    CommitTicket second_ticket;
    CHECK(core.BeginCommit(second, &second_ticket).status == Status::Ok);
    CHECK(core.CompleteCommit(second_ticket, SyncResult(second_ticket)).status ==
          Status::Ok);
    CHECK(pread(second_ticket.handle, bytes, 5, 0) == 5);
    CHECK(std::strcmp(bytes, "other") == 0);
    CHECK(core.CleanShutdown().status == Status::Ok);
    CHECK(close(replacement) == 0);
}

void PosixInjectedFailure(int error) {
    Scratch scratch;
    PosixHandles handles;
    const int source = open(scratch.first, O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(source >= 0);
    if (source < 0) return;
    CHECK(write(source, "own scratch data", 16) == 16);
    {
        CommitCore core(handles.Ops());
        Token token;
        CHECK(core.Track(source, &token).status == Status::Ok);
        CHECK(core.MarkDirty(token).status == Status::Ok);
        CommitTicket ticket;
        CHECK(core.BeginCommit(token, &ticket).status == Status::Ok);
        CHECK(core.CompleteCommit(ticket, SyncResult(ticket, error)).status ==
              Status::Failed);
        CHECK(Read(core, token).committed_generation == 0);
        CHECK(Read(core, token).state == State::NeedsRecovery);
        CHECK(core.CleanShutdown().status == Status::Failed);
        CHECK(fcntl(ticket.handle, F_GETFD) != -1);
    }
    CHECK(handles.closes == 1);
    CHECK(close(source) == 0);
}

void PosixDirtyWhileSealed() {
    Scratch scratch;
    PosixHandles handles;
    CommitCore core(handles.Ops());
    const int writer = open(scratch.first, O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(writer >= 0);
    if (writer < 0) return;
    CHECK(write(writer, "early", 5) == 5);
    Token token;
    CHECK(core.Track(writer, &token).status == Status::Ok);
    CHECK(core.MarkDirty(token).status == Status::Ok);
    CommitTicket sealed;
    CHECK(core.BeginCommit(token, &sealed).status == Status::Ok);
    CHECK(pwrite(writer, "later", 5, 0) == 5);
    CHECK(core.MarkDirty(token).status == Status::Ok);
    CHECK(core.CompleteCommit(sealed, SyncResult(sealed)).status == Status::Ok);
    CHECK(Read(core, token).state == State::Dirty);
    CHECK(Read(core, token).committed_generation == sealed.sealed_generation);
    CHECK(core.Retire(token).status == Status::Unpaid);
    CommitTicket latest;
    CHECK(core.BeginCommit(token, &latest).status == Status::Ok);
    CHECK(core.CompleteCommit(latest, SyncResult(latest)).status == Status::Ok);
    CHECK(core.CleanShutdown().status == Status::Ok);
    char bytes[6]{};
    CHECK(pread(writer, bytes, 5, 0) == 5);
    CHECK(std::strcmp(bytes, "later") == 0);
    CHECK(close(writer) == 0);
}
#endif

template <typename Test>
void Run(const char* name, Test test) {
    const int before = failures;
    test();
    ++cases;
    std::printf("%s %s\n", failures == before ? "PASS" : "FAIL", name);
}
}  // namespace

int main() {
    Run("capture failure and identity rejection", CaptureFailures);
    Run("owned capture may reuse source fd number", OwnedCaptureReusesSourceNumber);
    Run("same identity keeps independent obligations", AliasCapture);
    Run("new dirt during sealed commit and duplicate completion", DirtyDuringCommit);
    Run("source fd and slot reuse", ReusedSourceAndSlot);
    Run("EIO quarantine", [] { FlushFailure(EIO); });
    Run("ENOSPC quarantine", [] { FlushFailure(ENOSPC); });
    Run("capacity before capture", Capacity);
    Run("generation overflow", GenerationOverflow);
    Run("slot epoch overflow before capture", EpochOverflow);
    Run("shutdown preflight and close failure", ShutdownPreflightAndCloseFailure);
#ifndef _WIN32
    Run("POSIX dup fsync readback and fd reuse", PosixReuseAndReadback);
    Run("POSIX write after sealing remains dirty", PosixDirtyWhileSealed);
    Run("POSIX scratch EIO result injection", [] { PosixInjectedFailure(EIO); });
    Run("POSIX scratch ENOSPC result injection", [] { PosixInjectedFailure(ENOSPC); });
#else
    std::puts("SKIP POSIX scratch cases on Windows");
#endif
    std::printf("%d cases, %d failures\n", cases, failures);
    return failures ? 1 : 0;
}
