#ifndef SMART_HELPER_EBMC_H
#define SMART_HELPER_EBMC_H

#include <string>
#include <vector>

namespace smart::helper {

struct EbmcOptions {
    std::string ebmc;
    std::string topModule;
    int bound = 10;
    bool unbounded = true;
    int timeoutSeconds = 0;  // A synthesis block already has its own deadline.
    std::vector<std::string> extraFiles;
    std::size_t designFileIndex = 0;
};

enum class EbmcStatus { Verified, Refuted, Unknown, TimedOut, Error };
const char* statusName(EbmcStatus status);

struct EbmcResult {
    EbmcStatus status;
    std::string detail;
    std::string counterexample{};  // Set only for the requested property and instance.
};

std::string ebmcExecutable();
// True when SMART runs with --allow-constant-oob-read (SMART_ALLOW_CONSTANT_OOB_READ=1):
// EBMC reads the out-of-range bits of a compile-time constant select as x.
bool allowConstantOobRead();
std::string ebmcCommand(const std::string& designFile, const EbmcOptions& options);
std::string freshPropertyLabel(const std::string& source,
                               const std::vector<std::string>& extraFiles);
EbmcResult checkProperty(const std::string& designFile, const EbmcOptions& options,
                         const std::string& label, const std::string& trace = {},
                         const std::string& preferredScope = {});

}  // namespace smart::helper

#endif
