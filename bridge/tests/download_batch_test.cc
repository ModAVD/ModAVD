// Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later.
// Link this translation unit with download_control.cc. It exercises the production callback,
// core, journal, pathname copying and real POSIX operations on its own scratch
// tree. Direct callbacks do not establish installed seccomp/guardian coverage.
#include "../companion/jni/download_commit.cc"
#include "../companion/jni/download_journal.cc"
#include "../companion/jni/download_listener.cc"
#include "../companion/jni/download_batch.cc"

#include <sys/wait.h>
#include <initializer_list>

namespace {
using namespace modavd::download;
namespace listener_test = modavd::download_listener;
int failures = 0;
int cases = 0;
listener_test::Health health;

#define CHECK(expression)                                                    \
    do {                                                                     \
        if (!(expression)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression);  \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

void Require(bool condition, const char* operation) {
    if (!condition) { perror(operation); exit(2); }
}

void ResetHealth() {
    health.error.store(0, std::memory_order_release);
    listener_test::g_health = &health;
    listener_test::g_installed.store(true, std::memory_order_release);
}

Request Call(long number, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0,
             uint64_t d = 0, uint64_t e = 0) {
    Request request{};
    request.owner_pid = getpid();
    request.notification.pid = static_cast<uint32_t>(syscall(SYS_gettid));
    request.notification.data.nr = static_cast<int>(number);
    request.notification.data.args[0] = a;
    request.notification.data.args[1] = b;
    request.notification.data.args[2] = c;
    request.notification.data.args[3] = d;
    request.notification.data.args[4] = e;
    return request;
}

uint64_t Address(const char* string) {
    return reinterpret_cast<uintptr_t>(string);
}

size_t Owned(const Batch& batch) {
    size_t count = 0;
    for (const Batch::Record& record : batch.records) if (record.used) ++count;
    return count;
}

size_t Intents(int directory) {
    Fd copy(openat(directory, ".", kDirectory));
    Require(copy.get() >= 0, "open journal scan");
    DIR* scan = fdopendir(copy.release());
    Require(scan != nullptr, "fdopendir journal scan");
    size_t count = 0;
    while (const dirent* entry = readdir(scan))
        if (strncmp(entry->d_name, "intent-", 7) == 0) ++count;
    Require(closedir(scan) == 0, "closedir journal scan");
    return count;
}

void RemoveOwnFiles(int directory) {
    Fd copy(openat(directory, ".", kDirectory));
    Require(copy.get() >= 0, "open scratch scan");
    DIR* scan = fdopendir(copy.release());
    Require(scan != nullptr, "fdopendir scratch scan");
    while (const dirent* entry = readdir(scan)) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        CHECK(unlinkat(directory, entry->d_name, 0) == 0);
    }
    CHECK(closedir(scan) == 0);
}

struct Scratch {
    char base[128]{};
    char scope[160]{};
    char journal_path[160]{};
    char first[192]{};
    char second[192]{};
    char published[192]{};
    Fd journal;
    Batch* batch = nullptr;

    explicit Scratch(BatchIoOps io = {}, JournalIoOps journal_io = {}) {
        ResetHealth();
#ifdef __ANDROID__
        const char* parent = "/data/local/tmp";
#else
        const char* parent = "/tmp";
#endif
        snprintf(base, sizeof(base), "%s/modavd-batch-XXXXXX", parent);
        Require(mkdtemp(base) != nullptr, "mkdtemp");
        snprintf(scope, sizeof(scope), "%s/HGDownload", base);
        snprintf(journal_path, sizeof(journal_path), "%s/journal", base);
        snprintf(first, sizeof(first), "%s/first.downloading", scope);
        snprintf(second, sizeof(second), "%s/second.downloading", scope);
        snprintf(published, sizeof(published), "%s/published", scope);
        Require(mkdir(scope, 0700) == 0, "mkdir scratch scope");
        Require(mkdir(journal_path, 0700) == 0, "mkdir scratch journal");
        journal.reset(open(journal_path, kDirectory));
        Require(journal.get() >= 0, "open scratch journal");
        batch = new Batch(Mode::Active, io, journal_io);
        batch->root.reset(open(scope, kDirectory));
        Require(batch->root.get() >= 0, "open scratch scope");
        struct stat info{};
        Require(fstat(batch->root.get(), &info) == 0, "fstat scratch scope");
        batch->root_device = static_cast<uint64_t>(info.st_dev);
        memcpy(batch->scope, scope, strlen(scope) + 1);
        Require(batch->journal.AcquireSession(journal.get(), batch->root.get()).status ==
                    JournalStatus::Ok, "acquire scratch journal");
        Require(batch->journal.Recover().status == JournalStatus::Ok, "recover empty scratch journal");
    }
    ~Scratch() {
        delete batch;
        Fd root(open(scope, kDirectory));
        Require(root.get() >= 0, "open cleanup scope");
        RemoveOwnFiles(root.get());
        RemoveOwnFiles(journal.get());
        root.reset();
        journal.reset();
        CHECK(rmdir(scope) == 0);
        CHECK(rmdir(journal_path) == 0);
        Fd wrapper(open(base, kDirectory));
        Require(wrapper.get() >= 0, "open scratch wrapper cleanup");
        RemoveOwnFiles(wrapper.get());
        wrapper.reset();
        CHECK(rmdir(base) == 0);
    }
    int File(const char* path) {
        const int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        Require(fd >= 0, "open scratch file");
        Require(write(fd, "scratch", 7) == 7, "write scratch file");
        return fd;
    }
    Decision Sync(int fd) { return batch->Handle(Call(SYS_fsync, static_cast<uint64_t>(fd))); }
    Decision Rename(const char* target) {
        return batch->Handle(Call(SYS_rename, Address(first), Address(target)));
    }
};

struct Fault {
    int error = 0;
    int calls = 0;
    bool poison_health = false;
    static int Sync(void* context, int fd) {
        auto& self = *static_cast<Fault*>(context);
        ++self.calls;
        if (self.error) return self.error;
        const int error = fsync(fd) == 0 ? 0 : errno;
        if (self.poison_health) health.error.store(ETIMEDOUT, std::memory_order_release);
        return error;
    }
};

void JournalBeforeAck() {
    Fault journal_io;
    Scratch scratch({}, {&journal_io, Fault::Sync});
    Fd source(scratch.File(scratch.first));
    const Decision reply = scratch.Sync(source.get());
    CHECK(reply.action == Decision::kResult && reply.error == 0 && reply.value == 0);
    CHECK(journal_io.calls >= 2);
    CHECK(Intents(scratch.journal.get()) == 1);
    CHECK(scratch.batch->early_ack == 1 && Owned(*scratch.batch) == 1);
    CHECK(scratch.batch->Drain("scratch cleanup", true) == 0);
}

void JournalFailureCannotAck() {
    Fault journal_io;
    journal_io.error = EIO;
    Scratch scratch({}, {&journal_io, Fault::Sync});
    Fd source(scratch.File(scratch.first));
    const Decision reply = scratch.Sync(source.get());
    CHECK(reply.action == Decision::kContinue);
    CHECK(scratch.batch->early_ack == 0 && Owned(*scratch.batch) == 0);
    CHECK(scratch.batch->permanent_strict && scratch.batch->sticky_error == EIO);
    CHECK(fcntl(source.get(), F_GETFD) >= 0);
}

void RepeatedOfdUsesOneAdmission() {
    Fault journal_io;
    Scratch scratch({}, {&journal_io, Fault::Sync});
    Fd source(scratch.File(scratch.first));
    int admission_syncs = 0;
    for (size_t index = 0; index < 64; ++index) {
        CHECK(pwrite(source.get(), "next", 4, 0) == 4);
        const Decision reply = scratch.Sync(source.get());
        CHECK(reply.action == Decision::kResult && reply.error == 0);
        if (index == 0) admission_syncs = journal_io.calls;
    }
    CHECK(Owned(*scratch.batch) == 1 && scratch.batch->early_ack == 64);
    CHECK(scratch.batch->path_checks == 1 && scratch.batch->capture_checks == 1 && admission_syncs >= 2);
    CHECK(journal_io.calls == admission_syncs);
    CHECK(!scratch.batch->permanent_strict);
    CHECK(scratch.batch->Drain("repeated OFD cleanup", true) == 0);
    CHECK(scratch.batch->real_sync == 1);
}

void SameNumberNewOfd() {
    Scratch scratch;
    int source = scratch.File(scratch.first);
    CHECK(scratch.Sync(source).action == Decision::kResult);
    const Token first = scratch.batch->records[0].token;
    const int owned = scratch.batch->records[0].handle;
    const Decision closed = scratch.batch->Handle(Call(SYS_close, static_cast<uint64_t>(source)));
    CHECK(closed.action == Decision::kResult && closed.error == 0);
    CHECK(!scratch.batch->records[0].source_valid);
    CHECK(fcntl(owned, F_GETFD) >= 0);
    int reopened = open(scratch.first, O_RDWR | O_CLOEXEC);
    Require(reopened >= 0, "reopen same scratch inode");
    if (reopened != source) {
        Require(dup2(reopened, source) == source, "reuse scratch source number");
        Require(close(reopened) == 0, "close temporary reopen");
    }
    CHECK(scratch.Sync(source).action == Decision::kResult);
    CHECK(Owned(*scratch.batch) == 2);
    CHECK(scratch.batch->capture_checks == 2);
    CHECK(scratch.batch->records[1].token.slot != first.slot ||
          scratch.batch->records[1].token.epoch != first.epoch);
    CHECK(Intents(scratch.journal.get()) == 1);
    CHECK(scratch.batch->Drain("same-inode OFD cleanup", true) == 0);
    CHECK(scratch.batch->real_sync == 2);
    CHECK(close(source) == 0);
}

void Dup2InvalidatesMapping() {
    Scratch scratch;
    Fd first(scratch.File(scratch.first));
    Fd second(scratch.File(scratch.second));
    CHECK(scratch.Sync(first.get()).action == Decision::kResult);
    const int first_owned = scratch.batch->records[0].handle;
    const Decision duplicate = scratch.batch->Handle(Call(SYS_dup2,
        static_cast<uint64_t>(second.get()), static_cast<uint64_t>(first.get())));
    CHECK(duplicate.action == Decision::kResult && duplicate.error == 0 &&
          duplicate.value == first.get());
    CHECK(!scratch.batch->records[0].source_valid);
    CHECK(scratch.Sync(first.get()).action == Decision::kResult);
    CHECK(Owned(*scratch.batch) == 2 && Intents(scratch.journal.get()) == 2);
    struct stat old_info{}, new_info{};
    CHECK(fstat(first_owned, &old_info) == 0 && fstat(first.get(), &new_info) == 0);
    CHECK(old_info.st_ino != new_info.st_ino);
    CHECK(scratch.batch->Drain("dup2 cleanup", true) == 0);
}

void NegativeCacheNeverAcknowledgesDeletedObject() {
    Scratch scratch;
    Fd source(scratch.File(scratch.first));
    CHECK(unlink(scratch.first) == 0);  // Only this test's never-admitted file.
    for (size_t index = 0; index < 64; ++index) {
        CHECK(scratch.Sync(source.get()).action == Decision::kContinue);
        CHECK(fsync(source.get()) == 0);  // Execute the forwarded real operation.
    }
    CHECK(scratch.batch->capture_checks == 1 && scratch.batch->path_checks == 1);
    CHECK(scratch.batch->negative_hits == 63 && scratch.batch->real_sync == 64);
    CHECK(scratch.batch->early_ack == 0 && Intents(scratch.journal.get()) == 0);
    CHECK(Owned(*scratch.batch) == 0);
}

void NegativeCloseAndDup2Invalidate() {
    Scratch scratch;
    int source = scratch.File(scratch.published);
    CHECK(scratch.Sync(source).action == Decision::kContinue);
    const Decision closed = scratch.batch->Handle(Call(SYS_close, static_cast<uint64_t>(source)));
    CHECK(closed.action == Decision::kResult && closed.error == 0);
    const int reopened = open(scratch.published, O_RDWR | O_CLOEXEC);
    Require(reopened >= 0, "reopen negative scratch source");
    if (reopened != source) {
        const Decision copied = scratch.batch->Handle(Call(SYS_dup2,
            static_cast<uint64_t>(reopened), static_cast<uint64_t>(source)));
        CHECK(copied.action == Decision::kResult && copied.error == 0);
        CHECK(close(reopened) == 0);
    }
    CHECK(scratch.Sync(source).action == Decision::kContinue);
    CHECK(scratch.batch->capture_checks == 2 && scratch.batch->negative_hits == 0);
    Fd target(scratch.File(scratch.first));
    const Decision copied = scratch.batch->Handle(Call(SYS_dup2,
        static_cast<uint64_t>(target.get()), static_cast<uint64_t>(source)));
    CHECK(copied.action == Decision::kResult && copied.error == 0);
    CHECK(scratch.Sync(source).action == Decision::kResult);
    CHECK(scratch.batch->capture_checks == 3 && scratch.batch->early_ack == 1);
    CHECK(scratch.batch->negative_hits == 0);
    CHECK(scratch.batch->Drain("negative replacement cleanup", true) == 0);
    CHECK(close(source) == 0);
}

void NegativeNamespaceRequiresFreshAdmission() {
    Scratch scratch;
    Fd source(scratch.File(scratch.published));
    CHECK(scratch.Sync(source.get()).action == Decision::kContinue);
    CHECK(scratch.Sync(source.get()).action == Decision::kContinue);
    CHECK(scratch.batch->negative_hits == 1);
    const Decision moved = scratch.batch->Handle(Call(SYS_rename,
        Address(scratch.published), Address(scratch.first)));
    CHECK(moved.action == Decision::kResult && moved.error == 0);
    CHECK(scratch.Sync(source.get()).action == Decision::kResult);
    CHECK(scratch.batch->capture_checks == 2 && scratch.batch->path_checks == 2);
    CHECK(scratch.batch->early_ack == 1 && Intents(scratch.journal.get()) == 1);
    CHECK(scratch.batch->Drain("negative namespace cleanup", true) == 0);
}

void NegativeCapacityFallsBackToRealChecks() {
    Scratch scratch;
    std::array<int, CommitCore::kCapacity + 1> files{};
    files[0] = scratch.File(scratch.published);
    for (size_t index = 1; index < files.size(); ++index) {
        files[index] = open(scratch.published, O_RDWR | O_CLOEXEC);
        Require(files[index] >= 0, "open independent negative source");
    }
    for (int fd : files) CHECK(scratch.Sync(fd).action == Decision::kContinue);
    CHECK(scratch.batch->capture_checks == files.size());
    CHECK(scratch.Sync(files.back()).action == Decision::kContinue);
    CHECK(scratch.batch->capture_checks == files.size() + 1);
    CHECK(scratch.Sync(files.front()).action == Decision::kContinue);
    CHECK(scratch.batch->negative_hits == 1);
    CHECK(scratch.batch->capture_checks == files.size() + 1);
    CHECK(scratch.batch->early_ack == 0 && Intents(scratch.journal.get()) == 0);
    for (int fd : files) CHECK(close(fd) == 0);
}

void NamespaceReplacementRechecksObjectIdentity() {
    Scratch scratch;
    Fd original(scratch.File(scratch.first));
    Fd replacement(scratch.File(scratch.second));
    CHECK(scratch.Sync(original.get()).action == Decision::kResult);
    const FileIdentity old_identity = scratch.batch->records[0].identity;
    const Decision replaced = scratch.batch->Handle(Call(SYS_rename,
        Address(scratch.second), Address(scratch.first)));
    CHECK(replaced.action == Decision::kResult && replaced.error == 0);
    CHECK(Owned(*scratch.batch) == 0 && Intents(scratch.journal.get()) == 0);
    CHECK(scratch.Sync(original.get()).action == Decision::kContinue);
    Fd reopened(open(scratch.first, O_RDWR | O_CLOEXEC));
    Require(reopened.get() >= 0, "open namespace replacement inode");
    CHECK(scratch.Sync(reopened.get()).action == Decision::kResult);
    CHECK(scratch.batch->early_ack == 2 && scratch.batch->capture_checks == 3);
    CHECK(!Same(scratch.batch->records[0].identity, old_identity));
    CHECK(scratch.batch->Drain("namespace identity replacement cleanup", true) == 0);
}

void StrictMutationDoesNotTouchWorkerFd() {
    Scratch scratch;
    Fd source(scratch.File(scratch.first));
    CHECK(scratch.Sync(source.get()).action == Decision::kResult);
    const Decision range = scratch.batch->Handle(Call(kCloseRange, 0, UINT_MAX, 0));
    CHECK(range.action == Decision::kContinue);
    CHECK(scratch.batch->permanent_strict && Owned(*scratch.batch) == 0);
    CHECK(Intents(scratch.journal.get()) == 0);
    const Decision closed = scratch.batch->Handle(Call(SYS_close,
                                                      static_cast<uint64_t>(source.get())));
    CHECK(closed.action == Decision::kContinue);
    CHECK(fcntl(source.get(), F_GETFD) >= 0);
    // CONTINUE belongs to the requester's possibly unshared table. This direct
    // callback test intentionally does not close the worker's same-number fd.
    CHECK(scratch.Sync(source.get()).action == Decision::kContinue);
    CHECK(scratch.batch->early_ack == 1);
}

void FailedRenameIsReported() {
    Scratch scratch;
    Fd source(scratch.File(scratch.first));
    CHECK(scratch.Sync(source.get()).action == Decision::kResult);
    char missing[192];
    snprintf(missing, sizeof(missing), "%s/missing/published", scratch.scope);
    const Decision renamed = scratch.Rename(missing);
    CHECK(renamed.action == Decision::kResult && renamed.error == ENOENT);
    CHECK(access(scratch.first, F_OK) == 0);
    CHECK(Owned(*scratch.batch) == 0 && Intents(scratch.journal.get()) == 0);
    CHECK(scratch.Sync(source.get()).action == Decision::kResult);
    CHECK(scratch.batch->early_ack == 2 && scratch.batch->path_checks == 2);
    CHECK(Intents(scratch.journal.get()) == 1);
    CHECK(scratch.batch->Drain("rename retry cleanup", true) == 0);
}

void CompletedNamespaceAllowsFreshAdmission() {
    Scratch scratch;
    Fd source(scratch.File(scratch.first));
    Fd unrelated(scratch.File(scratch.second));
    CHECK(scratch.Sync(source.get()).action == Decision::kResult);
    const Decision removed = scratch.batch->Handle(Call(SYS_unlink, Address(scratch.second)));
    CHECK(removed.action == Decision::kResult && removed.error == 0);
    CHECK(Owned(*scratch.batch) == 0 && Intents(scratch.journal.get()) == 0);
    CHECK(!scratch.batch->permanent_strict);
    CHECK(pwrite(source.get(), "later", 5, 0) == 5);
    CHECK(scratch.Sync(source.get()).action == Decision::kResult);
    CHECK(scratch.batch->early_ack == 2 && scratch.batch->path_checks == 2);
    CHECK(Intents(scratch.journal.get()) == 1);
    CHECK(scratch.batch->Drain("continued download cleanup", true) == 0);
}

void FailedFlushBlocksRename() {
    Fault flush;
    flush.error = EIO;
    Scratch scratch({&flush, Fault::Sync});
    Fd source(scratch.File(scratch.first));
    CHECK(scratch.Sync(source.get()).action == Decision::kResult);
    const Decision renamed = scratch.Rename(scratch.published);
    CHECK(renamed.action == Decision::kResult && renamed.error == EIO);
    CHECK(access(scratch.first, F_OK) == 0 && access(scratch.published, F_OK) == -1);
    CHECK(Intents(scratch.journal.get()) == 1);
    CHECK(scratch.batch->early_ack == 1 && scratch.batch->flush_errors == 1);
    CHECK(scratch.Rename(scratch.published).error == EIO);
    CHECK(flush.calls == 1);
}

void UnhealthyStopsAck() {
    Scratch scratch;
    Fd source(scratch.File(scratch.first));
    CHECK(scratch.Sync(source.get()).action == Decision::kResult);
    health.error.store(ETIMEDOUT, std::memory_order_release);
    const Decision reply = scratch.Sync(source.get());
    CHECK(reply.action == Decision::kContinue);
    CHECK(scratch.batch->permanent_strict && scratch.batch->early_ack == 1);
    CHECK(Owned(*scratch.batch) == 0 && Intents(scratch.journal.get()) == 0);
}

void HealthLostBeforeNamespaceExecute() {
    Fault flush;
    flush.poison_health = true;
    Scratch scratch({&flush, Fault::Sync});
    Fd source(scratch.File(scratch.first));
    CHECK(scratch.Sync(source.get()).action == Decision::kResult);
    const Decision renamed = scratch.Rename(scratch.published);
    CHECK(renamed.action == Decision::kResult && renamed.error == ETIMEDOUT);
    CHECK(access(scratch.first, F_OK) == 0 && access(scratch.published, F_OK) == -1);
    CHECK(scratch.batch->permanent_strict && Intents(scratch.journal.get()) == 0);
}

void CapacityDrainsBeforeStrict() {
    Scratch scratch;
    std::array<int, CommitCore::kCapacity + 1> files{};
    files[0] = scratch.File(scratch.first);
    for (size_t index = 1; index < files.size(); ++index) {
        files[index] = open(scratch.first, O_RDWR | O_CLOEXEC);
        Require(files[index] >= 0, "open independent scratch OFD");
    }
    for (size_t index = 0; index < CommitCore::kCapacity; ++index)
        CHECK(scratch.Sync(files[index]).action == Decision::kResult);
    CHECK(scratch.Sync(files.back()).action == Decision::kContinue);
    CHECK(scratch.batch->early_ack == CommitCore::kCapacity);
    CHECK(scratch.batch->real_sync == CommitCore::kCapacity);
    CHECK(scratch.batch->permanent_strict && Owned(*scratch.batch) == 0);
    CHECK(Intents(scratch.journal.get()) == 0);
    for (int fd : files) CHECK(close(fd) == 0);
}

void ForeignPointersAreNotRead() {
    Scratch scratch;
    Fd source(scratch.File(scratch.first));
    CHECK(scratch.Sync(source.get()).action == Decision::kResult);
    Request close_request = Call(SYS_close, static_cast<uint64_t>(source.get()));
    close_request.notification.pid = 0;
    CHECK(scratch.batch->Handle(close_request).action == Decision::kContinue);
    CHECK(!scratch.batch->permanent_strict && fcntl(source.get(), F_GETFD) >= 0);
    Request namespace_request = Call(SYS_rename, 1, 1);
    namespace_request.notification.pid = 0;
    CHECK(scratch.batch->Handle(namespace_request).action == Decision::kContinue);
    CHECK(scratch.batch->permanent_strict && scratch.batch->foreign_strict == 1);
    CHECK(Owned(*scratch.batch) == 0 && Intents(scratch.journal.get()) == 0);
}

void MetadataRejectsHardlinkBeforeWriting() {
    Scratch scratch;
    Fd file(scratch.File(scratch.first));
    CHECK(linkat(AT_FDCWD, scratch.first, scratch.journal.get(), ".session-next", 0) == 0);
    CHECK(DurableMetadata(scratch.journal.get(), "session", ".session-next", "nonce", 5) == EPERM);
    char content[8]{};
    CHECK(pread(file.get(), content, 7, 0) == 7 && strcmp(content, "scratch") == 0);
}

void EmptyReplacementJournalIsRejected() {
    Scratch scratch;
    Fd wrapper(open(scratch.base, kDirectory));
    Require(wrapper.get() >= 0, "open scratch wrapper");
    CHECK(BindJournal(wrapper.get(), scratch.journal.get(), scratch.scope, false) == 0);
    Require(mkdirat(wrapper.get(), "replacement", 0700) == 0, "mkdir replacement journal");
    Fd replacement(openat(wrapper.get(), "replacement", kDirectory));
    Require(replacement.get() >= 0, "open replacement journal");
    CHECK(BindJournal(wrapper.get(), replacement.get(), scratch.scope, false, false) == ESTALE);
    CHECK(unlinkat(wrapper.get(), "binding", 0) == 0);
    replacement.reset();
    CHECK(unlinkat(wrapper.get(), "replacement", AT_REMOVEDIR) == 0);
}

void ScopeSearchDoesNotRequireParentRead() {
    Scratch scratch;
    Fd base(open(scratch.base, kDirectory));
    Require(base.get() >= 0, "open traversal scratch root");
    Require(fchmod(base.get(), 0711) == 0, "make scratch root searchable");
    Require(mkdirat(base.get(), "search-only", 0700) == 0, "mkdir search-only parent");
    Fd parent(openat(base.get(), "search-only", kDirectory));
    Require(parent.get() >= 0, "open search-only setup parent");
    Require(mkdirat(parent.get(), "leaf", 0755) == 0, "mkdir readable leaf");
    Fd readable_leaf(openat(parent.get(), "leaf", kDirectory));
    Require(readable_leaf.get() >= 0 && fchmod(readable_leaf.get(), 0755) == 0,
            "set readable leaf mode independently of umask");
    readable_leaf.reset();
    Require(fchmod(parent.get(), 0111) == 0, "remove parent read permission");
    Require(symlinkat("search-only", base.get(), "alias") == 0, "create own traversal symlink");
    const pid_t child = fork();
    Require(child >= 0, "fork traversal test");
    if (child == 0) {
        // Root's DAC override would make O_RDONLY pass and hide the bug.
        if (geteuid() == 0 &&
            (setresgid(65534, 65534, 65534) != 0 || setresuid(65534, 65534, 65534) != 0))
            _exit(10);
        if (geteuid() == 0) _exit(11);
        const int listing = openat(base.get(), "search-only", kDirectory);
        if (listing >= 0) { close(listing); _exit(12); }
        if (errno != EACCES) _exit(13);
        const char* components[] = {"search-only", "leaf"};
        const int leaf = OpenDirectoryChain(base.get(), components, 2);
        if (leaf < 0) _exit(14);
        const int flags = fcntl(leaf, F_GETFL);
        if (flags < 0 || (flags & O_PATH) || fsync(leaf) != 0) _exit(15);
        close(leaf);
        const char* alias[] = {"alias", "leaf"};
        const int followed = OpenDirectoryChain(base.get(), alias, 2);
        if (followed >= 0) { close(followed); _exit(16); }
        if (errno != ELOOP && errno != ENOTDIR) _exit(17);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fprintf(stderr, "non-root directory traversal child status=%d\n", status);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(fchmod(parent.get(), 0700) == 0);
    CHECK(unlinkat(parent.get(), "leaf", AT_REMOVEDIR) == 0);
    parent.reset();
    CHECK(unlinkat(base.get(), "search-only", AT_REMOVEDIR) == 0);
    CHECK(unlinkat(base.get(), "alias", 0) == 0);
}

constexpr ControlNonce kTestNonce{0x4a2e931e5287381aULL, 0x17ef6304a0897c62ULL};

void ConfigureControlOwner(Scratch& scratch, int directory) {
    Require(scratch.batch->control.CreateOwner(directory, kTestNonce).status == ControlStatus::Ok,
            "create scratch control owner");
    scratch.batch->controlled = true;
    scratch.batch->ready_pending = true;
    Require(scratch.batch->ServiceControl() == 0, "publish scratch owner ready");
    CHECK(scratch.batch->control.AdmissionAllowed());
}

int ConfigureControlHelper(Batch& helper, int directory) {
    // These operation tests start from an already-joined helper. Startup's
    // fresh-sequence proof is exercised separately below.
    const int error = Error(helper.control.OpenHelper(directory, kTestNonce));
    if (error) return error;
    helper.controlled = true;
    helper.helper = true;
    return 0;
}

void HelperJoinNeedsLiveWorkerService() {
    Scratch scratch;
    Fd wrapper(open(scratch.base, kDirectory));
    Require(wrapper.get() >= 0, "open unserviced join wrapper");
    ConfigureControlOwner(scratch, wrapper.get());
    Fd source(scratch.File(scratch.first));
    CHECK(scratch.Sync(source.get()).action == Decision::kResult);
    const pid_t child = fork();
    Require(child >= 0, "fork unserviced join helper");
    if (child == 0) {
        Batch candidate(Mode::Observe);
        if (candidate.control.OpenHelper(wrapper.get(), kTestNonce).status != ControlStatus::Ok) _exit(50);
        const int error = CompleteHelperJoin(candidate, 50);
        if (error != ETIMEDOUT || candidate.controlled || candidate.helper ||
            candidate.mode != Mode::Observe || candidate.early_ack) _exit(51);
        candidate.control.Close();
        _exit(0);
    }
    // The owner process and READY page remain live, but deliberately do not
    // service its worker until the child has returned its bounded failure.
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fprintf(stderr, "unserviced join child status=%d\n", status);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(!scratch.batch->control.AdmissionAllowed());
    CHECK(scratch.batch->ServiceControl() == ETIMEDOUT);
    CHECK(Owned(*scratch.batch) == 0 && Intents(scratch.journal.get()) == 0);
}

int WaitControlChild(Scratch& scratch, pid_t child) {
    const uint64_t deadline = BatchClockMs() + 8000;
    for (;;) {
        int status = 0;
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            scratch.batch->ServiceControl();
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                fprintf(stderr, "control helper child status=%d\n", status);
            return WIFEXITED(status) ? WEXITSTATUS(status) : 99;
        }
        if (result < 0) return 98;
        if (BatchClockMs() >= deadline) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            return 97;
        }
        scratch.batch->ServiceControl();
        const timespec delay{0, 2000000};
        ppoll(nullptr, 0, &delay, nullptr);
    }
}

void HelperJoinUsesFreshCompletedSequence() {
    Scratch scratch;
    Fd wrapper(open(scratch.base, kDirectory));
    Require(wrapper.get() >= 0, "open serviced join wrapper");
    ConfigureControlOwner(scratch, wrapper.get());
    Fd source(scratch.File(scratch.first));
    CHECK(scratch.Sync(source.get()).action == Decision::kResult);
    ControlSnapshot before;
    CHECK(scratch.batch->control.Inspect(&before).status == ControlStatus::Ok);
    const pid_t child = fork();
    Require(child >= 0, "fork serviced join helper");
    if (child == 0) {
        Batch candidate(Mode::Observe);
        if (candidate.control.OpenHelper(wrapper.get(), kTestNonce).status != ControlStatus::Ok) _exit(60);
        if (CompleteHelperJoin(candidate, 1000) != 0 || !candidate.controlled ||
            !candidate.helper || candidate.mode != Mode::Active || candidate.early_ack) _exit(61);
        ControlSnapshot completed;
        if (candidate.control.Inspect(&completed).status != ControlStatus::Ok ||
            completed.sequence <= before.sequence ||
            (completed.state != ControlState::Finished && completed.state != ControlState::Idle)) _exit(62);
        candidate.control.Close();
        _exit(0);
    }
    CHECK(WaitControlChild(scratch, child) == 0);
    ControlSnapshot after;
    CHECK(scratch.batch->control.Inspect(&after).status == ControlStatus::Ok);
    CHECK(after.sequence == before.sequence + 1);
    CHECK(scratch.batch->control.AdmissionAllowed() && !scratch.batch->permanent_strict);
    CHECK(Owned(*scratch.batch) == 0 && Intents(scratch.journal.get()) == 0);
    CHECK(scratch.Sync(source.get()).action == Decision::kResult);
    CHECK(scratch.batch->early_ack == 2);
    CHECK(scratch.batch->Drain("serviced join cleanup", true) == 0);
}

struct BlockedHelperFlush {
    int ready = -1;
    int release = -1;
    static int Sync(void* context, int fd) {
        auto& self = *static_cast<BlockedHelperFlush*>(context);
        if (fsync(fd) != 0) return errno;
        const char marker = 'G';
        if (write(self.ready, &marker, 1) != 1) return EIO;
        char finish = 0;
        return read(self.release, &finish, 1) == 1 && finish == 'E' ? 0 : EIO;
    }
};

void HelperBarrierPausesOwnerUntilActualEnd() {
    Scratch scratch;
    Fd wrapper(open(scratch.base, kDirectory));
    Require(wrapper.get() >= 0, "open control wrapper");
    ConfigureControlOwner(scratch, wrapper.get());
    Fd source(scratch.File(scratch.first));
    Fd unrelated(scratch.File(scratch.second));
    CHECK(scratch.Sync(source.get()).action == Decision::kResult);
    int ready[2], release[2];
    Require(pipe2(ready, O_CLOEXEC) == 0 && pipe2(release, O_CLOEXEC) == 0,
            "create helper execution gates");
    const pid_t child = fork();
    Require(child >= 0, "fork real control helper");
    if (child == 0) {
        close(ready[0]); close(release[1]);
        BlockedHelperFlush gate{ready[1], release[0]};
        Batch helper(Mode::Active, {&gate, BlockedHelperFlush::Sync});
        if (ConfigureControlHelper(helper, wrapper.get())) _exit(20);
        Fd own_source(open(scratch.first, O_RDWR | O_CLOEXEC));
        if (own_source.get() < 0 || pwrite(own_source.get(), "helper", 6, 0) != 6) _exit(21);
        const Decision synced = helper.Handle(Call(SYS_fsync, static_cast<uint64_t>(own_source.get())));
        if (synced.action != Decision::kResult || synced.error != 0 || helper.early_ack != 0) _exit(22);
        char missing[192];
        snprintf(missing, sizeof(missing), "%s/missing/published", scratch.scope);
        const Decision renamed = helper.Handle(Call(SYS_rename, Address(scratch.first), Address(missing)));
        if (renamed.action != Decision::kResult || renamed.error != ENOENT) _exit(23);
        helper.control.Close();
        _exit(0);
    }
    close(ready[1]); close(release[0]);
    bool executing = false;
    const uint64_t deadline = BatchClockMs() + 7000;
    while (BatchClockMs() < deadline) {
        scratch.batch->ServiceControl();
        pollfd gate{ready[0], POLLIN, 0};
        const timespec delay{0, 2000000};
        const int polled = ppoll(&gate, 1, &delay, nullptr);
        if (polled > 0 && (gate.revents & POLLIN)) {
            char marker = 0;
            executing = read(ready[0], &marker, 1) == 1 && marker == 'G';
            break;
        }
        if (polled > 0 && (gate.revents & (POLLHUP | POLLERR))) break;
    }
    CHECK(executing);
    if (executing) {
        CHECK(!scratch.batch->control.AdmissionAllowed());
        CHECK(Owned(*scratch.batch) == 0 && Intents(scratch.journal.get()) == 0);
        CHECK(scratch.Sync(source.get()).action == Decision::kContinue);
        CHECK(scratch.batch->early_ack == 1);
        const Decision removed = scratch.batch->Handle(Call(SYS_unlink, Address(scratch.second)));
        CHECK(removed.action == Decision::kResult && removed.error == 0);
        CHECK(!scratch.batch->control.AdmissionAllowed());
        const char finish = 'E';
        CHECK(write(release[1], &finish, 1) == 1);
    }
    close(ready[0]); close(release[1]);
    CHECK(WaitControlChild(scratch, child) == 0);
    CHECK(scratch.batch->control.AdmissionAllowed() && !scratch.batch->permanent_strict);
    CHECK(scratch.Sync(source.get()).action == Decision::kResult);
    CHECK(scratch.batch->early_ack == 2 && scratch.batch->path_checks == 2);
    CHECK(scratch.batch->Drain("control normal cleanup", true) == 0);
}

void HelperForeignRequestRestrictsWholeSession() {
    Scratch scratch;
    Fd wrapper(open(scratch.base, kDirectory));
    Require(wrapper.get() >= 0, "open foreign control wrapper");
    ConfigureControlOwner(scratch, wrapper.get());
    Fd source(scratch.File(scratch.first));
    CHECK(scratch.Sync(source.get()).action == Decision::kResult);
    const pid_t child = fork();
    Require(child >= 0, "fork foreign request helper");
    if (child == 0) {
        Batch helper(Mode::Active);
        if (ConfigureControlHelper(helper, wrapper.get())) _exit(30);
        Request foreign = Call(SYS_rename, 1, 1);
        foreign.notification.pid = 0;
        const Decision reply = helper.Handle(foreign);
        if (reply.action != Decision::kContinue || !helper.permanent_strict || helper.early_ack) _exit(31);
        helper.control.Close();
        _exit(0);
    }
    CHECK(WaitControlChild(scratch, child) == 0);
    CHECK(Owned(*scratch.batch) == 0 && Intents(scratch.journal.get()) == 0);
    CHECK(scratch.batch->permanent_strict && !scratch.batch->control.AdmissionAllowed());
    CHECK(scratch.Sync(source.get()).action == Decision::kContinue);
    CHECK(scratch.batch->early_ack == 1);
}

void OwnerIoUringRestrictsWholeSession() {
    for (const long number : {425L, 426L, 427L}) {
        Scratch scratch;
        Fd wrapper(open(scratch.base, kDirectory));
        Require(wrapper.get() >= 0, "open owner io_uring wrapper");
        ConfigureControlOwner(scratch, wrapper.get());
        Fd source(scratch.File(scratch.first));
        CHECK(scratch.Sync(source.get()).action == Decision::kResult);
        const Decision result = scratch.batch->Handle(Call(number));
        CHECK(result.action == Decision::kContinue);
        CHECK(Owned(*scratch.batch) == 0 && Intents(scratch.journal.get()) == 0);
        ControlSnapshot snapshot;
        CHECK(scratch.batch->control.Inspect(&snapshot).status == ControlStatus::Ok);
        CHECK(snapshot.permanent && !scratch.batch->control.AdmissionAllowed());
        CHECK(scratch.Sync(source.get()).action == Decision::kContinue);
        CHECK(scratch.batch->early_ack == 1);
    }
}

void HelperIoUringRestrictsWholeSession() {
    for (const long number : {425L, 426L, 427L}) {
        Scratch scratch;
        Fd wrapper(open(scratch.base, kDirectory));
        Require(wrapper.get() >= 0, "open helper io_uring wrapper");
        ConfigureControlOwner(scratch, wrapper.get());
        Fd source(scratch.File(scratch.first));
        CHECK(scratch.Sync(source.get()).action == Decision::kResult);
        const pid_t child = fork();
        Require(child >= 0, "fork io_uring request helper");
        if (child == 0) {
            Batch helper(Mode::Active);
            if (ConfigureControlHelper(helper, wrapper.get())) _exit(70);
            const Decision result = helper.Handle(Call(number));
            if (result.action != Decision::kContinue || !helper.permanent_strict) _exit(71);
            helper.control.Close();
            _exit(0);
        }
        CHECK(WaitControlChild(scratch, child) == 0);
        CHECK(Owned(*scratch.batch) == 0 && Intents(scratch.journal.get()) == 0);
        CHECK(scratch.batch->permanent_strict && !scratch.batch->control.AdmissionAllowed());
        CHECK(scratch.Sync(source.get()).action == Decision::kContinue);
        CHECK(scratch.batch->early_ack == 1);
    }
}

void OwnerFlushFailureDeniesHelperPublication() {
    Fault flush;
    flush.error = EIO;
    Scratch scratch({&flush, Fault::Sync});
    Fd wrapper(open(scratch.base, kDirectory));
    Require(wrapper.get() >= 0, "open failed control wrapper");
    ConfigureControlOwner(scratch, wrapper.get());
    Fd source(scratch.File(scratch.first));
    CHECK(scratch.Sync(source.get()).action == Decision::kResult);
    const pid_t child = fork();
    Require(child >= 0, "fork denied publication helper");
    if (child == 0) {
        Batch helper(Mode::Active);
        if (ConfigureControlHelper(helper, wrapper.get())) _exit(40);
        const Decision reply = helper.Handle(Call(SYS_rename,
            Address(scratch.first), Address(scratch.published)));
        if (reply.action != Decision::kResult || reply.error != EIO) _exit(41);
        helper.control.Close();
        _exit(0);
    }
    CHECK(WaitControlChild(scratch, child) == 0);
    CHECK(access(scratch.first, F_OK) == 0 && access(scratch.published, F_OK) == -1);
    CHECK(Intents(scratch.journal.get()) == 1 && scratch.batch->sticky_error == EIO);
    CHECK(!scratch.batch->control.AdmissionAllowed());
}

template <typename Test>
void Run(const char* name, Test test) {
    const int before = failures;
    test();
    ++cases;
    printf("%s %s\n", failures == before ? "PASS" : "FAIL", name);
}
}  // namespace

int main() {
    signal(SIGPIPE, SIG_IGN);
    Run("durable journal before early ACK", JournalBeforeAck);
    Run("journal failure cannot ACK", JournalFailureCannotAck);
    Run("repeated OFD avoids new path admission", RepeatedOfdUsesOneAdmission);
    Run("same fd and inode with a new OFD", SameNumberNewOfd);
    Run("dup2 invalidates source mapping", Dup2InvalidatesMapping);
    Run("negative cache keeps deleted objects real", NegativeCacheNeverAcknowledgesDeletedObject);
    Run("negative cache invalidated by close and dup2", NegativeCloseAndDup2Invalidate);
    Run("negative namespace path change rechecks admission", NegativeNamespaceRequiresFreshAdmission);
    Run("negative capacity falls back to real checks", NegativeCapacityFallsBackToRealChecks);
    Run("namespace replacement rechecks target identity", NamespaceReplacementRechecksObjectIdentity);
    Run("strict close belongs to requester table", StrictMutationDoesNotTouchWorkerFd);
    Run("real namespace failure is returned", FailedRenameIsReported);
    Run("completed namespace permits fresh admission", CompletedNamespaceAllowsFreshAdmission);
    Run("failed fsync blocks rename", FailedFlushBlocksRename);
    Run("unhealthy listener stops early ACK", UnhealthyStopsAck);
    Run("timeout before namespace has no side effect", HealthLostBeforeNamespaceExecute);
    Run("OFD capacity drains before strict fallback", CapacityDrainsBeforeStrict);
    Run("foreign pointers are never read", ForeignPointersAreNotRead);
    Run("metadata hardlink is rejected before write", MetadataRejectsHardlinkBeforeWriting);
    Run("empty replacement journal is rejected", EmptyReplacementJournalIsRejected);
    Run("scope traversal needs search, not parent listing", ScopeSearchDoesNotRequireParentRead);
    Run("helper barrier pauses owner until real END", HelperBarrierPausesOwnerUntilActualEnd);
    Run("helper foreign request restricts global owner", HelperForeignRequestRestrictsWholeSession);
    Run("owner io_uring requests restrict global session", OwnerIoUringRestrictsWholeSession);
    Run("helper io_uring requests restrict global owner", HelperIoUringRestrictsWholeSession);
    Run("owner flush failure blocks helper publication", OwnerFlushFailureDeniesHelperPublication);
    Run("helper join requires servicing owner worker", HelperJoinNeedsLiveWorkerService);
    Run("helper join proves fresh completed sequence", HelperJoinUsesFreshCompletedSequence);
    printf("%d callback cases, %d failures; no seccomp filter installed by this test\n", cases, failures);
    return failures ? 1 : 0;
}
