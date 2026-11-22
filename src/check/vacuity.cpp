#include "vacuity.hpp"

#include <cstdio>

#include <ebmc/ebmc_error.h>
#include <ebmc/ebmc_solver_factory.h>
#include <ebmc/instrument_buechi.h>
#include <ebmc/instrument_past.h>
#include <ebmc/property_checker.h>
#include <temporal-logic/normalize_property.h>
#include <temporal-logic/rewrite_sva_sequence.h>
#include <trans-word-level/instantiate_word_level.h>
#include <trans-word-level/lasso.h>
#include <trans-word-level/next_symbol.h>
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

// unwind includes an outgoing transition for the last sample. Close that
// transition onto any sampled state. Join every next-state reference (also
// monitor, history and reset state); combinational inputs on the loop repeat
// their already checked values. Only the initial prefix needs to satisfy init.
exprt loop_closure(const transition_systemt& model, std::size_t frames) {
    std::set<symbol_exprt> carried;
    auto collect = [&](const exprt& e) {
        if (e.id() == ID_next_symbol)
            carried.emplace(to_next_symbol_expr(e).identifier(), e.type());
    };
    model.trans_expr.trans().visit_pre(collect);
    model.trans_expr.invar().visit_pre(collect);
    exprt::operandst choices;
    for (std::size_t entry = 0; entry < frames; ++entry) {
        exprt::operandst equalities;
        for (const auto& v : carried)
            equalities.push_back(equal_exprt{timeframe_symbol(frames, v), timeframe_symbol(entry, v)});
        choices.push_back(conjunction(equalities));
    }
    return disjunction(choices);
}

// Encode the complete match inside the prefix. Extra samples may discharge
// future obligations and close a loop, but cannot supply a later match.
void certified_witnesses(cmdlinet smt, const transition_systemt& system, const ebmc_propertiest& batch,
                         const Probes& probes, const Targets& pending, std::size_t match_frames,
                         const trans_tracet* prefix,
                         message_handlert& messages, const Witness& hit) {
    // Allow two suffix samples on the cheap depth-4 search. For an engine
    // counterexample allow at least 16, or another prefix length for long traces.
    const auto suffix = prefix ? std::max<std::size_t>(16, match_frames) : 2;
    // A strict backend trace can already include a longer certified loop.
    // Recheck at its full length on the original model and assumption set.
    const auto frames = prefix && prefix->loop_start ? prefix->states.size() : match_frames + suffix;
    auto model = fresh_model(system);
    ebmc_propertiest assumptions;
    for (const auto& p : batch.properties)
        if (p.is_assumption()) assumptions.properties.push_back(p);
    // Every certificate must encode all the original assumptions.
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
    solver.set_to_true(loop_closure(model, frames));
    exprt::operandst fixed;
    if (prefix) {
        // Preserve every concrete assignment in the engine's failing prefix
        // that belongs to the original RTL. The two independently elaborated
        // monitor copies can use different auxiliary symbols.
        for (std::size_t t = 0; t < match_frames; ++t) {
            for (const auto& a : prefix->states.at(t).assignments) {
                if (a.rhs.is_nil() || !system.symbol_table.has_symbol(to_symbol_expr(a.lhs).identifier()))
                    continue;
                fixed.push_back(equal_exprt{instantiate(a.lhs, t, frames), a.rhs});
            }
        }
    }

    auto solve = [&](const exprt& query) {
        const auto result = solver(query);
        if (result == decision_proceduret::resultt::D_ERROR)
            throw std::runtime_error("BMC witness solver error");
        return result == decision_proceduret::resultt::D_SATISFIABLE;
    };
    for (const auto& id : pending) {
        const auto& probe = probes.at(id);
        if (!bmc_supports_property(probe.normalized_expr)) continue;
        const auto obligations = property(probe.normalized_expr, false, messages, match_frames);
        if (uses_lasso_symbol(obligations)) continue;
        const auto match = not_exprt{conjunction(obligations)};
        // An engine may choose unrelated input values which make its prefix
        // a dead end. Only a separate complete matching lasso can replace it.
        if ((!fixed.empty() && solve(and_exprt{match, conjunction(fixed)})) || solve(match))
            hit(probe.number);
    }
}

// Consult only the result for the exact auxiliary property checked on this
// batch's full model. An exact BDD totality proof need not carry a trace.
bool has_infinite_witness(const Property& probe) {
    using Certificate = Property::witness_certificatet;
    if (probe.witness_certificate == Certificate::TOTAL_BDD) return true;
    const auto& trace = probe.witness_trace;
    if (!trace || !trace->get_min_failing_timeframe()) return false;
    return (probe.witness_certificate == Certificate::LASSO && trace->loop_start.has_value()) ||
           (probe.witness_certificate == Certificate::TOTAL_PREFIX && trace->total_extension);
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

void check_antecedents(const cmdlinet& cmd, transition_systemt& system, ebmc_propertiest& batch,
                       const front::Task& task, Deadline deadline, message_handlert& messages,
                       const ReportOriginal& report, const ReportVacuity& publish,
                       const witness_lasso_observert& observe_lasso) {
    const namespacet ns(system.symbol_table);
    const bool bounded = task.mode.value == front::Mode::bmc;
    const bool pdr = task.mode.value == front::Mode::pdr;
    const bool buechi = pdr || task.mode.value == front::Mode::kind;
    const std::string original = bounded ? "passed" : "proved";
    const auto detail = bounded ? "bmc depth " + std::to_string(task.depth->value) : "";
    Probes probes;
    Targets constants, pending;
    std::map<std::size_t, irep_idt> ids;
    for (const auto& p : batch.properties) {
        if (p.is_disabled() || p.is_assumption()) continue;
        exprt::operandst disabled;
        const auto body = implication(p.original_expr, disabled);
        if (!body) continue;
        auto probe = p;
        try {
            probe.original_expr = unreachable(*body, std::move(disabled), ns, buechi);
        } catch (...) {
            probe.failure(failure_reason());
        }
        if (!probe.is_failure() && probe.original_expr.is_true()) {
            constants.insert(p.identifier);
        } else {
            probe.identifier = "crisp::antecedent::" + id2string(p.identifier);
            probe.normalized_expr = probe.original_expr;
            ids.emplace(p.number, p.identifier);
            probes.emplace(p.identifier, std::move(probe));
        }
    }
    bool checking = false;
    auto record = [&](const transition_systemt& model, const ebmc_propertiest& queries,
                      ebmc_propertiest::propertiest& results) {
        // An unsupported assumption invalidates the entire batch.
        for (auto& p : results) if (p.is_assumption()) report(model, queries, p);
        for (auto& p : results) {
            if (p.is_disabled() || p.is_assumption()) continue;
            const bool success = p.is_proved() || (bounded && p.is_proved_with_bound());
            if (success && constants.count(p.identifier)) {
                publish(p.identifier, "vacuous", "antecedent is constant false");
            } else if (success && probes.count(p.identifier) && probes.at(p.identifier).is_failure()) {
                publish(p.identifier, "unknown", original + "; vacuity check: " + probes.at(p.identifier).status_as_string());
            } else if (success && probes.count(p.identifier)) {
                publish(p.identifier, "checking", original);
                pending.insert(p.identifier);
            } else report(model, queries, p);
        }
        checking = true;
    };
    auto finish = [&](const Property& probe) {
        const auto& id = ids.at(probe.number);
        pending.erase(id);
        if (refuted(probe))
            publish(id, original.c_str(), detail);
        else if (probe.is_proved() || (bounded && probe.is_proved_with_bound()))
            publish(id, "vacuous", bounded ? "antecedent not reached within depth " + std::to_string(task.depth->value) :
                                             "antecedent proved unreachable");
        else publish(id, "unknown", original + "; vacuity check: " + probe.status_as_string());
    };
    auto check = [&](transition_systemt& model, ebmc_propertiest& queries,
                     const witness_lasso_observert& observer = {}) {
        if (buechi)
            instrument_buechi(model, queries, pdr, messages);
        auto result = property_checker(cmd, model, queries, messages, observer);
        if (result.status != property_checker_resultt::statust::VERIFICATION_RESULT)
            throw std::runtime_error("EBMC did not return verification results");
        return result;
    };
    try {
        if (bounded) {
            if (probes.empty()) {
                auto result = check(system, batch, observe_lasso);
                record(system, batch, result.properties);
            } else {
                bounded_checks(cmd, system, batch, probes, task.depth->value, [&](ebmc_propertiest& checked) {
                    property_checker_resultt originals{checked};
                    // Only original counterexamples need the backend's infinite
                    // extension. A passed assertion's probe uses its finite depth.
                    certify_property_witnesses(cmd, system, originals, task.depth->value, messages, observe_lasso);
                    record(system, checked, originals.properties);
                    return pending;
                }, finish, messages);
            }
            return;
        }
        // Monitor installation changes both the properties and the transition
        // relation. Each antecedent query starts from the original design.
        {
            auto primary = fresh_model(system);
            auto queries = batch;
            auto result = check(primary, queries, observe_lasso);
            record(primary, queries, result.properties);
        }
        if (pending.empty()) return;
        const auto now = std::chrono::steady_clock::now();
        const auto shallow_deadline = std::min(now + std::chrono::seconds(30), now + (deadline - now) / 4);
        try {
            run_shallow_bmc(shallow_deadline, [&](const Witness& hit) {
                constexpr std::size_t depth = 4;
                certified_witnesses(cmd, system, batch, probes, pending, depth + 1, nullptr, messages, hit);
            }, [&](std::size_t number) {
                const auto& id = ids.at(number);
                publish(id, original.c_str(), detail);
                pending.erase(id);
            });
        } catch (...) {
            std::fprintf(stderr, "shallow BMC: %s\n", failure_reason().c_str());
        }
        if (!pending.empty()) {
            auto model = fresh_model(system);
            ebmc_propertiest queries;
            for (const auto& p : batch.properties)
                if (p.is_assumption()) queries.properties.push_back(p);
            for (const auto& id : pending) {
                const auto& probe = probes.at(id);
                auto symbol = model.symbol_table.lookup_ref(id);
                symbol.name = probe.identifier;
                symbol.value = probe.original_expr;
                if (model.symbol_table.add(std::move(symbol)))
                    throw std::runtime_error("duplicate antecedent symbol: " + id2string(probe.identifier));
                queries.properties.push_back(probe);
            }
            auto exact = check(model, queries);
            for (const auto& p : exact.properties)
                if (p.is_assumption() && !p.is_assumed()) throw std::runtime_error(p.status_as_string());
            for (const auto& p : exact.properties) {
                if (p.is_assumption()) continue;
                const auto& id = ids.at(p.number);
                if (!pending.count(id) || p.identifier != probes.at(id).identifier)
                    throw std::runtime_error("unexpected EBMC antecedent result");
                if (!refuted(p) || has_infinite_witness(p)) finish(p);
            }
            for (auto& p : exact.properties) {
                if (p.is_assumption() || !refuted(p)) continue;
                const auto& id = ids.at(p.number);
                if (!pending.count(id)) continue; // already certified on this goal

                std::string reason = "finite counterexample has no certified infinite extension";
                bool certified = false;
                try {
                    if (!p.witness_trace)
                        p.witness_trace = reconstruct_counterexample(
                            cmd, model, queries, p, TraceRequirement::finite_prefix, messages);
                    const auto end = p.witness_trace ? p.witness_trace->get_min_failing_timeframe() : std::nullopt;
                    if (end) {
                        certified_witnesses(cmd, system, batch, probes, {id}, *end + 1, &*p.witness_trace, messages,
                                            [&](std::size_t) { certified = true; });
                    }
                } catch (...) {
                    reason += ": " + failure_reason();
                }
                if (certified) finish(p);
                else {
                    pending.erase(id);
                    publish(id, "unknown", original + "; vacuity check: " + reason);
                }
            }
            if (!pending.empty()) throw std::runtime_error("missing EBMC antecedent result");
        }
    } catch (...) {
        if (!checking) throw;
        const auto why = failure_reason();
        for (const auto& id : pending) publish(id, "unknown", original + "; vacuity check: " + why);
    }
}
} // namespace crisp::check
