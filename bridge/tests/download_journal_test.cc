// Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later.
// Operates only on fresh scratch files; this is not a power-loss experiment.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../companion/jni/download_journal.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace modavd::download;
int failures = 0;
int cases = 0;

#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); \
    ++failures; } } while (0)

void RequireImpl(bool success, const char* expression, int line) {
    if (!success) {
        const int error = errno;
        std::fprintf(stderr, "scratch setup line %d: %s: errno %d (%s)\n",
                     line, expression, error, std::strerror(error));
        std::abort();
    }
}
#define Require(expression) RequireImpl((expression), #expression, __LINE__)

void RemoveOwnTree(int directory) {
    const int scan = ::openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    Require(scan >= 0);
    DIR* entries = ::fdopendir(scan);
    Require(entries != nullptr);
    for (;;) {
        const dirent* entry = ::readdir(entries);
        if (entry == nullptr) break;
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        struct stat info{};
        Require(::fstatat(directory, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) == 0);
        if (S_ISDIR(info.st_mode)) {
            const int child = ::openat(directory, entry->d_name,
                                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            Require(child >= 0);
            RemoveOwnTree(child);
            ::close(child);
            Require(::unlinkat(directory, entry->d_name, AT_REMOVEDIR) == 0);
        } else {
            Require(::unlinkat(directory, entry->d_name, 0) == 0);
        }
    }
    ::closedir(entries);
}

struct Scratch {
#ifdef __ANDROID__
    char path[80] = "/data/local/tmp/modavd-journal-XXXXXX";
#else
    char path[80] = "/tmp/modavd-journal-XXXXXX";
#endif
    int directory = -1;
    int root = -1;
    int journal = -1;
    int outside = -1;

    Scratch() {
        Require(::mkdtemp(path) != nullptr);
        directory = ::open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        Require(directory >= 0);
        Require(::mkdirat(directory, "download", 0700) == 0);
        Require(::mkdirat(directory, "journal", 0700) == 0);
        Require(::mkdirat(directory, "outside", 0700) == 0);
        root = ::openat(directory, "download", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        journal = ::openat(directory, "journal", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        outside = ::openat(directory, "outside", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        Require(root >= 0 && journal >= 0 && outside >= 0);
    }
    ~Scratch() {
        ::close(root); ::close(journal); ::close(outside);
        RemoveOwnTree(directory);
        ::close(directory);
        Require(::rmdir(path) == 0);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

struct stat Write(int directory, const char* key, const char* contents = "scratch") {
    const int fd = ::openat(directory, key,
                             O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    Require(fd >= 0);
    const size_t length = std::strlen(contents);
    Require(::write(fd, contents, length) == static_cast<ssize_t>(length));
    Require(::fsync(fd) == 0);
    struct stat info{};
    Require(::fstat(fd, &info) == 0);
    Require(::close(fd) == 0);
    return info;
}

void SyncFile(int directory, const char* key) {
    const int fd = ::openat(directory, key, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    Require(fd >= 0);
    Require(::fsync(fd) == 0);
    Require(::close(fd) == 0);
}

bool Exists(int directory, const char* key) {
    struct stat info{};
    return ::fstatat(directory, key, &info, AT_SYMLINK_NOFOLLOW) == 0;
}

bool Contents(int directory, const char* key, const char* expected) {
    const int fd = ::openat(directory, key, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    char bytes[128]{};
    const ssize_t count = ::read(fd, bytes, sizeof(bytes));
    ::close(fd);
    return count == static_cast<ssize_t>(std::strlen(expected)) &&
           std::memcmp(bytes, expected, static_cast<size_t>(count)) == 0;
}

int RecordCount(int journal) {
    const int scan = ::openat(journal, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    Require(scan >= 0);
    DIR* directory = ::fdopendir(scan);
    Require(directory != nullptr);
    int count = 0;
    for (;;) {
        const dirent* entry = ::readdir(directory);
        if (entry == nullptr) break;
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) ++count;
    }
    ::closedir(directory);
    return count;
}

JournalResult AdmitKey(DownloadJournal& journal, Scratch& scratch,
                       const char* key, JournalToken* token) {
    struct stat info{};
    Require(::fstatat(scratch.root, key, &info, AT_SYMLINK_NOFOLLOW) == 0);
    return journal.Admit(key, static_cast<uint64_t>(info.st_dev),
                         static_cast<uint64_t>(info.st_ino), token);
}

void Ready(DownloadJournal& journal, Scratch& scratch) {
    Require(journal.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
    Require(journal.Recover().status == JournalStatus::Ok);
}

void Intent(Scratch& scratch, const char* key) {
    DownloadJournal journal;
    Ready(journal, scratch);
    JournalToken token;
    Require(AdmitKey(journal, scratch, key, &token).status == JournalStatus::Ok);
    Require(journal.Close().status == JournalStatus::Ok);
}

struct Fault {
    int calls = 0;
    int fail_at = 0;
    bool persistent = false;
    static int Sync(void* context, int fd) {
        auto& self = *static_cast<Fault*>(context);
        ++self.calls;
        if (self.fail_at > 0 && (self.calls == self.fail_at ||
            (self.persistent && self.calls >= self.fail_at))) return EIO;
        return ::fsync(fd) == 0 ? 0 : errno;
    }
    JournalIoOps Ops() { return {this, Sync}; }
    void Fail(int at, bool forever = false) { calls = 0; fail_at = at; persistent = forever; }
};

void SessionLock() {
    Scratch scratch;
    DownloadJournal first, second;
    Write(scratch.root, "x.downloading");
    JournalToken token;
    CHECK(first.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
    CHECK(AdmitKey(first, scratch, "x.downloading", &token).status == JournalStatus::NeedsRecovery);
    CHECK(second.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Busy);
    CHECK(first.Recover().status == JournalStatus::Ok);
    CHECK(first.Close().status == JournalStatus::Ok);
    CHECK(second.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
    CHECK(second.Close().status == JournalStatus::Ok);
    CHECK(second.AcquireSession(scratch.root, scratch.root).status == JournalStatus::Invalid);
    CHECK(second.AcquireSession(-1, scratch.root).status == JournalStatus::Invalid);
    CHECK(second.AcquireSession(AT_FDCWD, scratch.root).status == JournalStatus::Invalid);
}

void AdmitRetireAndEpoch() {
    Scratch scratch;
    Require(::mkdirat(scratch.root, "a", 0700) == 0);
    Require(::mkdirat(scratch.root, "a/b", 0700) == 0);
    Write(scratch.root, "a/b/item.downloading");
    Write(scratch.root, "a/b/item", "completed");
    DownloadJournal journal;
    Ready(journal, scratch);
    JournalToken first, second;
    CHECK(AdmitKey(journal, scratch, "a/b/item.downloading", &first).status == JournalStatus::Ok);
    CHECK(RecordCount(scratch.journal) == 1);
    CHECK(AdmitKey(journal, scratch, "a/b/item.downloading", &second).status == JournalStatus::Busy);
    CHECK(journal.Recover().status == JournalStatus::Busy);
    SyncFile(scratch.root, "a/b/item.downloading");
    CHECK(journal.Retire(first).status == JournalStatus::Ok);
    CHECK(journal.Retire(first).status == JournalStatus::Stale);
    CHECK(RecordCount(scratch.journal) == 0);
    CHECK(AdmitKey(journal, scratch, "a/b/item.downloading", &second).status == JournalStatus::Ok);
    CHECK(first.slot == second.slot && first.epoch != second.epoch);
    CHECK(journal.Retire(first).status == JournalStatus::Stale);
    SyncFile(scratch.root, "a/b/item.downloading");
    CHECK(journal.Retire(second).status == JournalStatus::Ok);
    CHECK(Exists(scratch.root, "a/b/item.downloading"));
    CHECK(Contents(scratch.root, "a/b/item", "completed"));
}

void ReplacedInodeAndExactKey() {
    Scratch scratch;
    const struct stat original = Write(scratch.root, "item.downloading", "old");
    Write(scratch.root, "item", "completed");
    Write(scratch.root, "other.downloading", "unadmitted");
    Intent(scratch, "item.downloading");
    Require(::renameat(scratch.root, "item.downloading", scratch.root, "held-old") == 0);
    const struct stat replacement = Write(scratch.root, "item.downloading", "replacement");
    CHECK(original.st_ino != replacement.st_ino);
    DownloadJournal journal;
    CHECK(journal.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
    CHECK(journal.Recover().status == JournalStatus::Ok);
    CHECK(!Exists(scratch.root, "item.downloading"));
    CHECK(Contents(scratch.root, "held-old", "old"));
    CHECK(Contents(scratch.root, "item", "completed"));
    CHECK(Contents(scratch.root, "other.downloading", "unadmitted"));
    CHECK(RecordCount(scratch.journal) == 0);
    CHECK(journal.Recover().status == JournalStatus::Ok);
}

void MissingKeys() {
    for (int absent_parent = 0; absent_parent != 2; ++absent_parent) {
        Scratch scratch;
        Require(::mkdirat(scratch.root, "gone", 0700) == 0);
        Require(::mkdirat(scratch.root, "gone/sub", 0700) == 0);
        Write(scratch.root, "gone/sub/x.downloading");
        Intent(scratch, "gone/sub/x.downloading");
        Require(::unlinkat(scratch.root, "gone/sub/x.downloading", 0) == 0);
        if (absent_parent) {
            Require(::unlinkat(scratch.root, "gone/sub", AT_REMOVEDIR) == 0);
            Require(::unlinkat(scratch.root, "gone", AT_REMOVEDIR) == 0);
        }
        DownloadJournal journal;
        CHECK(journal.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
        CHECK(journal.Recover().status == JournalStatus::Ok);
        CHECK(RecordCount(scratch.journal) == 0);
    }
}

void InvalidKeysAndIdentity() {
    Scratch scratch;
    const struct stat info = Write(scratch.root, "x.downloading");
    DownloadJournal journal;
    Ready(journal, scratch);
    JournalToken token;
    const char* invalid[] = {"", "/x.downloading", "../x.downloading", "a/../x.downloading",
        "./x.downloading", "a//x.downloading", "a/./x.downloading", "a/", "x.downloading.more",
        "x", "a\\x.downloading", "a\nx.downloading", ".downloading"};
    for (const char* key : invalid) CHECK(journal.Admit(key, 0, 0, &token).status == JournalStatus::Invalid);
    CHECK(journal.Admit(nullptr, 0, 0, &token).status == JournalStatus::Invalid);
    const std::string long_component = std::string(256, 'a') + ".downloading";
    CHECK(journal.Admit(long_component.c_str(), 0, 0, &token).status == JournalStatus::Invalid);
    const std::string long_key(DownloadJournal::kMaxKeyLength + 1, 'a');
    CHECK(journal.Admit(long_key.c_str(), 0, 0, &token).status == JournalStatus::Invalid);
    CHECK(journal.Admit("x.downloading", static_cast<uint64_t>(info.st_dev),
        static_cast<uint64_t>(info.st_ino) + 1, &token).status == JournalStatus::Unsafe);
    CHECK(journal.Admit("missing.downloading", 0, 0, &token).status == JournalStatus::Unsafe);
    CHECK(RecordCount(scratch.journal) == 0);
}

void MaximumKey() {
    Scratch scratch;
    const std::string component(255, 'd');
    std::string key;
    for (int index = 0; index != 3; ++index) {
        if (!key.empty()) key += '/';
        key += component;
        Require(::mkdirat(scratch.root, key.c_str(), 0700) == 0);
    }
    key += '/';
    key += std::string(255 - std::strlen(".downloading"), 'f') + ".downloading";
    CHECK(key.size() == DownloadJournal::kMaxKeyLength);
    Write(scratch.root, key.c_str());
    Intent(scratch, key.c_str());
    DownloadJournal journal;
    CHECK(journal.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
    CHECK(journal.Recover().status == JournalStatus::Ok);
    CHECK(!Exists(scratch.root, key.c_str()));
}

void FinalSymlinkAndHardlink() {
    for (int hardlink = 0; hardlink != 2; ++hardlink) {
        Scratch scratch;
        Write(scratch.root, "x.downloading");
        Write(scratch.outside, "victim", "outside");
        Intent(scratch, "x.downloading");
        if (hardlink) {
            Require(::linkat(scratch.root, "x.downloading", scratch.outside, "alias", 0) == 0);
        } else {
            Require(::unlinkat(scratch.root, "x.downloading", 0) == 0);
            Require(::symlinkat("../outside/victim", scratch.root, "x.downloading") == 0);
        }
        DownloadJournal journal;
        CHECK(journal.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
        CHECK(journal.Recover().status == JournalStatus::Unsafe);
        CHECK(Exists(scratch.root, "x.downloading"));
        CHECK(Contents(scratch.outside, "victim", "outside"));
        CHECK(RecordCount(scratch.journal) == 1);
        JournalToken token;
        CHECK(journal.Admit("new.downloading", 0, 0, &token).status == JournalStatus::NeedsRecovery);
    }
}

void IntermediateSymlink() {
    Scratch scratch;
    Require(::mkdirat(scratch.root, "dir", 0700) == 0);
    Write(scratch.root, "dir/x.downloading");
    Write(scratch.outside, "x.downloading", "outside");
    Intent(scratch, "dir/x.downloading");
    Require(::renameat(scratch.root, "dir", scratch.root, "saved") == 0);
    Require(::symlinkat("../outside", scratch.root, "dir") == 0);
    DownloadJournal journal;
    CHECK(journal.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
    CHECK(journal.Recover().status == JournalStatus::Unsafe);
    CHECK(Contents(scratch.outside, "x.downloading", "outside"));
    CHECK(Exists(scratch.root, "saved/x.downloading"));
    CHECK(RecordCount(scratch.journal) == 1);
}

void NonRegularTargets() {
    for (int fifo = 0; fifo != 2; ++fifo) {
        Scratch scratch;
        Write(scratch.root, "x.downloading");
        Intent(scratch, "x.downloading");
        Require(::unlinkat(scratch.root, "x.downloading", 0) == 0);
        if (fifo) Require(::mkfifoat(scratch.root, "x.downloading", 0600) == 0);
        else Require(::mkdirat(scratch.root, "x.downloading", 0700) == 0);
        DownloadJournal journal;
        CHECK(journal.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
        CHECK(journal.Recover().status == JournalStatus::Unsafe);
        CHECK(Exists(scratch.root, "x.downloading"));
        CHECK(RecordCount(scratch.journal) == 1);
    }
}

void CorruptRecordsBlockAll() {
    Scratch scratch;
    Write(scratch.root, "first.downloading");
    Write(scratch.root, "second.downloading");
    DownloadJournal live;
    Ready(live, scratch);
    JournalToken first, second;
    Require(AdmitKey(live, scratch, "first.downloading", &first).status == JournalStatus::Ok);
    Require(AdmitKey(live, scratch, "second.downloading", &second).status == JournalStatus::Ok);
    Require(live.Close().status == JournalStatus::Ok);
    const int fd = ::openat(scratch.journal, "intent-01", O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    Require(fd >= 0);
    char byte = 'X';
    Require(::pwrite(fd, &byte, 1, 40) == 1);
    Require(::close(fd) == 0);
    DownloadJournal recovery;
    CHECK(recovery.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
    CHECK(recovery.Recover().status == JournalStatus::Corrupt);
    CHECK(Exists(scratch.root, "first.downloading"));
    CHECK(Exists(scratch.root, "second.downloading"));
    CHECK(RecordCount(scratch.journal) == 2);
}

void PartialUnknownAndUnsafeRecords() {
    for (int variant = 0; variant != 4; ++variant) {
        Scratch scratch;
        Write(scratch.root, "x.downloading");
        Intent(scratch, "x.downloading");
        if (variant == 0) Write(scratch.journal, "intent-01", "partial");
        if (variant == 1) Write(scratch.journal, "unexpected", "unknown");
        if (variant == 2) {
            Require(::linkat(scratch.journal, "intent-00", scratch.outside, "record-alias", 0) == 0);
        }
        if (variant == 3) {
            Require(::unlinkat(scratch.journal, "intent-00", 0) == 0);
            Write(scratch.outside, "record", "outside");
            Require(::symlinkat("../outside/record", scratch.journal, "intent-00") == 0);
        }
        DownloadJournal journal;
        CHECK(journal.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
        CHECK(journal.Recover().status == JournalStatus::Corrupt);
        CHECK(Exists(scratch.root, "x.downloading"));
        CHECK(RecordCount(scratch.journal) >= 1);
    }
}

void DuplicateLogicalKeys() {
    Scratch scratch;
    Write(scratch.root, "x.downloading");
    Intent(scratch, "x.downloading");
    const int source = ::openat(scratch.journal, "intent-00", O_RDONLY | O_CLOEXEC);
    Require(source >= 0);
    char bytes[2048];
    const ssize_t count = ::read(source, bytes, sizeof(bytes));
    Require(count > 0);
    ::close(source);
    const int dest = ::openat(scratch.journal, "intent-01", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    Require(dest >= 0 && ::write(dest, bytes, static_cast<size_t>(count)) == count);
    ::close(dest);
    DownloadJournal journal;
    CHECK(journal.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
    CHECK(journal.Recover().status == JournalStatus::Corrupt);
    CHECK(Exists(scratch.root, "x.downloading"));
    CHECK(RecordCount(scratch.journal) == 2);
}

void Capacity() {
    Scratch scratch;
    DownloadJournal journal;
    Ready(journal, scratch);
    JournalToken tokens[DownloadJournal::kCapacity];
    char name[64];
    for (size_t index = 0; index != DownloadJournal::kCapacity; ++index) {
        std::snprintf(name, sizeof(name), "file-%zu.downloading", index);
        Write(scratch.root, name);
        CHECK(AdmitKey(journal, scratch, name, &tokens[index]).status == JournalStatus::Ok);
    }
    Write(scratch.root, "overflow.downloading");
    JournalToken token;
    CHECK(AdmitKey(journal, scratch, "overflow.downloading", &token).status == JournalStatus::Capacity);
    CHECK(RecordCount(scratch.journal) == static_cast<int>(DownloadJournal::kCapacity));
    for (size_t index = 0; index != DownloadJournal::kCapacity; ++index) {
        std::snprintf(name, sizeof(name), "file-%zu.downloading", index);
        SyncFile(scratch.root, name);
        CHECK(journal.Retire(tokens[index]).status == JournalStatus::Ok);
    }
    CHECK(RecordCount(scratch.journal) == 0);
}

void AdmissionSyncFailures() {
    for (int fail_at = 1; fail_at != 3; ++fail_at) {
        Scratch scratch;
        Write(scratch.root, "x.downloading");
        Fault fault;
        DownloadJournal journal(fault.Ops());
        Ready(journal, scratch);
        fault.Fail(fail_at);
        JournalToken token;
        const JournalResult failed = AdmitKey(journal, scratch, "x.downloading", &token);
        CHECK(failed.status == JournalStatus::IoError && failed.error == EIO);
        CHECK(token.slot == UINT32_MAX);
        CHECK(RecordCount(scratch.journal) == 1);
        CHECK(Exists(scratch.root, "x.downloading"));
        CHECK(journal.Admit("new.downloading", 0, 0, &token).status == JournalStatus::NeedsRecovery);
        fault.Fail(0);
        CHECK(journal.Recover().status == JournalStatus::Ok);
        CHECK(Exists(scratch.root, "x.downloading") == (fail_at == 1));
        CHECK(RecordCount(scratch.journal) == 0);
    }
}

void RecoveryInterruptedAfterUnlink() {
    Scratch scratch;
    Write(scratch.root, "x.downloading");
    Intent(scratch, "x.downloading");
    Fault fault;
    DownloadJournal interrupted(fault.Ops());
    CHECK(interrupted.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
    fault.Fail(1);
    const JournalResult result = interrupted.Recover();
    CHECK(result.status == JournalStatus::IoError && result.error == EIO);
    CHECK(!Exists(scratch.root, "x.downloading"));
    CHECK(RecordCount(scratch.journal) == 1);
    CHECK(interrupted.Close().status == JournalStatus::Ok);
    DownloadJournal repeated;
    CHECK(repeated.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
    CHECK(repeated.Recover().status == JournalStatus::Ok);
    CHECK(RecordCount(scratch.journal) == 0);
    CHECK(repeated.Recover().status == JournalStatus::Ok);
}

void RecoveryIntentRemovalSyncFailure() {
    for (int persistent = 0; persistent != 2; ++persistent) {
        Scratch scratch;
        Write(scratch.root, "x.downloading");
        Intent(scratch, "x.downloading");
        Fault fault;
        DownloadJournal journal(fault.Ops());
        CHECK(journal.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
        fault.Fail(2, persistent != 0);
        const JournalResult result = journal.Recover();
        CHECK(result.status == JournalStatus::IoError && result.error == EIO);
        CHECK(fault.calls == (persistent ? 3 : 4));
        CHECK(!Exists(scratch.root, "x.downloading"));
        CHECK(RecordCount(scratch.journal) == 1);
        JournalToken token;
        CHECK(journal.Admit("new.downloading", 0, 0, &token).status == JournalStatus::NeedsRecovery);
        fault.Fail(0);
        CHECK(journal.Recover().status == JournalStatus::Ok);
        CHECK(RecordCount(scratch.journal) == 0);
    }
}

void RetireIntentRemovalSyncFailure() {
    Scratch scratch;
    Write(scratch.root, "x.downloading");
    Fault fault;
    DownloadJournal journal(fault.Ops());
    Ready(journal, scratch);
    JournalToken token;
    Require(AdmitKey(journal, scratch, "x.downloading", &token).status == JournalStatus::Ok);
    SyncFile(scratch.root, "x.downloading");
    fault.Fail(1);
    const JournalResult result = journal.Retire(token);
    CHECK(result.status == JournalStatus::IoError && result.error == EIO);
    CHECK(RecordCount(scratch.journal) == 1);
    CHECK(Exists(scratch.root, "x.downloading"));
    CHECK(journal.Retire(token).status == JournalStatus::NeedsRecovery);
    CHECK(journal.Recover().status == JournalStatus::Busy);
    CHECK(journal.Close().status == JournalStatus::Ok);
    DownloadJournal recovered;
    CHECK(recovered.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
    CHECK(recovered.Recover().status == JournalStatus::Ok);
    CHECK(RecordCount(scratch.journal) == 0);
}

void ClosePreservesIntent() {
    Scratch scratch;
    Write(scratch.root, "x.downloading");
    {
        DownloadJournal journal;
        Ready(journal, scratch);
        JournalToken token;
        CHECK(AdmitKey(journal, scratch, "x.downloading", &token).status == JournalStatus::Ok);
    }
    CHECK(RecordCount(scratch.journal) == 1);
    CHECK(Exists(scratch.root, "x.downloading"));
}

struct AdmissionStop {
    int point = 0;
    bool armed = false;
    bool crash = false;
    int writes = 0;

    int Stop() const {
        if (crash) ::_exit(77);
        return EIO;
    }
    static int Write(void* context, int fd, const void* bytes, size_t length, size_t* written) {
        auto& self = *static_cast<AdmissionStop*>(context);
        if (self.armed && self.point == 0) return self.Stop();
        if (self.armed && self.point == 1) {
            if (++self.writes != 1) return self.Stop();
            const ssize_t count = ::write(fd, bytes, length / 2);
            if (count < 0) return errno;
            *written = static_cast<size_t>(count);
            if (self.crash) return self.Stop();
            return 0;
        }
        const ssize_t count = ::write(fd, bytes, length);
        if (count < 0) return errno;
        *written = static_cast<size_t>(count);
        return 0;
    }
    static int Checkpoint(void* context, JournalWriteCheckpoint point) {
        auto& self = *static_cast<AdmissionStop*>(context);
        if (self.armed && ((self.point == 2 && point == JournalWriteCheckpoint::AfterFileSync) ||
            (self.point == 3 && point == JournalWriteCheckpoint::BeforeDirectorySync))) return self.Stop();
        return 0;
    }
    JournalIoOps Ops() { return {this, nullptr, Write, Checkpoint}; }
};

void AdmissionInterrupted(int point, bool crash) {
    Scratch scratch;
    Write(scratch.root, "acknowledged.downloading", "prior ACK debt");
    Write(scratch.root, "interrupted.downloading", "never ACKed");
    Write(scratch.root, "acknowledged", "completed");
    auto interrupted = [&]() {
        AdmissionStop stop;
        stop.point = point;
        stop.crash = crash;
        DownloadJournal journal(stop.Ops());
        Ready(journal, scratch);
        JournalToken prior;
        Require(AdmitKey(journal, scratch, "acknowledged.downloading", &prior).status == JournalStatus::Ok);
        stop.armed = true;
        JournalToken rejected;
        const JournalResult result = AdmitKey(journal, scratch, "interrupted.downloading", &rejected);
        if (crash) ::_exit(99);
        CHECK(result.status == JournalStatus::IoError && result.error == EIO);
        CHECK(rejected.slot == UINT32_MAX);
        CHECK(journal.Admit("anything.downloading", 0, 0, &rejected).status == JournalStatus::NeedsRecovery);
        CHECK(journal.Close().status == JournalStatus::Ok);
    };
    if (crash) {
        const pid_t child = ::fork();
        Require(child >= 0);
        if (child == 0) interrupted();
        int status = 0;
        Require(::waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 77);
    } else {
        interrupted();
    }
    CHECK(RecordCount(scratch.journal) == 2);
    CHECK(Exists(scratch.root, "acknowledged.downloading"));
    CHECK(Exists(scratch.root, "interrupted.downloading"));
    const bool final_published = point == 3;
    CHECK(Exists(scratch.journal, final_published ? "intent-01" : "draft-01"));
    DownloadJournal recovered;
    CHECK(recovered.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
    CHECK(recovered.Recover().status == JournalStatus::Ok);
    CHECK(!Exists(scratch.root, "acknowledged.downloading"));
    CHECK(Exists(scratch.root, "interrupted.downloading") == !final_published);
    CHECK(Contents(scratch.root, "acknowledged", "completed"));
    CHECK(RecordCount(scratch.journal) == 0);
    CHECK(recovered.Recover().status == JournalStatus::Ok);
}

void AdmissionWriteAndPublishErrors() {
    for (int point = 0; point != 4; ++point) AdmissionInterrupted(point, false);
}

void AdmissionProcessCrashCutpoints() {
    for (int point = 0; point != 4; ++point) AdmissionInterrupted(point, true);
}

void UnsafeDraftsBlockRecovery() {
    for (int variant = 0; variant != 5; ++variant) {
        Scratch scratch;
        Write(scratch.root, "acknowledged.downloading");
        Intent(scratch, "acknowledged.downloading");
        if (variant == 0) Write(scratch.journal, "draft-99", "not an owned slot");
        if (variant == 1) {
            Write(scratch.outside, "foreign", "outside");
            Require(::symlinkat("../outside/foreign", scratch.journal, "draft-01") == 0);
        }
        if (variant == 2) {
            Write(scratch.journal, "draft-01", "hardlinked");
            Require(::linkat(scratch.journal, "draft-01", scratch.outside, "alias", 0) == 0);
        }
        if (variant == 3) Require(::mkdirat(scratch.journal, "draft-01", 0700) == 0);
        if (variant == 4) {
            Write(scratch.journal, "draft-01", "wrong mode");
            Require(::fchmodat(scratch.journal, "draft-01", 0644, 0) == 0);
        }
        DownloadJournal recovered;
        CHECK(recovered.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
        CHECK(recovered.Recover().status == JournalStatus::Corrupt);
        CHECK(Exists(scratch.root, "acknowledged.downloading"));
        CHECK(RecordCount(scratch.journal) == 2);
    }
}

void FinalPublicationNeverOverwrites() {
    Scratch scratch;
    Write(scratch.root, "x.downloading");
    DownloadJournal journal;
    Ready(journal, scratch);
    Write(scratch.journal, "intent-00", "existing final evidence");
    JournalToken token;
    const JournalResult result = AdmitKey(journal, scratch, "x.downloading", &token);
    CHECK(result.status == JournalStatus::IoError && result.error == EEXIST);
    CHECK(token.slot == UINT32_MAX);
    CHECK(Contents(scratch.journal, "intent-00", "existing final evidence"));
    CHECK(Exists(scratch.journal, "draft-00"));
    CHECK(journal.Close().status == JournalStatus::Ok);
    DownloadJournal recovered;
    CHECK(recovered.AcquireSession(scratch.journal, scratch.root).status == JournalStatus::Ok);
    CHECK(recovered.Recover().status == JournalStatus::Corrupt);
    CHECK(Exists(scratch.journal, "draft-00"));
    CHECK(Exists(scratch.root, "x.downloading"));
}

void Run(const char* name, void (*test)()) {
    const int before = failures;
    std::printf("RUN %s\n", name);
    test();
    ++cases;
    std::printf("%s %s\n", failures == before ? "PASS" : "FAIL", name);
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    Run("independent directory OFD lock and startup gate", SessionLock);
    Run("durable admit retire and stale token", AdmitRetireAndEpoch);
    Run("replacement inode and exact logical key", ReplacedInodeAndExactKey);
    Run("missing file and missing ancestor recovery", MissingKeys);
    Run("invalid keys and stale admission identity", InvalidKeysAndIdentity);
    Run("maximum key and component sizes", MaximumKey);
    Run("final symlink and hardlink rejected", FinalSymlinkAndHardlink);
    Run("intermediate symlink cannot escape root", IntermediateSymlink);
    Run("directory and FIFO rejected without blocking", NonRegularTargets);
    Run("all record checksums verified before deletion", CorruptRecordsBlockAll);
    Run("partial unknown hardlinked symlinked records retained", PartialUnknownAndUnsafeRecords);
    Run("duplicate logical keys are corruption", DuplicateLogicalKeys);
    Run("fixed admission capacity", Capacity);
    Run("file and directory sync failure forbid ACK", AdmissionSyncFailures);
    Run("recovery interrupted after cache unlink is repeatable", RecoveryInterruptedAfterUnlink);
    Run("recovery intent removal sync failure retains evidence", RecoveryIntentRemovalSyncFailure);
    Run("retire sync failure blocks new admission", RetireIntentRemovalSyncFailure);
    Run("destruction leaves unpaid intents", ClosePreservesIntent);
    Run("admission write and publish errors keep prior debt recoverable", AdmissionWriteAndPublishErrors);
    Run("process exits at four admission cutpoints preserve recovery", AdmissionProcessCrashCutpoints);
    Run("only strictly owned drafts can be discarded", UnsafeDraftsBlockRecovery);
    Run("atomic final publication never replaces evidence", FinalPublicationNeverOverwrites);
    std::printf("%d cases, %d failures\n", cases, failures);
    return failures == 0 ? 0 : 1;
}
