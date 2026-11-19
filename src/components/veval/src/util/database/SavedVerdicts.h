#pragma once
// Import/export proof annotations without running a solver. Existing records
// remain readable; current verification is performed by the SBY runtime.
#include "lowering/output/WriteLocalSpecs.h"
#include <iosfwd>
#include <map>
#include <string>

namespace veval::environment {
using VerdictMap = std::map<std::string, lowering::ProofLine>;
void writeVerdicts(const VerdictMap&, std::ostream&);
VerdictMap readVerdicts(std::istream&, const std::string& sourcePath = {});
}
