#include "ExportLoweringResults.h"
#include "../../util/cli/SpecSelectionAndDiagnostics.h"
#include "util/task/WriteGenerationMetadata.h"
#include "util/MeasureGeneration.h"
#include "util/database/LoweringDatabaseWriter.h"
#include "lowering/reports/ReachabilityReport.h"
#include "lowering/reports/ExportHierarchyGraph.h"
#include "lowering/reports/VerdictReport.h"
#include "util/files/AtomicOutput.h"
#include <filesystem>
#include <iostream>

namespace generation = veval::profile;
namespace veval::cli {

lowering::ModuleSpecOptions makeProductOptions(
    const CommandLineOptions& invocation, const sva::MonitorSet& set,
    const std::map<std::string, std::string>& environmentFingerprint,
    const std::map<std::string, std::vector<std::string>>& environmentShown,
    const lowering::EnvironmentProfile& profile) {
    lowering::ModuleSpecOptions options;
    options.boundary = invocation.options.top;
    options.clockPort = invocation.clockPort;
    options.multiclock = invocation.multiclock;
    options.samplingClocks = set.samplingClocks;
    options.instanceOnly = invocation.productInstance;
    options.tier2Notes = invocation.tier2Notes;
    options.boundarySpecPath = boundarySpecPath(invocation.options);
    std::size_t statedHere = 0;
    for (const auto& monitor : set.monitors)
        if (!monitor.isResetContract) statedHere++;
    options.boundaryProperties = statedHere;
    options.environmentFingerprint = environmentFingerprint;
    options.environmentShown = environmentShown;
    options.environmentProfile = profile;
    options.discharge = nullptr;
    options.inputFiles = invocation.options.files;
    options.includePaths = invocation.options.includePaths;
    // The escalation and the main writer use the same command-log pointer.
    for (std::size_t i = 0; i < invocation.argv.size(); ++i)
        options.command += (i ? " " : "") + invocation.argv[i];

    return options;
}

void writeTransportProducts(
    const CommandLineOptions& invocation, const std::vector<lowering::PropertyLanding>& transports,
    const std::map<irep_idt, irep_idt>& aliases, const namespacet& ns,
    lowering::ModuleSpecOptions productOptions) {
    // Tier-1 specifications, with optional Tier-2 explanation files.
    if (!invocation.outPath.empty()) {
        generation::Phase productPhase("products.export");
        const auto resultRoot = std::filesystem::path(invocation.outPath);
        const auto productsDir = invocation.unifiedResultLayout
                                     ? resultRoot / "products"
                                     : resultRoot;
        productOptions.outDir = productsDir.string();
        if (invocation.unifiedResultLayout) {
            productOptions.tier2Dir = (resultRoot / "tier2").string();
            productOptions.tier2Reference = "../tier2";
            productOptions.productReferenceFromTier2 = "../products";
        }
        auto stats = veval::lowering::writeModuleSpecs(transports, aliases, ns,
                                                        productOptions);
        // THE HEADLINE STAYS RAW (ruling D3). `Tier-2 not carried' is the
        // number of (property, instance) pairs that did not carry, which is
        // what report.md's 66,152 and every chip table count; the filter
        // decides what a reader of one module is SHOWN, and shrinking the
        // headline to the listed count would silently improve a published
        // number. So the raw total leads and the split follows it, and the
        // split PARTITIONS it: written out plus counted-by-class is exactly
        // the headline, over the same population an earlier run listed.
        const auto notCarried = stats.tier2Blocked + stats.tier2Aggregated;
        std::cerr << "wrote " << stats.files << " module spec(s) and "
                  << stats.notes << " notes file(s) to " << invocation.outPath
                  << " -- " << stats.tier1 << " Tier-1, " << stats.designStated
                  << " design constant(s), " << stats.tier2Partial
                  << " Tier-2 not whole, " << notCarried
                  << " Tier-2 not carried (" << stats.tier2Blocked
                  << (invocation.tier2Notes ? " written out, " : " individually classified, ")
                  << stats.tier2Aggregated
                  << " counted by class)\n";
        // AND THE THIRD NUMBER, whenever it differs. The class lines in the
        // notes files count a LARGER population than the headline: they also
        // count the pairs nothing arrived at and no premise fired on, which
        // no run has ever written out. A reader who adds up the breakdowns
        // and gets more than the headline is right, and is owed the reason
        // here rather than left to work it out.
        if (stats.tier2Counted != stats.tier2Aggregated)
            std::cerr << (invocation.tier2Notes
                              ? "the class lines in those notes files count "
                              : "the Tier-2 class counts cover ")
                      << stats.tier2Counted << " in all -- "
                      << (stats.tier2Counted - stats.tier2Aggregated)
                      << " more pair(s) that nothing arrived at and no "
                         "premise fired on, which no earlier run listed "
                         "either\n";
        // RULING 6, said out loud rather than left in eight headers: a product
        // that names another compilation unit is not a file a reader can hand
        // to a prover on its own, and the run that wrote it is where that is
        // cheapest to learn.
        if (stats.prerequisite != 0)
            std::cerr << stats.prerequisite << " of those module spec(s) name "
                      << "another compilation unit and carry a PREREQUISITE "
                         "block: their header names the file to read first\n";
        if (stats.stale != 0)
            std::cerr << "removed " << stats.stale
                      << " file(s) left by an earlier run\n";

        if (invocation.unifiedResultLayout) {
            veval::environment::VerdictMap viewVerdicts;
            for (const auto& transport : transports) {
                if (!veval::lowering::isTier1(transport)) continue;
                const auto key = veval::lowering::proofKey(
                    transport.instance, transport.property);
                viewVerdicts[key] = veval::lowering::ProofLine{
                    "unattempted", "not attempted: use veval check or consflow_check"};
            }
            veval::environment::ResultWriterOptions view;
            view.resultDir = resultRoot.string();
            view.productsDir = productsDir.string();
            view.repositoryRoot = std::filesystem::current_path().string();
            view.benchmark = resultRoot.filename().string();
            view.legacyLayout = false;
            const auto counts = veval::environment::writeByVerdict(viewVerdicts, view);
            writeUnifiedResultMetadata(invocation,
                                       productOptions.boundarySpecPath, counts);
            std::cerr << "wrote native by-verdict view to "
                      << (resultRoot / "by-verdict").string() << "\n";
        }
    }

}

void writeTransportLogs(const CommandLineOptions& invocation, const TransportLogView& records) {
    const auto& [transports, edges, set, authoredOverDriven, stoppedAbove, discharge] = records;
    // The LOG: the machine-readable record the experiment reads. The same
    // execution that wrote the product writes it, so the two can never
    // disagree.
    if (!invocation.logDir.empty()) {
        generation::Phase logPhase("logs.export");
        std::filesystem::create_directories(invocation.logDir);
        generation::timed("logs.config", [&] { writeEffectiveConfig(invocation); });
        if (invocation.logFormat == "both") {
            auto path =
                std::filesystem::path(invocation.logDir) / "transport.json";
            generation::timed("logs.transport-json", [&] {
                veval::util::writeAtomic(path.string(), [&](std::ostream& file) {
                    veval::lowering::writeJson(transports, file, set.unsupported, authoredOverDriven,
                                                stoppedAbove);
                });
            });
            std::cerr << "wrote log to " << path.string() << "\n";

            // THE SAME RECORDS, PER AUTHORED ENTRY (SPEC-broadcast D3). Written
            // by the same execution that wrote the ledger, so the two can never
            // disagree, and it checks its own identity as it writes: a report
            // whose buckets do not add up throws rather than being read.
            auto reachPath =
                std::filesystem::path(invocation.logDir) / "reach.json";
            generation::timed("logs.reach-json", [&] {
                veval::util::writeAtomic(reachPath.string(), [&](std::ostream& reachFile) {
                    veval::lowering::writeJson(
                        veval::lowering::reachReport(transports), reachFile);
                });
            });
            std::cerr << "wrote reach report to " << reachPath.string() << "\n";
        }

        // AND THE DESIGN ITSELF (SPEC-broadcast D4). Every question about
        // why an entry stopped where it did is a question about the graph,
        // and the ledger does not contain it: a reader can see that a
        // property stopped at an instance, not that the edge into it binds
        // forty-one ports of which four are of a kind nothing transports
        // across. Later specs -- proof lifting, counterexample lifting --
        // read this rather than the ledger.
        const auto graph = generation::timed("logs.graph-build", [&] {
            return veval::lowering::designGraph(edges, transports, invocation.environmentProfile);
        });
        if (invocation.logFormat == "both") {
            auto graphPath =
                std::filesystem::path(invocation.logDir) / "graph.json";
            generation::timed("logs.graph-json", [&] {
                veval::util::writeAtomic(graphPath.string(), [&](std::ostream& graphFile) {
                    veval::lowering::writeGraphJson(graph, graphFile);
                });
            });
            std::cerr << "wrote design graph to " << graphPath.string() << "\n";

            // AND THE PAIRING, beside it. The native database below stores the
            // same pairing even when these legacy JSON reports are omitted.
            auto dischargePath =
                std::filesystem::path(invocation.logDir) / "discharge.json";
            generation::timed("logs.discharge-json", [&] {
                veval::util::writeAtomic(dischargePath.string(), [&](std::ostream& dischargeFile) {
                    veval::lowering::writeJson(discharge, dischargeFile);
                });
            });
            std::cerr << "wrote discharge report to " << dischargePath.string()
                      << "\n";
        }
        generation::timed("logs.database", [&] {
            veval::result_validation::writeDatabase(invocation.logDir, graph, transports,
                                         discharge, edges, set, invocation.generationThreads.value_or(1));
        });
        std::cerr << "wrote indexed lowering database to "
                  << (std::filesystem::path(invocation.logDir) / "consflow.sqlite") << "\n";
    }

}

} // namespace veval::cli
