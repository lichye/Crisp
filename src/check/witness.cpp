#include "witness.hpp"

#include <ebmc/transition_system.h>
#include <ebmc/witness_lasso.h>
#include <solvers/decision_procedure.h>
#include <temporal-logic/temporal_logic.h>
#include <trans-word-level/instantiate_word_level.h>
#include <util/arith_tools.h>
#include <util/bitvector_types.h>
#include <util/json.h>
#include <util/mathematical_types.h>

#include <cstdlib>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace crisp::check {
namespace {

json_numbert index(std::size_t n) { return json_numbert(std::to_string(n)); }

struct Domain {
    bool boolean;
    mp_integer lower;
    mp_integer upper;

    jsont json() const {
        if (boolean) return json_stringt("bool");
        return json_arrayt{json_numbert(integer2string(lower)), json_numbert(integer2string(upper))};
    }

    jsont value(const exprt& sample, const irep_idt& name) const {
        if (boolean && (sample.is_true() || sample.is_false()))
            return jsont::json_boolean(sample.is_true());
        if (!boolean) {
            const auto number = numeric_cast<mp_integer>(sample);
            if (number && lower <= *number && *number <= upper)
                return json_numbert(integer2string(*number));
        }
        throw std::runtime_error("missing or non-scalar sampled value for " + id2string(name));
    }
};

Domain domain(const typet& type) {
    const auto id = type.id();
    if (id == ID_bool) return {true, 0, 1};
    if (id == ID_range) {
        const auto& range = to_integer_range_type(type);
        if (!range.empty()) return {false, range.from(), range.to()};
    }
    if (id == ID_unsignedbv || id == ID_signedbv) {
        const auto width = to_bitvector_type(type).get_width();
        if (width != 0) {
            if (id == ID_unsignedbv) return {false, 0, power(2, width) - 1};
            auto sign = power(2, width - 1);
            return {false, -sign, sign - 1};
        }
    }
    throw std::runtime_error("unsupported scalar type: " + id2string(id));
}

const exprt& safety_body(const ebmc_propertiest::propertyt& p) {
    const auto& expr = p.normalized_expr;
    if ((expr.id() == ID_AG || expr.id() == ID_G || expr.id() == ID_sva_always) &&
        expr.operands().size() == 1 && !has_temporal_operator(expr.operands().front()))
        return expr.operands().front();
    throw std::runtime_error("expected a compiled safety predicate for " + id2string(p.name));
}

class Export {
    struct Slot {
        exprt symbol;
        bool state;
        Domain type;
    };

    const transition_systemt& system;
    std::vector<Slot> slots;
    std::map<std::pair<irep_idt, irep_idt>, std::size_t> slot_ids;
    std::map<exprt, std::size_t> expressions;
    json_arrayt nodes;

    std::size_t slot(const exprt& expression) {
        const auto name = expression.get(ID_identifier);
        const auto kind = expression.id() == ID_nondet_symbol ? ID_nondet_symbol : ID_symbol;
        const auto key = std::make_pair(kind, name);
        if (auto found = slot_ids.find(key); found != slot_ids.end()) {
            if (slots[found->second].symbol.type() != expression.type())
                throw std::runtime_error("inconsistent type for " + id2string(name));
            return found->second;
        }
        const auto* symbol = kind == ID_symbol ? system.symbol_table.lookup(name) : nullptr;
        if (symbol && symbol->type != expression.type())
            throw std::runtime_error("inconsistent symbol table type for " + id2string(name));
        exprt sampled = kind == ID_symbol ? exprt{symbol_exprt{name, expression.type()}} : expression;
        const auto number = slots.size();
        slots.push_back({std::move(sampled), symbol && symbol->is_state_var, domain(expression.type())});
        slot_ids.emplace(key, number);
        return number;
    }

    std::size_t add(jsont node) {
        const auto n = nodes.size();
        nodes.push_back(std::move(node));
        return n;
    }

    std::size_t encode(const exprt& expr) {
        if (const auto found = expressions.find(expr); found != expressions.end()) return found->second;
        const auto n = encode_new(expr);
        expressions.emplace(expr, n);
        return n;
    }

    std::size_t operation(const char* name, const exprt::operandst& args) {
        json_arrayt expression{json_stringt(name)};
        for (const auto& arg : args) expression.push_back(index(encode(arg)));
        return add(std::move(expression));
    }

    std::size_t encode_new(const exprt& expr) {
        const auto id = expr.id();
        const auto type = domain(expr.type());
        const auto& args = expr.operands();
        if (expr.is_constant()) return add(type.value(expr, "constant"));
        if (id == ID_symbol || id == ID_nondet_symbol || id == ID_next_symbol)
            return add(json_arrayt{json_stringt(id == ID_next_symbol ? "next" : "current"), index(slot(expr))});
        if (id == ID_typecast && args.size() == 1) {
            const auto& source = args.front();
            const auto from = domain(source.type());
            if (expr.type() == source.type() || (!type.boolean && !from.boolean &&
                type.lower <= from.lower && from.upper <= type.upper))
                return encode(source);
            if (type.boolean && !from.boolean) return operation("asBool", args);
            if (!type.boolean && from.boolean && type.lower <= 0 && type.upper >= 1)
                return operation("asInt", args);
        }
        if (type.boolean) {
            if (id == ID_not && args.size() == 1) return operation("not", args);
            if (id == ID_and) return operation("and", args);
            if (id == ID_or) return operation("or", args);
            if (id == ID_xor) return operation("xor", args);
            if (id == ID_implies && args.size() == 2) return operation("implies", args);
            if (args.size() == 2 && args[0].type() == args[1].type()) {
                if (id == ID_equal) return operation("eq", args);
                if (id == ID_notequal)
                    return add(json_arrayt{json_stringt("not"), index(operation("eq", args))});
                if (id == ID_lt) return operation("lt", args);
                if (id == ID_le) return operation("le", args);
                if (id == ID_gt) return operation("lt", {args[1], args[0]});
                if (id == ID_ge) return operation("le", {args[1], args[0]});
            }
        }
        if (id == ID_if && args.size() == 3 && args[0].is_boolean() &&
            args[1].type() == expr.type() && args[2].type() == expr.type())
            return operation("ite", args);
        throw std::runtime_error("unsupported scalar expression: " + id2string(id));
    }

    std::size_t current_predicate(const exprt& expr) {
        if (!expr.is_boolean()) throw std::runtime_error("expected a Boolean frame predicate");
        expr.visit_pre([](const exprt& part) {
            if (part.id() == ID_next_symbol)
                throw std::runtime_error("frame predicate reads the next frame");
        });
        return encode(expr);
    }

    json_objectt property(const ebmc_propertiest::propertyt& p) {
        return json_objectt{{"name", json_stringt(id2string(p.name))},
                            {"source", json_stringt(p.description)},
                            {"root", index(current_predicate(safety_body(p)))}};
    }

public:
    explicit Export(const transition_systemt& system) : system(system) {}

    json_objectt encode(const witness_lasso_viewt& witness) {
        // Even a state not mentioned in the assertion belongs to the closure.
        for (const auto& reg : system.state_variables()) slot(reg);

        json_objectt certificate{
            {"format", json_stringt("crisp-safety-lasso-v1")},
            {"initial", index(current_predicate(witness.transition.init()))},
            {"invariant", index(current_predicate(witness.transition.invar()))},
            {"transition", index(encode(witness.transition.trans()))},
            {"assertion", property(witness.property)},
            {"loopStart", index(witness.loop_start)},
            {"badAt", index(witness.bad_frame)}
        };
        json_arrayt assumptions;
        for (const auto* p : witness.assumptions) assumptions.push_back(property(*p));
        certificate["assumptions"] = std::move(assumptions);
        certificate["nodes"] = std::move(nodes);

        json_arrayt variables, frames;
        for (const auto& entry : slots) {
            const auto id = entry.symbol.get(ID_identifier);
            const auto name = (entry.symbol.id() == ID_nondet_symbol ? "nondet::" : "") + id2string(id);
            variables.push_back(json_objectt{{"name", json_stringt(name)},
                                              {"state", jsont::json_boolean(entry.state)},
                                              {"domain", entry.type.json()}});
        }
        for (std::size_t t = 0; t < witness.frames; ++t) {
            json_arrayt frame;
            for (const auto& entry : slots) {
                const auto sample = witness.solver.get(instantiate(entry.symbol, t, witness.frames));
                frame.push_back(entry.type.value(sample, entry.symbol.get(ID_identifier)));
            }
            frames.push_back(std::move(frame));
        }
        certificate["variables"] = std::move(variables);
        certificate["frames"] = std::move(frames);
        return certificate;
    }
};

} // namespace

bool lasso_json_enabled() {
    const auto* value = std::getenv("CRISP_LASSO_JSON");
    return value && std::string_view(value) == "1";
}

void write_lasso_json(const witness_lasso_viewt& witness, const std::filesystem::path& path) {
    if (witness.frames < 2 || witness.bad_frame >= witness.frames - 1 ||
        witness.loop_start >= witness.frames - 1)
        throw std::runtime_error("no bad frame and nonempty cycle before the closing frame");
    auto certificate = Export(witness.system).encode(witness);
    auto staging = path;
    staging += ".tmp";
    std::ofstream out(staging);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << certificate << '\n';
    out.close();
    std::filesystem::rename(staging, path);
}

} // namespace crisp::check
