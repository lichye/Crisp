#include "LowerDesign.h"

#include "lowering/Symbolic/CombinationalDefinitions.h"
#include "lowering/Symbolic/SignalAliases.h"
#include "lowering/connectivity/ConnectionMap.h"
#include "lowering/output/WriteLocalSpecs.h"
#include "util/MeasureGeneration.h"

#include <ebmc/transition_system.h>
#include <util/namespace.h>
#include <util/std_expr.h>

#include <algorithm>
#include <set>

namespace generation = veval::profile;

namespace veval {
namespace lowering {
namespace {

// A whole-DUT proof must preserve the wrapper's actual input environment.
bool directWrapperInputs(const InstanceEdge& dut, const transition_systemt& system) {
    std::set<std::string> connected;
    std::set<irep_idt> available;
    for (const auto& input : system.inputs()) available.insert(input.get_identifier());
    for (const auto& binding : dut.bindings) {
        const auto port = id2string(binding.port.get_identifier());
        const auto local = port.substr(port.rfind('.') + 1);
        if (binding.isOutput || !dut.inputs.count(local)) continue;
        if (binding.parentExpr.id() != ID_symbol || binding.parentExpr.type() != binding.port.type())
            return false;
        const auto id = to_symbol_expr(binding.parentExpr).get_identifier();
        if (!available.erase(id)) return false;
        connected.insert(local);
    }
    return connected == dut.inputs;
}

} // namespace

LoweredDesign lowerDesign(const frontend::ElaboratedDesign& design, const sva::MonitorSet& set,
                          const LoweringConfig& config) {
    const auto& system = design.transitionSystem();
    singlePathPlacement = config.placement == "single-path";
    LoweredDesign result;
    auto& edges = result.edges;
    edges = generation::timed("design.connection-map", [&] { return connectionMap(system); });
    // The formal wrapper still binds its ports to the whole DUT. With root
    // placement, none of the edges inside that DUT are proof boundaries.
    if (config.placement == "root") {
        edges.erase(std::remove_if(edges.begin(), edges.end(), [](const auto& edge) {
            return !edge.parentInstance.empty();
        }), edges.end());
        if (edges.size() != 1 || !directWrapperInputs(edges.front(), system))
            throw LoweringRefused("root placement requires one DUT with distinct, directly wired wrapper inputs");
    }
    result.aliases = generation::timed("design.aliases", [&] { return signalAliases(system); });
    auto definitions = generation::timed("design.combinational-definitions", [&] {
        return symbolic::CombinationalDefinitions(system, result.aliases);
    });
    // Attribution explains failures but must not change the lowering result.
    auto attribution = generation::timed("design.attribution", [&] {
        return config.blind ? Attribution{} : attributionInputs(system, edges, result.aliases, config.threads);
    });
    // Blackbox detection is independent of the optional attribution pass.
    auto blackboxes = generation::timed("design.blackboxes", [&] {
        return blackboxInstances(system, edges, config.threads);
    });
    const namespacet ns(system.symbol_table);
    prepareLocalPortTypes(edges, ns);
    auto& transports = result.transports;
    transports = generation::timed("transport.initial", [&] {
        return lowerProperties(set, edges, result.aliases, attribution, blackboxes, &result.stoppedAbove,
                               config.threads, &definitions);
    });
    prepareProductFunctions(transports, ns);
    if (config.placement == "root") {
        std::set<std::string> boundAssumptions;
        for (auto& row : transports) {
            row.isAssumptionHere = row.isAssumption;
            if (row.abortDropped || !isTier1(row)) row.placed = false;
            if (row.placed && row.isAssumption && (row.isResetContract || usableAsAssumption(row)))
                boundAssumptions.insert(row.property);
        }
        for (const auto& item : set.monitors)
            if (item.isAssumption && !boundAssumptions.count(item.property))
                throw LoweringRefused("whole-DUT wrapper could not bind assumption " + item.property);
        for (const auto& item : set.unsupported)
            if (item.isAssumption)
                throw LoweringRefused("whole-DUT wrapper has unsupported assumption " + item.property);
    }
    if (config.lowered) config.lowered(result);

    // Share the same environment between products and saved-result matching.
    result.environment = generation::timed("environment", [&] {
        return environment::environment(transports, edges, system, ns);
    });
    const auto& profile = config.profile;
    std::map<std::string, std::set<std::string>> selectedAssumptions;
    if (!profile.isFull()) {
        for (const auto& transport : transports)
            if (usableAsAssumption(transport) && profile.usesAssumption(transport.instance, transport.property))
                selectedAssumptions[transport.instance].insert(transport.property);
    }
    for (const auto& [instance, own] : result.environment) {
        std::string digest = own.digest;
        if (!profile.isFull()) {
            digest += "\nenvironment-profile: " + profile.name();
            for (const auto& property : selectedAssumptions[instance]) digest += "\nassumption: " + property;
        }
        result.environmentFingerprint[instance] = fingerprintOf(digest);
        for (const auto& premise : own.premises)
            // Tie-offs already appear as statements in the product.
            if (premise.source != environment::Premise::Source::TieOff)
                result.environmentShown[instance].push_back(premise.shown);
    }
    if (config.savedVerdicts) result.verdicts = config.savedVerdicts();

    // Products and reports share the same assumption classifications.
    result.discharge = buildAssumptionDependencies(transports, result.verdicts, result.environmentFingerprint, ns);
    // Leave annotations outside this run's property filter untouched.
    for (const auto& [key, tier] : result.discharge.premiseTier) {
        auto found = result.verdicts.find(key);
        if (found != result.verdicts.end()) found->second.premiseTier = tier;
    }
    return result;
}

} // namespace lowering
} // namespace veval
