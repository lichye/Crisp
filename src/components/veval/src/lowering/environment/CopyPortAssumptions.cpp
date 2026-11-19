#include "CopyPortAssumptions.h"
#include "lowering/propagation/LandingEligibility.h"
#include "lowering/Symbolic/PropertySubstitution.h"
#include "frontend/rtl/ElaboratedDesign.h"
#include <util/replace_expr.h>

namespace veval::lowering {
std::vector<AssumptionCopy> copyPortAssumptions(const PropertyLanding& row) {
    std::vector<AssumptionCopy> copies;
    if (!row.isAssumption || row.isResetContract || row.monitor.hasLocalVariables ||
        row.result != LoweringCoverage::Full ||
        !row.rebasePrintable || !admitLanding(row, LandingUse::Assumption)) return copies;

    std::map<exprt, std::vector<symbol_exprt>> inputs;
    for (const auto& binding : row.edge->bindings)
        if (!binding.isOutput && binding.kind == BindingKind::Whole &&
            binding.parentExpr.id() == ID_symbol &&
            binding.port.type() == binding.parentExpr.type())
            inputs[binding.parentExpr].push_back(binding.port);

    const auto observations = row.monitor.atomSymbols();
    const std::set<symbol_exprt> observed(observations.begin(), observations.end());
    for (const auto& [parent, ports] : inputs) {
        for (const auto& from : ports) {
            if (!observed.count(from)) continue;
            for (const auto& to : ports) {
                // Keeping already-used ports distinct also makes the inverse
                // substitution unambiguous, including temporal observations.
                if (observed.count(to) || from.type() != to.type()) continue;
                const std::map<irep_idt, exprt> substitution{{from.get_identifier(), to}};
                auto monitor = symbolic::substituteAtoms(row.monitor, substitution);
                if (!symbolic::roundTripsBack(row.monitor, monitor, substitution, {})) continue;
                const auto restored = symbolic::substituteAtoms(monitor, {{to.get_identifier(), from}});
                if (restored.sourceProperty != row.monitor.sourceProperty ||
                    restored.originalProperty != row.monitor.originalProperty) continue;

                auto copy = row;
                copy.monitor = std::move(monitor);
                const replace_mapt replacement{{from, to}};
                for (auto& [name, value] : copy.claimedSubstitution) replace_expr(replacement, value);
                for (auto& rebase : copy.claimedRebases)
                    if (rebase.port == from) rebase.port = to;
                const auto shownFrom = frontend::displayName(id2string(from.get_identifier()));
                const auto shownTo = frontend::displayName(id2string(to.get_identifier()));
                for (auto& fate : copy.fates)
                    if (fate.childPort == shownFrom) fate.childPort = shownTo;
                copies.push_back({from, to, std::move(copy)});
            }
        }
    }
    return copies;
}
}
