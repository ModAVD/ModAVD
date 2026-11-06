// Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later.
#ifndef MODAVD_DOWNLOAD_JOURNAL_H_
#define MODAVD_DOWNLOAD_JOURNAL_H_

#include <array>
#include <cstddef>
#include <cstdint>

namespace modavd::download {

enum class JournalStatus {
    Ok,
    Busy,
    Invalid,
    Stale,
    Capacity,
    Overflow,
    NeedsRecovery,
    Corrupt,
    Unsafe,
    IoError,
};

struct JournalResult {
    JournalStatus status = JournalStatus::Ok;
    int error = 0;
};

struct JournalToken {
    uint32_t slot = UINT32_MAX;
    uint64_t epoch = 0;
};

enum class JournalWriteCheckpoint { AfterFileSync, BeforeDirectorySync };

// Optional fault injection. Return zero or a positive errno, do not close fd.
// Null callbacks use real POSIX fsync/write and omit checkpoints. A successful
// write callback reports 1..length bytes; it may perform a real short write.
// fsync and checkpoints are never retried here.
struct JournalIoOps {
    void* context = nullptr;
    int (*fsync)(void*, int fd) = nullptr;
    int (*write)(void*, int fd, const void* bytes, size_t length, size_t* written) = nullptr;
    int (*checkpoint)(void*, JournalWriteCheckpoint) = nullptr;
};

// One coordinator owns a session. The private journal directory must be bound
// by the adapter to this exact application's logical download root across
// restarts, and contain only this journal's files. Inputs are trusted directory
// fds, never paths supplied by an intent. The class opens independent directory
// OFDs and holds LOCK_EX|LOCK_NB on the journal directory, not the download root.
// That cooperative lock does NOT prevent game access. The caller must exclude
// every game process and namespace mutation throughout startup Recover.
//
// Intents identify exact relative cache keys, not persistent inode identities.
// Recovery may delete a replacement inode at the same unpublished cache key.
// The adapter must admit only experiment-owned, unpublished temporary objects;
// completed names, account data and other game resources are outside this API.
class DownloadJournal final {
public:
    static constexpr size_t kCapacity = 32;
    static constexpr size_t kMaxKeyLength = 1023;

    explicit DownloadJournal(JournalIoOps ops = {}) noexcept;
    ~DownloadJournal();
    DownloadJournal(const DownloadJournal&) = delete;
    DownloadJournal& operator=(const DownloadJournal&) = delete;
    DownloadJournal(DownloadJournal&&) = delete;
    DownloadJournal& operator=(DownloadJournal&&) = delete;

    JournalResult AcquireSession(int journal_directory, int download_root) noexcept;
    // Validate all intent files before touching any cache object. Strictly named
    // owned drafts are removed without reading a key or touching cache objects.
    // A partial or corrupt FINAL intent is still an error. Missing cache keys
    // are idempotent, but their nearest existing parent must still be synced.
    // Refuse symlinks, mount changes, non-regular objects and nlink != 1.
    // Recover is startup-only; after a successful Admit, close/reacquire under
    // external exclusivity before attempting recovery again.
    JournalResult Recover() noexcept;
    // A key has nonempty components, no . or .., at most 255 bytes/component,
    // and a final name with a nonempty prefix plus exactly ".downloading".
    // Reject absolute paths, backslashes and ASCII control characters.
    // Current identity must match these diagnostics at admission. Recovery
    // deliberately does not require the historical dev/ino to match.
    // Only Ok permits early ACK, after draft fsync, atomic NOREPLACE publication
    // as a final intent, AND journal-directory fsync.
    JournalResult Admit(const char* relative_key, uint64_t diagnostic_device,
                        uint64_t diagnostic_inode, JournalToken* token) noexcept;
    // The adapter may call this only after REAL fsync of the admitted object
    // has succeeded with no newer unpaid events. It must coordinate retirement
    // with all writers and later admissions. This class cannot establish those
    // facts. Ok means intent
    // removal reached the journal directory, not that publication was proven.
    JournalResult Retire(JournalToken token) noexcept;
    // Release only owned fds and the lock. This never retires intents or claims
    // cleanliness. An I/O failure blocks new admissions until startup recovery.
    JournalResult Close() noexcept;

private:
    struct Record {
        bool occupied = false;
        uint64_t epoch = 0;
        uint64_t device = 0;
        uint64_t inode = 0;
        std::array<char, kMaxKeyLength + 1> key{};
    };

    JournalResult LoadRecords() noexcept;
    JournalResult RemoveDrafts() noexcept;
    JournalResult ReadRecord(size_t index, Record* record) noexcept;
    JournalResult WriteRecord(size_t index, const Record& record) noexcept;
    JournalResult RemoveRecord(size_t index) noexcept;
    JournalResult RecoverRecord(size_t index) noexcept;
    int Sync(int fd) noexcept;
    int Checkpoint(JournalWriteCheckpoint point) noexcept;

    JournalIoOps ops_;
    int journal_fd_ = -1;
    int root_fd_ = -1;
    uint64_t journal_device_ = 0;
    uint64_t root_device_ = 0;
    bool ready_ = false;
    bool live_ = false;
    bool blocked_ = false;
    std::array<Record, kCapacity> records_{};
    std::array<bool, kCapacity> drafts_{};
};

}  // namespace modavd::download
#endif  // MODAVD_DOWNLOAD_JOURNAL_H_
