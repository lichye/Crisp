#include "Commands.h"
#include "ParseArguments.h"
#include "frontend/sva/PropertyMonitor.h"
#include "frontend/input/ReplaceDontCareConstants.h"
#include "lowering/connectivity/SignalReachability.h"
#include <ebmc/transition_system.h>
#include <util/format_expr.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <set>

namespace veval::cli {

// Contracts the DESIGN states about itself, as opposed to intent carried down.
//
// A tied-off port is a fact sitting in sigma: `.a(4'b0101)' says that child's
// input is that constant, always. Verified on its own, the child's inputs are
// free and a great many of its own properties are then unprovable; handing it
// this one line often makes them provable. It costs nothing to extract -- the
// binding is already classified.
//
// It is NOT broadcast intent, and the paper must never merge the two. An
// intent carried down says something the module's own source does not contain
// -- that is what the entailment experiment measured, 109 not entailed. A
// tie-off is by construction entailed by the design: it is written in this
// module's own instantiation context. Useful engineering, different claim,
// separate column.
int runContracts(const CommandLineOptions& invocation) {
    auto design =
        veval::frontend::ElaboratedDesign::elaborate(invocation.options);
    auto edges = veval::lowering::connectionMap(design.transitionSystem());

    std::size_t emitted = 0, instances = 0;
    for (const auto& edge : edges) {
        std::size_t here = 0;
        for (const auto& binding : edge.bindings) {
            if (binding.kind != veval::lowering::BindingKind::Constant)
                continue;
            if (binding.isOutput) continue;
            // Only real input ports: a child's own `assign w = k' also
            // arrives as a constant binding of `w'.
            const auto* port = design.transitionSystem().symbol_table.lookup(
                binding.port.get_identifier());
            if (port == nullptr || !port->is_input) continue;
            if (here == 0) {
                std::cout << "\n// " << edge.instance << " : " << edge.module
                          << "\n";
                instances++;
            }
            // Named as the module sees it, and as an immediate assumption: a
            // tie-off holds in every cycle whatever the module's clock is.
            auto name = veval::frontend::displayName(
                id2string(binding.port.get_identifier()));
            const auto prefix = edge.instance + '.';
            if (name.compare(0, prefix.size(), prefix) == 0) name = name.substr(prefix.size());
            std::cout << "always_comb sigma_tie_" << emitted << ": assume ("
                      << name << " == " << format(binding.parentExpr) << ");\n";
            here++;
            emitted++;
        }
    }
    std::cerr << emitted << " tie-off contract(s) over " << instances
              << " instance(s)\n";
    return 0;
}

int runConnections(const CommandLineOptions& invocation) {
    auto design =
        veval::frontend::ElaboratedDesign::elaborate(invocation.options);

    // `--show-trans' prints the transition relation instead of sigma.
    //
    // Sigma is read from invar(), and a register is not there: `always_ff
    // q <= d' appears in trans() as `next(q) == d' and nowhere in invar(),
    // which is why an observation naming d is reported as unrelated to a
    // child whose port is bound to q. Attribution has to read trans() to say
    // otherwise, and the shape it must read had never been looked at. This
    // is that window -- small, and worth keeping: the next person to wonder
    // what the transition relation looks like should not have to write a
    // program.
    if (invocation.showTrans) {
        std::cout << format(
                         design.transitionSystem().trans_expr.trans())
                  << "\n";
        return 0;
    }
    auto edges = veval::lowering::connectionMap(design.transitionSystem());

    if (invocation.json)
        veval::lowering::writeJson(edges, std::cout);
    else
        veval::lowering::writeText(edges, std::cout);

    return 0;
}

int runInspectSva(const CommandLineOptions& invocation) {
    auto design =
        veval::frontend::ElaboratedDesign::elaborate(invocation.options);
    auto set = veval::sva::filter(
        veval::sva::monitors(design.transitionSystem(),
                                   design.messageHandler()),
        invocation.propertyFilters);

    if (invocation.json)
        veval::sva::writeJson(set, std::cout);
    else
        veval::sva::writeText(set, std::cout);

    return set.monitors.empty() && !set.unsupported.empty() ? 1 : 0;
}

int runReachability(const CommandLineOptions& invocation) {
    auto design =
        veval::frontend::ElaboratedDesign::elaborate(invocation.options);
    auto edges = veval::lowering::connectionMap(design.transitionSystem());
    auto aliases = veval::lowering::signalAliases(design.transitionSystem());
    // Every candidate boundary down to a given depth, in one elaboration.
    //
    // This is the measurement the coverage question turns on. A chip boundary
    // reaches only the modules its ports actually touch; everything else gets
    // the clock and the reset and nothing else, which no property can be
    // built from. Whether a design is coverable at all is therefore a
    // question about a SET of boundaries, and answering it by hand is a
    // separate elaboration per candidate.
    if (invocation.boundaryDepth != 0) {
        auto top = veval::frontend::displayName(design.topInstance());

        std::vector<std::string> candidates{top};
        for (const auto& edge : edges) {
            if (edge.depth <= invocation.boundaryDepth)
                candidates.push_back(edge.instance);
        }

        // A module is only worth counting when enough of the boundary
        // arrives to write something. Two signals is the clock and the
        // reset.
        const std::size_t useful = invocation.minSignals;

        std::cout << "boundaries down to depth " << invocation.boundaryDepth
                  << ", counting a module covered when at least " << useful
                  << " boundary signals arrive\n\n";
        std::cout << "  MODULES  PORTS  BOUNDARY\n";

        std::vector<std::pair<std::string, std::set<std::string>>> coverage;

        for (const auto& candidate : candidates) {
            auto one = veval::lowering::reach(design.transitionSystem(),
                                               candidate, edges, aliases,
                                               invocation.internalBoundary);

            std::set<std::string> modules;
            for (const auto& reached : one.reached) {
                if (reached.signals < useful) continue;
                modules.insert(
                    reached.module.substr(0, reached.module.find('$')));
            }

            if (modules.empty()) continue;

            std::cout << "  " << modules.size() << "	   "
                      << one.boundarySignals << "	  " << candidate << "\n";
            coverage.emplace_back(candidate, std::move(modules));
        }

        // Greedy set cover: the marginal value of each further boundary is
        // what a reader wants, not the total.
        std::set<std::string> union_;
        std::cout << "\ngreedy cover -- each line is the next boundary that "
                     "adds the most:\n";
        std::cout << "  TOTAL  ADDED  BOUNDARY\n";

        while (true) {
            const std::pair<std::string, std::set<std::string>>* best = nullptr;
            std::size_t bestGain = 0;

            for (const auto& entry : coverage) {
                std::size_t gain = 0;
                for (const auto& module : entry.second)
                    if (union_.count(module) == 0) gain++;
                if (gain > bestGain) bestGain = gain, best = &entry;
            }

            if (best == nullptr) break;
            union_.insert(best->second.begin(), best->second.end());
            std::cout << "  " << union_.size() << "	  +" << bestGain << "	  "
                      << best->first << "\n";
        }

        std::cout << "\n" << union_.size()
                  << " module(s) coverable from these boundaries together\n";
        return 0;
    }

    // Default boundary is the top module; --boundary names any instance.
    auto boundary = invocation.boundary.empty()
                        ? veval::frontend::displayName(design.topInstance())
                        : invocation.boundary;

    auto result =
        veval::lowering::reach(design.transitionSystem(), boundary, edges,
                                aliases, invocation.internalBoundary);

    if (!invocation.reachTo.empty())
        veval::lowering::writeTo(result, invocation.reachTo, std::cout);
    else if (invocation.json)
        veval::lowering::writeJson(result, std::cout);
    else
        veval::lowering::writeText(result, std::cout);

    return 0;
}

// Explicitly rewrite netlist don't-cares into free values at the caller's
// chosen output path. Neither lowering nor checking calls this automatically.
int runUndrivenSignals(const CommandLineOptions& invocation) {
    if (invocation.options.files.size() != 1) {
        std::cerr << "freex: expected exactly one netlist\n";
        return 2;
    }
    std::ifstream in(invocation.options.files.front());
    if (!in) {
        std::cerr << "freex: cannot read " << invocation.options.files.front()
                  << "\n";
        return 2;
    }
    if (invocation.outPath.empty()) {
        std::cerr << "freex: --out PATH is required\n";
        return 2;
    }
    veval::frontend::FreeDontCareResult result;
    {
        std::ofstream out(invocation.outPath);
        try {
            result = veval::frontend::freeDontCares(in, out);
        } catch (const std::exception& e) {
            // A don't-care left in place would be read as 0; no output
            // is better than a model that looks freed and is not.
            out.close();
            std::remove(invocation.outPath.c_str());
            std::cerr << e.what() << "\n";
            return 2;
        }
    }
    std::cerr << result.literals << " don't-care literal(s) -> "
              << result.wires << " free wire(s) in " << result.modules
              << " module(s)\n";

    // The generated names, for a completion-cone check to treat as tainted.
    // Written to a companion file rather than stdout so that neither output
    // can be mistaken for the other.
    if (!invocation.freeXList.empty()) {
        std::ofstream list(invocation.freeXList);
        for (const auto& name : result.names) list << name << "\n";
        std::cerr << "wrote " << result.names.size() << " name(s) to "
                  << invocation.freeXList << "\n";
    }
    return 0;
}


} // namespace veval::cli
