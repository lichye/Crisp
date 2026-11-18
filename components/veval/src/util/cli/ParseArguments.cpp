#include "ParseArguments.h"
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <map>
#include <regex>
#include <stdexcept>
#include <vector>
namespace veval::cli {

namespace {

// Numbers are read whole or refused, naming the option. stoul/stod alone
// read `3abc' as 3, wrap `-1' to 2^64-1 and accept `nan', and each of
// those quietly changed the question a run asked.
std::size_t count(const std::string& option, const std::string& text, std::size_t least) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error(option + " needs a whole number, not `" + text + "'");
    errno = 0;
    char* end = nullptr;
    const auto value = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || *end != '\0')
        throw std::runtime_error(option + ": `" + text + "' is out of range");
    if (value < least)
        throw std::runtime_error(option + " must be at least " + std::to_string(least));
    return static_cast<std::size_t>(value);
}

// A non-negative finite amount (seconds, GiB, an epoch); 0 keeps its
// documented meaning of "no limit".
double amount(const std::string& option, const std::string& text) {
    std::size_t used = 0;
    double value = 0;
    try {
        value = std::stod(text, &used);
    } catch (const std::exception&) {
        used = 0;
    }
    if (used == 0 || used != text.size() || !std::isfinite(value) || value < 0 ||
        text.find_first_of("xX") != std::string::npos)
        throw std::runtime_error(option + " needs a finite non-negative number, not `" +
                                 text + "'");
    return value;
}

}  // namespace

// Hand-rolled rather than reusing the frontend's option parser, so that
// veval's own interface does not drift with hw-cbmc's.
CommandLineOptions parseArgs(int argc, const char* const* argv) {
    CommandLineOptions invocation;
    if (argc < 2) throw std::runtime_error("expected a command");

    invocation.command = argv[1];
    // Retain the old diagnostic spelling for scripts. It is not a solver.
    if (invocation.command == "automaton") invocation.command = "inspect-sva";
    if (invocation.command == "symeval" || invocation.command == "infer-mappings" ||
        invocation.command == "explain")
        throw std::runtime_error("mapping-inference research commands were archived; they are not part of Veval");
    if (invocation.command == "prove" || invocation.command == "evaluate")
        throw std::runtime_error("the built-in EBMC solver has been removed; use veval lower followed by veval check or consflow_check");
    // Keep the invocation for generated product headers.
    invocation.program = argv[0];
    for (int i = 0; i < argc; i++) invocation.argv.push_back(argv[i]);

    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        static const std::set<std::string> retired{
            "--engine", "--bound", "--memory", "--timeout", "--min-depth",
            "--no-monolithic", "--entailment", "--tainted", "--prove",
            "--validate-premises", "--prove-timeout", "--prove-memory",
            "--redeem", "--redeem-shared-memory", "--redeem-deadline",
            "--cover-antecedent", "--cover-bound", "--scout-bound", "--scout-timeout",
            "--escalate", "--escalate-timeout", "--escalate-memory", "--escalate-ladder",
            "--escalate-work", "--escalate-adapter", "--escalate-oss-cad", "--trace-signals"
        };
        if (retired.count(arg.substr(0, arg.find('='))))
            throw std::runtime_error(arg + ": the built-in EBMC solver has been removed; use veval lower followed by veval check or consflow_check");
        if (arg.size() > 1 && arg[0] == '-') invocation.given.insert(arg);
        auto next = [&]() -> std::string {
            if (i + 1 >= argc)
                throw std::runtime_error("option " + arg + " needs a value");
            return argv[++i];
        };

        if (arg == "--top" || arg == "--module") {
            invocation.options.top = next();
        } else if (arg == "--clock") {
            invocation.clockPort = next();
            if (!std::regex_match(invocation.clockPort,
                                 std::regex("[A-Za-z_][A-Za-z_0-9$]*")))
                throw std::runtime_error("--clock needs a local port identifier");
        } else if (arg == "--multiclock") {
            invocation.multiclock = true;
        } else if (arg == "--tier2-notes") {
            invocation.tier2Notes = true;
        } else if (arg == "--product-instance") {
            invocation.productInstance = next();
            if (invocation.productInstance.empty())
                throw std::runtime_error("--product-instance needs an instance path");
        } else if (arg == "--json") {
            invocation.json = true;
        } else if (arg == "-I") {
            invocation.options.includePaths.push_back(next());
        } else if (arg == "-D") {
            invocation.options.defines.push_back(next());
        } else if (arg == "--property") {
            invocation.propertyFilters.push_back(next());
        } else if (arg == "--assertion") {
            invocation.assertionLabels.push_back(next());
            if (!std::regex_match(invocation.assertionLabels.back(), std::regex("[A-Za-z_][A-Za-z_0-9$]*")))
                throw std::runtime_error("--assertion needs a bare source label");
        } else if (arg == "--environment-profile") {
            invocation.environmentProfile = next();
        } else if (arg == "--placement") {
            invocation.placement = next();
            if (invocation.placement != "single-path" && invocation.placement != "split")
                throw std::runtime_error("--placement must be single-path or split");
        } else if (arg == "--environment-allowlist") {
            invocation.environmentAllowlist = next();
        } else if (arg == "--config") {
            invocation.configPath = next();
        } else if (arg == "--boundary") {
            invocation.boundary = next();
        } else if (arg == "--boundary-depth") {
            invocation.boundaryDepth = count(arg, next(), 0);
        } else if (arg == "--min-signals") {
            invocation.minSignals = count(arg, next(), 0);
        } else if (arg == "--internal") {
            invocation.internalBoundary = true;
        } else if (arg == "--to") {
            invocation.reachTo = next();
        } else if (arg == "--list") {
            invocation.freeXList = next();
        } else if (arg == "--certificate" || arg == "--certificate-hops") {
            throw std::runtime_error("correspondence-certificate export was archived; it is not part of Veval");
        } else if (arg == "--json-out") {
            invocation.jsonOut = next();
        } else if (arg == "--hops") {
            // Diagnostic only. Published runs use the frozen bound; this
            // exists so "no path within 4" and "no path at all" can be told
            // apart, which is a measurement nobody can make otherwise.
            invocation.sequentialHops = count(arg, next(), 0);
        } else if (arg == "--out") {
            invocation.outPath = next();
        } else if (arg == "--result-layout") {
            invocation.unifiedResultLayout = true;
        } else if (arg == "--generation-profile") {
            invocation.generationProfile = next();
        } else if (arg == "--generation-threads" || arg == "-j") {
            invocation.generationThreads = count(arg, next(), 1);
        } else if (arg.rfind("-j", 0) == 0 && arg.size() > 2) {
            invocation.generationThreads = count("-j", arg.substr(2), 1);
        } else if (arg == "--infer-mappings" || arg == "--symeval") {
            invocation.symEval = true;
        } else if (arg == "--no-infer-mappings" || arg == "--no-symeval") {
            invocation.symEval = false;
        } else if (arg == "--mapping-cone-cap" || arg == "--symeval-cone-cap") {
            invocation.symEvalConeCap = count(arg, next(), 1);
        } else if (arg == "--mapping-timeout" || arg == "--symeval-timeout") {
            invocation.symEvalTimeoutSeconds = amount(arg, next());
        } else if (arg == "--mapping-crosscheck" || arg == "--symeval-crosscheck" ||
                   arg == "--constraints") {
            throw std::runtime_error("additional mapping inference was archived; it is not part of Veval");
        } else if (arg == "--log") {
            invocation.logDir = next();
        } else if (arg == "--log-format") {
            invocation.logFormat = next();
            if (invocation.logFormat != "both" && invocation.logFormat != "sqlite")
                throw std::runtime_error("--log-format needs both or sqlite");
        } else if (arg == "--blind") {
            invocation.blind = true;
        } else if (arg == "--show-trans") {
            invocation.showTrans = true;
        } else if (arg == "--verilog") {
            invocation.options.systemVerilog = false;
        } else if (arg == "--systemverilog") {
            // Already the default. Accepted so that a script driving ebmc
            // and veval alike does not need to know which it is talking to.
            invocation.options.systemVerilog = true;
        } else if (arg == "-v") {
            invocation.options.verbosity =
                static_cast<unsigned>(count(arg, next(), 0));
        } else if (arg.rfind("-I", 0) == 0 && arg.size() > 2) {
            invocation.options.includePaths.push_back(arg.substr(2));
        } else if (arg.rfind("-D", 0) == 0 && arg.size() > 2) {
            invocation.options.defines.push_back(arg.substr(2));
        } else if (!arg.empty() && arg[0] == '-') {
            throw std::runtime_error("unknown option " + arg);
        } else {
            invocation.options.files.push_back(arg);
        }
    }

    return invocation;
}

// The settings file, applied over the command line.
//
// GenerationConfig wins. A driver script that still carries a flag the campaign's
// settings file has since changed must not quietly run the old value, and a
// reader who has the settings file must not have to also reconstruct the
// shell line to know what ran. Anything the file does not mention keeps
// whatever the flag, or the tool's own default, put there.
void applyConfig(CommandLineOptions& invocation) {
    if (invocation.configPath.empty()) return;

    // `--config' belongs to `transport'. Every field in the file names a
    // valve of the transport pass, and a flag silently ignored is a flag a
    // reader believes took effect.
    if (invocation.command != "transport")
        throw std::runtime_error(
            "--config belongs to `transport', whose valves it sets; `" +
            invocation.command + "' has none of them");

    invocation.config = veval::readGenerationConfig(invocation.configPath);
    const auto& config = invocation.config;

    // Preserve legacy settings readback; enabling archived inference is rejected.
    if (config.symEval) invocation.symEval = *config.symEval;
    if (config.symEvalConeCap)
        invocation.symEvalConeCap =
            static_cast<std::size_t>(*config.symEvalConeCap);
    if (config.symEvalTimeoutSeconds)
        invocation.symEvalTimeoutSeconds = *config.symEvalTimeoutSeconds;
    if (config.environmentProfile)
        invocation.environmentProfile = *config.environmentProfile;
    if (config.placement) invocation.placement = *config.placement;

    // `engine' is parsed, validated and recorded, and it is NOT applied:
    // the engine adapter is a later spec. Recording it now means a settings
    // file written today survives that spec landing; applying it now would
    // mean the file's engine line decided nothing while looking as though it
    // did.
}

void validateInvocation(const CommandLineOptions& invocation) {
    if (invocation.given.count("--log-format")) {
        if (invocation.command != "transport")
            throw std::runtime_error("--log-format belongs to transport");
        if (invocation.logDir.empty())
            throw std::runtime_error("--log-format requires --log DIR");
    }
    if (invocation.generationThreads && invocation.command != "transport")
        throw std::runtime_error("--generation-threads belongs to transport");
    if (!invocation.generationProfile.empty() && invocation.command != "transport")
        throw std::runtime_error("--generation-profile belongs to transport");

    if (!invocation.clockPort.empty() && invocation.command != "transport")
        throw std::runtime_error("--clock belongs to `transport'");
    if (!invocation.assertionLabels.empty() && invocation.command != "transport")
        throw std::runtime_error("--assertion belongs to `transport'");
    if (invocation.multiclock && invocation.command != "transport")
        throw std::runtime_error("--multiclock belongs to `transport'");
    if (invocation.tier2Notes && invocation.command != "transport")
        throw std::runtime_error("--tier2-notes belongs to `transport'");
    if (!invocation.productInstance.empty() && invocation.command != "transport")
        throw std::runtime_error("--product-instance belongs to `transport'");
    if (invocation.symEval)
        throw std::runtime_error("additional mapping inference was archived; it is not part of Veval");
    if (invocation.unifiedResultLayout &&
        invocation.command != "transport")
        throw std::runtime_error(
            "--result-layout belongs to `transport', which writes the "
            "result tree");


}

// Apply the merged configuration once, after validation and before dispatch.
void applyRuntimePolicy(const CommandLineOptions& invocation) {
    if (invocation.sequentialHops)
        lowering::sequentialHops = *invocation.sequentialHops;
    if (invocation.command == "transport")
        lowering::singlePathPlacement = invocation.placement == "single-path";
}

} // namespace veval::cli
