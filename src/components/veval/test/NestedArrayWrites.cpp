#include "frontend/rtl/ElaboratedDesign.h"

#include <ebmc/transition_system.h>
#include <util/arith_tools.h>
#include <util/format_expr.h>
#include <util/namespace.h>
#include <util/simplify_expr.h>
#include <util/substitute_symbols.h>
#include <verilog/verilog_types.h>

#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
exprt initialMemory(const typet& type) {
    if (type.id() != ID_array) return from_integer(5, type);
    const auto& array = to_array_type(type);
    return array_of_exprt{initialMemory(array.element_type()), array};
}

exprt element(const exprt& array, unsigned index) {
    return index_exprt{array, from_integer(index, to_verilog_array_type(array.type()).index_type())};
}

// CBMC simplifies concrete array updates, but leaves a packed-word WITH intact.
std::optional<int> wordValue(const exprt& word) {
    if (word.id() != ID_with) return numeric_cast<int>(word);
    const auto& bit = to_with_expr(word);
    const auto old = numeric_cast<int>(bit.old());
    const auto index = numeric_cast<unsigned>(bit.where());
    if (!old || !index || *index >= 4 || bit.new_value().type().id() != ID_bool ||
        !bit.new_value().is_constant()) return {};
    const int mask = 1 << *index;
    return bit.new_value().is_true() ? (*old | mask) : (*old & ~mask);
}

using Bindings = std::map<irep_idt, exprt>;

exprt evaluate(exprt value, const Bindings& bindings, const namespacet& ns) {
    // The temporary wires form an acyclic chain once inputs and old memory are bound.
    for (std::size_t i = 0; i <= bindings.size(); ++i) {
        auto replaced = substitute_symbols(bindings, value);
        if (!replaced) return simplify_expr(std::move(value), ns);
        value = std::move(*replaced);
    }
    throw std::runtime_error("cyclic assignment");
}

void check(const std::string& top, const std::string& file) {
    veval::frontend::ElaborateOptions options;
    options.top = top;
    options.files = {file};
    const auto design = veval::frontend::ElaboratedDesign::elaborate(options);
    const auto& system = design.transitionSystem();
    const namespacet ns(system.symbol_table);
    const std::string prefix = "Verilog::$root." + top + ".";
    const auto& mem = ns.lookup(prefix + "mem");
    const bool matrix = to_array_type(mem.type).element_type().id() == ID_array;
    const bool comb = top == "combinational_bit";

    Bindings bindings;
    system.trans_expr.invar().visit_pre([&](const exprt& eq) {
        if (eq.id() == ID_equal && to_equal_expr(eq).lhs().id() == ID_symbol) {
            const auto& def = to_equal_expr(eq);
            bindings.emplace(to_symbol_expr(def.lhs()).identifier(), def.rhs());
        }
    });
    const auto result = comb ? bindings.at(mem.name) : to_equal_expr(system.trans_expr.trans()).rhs();
    bindings[mem.name] = initialMemory(mem.type);
    auto input = [&](const char* name, unsigned value) {
        const auto* symbol = system.symbol_table.lookup(prefix + name);
        if (symbol) bindings[symbol->name] = from_integer(value, symbol->type);
    };
    input("b", 1);
    input("d", 10);

    // Addresses 4 and 5 also exercise truncated-index aliases at both dimensions.
    for (unsigned a : {0u, 2u, 4u}) {
        input("a", a);
        for (unsigned c : {0u, 3u, 5u}) {
            input("c", c);
            const auto updated = evaluate(result, bindings, ns);
            for (unsigned row = 0; row < 4; ++row) {
                for (unsigned col = 0; col < (matrix ? 4u : 1u); ++col) {
                    auto word = element(updated, row);
                    if (matrix) word = element(word, col);
                    auto expected = comb ? 0 : 5;
                    if (row == a && top == "dynamic_word_bit") expected = 7;
                    if (row == a && comb) expected = 4;
                    if (row == a && matrix && col == (top == "fixed_column" ? 1u : c)) expected = 10;
                    // out_of_bounds_bit must preserve every word.
                    word = simplify_expr(std::move(word), ns);
                    if (wordValue(word) != expected)
                        throw std::runtime_error(top + " at a=" + std::to_string(a) +
                            ", c=" + std::to_string(c) + ", mem[" + std::to_string(row) +
                            "][" + std::to_string(col) + "] expected " + std::to_string(expected) + ", got " + format_to_string(word));
                }
            }
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try { check(argv[1], argv[2]); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
