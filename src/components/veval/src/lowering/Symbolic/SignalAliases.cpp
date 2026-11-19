#include "SignalAliases.h"

#include <ebmc/transition_system.h>
#include <util/arith_tools.h>
#include <util/bitvector_expr.h>
#include <util/std_expr.h>
#include <util/symbol_table.h>
#include <verilog/verilog_bits.h>

#include <algorithm>
#include <functional>
#include <vector>

namespace veval {
namespace lowering {
namespace {

void forEachConjunct(const exprt& expr,
                     const std::function<void(const exprt&)>& f) {
    if (expr.id() == ID_and) {
        for (const auto& op : expr.operands()) forEachConjunct(op, f);
    } else {
        f(expr);
    }
}

}  // namespace

std::map<irep_idt, irep_idt> signalAliases(
    const transition_systemt& transitionSystem) {
    // Union-find over symbol names, with the representative chosen by name so
    // that the answer does not depend on the order the constraints happen to
    // be in.
    std::map<irep_idt, irep_idt> parent;

    std::function<irep_idt(const irep_idt&)> find =
        [&](const irep_idt& name) -> irep_idt {
        auto it = parent.find(name);
        if (it == parent.end() || it->second == name) return name;
        auto root = find(it->second);
        it->second = root;
        return root;
    };

    auto join = [&](const irep_idt& a, const irep_idt& b) {
        auto ra = find(a), rb = find(b);
        if (ra == rb) return;
        // Smaller name wins, so the representative is stable.
        if (id2string(rb) < id2string(ra)) std::swap(ra, rb);
        parent[rb] = ra;
        parent.emplace(ra, ra);
    };

    // `assign a = b' between two signals of the same width but different
    // types -- a packed struct and the bit vector it is laid out as, say --
    // reaches here as an equality with a typecast on one side. It is still
    // one net under two names, so look through the cast. Requiring the types
    // to match exactly costs real transports: PLENA's TileLink A channel
    // crosses the boundary as a vector and is given the channel's struct
    // shape to name its fields, and a property about `opcode' then reached
    // nothing at all.
    // The single signal an expression denotes, bit for bit, or nothing.
    //
    // Three shapes count, and all three are ordinary RTL. A symbol. A
    // typecast of one, which is how `assign a = b' between two same-width
    // types arrives. And an aggregate built out of slices of ONE signal that
    // tile it completely -- which is what reinterpreting a bus as a packed
    // struct becomes:
    //
    //     top.view == '{ hi: top.done[7:4], lo: top.done[3:0] }
    //
    // A specification that names a channel's fields has to go through that
    // last one, and without it such a property reaches nothing at all.
    std::function<const symbol_exprt*(const exprt&)> as_symbol =
        [&](const exprt& side) -> const symbol_exprt* {
        if (side.id() == ID_symbol)
            return &to_symbol_expr(side);

        if (side.id() == ID_typecast) {
            auto& op = to_typecast_expr(side).op();
            auto outer = verilog_bits_opt(side.type());
            auto inner = verilog_bits_opt(op.type());
            if (outer.has_value() && inner.has_value() && *outer == *inner)
                return as_symbol(op);
            return nullptr;
        }

        if (side.id() != ID_struct && side.id() != ID_concatenation)
            return nullptr;
        if (side.operands().empty())
            return nullptr;

        // Every part must be a slice of the same signal, and together they
        // must tile it IN ORDER: most significant first, no gap, no overlap,
        // no permutation. Anything less and the aggregate is a new value.
        //
        // THE ORDER IS THE WHOLE OF IT, and leaving it out was a soundness
        // bug rather than a missed transport. `{instr[3:0], instr[7:4]}' is
        // built from every bit of `instr' exactly once and is NOT `instr':
        // it is `instr' with its halves exchanged. Accepted as a rename, a
        // boundary assumption `instr[3:0] == 5' was planted at the child as
        // `bundle[3:0] == 5' -- a sentence about the wrong four bits, with
        // `path: ["whole"]' and a round trip that passed, because the round
        // trip substitutes the same wrong alias back. A wrong assumption is
        // the one error that makes proofs pass.
        //
        // The shape this exists for is unaffected: reinterpreting a bus as a
        // packed struct lays the fields out in order, which is exactly the
        // case the check admits.
        const symbol_exprt* source = nullptr;
        std::vector<std::pair<mp_integer, mp_integer>> parts;  // (index, width)

        for (const auto& part : side.operands()) {
            if (part.id() != ID_extractbits)
                return nullptr;

            const auto& extract = to_extractbits_expr(part);
            if (extract.src().id() != ID_symbol)
                return nullptr;

            const auto& symbol = to_symbol_expr(extract.src());
            if (source == nullptr)
                source = &symbol;
            else if (source->get_identifier() != symbol.get_identifier())
                return nullptr;

            auto part_width = verilog_bits_opt(part.type());
            if (!part_width.has_value())
                return nullptr;
            // A part whose offset is not a constant cannot be placed, so the
            // aggregate cannot be shown to be the signal.
            const auto index = numeric_cast<mp_integer>(extract.index());
            if (!index.has_value() || *index < 0)
                return nullptr;
            parts.emplace_back(*index, *part_width);
        }

        auto whole = verilog_bits_opt(source->type());
        if (!whole.has_value())
            return nullptr;

        // Operands run most significant first, so the first part must end at
        // the top bit and each next part must start one below the last.
        mp_integer expected = *whole - 1;
        for (const auto& [index, width] : parts) {
            if (index + width - 1 != expected)
                return nullptr;
            expected = index - 1;
        }
        if (expected != -1)
            return nullptr;

        return source;
    };

    // Widths through verilog_bits, not just bitvector_typet: a packed struct
    // is a bit vector underneath, and a specification that names a channel's
    // fields has to go through one. Refuse anything whose width the frontend
    // itself will not state -- an unpacked array, say -- rather than guess.
    auto same_width = [](const typet& a, const typet& b) {
        auto wa = verilog_bits_opt(a), wb = verilog_bits_opt(b);
        return wa.has_value() && wb.has_value() && *wa == *wb && *wa != 0;
    };

    forEachConjunct(
        transitionSystem.trans_expr.invar(), [&](const exprt& conjunct) {
            if (conjunct.id() != ID_equal) return;
            const auto& equality = to_equal_expr(conjunct);

            const auto* lhs = as_symbol(equality.lhs());
            const auto* rhs = as_symbol(equality.rhs());
            if (lhs == nullptr || rhs == nullptr) return;
            if (!same_width(lhs->type(), rhs->type())) return;

            join(lhs->get_identifier(), rhs->get_identifier());
        });

    // A wire's definition is not always among the constraints. The frontend
    // sometimes emits `wire == expression' and sometimes inlines the
    // expression into the wire's uses and emits nothing, and which of the two
    // it does is not something a caller can predict. The definition is in the
    // symbol table either way, so read it from there as well.
    //
    // Without this a specification that goes through an intermediate signal
    // -- which is how it gets to write `a.opcode' rather than count bits --
    // can reach nothing at all, and the diagnostic says "does not reach this
    // instance", which is true and unhelpful.
    for (const auto& identifier :
         transitionSystem.symbol_table.sorted_symbol_names()) {
        const auto& symbol =
            transitionSystem.symbol_table.lookup_ref(identifier);

        if (symbol.is_macro) continue;       // a parameter, not a signal
        if (symbol.is_state_var) continue;   // a register, not a rename
        if (symbol.value.is_nil()) continue;

        const auto* denoted = as_symbol(symbol.value);
        if (denoted == nullptr) continue;

        auto width = verilog_bits_opt(symbol.type);
        auto other = verilog_bits_opt(denoted->type());
        if (!width.has_value() || !other.has_value() || *width != *other)
            continue;

        join(identifier, denoted->get_identifier());
    }

    std::map<irep_idt, irep_idt> result;
    for (const auto& entry : parent) result.emplace(entry.first, find(entry.first));
    return result;
}

}  // namespace lowering
}  // namespace veval
