#include "CombinationalDefinitions.h"
#include "frontend/rtl/ElaboratedDesign.h"

#include <ebmc/transition_system.h>
#include <util/arith_tools.h>
#include <util/bitvector_types.h>
#include <util/bitvector_expr.h>
#include <util/simplify_expr.h>
#include <util/namespace.h>
#include <algorithm>
#include <set>
#include <optional>

namespace veval::lowering::symbolic {
namespace {

bool scalar(const typet& type) {
    return type.id() == ID_bool || type.id() == ID_unsignedbv || type.id() == ID_signedbv;
}

bool operation(const exprt& expr) {
    const auto id = expr.id();
    if (id == ID_constant) return numeric_cast<mp_integer>(expr).has_value();
    if (!scalar(expr.type())) return false;
    return id == ID_symbol || id == ID_plus || id == ID_minus || id == ID_mult ||
        id == ID_unary_minus || id == ID_div || id == ID_mod ||
        id == ID_shl || id == ID_lshr || id == ID_ashr ||
        id == ID_bitand || id == ID_bitor || id == ID_bitxor || id == ID_bitxnor || id == ID_bitnot ||
        id == ID_reduction_and || id == ID_reduction_nand ||
        id == ID_reduction_or || id == ID_reduction_nor ||
        id == ID_reduction_xor || id == ID_reduction_xnor ||
        id == ID_and || id == ID_or || id == ID_xor || id == ID_not || id == ID_implies ||
        id == ID_equal || id == ID_notequal || id == ID_lt || id == ID_le ||
        id == ID_gt || id == ID_ge || id == ID_if || id == ID_typecast ||
        id == ID_extractbit || id == ID_extractbits || id == ID_concatenation ||
        id == ID_replication || id == ID_member || id == ID_with;
}

const symbol_exprt* rootSymbol(const exprt& expression) {
    const exprt* root = &expression;
    while (root->id() == ID_member) {
        const auto& aggregate = to_member_expr(*root).struct_op();
        if (aggregate.type().id() != ID_struct || !aggregate.type().get_bool(ID_packed)) return nullptr;
        root = &aggregate;
    }
    return root->id() == ID_symbol ? &to_symbol_expr(*root) : nullptr;
}

bool aliasOnly(const exprt& expr) {
    if (expr.id() == ID_symbol) return true;
    if (expr.id() != ID_typecast) return false;
    const auto& op = to_typecast_expr(expr).op();
    return (expr.type().id() == ID_unsignedbv || expr.type().id() == ID_signedbv) &&
        (op.type().id() == ID_unsignedbv || op.type().id() == ID_signedbv) &&
        to_bitvector_type(expr.type()).get_width() == to_bitvector_type(op.type()).get_width() &&
        aliasOnly(op);
}

} // namespace

irep_idt CombinationalDefinitions::key(const irep_idt& symbol) const {
    const auto found = aliases.find(symbol);
    return found == aliases.end() ? symbol : found->second;
}

exprt CombinationalDefinitions::memberKey(exprt expression) const {
    auto* root = &expression;
    while (root->id() == ID_member) root = &to_member_expr(*root).struct_op();
    auto& symbol = to_symbol_expr(*root);
    symbol.set_identifier(key(symbol.get_identifier()));
    return expression;
}

CombinationalDefinitions::CombinationalDefinitions(
    const transition_systemt& design,
    const std::map<irep_idt, irep_idt>& aliases) : design(design), aliases(aliases) {
    std::set<std::string> scopes;
    const auto top = frontend::displayName(id2string(design.main_symbol->name));
    scopes.insert(top);
    for (const auto& [id, symbol] : design.symbol_table.symbols) {
        if (symbol.type.id() == ID_verilog_module_instance && symbol.module == design.main_symbol->name)
            scopes.insert(frontend::displayName(id2string(id)));
        if (symbol.is_state_var) stateClasses.insert(key(id));
    }

    auto record = [&](const exprt& lhs, const exprt& value) {
        const auto* root = rootSymbol(lhs);
        if (!root) return;
        const auto id = root->get_identifier();
        const auto* symbol = design.symbol_table.lookup(id);
        if (!symbol || symbol->is_state_var || symbol->is_macro || symbol->is_property ||
            !scalar(lhs.type()) || value.type() != lhs.type() || !operation(value) ||
            (lhs.id() == ID_symbol && aliasOnly(value))) return;
        auto owner = frontend::displayName(id2string(id));
        do {
            const auto dot = owner.rfind('.');
            if (dot == std::string::npos) return;
            owner.resize(dot);
        } while (!scopes.count(owner));
        owners.emplace(id, owner);
        auto& candidates = lhs.id() == ID_symbol ? definitions[key(id)] : memberDefinitions[memberKey(lhs)];
        for (const auto& old : candidates)
            if (old.symbol == id && *old.value == value) return;
        candidates.push_back({id, &value});
    };

    std::vector<const exprt*> pending{&design.trans_expr.invar()};
    while (!pending.empty()) {
        const auto& expr = *pending.back();
        pending.pop_back();
        if (expr.id() == ID_and) {
            for (const auto& op : expr.operands()) pending.push_back(&op);
        } else if (expr.id() == ID_equal) {
            const auto& equality = to_equal_expr(expr);
            record(equality.lhs(), equality.rhs());
            // A read such as y = fields.hi does not drive fields.hi. Reversing
            // it would add a fictitious driver and a cycle beside the real
            // field assignment. Accept reversed expression equations only
            // when the left side is not itself an assignable signal.
            if (!rootSymbol(equality.lhs())) record(equality.rhs(), equality.lhs());
        }
    }
    for (const auto& [id, symbol] : design.symbol_table.symbols)
        if (!symbol.value.is_nil()) record(symbol.symbol_expr(), symbol.value);
    for (auto& [id, values] : definitions)
        std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) {
            return id2string(a.symbol) < id2string(b.symbol);
        });
}

std::map<irep_idt, exprt> CombinationalDefinitions::expand(
    const std::vector<symbol_exprt>& observations, const std::string& target,
    const std::function<bool(const symbol_exprt&)>& available) const {
    std::map<irep_idt, exprt> result;
    std::set<exprt> visiting;
    std::map<exprt, std::optional<exprt>> cache;
    // Bounds limit expansion work, never authorize an incomplete expression.
    std::size_t remaining = 16384;
    std::size_t outputRemaining = 16384;
    std::function<std::optional<exprt>(const exprt&, std::size_t)> rewrite;
    rewrite = [&](const exprt& expr, std::size_t depth) -> std::optional<exprt> {
        if (!remaining || depth > 128) return {};
        --remaining;
        if (!operation(expr)) return {};
        if (expr.id() != ID_symbol && expr.id() != ID_member) {
            exprt out = expr;
            for (std::size_t i = 0; i < expr.operands().size(); ++i) {
                auto op = rewrite(expr.operands()[i], depth + 1);
                if (!op) return {};
                out.operands()[i] = std::move(*op);
            }
            // Frontend partial assignments use bitvector `with` updates.
            // Write each fixed bit as a mask/insert, preserving width and
            // evaluating the previous value once in the expression DAG.
            if (out.id() == ID_with) {
                if (!can_cast_type<bitvector_typet>(out.type()) || out.operands().size() < 3 ||
                    out.operands().size() % 2 != 1) return {};
                const auto width = to_bitvector_type(out.type()).get_width();
                const unsignedbv_typet bits{width};
                exprt value = typecast_exprt::conditional_cast(out.operands()[0], bits);
                for (std::size_t i = 1; i < out.operands().size(); i += 2) {
                    const auto at = numeric_cast<mp_integer>(out.operands()[i]);
                    const auto& bit = out.operands()[i + 1];
                    if (!at || *at < 0 || *at >= width ||
                        (bit.type().id() != ID_bool &&
                         (!can_cast_type<bitvector_typet>(bit.type()) ||
                          to_bitvector_type(bit.type()).get_width() != 1))) return {};
                    const auto mask = power(2,width) - 1 - power(2,at->to_ulong());
                    auto inserted = typecast_exprt::conditional_cast(
                        typecast_exprt::conditional_cast(bit, unsignedbv_typet{1}), bits);
                    value = bitor_exprt{
                        bitand_exprt{std::move(value), from_integer(mask,bits)},
                        shl_exprt{std::move(inserted), from_integer(*at,unsignedbv_typet{32})}};
                }
                out = typecast_exprt::conditional_cast(std::move(value), expr.type());
                simplify_expr(out, namespacet{design.symbol_table});
            }
            return out;
        }
        const auto* root = rootSymbol(expr);
        if (!root) return {};
        const auto& symbol = *root;
        if (available(symbol)) return expr;
        const auto canonical = key(symbol.get_identifier());
        if (stateClasses.count(canonical)) return {};
        const auto lookup = memberKey(expr);
        const auto found = cache.find(lookup);
        if (found != cache.end()) return found->second;
        if (!visiting.insert(lookup).second) return {};
        std::optional<exprt> answer;
        const std::vector<Definition>* candidates = nullptr;
        if (expr.id() == ID_symbol) {
            const auto found = definitions.find(canonical);
            if (found != definitions.end()) candidates = &found->second;
        } else {
            const auto found = memberDefinitions.find(lookup);
            if (found != memberDefinitions.end()) candidates = &found->second;
        }
        if (candidates) {
            bool conflict = false;
            for (const auto& definition : *candidates) {
                const auto& owner = owners.at(definition.symbol);
                // A definition is usable only in a strict ancestor. In
                // particular, do not expand the output we intend to check.
                if (target.size() <= owner.size() || target.compare(0, owner.size(), owner) != 0 ||
                    target[owner.size()] != '.') continue;
                auto value = rewrite(*definition.value, depth + 1);
                if (!value) { conflict = true; break; }
                if (value->type() != expr.type())
                    value = typecast_exprt{std::move(*value), expr.type()};
                if (answer && *answer != *value) { conflict = true; break; }
                answer = std::move(value);
            }
            if (conflict) answer.reset();
        }
        visiting.erase(lookup);
        cache.emplace(lookup, answer);
        return answer;
    };
    for (const auto& symbol : observations) {
        if (available(symbol)) continue;
        auto value = rewrite(symbol, 0);
        if (!value || *value == symbol) continue;
        // A small shared DAG can print exponentially many nodes (t[n]=t[n-1]+t[n-1]).
        // Count occurrences, not unique nodes, before accepting an expansion.
        auto budget = outputRemaining;
        std::vector<const exprt*> pending{&*value};
        while (!pending.empty() && budget) {
            const auto* node = pending.back();
            pending.pop_back();
            --budget;
            for (const auto& op : node->operands()) pending.push_back(&op);
        }
        if (!pending.empty()) continue;
        outputRemaining = budget;
        result.emplace(symbol.get_identifier(), std::move(*value));
    }
    return result;
}

} // namespace veval::lowering::symbolic
