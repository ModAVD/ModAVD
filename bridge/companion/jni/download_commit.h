// Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later.
#ifndef MODAVD_DOWNLOAD_COMMIT_H_
#define MODAVD_DOWNLOAD_COMMIT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace modavd::download {

struct FileIdentity {
    uint64_t device = 0;
    uint64_t inode = 0;
};

struct CapturedHandle {
    int handle = -1;
    FileIdentity identity{};
    bool identity_valid = false;
};

// capture must duplicate source, then identify the duplicate without reopening
// a path. It returns zero or a positive errno. Any handle it returns belongs to
// the core, including on failure. It must return an owned reference, never a
// borrowed descriptor. Numeric equality with source does not prove borrowing
// when source can concurrently close and its descriptor number can be reused.
// close consumes ownership even on error. In particular, do not retry POSIX
// close after EINTR on systems where the fd may already have been released.
// An atomic dup captures the OFD at its syscall linearization point; concurrent
// source close/reuse does not require a separate fd-table lock. The adapter must
// validate identity and eligibility on the owned duplicate, rather than trust
// an earlier source-fd lookup, and must not use source's number as identity.
// Identity must distinguish live objects within the adapter's filesystem scope.
struct HandleOps {
    void* context = nullptr;
    int (*capture)(void*, int source, CapturedHandle*) = nullptr;
    int (*close)(void*, int handle) = nullptr;
};

struct Token {
    uint32_t slot = UINT32_MAX;
    uint64_t epoch = 0;
};

enum class Status {
    Ok,
    Clean,
    Busy,
    Stale,
    Capacity,
    CaptureFailed,
    HandleError,
    Unpaid,
    Failed,
    Overflow,
    Invalid,
};

struct Result {
    Status status = Status::Ok;
    int error = 0;
};

enum class State { Clean, Dirty, Committing, NeedsRecovery, Failed };

struct Snapshot {
    FileIdentity identity{};
    State state = State::Clean;
    uint64_t dirty_generation = 0;
    uint64_t committed_generation = 0;
    uint64_t sealed_generation = 0;
    int last_error = 0;
    bool owns_handle = false;
    bool commit_inflight = false;
};

// handle is borrowed until CompleteCommit. The I/O worker may use it only for
// this ticket, must not close it, and must stop using it before reporting back.
struct CommitTicket {
    Token token{};
    uint64_t sealed_generation = 0;
    int handle = -1;
};

// One coordinator calls all methods, including CompleteCommit. An I/O worker
// performs real fsync and returns its errno through that coordinator. This class
// does no I/O except capture/close and makes no publication or recovery claim.
// Generations count admitted dirty events, not bytes or a safe file prefix.
// The adapter must account for every relevant writer before relying on them.
class CommitCore final {
public:
    static constexpr size_t kCapacity = 32;

    // Lower limits exercise the same overflow checks without counter wrapping.
    explicit CommitCore(
        HandleOps ops,
        uint64_t generation_limit = std::numeric_limits<uint64_t>::max(),
        uint64_t epoch_limit = std::numeric_limits<uint64_t>::max()) noexcept;
    // All I/O workers must have stopped before destruction. Releasing handles
    // here never asserts that dirty generations have reached storage.
    ~CommitCore();
    CommitCore(const CommitCore&) = delete;
    CommitCore& operator=(const CommitCore&) = delete;
    CommitCore(CommitCore&&) = delete;
    CommitCore& operator=(CommitCore&&) = delete;

    // Track starts clean; MarkDirty is required for each admitted dirty event.
    // Clean covers recorded events only, not preexisting contents or unobserved
    // writes. Track itself is not a durability certificate.
    // Capacity is checked before capture. Every successful Track owns its own
    // capture and token, even for equal identities. Equal device/inode pairs
    // do not prove equal open file descriptions or writeback-error cursors.
    Result Track(int source, Token* token) noexcept;
    // Overflow quarantines the slot as Failed. An older successful ticket
    // cannot certify cleanliness after a dirty event could not be represented.
    Result MarkDirty(Token token) noexcept;
    Result BeginCommit(Token token, CommitTicket* ticket) noexcept;
    // Zero means real fsync succeeded. Positive errno quarantines the slot in
    // NeedsRecovery. A negative value is invalid and leaves the ticket pending.
    // Failed slots cannot be reset or retired through this core.
    // A ticket pending when MarkDirty overflowed must still be returned here;
    // it releases the in-flight borrow without advancing the committed count.
    Result CompleteCommit(const CommitTicket& ticket, int fsync_error) noexcept;
    Result Inspect(Token token, Snapshot* snapshot) const noexcept;
    Result Retire(Token token) noexcept;
    // Preflight every slot before closing any. A close failure remains visible
    // as Failed; successful shutdown permits later tracking with fresh epochs.
    Result CleanShutdown() noexcept;

private:
    struct Slot {
        bool occupied = false;
        uint64_t epoch = 0;
        int handle = -1;
        Snapshot snapshot{};
    };

    Slot* Find(Token token) noexcept;
    const Slot* Find(Token token) const noexcept;
    Result CanRetire(const Slot& slot) const noexcept;
    Result CloseSlot(Slot& slot) noexcept;

    HandleOps ops_;
    uint64_t generation_limit_;
    uint64_t epoch_limit_;
    std::array<Slot, kCapacity> slots_{};
};

}  // namespace modavd::download
#endif  // MODAVD_DOWNLOAD_COMMIT_H_
