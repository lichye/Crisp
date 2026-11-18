#pragma once
#include "frontend/sva/PropertyMonitor.h"
#include "lowering/environment/AssumptionDependencies.h"
#include "lowering/reports/ExportHierarchyGraph.h"
#include <filesystem>

namespace veval::result_validation {
void checkDatabaseDestination(const std::filesystem::path &log);
void writeDatabase(const std::filesystem::path &log, const lowering::DesignGraph &graph,
                   const std::vector<lowering::PropertyLanding> &transports,
                   const lowering::AssumptionDependencyReport &discharge,
                   const std::vector<lowering::InstanceEdge> &edges,
                   const sva::MonitorSet &sources, std::size_t workers = 1);
} // namespace veval::result_validation
