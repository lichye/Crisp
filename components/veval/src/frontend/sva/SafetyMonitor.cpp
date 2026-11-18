#include "SafetyMonitor.h"

#include <temporal-logic/ltl_to_buechi.h>
#include <trans-word-level/next_symbol.h>
#include <util/arith_tools.h>
#include <util/bitvector_expr.h>
#include <util/mathematical_types.h>

#include <stdexcept>
#include <vector>

namespace veval::sva {
namespace {
[[noreturn]] void unexpected() {
    throw std::runtime_error("unsupported safety automaton encoding for determinization");
}

// ltl_to_buechi emits a flat disjunction, with false for an empty set.
std::vector<exprt> alternatives(const exprt& expression) {
    if (expression.is_false()) return {};
    if (expression.id() == ID_or) return expression.operands();
    return {expression};
}

std::size_t stateNumber(const exprt& equality, const exprt& state, std::size_t count) {
    if (equality.id() != ID_equal || equality.operands().size() != 2 ||
        equality.operands()[0] != state) unexpected();
    const auto value = numeric_cast<std::size_t>(equality.operands()[1]);
    if (!value || *value >= count) unexpected();
    return *value;
}

void requireNoState(const exprt& expression, const irep_idt& identifier) {
    if ((expression.id() == ID_symbol || expression.id() == ID_next_symbol) &&
        expression.get(ID_identifier) == identifier) unexpected();
    for (const auto& operand : expression.operands()) requireNoState(operand, identifier);
}
}

void determinizeSafety(buechi_transt& nfa, message_handlert&) {
    if (!nfa.liveness_signal.is_false()) return;
    if (nfa.state_symbol.type().id() != ID_range) unexpected();
    const auto& range = to_integer_range_type(nfa.state_symbol.type());
    if (range.from() != 0 || range.to() < 0) unexpected();
    const auto identifier = nfa.state_symbol.get_identifier();
    const next_symbol_exprt oldNext{identifier, nfa.state_symbol.type()};
    // Spot reports determinism in the HOA header; no SAT query is needed.
    if (nfa.deterministic && alternatives(nfa.init).size() == 1) return;
    // Only subset encoding needs one bit per NFA state. A deterministic
    // monitor retains its compact range encoding regardless of this limit.
    if (range.to() >= 4096)
        throw std::runtime_error("safety monitor exceeds the 4096-state subset encoding limit");
    const auto count = numeric_cast_v<std::size_t>(range.to()) + 1;
    const unsignedbv_typet bitmapType{count};
    const symbol_exprt active{identifier, bitmapType};
    const next_symbol_exprt next{identifier, bitmapType};

    mp_integer initial = 0;
    for (const auto& choice : alternatives(nfa.init))
        initial = bitwise_or(initial, mp_integer{1} << stateNumber(choice, nfa.state_symbol, count));

    std::vector<std::vector<exprt>> incoming(count);
    for (const auto& edge : alternatives(nfa.trans)) {
        if (edge.id() != ID_and || edge.operands().size() != 3) unexpected();
        const auto from = stateNumber(edge.operands()[0], nfa.state_symbol, count);
        const auto to = stateNumber(edge.operands()[2], oldNext, count);
        requireNoState(edge.operands()[1], identifier);
        incoming[to].push_back(and_exprt{extractbit_exprt{active, from}, edge.operands()[1]});
    }
    std::vector<exprt> updates;
    for (std::size_t state = count; state-- > 0;)
        updates.push_back(typecast_exprt{disjunction(incoming[state]), unsignedbv_typet{1}});

    // Keep the vendored error's timing: it observes the pre-state and this
    // cycle's input, rather than delaying the failure until the next state.
    std::vector<exprt> failures;
    for (const auto& error : alternatives(nfa.error_signal)) {
        if (error.id() != ID_and || error.operands().size() != 2) unexpected();
        const auto from = stateNumber(error.operands()[0], nfa.state_symbol, count);
        requireNoState(error.operands()[1], identifier);
        failures.push_back(and_exprt{extractbit_exprt{active, from}, error.operands()[1]});
    }
    nfa.state_symbol = active;
    nfa.init = equal_exprt{active, from_integer(initial, bitmapType)};
    nfa.trans = equal_exprt{next, concatenation_exprt{updates, bitmapType}};
    nfa.error_signal = disjunction(failures);
}
}
