// SPDX-License-Identifier: BSD-3-Clause
// Adapted from veval's product formatting changes to hw-cbmc's expr2verilog.cpp.
// Original hw-cbmc author Daniel Kroening. See frontend/rtl/LICENSE.hw-cbmc.
// Product expressions are reparsed by SystemVerilog tools. Preserve the
// operand widths and grouping of the typed IR when emitting executable source.
#include "ExpressionPrinter.h"

#include <set>
#include <util/arith_tools.h>
#include <util/bitvector_types.h>
#include <verilog/expr2verilog_class.h>
#include <verilog/verilog_types.h>

namespace veval::frontend {
namespace {
using Precedence = verilog_precedencet;

bool selectTarget(const exprt& e) {
    if (e.id() == ID_index || e.id() == ID_verilog_bit_select || e.id() == ID_extractbit)
        return selectTarget(to_binary_expr(e).op0());
    if (e.id() == ID_member) return selectTarget(to_member_expr(e).compound());
    return e.id() == ID_symbol || e.id() == ID_verilog_identifier ||
           e.id() == ID_hierarchical_identifier;
}

std::optional<mp_integer> constantIndex(const exprt& e) {
    if (e.is_constant()) return numeric_cast<mp_integer>(e);
    if (e.id() == ID_unary_minus) {
        if (auto n = constantIndex(to_unary_expr(e).op())) return -*n;
        return {};
    }
    if ((e.id() != ID_plus && e.id() != ID_minus && e.id() != ID_mult) ||
        e.operands().empty()) return {};
    auto value = constantIndex(e.operands().front());
    for (size_t i = 1; value && i < e.operands().size(); ++i) {
        const auto operand = constantIndex(e.operands()[i]);
        if (!operand) return {};
        if (e.id() == ID_plus) *value += *operand;
        else if (e.id() == ID_minus) *value -= *operand;
        else *value *= *operand;
    }
    return value;
}

class ProductPrinter final : public expr2verilogt {
public:
    explicit ProductPrinter(const namespacet& ns, bool description = false)
        : expr2verilogt(ns), description(description) {}

private:
    using Result = expr2verilogt::resultt;
    const bool description;
    const exprt* parent = nullptr;

    static bool needsBrackets(const exprt& outer, const exprt& child, const Result& text) {
        // The shared printer brackets lower precedences. Equal precedence on
        // the right still matters, e.g. a - (b - c), and -- is a different token.
        const auto id = outer.id();
        if (outer.operands().size() == 1) {
            if (id == ID_unary_minus) {
                const auto quote = text.s.find_first_not_of("0123456789_");
                if (quote != 0 && quote != std::string::npos && text.s[quote] == '\'')
                    return true;
            }
            static const std::set<irep_idt> unary{
                ID_not, ID_bitnot, ID_unary_minus, ID_unary_plus, ID_reduction_or,
                ID_reduction_and, ID_reduction_nor, ID_reduction_nand,
                ID_reduction_xor, ID_reduction_xnor};
            return text.p == Precedence::NOT && unary.count(id);
        }
        if (outer.operands().empty() || &child == &outer.operands().front()) return false;
        static const std::map<irep_idt, Precedence> infix{
            {ID_plus, Precedence::ADD}, {ID_minus, Precedence::ADD},
            {ID_mult, Precedence::MULT}, {ID_div, Precedence::MULT}, {ID_mod, Precedence::MULT},
            {ID_power, Precedence::POWER}, {ID_shl, Precedence::SHIFT},
            {ID_lshr, Precedence::SHIFT}, {ID_ashr, Precedence::SHIFT},
            {ID_lt, Precedence::RELATION}, {ID_gt, Precedence::RELATION},
            {ID_le, Precedence::RELATION}, {ID_ge, Precedence::RELATION},
            {ID_equal, Precedence::EQUALITY}, {ID_notequal, Precedence::EQUALITY},
            {ID_verilog_logical_equality, Precedence::EQUALITY}, {ID_verilog_logical_inequality, Precedence::EQUALITY},
            {ID_verilog_case_equality, Precedence::EQUALITY}, {ID_verilog_case_inequality, Precedence::EQUALITY},
            {ID_verilog_wildcard_equality, Precedence::EQUALITY}, {ID_verilog_wildcard_inequality, Precedence::EQUALITY},
            {ID_bitand, Precedence::BITAND}, {ID_bitor, Precedence::BITOR},
            {ID_bitxor, Precedence::XOR}, {ID_bitxnor, Precedence::XOR},
            {ID_xor, Precedence::XOR}, {ID_xnor, Precedence::XOR},
            {ID_and, Precedence::AND}, {ID_or, Precedence::OR},
            {ID_verilog_implies, Precedence::IMPLIES}, {ID_verilog_iff, Precedence::IMPLIES}};
        const auto op = infix.find(id);
        if (op == infix.end() || op->second != text.p) return false;
        static const std::set<irep_idt> associative{
            ID_plus, ID_mult, ID_bitand, ID_bitor, ID_bitxor, ID_xor, ID_and, ID_or};
        return child.id() != id || !associative.count(id);
    }

    Result select(const exprt& source, const exprt& index, size_t width, bool single) {
        const auto offset = constantIndex(index);
        auto first = offset;
        const exprt* base = &source;
        if (!single && first && source.id() == ID_lshr) {
            const auto& shift = to_binary_expr(source);
            if (auto distance = constantIndex(shift.rhs()); distance && *distance >= 0) {
                *first += *distance;
                base = &shift.lhs();
            }
        }
        if (selectTarget(*base)) {
            if (single)
                return {Precedence::MEMBER, convert(*base) + '[' +
                        (first ? integer2string(*first) : convert(index)) + ']'};
            if (first && *first >= 0 &&
                (base->type().id() == ID_bool || can_cast_type<bitvector_typet>(base->type()))) {
                const auto bits = base->type().id() == ID_bool ? 1 : to_bitvector_type(base->type()).get_width();
                if (*first + width <= bits)
                    return {Precedence::MEMBER, convert(*base) + '[' + integer2string(*first + width - 1) +
                            ':' + integer2string(*first) + ']'};
            }
        }
        // Only identifiers can take a select suffix. Select from a computed
        // expression using a logical shift and an unsigned size cast instead.
        auto value = convert_rec(source);
        std::string inner = value.p < Precedence::SHIFT ? '(' + value.s + ')' : value.s;
        if (!offset || *offset != 0) {
            auto amount = convert_rec(index);
            inner += " >> " + (amount.p <= Precedence::SHIFT ? '(' + amount.s + ')' : amount.s);
        }
        if (source.type().id() == ID_signedbv || source.type().id() == ID_verilog_signedbv)
            inner = "$unsigned(" + inner + ')';
        auto result = std::to_string(width) + "'(" + inner + ')';
        return {Precedence::MEMBER, single ? '(' + result + ')' : result};
    }

    Result render(const exprt& e) {
        const auto id = e.id();
        if (description && id == ID_let && e.get_bool("#veval_sva_compact_function"))
            return {Precedence::MAX, "<pure-function-expression>"};
        if (id == ID_constant && e.type().id() == ID_bv && e.type().get(ID_C_verilog_type).empty()) {
            const auto width = to_bv_type(e.type()).get_width();
            const auto value = bvrep2integer(to_constant_expr(e).get_value(), width, false);
            return {Precedence::MAX, std::to_string(width) + "'b" + integer2binary(value, width)};
        }
        if (id == ID_constant && e.type().get(ID_C_verilog_type) != ID_verilog_enum &&
            (e.type().id() == ID_signedbv || e.type().id() == ID_unsignedbv)) {
            const auto width = to_bitvector_type(e.type()).get_width();
            const auto value = numeric_cast_v<mp_integer>(to_constant_expr(e));
            const auto prefix = std::to_string(width) + (e.type().id() == ID_signedbv ? "'s" : "'");
            return {Precedence::MAX, prefix + (value >= 256 ? "h" + integer2string(value, 16)
                                                           : "b" + integer2binary(value, width))};
        }
        if (id == ID_extractbit) {
            const auto& bit = to_extractbit_expr(e);
            return select(bit.src(), bit.index(), 1, true);
        }
        if (id == ID_extractbits) {
            const auto& bits = to_extractbits_expr(e);
            return select(bits.src(), bits.index(), to_bitvector_type(bits.type()).get_width(), false);
        }
        if (id == ID_implies) return convert_sva_binary("implies", to_binary_expr(e));
        if (id == ID_symbol && e.get_bool("#veval_sva_local"))
            return {Precedence::MAX, id2string(e.get("#veval_sva_local_name"))};
        if (id == "#veval_sva_match") {
            auto text = "(" + convert(e.operands().at(0));
            for (const auto& item : e.operands().at(1).operands()) text += ", " + convert(item);
            return {Precedence::MAX, text + ')'};
        }
        if (id == "#veval_sva_assign")
            return {Precedence::MAX, convert(e.operands().at(0)) + " = " + convert(e.operands().at(1))};
        return expr2verilogt::convert_rec(e);
    }

    Result convert_rec(const exprt& e) override {
        const auto* outer = parent;
        parent = &e;
        auto text = render(e);
        parent = outer;
        if (outer && needsBrackets(*outer, e, text))
            return {Precedence::MAX, '(' + text.s + ')'};
        return text;
    }
};
} // namespace

std::string productExpression(const exprt& e, const namespacet& ns) {
    return ProductPrinter(ns).convert(e);
}
std::string describeExpression(const exprt& e, const namespacet& ns) {
    return ProductPrinter(ns, true).convert(e);
}
} // namespace veval::frontend
