#pragma once

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace veval::util {
// Darwin lacks the atomic CLOEXEC variants. This fallback sets the flag before
// returning; callers must not fork concurrently with descriptor creation.
inline int closeOnExecPair(int fds[2], bool socket) {
#ifdef __APPLE__
    if ((socket ? ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) : ::pipe(fds)) < 0) return -1;
    for (int i = 0; i < 2; ++i) {
        if (::fcntl(fds[i], F_SETFD, FD_CLOEXEC) < 0) {
            int saved = errno;
            ::close(fds[0]); ::close(fds[1]); errno = saved; return -1;
        }
        if (socket) {
            int enabled = 1;
            if (::setsockopt(fds[i], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled) < 0) {
                int saved = errno;
                ::close(fds[0]); ::close(fds[1]); errno = saved; return -1;
            }
        }
    }
    return 0;
#else
    return socket ? ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds)
                  : ::pipe2(fds, O_CLOEXEC);
#endif
}
} // namespace veval::util
