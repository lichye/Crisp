#pragma once
#include "../util/task/TaskInputAndExecution.h"
#include "util/database/LoweringDatabase.h"

namespace veval::workflow {
// Generation depends on the complete source set, not the requested checks.
std::string loweringKey(const Input &, const J &inventory, const J &effective);
fs::path ensureLowering(Process &, const Input &, const J &inventory, const J &effective,
                        const fs::path &binary, const fs::path &bundle,
                        std::size_t generationThreads = 1);
// The one-shot driver and saved-bundle driver report per-module engine results.
void restoreLowering(const fs::path &bundle, const fs::path &transport, const J &inventory);
int lowerMain(int argc, char **argv);
}
