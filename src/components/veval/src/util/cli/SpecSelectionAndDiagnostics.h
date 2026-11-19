#pragma once

#include "ParseArguments.h"
#include "frontend/sva/PropertyMonitor.h"
#include "lowering/environment/AssumptionSelection.h"

namespace veval::cli {
lowering::EnvironmentProfile environmentProfile(const CommandLineOptions&);
void reportRefusals(const sva::MonitorSet&);
std::size_t reportReservedLabels(const sva::MonitorSet&);
std::string boundarySpecPath(const frontend::ElaborateOptions&);
} // namespace veval::cli
