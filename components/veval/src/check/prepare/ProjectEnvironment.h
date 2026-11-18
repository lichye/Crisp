#pragma once
#include "util/task/CommandArgumentsAndFiles.h"
#include "util/database/LoweringDatabase.h"

namespace veval::verification {
// A private input view for the advisory audit. Proof inputs are never edited.
// Return the original inputs when their hierarchy cannot be sliced exactly.
J projectEnvironmentInputs(Database &db, const std::string &root,
                           const J &queries, const J &inputs, const fs::path &dir);
}
