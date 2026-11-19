#include "PropertySubstitution.h"

#include <util/replace_expr.h>
#include <trans-word-level/next_symbol.h>

namespace veval {
namespace lowering {
namespace symbolic {

namespace {
exprt nextFrame(const exprt& expression) {
    if (expression.id() == ID_symbol)
        return next_symbol_exprt{to_symbol_expr(expression)};
    exprt result = expression;
    for (std::size_t i = 0; i < expression.operands().size(); ++i)
        result.operands()[i] = nextFrame(expression.operands()[i]);
    return result;
}
}

sva::Monitor substituteAtoms(const sva::Monitor& monitor,
                        const std::map<irep_idt, exprt>& substitution) {
    replace_mapt replaceMap;
    for (const auto& entry : substitution) {
        // Both the current-state and next-state forms of a signal have to be
        // substituted, or a renamed atom would still refer to the parent one
        // timeframe out.
        const auto& replacement = entry.second;
        symbol_exprt current{entry.first, replacement.type()};
        replaceMap.insert({current, replacement});

        next_symbol_exprt next{entry.first, replacement.type()};
        replaceMap.insert({next, nextFrame(replacement)});
    }

    sva::Monitor result = monitor;
    replace_expr(replaceMap, result.init);
    replace_expr(replaceMap, result.trans);
    replace_expr(replaceMap, result.errorSignal);
    replace_expr(replaceMap, result.livenessSignal);
    // The printable form travels with the automaton, under the SAME map --
    // so the SVA the writer emits and the monitor the prover splices can
    // never have seen different substitutions.
    replace_expr(replaceMap, result.sourceProperty);
    replace_expr(replaceMap, result.originalProperty);
    result.refreshAbortAtoms();
    return result;
}

void simplifyComplements(sva::Monitor& monitor) {
    for (exprt* part : {&monitor.init, &monitor.trans, &monitor.errorSignal,
                        &monitor.livenessSignal, &monitor.sourceProperty,
                        &monitor.originalProperty})
        simplifyComplements(*part);
}

// Complements are simplified only for a route that carried a signal as a
// complement, so every other comparison is the one it always was.
void normalizeForComparison(sva::Monitor& monitor, bool complements) {
    for (exprt* part : {&monitor.errorSignal, &monitor.trans, &monitor.init,
                        &monitor.livenessSignal}) {
        if (complements) simplifyComplements(*part);
        normalizeContiguousReads(*part);
        dropWidthPreservingCasts(*part);
    }
}

bool sameMonitor(const sva::Monitor& a, const sva::Monitor& b,
                 bool complements) {
    auto x = a, y = b;
    normalizeForComparison(x, complements);
    normalizeForComparison(y, complements);
    return x.errorSignal == y.errorSignal && x.trans == y.trans && x.init == y.init &&
        x.livenessSignal == y.livenessSignal;
}

// Did this edge carry some signal as the complement of a port?
bool usesComplement(const std::map<irep_idt, exprt>& substitution) {
    for (const auto& entry : substitution) {
        const exprt* target = &entry.second;
        while (target->id() == ID_typecast)
            target = &to_typecast_expr(*target).op();
        if (target->id() == ID_bitnot || target->id() == ID_not) return true;
    }
    return false;
}

// Undo one edge: put the parent's names back for the child's ports and
// reverse every claimed rebase. Nothing when it cannot be undone.
std::optional<sva::Monitor> undoHop(
    const sva::Monitor& landed, const std::map<irep_idt, exprt>& substitution,
    const std::vector<ClaimedRebase>& rebased) {
    std::map<irep_idt, irep_idt> reverse;
    // child port -> the parent signal that was carried as its complement
    std::map<irep_idt, irep_idt> complemented;
    for (const auto& entry : substitution) {
        const exprt* target = &entry.second;
        while (target->id() == ID_typecast)
            target = &to_typecast_expr(*target).op();
        if (target->id() == ID_bitnot || target->id() == ID_not) {
            const exprt* inner = &to_unary_expr(*target).op();
            while (inner->id() == ID_typecast)
                inner = &to_typecast_expr(*inner).op();
            if (inner->id() != ID_symbol) return std::nullopt;
            complemented.emplace(to_symbol_expr(*inner).get_identifier(),
                                 entry.first);
            continue;
        }
        if (target->id() != ID_symbol) return std::nullopt;
        reverse.emplace(to_symbol_expr(*target).get_identifier(), entry.first);
    }
    auto back = landed;
    for (exprt* part : {&back.init, &back.trans, &back.errorSignal, &back.livenessSignal})
        renameAtoms(*part, reverse);
    // A port that stands for a complemented signal goes back as the
    // complement of that signal; the comparison cancels the double
    // complement. A port also renamed above is no longer a port here, so a
    // landing that used one port both ways does not round-trip.
    if (!complemented.empty())
        for (exprt* part : {&back.init, &back.trans, &back.errorSignal, &back.livenessSignal})
            complementAtoms(*part, complemented);

    // Indexed by port, and undone in one pass: a whole-member concatenation
    // lands several signals on the same port, and a pass per signal would
    // meet the others' selects and call a sound transport unrecovered.
    RebasesByPort byPort;
    for (const auto& one : rebased)
        byPort[one.port.get_identifier()].push_back(&one);
    if (!byPort.empty() &&
        !(unrebase(back.init, byPort) && unrebase(back.trans, byPort) &&
          unrebase(back.errorSignal, byPort) && unrebase(back.livenessSignal, byPort)))
        return std::nullopt;
    return back;
}

// The only check there is that a landing says what it should. Everything else
// counts landings, and a count is indifferent to whether a bit index was
// rebased onto the wrong half of a port -- that failure produces a property
// about the child which is perfectly provable and about the wrong thing.
//
// One edge: undo it and compare with what arrived at the parent.
bool roundTripsBack(const sva::Monitor& arrived,
                    const sva::Monitor& landed,
                    const std::map<irep_idt, exprt>& substitution,
                    const std::vector<ClaimedRebase>& rebased) {
    const auto back = undoHop(landed, substitution, rebased);
    if (!back) return false;
    if (landed.abortDropped && !arrived.abortDropped) {
        const auto body = sva::withoutAbort(arrived);
        return body && sameMonitor(*body, *back, usesComplement(substitution));
    }
    return sameMonitor(arrived, *back, usesComplement(substitution));
}


}  // namespace symbolic
}  // namespace lowering
}  // namespace veval
