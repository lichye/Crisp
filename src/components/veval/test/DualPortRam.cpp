#include "frontend/rtl/ElaboratedDesign.h"

#include <ebmc/transition_system.h>
#include <util/arith_tools.h>
#include <util/format_expr.h>
#include <util/namespace.h>
#include <util/simplify_expr.h>
#include <util/substitute_symbols.h>
#include <verilog/verilog_types.h>

#include <array>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using Bindings = std::map<irep_idt, exprt>;

exprt evaluate(exprt value, const Bindings& bindings, bool secondWins,
               const namespacet& ns) {
    for (std::size_t i = 0; i <= bindings.size(); ++i) {
        auto replaced = substitute_symbols(bindings, value);
        if (!replaced) {
            value.visit_pre([&](exprt& expression) {
                if (expression.id() == ID_nondet_symbol &&
                    expression.get(ID_identifier) == "Verilog::$root.dual_port_ram.mem::write_order")
                    expression = secondWins ? exprt{true_exprt{}} : exprt{false_exprt{}};
            });
            return simplify_expr(std::move(value), ns);
        }
        value = std::move(*replaced);
    }
    throw std::runtime_error("cyclic assignment");
}

void check(const std::string& file) {
    veval::frontend::ElaborateOptions options;
    options.top = "dual_port_ram";
    options.files = {file};
    const auto design = veval::frontend::ElaboratedDesign::elaborate(options);
    const auto& system = design.transitionSystem();
    const namespacet ns(system.symbol_table);
    const std::string prefix = "Verilog::$root.dual_port_ram.";
    const auto& memory = ns.lookup(prefix + "mem");
    const auto& type = to_verilog_array_type(memory.type);
    const auto& word = type.element_type();

    Bindings values, updates;
    system.trans_expr.invar().visit_pre([&](const exprt& expression) {
        if (expression.id() != ID_equal) return;
        const auto& eq = to_equal_expr(expression);
        if (eq.lhs().id() == ID_symbol)
            values.emplace(to_symbol_expr(eq.lhs()).identifier(), eq.rhs());
    });
    system.trans_expr.trans().visit_pre([&](const exprt& expression) {
        if (expression.id() != ID_equal) return;
        const auto& eq = to_equal_expr(expression);
        if (eq.lhs().id() == ID_next_symbol)
            updates.emplace(eq.lhs().get(ID_identifier), eq.rhs());
    });

    const std::array<unsigned, 2> before{1, 2};
    values[memory.name] = array_exprt{{from_integer(before[0], word),
                                      from_integer(before[1], word)}, type};
    const auto input = [&](const char* name, unsigned value) {
        const auto& symbol = ns.lookup(prefix + name);
        values[symbol.name] = from_integer(value, symbol.type);
    };
    input("da", 0);
    input("db", 3);

    for (unsigned a : {0u, 1u}) {
        for (unsigned b : {0u, 1u}) {
            input("a", a);
            input("b", b);
            for (bool ena : {false, true}) {
                for (bool enb : {false, true}) {
                    input("ena", ena);
                    input("enb", enb);
                    for (bool secondWins : {false, true}) {
                        auto expected = before;
                        if (ena) expected[a] = 0;
                        if (enb && (!ena || a != b || secondWins)) expected[b] = 3;
                        for (unsigned address : {0u, 1u}) {
                            const auto observed = evaluate(index_exprt{updates.at(memory.name),
                                from_integer(address, type.index_type()), word}, values, secondWins, ns);
                            if (observed != from_integer(expected[address], word))
                                throw std::runtime_error("incorrect RAM word: " + format_to_string(observed));
                        }
                        const auto read = evaluate(updates.at(prefix + "read_word"), values, secondWins, ns);
                        if (read != from_integer(before[a], word))
                            throw std::runtime_error("the read did not sample the pre-clock word");
                    }
                }
            }
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try { check(argv[1]); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
