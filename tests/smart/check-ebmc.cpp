#include "Ebmc.h"
#include "Trace.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>

int main(int argc, char** argv)
{
    using namespace smart::helper;
    if (argc != 3) return 2;
    const std::string design = argv[1];
    const std::string trace = argv[2];
    EbmcOptions options;
    options.topModule = "scope_probe";
    options.unbounded = false;
    options.bound = 5;

    try {
        const auto narrow = checkProperty(design, options, "candidate", trace, "scope_probe.narrow.u");
        if (narrow.status != EbmcStatus::Refuted || !narrow.counterexample.empty() ||
            std::filesystem::exists(trace))
            throw std::runtime_error("another instance's counterexample escaped into refinement");

        const auto wide = checkProperty(design, options, "candidate", trace, "scope_probe.wide.u");
        if (wide.status != EbmcStatus::Refuted || wide.counterexample != trace ||
            std::filesystem::file_size(trace) == 0)
            throw std::runtime_error("missing the counterexample for the requested instance");

        // Both leaves are called u in the VCD. Read each one's own signal.
        Trace bad(SMT, trace, "", "scope_probe.wide.u");
        Trace good(SMT, trace, "", "scope_probe.narrow.u");
        std::unique_ptr<std::vector<Signal>> badSignals(bad.getAllSignals("scope_probe.wide.u"));
        std::unique_ptr<std::vector<Signal>> goodSignals(good.getAllSignals("scope_probe.narrow.u"));
        if (badSignals->size() != 1 || goodSignals->size() != 1 ||
            bad.getSignalValue(badSignals->front())->front()->toString() != "true" ||
            good.getSignalValue(goodSignals->front())->front()->toString() != "false")
            throw std::runtime_error("trace reader did not separate identically named instances");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
