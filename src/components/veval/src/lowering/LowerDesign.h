#pragma once
// The lowering from an elaborated design to the inputs of writeModuleSpecs:
// connectivity, property lowering, the local environment and the assumption
// dependencies. veval's runLowering and Crisp (src/check/lower.cpp) both call
// lowerDesign, so the two cannot drift apart: add any new step here.

#include "frontend/rtl/ElaboratedDesign.h"
#include "frontend/sva/PropertyMonitor.h"
#include "lowering/environment/AssumptionDependencies.h"
#include "lowering/environment/AssumptionSelection.h"
#include "lowering/environment/LocalEnvironment.h"
#include "lowering/propagation/PropertyLowering.h"

#include <cstddef>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace veval {
namespace lowering {

struct LoweredDesign {
    std::vector<InstanceEdge> edges;
    std::map<irep_idt, irep_idt> aliases;
    std::vector<PropertyLanding> transports;
    std::vector<StoppedAbove> stoppedAbove;
    std::map<std::string, environment::InstanceEnvironment> environment;
    // Per-instance environment fingerprints and the premises a product shows.
    std::map<std::string, std::string> environmentFingerprint;
    std::map<std::string, std::vector<std::string>> environmentShown;
    // Saved proof annotations, with the premise tiers of this run.
    std::map<std::string, ProofLine> verdicts;
    AssumptionDependencyReport discharge;
};

// The settings of a lowering, given in memory.
struct LoweringConfig {
    // single-path, split or root (see singlePathPlacement).
    std::string placement = "single-path";
    std::size_t threads = 1;
    // Skip attribution, which explains failures but does not change the result.
    bool blind = false;
    // Which transported assumptions products state.
    EnvironmentProfile profile;
    // Called once the landings are known, before the environment is built.
    std::function<void(const LoweredDesign&)> lowered;
    // Saved proof annotations to keep; called once the environment is built.
    std::function<std::map<std::string, ProofLine>()> savedVerdicts;
};

// A placement the design does not allow; the message says why.
struct LoweringRefused : std::runtime_error {
    using std::runtime_error::runtime_error;
};

LoweredDesign lowerDesign(const frontend::ElaboratedDesign&, const sva::MonitorSet&, const LoweringConfig&);

} // namespace lowering
} // namespace veval
