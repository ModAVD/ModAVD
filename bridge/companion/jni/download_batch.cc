// Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "download_batch.h"

#include "download_commit.h"
#include "download_control.h"
#include "download_journal.h"
#include "download_listener.h"

#include <android/log.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/sched.h>
#include <new>
#include <poll.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

namespace modavd::download {
namespace {
namespace listener = modavd::download_listener;
using listener::Decision;
using listener::Request;
constexpr char kPackage[] = "com.hypergryph.arknights";
constexpr char kWrapper[] = ".modavd-download-sync";
constexpr char kJournal[] = "journal";
constexpr int kDirectory = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
constexpr int kSearchDirectory = O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
constexpr size_t kRecords = CommitCore::kCapacity;
constexpr long kCloseRange = 436;
constexpr long kClone3 = 435;
enum class Mode { Off, Observe, Active };

class Fd final {
public:
    explicit Fd(int fd = -1) noexcept : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    int release() { const int fd = fd_; fd_ = -1; return fd; }
    void reset(int fd = -1) {
        const int saved_errno = errno;
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
        errno = saved_errno;
    }
private:
    int fd_;
};

int Error(JournalResult result) {
    return result.status == JournalStatus::Ok ? 0 :
        (result.error > 0 ? result.error : EIO);
}
int Error(Result result) {
    return result.status == Status::Ok ? 0 :
        (result.error > 0 ? result.error : EIO);
}
int Error(ControlResult result) {
    return result.status == ControlStatus::Ok ? 0 :
        (result.error > 0 ? result.error : EIO);
}
bool Same(FileIdentity left, FileIdentity right) {
    return left.device == right.device && left.inode == right.inode;
}
Decision SyscallResult(long value) {
    return value == -1 ? Decision::Result(0, errno) : Decision::Result(value);
}

bool ValidKey(const char* key) {
    const size_t length = strnlen(key, DownloadJournal::kMaxKeyLength + 1);
    constexpr char suffix[] = ".downloading";
    if (length == 0 || length > DownloadJournal::kMaxKeyLength || key[0] == '/')
        return false;
    size_t start = 0;
    for (size_t index = 0; index <= length; ++index) {
        const unsigned char byte = static_cast<unsigned char>(key[index]);
        if (byte == '/' || byte == 0) {
            const size_t component = index - start;
            if (component == 0 || component > 255 ||
                (component == 1 && key[start] == '.') ||
                (component == 2 && key[start] == '.' && key[start + 1] == '.'))
                return false;
            if (byte == 0)
                return component > sizeof(suffix) - 1 &&
                    memcmp(key + length - sizeof(suffix) + 1, suffix,
                           sizeof(suffix) - 1) == 0;
            start = index + 1;
        } else if (byte < 32 || byte == 127 || byte == '\\') {
            return false;
        }
    }
    return false;
}

// Each component below the fixed scope is opened without following symlinks.
int CheckKey(int root, uint64_t device, const char* key, FileIdentity identity) {
    Fd current(fcntl(root, F_DUPFD_CLOEXEC, 0));
    if (current.get() < 0) return errno;
    const char* part = key;
    for (;;) {
        const char* slash = strchr(part, '/');
        const size_t length = slash ? static_cast<size_t>(slash - part) : strlen(part);
        char component[256];
        memcpy(component, part, length);
        component[length] = 0;
        Fd next(openat(current.get(), component,
                       slash ? kDirectory : O_PATH | O_NOFOLLOW | O_CLOEXEC));
        if (next.get() < 0) return errno;
        struct stat info{};
        if (fstat(next.get(), &info) != 0) return errno;
        if (static_cast<uint64_t>(info.st_dev) != device) return EXDEV;
        if (!slash) {
            return S_ISREG(info.st_mode) && info.st_nlink == 1 &&
                Same({static_cast<uint64_t>(info.st_dev),
                      static_cast<uint64_t>(info.st_ino)}, identity) ? 0 : ESTALE;
        }
        if (!S_ISDIR(info.st_mode)) return ENOTDIR;
        current.reset(next.release());
        part = slash + 1;
    }
}

bool IsNamespace(long number) {
    return number == SYS_rename || number == SYS_renameat ||
        number == SYS_renameat2 || number == SYS_link || number == SYS_linkat ||
        number == SYS_unlink || number == SYS_unlinkat;
}
bool IsFdMutation(long number) {
    return number == SYS_close || number == SYS_dup2 || number == SYS_dup3 ||
        number == kCloseRange;
}
bool IsIoUring(long number) {
    return number == 425 || number == 426 || number == 427;
}

// Copy only the owner process's address space. Page-bounded reads avoid
// rejecting a terminated string merely because the following page is absent.
int CopyString(uint64_t address, char (&out)[PATH_MAX + 1]) {
    if (!address || address > UINTPTR_MAX) return EFAULT;
    size_t copied = 0;
    while (copied <= PATH_MAX) {
        if (address > UINTPTR_MAX - copied) return EFAULT;
        const uintptr_t remote_address = static_cast<uintptr_t>(address) + copied;
        size_t count = 4096 - (remote_address & 4095U);
        if (count > 256) count = 256;
        if (count > PATH_MAX + 1 - copied) count = PATH_MAX + 1 - copied;
        iovec local{out + copied, count};
        iovec remote{reinterpret_cast<void*>(remote_address), count};
        const ssize_t read = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
        if (read <= 0) return read < 0 ? errno : EFAULT;
        if (memchr(out + copied, 0, static_cast<size_t>(read))) return 0;
        copied += static_cast<size_t>(read);
    }
    return ENAMETOOLONG;
}

int ReadStatus(pid_t tid, char (&out)[8192]) {
    char path[80];
    snprintf(path, sizeof(path), "/proc/self/task/%d/status", tid);
    Fd fd(open(path, O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0) return errno;
    const ssize_t count = read(fd.get(), out, sizeof(out) - 1);
    if (count <= 0 || count == static_cast<ssize_t>(sizeof(out) - 1)) return EIO;
    out[count] = 0;
    return 0;
}

bool SameStatusField(const char* left, const char* right, const char* field) {
    const char* a = strstr(left, field);
    const char* b = strstr(right, field);
    if (!a || !b) return false;
    const char* end_a = strchr(a + 1, '\n');
    const char* end_b = strchr(b + 1, '\n');
    return end_a && end_b && end_a - a == end_b - b &&
        memcmp(a, b, static_cast<size_t>(end_a - a)) == 0;
}

bool SameCredentials(pid_t tid) {
    char owner[8192], worker[8192];
    if (ReadStatus(tid, owner) ||
        ReadStatus(static_cast<pid_t>(syscall(SYS_gettid)), worker)) return false;
    return SameStatusField(owner, worker, "\nUid:") &&
        SameStatusField(owner, worker, "\nGid:") &&
        SameStatusField(owner, worker, "\nGroups:") &&
        SameStatusField(owner, worker, "\nCapEff:");
}

struct NamespaceCall {
    long number = 0;
    Fd first_directory;
    Fd second_directory;
    int first_fd = AT_FDCWD;
    int second_fd = AT_FDCWD;
    unsigned flags = 0;
    char first[PATH_MAX + 1]{};
    char second[PATH_MAX + 1]{};

    int CaptureDirectory(int source, const char* path, Fd& owned, int* result) {
        if (path[0] == '/') { *result = AT_FDCWD; return 0; }
        owned.reset(source == AT_FDCWD ? open(".", kDirectory) :
                    fcntl(source, F_DUPFD_CLOEXEC, 0));
        if (owned.get() < 0) return errno;
        *result = owned.get();
        return 0;
    }

    int Capture(const seccomp_data& data) {
        number = data.nr;
        int first_source = AT_FDCWD, second_source = AT_FDCWD;
        uint64_t first_address = data.args[0], second_address = data.args[1];
        bool two_paths = number != SYS_unlink && number != SYS_unlinkat;
        if (number == SYS_renameat || number == SYS_renameat2 || number == SYS_linkat) {
            first_source = static_cast<int>(data.args[0]);
            first_address = data.args[1];
            second_source = static_cast<int>(data.args[2]);
            second_address = data.args[3];
            if (number != SYS_renameat) flags = static_cast<unsigned>(data.args[4]);
        } else if (number == SYS_unlinkat) {
            first_source = static_cast<int>(data.args[0]);
            first_address = data.args[1];
            flags = static_cast<unsigned>(data.args[2]);
        }
        int error = CopyString(first_address, first);
        if (!error && two_paths) error = CopyString(second_address, second);
        if (!error) error = CaptureDirectory(first_source, first, first_directory, &first_fd);
        if (!error && two_paths)
            error = CaptureDirectory(second_source, second, second_directory, &second_fd);
        return error;
    }

    Decision Execute() {
        if (number == SYS_renameat2)
            return SyscallResult(syscall(SYS_renameat2, first_fd, first,
                                         second_fd, second, flags));
        if (number == SYS_rename || number == SYS_renameat)
            return SyscallResult(syscall(SYS_renameat, first_fd, first,
                                         second_fd, second));
        if (number == SYS_link || number == SYS_linkat)
            return SyscallResult(syscall(SYS_linkat, first_fd, first,
                                         second_fd, second, flags));
        return SyscallResult(syscall(SYS_unlinkat, first_fd, first, flags));
    }
};

// The production boundary uses real syscalls. A configured I/O implementation
// can also report a positive errno without changing ownership/state handling.
struct BatchIoOps {
    void* context = nullptr;
    int (*fsync)(void*, int) = nullptr;
};

class Batch final {
public:
    struct Record {
        bool used = false;
        bool source_valid = false;
        int source = -1;
        int handle = -1;
        Token token{};
        FileIdentity identity{};
        size_t key = 0;
    };
    struct Key {
        bool used = false;
        JournalToken token{};
        char name[DownloadJournal::kMaxKeyLength + 1]{};
    };
    struct NegativeSource { int source = -1; };
    explicit Batch(Mode selected, BatchIoOps io = {}, JournalIoOps journal_io = {})
        : mode(selected), journal(journal_io), core({this, Capture, Close}), io_(io) {}

    static int Capture(void* context, int source, CapturedHandle* out) {
        auto& self = *static_cast<Batch*>(context);
        out->handle = fcntl(source, F_DUPFD_CLOEXEC, 0);
        if (out->handle < 0) return errno;
        struct stat info{};
        if (fstat(out->handle, &info) != 0) return errno;
        out->identity = {static_cast<uint64_t>(info.st_dev),
                         static_cast<uint64_t>(info.st_ino)};
        out->identity_valid = S_ISREG(info.st_mode) && info.st_nlink == 1;
        self.last_capture = out->handle;
        return out->identity_valid ? 0 : EINVAL;
    }
    static int Close(void*, int handle) { return close(handle) == 0 ? 0 : errno; }
    static Decision Callback(const Request& request, void* context) {
        return static_cast<Batch*>(context)->Handle(request);
    }
    static void Tick(void* context) {
        static_cast<Batch*>(context)->ServiceControl();
    }
    static int ControlDrain(void* context, bool permanent) {
        return static_cast<Batch*>(context)->Drain("shared helper barrier", permanent);
    }

    int ServiceControl() {
        if (!controlled || helper) return 0;
        if (sticky_error) return sticky_error;
        if (ready_pending) {
            const int error = Error(control.PublishReady());
            if (error) return Poison(error, "control ready failed");
            ready_pending = false;
        }
        if (!listener::Healthy()) {
            control.Restrict();
            const int error = Drain("owner guardian unhealthy", true);
            if (error) return Poison(error, "owner guardian drain failed");
        }
        const ControlResult service = control.Service(ControlDrain, this);
        const int error = service.status == ControlStatus::Busy ? 0 : Error(service);
        if (error) {
            // Restriction alone is not repayment. Try to pay any old debt even
            // when a vanished/timed-out helper has already poisoned the page.
            const int drain_error = Drain("control failure drain", true);
            return Poison(drain_error ? drain_error : error, "control service failed");
        }
        ControlSnapshot snapshot;
        const int inspected = Error(control.Inspect(&snapshot));
        if (inspected) return Poison(inspected, "control snapshot failed");
        if (snapshot.permanent) permanent_strict = true;
        return 0;
    }

    void Log(const char* reason) const {
        __android_log_print(ANDROID_LOG_INFO, "ModAvdDownload",
            "%s mode=%s role=%s strict=%d earlyACK=%llu real=%llu keys=%llu flusherr=%llu foreignstrict=%llu pathchecks=%llu capture_checks=%llu negative_hits=%llu",
            reason, mode == Mode::Active ? "active" : "observe", helper ? "helper" : "owner", permanent_strict,
            static_cast<unsigned long long>(early_ack),
            static_cast<unsigned long long>(real_sync),
            static_cast<unsigned long long>(keys_admitted),
            static_cast<unsigned long long>(flush_errors),
            static_cast<unsigned long long>(foreign_strict),
            static_cast<unsigned long long>(path_checks),
            static_cast<unsigned long long>(capture_checks),
            static_cast<unsigned long long>(negative_hits));
    }

    // All admitted OFDs are paid before any intent is retired. A failed or
    // ambiguous operation poisons the session and leaves recovery ownership.
    int Drain(const char* reason, bool strict) {
        ClearNegative();
        permanent_strict = permanent_strict || strict;
        if (sticky_error) return sticky_error;
        int first_error = 0;
        for (Record& record : records) {
            if (!record.used) continue;
            CommitTicket ticket;
            const Result begin = core.BeginCommit(record.token, &ticket);
            if (begin.status == Status::Clean) continue;
            if (begin.status != Status::Ok) {
                if (!first_error) first_error = Error(begin);
                continue;
            }
            ++real_sync;
            const int fsync_error = io_.fsync ? io_.fsync(io_.context, ticket.handle) :
                (syscall(SYS_fsync, ticket.handle) == 0 ? 0 : errno);
            const Result completed = core.CompleteCommit(ticket, fsync_error);
            if (completed.status != Status::Ok) {
                ++flush_errors;
                if (!first_error) first_error = Error(completed);
            }
        }
        if (first_error) return Poison(first_error, reason);
        for (Key& key : keys) {
            if (!key.used) continue;
            const int error = Error(journal.Retire(key.token));
            if (error) return Poison(error, "intent retirement failed");
            key.used = false;
        }
        for (Record& record : records) {
            if (!record.used) continue;
            const int error = Error(core.Retire(record.token));
            if (error) return Poison(error, "owned close failed");
            record = {};
        }
        Log(reason);
        return 0;
    }

    int Poison(int error, const char* reason) {
        permanent_strict = true;
        if (!sticky_error) {
            sticky_error = error > 0 ? error : EIO;
            if (controlled) control.Poison(sticky_error);
            Log(reason);
        }
        return sticky_error;
    }
    Decision Strict(const char* reason) {
        if (controlled && helper) return SharedStrict(reason);
        if (controlled) {
            const int error = Error(control.Restrict());
            if (error) return Decision::Result(0, Poison(error, "shared restriction failed"));
        }
        const int error = Drain(reason, true);
        return error ? Decision::Result(0, EIO) : Decision::Continue();
    }
    Decision SharedStrict(const char* reason) {
        ControlTicket ticket;
        const int error = Error(control.Begin(ControlKind::Permanent, &ticket));
        if (error) return Decision::Result(0, Poison(error, "helper strict barrier failed"));
        // CONTINUE has no completion report. Permanent restriction was made
        // global and prior debt was repaid before the grant. Do not call it a
        // successful emulated operation when returning the helper lock.
        const int ended = Error(control.FinishHandoff(ticket));
        permanent_strict = true;
        Log(reason);
        return ended ? Decision::Result(0, Poison(ended, "helper strict end failed")) :
            Decision::Continue();
    }

    Decision FinishHelper(const ControlTicket& ticket, Decision result, bool sync) {
        if (sync && (result.error == EIO || result.error == ENOSPC || result.error == EDQUOT))
            control.Restrict();
        const int error = Error(control.End(ticket, result.error));
        return error ? Decision::Result(0, Poison(error, "helper completion failed")) : result;
    }

    Decision HelperHandle(const Request& request, bool own_context) {
        const auto& data = request.notification.data;
        const long number = data.nr;
        if (!own_context) {
            if (IsFdMutation(number) || number == SYS_clone || number == kClone3 ||
                number == SYS_unshare || number == SYS_setns) return Decision::Continue();
            ++foreign_strict;
            return SharedStrict("helper foreign process request");
        }
        if (number == SYS_clone) {
            const uint64_t flags = data.args[0];
            const bool thread = (flags & CLONE_THREAD) != 0;
            const bool files = (flags & CLONE_FILES) != 0;
            const bool fs = (flags & CLONE_FS) != 0;
            if ((thread && (!files || !fs)) || (!thread && files))
                return SharedStrict("helper unsupported clone sharing");
            return Decision::Continue();
        }
        if (number == kClone3 || number == SYS_unshare || number == SYS_setns ||
            number == kCloseRange || IsIoUring(number))
            return SharedStrict("helper unsupported context change");
        if (IsFdMutation(number)) return Decision::Continue();
        if (permanent_strict) return SharedStrict("helper strict context operation");
        if (number == SYS_fsync) {
            Fd captured(fcntl(static_cast<int>(data.args[0]), F_DUPFD_CLOEXEC, 0));
            if (captured.get() < 0) return SharedStrict("helper fsync capture failed");
            ControlTicket ticket;
            const int error = Error(control.Begin(ControlKind::Normal, &ticket));
            if (error) return Decision::Result(0, Poison(error, "helper fsync barrier failed"));
            if (!listener::Healthy()) {
                control.Restrict();
                return FinishHelper(ticket, Decision::Result(0, ETIMEDOUT), true);
            }
            ++real_sync;
            const int fsync_error = io_.fsync ? io_.fsync(io_.context, captured.get()) :
                (syscall(SYS_fsync, captured.get()) == 0 ? 0 : errno);
            return FinishHelper(ticket, Decision::Result(0, fsync_error), true);
        }
        if (IsNamespace(number)) {
            if (!SameCredentials(static_cast<pid_t>(request.notification.pid)))
                return SharedStrict("helper namespace credentials differ");
            NamespaceCall operation;
            if (operation.Capture(data)) return SharedStrict("helper namespace capture failed");
            ControlTicket ticket;
            const int error = Error(control.Begin(ControlKind::Normal, &ticket));
            if (error) return Decision::Result(0, Poison(error, "helper namespace barrier failed"));
            if (!listener::Healthy()) {
                control.Restrict();
                return FinishHelper(ticket, Decision::Result(0, ETIMEDOUT), false);
            }
            return FinishHelper(ticket, operation.Execute(), false);
        }
        return SharedStrict("helper unknown operation");
    }
    void Invalidate(int source) {
        for (Record& record : records)
            if (record.used && record.source == source) record.source_valid = false;
        for (NegativeSource& entry : negative_sources)
            if (entry.source == source) entry.source = -1;
    }
    void ClearNegative() {
        for (NegativeSource& entry : negative_sources) entry.source = -1;
    }
    bool IsNegative(int source) const {
        for (const NegativeSource& entry : negative_sources)
            if (source >= 0 && entry.source == source) return true;
        return false;
    }
    void RememberNegative(int source) {
        for (NegativeSource& entry : negative_sources) {
            if (entry.source == source) return;
            if (entry.source < 0) { entry.source = source; return; }
        }
        // Full means another capture/check next time, never a synthetic ACK.
    }
    bool HasOwnedRecords() const {
        for (const Record& record : records) if (record.used) return true;
        return false;
    }

    int Eligible(int handle, char (&key)[DownloadJournal::kMaxKeyLength + 1],
                 FileIdentity* identity) {
        ++path_checks;
        struct stat info{};
        if (fstat(handle, &info) != 0) return errno;
        const int flags = fcntl(handle, F_GETFL);
        if (flags < 0) return errno;
        if (!S_ISREG(info.st_mode) || info.st_nlink != 1 ||
            (flags & O_ACCMODE) == O_RDONLY || (flags & O_PATH)) return EINVAL;
        char proc[64], path[PATH_MAX + 1];
        snprintf(proc, sizeof(proc), "/proc/self/fd/%d", handle);
        const ssize_t length = readlink(proc, path, PATH_MAX);
        if (length <= 0 || length == PATH_MAX) return length < 0 ? errno : ENAMETOOLONG;
        path[length] = 0;
        const size_t prefix_length = strlen(scope);
        if (static_cast<size_t>(length) <= prefix_length ||
            memcmp(path, scope, prefix_length) != 0 || path[prefix_length] != '/')
            return EXDEV;
        const char* relative = path + prefix_length + 1;
        if (!ValidKey(relative)) return EINVAL;
        *identity = {static_cast<uint64_t>(info.st_dev),
                     static_cast<uint64_t>(info.st_ino)};
        const int error = CheckKey(root.get(), root_device, relative, *identity);
        if (error) return error;
        memcpy(key, relative, strlen(relative) + 1);
        return 0;
    }

    Decision Ack(Record& record) {
        const Result dirty = core.MarkDirty(record.token);
        if (dirty.status != Status::Ok) {
            Poison(Error(dirty), "generation admission failed");
            ++real_sync;
            return Decision::Continue();
        }
        if (!listener::Healthy()) {
            const Decision strict = Strict("guardian became unhealthy before ACK");
            return strict.error ? strict : Decision::Result(0, ETIMEDOUT);
        }
        if (controlled && !control.AdmissionAllowed()) {
            const int serviced = ServiceControl();
            const int error = serviced ? serviced : Drain("shared admission paused before ACK", false);
            if (error) return Decision::Result(0, error);
            ++real_sync;
            return Decision::Continue();
        }
        ++early_ack;
        return Decision::Result(0);
    }

    Decision Sync(int source) {
        if (sticky_error) return Decision::Result(0, sticky_error);
        if (permanent_strict) { ++real_sync; return Decision::Continue(); }
        if (controlled) {
            const int serviced = ServiceControl();
            if (serviced) return Decision::Result(0, serviced);
            if (!control.AdmissionAllowed()) {
                const int error = Drain("shared admission paused", false);
                if (error) return Decision::Result(0, error);
                ++real_sync;
                return Decision::Continue();
            }
        }
        Record* available = nullptr;
        for (Record& record : records) {
            if (!record.used && !available) available = &record;
            // Only a validated, durably admitted source creates this mapping.
            // The coordinator invalidates it before real close/dup replacement;
            // every namespace barrier removes it. Unsupported table/context
            // sharing first drains and permanently disables this path.
            if (record.used && record.source_valid && record.source == source)
                return Ack(record);
        }
        if (IsNegative(source)) {
            ++negative_hits;
            ++real_sync;
            return Decision::Continue();
        }
        ++capture_checks;
        Fd captured(fcntl(source, F_DUPFD_CLOEXEC, 0));
        if (captured.get() < 0) {
            if (HasOwnedRecords()) return Strict("capture failed with outstanding ownership");
            ++real_sync;
            return Decision::Continue();
        }
        struct stat info{};
        if (fstat(captured.get(), &info) != 0) {
            if (HasOwnedRecords()) return Strict("owned identity could not be checked");
            ++real_sync;
            return Decision::Continue();
        }
        FileIdentity identity{static_cast<uint64_t>(info.st_dev),
                              static_cast<uint64_t>(info.st_ino)};
        for (Record& record : records) {
            if (!record.used) continue;
            if (Same(record.identity, identity) &&
                (!S_ISREG(info.st_mode) || info.st_nlink != 1))
                return Strict("owned object no longer eligible");
        }
        char name[DownloadJournal::kMaxKeyLength + 1];
        if (Eligible(captured.get(), name, &identity)) {
            for (const Record& record : records)
                if (record.used && Same(record.identity, identity))
                    return Strict("owned path eligibility changed");
            RememberNegative(source);
            ++real_sync;
            return Decision::Continue();
        }
        {
            if (!available) return Strict("OFD capacity exhausted");
            Key* key = nullptr;
            Key* empty_key = nullptr;
            for (Key& candidate : keys) {
                if (!candidate.used && !empty_key) empty_key = &candidate;
                if (candidate.used && strcmp(candidate.name, name) == 0) key = &candidate;
            }
            if (!key) {
                if (!empty_key) return Strict("key capacity exhausted");
                JournalToken token;
                const int error = Error(journal.Admit(name, identity.device,
                                                       identity.inode, &token));
                if (error) {
                    Poison(error, "intent admission failed");
                    // An admission failure never creates a synthetic success.
                    ++real_sync;
                    return Decision::Continue();
                }
                key = empty_key;
                key->used = true;
                key->token = token;
                memcpy(key->name, name, strlen(name) + 1);
                ++keys_admitted;
            }
            Token token;
            const Result tracked = core.Track(captured.get(), &token);
            if (tracked.status != Status::Ok) {
                // A durable intent already exists. Keep it for recovery if
                // capture failed; no previously admitted OFD may be forgotten.
                Poison(Error(tracked), "core capture failed");
                ++real_sync;
                return Decision::Continue();
            }
            // An earlier strict source may alias a newly admitted OFD after
            // changing eligibility. Recheck it before any real fsync could
            // consume that OFD's writeback-error cursor behind the core.
            ClearNegative();
            available->used = true;
            available->source_valid = true;
            available->source = source;
            available->handle = last_capture;
            available->token = token;
            available->identity = identity;
            available->key = static_cast<size_t>(key - keys.data());
            return Ack(*available);
        }
    }

    Decision Handle(const Request& request) {
        const auto& data = request.notification.data;
        const long number = data.nr;
        if (mode == Mode::Observe) {
            if (number == SYS_fsync) ++real_sync;
            if (IsNamespace(number) && (++namespace_requests == 1 || namespace_requests % 128 == 0))
                Log("observed namespace requests");
            return Decision::Continue();
        }
        if (controlled && !helper) ServiceControl();
        if (!listener::Healthy()) {
            return Strict("guardian unhealthy");
        }
        // Signal zero checks group membership without sending a signal. The
        // experiment initializes before game threads; clone/unshare barriers
        // prevent later unsupported owner file-table or fs-context changes.
        const bool owner = syscall(SYS_tgkill, getpid(), request.notification.pid, 0) == 0;
        if (controlled && helper) return HelperHandle(request, owner);
        if (!owner) {
            if (IsFdMutation(number) || number == SYS_clone || number == kClone3 ||
                number == SYS_unshare || number == SYS_setns) return Decision::Continue();
            if (!permanent_strict) ++foreign_strict;
            return Strict("foreign process request");
        }
        if (number == SYS_clone) {
            const uint64_t flags = data.args[0];
            const bool thread = (flags & CLONE_THREAD) != 0;
            const bool files = (flags & CLONE_FILES) != 0;
            const bool fs = (flags & CLONE_FS) != 0;
            if ((thread && (!files || !fs)) || (!thread && files))
                return Strict("unsupported clone sharing");
            return Decision::Continue();
        }
        if (number == kClone3 || number == SYS_unshare || number == SYS_setns ||
            number == kCloseRange || IsIoUring(number))
            return Strict("unsupported file table or namespace change");
        if (number == SYS_close || number == SYS_dup2 || number == SYS_dup3) {
            if (permanent_strict) return Decision::Continue();
            Invalidate(static_cast<int>(data.args[number == SYS_close ? 0 : 1]));
            // No additional game fsync on close. Owned duplicates keep debt.
            if (number == SYS_close) {
                const Decision result = SyscallResult(syscall(number, static_cast<int>(data.args[0])));
                if (HasOwnedRecords() &&
                    (result.error == EIO || result.error == ENOSPC || result.error == EDQUOT))
                    Poison(result.error, "game close reported writeback failure");
                return result;
            }
            return SyscallResult(syscall(number, static_cast<int>(data.args[0]),
                static_cast<int>(data.args[1]), static_cast<unsigned>(data.args[2])));
        }
        if (number == SYS_fsync) return Sync(static_cast<int>(data.args[0]));
        if (IsNamespace(number)) {
            if (sticky_error) return Decision::Result(0, sticky_error);
            if (permanent_strict) return Decision::Continue();
            if (!SameCredentials(static_cast<pid_t>(request.notification.pid)))
                return Strict("namespace credentials not shared");
            NamespaceCall operation;
            if (operation.Capture(data)) return Strict("namespace argument capture failed");
            const int error = Drain("namespace barrier", false);
            if (error) return Decision::Result(0, error);
            if (!listener::Healthy()) {
                permanent_strict = true;
                return Decision::Result(0, ETIMEDOUT);
            }
            // The worker's bypass executes exactly once. The callback remains
            // occupied until completion; CONTINUE never races new admission.
            return operation.Execute();
        }
        return Strict("unknown intercepted operation");
    }

    Mode mode;
    DownloadJournal journal;
    CommitCore core;
    DownloadControl control;
    Fd root;
    uint64_t root_device = 0;
    char scope[512]{};
    bool permanent_strict = false;
    bool controlled = false;
    bool helper = false;
    bool ready_pending = false;
    int sticky_error = 0;
    int last_capture = -1;
    std::array<Record, kRecords> records{};
    std::array<Key, kRecords> keys{};
    std::array<NegativeSource, kRecords> negative_sources{};
    uint64_t early_ack = 0, real_sync = 0, keys_admitted = 0;
    uint64_t flush_errors = 0, foreign_strict = 0, namespace_requests = 0, path_checks = 0;
    uint64_t capture_checks = 0, negative_hits = 0;
    BatchIoOps io_;
};

int PrivateRoot(const char* private_dir, char (&root)[PATH_MAX + 1], unsigned* user) {
    if (!private_dir) return EINVAL;
    size_t length = strnlen(private_dir, PATH_MAX + 1);
    if (!length || length > PATH_MAX) return EINVAL;
    while (length && private_dir[length - 1] == '/') --length;
    constexpr char suffix[] = "/code_cache";
    if (length >= sizeof(suffix) - 1 &&
        memcmp(private_dir + length - sizeof(suffix) + 1, suffix, sizeof(suffix) - 1) == 0)
        length -= sizeof(suffix) - 1;
    memcpy(root, private_dir, length);
    root[length] = 0;
    const char* position = nullptr;
    if (strncmp(root, "/data/user/", 11) == 0) position = root + 11;
    else if (strncmp(root, "/data/user_de/", 14) == 0) position = root + 14;
    else if (strncmp(root, "/data/data/", 11) == 0) {
        if (strcmp(root + 11, kPackage) || getuid() / 100000 != 0) return EINVAL;
        *user = 0;
        return 0;
    } else return EINVAL;
    if (*position < '0' || *position > '9') return EINVAL;
    unsigned value = 0;
    while (*position >= '0' && *position <= '9') {
        const unsigned digit = static_cast<unsigned>(*position++ - '0');
        if (value > (UINT_MAX - digit) / 10) return EINVAL;
        value = value * 10 + digit;
    }
    if (*position++ != '/' || strcmp(position, kPackage) || value != getuid() / 100000)
        return EINVAL;
    *user = value;
    return 0;
}

int HasEntries(int fd, bool* entries) {
    Fd duplicate(openat(fd, ".", kDirectory));
    if (duplicate.get() < 0) return errno;
    DIR* directory = fdopendir(duplicate.get());
    if (!directory) return errno;
    duplicate.release();
    *entries = false;
    errno = 0;
    while (const dirent* entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            *entries = true;
            break;
        }
    }
    const int error = errno;
    closedir(directory);
    return error;
}

int WriteAll(int fd, const char* bytes, size_t length) {
    while (length) {
        const ssize_t written = write(fd, bytes, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return written < 0 ? errno : EIO;
        bytes += written;
        length -= static_cast<size_t>(written);
    }
    return 0;
}

int ReadMetadata(int directory, const char* name, char* out, size_t capacity,
                 size_t* length, bool* exists) {
    Fd fd(openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (fd.get() < 0) {
        if (errno == ENOENT) { *exists = false; *length = 0; return 0; }
        return errno;
    }
    struct stat info{};
    if (fstat(fd.get(), &info) != 0) return errno;
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_uid != getuid()) return EPERM;
    const ssize_t count = read(fd.get(), out, capacity);
    if (count < 0) return errno;
    if (static_cast<size_t>(count) == capacity) return EILSEQ;
    *exists = true;
    *length = static_cast<size_t>(count);
    return 0;
}

int DurableMetadata(int directory, const char* final_name, const char* temporary_name,
                    const char* bytes, size_t length) {
    // These names belong to this bridge, never to a game cache key.
    struct stat stale{};
    if (fstatat(directory, temporary_name, &stale, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(stale.st_mode) || stale.st_nlink != 1 || stale.st_uid != getuid())
            return EPERM;
        if (unlinkat(directory, temporary_name, 0) != 0 || fsync(directory) != 0) return errno;
    } else if (errno != ENOENT) return errno;
    Fd fd(openat(directory, temporary_name,
                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (fd.get() < 0) return errno;
    struct stat info{};
    if (fstat(fd.get(), &info) != 0) return errno;
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_uid != getuid()) return EPERM;
    int error = WriteAll(fd.get(), bytes, length);
    if (!error && fsync(fd.get()) != 0) error = errno;
    if (!error && renameat(directory, temporary_name, directory, final_name) != 0) error = errno;
    if (!error && fsync(directory) != 0) error = errno;
    return error;
}

int BindJournal(int wrapper, int journal, const char* scope, bool records, bool create = true) {
    struct stat info{};
    if (fstat(journal, &info) != 0) return errno;
    char expected[768];
    const int wanted = snprintf(expected, sizeof(expected), "MODAVD-DOWNLOAD-1\n%llu %llu\n%s\n",
        static_cast<unsigned long long>(info.st_dev),
        static_cast<unsigned long long>(info.st_ino), scope);
    if (wanted < 0 || static_cast<size_t>(wanted) >= sizeof(expected)) return EINVAL;
    char previous[768];
    size_t length = 0;
    bool exists = false;
    int error = ReadMetadata(wrapper, "binding", previous, sizeof(previous), &length, &exists);
    if (error) return error;
    if (exists) return length == static_cast<size_t>(wanted) &&
        memcmp(previous, expected, length) == 0 ? 0 : ESTALE;
    // Existing debt or a consumed controller token cannot be adopted into a
    // replacement empty journal as if its history were known.
    struct stat session{};
    if (records || fstatat(wrapper, "session", &session, AT_SYMLINK_NOFOLLOW) == 0)
        return ESTALE;
    if (errno != ENOENT) return errno;
    if (!create) return 0;
    error = DurableMetadata(wrapper, "binding", ".binding-next", expected,
                            static_cast<size_t>(wanted));
    return error;
}

int ConsumeSession(int wrapper) {
    char nonce[PROP_VALUE_MAX]{};
    ControlNonce parsed;
    if (__system_property_get("modavd.download.session", nonce) != 32 ||
        !ParseControlNonce(nonce, &parsed)) return EPERM;
    char previous[64];
    size_t length = 0;
    bool exists = false;
    int error = ReadMetadata(wrapper, "session", previous, sizeof(previous), &length, &exists);
    if (error) return error;
    if (exists) {
        if (length != 33 || previous[32] != '\n') return EILSEQ;
        if (memcmp(previous, nonce, 32) == 0) return EALREADY;
    }
    nonce[32] = '\n';
    return DurableMetadata(wrapper, "session", ".session-next", nonce, 33);
}

int ControllerNonce(char (&text)[PROP_VALUE_MAX], ControlNonce* nonce) {
    if (__system_property_get("modavd.download.session", text) != 32 ||
        !ParseControlNonce(text, nonce)) return EPERM;
    return 0;
}

uint64_t BatchClockMs() {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000;
}

int CompleteHelperJoin(Batch& batch, unsigned remaining_ms) {
    // A stable READY page and a live pidfd do not prove that the owner's
    // callback worker is servicing this session. Require a fresh sequence,
    // real debt repayment and GRANT before publishing the helper role.
    ControlTicket ticket;
    int error = Error(batch.control.Begin(ControlKind::Normal, &ticket, remaining_ms));
    if (error) return error;
    // This initialization probe has no game syscall. Its empty operation has
    // completed, so END(0) reports that exact boundary rather than CONTINUE.
    error = Error(batch.control.End(ticket, 0));
    if (error) return error;
    batch.controlled = true;
    batch.helper = true;
    batch.mode = Mode::Active;
    return 0;
}

int JoinHelper(Batch& batch, int wrapper) {
    char text[PROP_VALUE_MAX]{};
    ControlNonce nonce;
    int error = ControllerNonce(text, &nonce);
    if (error) return error;
    const uint64_t started = BatchClockMs();
    if (!started) return EIO;
    for (;;) {
        char consumed[64];
        size_t length = 0;
        bool exists = false;
        error = ReadMetadata(wrapper, "session", consumed, sizeof(consumed), &length, &exists);
        if (error) return error;
        if (exists && (length != 33 || consumed[32] != '\n')) return EILSEQ;
        if (exists && memcmp(consumed, text, 32) == 0) {
            const ControlResult joined = batch.control.OpenHelper(wrapper, nonce);
            if (joined.status == ControlStatus::Ok) {
                ControlSnapshot snapshot;
                error = Error(batch.control.Inspect(&snapshot));
                if (error) return error;
                if (snapshot.state == ControlState::Poisoned)
                    return snapshot.error > 0 ? snapshot.error : EIO;
                if (snapshot.state != ControlState::Booting) {
                    const uint64_t current = BatchClockMs();
                    if (!current || current - started >= 5000) {
                        batch.control.Poison(ETIMEDOUT);
                        return ETIMEDOUT;
                    }
                    const unsigned remaining = 5000 - static_cast<unsigned>(current - started);
                    return CompleteHelperJoin(batch, remaining);
                }
                const uint64_t current = BatchClockMs();
                if (!current || current - started >= 5000) {
                    batch.control.Poison(ETIMEDOUT);
                    return ETIMEDOUT;
                }
                batch.control.Close();
            } else if (joined.status != ControlStatus::Busy && joined.status != ControlStatus::Stale)
                return Error(joined);
        }
        const uint64_t current = BatchClockMs();
        if (!current || current - started >= 5000) return ETIMEDOUT;
        const timespec delay{0, 10000000};
        ppoll(nullptr, 0, &delay, nullptr);
    }
}

int JournalBusy(int directory, bool* busy) {
    Fd probe(openat(directory, ".", kDirectory));
    if (probe.get() < 0) return errno;
    if (flock(probe.get(), LOCK_EX | LOCK_NB) == 0) { *busy = false; return 0; }
    if (errno == EWOULDBLOCK || errno == EAGAIN) { *busy = true; return 0; }
    return errno;
}

int ScopeOpenFailure(const char* component, int flags, int error) {
    __android_log_print(ANDROID_LOG_ERROR, "ModAvdDownload",
                        "scope open failed: component=%s flags=0x%x errno=%d uid=%u euid=%u",
                        component, flags, error, getuid(), geteuid());
    errno = error;
    return -1;
}

int StartupFailure(const char* stage, int error) {
    __android_log_print(ANDROID_LOG_ERROR, "ModAvdDownload",
                        "startup stage failed: stage=%s errno=%d uid=%u euid=%u",
                        stage, error, getuid(), geteuid());
    return error;
}

int OpenDirectoryChain(int parent, const char* const* components, size_t count) {
    Fd current(fcntl(parent, F_DUPFD_CLOEXEC, 0));
    if (current.get() < 0) return ScopeOpenFailure("root-capture", F_DUPFD_CLOEXEC, errno);
    for (size_t index = 0; index < count; ++index) {
        // Scoped storage may allow search into this application's package
        // without allowing the parent directory to be listed. Only the final
        // download root is opened for directory I/O and later fsync.
        const int flags = index + 1 == count ? kDirectory : kSearchDirectory;
        Fd next(openat(current.get(), components[index], flags));
        if (next.get() < 0) return ScopeOpenFailure(components[index], flags, errno);
        current.reset(next.release());
    }
    return current.release();
}

int OpenScope(unsigned user, char (&path)[512]) {
    char top[80];
    snprintf(top, sizeof(top), "/storage/emulated/%u", user);
    Fd current(open(top, kSearchDirectory));
    if (current.get() < 0) return ScopeOpenFailure("emulated-user-root", kSearchDirectory, errno);
    const char* components[] = {"Android", "data", kPackage, "files", "HGGameUpdate", "HGDownload"};
    const int result = OpenDirectoryChain(current.get(), components,
                                          sizeof(components) / sizeof(components[0]));
    if (result < 0) return -1;
    snprintf(path, sizeof(path), "%s/Android/data/%s/files/HGGameUpdate/HGDownload", top, kPackage);
    return result;
}

std::atomic<int> g_initialization{0};
std::atomic<int> g_error{0};
Batch* g_batch = nullptr;

int Initialize(const char* private_dir, Mode mode) {
    char private_root[PATH_MAX + 1];
    unsigned user = 0;
    int error = PrivateRoot(private_dir, private_root, &user);
    if (error) return StartupFailure("private-root-parse", error);
    Fd app(open(private_root, kDirectory));
    if (app.get() < 0) return StartupFailure("private-root-open", errno);
    struct stat app_info{};
    if (fstat(app.get(), &app_info) != 0) return StartupFailure("private-root-stat", errno);
    if (app_info.st_uid != getuid()) return StartupFailure("private-root-owner", EPERM);
    Fd wrapper(openat(app.get(), kWrapper, kDirectory));
    const bool wrapper_absent = wrapper.get() < 0 && errno == ENOENT;
    if (wrapper.get() < 0 && !wrapper_absent) return StartupFailure("wrapper-open", errno);
    if (wrapper_absent && mode == Mode::Off) return 0;
    Fd journal;
    bool has_records = false;
    bool known_helper = false;
    char scope[512];
    snprintf(scope, sizeof(scope),
             "/storage/emulated/%u/Android/data/%s/files/HGGameUpdate/HGDownload", user, kPackage);
    if (wrapper.get() >= 0) {
        journal.reset(openat(wrapper.get(), kJournal, kDirectory));
        if (journal.get() < 0 && errno != ENOENT) return errno;
        if (journal.get() >= 0) {
            error = HasEntries(journal.get(), &has_records);
            if (error) return error;
            error = BindJournal(wrapper.get(), journal.get(), scope, has_records, false);
            if (error) return error;
            error = JournalBusy(journal.get(), &known_helper);
            if (error) return error;
        } else {
            struct stat binding{};
            if (fstatat(wrapper.get(), "binding", &binding, AT_SYMLINK_NOFOLLOW) == 0)
                return ESTALE;
            if (errno != ENOENT) return errno;
        }
    }
    if (mode == Mode::Off && !has_records && !known_helper) return 0;
    Fd root;
    if (!known_helper) root.reset(OpenScope(user, scope));
    if (!known_helper && root.get() < 0) {
        error = errno;
        if (error == ENOENT && !has_records) {
            __android_log_print(ANDROID_LOG_INFO, "ModAvdDownload",
                                "not activated: HGDownload is not present");
            return 0;
        }
        return error;
    }
    Batch* batch = new (std::nothrow) Batch(mode);
    if (!batch) return ENOMEM;
    batch->root.reset(root.release());
    memcpy(batch->scope, scope, strlen(scope) + 1);
    struct stat root_info{};
    if (!known_helper) {
        if (fstat(batch->root.get(), &root_info) != 0) { error = errno; delete batch; return error; }
        batch->root_device = static_cast<uint64_t>(root_info.st_dev);
    }
    if (mode == Mode::Active || has_records || known_helper) {
        if (wrapper.get() < 0) {
            if (mkdirat(app.get(), kWrapper, 0700) != 0 && errno != EEXIST) {
                error = errno; delete batch; return error;
            }
            wrapper.reset(openat(app.get(), kWrapper, kDirectory));
            if (wrapper.get() < 0 || fsync(app.get()) != 0) {
                error = errno; delete batch; return error;
            }
        }
        if (journal.get() < 0) {
            if (mkdirat(wrapper.get(), kJournal, 0700) != 0 && errno != EEXIST) {
                error = errno; delete batch; return error;
            }
            journal.reset(openat(wrapper.get(), kJournal, kDirectory));
            if (journal.get() < 0 || fsync(wrapper.get()) != 0) {
                error = errno; delete batch; return error;
            }
        }
        const JournalResult acquired = known_helper ? JournalResult{JournalStatus::Busy, EWOULDBLOCK} :
            batch->journal.AcquireSession(journal.get(), batch->root.get());
        if (acquired.status == JournalStatus::Busy) {
            error = JoinHelper(*batch, wrapper.get());
            if (error) { delete batch; return StartupFailure("helper-join", error); }
        } else {
            error = Error(acquired);
            if (!error) error = BindJournal(wrapper.get(), journal.get(), scope, has_records);
            if (!error) error = ConsumeSession(wrapper.get());
            if (!error) error = Error(batch->journal.Recover());
            if (!error && mode == Mode::Active) {
                char text[PROP_VALUE_MAX]{};
                ControlNonce nonce;
                error = ControllerNonce(text, &nonce);
                if (!error) error = Error(batch->control.CreateOwner(wrapper.get(), nonce));
                if (!error) { batch->controlled = true; batch->ready_pending = true; }
            }
            if (error) { delete batch; return StartupFailure("owner-session", error); }
            __android_log_print(ANDROID_LOG_INFO, "ModAvdDownload",
                                "exclusive controller session recovery complete; prior_records=%d", has_records);
        }
    }
    if (mode == Mode::Off && !batch->helper) { delete batch; return 0; }
    if (mode == Mode::Observe && !batch->helper) {
        error = Error(batch->journal.Close());
        if (error) { delete batch; return error; }
    }
    // Retain the callback context for the irreversible filter's lifetime.
    g_batch = batch;
    const listener::StartResult started = listener::Start(Batch::Callback, batch, 10000,
        batch->controlled && !batch->helper ? Batch::Tick : nullptr);
    if (started.error) {
        if (!started.filter_installed) { g_batch = nullptr; delete batch; }
        return started.error;
    }
    // Mutable counters belong to the callback coordinator after Start.
    __android_log_print(ANDROID_LOG_INFO, "ModAvdDownload", "initialized mode=%s role=%s",
                        batch->mode == Mode::Active ? "active" : "observe", batch->helper ? "helper" : "owner");
    return 0;
}
}  // namespace
}  // namespace modavd::download

extern "C" __attribute__((visibility("default")))
int modavd_download_sync_init(const char* package, const char* private_dir) {
    using namespace modavd::download;
    if (!package || strcmp(package, kPackage)) return 0;
    int expected = 0;
    if (!g_initialization.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        return expected == 2 ? g_error.load(std::memory_order_acquire) : EBUSY;
    char value[PROP_VALUE_MAX]{};
    __system_property_get("modavd.download.sync", value);
    Mode mode = Mode::Off;
    int error = 0;
    if (!value[0] || strcmp(value, "off") == 0) mode = Mode::Off;
    else if (strcmp(value, "observe") == 0) mode = Mode::Observe;
    else if (strcmp(value, "active") == 0) mode = Mode::Active;
    else error = EINVAL;
    if (!error) error = Initialize(private_dir, mode);
    if (error)
        __android_log_print(ANDROID_LOG_ERROR, "ModAvdDownload", "initialization refused: errno=%d", error);
    g_error.store(error, std::memory_order_release);
    g_initialization.store(2, std::memory_order_release);
    return error;
}
