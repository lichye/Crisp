#pragma once
#include "../task/CommandArgumentsAndFiles.h"
#include <limits>

namespace veval::verification {
struct Execution {
    int exitCode = -1;
    bool timedOut = false, stopped = false, cleanupComplete = false;
    double elapsed = 0, solving = 0, cleanup = 0;
    long maxRssKb = 0;
    std::string error;
};
Execution execute(const std::vector<std::string> &, const fs::path &log,
                  const fs::path &cwd, double deadline = std::numeric_limits<double>::infinity(),
                  unsigned long long memoryBytes = 0, double cleanupSeconds = 2,
                  bool shareSupervisorSession = false,
                  const fs::path &stderrLog = {}, bool (*externalCancellation)() = nullptr);
std::string program(const std::string &, const std::string &prefix = "");
fs::path helper(const std::string &, const std::string &environment = "");
} // namespace veval::verification
