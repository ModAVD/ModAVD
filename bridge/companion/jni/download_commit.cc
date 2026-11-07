// Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later.
#include "download_commit.h"

#include <cerrno>

namespace modavd::download {
namespace {

int PositiveError(int error) noexcept {
    return error > 0 ? error : EINVAL;
}

}  // namespace

CommitCore::CommitCore(HandleOps ops, uint64_t generation_limit,
                       uint64_t epoch_limit) noexcept
    : ops_(ops), generation_limit_(generation_limit), epoch_limit_(epoch_limit) {}

CommitCore::~CommitCore() {
    for (Slot& slot : slots_) {
        if (slot.occupied && slot.handle >= 0 && ops_.close != nullptr) {
            ops_.close(ops_.context, slot.handle);
        }
    }
}

CommitCore::Slot* CommitCore::Find(Token token) noexcept {
    if (token.slot >= kCapacity) return nullptr;
    Slot& slot = slots_[token.slot];
    return slot.occupied && slot.epoch == token.epoch ? &slot : nullptr;
}

const CommitCore::Slot* CommitCore::Find(Token token) const noexcept {
    if (token.slot >= kCapacity) return nullptr;
    const Slot& slot = slots_[token.slot];
    return slot.occupied && slot.epoch == token.epoch ? &slot : nullptr;
}

Result CommitCore::Track(int source, Token* token) noexcept {
    if (source < 0 || token == nullptr || ops_.capture == nullptr ||
        ops_.close == nullptr) {
        return {Status::Invalid, EINVAL};
    }

    size_t available = kCapacity;
    bool has_empty_slot = false;
    for (size_t index = 0; index < kCapacity; ++index) {
        if (!slots_[index].occupied) {
            has_empty_slot = true;
            if (slots_[index].epoch < epoch_limit_) {
                available = index;
                break;
            }
        }
    }
    if (available == kCapacity) {
        return {has_empty_slot ? Status::Overflow : Status::Capacity, 0};
    }

    CapturedHandle captured;
    const int capture_error = ops_.capture(ops_.context, source, &captured);
    if (capture_error != 0 || captured.handle < 0 || !captured.identity_valid) {
        const int error = PositiveError(capture_error);
        if (captured.handle >= 0) {
            const int close_error = ops_.close(ops_.context, captured.handle);
            if (close_error != 0) {
                return {Status::HandleError, PositiveError(close_error)};
            }
        }
        return {Status::CaptureFailed, error};
    }

    Slot& slot = slots_[available];
    ++slot.epoch;
    slot.occupied = true;
    slot.handle = captured.handle;
    slot.snapshot = {};
    slot.snapshot.identity = captured.identity;
    slot.snapshot.owns_handle = true;
    *token = {static_cast<uint32_t>(available), slot.epoch};
    return {};
}

Result CommitCore::MarkDirty(Token token) noexcept {
    Slot* slot = Find(token);
    if (slot == nullptr) return {Status::Stale, 0};
    Snapshot& snapshot = slot->snapshot;
    if (snapshot.state == State::NeedsRecovery || snapshot.state == State::Failed) {
        return {Status::Failed, snapshot.last_error};
    }
    if (snapshot.dirty_generation >= generation_limit_) {
        snapshot.state = State::Failed;
        snapshot.last_error = EOVERFLOW;
        return {Status::Overflow, EOVERFLOW};
    }
    ++snapshot.dirty_generation;
    if (snapshot.state != State::Committing) snapshot.state = State::Dirty;
    return {};
}

Result CommitCore::BeginCommit(Token token, CommitTicket* ticket) noexcept {
    if (ticket == nullptr) return {Status::Invalid, EINVAL};
    Slot* slot = Find(token);
    if (slot == nullptr) return {Status::Stale, 0};
    Snapshot& snapshot = slot->snapshot;
    if (snapshot.state == State::NeedsRecovery || snapshot.state == State::Failed) {
        return {Status::Failed, snapshot.last_error};
    }
    if (snapshot.state == State::Committing) return {Status::Busy, 0};
    if (snapshot.dirty_generation == snapshot.committed_generation) {
        return {Status::Clean, 0};
    }
    snapshot.sealed_generation = snapshot.dirty_generation;
    snapshot.state = State::Committing;
    snapshot.commit_inflight = true;
    *ticket = {token, snapshot.sealed_generation, slot->handle};
    return {};
}

Result CommitCore::CompleteCommit(const CommitTicket& ticket,
                                  int fsync_error) noexcept {
    Slot* slot = Find(ticket.token);
    if (slot == nullptr) return {Status::Stale, 0};
    Snapshot& snapshot = slot->snapshot;
    if (!snapshot.commit_inflight || ticket.handle != slot->handle ||
        ticket.sealed_generation != snapshot.sealed_generation) {
        return {Status::Stale, 0};
    }
    if (fsync_error < 0) return {Status::Invalid, EINVAL};
    snapshot.commit_inflight = false;
    if (fsync_error != 0) {
        snapshot.state = State::NeedsRecovery;
        snapshot.last_error = fsync_error;
        return {Status::Failed, fsync_error};
    }
    if (snapshot.state == State::Failed) {
        return {Status::Failed, snapshot.last_error};
    }
    snapshot.committed_generation = snapshot.sealed_generation;
    snapshot.sealed_generation = 0;
    snapshot.state = snapshot.dirty_generation == snapshot.committed_generation
                         ? State::Clean
                         : State::Dirty;
    return {};
}

Result CommitCore::Inspect(Token token, Snapshot* snapshot) const noexcept {
    if (snapshot == nullptr) return {Status::Invalid, EINVAL};
    const Slot* slot = Find(token);
    if (slot == nullptr) return {Status::Stale, 0};
    *snapshot = slot->snapshot;
    return {};
}

Result CommitCore::CanRetire(const Slot& slot) const noexcept {
    const Snapshot& snapshot = slot.snapshot;
    if (snapshot.commit_inflight) return {Status::Busy, 0};
    if (snapshot.state == State::NeedsRecovery || snapshot.state == State::Failed) {
        return {Status::Failed, snapshot.last_error};
    }
    if (snapshot.dirty_generation != snapshot.committed_generation) {
        return {Status::Unpaid, 0};
    }
    return {};
}

Result CommitCore::CloseSlot(Slot& slot) noexcept {
    const int error = ops_.close(ops_.context, slot.handle);
    slot.handle = -1;
    slot.snapshot.owns_handle = false;
    if (error != 0) {
        slot.snapshot.state = State::Failed;
        slot.snapshot.last_error = PositiveError(error);
        return {Status::HandleError, slot.snapshot.last_error};
    }
    slot.occupied = false;
    slot.snapshot = {};
    return {};
}

Result CommitCore::Retire(Token token) noexcept {
    Slot* slot = Find(token);
    if (slot == nullptr) return {Status::Stale, 0};
    const Result ready = CanRetire(*slot);
    if (ready.status != Status::Ok) return ready;
    return CloseSlot(*slot);
}

Result CommitCore::CleanShutdown() noexcept {
    for (const Slot& slot : slots_) {
        if (!slot.occupied) continue;
        const Result ready = CanRetire(slot);
        if (ready.status != Status::Ok) return ready;
    }
    Result result;
    for (Slot& slot : slots_) {
        if (!slot.occupied) continue;
        const Result closed = CloseSlot(slot);
        if (closed.status != Status::Ok && result.status == Status::Ok) {
            result = closed;
        }
    }
    return result;
}

}  // namespace modavd::download
