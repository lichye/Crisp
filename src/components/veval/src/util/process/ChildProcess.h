// Helpers for callers that rely on waitpid/wait4 to own their children.
#ifndef VEVAL_UTIL_CHILDPROCESS_H
#define VEVAL_UTIL_CHILDPROCESS_H

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace veval::util {

// Normalize inherited auto-reaping SIGCHLD dispositions before this program
// starts children. A custom handler is left untouched and rejected because it
// may reap children behind the caller's back.
inline bool ensureWaitableChildren(std::string& error) {
    error.clear();
    struct sigaction current {};
    if (::sigaction(SIGCHLD, nullptr, &current) != 0) {
        error = std::string("sigaction(SIGCHLD) query: ") + std::strerror(errno);
        return false;
    }
    if (current.sa_handler != SIG_DFL && current.sa_handler != SIG_IGN) {
        error = "custom SIGCHLD handler is unsupported for wait-owned children";
        return false;
    }
    if (current.sa_handler == SIG_IGN || (current.sa_flags & SA_NOCLDWAIT)) {
        struct sigaction normal {};
        normal.sa_handler = SIG_DFL;
        sigemptyset(&normal.sa_mask);
        if (::sigaction(SIGCHLD, &normal, nullptr) != 0) {
            error = std::string("sigaction(SIGCHLD, SIG_DFL): ") +
                    std::strerror(errno);
            return false;
        }
    }
    return true;
}

// Keep internal pipe/socket descriptors away from stdin/stdout/stderr. This
// prevents a later dup2(fd, 1) followed by closing another RAII fd numbered 1
// from closing the newly installed stdout stream.
inline bool moveAboveStandardDescriptors(int& fd, std::string& error,
                                        const char* description) {
    if (fd > STDERR_FILENO) return true;
    const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (moved < 0) {
        error = std::string(description) + " F_DUPFD_CLOEXEC: " +
                std::strerror(errno);
        return false;
    }
    if (::close(fd) < 0 && errno != EINTR) {
        const int saved = errno;
        ::close(moved);
        error = std::string(description) + " close: " + std::strerror(saved);
        return false;
    }
    fd = moved;
    return true;
}

} // namespace veval::util

#endif
