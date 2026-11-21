#include "vacuity.hpp"

#include <cstdio>

#include <ebmc/ebmc_error.h>
#include <ebmc/instrument_buechi.h>
#include <ebmc/property_checker.h>
#include <ebmc/random_traces.h>
#include <solvers/flattening/boolbv.h>
#include <solvers/sat/satcheck.h>
#include <temporal-logic/normalize_property.h>
#include <temporal-logic/rewrite_sva_sequence.h>
#include <trans-word-level/cone_of_influence.h>
#include <trans-word-level/lasso.h>
#include <util/exception_utils.h>
#include <util/simplify_expr.h>
#include <verilog/sva_expr.h>
#include <verilog/verilog_types.h>

namespace crisp::check {
namespace {
using Property = ebmc_propertiest::propertyt;

// Random input constraints and successive query subsets can mention SAT
// variables after the first solve. Minisat's eliminating variant cannot
// introduce such constraints incrementally (in particular for packed inputs).
ebmc_solver_factoryt incremental_solver(const cmdlinet& cmd) {
    if (cmd.isset("yices")) return ebmc_solver_factory(cmd);
    return [](const namespacet& ns, message_handlert& messages) {
        auto sat = std::make_unique<satcheck_no_simplifiert>(messages);
        auto bv = std::make_unique<boolbvt>(ns, *sat, messages);
        return ebmc_solvert{std::move(sat), std::move(bv)};
    };
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

void sample_antecedents(const transition_systemt& system, const ebmc_propertiest& assumptions,
                        const Probes& probes, const Targets& targets, std::size_t bound, bool bounded,
                        const irep_idt& reset, const ebmc_solver_factoryt& factory,
                        message_handlert& messages,
                        const std::function<void(const irep_idt&)>& reached) {
    // Give random_traces the real transition relation and the elaborated
    // root inputs. Only sample scalar/packed inputs it can randomize. Other
    // inputs and initial memory contents are chosen by the constrained solver.
    transition_systemt view;
    auto root = *system.main_symbol;
    auto& ports = to_module_type(root.type).ports();
    module_typet::portst sampled;
    for (auto port : ports) {
        if (!port.input() || port.output()) continue;
        const auto* signal = system.symbol_table.lookup(port.identifier());
        if (!signal || signal->name == reset) continue;
        const auto type = signal->type.id();
        if (type != ID_bool && type != ID_signedbv && type != ID_unsignedbv && type != ID_bv) continue;
        port.type() = signal->type;
        sampled.push_back(std::move(port));
    }
    if (sampled.empty()) return;
    // EBMC keeps all general constraints and all definitions needed by the
    // seeds. A trace of this projection extends to a trace of the full model.
    // In particular keep reset logic and every assumption in the cone, and
    // seed the sampled inputs since random_traces will constrain those too.
    const auto frames = bound + 1;
    exprt::operandst seeds, constraints;
    std::map<irep_idt, exprt> matches;
    if (!reset.empty()) seeds.push_back(system.symbol_table.lookup_ref(reset).symbol_expr());
    for (const auto& p : assumptions.properties) {
        if (!p.is_assumption()) continue;
        const auto& e = p.normalized_expr;
        // A finite prefix alone cannot discharge a future assumption in an
        // unbounded proof. Let the original engine handle it.
        if (!bounded && (e.id() != ID_sva_always || has_temporal_operator(to_unary_expr(e).op()))) return;
        if (!bmc_supports_property(e)) return;
        auto obligations = property(e, true, messages, frames);
        if (uses_lasso_symbol(obligations)) return; // loop equality needs the full state
        constraints.push_back(conjunction(obligations));
        seeds.push_back(e);
    }
    for (const auto& id : targets) {
        const auto& e = probes.at(id).normalized_expr;
        if (!bmc_supports_property(e)) continue;
        auto obligations = property(e, false, messages, frames);
        if (uses_lasso_symbol(obligations)) return;
        matches.emplace(id, not_exprt{conjunction(obligations)});
        seeds.push_back(e);
    }
    if (matches.empty()) return;
    for (const auto& input : sampled) seeds.push_back(symbol_exprt{input.identifier(), input.type()});
    view.trans_expr = cone_of_influence(system.trans_expr, seeds).trans;
    ports = std::move(sampled);
    const auto root_name = root.name;
    view.symbol_table.add(std::move(root));
    view.main_symbol = view.symbol_table.lookup(root_name);

    const namespacet ns(system.symbol_table);
    std::map<irep_idt, exprt> handles;
    decision_proceduret* solver = nullptr;
    auto constrained = [&](const namespacet&, message_handlert& handler) {
        auto container = factory(ns, handler);
        solver = &container.decision_procedure();
        for (const auto& c : constraints) solver->set_to_true(c);
        for (const auto& [id, match] : matches) handles.emplace(id, solver->handle(match));
        return container;
    };

    const auto wanted = targets.size();
    Targets hit;
    struct AllReached {};
    auto visit = [&](trans_tracet) {
        // The callback runs only after SAT. Read the property handles in
        // that very model, without parsing a VCD or simulating SVA.
        for (const auto& [id, handle] : handles)
            if (!hit.count(id) && solver->get(handle).is_true()) {
                hit.insert(id);
                reached(id);
            }
        if (hit.size() == wanted) throw AllReached{};
    };
    try {
        random_traces(view, visit, 8, bound, constrained, messages);
    } catch (const AllReached&) {}
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
                       const front::Task& task, const irep_idt& reset, Deadline deadline, message_handlert& messages,
                       const ReportOriginal& report, const ReportVacuity& publish) {
    const namespacet ns(system.symbol_table);
    const bool bounded = task.mode.value == front::Mode::bmc;
    const bool pdr = task.mode.value == front::Mode::pdr;
    const bool buechi = pdr || task.mode.value == front::Mode::kind;
    const std::size_t bound = bounded ? task.depth->value : 20;
    const std::string original = bounded ? "passed" : "proved";
    const auto detail = bounded ? "bmc depth " + std::to_string(bound) : "";
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
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    Clock::duration original_time{};
    auto record = [&](const transition_systemt& model, const ebmc_propertiest& queries,
                      ebmc_propertiest::propertiest& results) {
        original_time = Clock::now() - start;
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
    auto sample = [&] {
        if (!pending.empty()) {
            const auto now = Clock::now();
            const auto budget = deadline == Deadline::max() ? original_time :
                                std::min(original_time, (deadline - now) / 4);
            const auto stop = now + budget;
            try {
                run_sampling(stop, [&](const Witness& hit) {
                    try {
                        sample_antecedents(system, batch, probes, pending, bound, bounded, reset,
                                           incremental_solver(cmd), messages,
                                           [&](const irep_idt& id) { hit(probes.at(id).number); });
                    } catch (...) { throw std::runtime_error(failure_reason()); }
                }, [&](std::size_t number) {
                    const auto& id = ids.at(number);
                    publish(id, original.c_str(), detail);
                    pending.erase(id);
                });
            } catch (const std::exception& e) {
                std::fprintf(stderr, "random traces: %s\n", e.what());
            }
        }
        return pending;
    };
    auto finish = [&](const Property& probe) {
        const auto& id = ids.at(probe.number);
        pending.erase(id);
        if (probe.is_refuted() || probe.status == Property::statust::REFUTED_WITH_BOUND)
            publish(id, original.c_str(), detail);
        else if (probe.is_proved() || (bounded && probe.is_proved_with_bound()))
            publish(id, "vacuous", bounded ? "antecedent not reached within depth " + std::to_string(bound) :
                                             "antecedent proved unreachable");
        else publish(id, "unknown", original + "; vacuity check: " + probe.status_as_string());
    };
    auto fresh_model = [&] {
        auto model = system;
        model.main_symbol = &model.symbol_table.lookup_ref(system.main_symbol->name);
        return model;
    };
    auto check = [&](transition_systemt& model, ebmc_propertiest& queries) {
        if (buechi)
            instrument_buechi(model, queries, pdr, messages);
        auto result = property_checker(cmd, model, queries, messages);
        if (result.status != property_checker_resultt::statust::VERIFICATION_RESULT)
            throw std::runtime_error("EBMC did not return verification results");
        return result;
    };
    try {
        if (bounded) {
            if (probes.empty()) {
                auto result = check(system, batch);
                record(system, batch, result.properties);
            } else {
                bounded_checks(cmd, system, batch, probes, bound, [&](ebmc_propertiest& checked) {
                    record(system, checked, checked.properties);
                    return sample();
                }, finish, messages);
            }
            return;
        }
        // Monitor installation changes both the properties and the transition
        // relation. Release this copy before sampling the original design.
        {
            auto primary = fresh_model();
            auto queries = batch;
            auto result = check(primary, queries);
            record(primary, queries, result.properties);
        }
        sample();
        if (!pending.empty()) {
            auto model = fresh_model();
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
            const auto exact = check(model, queries);
            for (const auto& p : exact.properties)
                if (p.is_assumption() && !p.is_assumed()) throw std::runtime_error(p.status_as_string());
            for (const auto& p : exact.properties) {
                if (p.is_assumption()) continue;
                const auto& id = ids.at(p.number);
                if (!pending.count(id) || p.identifier != probes.at(id).identifier)
                    throw std::runtime_error("unexpected EBMC antecedent result");
                finish(p);
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
