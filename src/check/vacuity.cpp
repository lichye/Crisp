#include "vacuity.hpp"

#include <cstdio>

#include <ebmc/ebmc_error.h>
#include <ebmc/ebmc_solver_factory.h>
#include <ebmc/instrument_buechi.h>
#include <ebmc/instrument_past.h>
#include <ebmc/property_checker.h>
#include <temporal-logic/normalize_property.h>
#include <temporal-logic/rewrite_sva_sequence.h>
#include <trans-word-level/lasso.h>
#include <trans-word-level/unwind.h>
#include <util/exception_utils.h>
#include <util/simplify_expr.h>
#include <verilog/sva_expr.h>
#include <verilog/verilog_types.h>

namespace crisp::check {
namespace {
using Property = ebmc_propertiest::propertyt;

transition_systemt fresh_model(const transition_systemt& system) {
    auto model = system;
    model.main_symbol = &model.symbol_table.lookup_ref(system.main_symbol->name);
    return model;
}

const exprt* implication(const exprt& original, exprt::operandst& disabled) {
    const exprt* body = &original;
    // EBMC has already elaborated named properties and sampled clocks. A
    // Boolean -> also has implicit sequence-to-property coercions around it.
    for (;;) {
        const auto id = body->id();
        if (id == ID_sva_disable_iff) {
            disabled.push_back(to_binary_expr(*body).lhs());
            body = &to_binary_expr(*body).rhs();
        } else if (id == ID_sva_always || id == ID_sva_implicit_weak ||
                   id == ID_sva_implicit_strong || id == ID_sva_boolean) {
            body = &to_unary_expr(*body).op();
        } else break;
    }
    if (body->id() != ID_implies && body->id() != ID_sva_overlapped_implication &&
        body->id() != ID_sva_non_overlapped_implication)
        return nullptr;
    return body;
}

// The dual safety property is violated exactly when an enabled match
// completes. Keep disable conditions at the starting sample (spec 21 §1).
exprt unreachable(const exprt& implication, exprt::operandst disabled, const namespacet& ns, bool buechi) {
    auto lhs = to_binary_expr(implication).lhs();
    if (lhs.id() == ID_sva_boolean) lhs = exprt{to_sva_boolean_expr(lhs).op()};
    lhs = simplify_expr(std::move(lhs), ns);
    if (lhs.is_false()) return true_exprt{};
    // Negating a strong sequence only admits completed matches in EBMC,
    // including one that ends on the last sample of the bound.
    if (lhs.type().id() == ID_verilog_sva_sequence) {
        if (implication.id() == ID_sva_non_overlapped_implication && admits_empty(lhs))
            lhs = true_exprt{};
        else if (buechi && disabled.empty()) {
            // EBMC lowers assertion-local captures on implication nodes. A
            // completed nonempty match is exactly a violation of A |-> false.
            return normalize_property(
                sva_always_exprt{sva_overlapped_implication_exprt{std::move(lhs), false_exprt{}}});
        }
        else
            lhs = normalize_property(sva_strong_exprt{ID_sva_strong, std::move(lhs)});
    }
    disabled.push_back(not_exprt{std::move(lhs)});
    return normalize_property(sva_always_exprt{disjunction(disabled)});
}

ebmc_propertiest assumptions_of(const ebmc_propertiest& batch) {
    ebmc_propertiest assumptions;
    for (const auto& p : batch.properties)
        if (p.is_assumption()) assumptions.properties.push_back(p);
    return assumptions;
}

void find_shallow_matches(cmdlinet smt, const transition_systemt& system, const ebmc_propertiest& batch,
                          const Probes& probes, const Targets& pending, message_handlert& messages,
                          const Witness& hit) {
    constexpr std::size_t frames = 5; // depth 4, including the initial frame
    auto model = fresh_model(system);
    auto assumptions = assumptions_of(batch);
    instrument_buechi(model, assumptions, true, messages);
    instrument_past(model, assumptions);
    if (has_past(model, assumptions)) return;
    const namespacet ns(model.symbol_table);
    smt.set("yices");
    auto container = ebmc_solver_factory(smt)(ns, messages);
    auto& solver = container.decision_procedure();
    unwind(model.trans_expr, messages, solver, frames, ns);
    for (const auto& p : assumptions.properties) {
        const auto& expr = p.normalized_expr;
        if (!p.is_assumed() || !bmc_supports_property(expr) ||
            (expr.id() != ID_AG && expr.id() != ID_G && expr.id() != ID_sva_always) ||
            has_temporal_operator(to_unary_expr(expr).op())) return;
        solver.set_to_true(conjunction(property(expr, true, messages, frames)));
    }

    for (const auto& id : pending) {
        const auto& probe = probes.at(id);
        if (!bmc_supports_property(probe.normalized_expr)) continue;
        const auto obligations = property(probe.normalized_expr, false, messages, frames);
        if (uses_lasso_symbol(obligations)) continue;
        const auto result = solver(not_exprt{conjunction(obligations)});
        if (result == decision_proceduret::resultt::D_ERROR)
            throw std::runtime_error("BMC witness solver error");
        if (result == decision_proceduret::resultt::D_SATISFIABLE) hit(probe.number);
    }
}

struct Antecedents {
    Probes probes;
    Targets constants;
    std::map<std::size_t, irep_idt> ids;
};

Antecedents prepare_antecedents(const transition_systemt& system, const ebmc_propertiest& batch,
                               bool buechi, const Targets& selected) {
    const namespacet ns(system.symbol_table);
    Antecedents out;
    for (const auto& p : batch.properties) {
        if (p.is_disabled() || p.is_assumption() || !selected.count(p.identifier)) continue;
        exprt::operandst disabled;
        const auto body = implication(p.original_expr, disabled);
        if (!body) continue;
        auto probe = p;
        try {
            probe.original_expr = unreachable(*body, std::move(disabled), ns, buechi);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (...) {
            continue;
        }
        if (probe.original_expr.is_true()) out.constants.insert(p.identifier);
        else {
            probe.identifier = "crisp::antecedent::" + id2string(p.identifier);
            probe.normalized_expr = probe.original_expr;
            out.ids.emplace(p.number, p.identifier);
            out.probes.emplace(p.identifier, std::move(probe));
        }
    }
    return out;
}

property_checker_resultt check(const cmdlinet& cmd, transition_systemt& model, ebmc_propertiest& queries,
                               front::Mode mode, message_handlert& messages,
                               const witness_lasso_observert& observer = {}) {
    if (mode == front::Mode::pdr || mode == front::Mode::kind)
        instrument_buechi(model, queries, mode == front::Mode::pdr, messages);
    if (cmd.isset("finite-prefix") && (mode == front::Mode::pdr || mode == front::Mode::kind)) {
        // Keep the same temporal assumption lowering that the strict backend
        // performs before solving. Only auxiliary witness certification is skipped.
        try {
            instrument_buechi(model, queries, true, messages, true);
        } catch (const ebmc_errort& error) {
            messaget{messages}.warning() << error.what() << messaget::eom;
        }
    }
    auto result = property_checker(cmd, model, queries, messages, observer);
    if (result.status != property_checker_resultt::statust::VERIFICATION_RESULT)
        throw std::runtime_error("EBMC did not return verification results");
    return result;
}

void report_assertions(const transition_systemt& model, const ebmc_propertiest& queries,
                       ebmc_propertiest::propertiest& results, const ReportOriginal& report) {
    // An unsupported assumption invalidates the entire batch.
    for (auto& p : results) if (p.is_assumption()) report(model, queries, p);
    for (auto& p : results) if (!p.is_assumption() && !p.is_disabled()) report(model, queries, p);
}

std::string bounded_miss(std::size_t bound) {
    return "not triggered within " + std::to_string(bound);
}

std::string failure_reason() {
    try { throw; }
    catch (const ebmc_errort& e) { return e.what(); }
    catch (const cprover_exception_baset& e) { return e.what(); }
    catch (const invariant_failedt& e) { return e.what(); }
    catch (const std::bad_alloc&) { return "out of memory"; }
    catch (const std::exception& e) { return e.what(); }
    catch (const std::string& e) { return e; }
    catch (const char* e) { return e; }
    catch (...) { return "engine failure"; }
}

} // namespace

std::optional<exprt> local_implication(exprt original) {
    exprt::operandst disabled;
    const auto body = implication(original, disabled);
    if (!body) return {};
    if (body->id() == ID_implies) {
        // veval's printer spells ID_implies as the property keyword "implies".
        // Use the one-cycle sequence equivalent for an authored Boolean ->.
        const auto& arrow = to_binary_expr(*body);
        auto printed = sva_overlapped_implication_exprt{
            sva_boolean_exprt{arrow.lhs(), verilog_sva_sequence_typet{}}, arrow.rhs()};
        auto* head = &original;
        while (head->id() == ID_sva_always || head->id() == ID_sva_disable_iff)
            head = &head->operands().back();
        *head = std::move(printed); // replaces implicit Boolean-to-sequence coercions as well
    }
    // veval carries the abort outside its implicit sampling always.
    if (original.id() == ID_sva_always && to_unary_expr(original).op().id() == ID_sva_disable_iff) {
        const auto& guarded = to_sva_disable_iff_expr(to_unary_expr(original).op());
        return sva_disable_iff_exprt{guarded.condition(), sva_always_exprt{guarded.property()}};
    }
    return original;
}

bool has_antecedent(const exprt& original) {
    exprt::operandst disabled;
    return implication(original, disabled) != nullptr;
}

void check_assertions(const cmdlinet& cmd, const transition_systemt& system, const ebmc_propertiest& batch,
                      const front::Task& task, message_handlert& messages, const ReportOriginal& report,
                      const witness_lasso_observert& observe_lasso) {
    auto model = fresh_model(system);
    auto queries = batch;
    auto result = check(cmd, model, queries, task.mode.value, messages, observe_lasso);
    report_assertions(model, queries, result.properties, report);
}

void check_bounded_assertions(const cmdlinet& cmd, transition_systemt& system, ebmc_propertiest& batch,
                              const front::Task& task, message_handlert& messages, const ReportOriginal& report,
                              const std::function<void()>& complete, const ReportVacuity& publish,
                              const witness_lasso_observert& observe_lasso) {
    Targets selected;
    for (const auto& p : batch.properties)
        if (!p.is_disabled() && !p.is_assumption()) selected.insert(p.identifier);
    auto antecedents = prepare_antecedents(system, batch, false, selected);
    const auto bound = task.depth->value;
    Targets passed;
    const auto record = [&](const transition_systemt& model, const ebmc_propertiest& queries, Property& p) {
        report(model, queries, p);
        if (p.is_proved() || p.is_proved_with_bound()) passed.insert(p.identifier);
    };
    const auto record_constants = [&] {
        complete();
        for (const auto& id : antecedents.constants)
            if (passed.count(id)) publish(id, "never triggered");
    };
    if (antecedents.probes.empty()) {
        check_assertions(cmd, system, batch, task, messages, record, observe_lasso);
        record_constants();
        return;
    }
    bounded_checks(cmd, system, batch, antecedents.probes, bound, [&](ebmc_propertiest& checked) {
        // The original counterexamples use the original-goal batch and the
        // same strict EBMC certification as standalone checks.
        property_checker_resultt originals{checked};
        certify_property_witnesses(cmd, system, originals, bound, messages, observe_lasso);
        report_assertions(system, checked, originals.properties, record);
        record_constants();
        Targets pending;
        for (const auto& id : passed)
            if (antecedents.probes.count(id)) pending.insert(id);
        return pending;
    }, [&](const Property& probe) {
        const auto& id = antecedents.ids.at(probe.number);
        if (refuted(probe)) publish(id, "triggered");
        else if (probe.is_proved_with_bound()) publish(id, bounded_miss(bound));
    }, messages);
}

void check_antecedents(const cmdlinet& cmd, const transition_systemt& system, const ebmc_propertiest& batch,
                       const Targets& selected, const front::Task& task, Deadline deadline,
                       message_handlert& messages, const ReportVacuity& publish) {
    if (selected.empty() || std::chrono::steady_clock::now() >= deadline) return;
    const auto mode = task.mode.value;
    auto antecedents = prepare_antecedents(system, batch, mode == front::Mode::pdr || mode == front::Mode::kind, selected);
    for (const auto& id : antecedents.constants) publish(id, "never triggered");
    Targets pending;
    for (const auto& entry : antecedents.probes) pending.insert(entry.first);
    if (pending.empty()) return;
    if (mode == front::Mode::bmc) {
        // Veval landings run their primary engine first and check auxiliary
        // covers only after the remaining primary work in the task.
        auto model = fresh_model(system);
        auto assumptions = assumptions_of(batch);
        const auto bound = task.depth->value;
        bounded_checks(cmd, model, assumptions, antecedents.probes, bound, [&](ebmc_propertiest& checked) {
            for (const auto& p : checked.properties)
                if (!p.is_assumed()) throw std::runtime_error(p.status_as_string());
            return pending;
        }, [&](const Property& probe) {
            const auto& id = antecedents.ids.at(probe.number);
            if (refuted(probe)) publish(id, "triggered");
            else if (probe.is_proved_with_bound()) publish(id, bounded_miss(bound));
        }, messages);
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto shallow_deadline = std::min(now + std::chrono::seconds(30), now + (deadline - now) / 4);
    try {
        run_shallow_bmc(shallow_deadline, [&](const Witness& hit) {
            find_shallow_matches(cmd, system, batch, antecedents.probes, pending, messages, hit);
        }, [&](std::size_t number) {
            const auto& id = antecedents.ids.at(number);
            publish(id, "triggered");
            pending.erase(id);
        });
    } catch (...) {
        std::fprintf(stderr, "shallow BMC: %s\n", failure_reason().c_str());
    }
    if (pending.empty() || std::chrono::steady_clock::now() >= deadline) return;
    auto model = fresh_model(system);
    auto queries = assumptions_of(batch);
    for (const auto& id : pending) {
        const auto& probe = antecedents.probes.at(id);
        auto symbol = model.symbol_table.lookup_ref(id);
        symbol.name = probe.identifier;
        symbol.value = probe.original_expr;
        if (model.symbol_table.add(std::move(symbol)))
            throw std::runtime_error("duplicate antecedent symbol: " + id2string(probe.identifier));
        queries.properties.push_back(probe);
    }
    auto auxiliary = cmd;
    auxiliary.set("finite-prefix");
    auto exact = check(auxiliary, model, queries, mode, messages);
    for (const auto& p : exact.properties)
        if (p.is_assumption() && !p.is_assumed()) throw std::runtime_error(p.status_as_string());
    for (const auto& p : exact.properties) {
        if (p.is_assumption()) continue;
        const auto& id = antecedents.ids.at(p.number);
        if (!pending.count(id) || p.identifier != antecedents.probes.at(id).identifier)
            throw std::runtime_error("unexpected EBMC antecedent result");
        if (refuted(p)) publish(id, "triggered");
        else if (p.is_proved()) publish(id, "never triggered");
    }
}
} // namespace crisp::check
