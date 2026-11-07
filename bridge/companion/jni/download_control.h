// Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later.
#ifndef MODAVD_DOWNLOAD_CONTROL_H_
#define MODAVD_DOWNLOAD_CONTROL_H_

#include <cstdint>

namespace modavd::download {

struct ControlNonce { uint64_t high = 0; uint64_t low = 0; };
bool ParseControlNonce(const char* text, ControlNonce* nonce) noexcept;
struct ControlProcess { int32_t pid = 0; uint64_t start_ticks = 0; };
enum class ControlKind { Normal, Permanent };
enum class ControlState { Booting, Idle, Requested, Draining, Granted, Executing, Finished, HandedOff, Poisoned };
enum class ControlStatus { Ok, Busy, Invalid, Stale, Timeout, Dead, Poisoned, IoError, Overflow };
struct ControlResult { ControlStatus status = ControlStatus::Ok; int error = 0; };
struct ControlTicket {
    ControlNonce nonce{};
    uint64_t sequence = 0;
    ControlProcess owner{};
    ControlProcess helper{};
    ControlKind kind = ControlKind::Normal;
};
struct ControlSnapshot {
    ControlState state = ControlState::Booting;
    uint64_t sequence = 0;
    bool permanent = false;
    int error = 0;
    ControlProcess owner{};
    ControlProcess helper{};
};

// One owner worker mutates CommitCore/journal and calls PublishReady/Service.
// Helpers only request a barrier, then perform the REAL syscall in their own
// address space/fd table, then End. Helpers never early-ACK or recover files.
// This module owns exactly control.page/control.lock in the private bridge
// directory; other bridge-owned files may coexist there.
// The cooperative helper lock is held from BEGIN through END. Owner methods
// NEVER acquire it, including while a helper is waiting for its grant.
class DownloadControl final {
public:
    // Drain returns zero only after all prior early-ACK debt is really synced,
    // its intents durably retired, and old admission maps/records invalidated.
    // New admissions stay paused until END. A positive errno denies the grant.
    using Drain = int (*)(void* context, bool permanent);

    // Optional initialization cutpoint for native race tests. Called while the
    // atomic header generation is odd, after nonce but before owner identity.
    using InitCheckpoint = void (*)(void* context);
    explicit DownloadControl(InitCheckpoint checkpoint = nullptr, void* context = nullptr) noexcept;
    ~DownloadControl();
    DownloadControl(const DownloadControl&) = delete;
    DownloadControl& operator=(const DownloadControl&) = delete;
    DownloadControl(DownloadControl&&) = delete;
    DownloadControl& operator=(DownloadControl&&) = delete;

    // Caller must already own the journal. Resetting a previous session requires
    // a NEW trusted nonce and the controller to have stopped the entire UID.
    // This never elects a replacement owner for an existing nonce.
    ControlResult CreateOwner(int private_directory, ControlNonce nonce) noexcept;
    // Nonblocking open; Busy means the owner's page is not published yet.
    // No recovery, token consumption, page reset or owner election occurs here.
    // Holds a pidfd for peer liveness; peer /proc visibility is not required.
    // Open success is not execution permission. A fresh Begin/GRANT is required.
    ControlResult OpenHelper(int private_directory, ControlNonce nonce) noexcept;
    // Call on the owner worker only after exclusive startup recovery completed.
    ControlResult PublishReady() noexcept;
    // Call from the same worker before callbacks and during idle ticks (~100ms).
    // Service never waits for helper END or takes the helper's flock. Drain is
    // synchronous; late completion after timeout cannot grant a poisoned ticket.
    ControlResult Service(Drain drain, void* context) noexcept;
    bool AdmissionAllowed() const noexcept;

    // Wait at most timeout_ms (1..5000). Ok returns an EXECUTING ticket; only
    // then may the helper execute its real syscall. Begin errors prohibit it.
    // Deadline covers execution until END too. Timeout does not cancel a syscall.
    ControlResult Begin(ControlKind kind, ControlTicket* ticket,
                        unsigned timeout_ms = 5000) noexcept;
    // Zero means real success, positive errno means the real operation failed.
    // Known namespace errors do not themselves disable fresh admissions. The
    // adapter must Restrict/Poison on durability failure or uncertain completion.
    // Never report CONTINUE as success; use FinishHandoff for that separate path.
    // Stale/late END cannot finish a later sequence or clear poison.
    ControlResult End(const ControlTicket& ticket, int operation_error) noexcept;
    // Permanent tickets only. Release a drained barrier before handing the
    // original syscall back to the kernel. This does NOT claim completion or
    // success. The permanent latch remains set. Failure prohibits CONTINUE.
    ControlResult FinishHandoff(const ControlTicket& ticket) noexcept;

    // Restrict pauses future early ACKs permanently; it does not repay debt.
    // Poison also refuses future grants after uncertainty (timeout/death/etc.).
    ControlResult Restrict() noexcept;
    ControlResult Poison(int error) noexcept;
    ControlResult Inspect(ControlSnapshot* snapshot) const noexcept;
    void Close() noexcept;

private:
    struct SharedPage;
    struct HeaderSnapshot;
    static ControlResult ReadHeader(const SharedPage* page, HeaderSnapshot* header) noexcept;
    ControlResult Open(int directory, ControlNonce nonce, bool owner) noexcept;
    bool Matches() const noexcept;
    ControlResult Failure() const noexcept;
    ControlResult Finish(const ControlTicket& ticket, int error, bool handoff) noexcept;
    void ReleaseHelperLock() noexcept;
    void ReleaseHelperPeer() noexcept;

    SharedPage* page_ = nullptr;
    int page_fd_ = -1;
    int lock_fd_ = -1;
    bool owner_ = false;
    bool helper_locked_ = false;
    bool active_ = false;
    ControlNonce nonce_{};
    ControlProcess self_{};
    ControlProcess owner_process_{};
    uint64_t active_sequence_ = 0;
    int owner_pidfd_ = -1;
    int helper_pidfd_ = -1;
    uint64_t helper_sequence_ = 0;
    ControlProcess helper_process_{};
    InitCheckpoint init_checkpoint_ = nullptr;
    void* init_context_ = nullptr;
};

}  // namespace modavd::download
#endif  // MODAVD_DOWNLOAD_CONTROL_H_
