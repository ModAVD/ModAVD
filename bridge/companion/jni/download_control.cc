// Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "download_control.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace modavd::download {
namespace {
constexpr size_t kPageBytes = 4096;
constexpr uint32_t kMagic = 0x4d435432;
constexpr uint32_t kVersion = 2;
constexpr uint64_t kStateBits = 4;
constexpr uint64_t kMaxSequence = UINT64_MAX >> kStateBits;
static_assert(std::atomic<uint64_t>::is_always_lock_free, "shared state requires lock-free atomics");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "shared flags require lock-free atomics");

bool Same(ControlNonce left, ControlNonce right) noexcept {
    return left.high == right.high && left.low == right.low;
}
bool Same(ControlProcess left, ControlProcess right) noexcept {
    return left.pid == right.pid && left.start_ticks == right.start_ticks;
}
uint64_t Word(uint64_t sequence, ControlState state) noexcept {
    return (sequence << kStateBits) | static_cast<uint64_t>(state);
}
ControlState Phase(uint64_t word) noexcept { return static_cast<ControlState>(word & 15); }
uint64_t Sequence(uint64_t word) noexcept { return word >> kStateBits; }
ControlResult Error(int error) noexcept { return {ControlStatus::IoError, error > 0 ? error : EINVAL}; }
uint64_t Now() noexcept {
    timespec time{};
    if (::clock_gettime(CLOCK_MONOTONIC, &time) != 0) return 0;
    return static_cast<uint64_t>(time.tv_sec) * 1000000000ULL + static_cast<uint64_t>(time.tv_nsec);
}
void Pause() noexcept {
    const timespec delay{0, 2000000};
    // Android's app allowlist permits ppoll, not raw SYS_poll.
    ::ppoll(nullptr, 0, &delay, nullptr);
}

int ReadSelf(ControlProcess* process) noexcept {
    const int32_t pid = static_cast<int32_t>(::getpid());
    const int fd = ::open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno;
    char bytes[4096];
    ssize_t count;
    do { count = ::read(fd, bytes, sizeof(bytes) - 1); } while (count < 0 && errno == EINTR);
    const int error = count < 0 ? errno : 0;
    ::close(fd);
    if (error != 0) return error;
    if (count <= 0 || count == static_cast<ssize_t>(sizeof(bytes) - 1)) return EIO;
    bytes[count] = '\0';
    const char* end_name = std::strrchr(bytes, ')');
    if (end_name == nullptr || end_name[1] != ' ' || end_name[2] == '\0') return EIO;
    const char* cursor = end_name + 2;
    if (*cursor == 'Z' || *cursor == 'X' || *cursor == 'x') return ESRCH;
    for (int field = 3; field < 22; ++field) {
        cursor = std::strchr(cursor, ' ');
        if (cursor == nullptr) return EIO;
        while (*cursor == ' ') ++cursor;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long start = std::strtoull(cursor, &end, 10);
    if (errno != 0 || end == cursor || (*end != ' ' && *end != '\n' && *end != '\0') || start == 0) return EIO;
    *process = {pid, static_cast<uint64_t>(start)};
    return 0;
}
int OpenPidfd(int32_t pid, int* fd) noexcept {
    if (pid <= 0) return EINVAL;
#ifdef SYS_pidfd_open
    const int opened = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
    if (opened < 0) return errno;
    *fd = opened;
    return 0;
#else
    (void)fd;
    return ENOSYS;
#endif
}
int PeerAlive(int fd) noexcept {
    if (fd < 0) return EBADF;
    pollfd peer{fd, POLLIN, 0};
    const timespec immediate{0, 0};
    int result;
    do { result = ::ppoll(&peer, 1, &immediate, nullptr); } while (result < 0 && errno == EINTR);
    if (result < 0) return errno;
    if (result == 0) return 0;
    if (peer.revents & POLLNVAL) return EBADF;
    if (peer.revents & (POLLIN | POLLHUP)) return EOWNERDEAD;
    return EIO;
}
bool PrivateFile(const struct stat& info, dev_t device) noexcept {
    return S_ISREG(info.st_mode) && info.st_nlink == 1 && info.st_uid == ::geteuid() &&
           (info.st_mode & 0077) == 0 && info.st_dev == device;
}
}  // namespace

struct DownloadControl::SharedPage {
    std::atomic<uint32_t> initialized{0};
    std::atomic<uint32_t> version{0};
    std::atomic<uint32_t> bytes{0};
    std::atomic<uint32_t> reserved{0};
    std::atomic<uint64_t> header_generation{0};
    std::atomic<uint64_t> nonce_high{0};
    std::atomic<uint64_t> nonce_low{0};
    std::atomic<int32_t> owner_pid{0};
    std::atomic<uint64_t> owner_start{0};
    alignas(8) std::atomic<uint64_t> word{Word(0, ControlState::Booting)};
    std::atomic<uint32_t> permanent{0};
    std::atomic<uint32_t> poisoned{0};
    std::atomic<int32_t> error{0};
    std::atomic<int32_t> helper_pid{0};
    std::atomic<uint64_t> helper_start{0};
    std::atomic<uint64_t> deadline{0};
    std::atomic<uint32_t> kind{0};
    std::atomic<int32_t> operation_error{0};
};

struct DownloadControl::HeaderSnapshot {
    uint64_t generation = 0;
    ControlNonce nonce{};
    ControlProcess owner{};
};

ControlResult DownloadControl::ReadHeader(const SharedPage* page, HeaderSnapshot* header) noexcept {
    // These header operations are sequentially consistent. With a monotonically
    // increasing generation, equal even reads cannot surround mixed header
    // stores. Atomic fields also prevent compiler hoisting/data races during a
    // legitimate concurrent helper open and owner session reset.
    const uint64_t before = page->header_generation.load();
    if (before == 0 || (before & 1)) return {ControlStatus::Busy, EAGAIN};
    const uint32_t magic = page->initialized.load();
    const uint32_t version = page->version.load();
    const uint32_t bytes = page->bytes.load();
    const uint32_t reserved = page->reserved.load();
    HeaderSnapshot candidate;
    candidate.generation = before;
    candidate.nonce = {page->nonce_high.load(), page->nonce_low.load()};
    candidate.owner = {page->owner_pid.load(), page->owner_start.load()};
    const uint64_t after = page->header_generation.load();
    if (before != after || (after & 1)) return {ControlStatus::Busy, EAGAIN};
    // Old layouts and an unpublished initial page cannot establish identity.
    if (magic != kMagic) return {ControlStatus::Busy, EAGAIN};
    if (version != kVersion || bytes != kPageBytes || reserved != 0) return {ControlStatus::Stale, ESTALE};
    *header = candidate;
    return {};
}

bool ParseControlNonce(const char* text, ControlNonce* nonce) noexcept {
    if (text == nullptr || nonce == nullptr) return false;
    ControlNonce parsed;
    for (size_t index = 0; index != 32; ++index) {
        const char digit = text[index];
        unsigned value;
        if (digit >= '0' && digit <= '9') value = static_cast<unsigned>(digit - '0');
        else if (digit >= 'a' && digit <= 'f') value = static_cast<unsigned>(digit - 'a' + 10);
        else if (digit >= 'A' && digit <= 'F') value = static_cast<unsigned>(digit - 'A' + 10);
        else return false;
        uint64_t& half = index < 16 ? parsed.high : parsed.low;
        half = (half << 4) | value;
    }
    if (text[32] != '\0' || (parsed.high == 0 && parsed.low == 0)) return false;
    *nonce = parsed;
    return true;
}

DownloadControl::DownloadControl(InitCheckpoint checkpoint, void* context) noexcept
    : init_checkpoint_(checkpoint), init_context_(context) {}
DownloadControl::~DownloadControl() { Close(); }

ControlResult DownloadControl::Open(int directory, ControlNonce nonce, bool owner) noexcept {
    static_assert(sizeof(SharedPage) <= kPageBytes, "control page overflow");
    if (page_ != nullptr) return {ControlStatus::Busy, 0};
    if (directory < 0 || (nonce.high == 0 && nonce.low == 0)) return {ControlStatus::Invalid, EINVAL};
    struct stat directory_info{};
    if (::fstat(directory, &directory_info) != 0) return Error(errno);
    if (!S_ISDIR(directory_info.st_mode)) return {ControlStatus::Invalid, ENOTDIR};
    ControlProcess self;
    int error = ReadSelf(&self);
    if (error != 0) return Error(error);
    const int flags = O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (owner ? O_CREAT : 0);
    const int page_fd = ::openat(directory, "control.page", flags, 0600);
    if (page_fd < 0) return !owner && errno == ENOENT ? ControlResult{ControlStatus::Busy, ENOENT} : Error(errno);
    const int lock_fd = ::openat(directory, "control.lock", flags, 0600);
    if (lock_fd < 0) {
        error = errno; ::close(page_fd);
        return !owner && error == ENOENT ? ControlResult{ControlStatus::Busy, error} : Error(error);
    }
    struct stat page_info{}, lock_info{};
    if (::fstat(page_fd, &page_info) != 0 || ::fstat(lock_fd, &lock_info) != 0) error = errno;
    else if (!PrivateFile(page_info, directory_info.st_dev) || !PrivateFile(lock_info, directory_info.st_dev)) error = EINVAL;
    if (error != 0) { ::close(lock_fd); ::close(page_fd); return Error(error); }
    if (owner && ::flock(page_fd, LOCK_EX | LOCK_NB) != 0) {
        error = errno; ::close(lock_fd); ::close(page_fd);
        return error == EAGAIN || error == EWOULDBLOCK ? ControlResult{ControlStatus::Busy, error} : Error(error);
    }
    if (page_info.st_size != static_cast<off_t>(kPageBytes)) {
        if (!owner) { ::close(lock_fd); ::close(page_fd); return {ControlStatus::Busy, 0}; }
        if (::ftruncate(page_fd, kPageBytes) != 0) {
            error = errno; ::close(lock_fd); ::close(page_fd); return Error(error);
        }
    }
    void* mapping = ::mmap(nullptr, kPageBytes, PROT_READ | PROT_WRITE, MAP_SHARED, page_fd, 0);
    if (mapping == MAP_FAILED) {
        error = errno; ::close(lock_fd); ::close(page_fd); return Error(error);
    }
    auto* page = static_cast<SharedPage*>(mapping);
    HeaderSnapshot header;
    int owner_pidfd = -1;
    const ControlResult initial_header = ReadHeader(page, &header);
    if (owner) {
        if (initial_header.status == ControlStatus::Ok && Same(header.nonce, nonce)) error = ESTALE;
        else {
            uint64_t previous = 0;
            if (page->initialized.load() == kMagic && page->version.load() == kVersion) previous = page->header_generation.load();
            if (previous >= UINT64_MAX - 3) error = EOVERFLOW;
            else {
                const uint64_t odd = (previous | 1ULL) + 2;
                page->header_generation.store(odd);
                page->nonce_high.store(nonce.high);
                page->nonce_low.store(nonce.low);
                if (init_checkpoint_ != nullptr) init_checkpoint_(init_context_);
                page->owner_pid.store(self.pid);
                page->owner_start.store(self.start_ticks);
                page->version.store(kVersion);
                page->bytes.store(kPageBytes);
                page->reserved.store(0);
                page->word.store(Word(0, ControlState::Booting));
                page->permanent.store(0);
                page->poisoned.store(0);
                page->error.store(0);
                page->helper_pid.store(0);
                page->helper_start.store(0);
                page->deadline.store(0);
                page->kind.store(0);
                page->operation_error.store(0);
                page->initialized.store(kMagic);
                page->header_generation.store(odd + 1);
                if (::msync(mapping, kPageBytes, MS_SYNC) != 0 || ::fsync(page_fd) != 0 || ::fsync(directory) != 0) error = errno;
                header = {odd + 1, nonce, self};
            }
            if (error != 0) {
                page->permanent.store(1);
                page->error.store(error);
                page->poisoned.store(1);
                page->word.store(Word(0, ControlState::Poisoned));
            }
        }
    } else {
        if (initial_header.status != ControlStatus::Ok) error = initial_header.error;
        else if (!Same(header.nonce, nonce)) error = ESTALE;
        else {
            int alive_error = OpenPidfd(header.owner.pid, &owner_pidfd);
            if (alive_error == 0) alive_error = PeerAlive(owner_pidfd);
            HeaderSnapshot confirmed;
            const ControlResult confirmation = ReadHeader(page, &confirmed);
            // Even a failed liveness lookup is not evidence of death if the
            // owner header changed while /proc was being read.
            if (confirmation.status == ControlStatus::Busy ||
                (confirmation.status == ControlStatus::Ok &&
                 (confirmed.generation != header.generation || !Same(confirmed.nonce, header.nonce) || !Same(confirmed.owner, header.owner)))) error = EAGAIN;
            else if (confirmation.status != ControlStatus::Ok) error = confirmation.error;
            else error = alive_error;
        }
    }
    if (error != 0) {
        if (owner_pidfd >= 0) ::close(owner_pidfd);
        ::munmap(mapping, kPageBytes); ::close(lock_fd); ::close(page_fd);
        if (error == ESTALE) return {ControlStatus::Stale, error};
        if (error == EAGAIN) return {ControlStatus::Busy, error};
        if (!owner && (error == ESRCH || error == EOWNERDEAD)) return {ControlStatus::Dead, error};
        return Error(error);
    }
    page_ = page; page_fd_ = page_fd; lock_fd_ = lock_fd;
    owner_ = owner; nonce_ = nonce; self_ = self; owner_process_ = header.owner;
    owner_pidfd_ = owner_pidfd;
    return {};
}

ControlResult DownloadControl::CreateOwner(int directory, ControlNonce nonce) noexcept { return Open(directory, nonce, true); }
ControlResult DownloadControl::OpenHelper(int directory, ControlNonce nonce) noexcept { return Open(directory, nonce, false); }
bool DownloadControl::Matches() const noexcept {
    HeaderSnapshot header;
    return page_ != nullptr && ReadHeader(page_, &header).status == ControlStatus::Ok &&
           Same(header.nonce, nonce_) && Same(header.owner, owner_process_);
}
ControlResult DownloadControl::Failure() const noexcept {
    if (!Matches()) return {ControlStatus::Stale, ESTALE};
    const int error = page_->error.load(std::memory_order_acquire);
    if (error == ETIMEDOUT) return {ControlStatus::Timeout, error};
    if (error == ESRCH || error == EOWNERDEAD) return {ControlStatus::Dead, error};
    return {ControlStatus::Poisoned, error > 0 ? error : ECANCELED};
}
ControlResult DownloadControl::Restrict() noexcept {
    if (!Matches()) return {ControlStatus::Stale, ESTALE};
    if (self_.pid != static_cast<int32_t>(::getpid())) return {ControlStatus::Invalid, ECHILD};
    page_->permanent.store(1, std::memory_order_release);
    return {};
}
ControlResult DownloadControl::Poison(int error) noexcept {
    const ControlResult restricted = Restrict();
    if (restricted.status != ControlStatus::Ok) return restricted;
    error = error > 0 ? error : ECANCELED;
    int32_t empty = 0;
    page_->error.compare_exchange_strong(empty, error, std::memory_order_acq_rel);
    page_->poisoned.store(1, std::memory_order_release);
    uint64_t word = page_->word.load(std::memory_order_acquire);
    while (Phase(word) != ControlState::Poisoned &&
           !page_->word.compare_exchange_weak(word, Word(Sequence(word), ControlState::Poisoned), std::memory_order_acq_rel)) {}
    return Failure();
}
ControlResult DownloadControl::PublishReady() noexcept {
    if (!owner_ || self_.pid != static_cast<int32_t>(::getpid())) return {ControlStatus::Invalid, ECHILD};
    if (!Matches()) return {ControlStatus::Stale, ESTALE};
    if (page_->poisoned.load(std::memory_order_acquire)) return Failure();
    uint64_t expected = Word(0, ControlState::Booting);
    if (!page_->word.compare_exchange_strong(expected, Word(0, ControlState::Idle), std::memory_order_acq_rel)) {
        return Phase(expected) == ControlState::Idle ? ControlResult{} : ControlResult{ControlStatus::Busy, 0};
    }
    return {};
}
bool DownloadControl::AdmissionAllowed() const noexcept {
    return owner_ && self_.pid == static_cast<int32_t>(::getpid()) && Matches() &&
           !page_->permanent.load(std::memory_order_acquire) && !page_->poisoned.load(std::memory_order_acquire) &&
           Phase(page_->word.load(std::memory_order_acquire)) == ControlState::Idle;
}

ControlResult DownloadControl::Service(Drain drain, void* context) noexcept {
    if (!owner_ || self_.pid != static_cast<int32_t>(::getpid()) || drain == nullptr) return {ControlStatus::Invalid, EINVAL};
    if (!Matches()) return {ControlStatus::Stale, ESTALE};
    if (page_->poisoned.load(std::memory_order_acquire)) return Failure();
    uint64_t word = page_->word.load(std::memory_order_acquire);
    const ControlState phase = Phase(word);
    if (phase == ControlState::Booting) return {ControlStatus::Busy, 0};
    if (phase == ControlState::Idle) return {};
    if (phase == ControlState::Poisoned) return Failure();
    if (phase == ControlState::Finished || phase == ControlState::HandedOff) {
        if (phase == ControlState::HandedOff && (!page_->permanent.load() ||
            page_->kind.load() != static_cast<uint32_t>(ControlKind::Permanent))) return Poison(EINVAL);
        if (!page_->word.compare_exchange_strong(word, Word(Sequence(word), ControlState::Idle), std::memory_order_acq_rel)) return Failure();
        ReleaseHelperPeer();
        return page_->poisoned.load(std::memory_order_acquire) ? Failure() : ControlResult{};
    }
    const ControlProcess helper{page_->helper_pid.load(), page_->helper_start.load()};
    const uint64_t sequence = Sequence(word);
    if (helper_sequence_ != sequence || !Same(helper_process_, helper)) {
        if (phase != ControlState::Requested) return Poison(ESTALE);
        ReleaseHelperPeer();
        const int open_error = OpenPidfd(helper.pid, &helper_pidfd_);
        if (open_error != 0) return Poison(open_error);
        helper_sequence_ = sequence;
        helper_process_ = helper;
    }
    const int alive_error = PeerAlive(helper_pidfd_);
    if (alive_error != 0) return Poison(alive_error);
    if (Now() >= page_->deadline.load(std::memory_order_acquire)) return Poison(ETIMEDOUT);
    if (phase != ControlState::Requested) return {ControlStatus::Busy, 0};
    if (!page_->word.compare_exchange_strong(word, Word(Sequence(word), ControlState::Draining), std::memory_order_acq_rel)) return Failure();
    const bool permanent = page_->kind.load() == static_cast<uint32_t>(ControlKind::Permanent);
    if (permanent) page_->permanent.store(1, std::memory_order_release);
    const int error = drain(context, permanent || page_->permanent.load(std::memory_order_acquire));
    if (error != 0) return Poison(error > 0 ? error : EINVAL);
    if (page_->poisoned.load(std::memory_order_acquire)) return Failure();
    const int after_drain_error = PeerAlive(helper_pidfd_);
    if (after_drain_error != 0) return Poison(after_drain_error);
    if (Now() >= page_->deadline.load(std::memory_order_acquire)) return Poison(ETIMEDOUT);
    word = Word(sequence, ControlState::Draining);
    if (!page_->word.compare_exchange_strong(word, Word(sequence, ControlState::Granted), std::memory_order_acq_rel)) return Failure();
    return {};
}

void DownloadControl::ReleaseHelperLock() noexcept {
    if (helper_locked_) {
        // A fork copy shares the parent's lock OFD. Closing its reference is
        // harmless; explicit LOCK_UN would release the still-active parent's lock.
        if (self_.pid == static_cast<int32_t>(::getpid())) ::flock(lock_fd_, LOCK_UN);
        helper_locked_ = false;
    }
}
void DownloadControl::ReleaseHelperPeer() noexcept {
    if (helper_pidfd_ >= 0) { ::close(helper_pidfd_); helper_pidfd_ = -1; }
    helper_sequence_ = 0;
    helper_process_ = {};
}
ControlResult DownloadControl::Begin(ControlKind kind, ControlTicket* ticket, unsigned timeout_ms) noexcept {
    if (owner_ || page_ == nullptr || ticket == nullptr || active_ || timeout_ms == 0 || timeout_ms > 5000 ||
        (kind != ControlKind::Normal && kind != ControlKind::Permanent) || self_.pid != static_cast<int32_t>(::getpid())) return {ControlStatus::Invalid, EINVAL};
    if (!Matches()) return {ControlStatus::Stale, ESTALE};
    const uint64_t now = Now();
    if (now == 0) return Error(EIO);
    const uint64_t deadline = now + static_cast<uint64_t>(timeout_ms) * 1000000ULL;
    auto check = [&]() -> ControlResult {
        if (!Matches()) return {ControlStatus::Stale, ESTALE};
        if (page_->poisoned.load(std::memory_order_acquire)) return Failure();
        const int alive_error = PeerAlive(owner_pidfd_);
        if (alive_error != 0) return Poison(alive_error);
        if (Now() >= deadline) return Poison(ETIMEDOUT);
        return {};
    };
    for (;;) {
        ControlResult result = check();
        if (result.status != ControlStatus::Ok) return result;
        if (::flock(lock_fd_, LOCK_EX | LOCK_NB) == 0) { helper_locked_ = true; break; }
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) return Error(errno);
        Pause();
    }
    uint64_t word;
    for (;;) {
        const ControlResult result = check();
        if (result.status != ControlStatus::Ok) { ReleaseHelperLock(); return result; }
        word = page_->word.load(std::memory_order_acquire);
        if (Phase(word) == ControlState::Idle) break;
        if (Phase(word) != ControlState::Booting && Phase(word) != ControlState::Finished && Phase(word) != ControlState::HandedOff) {
            const ControlResult failed = Poison(EOWNERDEAD); ReleaseHelperLock(); return failed;
        }
        Pause();
    }
    const uint64_t sequence = Sequence(word) + 1;
    if (Sequence(word) == kMaxSequence) { const ControlResult failed = Poison(EOVERFLOW); ReleaseHelperLock(); return {ControlStatus::Overflow, failed.error}; }
    page_->helper_pid.store(self_.pid);
    page_->helper_start.store(self_.start_ticks);
    page_->deadline.store(deadline);
    page_->kind.store(static_cast<uint32_t>(kind));
    page_->operation_error.store(0);
    if (!page_->word.compare_exchange_strong(word, Word(sequence, ControlState::Requested), std::memory_order_acq_rel)) {
        const ControlResult failed = Poison(ECANCELED); ReleaseHelperLock(); return failed;
    }
    for (;;) {
        const ControlResult result = check();
        if (result.status != ControlStatus::Ok) { ReleaseHelperLock(); return result; }
        word = page_->word.load(std::memory_order_acquire);
        if (Sequence(word) != sequence) { const ControlResult failed = Poison(ESTALE); ReleaseHelperLock(); return failed; }
        if (Phase(word) == ControlState::Granted) {
            if (!page_->word.compare_exchange_strong(word, Word(sequence, ControlState::Executing), std::memory_order_acq_rel)) continue;
            active_ = true; active_sequence_ = sequence;
            *ticket = {nonce_, sequence, owner_process_, self_, kind};
            return {};
        }
        if (Phase(word) != ControlState::Requested && Phase(word) != ControlState::Draining) {
            const ControlResult failed = Poison(ECANCELED); ReleaseHelperLock(); return failed;
        }
        Pause();
    }
}

ControlResult DownloadControl::End(const ControlTicket& ticket, int operation_error) noexcept {
    return Finish(ticket, operation_error, false);
}
ControlResult DownloadControl::FinishHandoff(const ControlTicket& ticket) noexcept {
    return Finish(ticket, EINPROGRESS, true);
}
ControlResult DownloadControl::Finish(const ControlTicket& ticket, int operation_error, bool handoff) noexcept {
    if (owner_ || !active_ || !helper_locked_ || !Same(ticket.nonce, nonce_) || ticket.sequence != active_sequence_ ||
        !Same(ticket.owner, owner_process_) || !Same(ticket.helper, self_)) return {ControlStatus::Stale, ESTALE};
    if (!Matches()) { active_ = false; ReleaseHelperLock(); return {ControlStatus::Stale, ESTALE}; }
    if (self_.pid != static_cast<int32_t>(::getpid()) || operation_error < 0) return {ControlStatus::Invalid, EINVAL};
    if (static_cast<uint32_t>(ticket.kind) != page_->kind.load() ||
        (handoff && (ticket.kind != ControlKind::Permanent || !page_->permanent.load()))) return {ControlStatus::Invalid, EINVAL};
    const int alive_error = PeerAlive(owner_pidfd_);
    if (alive_error != 0) { const ControlResult failed = Poison(alive_error); active_ = false; ReleaseHelperLock(); return failed; }
    if (page_->poisoned.load(std::memory_order_acquire)) { active_ = false; ReleaseHelperLock(); return Failure(); }
    if (Now() >= page_->deadline.load(std::memory_order_acquire)) { const ControlResult failed = Poison(ETIMEDOUT); active_ = false; ReleaseHelperLock(); return failed; }
    uint64_t word = Word(ticket.sequence, ControlState::Executing);
    page_->operation_error.store(operation_error, std::memory_order_release);
    const bool ended = page_->word.compare_exchange_strong(word,
        Word(ticket.sequence, handoff ? ControlState::HandedOff : ControlState::Finished), std::memory_order_acq_rel);
    active_ = false;
    ReleaseHelperLock();
    return ended ? ControlResult{} : Failure();
}

ControlResult DownloadControl::Inspect(ControlSnapshot* snapshot) const noexcept {
    if (snapshot == nullptr) return {ControlStatus::Invalid, EINVAL};
    if (!Matches()) return {ControlStatus::Stale, ESTALE};
    const uint64_t word = page_->word.load(std::memory_order_acquire);
    snapshot->state = Phase(word); snapshot->sequence = Sequence(word);
    snapshot->permanent = page_->permanent.load(std::memory_order_acquire) != 0;
    snapshot->error = page_->poisoned.load(std::memory_order_acquire) ? page_->error.load() : page_->operation_error.load();
    snapshot->owner = owner_process_;
    snapshot->helper = {page_->helper_pid.load(), page_->helper_start.load()};
    return {};
}
void DownloadControl::Close() noexcept {
    if (page_ != nullptr) {
        if (Matches() && self_.pid == static_cast<int32_t>(::getpid()) && (owner_ || active_)) Poison(owner_ ? EOWNERDEAD : ECANCELED);
        active_ = false;
        ReleaseHelperLock();
        ::munmap(page_, kPageBytes);
        page_ = nullptr;
    }
    if (lock_fd_ >= 0) { ::close(lock_fd_); lock_fd_ = -1; }
    if (page_fd_ >= 0) { ::close(page_fd_); page_fd_ = -1; }
    if (owner_pidfd_ >= 0) { ::close(owner_pidfd_); owner_pidfd_ = -1; }
    ReleaseHelperPeer();
    owner_ = false; helper_locked_ = false;
}

}  // namespace modavd::download
