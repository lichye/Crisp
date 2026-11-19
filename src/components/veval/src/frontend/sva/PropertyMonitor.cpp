#include "PropertyMonitor.h"
#include "util/database/JsonEscape.h"
#include "LocalVariableMonitor.h"
#include "ClockSampling.h"
#include "SafetyMonitor.h"

#include "frontend/rtl/ElaboratedDesign.h"

#include <cstdint>
#include <util/arith_tools.h>
#include <util/bitvector_types.h>
#include <util/format_expr.h>
#include <util/mathematical_types.h>
#include <util/replace_expr.h>
#include <util/std_expr.h>

#include <ebmc/ebmc_error.h>
#include <ebmc/ebmc_properties.h>
#include <ebmc/transition_system.h>
#include <temporal-logic/ltl_to_buechi.h>
#include <temporal-logic/normalize_property.h>
#include <trans-word-level/next_symbol.h>
#include <temporal-logic/temporal_logic.h>
#include <verilog/sva_expr.h>

#include <algorithm>
#include <ostream>
#include <functional>
#include <limits>
#include <set>
#include <sstream>

namespace veval {
namespace sva {

namespace {

std::string asString(const exprt& expr) {
    std::ostringstream out;
    out << format(expr);
    return frontend::displayName(out.str());
}

std::string escape(const std::string& s) {
    return veval::util::jsonEscape(s);
}

// Collects design symbols, skipping the automaton's own state variable and
// the `next` form of it.
void collectAtomSymbols(const exprt& expr, const irep_idt& stateSymbol,
                        std::vector<symbol_exprt>& dest,
                        std::set<irep_idt>& seen) {
    if (expr.id() == ID_symbol) {
        const auto& symbol = to_symbol_expr(expr);
        if (symbol.get_bool("#veval_sva_function_local")) return;
        if (symbol.get_identifier() != stateSymbol &&
            seen.insert(symbol.get_identifier()).second)
            dest.push_back(symbol);
        return;
    }
    // A next-state reference carries the same identifier as the symbol it
    // refers to; the state variable must not leak in through it.
    if (expr.id() == ID_next_symbol) {
        if (expr.get(ID_identifier) != stateSymbol) {
            symbol_exprt as_symbol{expr.get(ID_identifier), expr.type()};
            if (seen.insert(as_symbol.get_identifier()).second)
                dest.push_back(as_symbol);
        }
        return;
    }
    for (const auto& op : expr.operands())
        collectAtomSymbols(op, stateSymbol, dest, seen);
}

}  // namespace

std::size_t Monitor::stateCount() const {
    // The safety monitor is a subset bitmap. Report an upper bound, saturating
    // instead of overflowing for an automaton with a wide bitmap.
    if (stateSymbol.type().id() == ID_unsignedbv) {
        const auto width = to_unsignedbv_type(stateSymbol.type()).get_width();
        return width >= std::numeric_limits<std::size_t>::digits
                   ? std::numeric_limits<std::size_t>::max() : std::size_t{1} << width;
    }
    if (stateSymbol.type().id() == ID_range) {
        const auto size = to_integer_range_type(stateSymbol.type()).size();
        return size > std::numeric_limits<std::size_t>::max()
                   ? std::numeric_limits<std::size_t>::max() : numeric_cast_v<std::size_t>(size);
    }
    return 0;
}

std::vector<symbol_exprt> Monitor::atomSymbols() const {
    std::vector<symbol_exprt> result;
    std::set<irep_idt> seen;
    const auto& state = stateSymbol.get_identifier();
    collectAtomSymbols(trans, state, result, seen);
    collectAtomSymbols(init, state, result, seen);
    collectAtomSymbols(errorSignal, state, result, seen);
    collectAtomSymbols(livenessSignal, state, result, seen);
    std::sort(result.begin(), result.end(),
              [](const symbol_exprt& a, const symbol_exprt& b) {
                  return id2string(a.get_identifier()) <
                         id2string(b.get_identifier());
              });
    return result;
}

// Reject four-state-sensitive operations before building a two-state monitor.
// The frontend preserves their original operator in a marker after normalization.
std::string xzConstruct(const exprt& expr) {
    // Normalization can erase the original operator ID; check the marker first.
    auto marked = expr.get(ID_C_verilog_xz_sensitive);
    if (!marked.empty()) return id2string(marked);

    // Also recognize expressions that have not been normalized yet.
    if (expr.id() == ID_verilog_case_equality) return "===";
    if (expr.id() == ID_verilog_case_inequality) return "!==";
    if (expr.id() == ID_verilog_wildcard_equality) return "==?";
    if (expr.id() == ID_verilog_wildcard_inequality) return "!=?";

    for (const auto& op : expr.operands()) {
        auto found = xzConstruct(op);
        if (!found.empty()) return found;
    }
    return {};
}

namespace {

// Collect guard symbols before normalization removes the disable-iff nodes.
void collectAbortAtoms(const exprt& expr, std::vector<symbol_exprt>& into) {
    std::set<irep_idt> seen;
    for (const auto& symbol : into) seen.insert(symbol.get_identifier());
    std::function<void(const exprt&, bool)> visit = [&](const exprt& node, bool inGuard) {
        if (inGuard && node.id() == ID_symbol) {
            const auto& symbol = to_symbol_expr(node);
            if (seen.insert(symbol.get_identifier()).second) into.push_back(symbol);
        }
        const bool abort = node.id() == ID_sva_disable_iff ||
                           node.id() == ID_sva_sequence_disable_iff;
        // Operand 0 is the guard. Preserve first-occurrence order across guards.
        for (std::size_t i = 0; i < node.operands().size(); ++i)
            visit(node.operands()[i], inGuard || (abort && i == 0));
    };
    visit(expr, false);
}

// Is there an abort anywhere under here?
bool hasDisableIff(const exprt& expr) {
    if (expr.id() == ID_sva_disable_iff ||
        expr.id() == ID_sva_sequence_disable_iff)
        return true;
    for (const auto& op : expr.operands())
        if (hasDisableIff(op)) return true;
    return false;
}

// Split [assume] always(disable iff(C) BODY) before normalizing the body.
// The vendored C-or-BODY rewrite checks C only at the first cycle; SVA aborts
// an attempt whenever C holds during its window. Reapply C to the monitor
// transition and error predicates instead. Nested or temporal guards are refused.
enum class AbortShape { None, Split, Refused };

AbortShape splitAbort(const exprt& original, exprt& condition,
                      exprt& bodyProperty, std::string& refusal) {
    if (!hasDisableIff(original)) return AbortShape::None;

    exprt outer = original;
    if (outer.id() == ID_sva_assume && outer.operands().size() == 1)
        outer = to_unary_expr(outer).op();

    bool underAlways = false;
    exprt inner = outer;
    if (inner.id() == ID_sva_always && inner.operands().size() == 1) {
        underAlways = true;
        inner = to_unary_expr(inner).op();
    }

    if (inner.id() != ID_sva_disable_iff || inner.operands().size() != 2) {
        refusal =
            "DISABLE-IFF-SHAPE: the abort is not at the head of the property, "
            "where SVA puts it. Its per-attempt window cannot be computed "
            "from this shape, and the only fallback is the vendored rewrite "
            "`C or BODY', which is STRICTLY STRONGER than what was written";
        return AbortShape::Refused;
    }

    const auto& abort = to_sva_abort_expr(inner);
    condition = abort.condition();
    const auto& body = abort.property();

    if (hasDisableIff(condition) || hasDisableIff(body)) {
        refusal =
            "DISABLE-IFF-SHAPE: more than one `disable iff' in one property; "
            "which attempt each aborts is not decided by this tool";
        return AbortShape::Refused;
    }
    if (has_temporal_operator(condition)) {
        refusal =
            "DISABLE-IFF-SHAPE: the abort condition is temporal. `disable "
            "iff' takes an expression that is evaluated at each cycle of the "
            "attempt (1800-2017 16.6), and a temporal condition has no such "
            "reading here";
        return AbortShape::Refused;
    }
    if (condition.type().id() != ID_bool) {
        refusal =
            "DISABLE-IFF-SHAPE: the abort condition did not arrive as a "
            "boolean, so it cannot be conjoined to the monitor's error line "
            "as written";
        return AbortShape::Refused;
    }

    bodyProperty = underAlways ? static_cast<exprt>(sva_always_exprt{body})
                               : body;
    return AbortShape::Split;
}

exprt peelSequenceProperty(exprt e) {
    while ((e.id() == ID_sva_sequence_property || e.id() == ID_sva_implicit_weak ||
            e.id() == ID_sva_implicit_strong || e.id() == ID_sva_weak ||
            e.id() == ID_sva_strong || e.id() == ID_sva_boolean) &&
           e.operands().size() == 1) {
        exprt op = e.operands()[0];
        e = std::move(op);
    }
    return e;
}

std::optional<exprt> statePredicateExpression(const exprt& expression) {
    const auto e = peelSequenceProperty(expression);
    if (e.type().id() == ID_bool && !has_temporal_operator(e)) return e;
    // An overlapped implication between two state predicates consumes one
    // sample. Preserve its Boolean meaning for the satisfiability check too.
    if ((e.id() == ID_sva_overlapped_implication || e.id() == ID_sva_implies) &&
        e.operands().size() == 2) {
        auto lhs = statePredicateExpression(e.operands()[0]);
        auto rhs = statePredicateExpression(e.operands()[1]);
        if (lhs && rhs) return implies_exprt{*lhs, *rhs};
    }
    return {};
}

bool statePredicate(const exprt& e) {
    return statePredicateExpression(e).has_value();
}

// Require a state predicate whose Boolean skeleton is satisfiable, treating
// non-Boolean subexpressions as independent propositions, as Spot does.
bool propositionallySatisfiable(const exprt& expression) {
    const auto predicate = statePredicateExpression(expression);
    if (!predicate) return false;
    const auto& b = *predicate;
    std::vector<exprt> atoms;
    std::function<void(const exprt&)> collect = [&](const exprt& e) {
        if (e.is_true() || e.is_false()) return;
        if (e.id() == ID_and || e.id() == ID_or || e.id() == ID_not || e.id() == ID_implies ||
            e.id() == ID_xor ||
            ((e.id() == ID_equal || e.id() == ID_notequal) && e.operands().size() == 2 &&
             e.operands()[0].type().id() == ID_bool)) {
            for (const auto& op : e.operands()) collect(op);
            return;
        }
        if (std::find(atoms.begin(), atoms.end(), e) == atoms.end()) atoms.push_back(e);
    };
    collect(b);
    if (atoms.size() > 20) return false;  // not decided here: refuse
    std::function<bool(const exprt&, std::uint32_t)> eval = [&](const exprt& e, std::uint32_t v) -> bool {
        if (e.is_true()) return true;
        if (e.is_false()) return false;
        if (e.id() == ID_and) {
            for (const auto& op : e.operands()) if (!eval(op, v)) return false;
            return true;
        }
        if (e.id() == ID_or) {
            for (const auto& op : e.operands()) if (eval(op, v)) return true;
            return false;
        }
        if (e.id() == ID_not) return !eval(e.operands()[0], v);
        if (e.id() == ID_implies) return !eval(e.operands()[0], v) || eval(e.operands()[1], v);
        if (e.id() == ID_xor) {
            bool r = false;
            for (const auto& op : e.operands()) r ^= eval(op, v);
            return r;
        }
        if ((e.id() == ID_equal || e.id() == ID_notequal) && e.operands().size() == 2 &&
            e.operands()[0].type().id() == ID_bool)
            return (eval(e.operands()[0], v) == eval(e.operands()[1], v)) == (e.id() == ID_equal);
        const auto at = std::find(atoms.begin(), atoms.end(), e) - atoms.begin();
        return (v >> at) & 1u;
    };
    for (std::uint32_t v = 0; v < (1u << atoms.size()); ++v)
        if (eval(b, v)) return true;
    return false;
}

}  // namespace

bool abortWindowExact(const exprt& property) {
    if (hasLocalVariables(property)) return localAbortWindowExact(property);
    const auto body = peelSequenceProperty(property);
    // A state predicate: every attempt is one cycle long.
    if (statePredicate(body)) return true;
    // Recognize the printed equivalent of a fixed-delay implication:
    // (!a) or (always [k:k] b). Multiple required cycles remain unsupported.
    if ((body.id() == ID_or || body.id() == ID_sva_or) && body.operands().size() == 2) {
        for (unsigned i = 0; i < 2; ++i) {
            const auto antecedent = peelSequenceProperty(body.operands()[i]);
            const auto delayed = peelSequenceProperty(body.operands()[1-i]);
            if (antecedent.id() != ID_not || antecedent.operands().size() != 1 ||
                !statePredicate(antecedent.operands()[0]) ||
                delayed.id() != ID_sva_ranged_always || delayed.operands().size() != 3)
                continue;
            const auto from = numeric_cast<mp_integer>(delayed.operands()[0]);
            const auto to = numeric_cast<mp_integer>(delayed.operands()[1]);
            auto required = peelSequenceProperty(delayed.operands()[2]);
            // For a |=> ##k b, the delay can follow always [1:1]. Strip only
            // fixed delays with no intervening requirement.
            while (required.id() == ID_sva_cycle_delay && required.operands().size() == 4 &&
                   required.operands()[0].is_nil()) {
                const auto lo = numeric_cast<mp_integer>(required.operands()[1]);
                const auto hi = required.operands()[2].is_nil()
                                    ? lo : numeric_cast<mp_integer>(required.operands()[2]);
                if (!lo || !hi || *lo < 0 || *lo != *hi) break;
                required = peelSequenceProperty(required.operands()[3]);
            }
            if (from && to && *from >= 0 && *from == *to &&
                propositionallySatisfiable(required)) return true;
        }
    }
    const bool booleanImplication = body.id() == ID_sva_implies || body.id() == ID_implies;
    if ((body.id() != ID_sva_overlapped_implication &&
         body.id() != ID_sva_non_overlapped_implication && !booleanImplication) ||
        body.operands().size() != 2)
        return false;
    // The printer spells normalized `a |-> ##k b' as `a implies (##k b)'.
    // Property implication has the same trigger only for a state antecedent.
    if (booleanImplication && !statePredicate(body.operands()[0])) return false;
    // A shared satisfiable consequent can satisfy all open attempts together.
    // Multiple requirements or an impossible consequent may report failure early.
    const auto consequent = peelSequenceProperty(body.operands()[1]);
    if (statePredicate(consequent)) return propositionallySatisfiable(consequent);
    // With Boolean p and q, failure occurs only when p drops before q.
    if ((consequent.id() == ID_sva_until || consequent.id() == ID_sva_s_until) &&
        consequent.operands().size() == 2) {
        const auto p = peelSequenceProperty(consequent.operands()[0]);
        const auto q = peelSequenceProperty(consequent.operands()[1]);
        return statePredicate(p) && statePredicate(q) && propositionallySatisfiable(or_exprt{p, q});
    }
    // Response liveness has no finite failure; abort resets the pending obligation.
    // The backend retains the liveness acceptance condition.
    if (consequent.id() == ID_sva_s_eventually && consequent.operands().size() == 1) {
        const auto eventual = peelSequenceProperty(consequent.operands()[0]);
        return propositionallySatisfiable(eventual);
    }
    if (consequent.id() != ID_sva_cycle_delay || consequent.operands().size() != 4 ||
        consequent.operands()[0].is_not_nil() ||
        consequent.operands()[2].id() == ID_infinity)
        return false;
    const auto required = peelSequenceProperty(consequent.operands()[3]);
    return propositionallySatisfiable(required);
}

bool resetContractLabel(const std::string& name) {
    auto dot = name.rfind('.');
    const auto label = dot == std::string::npos ? name : name.substr(dot + 1);
    static const std::string prefix = "reset_contract";
    return label.compare(0, prefix.size(), prefix) == 0;
}

bool engineDropsAsAssumption(const exprt& normalized) {
    // No printable sentence means no external-engine warning.
    if (normalized.is_nil()) return false;

    // Match external k-induction's always/AG/G state-predicate check.
    // BDD admits fewer shapes; this predicate is not a general engine capability test.
    if (normalized.id() != ID_sva_always && normalized.id() != ID_AG &&
        normalized.id() != ID_G)
        return true;
    if (normalized.operands().size() != 1) return true;
    return has_temporal_operator(to_unary_expr(normalized).op());
}

// Why a property cannot be compiled faithfully, or "" when it can.
std::string lostConstruct(const exprt& expr) {
    if (expr.id() == ID_sva_accept_on || expr.id() == ID_sva_reject_on ||
        expr.id() == ID_sva_sync_accept_on || expr.id() == ID_sva_sync_reject_on)
        return "accept_on/reject_on is unsupported: the available lowering reads "
               "the abort condition at the attempt's first cycle only";
    if (expr.id() == ID_verilog_past && expr.get_bool("#veval_dropped_sampling_arguments"))
        return "a gated or explicitly clocked sampled-value call is unsupported: "
               "its gate or clock is not modelled";
    for (const auto& operand : expr.operands())
        if (auto lost = lostConstruct(operand); !lost.empty()) return lost;
    return {};
}

MonitorSet monitors(const transition_systemt& transitionSystem,
                    message_handlert& messageHandler) {
    MonitorSet result;

    auto properties = ebmc_propertiest::from_transition_system(
        transitionSystem, messageHandler);
    SamplingCheck sampling(transitionSystem);

    for (const auto& property : properties.properties) {
        const auto name =
            frontend::displayName(id2string(property.identifier));
        const bool isAssumption = property.is_assumption();
        const bool isResetContract = resetContractLabel(name);
        auto reject = [&](std::string reason) {
            result.unsupported.push_back({name, std::move(reason), isAssumption, isResetContract});
        };

        if (!is_LTL(property.normalized_expr) &&
            !is_SVA(property.normalized_expr)) {
            reject("not LTL or SVA");
            continue;
        }

        auto xz = xzConstruct(property.original_expr);
        if (xz.empty()) xz = xzConstruct(property.normalized_expr);
        if (!xz.empty()) {
            reject(std::string("XZ-CONSTRUCT: uses `") + xz +
                   "', whose meaning depends on X and Z. Lowering makes "
                   "it ordinary equality, so a monitor built from it "
                   "would check a different property than the one written");
            continue;
        }

        // Cover reachability is not an assertion violation. Other unsupported
        // constructs are rejected before their temporal meaning can be lost.
        if (property.original_expr.id() == ID_sva_cover) {
            reject("a cover statement is not an obligation; reaching it is not a "
                   "refutation, and this tool decides obligations only");
            continue;
        }
        if (auto lost = lostConstruct(property.original_expr); !lost.empty()) {
            reject(std::move(lost));
            continue;
        }
        // Validate the original clock event before compiling a monitor.
        if (auto why = sampling.refusal(property.identifier, property.original_expr); !why.empty()) {
            reject(std::move(why));
            continue;
        }

        if (hasLocalVariables(property.original_expr)) {
            try {
                if (isAssumption || isResetContract)
                    throw std::runtime_error("SVA-LOCALS: assumptions with local variables are unsupported");
                auto monitor = localViolationMonitor(property.original_expr, name).monitor;
                monitor.sourceExpr = asString(monitor.sourceProperty);
                if (auto net = sampling.clock(property.identifier, property.original_expr); !net.empty())
                    result.samplingClocks[name] = net;
                result.monitors.push_back(std::move(monitor));
            } catch (const std::exception& error) {
                reject(error.what());
            }
            continue;
        }

        // Normalize an aborted body separately; the original guard is applied below.
        exprt abortCondition = nil_exprt{};
        exprt carried = property.normalized_expr;
        {
            exprt bodyProperty;
            std::string refusal;
            auto shape = splitAbort(property.original_expr, abortCondition,
                                    bodyProperty, refusal);
            if (shape == AbortShape::Refused) {
                reject(std::move(refusal));
                continue;
            }
            if (shape == AbortShape::Split) {
                const exprt& written =
                    bodyProperty.id() == ID_sva_always && bodyProperty.operands().size() == 1
                        ? bodyProperty.operands()[0] : bodyProperty;
                if (!abortWindowExact(written)) {
                    reject("DISABLE-IFF-WINDOW: the monitor may report this body's failure "
                           "before the attempt's window ends, so the abort condition would "
                           "be checked over too short a window (a false counterexample on "
                           "an assert, a stronger environment on an assume). Refused until "
                           "the encoding is ruled on");
                    continue;
                }
                try {
                    carried = normalize_property(bodyProperty);
                } catch (const std::exception& error) {
                    reject(std::string("the body under `disable iff' does "
                                       "not normalise: ") + error.what());
                    continue;
                }
            }
        }

        try {
            // The automaton is built for the negation, so it accepts exactly
            // the violating traces.
            auto buechi = ltl_to_buechi(not_exprt{carried}, messageHandler);

            // Any assertion can become an assumption after transport. Keep
            // all reachable NFA states, so !error excludes EVERY violating
            // run instead of allowing the solver to choose a harmless run.
            determinizeSafety(buechi, messageHandler);

            // Abort masks failure and resets monitor progress at every guarded cycle.
            // Attempts started after the abort remain independent of earlier obligations.
            if (abortCondition.is_not_nil()) {
                exprt nextInitial = buechi.init;
                replace_mapt toNext;
                toNext.insert(
                    {buechi.state_symbol,
                     next_symbol_exprt{buechi.state_symbol.get_identifier(),
                                       buechi.state_symbol.type()}});
                replace_expr(toNext, nextInitial);

                buechi.error_signal =
                    and_exprt{buechi.error_signal, not_exprt{abortCondition}};
                buechi.trans = if_exprt{abortCondition, nextInitial,
                                        buechi.trans};
            }

            Monitor monitor;
            monitor.property = name;
            monitor.isAssumption = isAssumption;
            monitor.isResetContract = isResetContract;
            if (auto net = sampling.clock(property.identifier, property.original_expr); !net.empty())
                result.samplingClocks[name] = net;
            // Preserve disable iff in generated SVA; C-or-BODY would be stronger.
            monitor.sourceProperty =
                abortCondition.is_nil()
                    ? carried
                    : static_cast<exprt>(
                          sva_disable_iff_exprt{abortCondition, carried});
            monitor.originalProperty = property.original_expr;
            monitor.sourceExpr = asString(monitor.sourceProperty);
            monitor.stateSymbol = buechi.state_symbol;
            monitor.init = buechi.init;
            monitor.trans = buechi.trans;
            monitor.errorSignal = buechi.error_signal;
            monitor.livenessSignal = buechi.liveness_signal;
            collectAbortAtoms(property.original_expr, monitor.abortAtoms);
            result.monitors.push_back(std::move(monitor));
        } catch (const ltl_to_buechi_unsupportedt& error) {
            reject(error.expr.id_string() + " does not convert");
        } catch (const ebmc_errort& error) {
            // Record conversion failures per property, including a missing Spot executable.
            reject(error.what());
        } catch (const std::exception& error) {
            reject(error.what());
        } catch (const std::string& error) {
            reject(error);
        } catch (const char* error) {
            reject(error);
        }
    }
    return result;
}

void Monitor::refreshAbortAtoms() {
    abortAtoms.clear();
    collectAbortAtoms(sourceProperty, abortAtoms);
}

std::optional<Monitor> withoutAbort(const Monitor& monitor) {
    if (monitor.isAssumption || monitor.isResetContract ||
        monitor.sourceProperty.id() != ID_sva_disable_iff ||
        monitor.sourceProperty.operands().size() != 2 ||
        monitor.trans.id() != ID_if || monitor.trans.operands().size() != 3 ||
        monitor.errorSignal.id() != ID_and || monitor.errorSignal.operands().size() != 2)
        return {};
    const auto& guard = monitor.sourceProperty.operands()[0];
    if (monitor.trans.operands()[0] != guard ||
        monitor.errorSignal.operands()[1] != not_exprt{guard}) return {};
    auto result = monitor;
    result.sourceProperty = monitor.sourceProperty.operands()[1];
    result.trans = monitor.trans.operands()[2];
    result.errorSignal = monitor.errorSignal.operands()[0];
    unsigned removed = 0;
    std::function<void(exprt&)> strip = [&](exprt& e) {
        if (e.id() == ID_sva_disable_iff && e.operands().size() == 2) {
            exprt body = e.operands()[1];
            e = std::move(body);
            ++removed;
        }
        for (auto& operand : e.operands()) strip(operand);
    };
    strip(result.originalProperty);
    if (removed != 1) return {};
    result.abortAtoms.clear();
    result.abortDropped = true;
    result.sourceExpr = asString(result.sourceProperty);
    return result;
}

std::optional<Monitor> antecedentCoverMonitor(const Monitor& monitor,
                                              message_handlert& handler,
                                              std::string& refusal) {
    exprt property = monitor.originalProperty.is_nil()
                         ? monitor.sourceProperty
                         : monitor.originalProperty;
    if (property.id() == ID_sva_assume && property.operands().size() == 1)
        property = to_unary_expr(property).op();
    if (property.id() == ID_sva_always && property.operands().size() == 1)
        property = to_unary_expr(property).op();

    exprt abort = nil_exprt{};
    if (property.id() == ID_sva_disable_iff && property.operands().size() == 2) {
        const auto& split = to_sva_abort_expr(property);
        abort = split.condition();
        property = split.property();
    }

    if (property.id() != ID_implies &&
        property.id() != ID_sva_overlapped_implication &&
        property.id() != ID_sva_non_overlapped_implication) {
        refusal = "the source sentence is not an implication";
        return std::nullopt;
    }

    exprt antecedent = to_binary_expr(property).lhs();
    if (antecedent.type().id() != ID_bool ||
        has_temporal_operator(antecedent)) {
        refusal = "the implication antecedent is temporal";
        return std::nullopt;
    }
    if (abort.is_not_nil()) {
        if (abort.type().id() != ID_bool || has_temporal_operator(abort)) {
            refusal = "the implication's disable-iff guard is temporal";
            return std::nullopt;
        }
        antecedent = and_exprt{not_exprt{abort}, antecedent};
    }

    try {
        exprt unreachable = sva_always_exprt{not_exprt{antecedent}};
        auto buechi = ltl_to_buechi(not_exprt{unreachable}, handler);
        Monitor cover;
        cover.property = monitor.property + " antecedent cover";
        cover.sourceProperty = unreachable;
        cover.originalProperty = unreachable;
        cover.sourceExpr = asString(unreachable);
        cover.stateSymbol = buechi.state_symbol;
        cover.init = buechi.init;
        cover.trans = buechi.trans;
        cover.errorSignal = buechi.error_signal;
        cover.livenessSignal = buechi.liveness_signal;
        return cover;
    } catch (const ltl_to_buechi_unsupportedt& error) {
        refusal = error.expr.id_string() + " does not convert to a cover monitor";
    } catch (const std::exception& error) {
        refusal = error.what();
    }
    return std::nullopt;
}

MonitorSet filter(MonitorSet set, const std::vector<std::string>& patterns) {
    if (patterns.empty()) return set;

    auto rejected = [&](const auto& entry) {
        return std::none_of(patterns.begin(), patterns.end(), [&](const auto& pattern) {
            return entry.property.find(pattern) != std::string::npos;
        });
    };

    set.monitors.erase(std::remove_if(set.monitors.begin(), set.monitors.end(), rejected),
                       set.monitors.end());
    set.unsupported.erase(std::remove_if(set.unsupported.begin(), set.unsupported.end(), rejected),
                          set.unsupported.end());
    // An unmatched explicit filter is an error, not a successful empty run.
    if (set.monitors.empty() && set.unsupported.empty())
        throw std::runtime_error("--property matched no property");
    return set;
}

MonitorSet selectAssertions(MonitorSet set, const std::vector<std::string>& labels) {
    if (labels.empty()) return set;
    const std::set<std::string> wanted(labels.begin(), labels.end());
    std::set<std::string> found;
    auto label = [](const std::string& name) { return name.substr(name.rfind('.') + 1); };
    auto rejectMonitor = [&](const Monitor& monitor) {
        const bool assertion = !monitor.isAssumption && !monitor.isResetContract;
        if (assertion && !wanted.count(label(monitor.property))) return true;
        if (assertion) found.insert(label(monitor.property));
        return false;
    };
    set.monitors.erase(std::remove_if(set.monitors.begin(), set.monitors.end(), rejectMonitor),
                       set.monitors.end());
    // An unsupported entry is reported only when it is one of the selection.
    auto rejectUnsupported = [&](const UnsupportedProperty& unsupported) {
        const auto name = label(unsupported.property);
        if (!wanted.count(name)) return true;
        found.insert(name);
        return false;
    };
    set.unsupported.erase(
        std::remove_if(set.unsupported.begin(), set.unsupported.end(), rejectUnsupported),
        set.unsupported.end());
    for (const auto& name : wanted)
        if (!found.count(name)) throw std::runtime_error("--assertion " + name + " names no source assertion");
    return set;
}

void writeText(const MonitorSet& set, std::ostream& out) {
    for (const auto& monitor : set.monitors) {
        out << monitor.property << ": " << monitor.sourceExpr << '\n';
        out << "  kind        "
            << (monitor.isSafetyOnly() ? "safety" : "safety + liveness")
            << '\n';
        out << "  state       "
            << frontend::displayName(
                   id2string(monitor.stateSymbol.get_identifier()))
            << '\n';
        out << "  init        " << asString(monitor.init) << '\n';
        out << "  error       " << asString(monitor.errorSignal) << '\n';
        if (!monitor.isSafetyOnly())
            out << "  liveness    " << asString(monitor.livenessSignal)
                << '\n';
        out << "  atoms over  ";
        auto symbols = monitor.atomSymbols();
        for (std::size_t i = 0; i < symbols.size(); i++) {
            if (i != 0) out << ", ";
            out << frontend::displayName(
                id2string(symbols[i].get_identifier()));
        }
        out << '\n';
        out << "  trans       " << asString(monitor.trans) << '\n';
        out << '\n';
    }

    for (const auto& entry : set.unsupported)
        out << entry.property << ": not converted -- " << entry.reason << '\n';

    out << set.monitors.size() << " converted, " << set.unsupported.size()
        << " not converted\n";
}

void writeJson(const MonitorSet& set, std::ostream& out) {
    out << "{\n \"monitors\": [\n";
    for (std::size_t i = 0; i < set.monitors.size(); i++) {
        const auto& monitor = set.monitors[i];
        out << "  {\"property\": \"" << escape(monitor.property)
            << "\", \"source\": \"" << escape(monitor.sourceExpr)
            << "\", \"safety_only\": "
            << (monitor.isSafetyOnly() ? "true" : "false")
            << ", \"state\": \""
            << escape(frontend::displayName(
                   id2string(monitor.stateSymbol.get_identifier())))
            << "\", \"init\": \"" << escape(asString(monitor.init))
            << "\", \"trans\": \"" << escape(asString(monitor.trans))
            << "\", \"error\": \"" << escape(asString(monitor.errorSignal))
            << "\", \"atoms_over\": [";
        auto symbols = monitor.atomSymbols();
        for (std::size_t s = 0; s < symbols.size(); s++) {
            if (s != 0) out << ", ";
            out << "\""
                << escape(frontend::displayName(
                       id2string(symbols[s].get_identifier())))
                << "\"";
        }
        out << "]}" << (i + 1 == set.monitors.size() ? "\n" : ",\n");
    }
    out << " ],\n \"unsupported\": [\n";
    for (std::size_t i = 0; i < set.unsupported.size(); i++) {
        out << "  {\"property\": \"" << escape(set.unsupported[i].property)
            << "\", \"reason\": \"" << escape(set.unsupported[i].reason)
            << "\"}" << (i + 1 == set.unsupported.size() ? "\n" : ",\n");
    }
    out << " ]\n}\n";
}

}  // namespace sva
}  // namespace veval
