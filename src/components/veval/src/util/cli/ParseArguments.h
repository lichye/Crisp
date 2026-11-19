#pragma once

#include "GenerationConfig.h"
#include "frontend/rtl/ElaboratedDesign.h"
#include "lowering/propagation/PropertyLowering.h"
#include <optional>
#include <set>

namespace veval::cli {

// CLI values and their provenance. Parsing does not change runtime policy.
struct CommandLineOptions {
    std::string command;
    // Every option spelled on the command line, so that one a command would
    // ignore can be refused rather than silently dropped.
    std::set<std::string> given;
    std::string program;
    veval::frontend::ElaborateOptions options;
    std::optional<std::size_t> sequentialHops;
    bool json = false;
    bool showTrans = false;
    bool blind = false;
    // `--out': the tool's PRODUCT. For transport, a directory of per-module
    // specification files; for freex, the rewritten netlist. Everything else
    // any command prints is a log.
    std::string outPath;
    // Explicit local sampling port for transported verification products.
    std::string clockPort;
    // Products are checked with every clock event (SBY multiclock): sample on
    // the instance port the source clock reaches, and tie platform constants
    // and shared clocks in the product's own connections.
    bool multiclock = false;
    std::string productInstance;
    bool unifiedResultLayout = false;
    bool tier2Notes = false;
    // Legacy configuration readback only: enabling inference is rejected.
    bool symEval = false;
    std::string generationProfile;
    std::optional<std::size_t> generationThreads;
    std::size_t symEvalConeCap = 2000;
    double symEvalTimeoutSeconds = 60;
    // `--log': where transport writes its machine-readable record. The
    // experiment reads this; the tool's product never depends on it.
    std::string logDir;
    // SQLite already contains the records needed by native bundle consumers.
    // Keep the legacy JSON exports by default for existing reporting tools.
    std::string logFormat = "both";
    std::string freeXList;
    std::string jsonOut;
    std::string boundary;
    std::string reachTo;
    std::size_t boundaryDepth = 0;
    std::size_t minSignals = 3;
    bool internalBoundary = false;
    std::vector<std::string> propertyFilters;
    // `--assertion': exact assertion labels; assumptions are always kept.
    std::vector<std::string> assertionLabels;
    std::string environmentProfile = "full";
    std::string environmentAllowlist;
    // `--placement single-path|split|root': see lowering::singlePathPlacement.
    std::string placement = "single-path";
    // `--config FILE': the run's settings file. Parsed after the command
    // line and applied OVER it -- see GenerationConfig.h for why that direction.
    std::string configPath;
    veval::GenerationConfig config;
    // The command line verbatim, for the effective record. Nothing reads it
    // to decide behaviour; it exists so that `effective_config.json' can say
    // what was asked for as well as what was used.
    std::vector<std::string> argv;
};

CommandLineOptions parseArgs(int argc, const char* const* argv);
void applyConfig(CommandLineOptions&);
void validateInvocation(const CommandLineOptions&);
void applyRuntimePolicy(const CommandLineOptions&);
void usage();

} // namespace veval::cli
