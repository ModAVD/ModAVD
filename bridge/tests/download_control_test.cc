// Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../companion/jni/download_control.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {
using namespace modavd::download;
constexpr char kNonce[] = "0123456789abcdeffedcba9876543210";
int failures = 0, cases = 0;
char executable[4096];
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #value); ++failures; } } while (0)
#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "%s:%d: %s errno=%d\n", __func__, __LINE__, #value, errno); std::abort(); } } while (0)
uint64_t Now() {
    timespec time{}; REQUIRE(::clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return static_cast<uint64_t>(time.tv_sec) * 1000000000ULL + static_cast<uint64_t>(time.tv_nsec);
}
void Sleep(unsigned ms = 2) { const timespec wait{static_cast<time_t>(ms / 1000), static_cast<long>(ms % 1000) * 1000000}; ::ppoll(nullptr, 0, &wait, nullptr); }
ControlNonce Nonce() { ControlNonce nonce; REQUIRE(ParseControlNonce(kNonce, &nonce)); return nonce; }
struct Scratch {
#ifdef __ANDROID__
    char path[96] = "/data/local/tmp/modavd-control-XXXXXX";
#else
    char path[96] = "/tmp/modavd-control-XXXXXX";
#endif
    int fd = -1;
    Scratch() {
        REQUIRE(::mkdtemp(path) != nullptr);
        fd = ::open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        REQUIRE(fd >= 0);
        const char* files[] = {"debt", "operation"};
        for (const char* name : files) {
            const int file = ::openat(fd, name, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
            REQUIRE(file >= 0); REQUIRE(::write(file, "x", 1) == 1); REQUIRE(::close(file) == 0);
        }
    }
    ~Scratch() {
        const int scan = ::openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        REQUIRE(scan >= 0);
        DIR* dir = ::fdopendir(scan); REQUIRE(dir != nullptr);
        while (const dirent* entry = ::readdir(dir)) {
            if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) REQUIRE(::unlinkat(fd, entry->d_name, 0) == 0);
        }
        ::closedir(dir); ::close(fd); REQUIRE(::rmdir(path) == 0);
    }
};
struct DrainState {
    int fd;
    int calls = 0;
    int failure = 0;
    unsigned delay_ms = 0;
    bool permanent = false;
    static int Drain(void* context, bool permanent) {
        auto& self = *static_cast<DrainState*>(context);
        ++self.calls; self.permanent = self.permanent || permanent;
        if (self.delay_ms) Sleep(self.delay_ms);
        if (self.failure) return self.failure;
        return ::fsync(self.fd) == 0 ? 0 : errno;
    }
};
int Debt(Scratch& scratch) { const int fd = ::openat(scratch.fd, "debt", O_RDWR | O_CLOEXEC); REQUIRE(fd >= 0); return fd; }
void Owner(DownloadControl& owner, Scratch& scratch) {
    REQUIRE(owner.CreateOwner(scratch.fd, Nonce()).status == ControlStatus::Ok);
    REQUIRE(owner.PublishReady().status == ControlStatus::Ok);
}
pid_t Spawn(Scratch& scratch, const char* mode, unsigned timeout = 1500, bool owner = false, const char* nonce = kNonce) {
    const pid_t child = ::fork(); REQUIRE(child >= 0);
    if (child == 0) {
        char timeout_text[32]; std::snprintf(timeout_text, sizeof(timeout_text), "%u", timeout);
        ::execl(executable, executable, owner ? "--owner" : "--helper", scratch.path, nonce, mode, timeout_text, nullptr);
        ::_exit(126);
    }
    return child;
}
int ChildMain(int argc, char** argv) {
    if (argc != 6) return 110;
    ControlNonce nonce; if (!ParseControlNonce(argv[3], &nonce)) return 111;
    const int directory = ::open(argv[2], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return 112;
    const char* mode = argv[4];
    if (std::strcmp(mode, "nondumpable") == 0 || std::strcmp(mode, "serve-nondumpable") == 0) {
        if (::prctl(PR_SET_DUMPABLE, 0) != 0 || ::prctl(PR_GET_DUMPABLE) != 0) return 138;
    }
    DownloadControl control;
    if (std::strcmp(argv[4], "open-busy") == 0) {
        const ControlResult result = control.OpenHelper(directory, nonce);
        if (result.status != ControlStatus::Busy) {
            std::fprintf(stderr, "initializing header OpenHelper status=%d errno=%d\n", static_cast<int>(result.status), result.error);
            return 137;
        }
        return 0;
    }
    if (std::strcmp(argv[1], "--owner") == 0) {
        if (control.CreateOwner(directory, nonce).status != ControlStatus::Ok || control.PublishReady().status != ControlStatus::Ok) return 113;
        if (std::strcmp(mode, "serve-nondumpable") == 0) {
            const int debt = ::openat(directory, "debt", O_RDWR | O_CLOEXEC);
            if (debt < 0) return 139;
            DrainState drain{debt};
            const uint64_t deadline = Now() + 2000000000ULL;
            while (Now() < deadline) {
                const ControlResult service = control.Service(DrainState::Drain, &drain);
                if (service.status != ControlStatus::Ok && service.status != ControlStatus::Busy) return 140;
                ControlSnapshot snapshot;
                if (control.Inspect(&snapshot).status != ControlStatus::Ok) return 141;
                if (drain.calls == 1 && snapshot.state == ControlState::Idle && snapshot.sequence != 0) {
                    ::close(debt); return 0;
                }
                Sleep();
            }
            return 142;
        }
        for (;;) Sleep(20);
    }
    if (control.OpenHelper(directory, nonce).status != ControlStatus::Ok) return 114;
    const bool permanent = std::strcmp(mode, "handoff") == 0;
    ControlTicket ticket;
    const unsigned timeout = static_cast<unsigned>(std::strtoul(argv[5], nullptr, 10));
    const ControlResult began = control.Begin(permanent ? ControlKind::Permanent : ControlKind::Normal, &ticket, timeout);
    if (std::strcmp(mode, "denied") == 0) return began.status == ControlStatus::Poisoned && began.error == EIO ? 0 : 115;
    if (std::strcmp(mode, "timeout") == 0) return began.status == ControlStatus::Timeout ? 0 : 116;
    if (began.status != ControlStatus::Ok) { std::fprintf(stderr, "child Begin status=%d errno=%d\n", static_cast<int>(began.status), began.error); return 117; }
    if (std::strcmp(mode, "wait") == 0) for (;;) Sleep(20);
    if (std::strcmp(mode, "close") == 0) { control.Close(); return 0; }
    if (std::strcmp(mode, "handoff") == 0) return control.FinishHandoff(ticket).status == ControlStatus::Ok ? 0 : 118;
    if (std::strcmp(mode, "delay") == 0) Sleep(80);
    if (std::strcmp(mode, "known-error") == 0) {
        if (::renameat(directory, "missing", directory, "missing2") == 0 || errno != ENOENT) return 119;
        return control.End(ticket, ENOENT).status == ControlStatus::Ok ? 0 : 120;
    }
    if (control.FinishHandoff(ticket).status != ControlStatus::Invalid) return 121;
    const int operation = ::openat(directory, "operation", O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC);
    if (operation < 0 || ::write(operation, "h", 1) != 1 || ::fsync(operation) != 0) return 122;
    ::close(operation);
    if (std::strcmp(mode, "fork-close") == 0) {
        const pid_t copy = ::fork();
        if (copy < 0) return 131;
        if (copy == 0) { control.Close(); ::_exit(0); }
        int status = 0;
        if (::waitpid(copy, &status, 0) != copy || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 132;
        const pid_t third = ::fork();
        if (third < 0) return 133;
        if (third == 0) {
            ::execl(argv[0], argv[0], "--helper", argv[2], argv[3], "normal", argv[5], nullptr);
            ::_exit(126);
        }
        Sleep(80);
        ControlSnapshot snapshot;
        if (control.Inspect(&snapshot).status != ControlStatus::Ok ||
            snapshot.state != ControlState::Executing || snapshot.sequence != ticket.sequence) return 134;
        if (control.End(ticket, 0).status != ControlStatus::Ok) return 135;
        if (::waitpid(third, &status, 0) != third || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 136;
        return 0;
    }
    if (std::strcmp(mode, "late-end") == 0) {
        Sleep(timeout + 30);
        return control.End(ticket, 0).status == ControlStatus::Timeout ? 0 : 123;
    }
    if (control.End(ticket, 0).status != ControlStatus::Ok) return 124;
    if (std::strcmp(mode, "late-ticket") == 0) {
        const ControlTicket old = ticket;
        if (control.Begin(ControlKind::Normal, &ticket, timeout).status != ControlStatus::Ok) return 125;
        if (control.End(old, 0).status != ControlStatus::Stale) return 127;
        ControlTicket wrong_nonce = ticket; wrong_nonce.nonce.high ^= 1;
        if (control.End(wrong_nonce, 0).status != ControlStatus::Stale) return 128;
        ControlSnapshot snapshot;
        if (control.Inspect(&snapshot).status != ControlStatus::Ok || snapshot.state != ControlState::Executing || snapshot.sequence != ticket.sequence) return 129;
        if (control.End(ticket, 0).status != ControlStatus::Ok) return 130;
    }
    return 0;
}
void WaitChild(pid_t child, DownloadControl* owner, DrainState* drain, unsigned limit_ms = 2500) {
    const uint64_t deadline = Now() + static_cast<uint64_t>(limit_ms) * 1000000;
    int status = 0;
    for (;;) {
        if (owner != nullptr) owner->Service(DrainState::Drain, drain);
        const pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result == child) break;
        REQUIRE(result == 0 && Now() < deadline);
        Sleep();
    }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) std::fprintf(stderr, "child exit=%d\n", WEXITSTATUS(status));
    if (owner != nullptr) owner->Service(DrainState::Drain, drain);
}
ControlSnapshot Snapshot(DownloadControl& owner) {
    ControlSnapshot snapshot; REQUIRE(owner.Inspect(&snapshot).status == ControlStatus::Ok); return snapshot;
}
void WaitPhase(DownloadControl& owner, ControlState phase) {
    const uint64_t deadline = Now() + 1500000000ULL;
    while (Snapshot(owner).state != phase) { REQUIRE(Now() < deadline); Sleep(); }
}
off_t OperationSize(Scratch& scratch) { struct stat info{}; REQUIRE(::fstatat(scratch.fd, "operation", &info, AT_SYMLINK_NOFOLLOW) == 0); return info.st_size; }

void NonceAndBootstrap() {
    ControlNonce nonce;
    CHECK(!ParseControlNonce("", &nonce)); CHECK(!ParseControlNonce("00000000000000000000000000000000", &nonce));
    CHECK(!ParseControlNonce("0123456789abcdeffedcba98765432100", &nonce));
    CHECK(ParseControlNonce(kNonce, &nonce));
    Scratch scratch;
    DownloadControl owner, helper, duplicate;
    CHECK(helper.OpenHelper(scratch.fd, nonce).status == ControlStatus::Busy);
    CHECK(owner.CreateOwner(scratch.fd, nonce).status == ControlStatus::Ok);
    CHECK(!owner.AdmissionAllowed());
    CHECK(duplicate.CreateOwner(scratch.fd, nonce).status == ControlStatus::Busy);
    CHECK(helper.OpenHelper(scratch.fd, nonce).status == ControlStatus::Ok);
    CHECK(!helper.AdmissionAllowed());
    CHECK(owner.PublishReady().status == ControlStatus::Ok);
    CHECK(owner.AdmissionAllowed());
    owner.Close(); helper.Close();
    CHECK(duplicate.CreateOwner(scratch.fd, nonce).status == ControlStatus::Stale);
}
void IdleTickAndLock() {
    Scratch scratch; DownloadControl owner; Owner(owner, scratch);
    DrainState drain{Debt(scratch)};
    const pid_t helper = Spawn(scratch, "normal");
    WaitPhase(owner, ControlState::Requested);
    CHECK(!owner.AdmissionAllowed());
    const uint64_t before = Now();
    CHECK(owner.Service(DrainState::Drain, &drain).status == ControlStatus::Ok);
    CHECK(Now() - before < 500000000ULL); // Helper is holding its flock throughout.
    WaitChild(helper, &owner, &drain);
    CHECK(drain.calls == 1 && owner.AdmissionAllowed());
    CHECK(OperationSize(scratch) == 2);
    ::close(drain.fd);
}
void MultipleHelpers() {
    Scratch scratch; DownloadControl owner; Owner(owner, scratch);
    DrainState drain{Debt(scratch)};
    const pid_t first = Spawn(scratch, "delay");
    WaitPhase(owner, ControlState::Requested);
    CHECK(owner.Service(DrainState::Drain, &drain).status == ControlStatus::Ok);
    WaitPhase(owner, ControlState::Executing);
    CHECK(!owner.AdmissionAllowed());
    const pid_t second = Spawn(scratch, "normal");
    WaitChild(first, &owner, &drain); WaitChild(second, &owner, &drain);
    CHECK(drain.calls == 2 && owner.AdmissionAllowed()); CHECK(OperationSize(scratch) == 3);
    ::close(drain.fd);
}
void KnownNamespaceFailure() {
    Scratch scratch; DownloadControl owner; Owner(owner, scratch); DrainState drain{Debt(scratch)};
    WaitChild(Spawn(scratch, "known-error"), &owner, &drain);
    CHECK(owner.AdmissionAllowed()); CHECK(!Snapshot(owner).permanent);
    ::close(drain.fd);
}
void ForkCopyCannotUnlockParent() {
    Scratch scratch; DownloadControl owner; Owner(owner, scratch); DrainState drain{Debt(scratch)};
    WaitChild(Spawn(scratch, "fork-close"), &owner, &drain);
    CHECK(drain.calls == 2 && owner.AdmissionAllowed()); CHECK(OperationSize(scratch) == 3);
    ::close(drain.fd);
}
void PermanentHandoff() {
    Scratch scratch; DownloadControl owner; Owner(owner, scratch); DrainState drain{Debt(scratch)};
    WaitChild(Spawn(scratch, "handoff"), &owner, &drain);
    CHECK(drain.permanent && !owner.AdmissionAllowed());
    CHECK(Snapshot(owner).permanent && Snapshot(owner).error == EINPROGRESS);
    WaitChild(Spawn(scratch, "normal"), &owner, &drain);
    CHECK(drain.calls == 2 && !owner.AdmissionAllowed());
    ::close(drain.fd);
}
void DrainFailure() {
    Scratch scratch; DownloadControl owner; Owner(owner, scratch); DrainState drain{Debt(scratch)}; drain.failure = EIO;
    WaitChild(Spawn(scratch, "denied"), &owner, &drain);
    CHECK(drain.calls == 1 && !owner.AdmissionAllowed()); CHECK(OperationSize(scratch) == 1);
    CHECK(Snapshot(owner).state == ControlState::Poisoned && Snapshot(owner).error == EIO);
    ::close(drain.fd);
}
void RequestTimeout() {
    Scratch scratch; DownloadControl owner; Owner(owner, scratch);
    WaitChild(Spawn(scratch, "timeout", 50), nullptr, nullptr);
    CHECK(!owner.AdmissionAllowed()); CHECK(Snapshot(owner).error == ETIMEDOUT); CHECK(OperationSize(scratch) == 1);
}
void LateDrainCannotGrant() {
    Scratch scratch; DownloadControl owner; Owner(owner, scratch); DrainState drain{Debt(scratch)}; drain.delay_ms = 100;
    const pid_t child = Spawn(scratch, "timeout", 40); WaitPhase(owner, ControlState::Requested);
    const ControlResult result = owner.Service(DrainState::Drain, &drain);
    CHECK(result.status == ControlStatus::Timeout);
    WaitChild(child, &owner, &drain);
    CHECK(!owner.AdmissionAllowed() && Snapshot(owner).state == ControlState::Poisoned); CHECK(OperationSize(scratch) == 1);
    ::close(drain.fd);
}
void DeadHelper(bool granted) {
    Scratch scratch; DownloadControl owner; Owner(owner, scratch); DrainState drain{Debt(scratch)};
    const pid_t child = Spawn(scratch, "wait", 1500); WaitPhase(owner, ControlState::Requested);
    if (granted) {
        REQUIRE(::kill(child, SIGSTOP) == 0);
        int stopped = 0; REQUIRE(::waitpid(child, &stopped, WUNTRACED) == child && WIFSTOPPED(stopped));
    }
    CHECK(owner.Service(DrainState::Drain, &drain).status == ControlStatus::Ok);
    if (granted) CHECK(Snapshot(owner).state == ControlState::Granted);
    else WaitPhase(owner, ControlState::Executing);
    REQUIRE(::kill(child, SIGKILL) == 0); int status = 0; REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK(owner.Service(DrainState::Drain, &drain).status == ControlStatus::Dead);
    CHECK(!owner.AdmissionAllowed() && Snapshot(owner).permanent);
    ::close(drain.fd);
}
void GrantedHelperDeath() { DeadHelper(true); }
void ExecutingHelperDeath() { DeadHelper(false); }
void LateEndAndOldTicket() {
    Scratch scratch; DownloadControl owner; Owner(owner, scratch); DrainState drain{Debt(scratch)};
    WaitChild(Spawn(scratch, "late-ticket"), &owner, &drain);
    CHECK(drain.calls == 2 && owner.AdmissionAllowed());
    ::close(drain.fd);
}
void ExecutionDeadline() {
    Scratch scratch; DownloadControl owner; Owner(owner, scratch); DrainState drain{Debt(scratch)};
    WaitChild(Spawn(scratch, "late-end", 70), &owner, &drain);
    CHECK(!owner.AdmissionAllowed() && Snapshot(owner).error == ETIMEDOUT);
    ::close(drain.fd);
}
void AbandonedExecution() {
    Scratch scratch; DownloadControl owner; Owner(owner, scratch); DrainState drain{Debt(scratch)};
    WaitChild(Spawn(scratch, "close"), &owner, &drain);
    CHECK(!owner.AdmissionAllowed() && Snapshot(owner).error == ECANCELED);
    ::close(drain.fd);
}
void DeadOwnerAndNoElection() {
    Scratch scratch;
    const pid_t owner = Spawn(scratch, "owner", 1500, true);
    DownloadControl helper;
    const uint64_t deadline = Now() + 1500000000ULL;
    for (;;) {
        const ControlResult result = helper.OpenHelper(scratch.fd, Nonce());
        if (result.status == ControlStatus::Ok) break;
        REQUIRE(result.status == ControlStatus::Busy && Now() < deadline); Sleep();
    }
    REQUIRE(::kill(owner, SIGKILL) == 0); int status = 0; REQUIRE(::waitpid(owner, &status, 0) == owner);
    ControlTicket ticket; CHECK(helper.Begin(ControlKind::Normal, &ticket, 100).status == ControlStatus::Dead);
    DownloadControl replacement; CHECK(replacement.CreateOwner(scratch.fd, Nonce()).status == ControlStatus::Stale);
}
void OldSessionCannotMutateNew() {
    Scratch scratch; DownloadControl owner, old_helper; Owner(owner, scratch);
    REQUIRE(old_helper.OpenHelper(scratch.fd, Nonce()).status == ControlStatus::Ok);
    owner.Close();
    ControlNonce fresh = Nonce(); ++fresh.low;
    REQUIRE(owner.CreateOwner(scratch.fd, fresh).status == ControlStatus::Ok); REQUIRE(owner.PublishReady().status == ControlStatus::Ok);
    CHECK(old_helper.Restrict().status == ControlStatus::Stale);
    CHECK(old_helper.Poison(EIO).status == ControlStatus::Stale);
    ControlTicket ticket; CHECK(old_helper.Begin(ControlKind::Normal, &ticket, 50).status == ControlStatus::Stale);
    CHECK(owner.AdmissionAllowed());
    DownloadControl wrong; CHECK(wrong.OpenHelper(scratch.fd, Nonce()).status == ControlStatus::Stale);
}
struct HeaderCutpoint {
    Scratch* scratch;
    const char* nonce;
    int calls = 0;
    static void ConcurrentOpen(void* context) {
        auto& self = *static_cast<HeaderCutpoint*>(context);
        ++self.calls;
        // The new nonce has been written, but owner identity is still old.
        // A real independently exec'ed helper must see Busy, never mixed Dead.
        WaitChild(Spawn(*self.scratch, "open-busy", 1500, false, self.nonce), nullptr, nullptr);
    }
};
void HeaderResetOpenRace() {
    Scratch scratch;
    const pid_t old_owner = Spawn(scratch, "owner", 1500, true);
    DownloadControl observer;
    const uint64_t deadline = Now() + 1500000000ULL;
    for (;;) {
        const ControlResult result = observer.OpenHelper(scratch.fd, Nonce());
        if (result.status == ControlStatus::Ok) break;
        REQUIRE(result.status == ControlStatus::Busy && Now() < deadline); Sleep();
    }
    observer.Close();
    REQUIRE(::kill(old_owner, SIGKILL) == 0);
    int status = 0; REQUIRE(::waitpid(old_owner, &status, 0) == old_owner);
    for (unsigned iteration = 1; iteration != 9; ++iteration) {
        ControlNonce fresh = Nonce(); fresh.low += iteration;
        char text[33];
        std::snprintf(text, sizeof(text), "%016llx%016llx",
                      static_cast<unsigned long long>(fresh.high), static_cast<unsigned long long>(fresh.low));
        HeaderCutpoint cutpoint{&scratch, text};
        DownloadControl owner(HeaderCutpoint::ConcurrentOpen, &cutpoint);
        CHECK(owner.CreateOwner(scratch.fd, fresh).status == ControlStatus::Ok);
        CHECK(cutpoint.calls == 1);
        CHECK(owner.PublishReady().status == ControlStatus::Ok);
        DrainState drain{Debt(scratch)};
        WaitChild(Spawn(scratch, "normal", 1500, false, text), &owner, &drain);
        CHECK(drain.calls == 1 && owner.AdmissionAllowed());
        CHECK(Snapshot(owner).owner.pid == static_cast<int32_t>(::getpid()));
        ::close(drain.fd);
    }
    CHECK(OperationSize(scratch) == 9);
}
void NondumpablePeerHandshake() {
    Scratch scratch;
    const pid_t owner = Spawn(scratch, "serve-nondumpable", 1500, true);
    DownloadControl observer;
    const uint64_t deadline = Now() + 1500000000ULL;
    for (;;) {
        const ControlResult result = observer.OpenHelper(scratch.fd, Nonce());
        if (result.status == ControlStatus::Ok) break;
        REQUIRE(result.status == ControlStatus::Busy && Now() < deadline); Sleep();
    }
    observer.Close();
    const pid_t helper = Spawn(scratch, "nondumpable");
    WaitChild(helper, nullptr, nullptr);
    WaitChild(owner, nullptr, nullptr);
    CHECK(OperationSize(scratch) == 2);
}
void Run(const char* name, void (*test)()) {
    const int before = failures; std::printf("RUN %s\n", name); test(); ++cases;
    std::printf("%s %s\n", failures == before ? "PASS" : "FAIL", name);
}
}  // namespace
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); std::setvbuf(stderr, nullptr, _IONBF, 0);
    if (argc > 1) return ChildMain(argc, argv);
    const ssize_t size = ::readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    REQUIRE(size > 0); executable[size] = '\0';
    Run("nonce bootstrap and independent owner lock", NonceAndBootstrap);
    Run("idle owner Tick grants while helper holds lock", IdleTickAndLock);
    Run("separate exec helpers serialize without blocking owner", MultipleHelpers);
    Run("fork-copy Close cannot release active parent helper lock", ForkCopyCannotUnlockParent);
    Run("known namespace failure permits fresh admission", KnownNamespaceFailure);
    Run("permanent handoff does not claim syscall completion", PermanentHandoff);
    Run("failed real debt drain never grants", DrainFailure);
    Run("unserviced request deadline poisons session", RequestTimeout);
    Run("late successful drain cannot grant expired request", LateDrainCannotGrant);
    Run("helper death after GRANT stays strict", GrantedHelperDeath);
    Run("helper death during EXECUTING stays strict", ExecutingHelperDeath);
    Run("late END cannot finish next sequence", LateEndAndOldTicket);
    Run("execution deadline cannot reopen admission", ExecutionDeadline);
    Run("closing active helper poisons instead of finishing", AbandonedExecution);
    Run("dead owner identity forbids same-session election", DeadOwnerAndNoElection);
    Run("old session cannot poison new nonce", OldSessionCannotMutateNew);
    Run("concurrent helper open cannot join mixed reset header", HeaderResetOpenRace);
    Run("independent nondumpable owner and helper complete fresh handshake", NondumpablePeerHandshake);
    std::printf("%d cases, %d failures\n", cases, failures);
    return failures == 0 ? 0 : 1;
}
