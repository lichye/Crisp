// Process setup, command dispatch and the top-level error boundary.
#include "util/cli/Commands.h"
#include "util/cli/ParseArguments.h"
#include "util/cli/CheckCommands.h"
#include "lowering/LoweringStore.h"
#include "util/files/AtomicOutput.h"
#include "util/process/ChildProcess.h"
#include "util/process/ParallelFor.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace veval::cli;

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "-h") == 0 ||
        std::strcmp(argv[1], "--help") == 0) {
        usage();
        return argc < 2 ? 1 : 0;
    }

    try {
        veval::util::atomicOutputCreationMode();
        std::string childError;
        if (!veval::util::ensureWaitableChildren(childError))
            throw std::runtime_error(childError);
        if (std::strcmp(argv[1], "check") == 0) return veval::workflow::main(argc, argv);
        if (std::strcmp(argv[1], "lower") == 0) return veval::workflow::lowerMain(argc, argv);
        if (std::strcmp(argv[1], "__check-source") == 0) return veval::workflow::prepareSources(argc, argv);
        if (std::strcmp(argv[1], "__check-exec") == 0) return veval::workflow::executeOwned(argc, argv);
        if (std::strcmp(argv[1], "__check-command") == 0) return veval::workflow::superviseCommand(argc, argv);
        auto invocation = parseArgs(argc, argv);
        applyConfig(invocation);
        validateInvocation(invocation);
        if (invocation.generationThreads)
            invocation.generationThreads = veval::util::effectiveGenerationThreads(*invocation.generationThreads);
        applyRuntimePolicy(invocation);

        if (invocation.command == "freex") return runUndrivenSignals(invocation);
        if (invocation.command == "sigma") return runConnections(invocation);
        if (invocation.command == "contracts") return runContracts(invocation);
        if (invocation.command == "inspect-sva") return runInspectSva(invocation);
        if (invocation.command == "transport") return runLowering(invocation);
        if (invocation.command == "reach") return runReachability(invocation);

        std::cerr << "veval: unknown command '" << invocation.command << "'\n";
        usage();
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "veval: " << error.what() << '\n';
        return 1;
    }
}
