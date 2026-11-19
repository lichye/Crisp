#include "ExpressionPrinter.h"

#include <util/arith_tools.h>
#include <util/bitvector_types.h>
#include <util/mathematical_types.h>
#include <util/std_expr.h>
#include <verilog/verilog_expr.h>
#include <verilog/verilog_types.h>
#include <util/simplify_expr.h>
#include <util/symbol_table.h>
#include <util/namespace.h>
#include <verilog/expr2verilog.h>
#include <cstdint>
#include <functional>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace veval {
namespace frontend {
std::optional<StaticArrayElement> staticArrayElement(const exprt& expression) {
    if (expression.id() != ID_index || expression.operands().size() != 2 ||
        expression.operands()[0].type().id() != ID_array) return {};
    const auto& array = to_verilog_array_type(expression.operands()[0].type());
    if (!array.is_unpacked()) return {};
    // Only constant expressions are admitted. Symbolic selectors stay refused.
    const auto hasSymbol = [](const exprt& e, const auto& self) -> bool {
        if (e.id() == ID_symbol) return true;
        for (const auto& op : e.operands()) if (self(op, self)) return true;
        return false;
    };
    if (hasSymbol(expression.operands()[1], hasSymbol)) return {};
    symbol_tablet empty;
    namespacet ns(empty);
    const auto folded = simplify_expr(expression.operands()[1], ns);
    auto index = numeric_cast<mp_integer>(folded);
    if (!index || *index < 0 || *index >= array.size_int()) return {};
    StaticArrayElement result{*index, 0, array.offset(), array.size_int(), array.increasing()};
    // Compute in unbounded integers: size 8 cannot fit a three-bit index type.
    result.declaredIndex = result.increasing ? result.offset + *index
        : result.offset + result.size - 1 - *index;
    return result;
}

exprt prepareTypedPrint(exprt expression) {
    if (expression.get_bool("#veval_sva_assignment_boundary")) {
        expression.remove("#veval_sva_assignment_boundary");
        const auto type = expression.type();
        if (can_cast_type<bitvector_typet>(type)) {
            // A removed automatic-function variable was self-determined.
            // Seal its original width before an enclosing cast or arithmetic
            // context can widen the operands of its defining expression.
            auto value = prepareTypedPrint(std::move(expression));
            value = verilog_explicit_size_cast_exprt{
                from_integer(to_bitvector_type(type).get_width(), integer_typet{}), value, type};
            return verilog_explicit_signing_cast_exprt{value, type};
        }
    }
    // Decode before cast rendering introduces printer-only explicit casts.
    const auto element = staticArrayElement(expression);
    // A variable index into an unpacked array declared descending or with an
    // offset: the lowered index is zero-based, the printed select is read
    // against the declared range. `md[i]' over md[3:0] printed as
    // `md[2'b00 + 2'b00 - 2'b01 - i]', another element.
    std::optional<exprt> declaredIndex;
    if (!element && expression.id() == ID_index && expression.operands().size() == 2 &&
        expression.operands()[0].type().id() == ID_array) {
        const auto& array = to_verilog_array_type(expression.operands()[0].type());
        // [0:N-1] (and `[N]') already reads zero-based; leave it as it was.
        if (array.is_unpacked() && (array.offset() != 0 || !array.increasing())) {
            // The lowered index wraps at its own width (`2'b00 - 2'b01 - i');
            // a size cast keeps that before it meets the integer arithmetic.
            exprt lowered = expression.operands()[1];
            if (can_cast_type<bitvector_typet>(lowered.type()))
                lowered = verilog_explicit_size_cast_exprt{
                    from_integer(to_bitvector_type(lowered.type()).get_width(), integer_typet{}),
                    lowered, unsignedbv_typet{to_bitvector_type(lowered.type()).get_width()}};
            const mp_integer offset = array.offset();
            const mp_integer last = offset + array.size_int() - 1;
            declaredIndex = array.increasing()
                ? static_cast<exprt>(plus_exprt{from_integer(offset, integer_typet{}), lowered})
                : static_cast<exprt>(minus_exprt{from_integer(last, integer_typet{}), lowered});
        }
    }
    for (auto& operand : expression.operands())
        operand = prepareTypedPrint(std::move(operand));
    if (expression.id() == ID_concatenation) {
        for (auto& operand : expression.operands()) {
            if (operand.id() != ID_if ||
                !can_cast_type<bitvector_typet>(operand.type())) continue;
            // Yosys' Verilog frontend can size a conditional from the operand
            // inside a branch's narrowing cast. In {sign, c ? 6'b0 : 6'(x32),
            // fraction5}, that moves sign above bit 11 and truncates it. A
            // re-read pure function may have lost its redundant outer cast;
            // retain the typed width at this self-determined concat boundary.
            const auto type = operand.type();
            operand = verilog_explicit_size_cast_exprt{
                from_integer(to_bitvector_type(type).get_width(), integer_typet{}),
                operand, type};
            operand = verilog_explicit_signing_cast_exprt{operand, type};
        }
    }
    if (element) {
        return verilog_bit_select_exprt{expression.operands()[0],
            from_integer(element->declaredIndex, integer_typet{}), expression.type()};
    }
    if (declaredIndex) {
        return verilog_bit_select_exprt{expression.operands()[0],
            prepareTypedPrint(*declaredIndex), expression.type()};
    }
    if (expression.id() != ID_typecast) return expression;
    auto operand = to_typecast_expr(expression).op();
    const auto& target = expression.type();
    const bool sourceBoolean = operand.type().id() == ID_bool;
    if (!can_cast_type<bitvector_typet>(target) ||
        (!sourceBoolean && !can_cast_type<bitvector_typet>(operand.type()))) return expression;
    const auto width = to_bitvector_type(target).get_width();
    const auto sourceWidth = sourceBoolean ? 1 : to_bitvector_type(operand.type()).get_width();
    const bool sourceSigned = operand.type().id() == ID_signedbv;
    if (width != sourceWidth) {
        typet intermediate = sourceSigned
            ? static_cast<typet>(signedbv_typet{width})
            : static_cast<typet>(unsignedbv_typet{width});
        operand = verilog_explicit_size_cast_exprt{
            from_integer(width, integer_typet{}), operand, intermediate};
    }
    if ((target.id() == ID_signedbv) != sourceSigned)
        operand = verilog_explicit_signing_cast_exprt{operand, target};
    return operand;
}

FunctionPrint prepareFunctionPrint(exprt expression, const namespacet& ns) {
    FunctionPrint out;
    auto type = [](const typet& t) {
        if (t.id()==ID_bool) return std::string("logic");
        if (!can_cast_type<bitvector_typet>(t)) throw std::runtime_error("pure function printing requires packed values");
        return std::string("logic ")+(t.id()==ID_signedbv?"signed ":"")+"["+
            std::to_string(to_bitvector_type(t).get_width()-1)+":0]";
    };
    std::function<exprt(exprt)> convert = [&](exprt e) -> exprt {
        if (e.id() != ID_let || !e.get_bool("#veval_sva_compact_function")) {
            for (auto &op : e.operands())
                op = convert(std::move(op));
            return e;
        }
        std::vector<std::pair<symbol_exprt, exprt>> bindings;
        auto body = e;
        while (body.id() == ID_let) {
            const auto &let = to_let_expr(body);
            bindings.emplace_back(let.symbol(), let.value());
            body = let.where();
        }
        std::map<irep_idt, symbol_exprt> inputs;
        // Sampled-value calls are evaluated in the assertion's clocking
        // context, never in a combinational helper function. Other supported
        // sampled functions ($rose/$fell/$stable/$changed) have already been
        // lowered to expressions containing verilog_past at this boundary.
        std::map<exprt, symbol_exprt> sampled;
        std::function<void(const exprt &)> collect = [&](const exprt &n) {
            if (n.id() == ID_verilog_past) {
                sampled.emplace(n, symbol_exprt{"", n.type()});
                return;
            }
            if (n.id() == ID_symbol && !n.get_bool("#veval_sva_function_local"))
                inputs.emplace(n.get(ID_identifier), to_symbol_expr(n));
            for (const auto &op : n.operands())
                collect(op);
        };
        collect(e);
        std::map<irep_idt, symbol_exprt> rename;
        exprt::operandst args;
        std::string ports, decls, statements;
        for (const auto &[id, s] : inputs) {
            auto name = "__cf_arg_" + std::to_string(args.size());
            rename.emplace(id, symbol_exprt{name, s.type()});
            args.push_back(s);
            ports +=
                (ports.empty() ? "" : ", ") + std::string("input ") + type(s.type()) + " " + name;
        }
        for (auto &[sample, parameter] : sampled) {
            // A sampled value depending on a local defined inside this
            // helper cannot be moved outside its lexical scope.
            sample.visit_pre([&](const exprt &v) {
                if (v.id() == ID_symbol)
                    for (const auto &binding : bindings)
                        if (v.get(ID_identifier) == binding.first.identifier())
                            throw std::runtime_error("sampled function argument depends on a function-local binding");
            });
            const auto name = "__cf_arg_" + std::to_string(args.size());
            parameter = symbol_exprt{name, sample.type()};
            args.push_back(convert(sample));
            ports += (ports.empty() ? "" : ", ") + std::string("input ") + type(sample.type()) + " " + name;
        }
        for (std::size_t i = 0; i < bindings.size(); ++i) {
            const auto &s = bindings[i].first;
            auto name = "__cf_value_" + std::to_string(i);
            rename.emplace(s.identifier(), symbol_exprt{name, s.type()});
            decls += "    " + type(s.type()) + " " + name + ";\n";
        }
        std::function<exprt(exprt)> replace = [&](exprt v) -> exprt {
                if (v.id() == ID_verilog_past)
                    return sampled.at(v);
                if (v.id() == ID_symbol) {
                    auto found = rename.find(v.get(ID_identifier));
                    if (found != rename.end())
                        v = found->second;
                }
                for (auto &op : v.operands()) op = replace(std::move(op));
                return v;
        };
        auto print = [&](exprt n) {
            n = replace(std::move(n));
            return expr2verilog(prepareTypedPrint(std::move(n)), ns);
        };
        for (const auto &[s, value] : bindings)
            statements += "    " + id2string(rename.at(s.identifier()).identifier()) + " = " +
                          print(value) + ";\n";
        const auto returned = print(body);
        const auto signature = type(e.type()) + ports + decls + statements + returned;
        std::uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : signature) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        std::ostringstream name;
        name << "__cf_function_" << std::hex << hash;
        const auto definition = "  function automatic " + type(e.type()) + " " + name.str() + "(" +
                                ports + ");\n" + decls + "    begin\n" + statements + "    " +
                                name.str() + " = " + returned + ";\n    end\n  endfunction\n";
        auto found = out.definitions.emplace(name.str(), definition);
        if (!found.second && found.first->second != definition)
            throw std::runtime_error("pure function name collision");
        return exprt{ID_function_call,
                     e.type(),
                     {symbol_exprt{name.str(), typet{ID_code}},
                      exprt{ID_arguments, typet{}, std::move(args)}}};
    };
    out.expression = convert(std::move(expression));
    return out;
}
} // namespace frontend
} // namespace veval
