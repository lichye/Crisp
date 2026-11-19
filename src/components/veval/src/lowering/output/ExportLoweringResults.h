#pragma once

#include "../../util/cli/ParseArguments.h"
#include "lowering/environment/AssumptionDependencies.h"

namespace veval::cli {

// Shared input/provenance/environment for engine staging and final export.
lowering::ModuleSpecOptions makeProductOptions(
    const CommandLineOptions&, const sva::MonitorSet&,
    const std::map<std::string, std::string>& environmentFingerprint,
    const std::map<std::string, std::vector<std::string>>& environmentShown,
    const lowering::EnvironmentProfile& profile);

void writeTransportProducts(
    const CommandLineOptions&, const std::vector<lowering::PropertyLanding>&,
    const std::map<irep_idt, irep_idt>& aliases, const namespacet&,
    lowering::ModuleSpecOptions);

// Borrowed final records. Rendering cannot change transport or proof decisions.
struct TransportLogView {
    const std::vector<lowering::PropertyLanding>& transports;
    const std::vector<lowering::InstanceEdge>& edges;
    const sva::MonitorSet& monitors;
    const std::vector<lowering::AuthoredAssumeOverDriven>& authoredOverDriven;
    const std::vector<lowering::StoppedAbove>& stoppedAbove;
    const lowering::AssumptionDependencyReport& discharge;
};
void writeTransportLogs(const CommandLineOptions&, const TransportLogView&);
} // namespace veval::cli
