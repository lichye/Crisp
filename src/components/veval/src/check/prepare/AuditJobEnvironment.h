#pragma once
#include "../../util/task/CommandArgumentsAndFiles.h"
namespace veval::verification {
// Runs after product creation, when the design-only file list is known.
// Publishes advisory metadata; never changes products, Jobs, or verdicts.
void annotateEnvironment(const fs::path &log, const J &jobs,
                         const J &inputs, const J &includes,
                         const J &defines, bool systemVerilog, bool inheritProcessSession);
}
