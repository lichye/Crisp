// veval's lowering inside Crisp (spec 25): the steps of veval's runLowering,
// called as a library with the task's settings, writing one product per
// landing instance into the task's scratch directory. Built in ebmc.mk with the
// engine's flags.
//
// Keep these steps in step with src/components/veval/src/lowering/RunLowering.cpp
// (runLowering): when veval adds a step there, add it here too. Its logs,
// reports and saved verdicts are left out on purpose.
#include "engine.hpp"

#include <frontend/rtl/ElaboratedDesign.h>
#include <frontend/sva/PropertyMonitor.h>
#include <lowering/Symbolic/CombinationalDefinitions.h>
#include <lowering/Symbolic/SignalAliases.h>
#include <lowering/connectivity/ConnectionMap.h>
#include <lowering/environment/AssumptionDependencies.h>
#include <lowering/environment/LocalEnvironment.h>
#include <lowering/output/WriteLocalSpecs.h>
#include <lowering/propagation/PropertyLowering.h>

#include <ebmc/transition_system.h>
#include <util/namespace.h>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace crisp::check {

namespace fs = std::filesystem;
using namespace veval;

void lower_for_veval(const front::Task& task, const fs::path& scratch, Reporter reporter) {
    if (!task.top) throw std::runtime_error("veval needs [design] top");
    frontend::ElaborateOptions options;
    for (const auto& file : task.files) options.files.push_back(fs::absolute(file.value).string());
    options.top = task.top->value;
    for (const auto& dir : task.includes) options.includePaths.push_back(dir.value.string());
    for (const auto& define : task.defines)
        options.defines.push_back(define.value.name + (define.value.value ? "=" + *define.value.value : ""));
    auto design = frontend::ElaboratedDesign::elaborate(options);
    auto& system = design.transitionSystem();

    auto set = sva::monitors(system, design.messageHandler());
    auto aliases = lowering::signalAliases(system);
    auto edges = lowering::connectionMap(system);
    // One clock may have several names: aliases, and a child's input port
    // connected straight to the parent's signal.
    std::map<std::string, std::string> same;
    const auto find = [&](std::string name) {
        while (same.count(name) && same[name] != name) name = same[name];
        return name;
    };
    const auto strip = [](std::string name) { // Verilog::$root.top.clk -> top.clk
        for (const std::string prefix : {"Verilog::", "$root."})
            if (name.compare(0, prefix.size(), prefix) == 0) name.erase(0, prefix.size());
        return name;
    };
    const auto unite = [&](const std::string& a, const std::string& b) {
        const auto x = find(strip(a)), y = find(strip(b));
        if (x != y) same[x] = y;
    };
    for (const auto& [name, representative] : aliases) unite(id2string(name), id2string(representative));
    for (const auto& edge : edges)
        for (const auto& binding : edge.bindings)
            if (!binding.isOutput && binding.parentExpr.id() == ID_symbol)
                unite(id2string(binding.port.get_identifier()),
                      id2string(to_symbol_expr(binding.parentExpr).get_identifier()));
    const auto net = [&](const std::string& name) { return find(strip(name)); };
    std::set<std::string> clocks;
    for (const auto& [property, clock] : set.samplingClocks) clocks.insert(net(clock));
    if (clocks.size() > 1) {
        std::string list;
        for (const auto& clock : clocks) list += (list.empty() ? "" : ", ") + clock;
        throw std::runtime_error("veval: the properties are sampled on more than one clock (" + list +
                                 "); veval supports a single clock");
    }
    // properties PATTERN (spec 20) selects the assertions to lower; assumptions all stay.
    const auto selected = [&](const std::string& name) {
        if (task.properties.empty()) return true;
        for (const auto& pattern : task.properties)
            if (pattern_matches(pattern.value, name)) return true;
        return false;
    };
    set.monitors.erase(std::remove_if(set.monitors.begin(), set.monitors.end(), [&](const auto& m) {
        return !m.isAssumption && !m.isResetContract && !selected(m.property);
    }), set.monitors.end());

    // Announce the assertions in source order; those veval cannot encode get a reason.
    std::size_t index = 0;
    for (const auto& monitor : set.monitors)
        if (!monitor.isAssumption && !monitor.isResetContract) reporter.send(index++, "pending", monitor.property);
    for (const auto& item : set.unsupported) {
        if (item.isAssumption || item.isResetContract || !selected(item.property)) continue;
        reporter.send(index, "pending", item.property);
        reporter.send(index++, "unlowered", "veval cannot encode it: " + item.reason);
    }

    lowering::symbolic::CombinationalDefinitions definitions(system, aliases);
    auto attribution = lowering::attributionInputs(system, edges, aliases, 1);
    auto blackboxes = lowering::blackboxInstances(system, edges, 1);
    const namespacet ns(system.symbol_table);
    lowering::prepareLocalPortTypes(edges, ns); // struct and array ports keep their types in products
    lowering::singlePathPlacement = true;
    std::vector<lowering::StoppedAbove> stopped;
    auto transports = lowering::lowerProperties(set, edges, aliases, attribution, blackboxes, &stopped, 1,
                                                &definitions);

    auto environments = environment::environment(transports, edges, system, ns);
    std::map<std::string, std::string> fingerprints;
    std::map<std::string, std::vector<std::string>> shown;
    for (const auto& [instance, own] : environments) {
        fingerprints[instance] = lowering::fingerprintOf(own.digest);
        for (const auto& premise : own.premises)
            if (premise.source != environment::Premise::Source::TieOff) shown[instance].push_back(premise.shown);
    }
    auto discharge = lowering::buildAssumptionDependencies(transports, {}, fingerprints, ns);

    // An assumption that reaches an instance with an assertion but cannot be
    // stated there is checked without it; the explanation must say so.
    std::set<std::string> checked;
    for (const auto& row : transports)
        if (!row.isAssumption && lowering::isTier1(row)) checked.insert(row.instance);
    for (const auto& row : transports) {
        if (!row.isAssumption || row.isResetContract || !checked.count(row.instance)) continue;
        if (lowering::usableAsAssumption(row)) continue;
        std::string reason = "arrives " + lowering::toString(row.result);
        if (!row.stopReason.empty()) reason += ", " + row.stopReason;
        reporter.send(0, "missing", row.instance + '\t' + row.property + '\t' + reason);
    }

    lowering::ModuleSpecOptions products;
    products.boundary = task.top->value;
    if (task.clock) products.clockPort = task.clock->value;
    products.samplingClocks = set.samplingClocks;
    products.outDir = (scratch / "veval").string();
    for (const auto& monitor : set.monitors)
        if (!monitor.isResetContract) ++products.boundaryProperties;
    products.environmentFingerprint = fingerprints;
    products.environmentShown = shown;
    products.discharge = &discharge;
    products.inputFiles = options.files;
    products.includePaths = options.includePaths;
    fs::create_directories(products.outDir);
    lowering::writeModuleSpecs(transports, aliases, ns, products);
}

} // namespace crisp::check
