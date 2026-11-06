// Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "download_journal.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace modavd::download {
namespace {

constexpr size_t kHeaderSize = 40;
constexpr size_t kChecksumOffset = 32;
constexpr char kMagic[8] = {'M', 'A', 'V', 'D', 'J', 'N', '0', '1'};
constexpr char kSuffix[] = ".downloading";
constexpr int kDirectoryFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

class OwnedFd final {
public:
    explicit OwnedFd(int fd = -1) noexcept : fd_(fd) {}
    ~OwnedFd() { if (fd_ >= 0) ::close(fd_); }
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    int get() const noexcept { return fd_; }
    int release() noexcept { const int fd = fd_; fd_ = -1; return fd; }
    void reset(int fd) noexcept { if (fd_ >= 0) ::close(fd_); fd_ = fd; }
private:
    int fd_;
};

JournalResult IoError(int error) noexcept {
    return {JournalStatus::IoError, error > 0 ? error : EINVAL};
}

JournalResult PathError(int error) noexcept {
    if (error == ELOOP || error == ENOTDIR || error == EXDEV) {
        return {JournalStatus::Unsafe, error};
    }
    return IoError(error);
}

bool ValidateKey(const char* key, size_t* length) noexcept {
    if (key == nullptr || key[0] == '\0' || key[0] == '/') return false;
    size_t component = 0;
    size_t component_start = 0;
    size_t index = 0;
    for (; index <= DownloadJournal::kMaxKeyLength; ++index) {
        const unsigned char byte = static_cast<unsigned char>(key[index]);
        if (byte == '\0' || byte == '/') {
            if (component == 0 || component > 255 ||
                (component == 1 && key[component_start] == '.') ||
                (component == 2 && key[component_start] == '.' &&
                 key[component_start + 1] == '.')) return false;
            if (byte == '\0') break;
            component = 0;
            component_start = index + 1;
        } else {
            if (byte < 32 || byte == 127 || byte == '\\') return false;
            ++component;
        }
    }
    if (index > DownloadJournal::kMaxKeyLength ||
        component <= sizeof(kSuffix) - 1 ||
        std::memcmp(key + index - (sizeof(kSuffix) - 1), kSuffix,
                    sizeof(kSuffix) - 1) != 0) return false;
    *length = index;
    return true;
}

void RecordName(size_t index, char (&name)[10]) noexcept {
    std::memcpy(name, "intent-00", 10);
    name[7] = static_cast<char>('0' + index / 10);
    name[8] = static_cast<char>('0' + index % 10);
}

bool ParseRecordName(const char* name, size_t* index) noexcept {
    if (std::strlen(name) != 9 || std::memcmp(name, "intent-", 7) != 0 ||
        name[7] < '0' || name[7] > '9' || name[8] < '0' || name[8] > '9') {
        return false;
    }
    *index = static_cast<size_t>((name[7] - '0') * 10 + name[8] - '0');
    return *index < DownloadJournal::kCapacity;
}

void DraftName(size_t index, char (&name)[9]) noexcept {
    std::memcpy(name, "draft-00", 9);
    name[6] = static_cast<char>('0' + index / 10);
    name[7] = static_cast<char>('0' + index % 10);
}

bool ParseDraftName(const char* name, size_t* index) noexcept {
    if (std::strlen(name) != 8 || std::memcmp(name, "draft-", 6) != 0 ||
        name[6] < '0' || name[6] > '9' || name[7] < '0' || name[7] > '9') return false;
    *index = static_cast<size_t>((name[6] - '0') * 10 + name[7] - '0');
    return *index < DownloadJournal::kCapacity;
}

JournalResult CheckDraft(int directory, const char* name, uint64_t device) noexcept {
    struct stat info{};
    if (::fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW) != 0) return IoError(errno);
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_uid != ::geteuid() ||
        (info.st_mode & 0177) != 0 || static_cast<uint64_t>(info.st_dev) != device ||
        info.st_size < 0 || info.st_size > static_cast<off_t>(kHeaderSize + DownloadJournal::kMaxKeyLength)) {
        return {JournalStatus::Corrupt, EINVAL};
    }
    return {};
}

void Put32(uint8_t* bytes, uint32_t value) noexcept {
    for (unsigned index = 0; index != 4; ++index) {
        bytes[index] = static_cast<uint8_t>(value >> (8 * index));
    }
}

void Put64(uint8_t* bytes, uint64_t value) noexcept {
    for (unsigned index = 0; index != 8; ++index) {
        bytes[index] = static_cast<uint8_t>(value >> (8 * index));
    }
}

uint32_t Get32(const uint8_t* bytes) noexcept {
    uint32_t value = 0;
    for (unsigned index = 0; index != 4; ++index) {
        value |= static_cast<uint32_t>(bytes[index]) << (8 * index);
    }
    return value;
}

uint64_t Get64(const uint8_t* bytes) noexcept {
    uint64_t value = 0;
    for (unsigned index = 0; index != 8; ++index) {
        value |= static_cast<uint64_t>(bytes[index]) << (8 * index);
    }
    return value;
}

uint32_t Checksum(const uint8_t* bytes, size_t length) noexcept {
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index != length; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit != 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

// Return an owned parent, or the nearest existing parent when an intermediate
// component is missing. The latter must be synced before retiring a missing key.
JournalResult ResolveParent(int root, uint64_t root_device, const char* key,
                            int* parent, char (&name)[256], bool* missing) noexcept {
    OwnedFd current(::fcntl(root, F_DUPFD_CLOEXEC, 0));
    if (current.get() < 0) return IoError(errno);
    *missing = false;
    const char* component = key;
    for (;;) {
        const char* slash = std::strchr(component, '/');
        const size_t length = slash == nullptr ? std::strlen(component)
                                              : static_cast<size_t>(slash - component);
        std::memcpy(name, component, length);
        name[length] = '\0';
        if (slash == nullptr) {
            *parent = current.release();
            return {};
        }
        const int next = ::openat(current.get(), name, kDirectoryFlags);
        if (next < 0) {
            if (errno == ENOENT) {
                *missing = true;
                *parent = current.release();
                return {};
            }
            return PathError(errno);
        }
        OwnedFd next_fd(next);
        struct stat info{};
        if (::fstat(next, &info) != 0) return IoError(errno);
        if (!S_ISDIR(info.st_mode) || static_cast<uint64_t>(info.st_dev) != root_device) {
            return {JournalStatus::Unsafe, EXDEV};
        }
        current.reset(next_fd.release());
        component = slash + 1;
    }
}

JournalResult CheckTarget(int parent, const char* name, uint64_t root_device,
                          struct stat* info, bool* missing) noexcept {
    // O_PATH avoids opening devices/FIFOs for I/O before their type is known.
    // O_NOFOLLOW lets fstat reject the symlink itself without following it.
    OwnedFd target(::openat(parent, name, O_PATH | O_NOFOLLOW | O_CLOEXEC));
    if (target.get() < 0) {
        if (errno == ENOENT) { *missing = true; return {}; }
        return PathError(errno);
    }
    if (::fstat(target.get(), info) != 0) return IoError(errno);
    if (!S_ISREG(info->st_mode) || info->st_nlink != 1 ||
        static_cast<uint64_t>(info->st_dev) != root_device) {
        return {JournalStatus::Unsafe, EXDEV};
    }
    *missing = false;
    return {};
}

}  // namespace

DownloadJournal::DownloadJournal(JournalIoOps ops) noexcept : ops_(ops) {}

DownloadJournal::~DownloadJournal() { Close(); }

int DownloadJournal::Sync(int fd) noexcept {
    if (ops_.fsync != nullptr) {
        const int error = ops_.fsync(ops_.context, fd);
        return error >= 0 ? error : EINVAL;
    }
    return ::fsync(fd) == 0 ? 0 : errno;
}

int DownloadJournal::Checkpoint(JournalWriteCheckpoint point) noexcept {
    if (ops_.checkpoint == nullptr) return 0;
    const int error = ops_.checkpoint(ops_.context, point);
    return error >= 0 ? error : EINVAL;
}

JournalResult DownloadJournal::AcquireSession(int journal_directory,
                                              int download_root) noexcept {
    if (journal_fd_ >= 0 || root_fd_ >= 0) return {JournalStatus::Busy, 0};
    if (journal_directory < 0 || download_root < 0) return {JournalStatus::Invalid, EBADF};
    OwnedFd journal(::openat(journal_directory, ".", kDirectoryFlags));
    if (journal.get() < 0) return IoError(errno);
    OwnedFd root(::openat(download_root, ".", kDirectoryFlags));
    if (root.get() < 0) return IoError(errno);
    struct stat journal_info{}, root_info{};
    if (::fstat(journal.get(), &journal_info) != 0 ||
        ::fstat(root.get(), &root_info) != 0) return IoError(errno);
    if (!S_ISDIR(journal_info.st_mode) || !S_ISDIR(root_info.st_mode) ||
        (journal_info.st_dev == root_info.st_dev &&
         journal_info.st_ino == root_info.st_ino)) return {JournalStatus::Invalid, EINVAL};
    if (::flock(journal.get(), LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        if (error == EWOULDBLOCK || error == EAGAIN) return {JournalStatus::Busy, error};
        return IoError(error);
    }
    journal_device_ = static_cast<uint64_t>(journal_info.st_dev);
    root_device_ = static_cast<uint64_t>(root_info.st_dev);
    journal_fd_ = journal.release();
    root_fd_ = root.release();
    ready_ = false;
    live_ = false;
    blocked_ = false;
    return {};
}

JournalResult DownloadJournal::ReadRecord(size_t index, Record* record) noexcept {
    char name[10];
    RecordName(index, name);
    struct stat entry{};
    if (::fstatat(journal_fd_, name, &entry, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? JournalResult{JournalStatus::Corrupt, ENOENT} : IoError(errno);
    }
    if (!S_ISREG(entry.st_mode) || entry.st_nlink != 1 ||
        static_cast<uint64_t>(entry.st_dev) != journal_device_) {
        return {JournalStatus::Corrupt, EINVAL};
    }
    OwnedFd fd(::openat(journal_fd_, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (fd.get() < 0) {
        if (errno == ELOOP || errno == ENOENT) return {JournalStatus::Corrupt, errno};
        return IoError(errno);
    }
    struct stat info{};
    if (::fstat(fd.get(), &info) != 0) return IoError(errno);
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_ino != entry.st_ino ||
        static_cast<uint64_t>(info.st_dev) != journal_device_ ||
        info.st_size <= static_cast<off_t>(kHeaderSize) ||
        info.st_size > static_cast<off_t>(kHeaderSize + kMaxKeyLength)) {
        return {JournalStatus::Corrupt, EINVAL};
    }
    std::array<uint8_t, kHeaderSize + kMaxKeyLength> bytes{};
    const size_t size = static_cast<size_t>(info.st_size);
    size_t done = 0;
    while (done < size) {
        const ssize_t count = ::read(fd.get(), bytes.data() + done, size - done);
        if (count < 0) { if (errno == EINTR) continue; return IoError(errno); }
        if (count == 0) return {JournalStatus::Corrupt, EINVAL};
        done += static_cast<size_t>(count);
    }
    uint8_t extra = 0;
    ssize_t count;
    do { count = ::read(fd.get(), &extra, 1); } while (count < 0 && errno == EINTR);
    if (count < 0) return IoError(errno);
    if (count != 0 || std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0 ||
        Get32(bytes.data() + 8) != 1 || Get32(bytes.data() + 36) != 0) {
        return {JournalStatus::Corrupt, EINVAL};
    }
    const size_t length = Get32(bytes.data() + 12);
    if (length == 0 || length > kMaxKeyLength || size != kHeaderSize + length) {
        return {JournalStatus::Corrupt, EINVAL};
    }
    const uint32_t expected_checksum = Get32(bytes.data() + kChecksumOffset);
    Put32(bytes.data() + kChecksumOffset, 0);
    if (Checksum(bytes.data(), size) != expected_checksum) {
        return {JournalStatus::Corrupt, EINVAL};
    }
    std::memcpy(record->key.data(), bytes.data() + kHeaderSize, length);
    record->key[length] = '\0';
    size_t validated_length = 0;
    if (!ValidateKey(record->key.data(), &validated_length) || validated_length != length) {
        return {JournalStatus::Corrupt, EINVAL};
    }
    record->device = Get64(bytes.data() + 16);
    record->inode = Get64(bytes.data() + 24);
    record->occupied = true;
    return {};
}

JournalResult DownloadJournal::LoadRecords() noexcept {
    for (Record& record : records_) { record.occupied = false; record.key[0] = '\0'; }
    drafts_.fill(false);
    OwnedFd scan(::openat(journal_fd_, ".", kDirectoryFlags));
    if (scan.get() < 0) return IoError(errno);
    DIR* directory = ::fdopendir(scan.get());
    if (directory == nullptr) return IoError(errno);
    scan.release();
    JournalResult result;
    for (;;) {
        errno = 0;
        const dirent* entry = ::readdir(directory);
        if (entry == nullptr) { if (errno != 0) result = IoError(errno); break; }
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        size_t index = 0;
        if (ParseDraftName(entry->d_name, &index)) {
            result = CheckDraft(journal_fd_, entry->d_name, journal_device_);
            if (result.status != JournalStatus::Ok) break;
            drafts_[index] = true;
            continue;
        }
        if (!ParseRecordName(entry->d_name, &index) || records_[index].occupied) {
            result = {JournalStatus::Corrupt, EINVAL};
            break;
        }
        result = ReadRecord(index, &records_[index]);
        if (result.status != JournalStatus::Ok) break;
        for (size_t other = 0; other != kCapacity; ++other) {
            if (other != index && records_[other].occupied &&
                std::strcmp(records_[other].key.data(), records_[index].key.data()) == 0) {
                result = {JournalStatus::Corrupt, EINVAL};
                break;
            }
        }
        if (result.status != JournalStatus::Ok) break;
    }
    const int close_result = ::closedir(directory);
    if (close_result != 0 && result.status == JournalStatus::Ok) return IoError(errno);
    return result;
}

JournalResult DownloadJournal::RemoveDrafts() noexcept {
    bool removed = false;
    for (size_t index = 0; index != kCapacity; ++index) {
        if (!drafts_[index]) continue;
        char name[9];
        DraftName(index, name);
        const JournalResult result = CheckDraft(journal_fd_, name, journal_device_);
        if (result.status != JournalStatus::Ok) return result;
        if (::unlinkat(journal_fd_, name, 0) != 0) return IoError(errno);
        drafts_[index] = false;
        removed = true;
    }
    if (!removed) return {};
    const int error = Sync(journal_fd_);
    return error == 0 ? JournalResult{} : IoError(error);
}

JournalResult DownloadJournal::WriteRecord(size_t index, const Record& record) noexcept {
    char name[10];
    RecordName(index, name);
    char draft[9];
    DraftName(index, draft);
    OwnedFd fd(::openat(journal_fd_, draft,
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (fd.get() < 0) return errno == EEXIST ? JournalResult{JournalStatus::Corrupt, EEXIST}
                                          : IoError(errno);
    std::array<uint8_t, kHeaderSize + kMaxKeyLength> bytes{};
    const size_t length = std::strlen(record.key.data());
    const size_t size = kHeaderSize + length;
    std::memcpy(bytes.data(), kMagic, sizeof(kMagic));
    Put32(bytes.data() + 8, 1);
    Put32(bytes.data() + 12, static_cast<uint32_t>(length));
    Put64(bytes.data() + 16, record.device);
    Put64(bytes.data() + 24, record.inode);
    std::memcpy(bytes.data() + kHeaderSize, record.key.data(), length);
    Put32(bytes.data() + kChecksumOffset, Checksum(bytes.data(), size));
    size_t done = 0;
    while (done < size) {
        size_t written = 0;
        if (ops_.write != nullptr) {
            const int error = ops_.write(ops_.context, fd.get(), bytes.data() + done,
                                         size - done, &written);
            if (error != 0) return IoError(error);
        } else {
            const ssize_t count = ::write(fd.get(), bytes.data() + done, size - done);
            if (count < 0) { if (errno == EINTR) continue; return IoError(errno); }
            written = static_cast<size_t>(count);
        }
        if (written == 0 || written > size - done) return IoError(EIO);
        done += written;
    }
    int error = Sync(fd.get());
    if (error != 0) return IoError(error);
    const int raw_fd = fd.release();
    if (::close(raw_fd) != 0) return IoError(errno);
    error = Checkpoint(JournalWriteCheckpoint::AfterFileSync);
    if (error != 0) return IoError(error);
    if (::renameat2(journal_fd_, draft, journal_fd_, name, RENAME_NOREPLACE) != 0) return IoError(errno);
    error = Checkpoint(JournalWriteCheckpoint::BeforeDirectorySync);
    if (error != 0) return IoError(error);
    error = Sync(journal_fd_);
    return error == 0 ? JournalResult{} : IoError(error);
}

JournalResult DownloadJournal::RemoveRecord(size_t index) noexcept {
    Record on_disk;
    JournalResult result = ReadRecord(index, &on_disk);
    if (result.status != JournalStatus::Ok) return result;
    const Record& record = records_[index];
    if (on_disk.device != record.device || on_disk.inode != record.inode ||
        std::strcmp(on_disk.key.data(), record.key.data()) != 0) {
        return {JournalStatus::Corrupt, ESTALE};
    }
    char name[10];
    RecordName(index, name);
    if (::unlinkat(journal_fd_, name, 0) != 0) return IoError(errno);
    const int error = Sync(journal_fd_);
    if (error != 0) {
        // The unlink already happened. Best effort restores evidence without
        // overwriting anything or hiding the original durability failure.
        // Neither a visible path nor successful restoration is guaranteed.
        WriteRecord(index, record);
        return IoError(error);
    }
    records_[index].occupied = false;
    records_[index].key[0] = '\0';
    return {};
}

JournalResult DownloadJournal::RecoverRecord(size_t index) noexcept {
    int raw_parent = -1;
    char name[256];
    bool missing = false;
    JournalResult result = ResolveParent(root_fd_, root_device_, records_[index].key.data(),
                                         &raw_parent, name, &missing);
    if (result.status != JournalStatus::Ok) return result;
    OwnedFd parent(raw_parent);
    if (!missing) {
        struct stat before{};
        result = CheckTarget(parent.get(), name, root_device_, &before, &missing);
        if (result.status != JournalStatus::Ok) return result;
        if (!missing) {
            struct stat current{};
            if (::fstatat(parent.get(), name, &current, AT_SYMLINK_NOFOLLOW) != 0) {
                if (errno != ENOENT) return PathError(errno);
            } else {
                if (!S_ISREG(current.st_mode) || current.st_nlink != 1 ||
                    current.st_dev != before.st_dev || current.st_ino != before.st_ino) {
                    return {JournalStatus::Unsafe, ESTALE};
                }
                if (::unlinkat(parent.get(), name, 0) != 0 && errno != ENOENT) return IoError(errno);
            }
        }
    }
    const int error = Sync(parent.get());
    if (error != 0) return IoError(error);
    return RemoveRecord(index);
}

JournalResult DownloadJournal::Recover() noexcept {
    if (journal_fd_ < 0 || root_fd_ < 0) return {JournalStatus::Invalid, EBADF};
    if (live_) return {JournalStatus::Busy, 0};
    ready_ = false;
    JournalResult result = LoadRecords();
    if (result.status == JournalStatus::Ok) result = RemoveDrafts();
    if (result.status == JournalStatus::Ok) {
        for (size_t index = 0; index != kCapacity; ++index) {
            if (!records_[index].occupied) continue;
            result = RecoverRecord(index);
            if (result.status != JournalStatus::Ok) break;
        }
    }
    blocked_ = result.status != JournalStatus::Ok;
    ready_ = !blocked_;
    return result;
}

JournalResult DownloadJournal::Admit(const char* relative_key, uint64_t diagnostic_device,
                                     uint64_t diagnostic_inode, JournalToken* token) noexcept {
    if (journal_fd_ < 0 || root_fd_ < 0) return {JournalStatus::Invalid, EBADF};
    if (!ready_ || blocked_) return {JournalStatus::NeedsRecovery, 0};
    size_t length = 0;
    if (token == nullptr || !ValidateKey(relative_key, &length)) return {JournalStatus::Invalid, EINVAL};
    size_t available = kCapacity;
    bool empty = false;
    for (size_t index = 0; index != kCapacity; ++index) {
        const Record& record = records_[index];
        if (record.occupied) {
            if (std::strcmp(record.key.data(), relative_key) == 0) return {JournalStatus::Busy, 0};
        } else {
            empty = true;
            if (available == kCapacity && record.epoch != std::numeric_limits<uint64_t>::max()) available = index;
        }
    }
    if (available == kCapacity) return {empty ? JournalStatus::Overflow : JournalStatus::Capacity, 0};
    int raw_parent = -1;
    char name[256];
    bool missing = false;
    JournalResult result = ResolveParent(root_fd_, root_device_, relative_key, &raw_parent, name, &missing);
    if (result.status != JournalStatus::Ok) return result;
    OwnedFd parent(raw_parent);
    struct stat info{};
    if (!missing) result = CheckTarget(parent.get(), name, root_device_, &info, &missing);
    if (result.status != JournalStatus::Ok) return result;
    if (missing || static_cast<uint64_t>(info.st_dev) != diagnostic_device ||
        static_cast<uint64_t>(info.st_ino) != diagnostic_inode) return {JournalStatus::Unsafe, ESTALE};
    Record& record = records_[available];
    record.occupied = true;
    ++record.epoch;
    record.device = diagnostic_device;
    record.inode = diagnostic_inode;
    std::memcpy(record.key.data(), relative_key, length + 1);
    result = WriteRecord(available, record);
    if (result.status != JournalStatus::Ok) {
        blocked_ = true;
        ready_ = false;
        return result;
    }
    live_ = true;
    *token = {static_cast<uint32_t>(available), record.epoch};
    return {};
}

JournalResult DownloadJournal::Retire(JournalToken token) noexcept {
    if (journal_fd_ < 0 || root_fd_ < 0) return {JournalStatus::Invalid, EBADF};
    if (token.slot >= kCapacity || !records_[token.slot].occupied ||
        records_[token.slot].epoch != token.epoch) return {JournalStatus::Stale, 0};
    if (!ready_ || blocked_) return {JournalStatus::NeedsRecovery, 0};
    const JournalResult result = RemoveRecord(token.slot);
    if (result.status != JournalStatus::Ok) { blocked_ = true; ready_ = false; }
    return result;
}

JournalResult DownloadJournal::Close() noexcept {
    int error = 0;
    if (root_fd_ >= 0) {
        const int fd = root_fd_;
        root_fd_ = -1;
        if (::close(fd) != 0) error = errno;
    }
    if (journal_fd_ >= 0) {
        const int fd = journal_fd_;
        journal_fd_ = -1;
        if (::close(fd) != 0 && error == 0) error = errno;
    }
    ready_ = false;
    live_ = false;
    blocked_ = false;
    for (Record& record : records_) { record.occupied = false; record.key[0] = '\0'; }
    drafts_.fill(false);
    return error == 0 ? JournalResult{} : IoError(error);
}

}  // namespace modavd::download
