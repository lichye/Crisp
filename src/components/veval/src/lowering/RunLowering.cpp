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
#include "lowering/LowerDesign.h"
#include "util/files/AtomicOutput.h"
#include <ebmc/transition_system.h>
#include <util/namespace.h>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace generation = veval::profile;

namespace veval::cli {

namespace {

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
    // The steps from here to the products are shared with Crisp (LowerDesign.h).
    std::optional<generation::Phase> preparation;
    std::vector<veval::lowering::AuthoredAssumeOverDriven> authoredOverDriven;
    veval::lowering::LoweringConfig config;
    config.placement = invocation.placement;
    config.threads = invocation.generationThreads.value_or(1);
    config.blind = invocation.blind;
    config.profile = profile;
    config.lowered = [&](const veval::lowering::LoweredDesign& lowered) {
        preparation.emplace("products.prepare");
        // Disclose authored assumptions over driven nets without changing them.
        authoredOverDriven = veval::lowering::authoredAssumesOverDriven(lowered.transports);
        const bool manifestWritten = !invocation.logDir.empty() ||
                                     invocation.json ||
                                     !invocation.jsonOut.empty();
        veval::lowering::warnAuthoredAssumesOverDriven(
            authoredOverDriven, std::cerr, manifestWritten);

        if (invocation.json)
            veval::lowering::writeJson(lowered.transports, std::cout, set.unsupported,
                                        authoredOverDriven, lowered.stoppedAbove);
        else
            veval::lowering::writeText(lowered.transports, std::cout);
    };
    config.savedVerdicts = [&] {
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
        return verdicts;
    };
    veval::lowering::LoweredDesign lowered;
    try {
        lowered = veval::lowering::lowerDesign(design, set, config);
    } catch (const veval::lowering::LoweringRefused& refused) {
        std::cerr << refused.what() << "\n";
        return 2;
    }
    auto& transports = lowered.transports;
    const auto& edges = lowered.edges;
    const auto& aliases = lowered.aliases;
    const auto& stoppedAbove = lowered.stoppedAbove;
    auto& verdicts = lowered.verdicts;
    const auto& discharge = lowered.discharge;
    namespacet ns(design.transitionSystem().symbol_table);
    auto productOptions = makeProductOptions(invocation, set,
        lowered.environmentFingerprint, lowered.environmentShown, profile);

    veval::lowering::writeText(discharge, std::cerr,
                                !invocation.logDir.empty());

    if (preparation) preparation->stop();
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
