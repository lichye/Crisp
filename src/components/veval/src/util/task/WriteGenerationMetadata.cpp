#include "WriteGenerationMetadata.h"
#include "util/task/CommandArgumentsAndFiles.h"
#include "util/database/JsonEscape.h"
#include "util/files/AtomicOutput.h"
#include <util/json.h>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

namespace veval::cli {

std::string jsonEscape(const std::string& text) {
    return veval::util::jsonEscape(text);
}

std::string shellQuote(const std::string& text) {
    std::string quoted = "'";
    for (char ch : text)
        quoted += ch == '\'' ? "'\\''" : std::string(1, ch);
    return quoted + "'";
}

std::string commandOutput(const std::string& command) {
    std::string output;
    auto* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) return output;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) output += buffer;
    pclose(pipe);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();
    return output;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) return {};
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
}

std::string sha256(const std::string& path) {
    if (path.empty()) return {};
    return veval::verification::fileDigest(path);
}

std::string utcTimestamp() {
    const auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm value{};
    gmtime_r(&now, &value);
    std::ostringstream out;
    out << std::put_time(&value, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

// The run's effective settings, beside its ledger.
//
// The question this answers is "what did this run actually use?", and it is
// not answerable from the shell line once a settings file can override it,
// nor from the settings file once a flag can fill in what the file omits.
// So the tool -- the only party that saw both -- writes the merged values,
// and says for each one where it came from. `scripts/consflow.py' folds this
// into `command.txt'.
void writeEffectiveConfig(const CommandLineOptions& invocation) {
    if (invocation.logDir.empty()) return;

    // Provenance, from the command line as it was given: a flag mentioned
    // there set the value unless the file overrode it. This reads the
    // recorded argv rather than a per-flag bookkeeping set, so a flag added
    // later cannot forget to register itself.
    auto flagGiven = [&invocation](const std::string& flag) {
        for (const auto& argument : invocation.argv)
            if (argument == flag) return true;
        return false;
    };
    auto from = [&](bool inConfig, bool inFlags) {
        return inConfig ? "config" : (inFlags ? "flag" : "default");
    };

    const auto& config = invocation.config;
    const auto path =
        std::filesystem::path(invocation.logDir) / "effective_config.json";
    std::filesystem::create_directories(invocation.logDir);
    veval::util::writeAtomic(path.string(), [&](std::ostream& out) {
        out << "{\n"
            << "  \"schema\": \"consflow-config/1\",\n"
            << "  \"timestamp\": \"" << utcTimestamp() << "\",\n"
            << "  \"binary\": {\"path\": \"" << jsonEscape(invocation.program)
            << "\", \"revision\": \""
            << jsonEscape(commandOutput("git rev-parse HEAD 2>/dev/null"))
            << "\"},\n"
            << "  \"config\": ";
        if (invocation.configPath.empty())
            out << "null";
        else
            out << "{\"path\": \"" << jsonEscape(invocation.configPath)
                << "\", \"sha256\": \"" << sha256(invocation.configPath)
                << "\"}";
        json_objectt frontend;
        frontend["top"] = json_stringt(invocation.options.top);
        frontend["system_verilog"] = jsont::json_boolean(invocation.options.systemVerilog);
        json_arrayt files, includes, defines;
        for (const auto& file : invocation.options.files)
            files.push_back(json_stringt(std::filesystem::absolute(file).lexically_normal().string()));
        for (const auto& path : invocation.options.includePaths)
            includes.push_back(json_stringt(std::filesystem::absolute(path).lexically_normal().string()));
        for (const auto& define : invocation.options.defines) defines.push_back(json_stringt(define));
        frontend["file_order"] = files;
        frontend["include_paths"] = includes;
        frontend["defines"] = defines;
        // Bind later source-sigma audits to this invocation's input bytes. The
        // include inventory is a conservative superset of search-root files,
        // not a claim that every header was opened by the preprocessor.
        std::set<std::string> identityPaths;
        for (const auto& file : invocation.options.files)
            identityPaths.insert(std::filesystem::absolute(file).lexically_normal().string());
        for (const auto& directory : invocation.options.includePaths) {
            if (!std::filesystem::is_directory(directory)) continue;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
                if (entry.is_regular_file())
                    identityPaths.insert(std::filesystem::absolute(entry.path()).lexically_normal().string());
        }
        json_arrayt identities;
        for (const auto& path : identityPaths) {
            json_objectt identity;
            identity["path"] = json_stringt(path);
            identity["sha256"] = json_stringt(sha256(path));
            identities.push_back(identity);
        }
        frontend["input_identities"] = identities;
        frontend["input_identity_scope"] = json_stringt("explicit files and all regular files under declared include search roots");
        out << ",\n  \"frontend\": "; frontend.output(out);
        out << ",\n  \"argv\": [";
        for (std::size_t i = 0; i < invocation.argv.size(); i++)
            out << (i ? ", " : "") << "\"" << jsonEscape(invocation.argv[i])
                << "\"";
        // Retain serialized keys for existing result readers; CLI/config
        // aliases select the same settings and do not change artifact formats.
        out << "],\n"
            << "  \"effective\": {\n"
            << "    \"symeval\": " << (invocation.symEval ? "true" : "false")
            << ",\n    \"symeval_cone_cap\": " << invocation.symEvalConeCap
            << ",\n    \"symeval_timeout\": "
            << invocation.symEvalTimeoutSeconds
            << ",\n    \"environment_profile\": \""
            << jsonEscape(invocation.environmentProfile)
            << "\",\n    \"placement\": \"" << jsonEscape(invocation.placement) << "\"";
        if (!invocation.clockPort.empty())
            out << ",\n    \"clock_port\": \"" << jsonEscape(invocation.clockPort) << "\"";
        if (invocation.multiclock) out << ",\n    \"multiclock\": true";
        out << "\n  },\n"
            << "  \"from\": {\n"
            << "    \"symeval\": \""
            << from(config.symEval.has_value(),
                    flagGiven("--infer-mappings") || flagGiven("--no-infer-mappings") ||
                    flagGiven("--symeval") || flagGiven("--no-symeval"))
            << "\",\n    \"symeval_cone_cap\": \""
            << from(config.symEvalConeCap.has_value(),
                    flagGiven("--mapping-cone-cap") || flagGiven("--symeval-cone-cap"))
            << "\",\n    \"symeval_timeout\": \""
            << from(config.symEvalTimeoutSeconds.has_value(),
                    flagGiven("--mapping-timeout") || flagGiven("--symeval-timeout"))
            << "\",\n    \"environment_profile\": \""
            << from(config.environmentProfile.has_value(),
                    flagGiven("--environment-profile"))
            << "\",\n    \"placement\": \""
            << from(config.placement.has_value(), flagGiven("--placement"))
            << "\"\n  },\n";
        // Declared, recorded, not applied. `applied: false' is part of the
        // record: a reader who finds an engine here and no engine in the run has
        // the answer in the same file.
        out << "  \"engine\": ";
        if (!config.engineName && !config.engineTimeoutSeconds &&
            !config.engineMemoryGb && !config.engineLadder) {
            out << "null\n";
        } else {
            out << "{\"applied\": false";
            if (config.engineName)
                out << ", \"name\": \"" << jsonEscape(*config.engineName) << "\"";
            if (config.engineTimeoutSeconds)
                out << ", \"timeout\": " << *config.engineTimeoutSeconds;
            if (config.engineMemoryGb)
                out << ", \"memory\": " << *config.engineMemoryGb;
            if (config.engineLadder)
                out << ", \"ladder\": \"" << jsonEscape(*config.engineLadder)
                    << "\"";
            out << "}\n";
        }
        out << "}\n";
    });
    std::cerr << "wrote effective settings to " << path.string() << "\n";
}

void writeUnifiedResultMetadata(
    const CommandLineOptions& invocation, const std::string& boundaryFile,
    const std::map<std::string, std::size_t>& counts) {
    const auto root = std::filesystem::path(invocation.outPath);
    const auto bench = root.filename().string();
    std::filesystem::create_directories(root);

    veval::util::writeAtomic((root / "README.md").string(), [&](std::ostream& readme) {
        readme << "# " << bench << " result\n\n"
               << "- `products/` contains the Tier-1 module products.\n"
               << (invocation.tier2Notes
                       ? "- `tier2/` contains the unchanged Tier-2 notes.\n"
                       : "- Tier-2 explanation files are omitted; enable `--tier2-notes` to write them.\n")
               << "- `by-verdict/` contains one rendered file per landing.\n"
               << "- `manifest.json` records this run's inputs and budgets.\n\n"
               << "The machine-readable ledger is under `log/" << bench
               << "/`.\n";

    });
    const auto netlist = invocation.options.files.empty()
                             ? std::string()
                             : invocation.options.files.back();
    const auto revision = commandOutput("git rev-parse HEAD 2>/dev/null");
    veval::util::writeAtomic((root / "manifest.json").string(), [&](std::ostream& manifest) {
        manifest << "{\n"
                 << "  \"schema\": \"consflow-result/1\",\n"
                 << "  \"binary_revision\": \"" << jsonEscape(revision)
                 << "\",\n"
                 << "  \"timestamp\": \"" << utcTimestamp() << "\",\n"
                 << "  \"inputs\": {\"spec\": \"" << jsonEscape(boundaryFile)
                 << "\", \"spec_sha256\": \"" << sha256(boundaryFile)
                 << "\", \"netlist\": \"" << jsonEscape(netlist)
                 << "\", \"netlist_sha256\": \"" << sha256(netlist)
                 << "\"},\n"
                 << "  \"budgets\": {\"symeval_cone_cap\": "
                 << invocation.symEvalConeCap << ", \"symeval_timeout_s\": "
                 << invocation.symEvalTimeoutSeconds << "},\n"
                 << "  \"flags\": {\"symeval\": " << (invocation.symEval ? "true" : "false")
                 << "},\n"
                 << "  \"counts\": {";
        bool first = true;
        for (const auto& [verdict, count] : counts) {
            if (!first) manifest << ", ";
            manifest << "\"" << jsonEscape(verdict) << "\": " << count;
            first = false;
        }
        manifest << "}\n}\n";

    });
    const auto legacy = root.parent_path() / (bench + "Spec");
    std::error_code error;
    if (std::filesystem::is_symlink(legacy, error))
        std::filesystem::remove(legacy, error);
    if (!std::filesystem::exists(legacy, error)) {
        const auto target = std::filesystem::relative(root / "products",
                                                       legacy.parent_path(), error);
        std::filesystem::create_directory_symlink(
            error ? root / "products" : target, legacy, error);
    }
}


} // namespace veval::cli
