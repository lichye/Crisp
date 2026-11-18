#pragma once

#ifdef __APPLE__
#include "ProcessSession.h"
#include <sys/socket.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <csignal>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>

namespace veval::util {

inline long groupMemoryKb(double gib) {
    const long double kb = static_cast<long double>(gib) * 1024 * 1024;
    if (!std::isfinite(gib) || gib < 0 ||
        kb > static_cast<long double>(std::numeric_limits<long>::max()))
        throw std::invalid_argument("context RLIMIT_AS cap conversion overflow/invalid");
    return static_cast<long>(kb);
}

inline std::string applyGroupMemoryLimit(long kilobytes) {
    if (kilobytes <= 0) return {};
#ifdef __APPLE__
    return "context address-space limits are unavailable on macOS";
#else
    if (static_cast<unsigned long long>(kilobytes) >
        std::numeric_limits<rlim_t>::max() / 1024u)
        return "context RLIMIT_AS cap overflow";
    const auto bytes = static_cast<rlim_t>(kilobytes) * 1024u;
    const rlimit cap{bytes, bytes};
    if (::setrlimit(RLIMIT_AS, &cap) < 0)
        return std::string("context RLIMIT_AS: ") + std::strerror(errno);
    return {};
#endif
}

struct GroupWait {
    int status = 0;
    bool timedOut = false;
    std::string error;
};

#ifdef __APPLE__
// Hold a forked proof context before it can create descendants. The
// supervisor verifies that it can enumerate and signal the new session.
inline pid_t forkProofContext() {
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return -1;
    const auto pid = ::fork();
    if (pid == 0) {
        ::close(sockets[0]);
        if (::setsid() < 0 || ::signal(SIGCONT, SIG_DFL) == SIG_ERR ||
            ::send(sockets[1], "R", 1, MSG_NOSIGNAL) != 1) ::_exit(126);
        char ack = 0;
        ssize_t received;
        do { received = ::read(sockets[1], &ack, 1); }
        while (received < 0 && errno == EINTR);
        ::close(sockets[1]);
        if (received != 1 || ack != 'G') ::_exit(126);
        return 0;
    }
    const int forkError = errno;
    ::close(sockets[1]);
    if (pid < 0) { ::close(sockets[0]); errno = forkError; return -1; }
    char ready = 0;
    ssize_t received;
    do { received = ::read(sockets[0], &ready, 1); }
    while (received < 0 && errno == EINTR);
    std::string error;
    const bool launched = received == 1 && ready == 'R' &&
        probePidfd(pid, pid, error) && ::send(sockets[0], "G", 1, MSG_NOSIGNAL) == 1;
    ::close(sockets[0]);
    if (!launched) {
        while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        errno = EIO;
        return -1;
    }
    return pid;
}

inline GroupWait waitForProofContext(pid_t pid, std::chrono::steady_clock::time_point deadline,
    std::chrono::milliseconds pollInterval, const std::function<void()>& progress = [] {}) {
    using Clock = std::chrono::steady_clock;
    GroupWait result;
    auto clean = [&] {
        const auto end = Clock::now() + std::chrono::seconds(5);
        do {
            std::size_t active = 0;
            std::string error;
            if (inspectSession(pid, active, error) && active == 0) {
                const auto waited = ::waitpid(pid, &result.status, WNOHANG);
                if (waited == pid) return;
                if (waited < 0 && errno != EINTR) {
                    result.error = std::string("wait proof context: ") + std::strerror(errno);
                    return;
                }
            } else {
                signalSession(pid, SIGKILL, error);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (Clock::now() < end);
        result.error = "proof context cleanup could not be confirmed";
    };
    try {
        for (;;) {
            siginfo_t info{};
            if (::waitid(P_PID, pid, &info, WEXITED | WNOHANG | WNOWAIT) != 0) {
                if (errno == EINTR) continue;
                result.error = std::string("wait proof context: ") + std::strerror(errno);
                if (errno != ECHILD) clean();
                return result;
            }
            progress();
            if (info.si_pid == pid) break;
            if (Clock::now() >= deadline) { result.timedOut = true; break; }
            std::this_thread::sleep_until(std::min(deadline, Clock::now() + pollInterval));
        }
    } catch (...) {
        clean();
        throw;
    }
    clean();
    return result;
}
#endif

// Call setpgid(0, 0) in the child before it can create descendants. Its
// attempts must inherit this group rather than create their own sessions.
// The parent repeats setpgid to close the scheduling race. Keep the leader
// unreaped until the group is killed, so its PID cannot be reused meanwhile.
inline GroupWait waitForProcessGroup(
    pid_t pid, std::chrono::steady_clock::time_point deadline,
    std::chrono::milliseconds pollInterval,
    const std::function<void()>& progress = [] {}) {
    GroupWait result;
    if (pid <= 0) {
        result.error = "invalid context group pid";
        return result;
    }
    const bool ownsGroup = ::setpgid(pid, pid) == 0 || ::getpgid(pid) == pid;
    auto cleanup = [&] {
        if (::kill(ownsGroup ? -pid : pid, SIGKILL) < 0 && errno != ESRCH &&
            result.error.empty())
            result.error = std::string("kill context group: ") + std::strerror(errno);
        pid_t waited;
        do { waited = ::waitpid(pid, &result.status, 0); }
        while (waited < 0 && errno == EINTR);
        if (waited < 0 && result.error.empty())
            result.error = std::string("wait context group: ") + std::strerror(errno);
    };
    if (!ownsGroup) {
        result.error = "could not isolate context process group";
        cleanup();
        return result;
    }
    try {
        for (;;) {
            siginfo_t info{};
            int waited = ::waitid(P_PID, pid, &info, WEXITED | WNOHANG | WNOWAIT);
            if (waited < 0) {
                if (errno == EINTR) continue;
                const int error = errno;
                result.error = std::string("wait context group: ") + std::strerror(error);
                // ECHILD means the PID is no longer reserved by this parent.
                if (error != ECHILD) cleanup();
                return result;
            }
            progress();
            if (info.si_pid == pid) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                result.timedOut = true;
                break;
            }
            std::this_thread::sleep_until(
                std::min(deadline, std::chrono::steady_clock::now() + pollInterval));
        }
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
    return result;
}

#ifndef __APPLE__
inline pid_t forkProofContext() { return ::fork(); }
inline GroupWait waitForProofContext(pid_t pid, std::chrono::steady_clock::time_point deadline,
    std::chrono::milliseconds pollInterval, const std::function<void()>& progress = [] {}) {
    return waitForProcessGroup(pid, deadline, pollInterval, progress);
}
#endif

} // namespace veval::util
