#include "LocalEnvironment.h"

#include "frontend/rtl/ElaboratedDesign.h"
#include "util/verilog/ExpressionPrinter.h"
#include "lowering/output/WriteLocalSpecs.h"

#include <util/arith_tools.h>
#include <util/bitvector_types.h>
#include <util/format_expr.h>
#include <util/namespace.h>
#include <util/replace_expr.h>
#include <util/std_expr.h>
#include <util/symbol.h>

#include <ebmc/transition_system.h>
#include <temporal-logic/temporal_logic.h>
#include <trans-word-level/next_symbol.h>
#include <verilog/expr2verilog.h>

#include <algorithm>
#include <functional>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace veval {
namespace environment {

namespace {

std::string display(const irep_idt& id) {
    return frontend::displayName(id2string(id));
}

std::string lastComponent(const std::string& name) {
    auto dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(dot + 1);
}

// Keep full names and a stable rendering in environment fingerprints.
std::string asString(const exprt& expr) {
    std::ostringstream out;
    out << format(expr);
    return out.str();
}

// Initial-state constraints cannot contain temporal operators.
bool statePredicate(const exprt& expr) {
    if (expr.id() == ID_next_symbol) return false;
    if (is_temporal_operator(expr)) return false;
    for (const auto& op : expr.operands())
        if (!statePredicate(op)) return false;
    return true;
}

}  // namespace

// Reset-contract labels give the otherwise always-qualified body cycle-0 meaning.
std::optional<exprt> resetContractStatePredicate(const exprt& body) {
    exprt inner = body;
    if (inner.id() == ID_sva_always || inner.id() == ID_G ||
        inner.id() == ID_AG) {
        if (inner.operands().size() != 1) return std::nullopt;
        inner = to_unary_expr(inner).op();
    }
    if (!statePredicate(inner)) return std::nullopt;
    return inner;
}

namespace {

// Index invariant definitions once instead of scanning them for each input.
using Definitions = std::map<irep_idt, exprt>;

Definitions definitions(const transition_systemt& system) {
    Definitions defined;

    std::function<void(const exprt&)> walk = [&](const exprt& expr) {
        if (expr.id() == ID_and) {
            for (const auto& op : expr.operands()) walk(op);
            return;
        }
        if (expr.id() != ID_equal || expr.operands().size() != 2) return;
        const auto& lhs = to_binary_expr(expr).op0();
        const auto& rhs = to_binary_expr(expr).op1();
        // Prefer a symbol on the left; pure aliases retain their written direction.
        if (lhs.id() == ID_symbol)
            defined.emplace(to_symbol_expr(lhs).get_identifier(), rhs);
        else if (rhs.id() == ID_symbol)
            defined.emplace(to_symbol_expr(rhs).get_identifier(), lhs);
    };

    walk(system.trans_expr.invar());
    return defined;
}

// Follow aliases with a fixed budget to terminate on combinational loops.
exprt resolved(const exprt& expr, const Definitions& defined, int budget) {
    exprt current = expr;
    while (budget-- > 0 && current.id() == ID_symbol) {
        auto found = defined.find(to_symbol_expr(current).get_identifier());
        if (found == defined.end()) return current;
        current = found->second;
    }
    return current;
}

// Strip zero extensions only; sign extension can change comparison semantics.
exprt widened(const exprt& expr) {
    exprt current = expr;
    for (int guard = 0; guard < 8; guard++) {
        if (current.id() == ID_typecast && current.operands().size() == 1) {
            const auto& inner = to_unary_expr(current).op();
            // Only unsigned values can be widened without changing their value.
            if (inner.type().id() != ID_unsignedbv) return current;
            const auto from = to_unsignedbv_type(inner.type()).get_width();
            if (current.type().id() == ID_unsignedbv) {
                if (to_unsignedbv_type(current.type()).get_width() < from)
                    return current;
            } else if (current.type().id() == ID_signedbv) {
                // A signed destination needs an extra bit to preserve the unsigned range.
                if (to_signedbv_type(current.type()).get_width() <= from)
                    return current;
            } else {
                return current;
            }
            current = inner;
            continue;
        }
        if (current.id() == ID_concatenation && current.operands().size() == 2) {
            const auto& high = current.operands()[0];
            const auto value = numeric_cast<mp_integer>(high);
            if (!value.has_value() || *value != 0) return current;
            current = current.operands()[1];
            continue;
        }
        return current;
    }
    return current;
}

// Follow aliases and zero extensions, stopping at the last signal name.
// Do not expand logic: both sides of a clamp must resolve to the same signal.
exprt canonicalSignal(const exprt& expr, const Definitions& defined) {
    exprt current = widened(expr);
    for (int guard = 0; guard < 16; guard++) {
        if (current.id() != ID_symbol) return current;
        auto found = defined.find(to_symbol_expr(current).get_identifier());
        if (found == defined.end()) return current;
        auto next = widened(found->second);
        if (next.id() != ID_symbol) return current;
        current = next;
    }
    return current;
}

bool sameSignal(const exprt& a, const exprt& b, const Definitions& defined) {
    const auto left = canonicalSignal(a, defined);
    const auto right = canonicalSignal(b, defined);
    if (left.id() != ID_symbol || right.id() != ID_symbol) return false;
    return to_symbol_expr(left).get_identifier() ==
           to_symbol_expr(right).get_identifier();
}

// Recognize x > K ? K : x and x < K ? x : K, including >=, <= and
// reversed comparisons. The constant arm must equal K and the other arm
// must name the compared signal. Resolve constants, but leave signal arms
// to canonicalSignal so comparisons stop at the same alias depth.
std::optional<mp_integer> clampBound(const exprt& driver,
                                     const Definitions& defined) {
    const auto expr = resolved(driver, defined, 8);
    if (expr.id() != ID_if || expr.operands().size() != 3) return std::nullopt;

    const auto& condition = resolved(expr.operands()[0], defined, 8);
    const auto& whenTrue = expr.operands()[1];
    const auto& whenFalse = expr.operands()[2];

    if (condition.operands().size() != 2) return std::nullopt;
    auto relation = condition.id();
    exprt signalSide = condition.operands()[0];
    exprt boundSide = condition.operands()[1];

    // `K < x` is equivalent to `x > K`.
    if (numeric_cast<mp_integer>(widened(signalSide)).has_value()) {
        std::swap(signalSide, boundSide);
        if (relation == ID_lt) relation = ID_gt;
        else if (relation == ID_le) relation = ID_ge;
        else if (relation == ID_gt) relation = ID_lt;
        else if (relation == ID_ge) relation = ID_le;
    }

    const auto bound = numeric_cast<mp_integer>(widened(boundSide));
    if (!bound.has_value()) return std::nullopt;

    const exprt* constantArm = nullptr;
    const exprt* signalArm = nullptr;
    if (relation == ID_gt || relation == ID_ge) {
        constantArm = &whenTrue;
        signalArm = &whenFalse;
    } else if (relation == ID_lt || relation == ID_le) {
        constantArm = &whenFalse;
        signalArm = &whenTrue;
    } else {
        return std::nullopt;
    }

    const auto arm =
        numeric_cast<mp_integer>(widened(resolved(*constantArm, defined, 8)));
    if (!arm.has_value() || *arm != *bound) return std::nullopt;
    if (!sameSignal(*signalArm, signalSide, defined)) return std::nullopt;

    return *bound;
}

std::string bareName(const symbol_exprt& symbol) {
    return lastComponent(display(symbol.get_identifier()));
}

// Render premises in the child module's names.
std::string localText(exprt expr, const namespacet& ns) {
    std::function<void(exprt&)> rename = [&](exprt& e) {
        if (e.id() == ID_symbol) {
            auto& symbol = to_symbol_expr(e);
            symbol.set_identifier(bareName(symbol));
            return;
        }
        for (auto& operand : e.operands()) rename(operand);
    };
    rename(expr);
    // Preserve casts because the wrapper compiles this text as an assumption.
    return expr2verilog(frontend::prepareTypedPrint(expr), ns);
}

}  // namespace

std::map<std::string, InstanceEnvironment> environment(
    const std::vector<lowering::PropertyLanding>& transports,
    const std::vector<lowering::InstanceEdge>& edges,
    const transition_systemt& system, const namespacet& ns) {
    std::map<std::string, InstanceEnvironment> byInstance;
    // Sorted, deduplicated digest lines make saved-result matching order independent.
    std::map<std::string, std::set<std::string>> digestLines;
    auto addPremise = [&](const std::string& instance, Premise premise) {
        premise.digestLine = (premise.when == Premise::When::Initial ? "initial: " : "always: ") +
                             asString(premise.constraint);
        digestLines[instance].insert(premise.digestLine);
        byInstance[instance].premises.push_back(std::move(premise));
    };

    // Aggregate rejected reset contracts by label and reason; show at most three sites.
    struct Dropped {
        std::size_t landings = 0;
        std::vector<std::string> examples;
    };
    std::map<std::pair<std::string, std::string>, Dropped> dropped;
    auto drop = [&dropped](const std::string& property,
                           const std::string& reason,
                           const std::string& instance) {
        auto& record = dropped[{property, reason}];
        record.landings++;
        if (record.examples.size() < 3) record.examples.push_back(instance);
    };

    for (const auto& row : transports) {
        if (!row.isResetContract) continue;
        // Do not warn for instances the reset never reached.
        if (row.result == lowering::LoweringCoverage::Blocked || !row.placed) continue;
        // A failed round trip must never introduce an assumption.
        if (!row.roundTrips) {
            drop(row.property,
                 "its round trip failed here, so the sentence that would be "
                 "fed is not the one the boundary wrote",
                 row.instance);
            continue;
        }
        // An environment reset must arrive through inputs, not constrain driven outputs.
        if (!row.isAssumptionHere) {
            drop(row.property,
                 row.isAssumption
                     ? "it names a signal this instance DRIVES, so it is not "
                       "a fact about this instance's environment"
                     : "it is an `assert' over a signal this instance DRIVES; "
                       "the reserved `reset_contract' prefix makes it neither "
                       "an obligation nor a premise",
                 row.instance);
            continue;
        }

        // Temporal bodies, including disable iff, are not initial-state predicates.
        auto predicate = resetContractStatePredicate(row.monitor.sourceProperty);
        if (!predicate.has_value()) {
            drop(row.property,
                 "its body is not a state predicate, so it has no reading at "
                 "the initial instant",
                 row.instance);
            continue;
        }

        Premise premise;
        premise.when = Premise::When::Initial;
        premise.source = Premise::Source::ResetContract;
        premise.constraint = *predicate;
        // Use the predicate's actual signal and polarity, not a guessed reset port.
        premise.shown = "reset asserted at the initial instant, from `" +
                        row.property + "' (predicate: " + localText(*predicate, ns) + ")";
        addPremise(row.instance, std::move(premise));
    }

    for (const auto& [what, record] : dropped) {
        std::cerr << "reset contract `" << what.first
                  << "' fed no initial premise at " << record.landings
                  << " landing(s): " << what.second << " -- ";
        for (std::size_t i = 0; i < record.examples.size(); i++)
            std::cerr << (i == 0 ? "" : ", ") << record.examples[i];
        if (record.landings > record.examples.size())
            std::cerr << ", and " << (record.landings - record.examples.size())
                      << " more";
        std::cerr << "\n";
    }

    const auto defined = definitions(system);

    for (const auto& edge : edges) {
        // Constant connections use the same extraction as generated tie-off statements.
        for (const auto& binding :
             veval::lowering::constantInputBindings(edge, ns)) {
            if (binding.subject().type() != binding.value.type()) continue;
            Premise premise;
            premise.when = Premise::When::Always;
            premise.source = Premise::Source::TieOff;
            premise.constraint = binding.relation();
            premise.shown = binding.localName() + " == " +
                            expr2verilog(binding.value, ns) +
                            " (the design's own tie-off)";
            addPremise(edge.instance, std::move(premise));
        }

        // Recognize bounds imposed by the parent's combinational driver.
        std::set<irep_idt> seen;
        const auto prefix = edge.instance + '.';
        for (const auto& binding : edge.bindings) {
            if (binding.isOutput) continue;
            if (binding.kind != veval::lowering::BindingKind::Whole) continue;

            const auto id = binding.port.get_identifier();
            const auto shown = display(id);
            if (shown.compare(0, prefix.size(), prefix) != 0 ||
                shown.find('.', prefix.size()) != std::string::npos)
                continue;
            if (!seen.insert(id).second) continue;

            const symbolt* symbol = nullptr;
            if (ns.lookup(id, symbol) || !symbol->is_input) continue;
            // Signed ports need a different bound interpretation.
            if (binding.port.type().id() != ID_unsignedbv) continue;

            auto bound = clampBound(binding.parentExpr, defined);
            if (!bound.has_value()) continue;

            const auto width =
                to_unsignedbv_type(binding.port.type()).get_width();
            // Omit bounds already implied by the port width.
            mp_integer widest = 1;
            for (std::size_t bit = 0; bit < width; bit++) widest *= 2;
            widest -= 1;
            if (*bound < 0 || *bound >= widest) continue;

            Premise premise;
            premise.when = Premise::When::Always;
            premise.source = Premise::Source::Clamp;
            const auto limit = from_integer(*bound, binding.port.type());
            premise.constraint =
                binary_relation_exprt{binding.port, ID_le, limit};
            premise.shown = bareName(binding.port) + " <= " +
                            expr2verilog(limit, ns) +
                            " (the parent drives it through a clamp)";
            addPremise(edge.instance, std::move(premise));
        }
    }

    // Transported assumptions also affect saved-result validity, even though
    // they are compiled as monitors rather than environment predicates.
    for (const auto& row : transports) {
        if (!veval::lowering::usableAsAssumption(row)) continue;
        digestLines[row.instance].insert(
            "assumed: " + row.property + " " +
            veval::lowering::propertyFingerprint(row, ns));
    }

    for (const auto& [instance, lines] : digestLines) {
        std::string digest;
        for (const auto& line : lines) digest += line + "\n";
        byInstance[instance].digest = std::move(digest);
    }
    return byInstance;
}

}  // namespace environment
}  // namespace veval
