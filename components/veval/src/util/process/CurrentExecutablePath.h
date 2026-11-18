#pragma once

#include <filesystem>
#include <stdexcept>
#ifdef __APPLE__
#include <libproc.h>
#include <sys/proc_info.h>
#include <unistd.h>
#endif

namespace veval::util {
// Locate the running image independent of argv[0] and changes to cwd.
inline std::filesystem::path currentExecutable() {
#ifdef __APPLE__
    char path[PROC_PIDPATHINFO_MAXSIZE]{};
    if (::proc_pidpath(::getpid(), path, sizeof path) <= 0)
        throw std::runtime_error("cannot locate the current executable");
    return std::filesystem::canonical(path);
#else
    return std::filesystem::canonical("/proc/self/exe");
#endif
}
} // namespace veval::util
