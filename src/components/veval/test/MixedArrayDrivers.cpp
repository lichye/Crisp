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
#include <stdexcept>

namespace {
exprt element(const exprt& array, unsigned index) {
    const auto& type = to_verilog_array_type(array.type());
    return index_exprt{array, from_integer(index, type.index_type()), type.element_type()};
}

void checkMixed(const transition_systemt& system, std::string top) {
    const namespacet ns(system.symbol_table);
    const auto prefix = "Verilog::$root." + top + ".";
    const auto& lane = ns.lookup(prefix + "lane");
    const auto& input = ns.lookup(prefix + "d");
    if (lane.is_state_var || lane.is_lvalue)
        throw std::runtime_error("mixed array must remain observable as a wire");

    std::map<irep_idt, exprt> definitions;
    std::optional<exprt> head, tail;
    system.trans_expr.invar().visit_pre([&](const exprt& e) {
        if (e.id() != ID_equal) return;
        const auto& eq = to_equal_expr(e);
        if (eq.lhs().id() == ID_symbol)
            definitions.emplace(to_symbol_expr(eq.lhs()).identifier(), eq.rhs());
        if (eq.lhs().id() != ID_index) return;
        const auto& index = to_index_expr(eq.lhs());
        if (index.array().get(ID_identifier) != lane.name) return;
        const auto selected = numeric_cast<unsigned>(simplify_expr(index.index(), ns));
        if (selected == 0) head = eq.rhs();
        if (selected == 1) tail = eq.rhs();
    });
    if (!head || !tail || *head != input.symbol_expr())
        throw std::runtime_error("mixed array lost a visible element driver");
    if (top == "mixed_comb") {
        if (system.trans_expr.trans() != true_exprt{})
            throw std::runtime_error("combinational array introduced state");
        return;
    }

    const auto& next = to_equal_expr(system.trans_expr.trans());
    const auto storageId = next.lhs().get(ID_identifier);
    const auto& storage = ns.lookup(storageId);
    if (!storage.is_state_var || *tail != element(storage.symbol_expr(), 1))
        throw std::runtime_error("registered element does not read private storage");

    // Bind the wire head and previous registered tail independently. The
    // next tail must read today's d, and feed back today's stored tail.
    for (unsigned d = 0; d <= 1; ++d) {
        for (unsigned old = 0; old <= 1; ++old) {
            auto bindings = definitions;
            auto dValue = from_integer(d, to_array_type(lane.type).element_type());
            auto oldValue = from_integer(old, dValue.type());
            bindings[lane.name] = array_exprt{{dValue, oldValue}, to_array_type(lane.type)};
            auto value = element(next.rhs(), 1);
            for (std::size_t i = 0; i <= bindings.size(); ++i) {
                auto replaced = substitute_symbols(bindings, value);
                if (!replaced) break;
                value = std::move(*replaced);
            }
            value = simplify_expr(std::move(value), ns);
            if (value != from_integer(d ^ old, value.type()))
                throw std::runtime_error("wrong registered update: " + format_to_string(value));
        }
    }
    if (system.trans_expr.init() == true_exprt{})
        throw std::runtime_error("initial tail assignment disappeared");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string top = argv[1];
    try {
        veval::frontend::ElaborateOptions options;
        options.top = top;
        options.files = {argv[2]};
        const auto design = veval::frontend::ElaboratedDesign::elaborate(options);
        if (top == "mixed" || top == "mixed_comb")
            checkMixed(design.transitionSystem(), top);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
