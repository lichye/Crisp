// smart: mine formally verified SystemVerilog assertions from one design file.
//
//   smart cat.sv        ->  cat_assertion.sv
//
// The binary has a second, internal role: `smart --block ...` runs a single
// synthesis block. The pipeline re-execs itself that way instead of shelling
// out to a separate worker, so there is still exactly one binary to ship —
// see BlockRunner.h for why blocks are processes and not threads.

#include <cstdlib>
#include "Version.h"
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "BlockRunner.h"
#include "HwcbmcFrontend.h"
#include "Options.h"
#include "Pipeline.h"
#include "SVModule.h"

namespace {

namespace fs = std::filesystem;

// Let callers use simulators installed on PATH or in the local tool suite.
void addBundledToolsToPath(const std::string& self) {
    const auto executableDir = fs::path(self).parent_path();
    const auto root = executableDir.parent_path();
    const std::vector<fs::path> candidates = {
        // Release archives put smart and ebmc next to each other in bin/.
        executableDir,
        root / ".deps/oss-cad-suite/bin",
        root / "otherTools/oss-cad-suite/bin",
    };

    std::string path = std::getenv("PATH") != nullptr ? std::getenv("PATH") : "";
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (!fs::is_directory(candidate, error)) continue;
        if (path.find(candidate.string()) != std::string::npos) continue;
        path += (path.empty() ? "" : ":") + candidate.string();
    }
    setenv("PATH", path.c_str(), 1);
}

// Absolute path to this executable, for re-exec.
std::string selfPath(const char* argv0) {
    std::error_code error;
    const auto viaProc = fs::read_symlink("/proc/self/exe", error);
    if (!error) return viaProc.string();
    return fs::absolute(argv0).string();
}

int dumpFrontend(const smart::pipeline::Options& options) {
    if (options.designFiles().empty()) {
        std::cerr << "smart: --dump-frontend needs a design file\n";
        return static_cast<int>(smart::pipeline::ExitCode::UserError);
    }
    const auto& files = options.designFiles();
    const auto& file = files.front();
    std::string top = options.getString("top");
    if (top.empty()) top = fs::path(file).stem().string();

    smart::frontend::HwcbmcOptions frontendOptions;
    frontendOptions.autoDiscoverSiblings =
        files.size() == 1 && options.getBool("discover_siblings");
    frontendOptions.extraFiles.assign(files.begin() + 1, files.end());
    const auto info = smart::frontend::parseModuleFile(file, top, frontendOptions);
    std::cout << smart::frontend::dumpJson(info) << "\n";
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace smart::pipeline;

    // Internal: run one block. Everything after --block is the block's own
    // argv, matching the standalone smart.out contract.
    if (argc >= 2 && std::string(argv[1]) == "--block") {
        std::vector<char*> blockArgv;
        blockArgv.push_back(argv[0]);
        for (int i = 2; i < argc; ++i) blockArgv.push_back(argv[i]);
        try {
            return runSmartBlock(static_cast<int>(blockArgv.size()), blockArgv.data());
        } catch (const std::exception& e) {
            std::cerr << "smart: " << e.what() << "\n";
            return static_cast<int>(ExitCode::Failure);
        }
    }

    Options options;
    try {
        options.parseCommandLine(argc, argv);
        options.validate();
    } catch (const std::exception& e) {
        std::cerr << "smart: " << e.what() << "\n\n" << Options::usage();
        return static_cast<int>(ExitCode::UserError);
    }

    if (options.versionRequested()) {
        std::cout << "smart " << smart::version() << "\n";
        return 0;
    }

    if (options.helpRequested()) {
        std::cout << Options::usage();
        return 0;
    }

    const auto self = selfPath(argv[0]);
    if (const auto* ebmc = std::getenv("SMART_EBMC")) {
        const auto path = fs::absolute(ebmc).string();
        setenv("SMART_EBMC", path.c_str(), 1);
    } else {
        const auto packaged = fs::path(self).parent_path() / "ebmc";
        if (fs::is_regular_file(packaged))
            setenv("SMART_EBMC", packaged.c_str(), 1);
    }
    if (const auto* cvc5 = std::getenv("SMART_CVC5")) {
        const auto path = fs::absolute(cvc5).string();
        setenv("SMART_CVC5", path.c_str(), 1);
    } else {
        const auto packaged = fs::path(self).parent_path() / "cvc5";
        if (fs::is_regular_file(packaged))
            setenv("SMART_CVC5", packaged.c_str(), 1);
    }
    addBundledToolsToPath(self);

    if (options.checkEnvRequested())
        return static_cast<int>(checkEnvironment(options, self));

    if (options.dumpConfigRequested()) {
        for (const auto& warning : options.warnings())
            std::cerr << "note: " << warning << "\n";
        std::cout << options.toJson() << "\n";
        return 0;
    }

    if (options.dumpFrontendRequested()) {
        try {
            return dumpFrontend(options);
        } catch (const std::exception& e) {
            std::cerr << "smart: " << e.what() << "\n";
            return static_cast<int>(ExitCode::UserError);
        }
    }

    RunSummary summary;
    ExitCode code;
    try {
        Pipeline pipeline(options, self);
        code = pipeline.run(summary);
    } catch (const std::exception& e) {
        std::cerr << "smart: " << e.what() << "\n";
        return static_cast<int>(ExitCode::Failure);
    }

    if (code == ExitCode::Success) {
        std::cout << summary.verifiedAssertions << " ";
        if (!summary.finalChecked)
            std::cout << "unchecked candidate assertion";
        else if (summary.finalUnbounded)
            std::cout << "k-induction-proved invariant";
        else
            std::cout << "bounded-verified assertion";
        std::cout << (summary.verifiedAssertions == 1 ? "" : "s") << " of "
                  << summary.minedAssertions << " mined in " << summary.rounds
                  << " round" << (summary.rounds == 1 ? "" : "s") << ", "
                  << static_cast<int>(summary.seconds) << "s -> "
                  << summary.outputFile << "\n";
        if (summary.workDirKept)
            std::cout << "workdir kept at " << summary.workDir << "\n";
    } else if (!summary.workDir.empty() && summary.workDirKept) {
        // The only place this is printed: the stages report what went wrong,
        // not where the evidence is.
        std::cerr << "smart: workdir kept at " << summary.workDir << "\n";
    }
    return static_cast<int>(code);
}
