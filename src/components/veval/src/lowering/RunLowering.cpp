#include "../util/cli/Commands.h"
#include "../util/cli/SpecSelectionAndDiagnostics.h"
#include "util/task/WriteGenerationMetadata.h"
#include "util/MeasureGeneration.h"
#include "util/database/LoweringDatabaseWriter.h"
#include "lowering/environment/AssumptionDependencies.h"
#include "lowering/environment/LocalEnvironment.h"
#include "util/database/SavedVerdicts.h"
#include "lowering/output/ExportLoweringResults.h"
#include "lowering/output/WriteLocalSpecs.h"
#include "util/files/AtomicOutput.h"
#include <ebmc/transition_system.h>
#include <util/namespace.h>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace generation = veval::profile;

namespace veval::cli {

namespace {

// A whole-DUT proof must preserve the wrapper's actual input environment.
bool directWrapperInputs(const lowering::InstanceEdge& dut, const transition_systemt& system) {
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

// The design a stored verdict was decided on, by CONTENT: init, invar and
// trans of the elaborated design, which hold no property. Comments
// (`#source_location') are skipped, so an edit that only moves lines or
// rewrites a property keeps the digest; named children are combined in the
// order of their spelled names, so string-table numbering does not matter.
// Shared subtrees are hashed once.
std::string designDigest(const transition_systemt& system) {
    std::map<const void*, std::uint64_t> memo;
    const auto mix = [](std::uint64_t h, const std::string& text) {
        for (unsigned char c : text) { h ^= c; h *= 1099511628211ull; }
        h ^= 0xff; h *= 1099511628211ull;
        return h;
    };
    std::function<std::uint64_t(const irept&)> hash = [&](const irept& node) -> std::uint64_t {
        const void* key = &node.read();
        if (auto found = memo.find(key); found != memo.end()) return found->second;
        std::uint64_t h = mix(1469598103934665603ull, node.id_string());
        for (const auto& sub : node.get_sub()) h = mix(h, std::to_string(hash(sub)));
        std::vector<std::pair<std::string, std::uint64_t>> named;
        for (const auto& [name, sub] : node.get_named_sub()) {
            const auto spelled = id2string(name);
            if (!spelled.empty() && spelled[0] == '#') continue;
            named.emplace_back(spelled, hash(sub));
        }
        std::sort(named.begin(), named.end());
        for (const auto& [name, value] : named) h = mix(mix(h, name), std::to_string(value));
        memo.emplace(key, h);
        return h;
    };
    std::ostringstream out;
    out << std::hex << hash(system.trans_expr.init()) << ' ' << hash(system.trans_expr.invar()) << ' '
        << hash(system.trans_expr.trans()) << '\n';
    return out.str();
}

}  // namespace

int runLowering(const CommandLineOptions& invocation) {
    // Otherwise an older JSON report could silently describe a different run
    // from the new database. Never remove an existing diagnostic on the user's
    // behalf; refuse before elaborating and require a separate destination.
    if (invocation.logFormat == "sqlite") {
        for (const auto* name : {"transport.json", "reach.json", "graph.json", "discharge.json"})
            if (std::filesystem::exists(std::filesystem::path(invocation.logDir) / name))
                throw std::runtime_error(
                    "--log-format sqlite requires a log directory without legacy JSON reports; "
                    "choose a fresh log directory");
    }
    if (!invocation.logDir.empty())
        veval::result_validation::checkDatabaseDestination(invocation.logDir);
    if (!invocation.generationProfile.empty()) {
        const auto destination = std::filesystem::weakly_canonical(
            std::filesystem::absolute(invocation.generationProfile));
        const auto same = [&](const std::string& path) {
            return !path.empty() && destination == std::filesystem::weakly_canonical(
                std::filesystem::absolute(path));
        };
        const auto inside = [&](const std::string& directory) {
            if (directory.empty()) return false;
            const auto root = std::filesystem::weakly_canonical(std::filesystem::absolute(directory));
            auto d = destination.begin();
            for (auto part = root.begin(); part != root.end(); ++part, ++d)
                if (d == destination.end() || *part != *d) return false;
            return true;
        };
        if (inside(invocation.outPath) || inside(invocation.logDir) ||
            same(invocation.jsonOut) ||
            std::any_of(invocation.options.files.begin(), invocation.options.files.end(), same))
            throw std::runtime_error("generation profile must be separate from input/product/log/report targets");
    }
    generation::Sink timing(invocation.generationProfile);
    generation::Phase whole("generation");
    const auto profile = environmentProfile(invocation);
    if (invocation.unifiedResultLayout && invocation.outPath.empty())
        throw std::runtime_error("--result-layout needs --out Result/<bench>");

    auto design = generation::timed("frontend.elaborate", [&] {
        auto options = invocation.options;
        return veval::frontend::ElaboratedDesign::elaborate(options);
    });
    auto set = generation::timed("frontend.monitors", [&] {
        return veval::sva::selectAssertions(veval::sva::filter(
            veval::sva::monitors(design.transitionSystem(), design.messageHandler()),
            invocation.propertyFilters), invocation.assertionLabels);
    });
    reportRefusals(set);
    // Reject reserved labels before lowering or writing products.
    if (reportReservedLabels(set) != 0) return 2;
    auto edges = generation::timed("design.connection-map", [&] {
        return veval::lowering::connectionMap(design.transitionSystem());
    });
    // The formal wrapper still binds its ports to the whole DUT. With root
    // placement, none of the edges inside that DUT are proof boundaries.
    if (invocation.placement == "root") {
        edges.erase(std::remove_if(edges.begin(), edges.end(), [](const auto& edge) {
            return !edge.parentInstance.empty();
        }), edges.end());
        if (edges.size() != 1 || !directWrapperInputs(edges.front(), design.transitionSystem())) {
            std::cerr << "root placement requires one DUT with distinct, directly wired wrapper inputs\n";
            return 2;
        }
    }
    auto aliases = generation::timed("design.aliases", [&] {
        return veval::lowering::signalAliases(design.transitionSystem());
    });
    auto definitions = generation::timed("design.combinational-definitions", [&] {
        return veval::lowering::symbolic::CombinationalDefinitions(design.transitionSystem(), aliases);
    });
    // Attribution explains failures but must not change the lowering result.
    auto attribution = generation::timed("design.attribution", [&] {
        return invocation.blind ? veval::lowering::Attribution{}
            : veval::lowering::attributionInputs(design.transitionSystem(), edges, aliases,
                                                  invocation.generationThreads.value_or(1));
    });
    // Blackbox detection is independent of the optional attribution pass.
    auto blackboxes = generation::timed("design.blackboxes", [&] {
        return veval::lowering::blackboxInstances(
            design.transitionSystem(), edges, invocation.generationThreads.value_or(1));
    });
    namespacet ns(design.transitionSystem().symbol_table);
    veval::lowering::prepareLocalPortTypes(edges, ns);
    std::vector<veval::lowering::StoppedAbove> stoppedAbove;
    auto transports = generation::timed("transport.initial", [&] {
        return veval::lowering::lowerProperties(set, edges, aliases, attribution, blackboxes,
                                              &stoppedAbove, invocation.generationThreads.value_or(1), &definitions);
    });
    if (invocation.placement == "root") {
        std::set<std::string> boundAssumptions;
        for (auto& row : transports) {
            row.isAssumptionHere = row.isAssumption;
            if (row.abortDropped || !lowering::isTier1(row)) row.placed = false;
            if (row.placed && row.isAssumption &&
                (row.isResetContract || lowering::usableAsAssumption(row)))
                boundAssumptions.insert(row.property);
        }
        for (const auto& item : set.monitors) {
            if (item.isAssumption && !boundAssumptions.count(item.property)) {
                std::cerr << "whole-DUT wrapper could not bind assumption " << item.property << "\n";
                return 2;
            }
        }
        for (const auto& item : set.unsupported) {
            if (item.isAssumption) {
                std::cerr << "whole-DUT wrapper has unsupported assumption " << item.property << "\n";
                return 2;
            }
        }
    }
    generation::Phase preparation("products.prepare");
    // Disclose authored assumptions over driven nets without changing them.
    auto authoredOverDriven =
        veval::lowering::authoredAssumesOverDriven(transports);
    const bool manifestWritten = !invocation.logDir.empty() ||
                                 invocation.json ||
                                 !invocation.jsonOut.empty();
    veval::lowering::warnAuthoredAssumesOverDriven(
        authoredOverDriven, std::cerr, manifestWritten);

    if (invocation.json)
        veval::lowering::writeJson(transports, std::cout, set.unsupported,
                                    authoredOverDriven, stoppedAbove);
    else
        veval::lowering::writeText(transports, std::cout);

    // Share the same environment between products and saved-result matching.
    auto environment = generation::timed("environment", [&] {
        return veval::environment::environment(
            transports, edges, design.transitionSystem(), ns);
    });

    std::map<std::string, std::string> environmentFingerprint;
    std::map<std::string, std::vector<std::string>> environmentShown;
    std::map<std::string, std::set<std::string>> selectedAssumptions;
    if (!profile.isFull()) {
        for (const auto& transport : transports)
            if (veval::lowering::usableAsAssumption(transport) &&
                profile.usesAssumption(transport.instance, transport.property))
                selectedAssumptions[transport.instance].insert(
                    transport.property);
    }
    for (const auto& [instance, own] : environment) {
        std::string digest = own.digest;
        if (!profile.isFull()) {
            digest += "\nenvironment-profile: " + profile.name();
            for (const auto& property : selectedAssumptions[instance])
                digest += "\nassumption: " + property;
        }
        environmentFingerprint[instance] = veval::lowering::fingerprintOf(digest);
        for (const auto& premise : own.premises)
            // Tie-offs already appear as statements in the product.
            if (premise.source != veval::environment::Premise::Source::TieOff)
                environmentShown[instance].push_back(premise.shown);
    }

    // Preserve saved annotations only when the elaborated design still matches.
    veval::environment::VerdictMap verdicts;
    if (!invocation.logDir.empty()) {
        const auto verdictsPath = (std::filesystem::path(invocation.logDir) / "verdicts.json").string();
        std::ifstream previous(verdictsPath);
        if (previous) {
            verdicts = veval::environment::readVerdicts(previous, verdictsPath);
            std::cerr << "read " << verdicts.size() << " previous verdict(s) from " << verdictsPath << "\n";
            if (!verdicts.empty() && readText(verdictsPath + ".design") != designDigest(design.transitionSystem())) {
                std::cerr << "retired " << verdicts.size() << " previous verdict(s): the design changed, or its digest was not recorded\n";
                verdicts.clear();
            }
        }
    }
    auto productOptions = makeProductOptions(invocation, set,
        environmentFingerprint, environmentShown, profile);

    // Products and reports share the same assumption classifications.
    auto discharge = veval::lowering::buildAssumptionDependencies(
        transports, verdicts, environmentFingerprint, ns);
    // Leave annotations outside this run's property filter untouched.
    for (const auto& [key, tier] : discharge.premiseTier) {
        auto found = verdicts.find(key);
        if (found != verdicts.end()) found->second.premiseTier = tier;
    }

    veval::lowering::writeText(discharge, std::cerr,
                                !invocation.logDir.empty());

    preparation.stop();
    productOptions.proofs = std::move(verdicts);
    productOptions.discharge = &discharge;
    writeTransportProducts(
        invocation, transports, aliases, ns, std::move(productOptions));
    writeTransportLogs(invocation,
        {transports, edges, set, authoredOverDriven, stoppedAbove, discharge});

    // Text and JSON use the same lowering records.
    if (!invocation.jsonOut.empty()) {
        veval::util::writeAtomic(invocation.jsonOut, [&](std::ostream& file) {
            veval::lowering::writeJson(transports, file, set.unsupported,
                                        authoredOverDriven, stoppedAbove);
        });
        std::cerr << "wrote JSON report to " << invocation.jsonOut << "\n";
    }

    return 0;
}


} // namespace veval::cli
