#pragma once
#include "../../util/task/RunProgressLog.h"
namespace veval::workflow {
int workflow(Process &, Journal &);
void completed(const J &, bool includeStderr = false);
}
