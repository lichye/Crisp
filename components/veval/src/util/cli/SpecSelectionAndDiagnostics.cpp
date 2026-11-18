#include "SpecSelectionAndDiagnostics.h"
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace veval::cli {

veval::lowering::EnvironmentProfile environmentProfile(
    const CommandLineOptions& invocation) {
    using Mode = veval::lowering::EnvironmentProfileMode;
    veval::lowering::EnvironmentProfile profile;
    if (invocation.environmentProfile == "full") {
        profile.mode = Mode::Full;
    } else if (invocation.environmentProfile == "isolation") {
        profile.mode = Mode::Isolation;
    } else if (invocation.environmentProfile == "allowlist") {
        profile.mode = Mode::Allowlist;
        std::ifstream names(invocation.environmentAllowlist);
        if (!names)
            throw std::runtime_error(
                "--environment-profile allowlist needs a readable "
                "--environment-allowlist FILE");
        std::string line;
        while (std::getline(names, line)) {
            if (line.empty() || line.front() == '#') continue;
            const auto separator = line.find('\t');
            if (separator == std::string::npos)
                throw std::runtime_error(
                    "--environment-allowlist lines must be "
                    "instance<TAB>property");
            profile.assumptions.push_back(
                {line.substr(0, separator), line.substr(separator + 1)});
        }
    } else {
        throw std::runtime_error(
            "--environment-profile must be full, isolation, or allowlist");
    }
    return profile;
}

// Say which properties never became monitors, and why.
//
// A refusal that only happens internally is indistinguishable from a
// property that was never written. The X/Z guard makes this concrete: a
// specification using `===' is silently absent from the results unless the
// refusal is printed, and the author has no way to learn that their property
// was dropped rather than transported.
//
// Written to stderr so it cannot corrupt the stdout the analysis scripts
// parse.
void reportRefusals(const veval::sva::MonitorSet& set) {
    for (const auto& refused : set.unsupported)
        std::cerr << "not converted -- " << refused.property << ": "
                  << refused.reason << "\n";
}

// Refuse to run when an entry claimed the reserved prefix and is an ASSERT.
//
// `reset_contract...' is a label convention, and a convention has no compiler
// behind it. An entry that begins with it and is an ASSERT is swallowed in one
// of two ways depending on which signals it names -- both measured on a
// corpus, both once silent:
//
//   over an output   it is not an obligation (reset contracts are excluded
//                    from products and checking) and not a premise
//                    (its signals are not all inputs), so it disappears
//                    entirely: no verdict, no line in any file, no count.
//   over inputs only it becomes an INITIAL-STATE PREMISE. An assertion the
//                    boundary wrote as an obligation is then granted to the
//                    engine instead of being discharged, which is the one
//                    direction that can make a proof succeed.
//
// RULING 8 (Leiqi, 2026-08-04): this is a HARD ERROR and not a warning. The
// prefix has no legal `assert' use at all, so the false-positive surface is
// zero; the second direction above is the error class that puts a proof
// nobody discharged into a published table; and stderr is a weak channel in a
// run that prints for minutes. The counter-argument on file -- that one
// misspelling then stops a whole campaign -- is answered by where the check
// sits: it runs on the elaborated monitor set, before a single edge is
// transported and before any engine is started, so the run costs an
// elaboration rather than an evening.
//
// The other direction stays a warning and is deliberately not moved here: a
// genuine `assume'-labelled reset contract that feeds no premise at some
// landing (prove/Environment.cpp) is a fact about where it landed, not a
// mislabel, and it has legitimate shapes.
//
// Returns how many offenders were named. The caller stops on non-zero.
std::size_t reportReservedLabels(const veval::sva::MonitorSet& set) {
    std::size_t offenders = 0;
    for (const auto& monitor : set.monitors) {
        if (!monitor.isResetContract || monitor.isAssumption) continue;
        offenders++;
        std::cerr
            << "error: reserved label -- " << monitor.property
            << ": `reset_contract...' is reserved for a reset-contract "
               "environment fact, which must be written `assume property'. "
               "This entry is an `assert', so it is not an obligation of any "
               "module (reset contracts are never proved) and it carries no "
               "semantic class; over inputs alone it would instead be GRANTED "
               "to the engine as an initial-state premise. Rename it, or "
               "write it as an assumption if a reset contract is what was "
               "meant.\n";
    }
    if (offenders != 0)
        std::cerr << "error: " << offenders << " entr"
                  << (offenders == 1 ? "y claims" : "ies claim")
                  << " the reserved `reset_contract' prefix on an `assert'. "
                     "Nothing was transported and nothing was proved (ruling "
                     "8, 2026-08-04).\n";
    return offenders;
}

// The file that DECLARES the boundary's top module.
//
// That file is the specification the properties were written in, and a Tier-2
// entry is only useful if it can say where to go and read the property in
// full. The rule is scripts/table.py's: the netlist declares the design, the
// specification declares the top the tool was pointed at. First match wins --
// the Makefile passes the specification before the netlist -- and no match is
// not an error: the pointer then names the module and the run goes on.
std::string boundarySpecPath(const veval::frontend::ElaborateOptions& options) {
    for (const auto& file : options.files) {
        std::ifstream in(file);
        if (!in) continue;
        std::string line;
        while (std::getline(in, line)) {
            auto at = line.find_first_not_of(" \t");
            if (at == std::string::npos) continue;
            if (line.compare(at, 6, "module") != 0) continue;
            at = line.find_first_not_of(" \t", at + 6);
            if (at == std::string::npos ||
                line.compare(at, options.top.size(), options.top) != 0)
                continue;
            auto after = at + options.top.size();
            if (after < line.size() &&
                (std::isalnum(static_cast<unsigned char>(line[after])) ||
                 line[after] == '_' || line[after] == '$'))
                continue;
            return file;
        }
    }
    return {};
}


} // namespace veval::cli
