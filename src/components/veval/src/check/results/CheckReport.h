#pragma once
#include "../../util/task/RunProgressLog.h"
#include "util/database/LoweringDatabase.h"
namespace veval::workflow {
void publishReport(Process &, Journal &, Database &, const J &inventory, const J &index,
                   const std::string &runStatus);
}
