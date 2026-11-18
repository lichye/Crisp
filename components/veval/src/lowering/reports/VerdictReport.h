// Native result views written as part of a transport run.

#ifndef VEVAL_ENVIRONMENT_RESULTWRITER_H
#define VEVAL_ENVIRONMENT_RESULTWRITER_H

#include "util/database/SavedVerdicts.h"

#include <map>
#include <string>

namespace veval {
namespace environment {

struct ResultWriterOptions {
    // The result root and its Tier-1 product directory. For the legacy
    // layout they are the same directory; the unified layout uses
    // Result/<bench>/ and Result/<bench>/products/.
    std::string resultDir;
    std::string productsDir;
    std::string repositoryRoot;
    std::string benchmark;
    bool legacyLayout = false;
};

// Write one file per (property, instance) landing below by-verdict/, plus its
// index. The verdict map comes directly from the run; this deliberately never
// reparses verdicts.json.
std::map<std::string, std::size_t> writeByVerdict(
    const VerdictMap&, const ResultWriterOptions&);

}  // namespace environment
}  // namespace veval

#endif  // VEVAL_ENVIRONMENT_RESULTWRITER_H
