#include "vacuity.hpp"

#include <algorithm>
#include <stdexcept>

#include <ebmc/netlist.h>
#include <solvers/sat/satcheck.h>
#include <trans-netlist/trans_trace_netlist.h>
#include <trans-netlist/unwind_netlist.h>
#include <trans-word-level/lasso.h>
#include <trans-word-level/trans_trace_word_level.h>
#include <trans-word-level/unwind.h>

namespace crisp::check {
namespace {
using Property = ebmc_propertiest::propertyt;
using Batch = std::vector<Property*>;

Batch active(ebmc_propertiest& assertions) {
    Batch batch;
    for (auto& p : assertions.properties)
        if (!p.is_disabled() && !p.is_assumption() && !p.is_failure()) batch.push_back(&p);
    return batch;
}

void word_checks(const cmdlinet& cmd, const transition_systemt& system, ebmc_propertiest& assertions,
                 Probes& probes, std::size_t bound, const AfterAssertions& after, const ExactResult& resolved,
                 message_handlert& messages) {
    const namespacet ns(system.symbol_table);
    const auto frames = bound + 1;
    auto container = ebmc_solver_factory(cmd)(ns, messages);
    auto& solver = container.decision_procedure();
    unwind(system.trans_expr, messages, solver, frames, ns);

    bool lasso = false;
    auto encode = [&](Property& p, bool probe) {
        if (p.is_failure()) return;
        if (!bmc_supports_property(p.normalized_expr)) {
            p.failure("property not supported by BMC engine");
            return;
        }
        const auto obligations = property(p.normalized_expr, !p.is_exists_path(), messages, frames);
        lasso |= uses_lasso_symbol(obligations);
        for (const auto& op : obligations)
            if (!probe || !op.is_true()) p.timeframe_handles.push_back(solver.handle(op));
        if (p.is_assumption()) solver.set_to_true(conjunction(p.timeframe_handles));
    };
    for (auto& p : assertions.properties)
        if (!p.is_disabled()) encode(p, false);
    // Freeze potential cover handles before the first SAT call: incremental
    // bit-vector conversion may eliminate unreferenced variables afterwards.
    for (auto& entry : probes) encode(entry.second, true);
    if (lasso) lasso_constraints(solver, frames, ns, system.main_symbol->name);

    // Like EBMC's batched BMC, one model can refute several properties. The
    // disjunction is a solve assumption: no assertion or prior batch's UNSAT
    // blocking clause becomes a permanent constraint on the next query.
    auto solve = [&](Batch batch, bool write_traces) {
        while (!batch.empty()) {
            exprt::operandst violations;
            for (const auto p : batch)
                for (const auto& handle : p->timeframe_handles) violations.push_back(not_exprt{handle});
            const auto result = solver(disjunction(violations));
            if (result != decision_proceduret::resultt::D_SATISFIABLE) {
                for (auto p : batch) {
                    if (result == decision_proceduret::resultt::D_UNSATISFIABLE) p->proved_with_bound(bound);
                    else p->failure("BMC solver error");
                    if (!write_traces) resolved(*p);
                }
                break;
            }
            const auto previous = batch.size();
            batch.erase(std::remove_if(batch.begin(), batch.end(), [&](Property* p) {
                for (const auto& handle : p->timeframe_handles) {
                    if (!solver.get(handle).is_false()) continue;
                    p->refuted();
                    if (write_traces)
                        p->witness_trace = compute_trans_trace(p->timeframe_handles, solver, frames, ns,
                                                               system.main_symbol->name);
                    else resolved(*p);
                    return true;
                }
                return false;
            }), batch.end());
            if (batch.size() == previous) throw std::runtime_error("BMC model did not identify a violated property");
        }
    };

    solve(active(assertions), true);
    // Check narrower matching windows first. This keeps a long SAT search for
    // a single-sample predicate from delaying an already decidable sequence.
    // Each group shares the original unwind and reports every answer as it arrives.
    std::map<std::size_t, Batch> windows;
    for (const auto& id : after(assertions)) {
        auto& probe = probes.at(id);
        if (probe.is_failure()) resolved(probe);
        else windows[probe.timeframe_handles.size()].push_back(&probe);
    }
    for (auto& entry : windows) solve(std::move(entry.second), false);
}

void bit_checks(const cmdlinet& cmd, transition_systemt& system, ebmc_propertiest& assertions,
                Probes& probes, std::size_t bound, const AfterAssertions& after, const ExactResult& resolved,
                message_handlert& messages) {
    auto encoded = assertions;
    for (const auto& entry : probes) encoded.properties.push_back(entry.second);
    const auto netlist = make_netlist(system, encoded, cmd, messages);
    satcheck_no_simplifiert solver(messages);
    const bmc_mapt frames(netlist, bound + 1, solver);
    messaget message(messages);
    unwind(netlist, frames, message, solver);
    const namespacet ns(system.symbol_table);
    // make_netlist instruments $past in the combined batch.
    auto item = encoded.properties.begin();
    auto encode = [&](Property& p) {
        p.normalized_expr = (item++)->normalized_expr;
        if (p.is_disabled() || p.is_failure()) return;
        const auto found = netlist.properties.find(p.identifier);
        if (!netlist_bmc_supports_property(p.normalized_expr) ||
            found == netlist.properties.end() || !found->second) {
            p.failure("property not supported by netlist BMC engine");
            return;
        }
        p.timeframe_literals = unwind_property(*found->second, frames);
        for (const auto lit : p.timeframe_literals) {
            if (p.is_assumption()) solver.l_set_to(lit, true);
            else solver.set_frozen(lit);
        }
    };
    for (auto& p : assertions.properties) encode(p);
    for (auto& entry : probes) encode(entry.second);

    auto solve = [&](Property& p, bool write_trace) {
        if (p.is_failure()) return;
        switch (solver.prop_solve({!solver.land(p.timeframe_literals)})) {
        case propt::resultt::P_SATISFIABLE:
            p.refuted();
            if (write_trace) p.witness_trace = compute_trans_trace(p.timeframe_literals, frames, solver, ns);
            break;
        case propt::resultt::P_UNSATISFIABLE: p.proved_with_bound(bound); break;
        case propt::resultt::P_ERROR: p.failure("netlist BMC solver error");
        }
    };
    for (auto p : active(assertions)) solve(*p, true);
    for (const auto& id : after(assertions)) {
        auto& probe = probes.at(id);
        solve(probe, false);
        resolved(probe);
    }
}
} // namespace

void bounded_checks(const cmdlinet& cmd, transition_systemt& system, ebmc_propertiest& assertions,
                    Probes& probes, std::size_t bound, const AfterAssertions& after, const ExactResult& resolved,
                    message_handlert& messages) {
    if (cmd.isset("aig")) bit_checks(cmd, system, assertions, probes, bound, after, resolved, messages);
    else word_checks(cmd, system, assertions, probes, bound, after, resolved, messages);
}
} // namespace crisp::check
