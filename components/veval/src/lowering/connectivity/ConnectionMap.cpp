#include "util/database/JsonEscape.h"
#include "ConnectionMap.h"

#include "frontend/rtl/ElaboratedDesign.h"

#include <util/arith_tools.h>
#include <util/bitvector_expr.h>

#include <verilog/verilog_bits.h>
#include <verilog/verilog_types.h>
#include <util/bitvector_types.h>
#include <util/format_expr.h>
#include <util/json_irep.h>
#include <util/mathematical_types.h>
#include <util/std_expr.h>
#include <util/symbol_table.h>

#include <ebmc/transition_system.h>

#include <cstdio>
#include <functional>
#include <algorithm>
#include <map>
#include <set>
#include <unordered_set>
#include <ostream>
#include <sstream>

namespace veval {
namespace lowering {

namespace {
std::size_t packedWidth(const typet& t) {
    if (t.id() == ID_bool) return 1;
    return can_cast_type<bitvector_typet>(t) ? to_bitvector_type(t).get_width() : 0;
}

// -1 means unconstrained. Only wiring constructs are inspected: arithmetic
// and arbitrary expressions remain unknown, even if they could be simplified.
std::vector<int> constantBits(const exprt& e) {
    std::vector<int> result(packedWidth(e.type()), -1);
    if (result.empty()) return result;
    if (e.is_constant()) {
        const auto n = numeric_cast<mp_integer>(e);
        if (n) {
            const auto binary = integer2binary(*n, result.size());
            for (std::size_t i=0; i<result.size(); ++i)
                result[i] = binary[result.size()-1-i]-'0';
        }
    } else if (e.id() == ID_typecast && e.operands().size() == 1) {
        const auto& op = e.operands()[0];
        const auto bits = constantBits(op);
        if (bits.empty()) return result;
        // Boolean conversion is nonzero testing, not truncation to bit zero.
        if (e.type().id() == ID_bool && bits.size() > 1) {
            if (std::find(bits.begin(), bits.end(), 1) != bits.end()) result[0]=1;
            else if (std::find(bits.begin(), bits.end(), -1) == bits.end()) result[0]=0;
            return result;
        }
        const int extension = op.type().id() == ID_signedbv ? bits.back() : 0;
        for (std::size_t i=0; i<result.size(); ++i)
            result[i] = i<bits.size() ? bits[i] : extension;
    } else if (e.id() == ID_concatenation) {
        std::size_t offset=0;
        for (auto i=e.operands().rbegin(); i!=e.operands().rend(); ++i) {
            const auto bits = constantBits(*i);
            if (bits.empty()) return std::vector<int>(result.size(), -1);
            for (std::size_t k=0; k<bits.size() && offset+k<result.size(); ++k)
                result[offset+k]=bits[k];
            offset += bits.size();
        }
    } else if (e.id()==ID_replication && e.operands().size()==2) {
        const auto times=numeric_cast<mp_integer>(e.operands()[0]);
        const auto bits=constantBits(e.operands()[1]);
        if (times && *times>0 && !bits.empty() && *times*bits.size()==result.size())
            for (std::size_t i=0; i<result.size(); ++i) result[i]=bits[i%bits.size()];
    } else if ((e.id()==ID_extractbit || e.id()==ID_extractbits) && e.operands().size()==2) {
        const auto at = numeric_cast<mp_integer>(e.operands()[1]);
        const auto bits = constantBits(e.operands()[0]);
        if (at && *at>=0 && *at+result.size()<=bits.size()) {
            const auto low=at->to_ulong();
            std::copy_n(bits.begin()+low, result.size(), result.begin());
        }
    }
    return result;
}
}

exprt ConstantBinding::subject() const {
    if (!slice) return port;
    return extractbits_exprt{port, from_integer(slice->first, integer_typet{}),
                             unsignedbv_typet{slice->second}};
}
exprt ConstantBinding::relation() const { return equal_exprt{subject(), value}; }
std::string ConstantBinding::localName() const {
    auto name=frontend::displayName(id2string(port.get_identifier()));
    name=name.substr(name.find_last_of('.')+1);
    if (slice) name += "["+std::to_string(slice->first+slice->second-1)+":"+
                          std::to_string(slice->first)+"]";
    return name;
}

std::vector<ConstantBinding> constantBindingParts(const PortBinding& binding) {
    if (binding.isOutput) return {};
    // Preserve the original whole-constant expression and printed spelling.
    if (binding.kind == BindingKind::Constant)
        return {{binding.port, binding.parentExpr, {}}};
    const auto width=packedWidth(binding.port.type());
    if (!width) return {};
    const auto value=typecast_exprt::conditional_cast(binding.parentExpr, binding.port.type());
    const auto bits=constantBits(value);
    std::vector<ConstantBinding> result;
    for (std::size_t low=0; low<bits.size();) {
        if (bits[low]<0) { ++low; continue; }
        auto high=low;
        mp_integer number=0, place=1;
        while (high<bits.size() && bits[high]>=0) {
            if (bits[high]) number+=place;
            place*=2; ++high;
        }
        const auto count=high-low;
        result.push_back({binding.port, from_integer(number, unsignedbv_typet{count}),
                          std::make_pair(low,count)});
        low=high;
    }
    return result;
}

namespace {

// Include packed widths and array dimensions when comparing elaborations.
std::string typeDigest(const typet& type) {
    std::ostringstream out;
    out << type.id();

    if (type.id() == ID_unsignedbv || type.id() == ID_signedbv)
        out << '[' << to_bitvector_type(type).get_width() << ']';
    else if (type.id() == ID_array)
        out << '[' << format(to_array_type(type).size()) << ']'
            << typeDigest(to_array_type(type).element_type());

    return out.str();
}

}  // namespace


std::string toString(BindingKind kind) {
    switch (kind) {
        case BindingKind::Whole:
            return "whole";
        case BindingKind::Partial:
            return "partial";
        case BindingKind::Constant:
            return "constant";
        case BindingKind::Computed:
            return "computed";
    }
    return "?";
}

namespace {

void collectSymbols(const exprt& expr, const irep_idt& instance,
                    std::vector<symbol_exprt>& dest) {
    // Preserve order and multiplicity. Memoize only empty shared subtrees to
    // avoid expanding internal update chains that contribute no parent names.
    const auto prefix = id2string(instance) + ".";
    std::unordered_set<const void*> empty;
    struct Frame {
        const exprt* node;
        std::size_t child, firstSymbol;
    };
    std::vector<Frame> pending;
    const auto enter = [&](const exprt& node) {
        if (node.id() == ID_symbol) {
            const auto& symbol = to_symbol_expr(node);
            const auto& name = id2string(symbol.get_identifier());
            if (name.compare(0, prefix.size(), prefix) != 0)
                dest.push_back(symbol);
        } else if (!node.operands().empty() && !empty.count(&node.read())) {
            pending.push_back({&node, 0, dest.size()});
        }
    };
    enter(expr);
    while (!pending.empty()) {
        auto& frame = pending.back();
        const auto& operands = frame.node->operands();
        if (frame.child < operands.size()) {
            const auto& child = operands[frame.child++];
            enter(child);
        } else {
            if (dest.size() == frame.firstSymbol && frame.node->read().ref_count > 1)
                empty.insert(&frame.node->read());
            pending.pop_back();
        }
    }
}

// What does the child port see of the parent's side?
BindingKind classify(const exprt& expr) {
    if (expr.id() == ID_symbol) return BindingKind::Whole;
    if (expr.is_constant()) return BindingKind::Constant;
    if (expr.id() == ID_typecast) return classify(to_typecast_expr(expr).op());

    // Selections and aggregates carry only the bits they name.
    if (expr.id() == ID_extractbit || expr.id() == ID_extractbits ||
        expr.id() == ID_index || expr.id() == ID_member ||
        expr.id() == ID_concatenation || expr.id() == ID_array ||
        expr.id() == ID_vector || expr.id() == ID_struct) {
        for (const auto& op : expr.operands())
            if (classify(op) == BindingKind::Computed)
                return BindingKind::Computed;
        return BindingKind::Partial;
    }

    return BindingKind::Computed;
}

void forEachConjunct(const exprt& expr,
                     const std::function<void(const exprt&)>& f) {
    if (expr.id() == ID_and) {
        for (const auto& op : expr.operands()) forEachConjunct(op, f);
    } else {
        f(expr);
    }
}

std::string asString(const exprt& expr) {
    std::ostringstream out;
    out << format(expr);
    return frontend::displayName(out.str());
}

std::string escape(const std::string& s) {
    return veval::util::jsonEscape(s);
}

}  // namespace

std::vector<symbol_exprt> PortBinding::parentSymbols() const {
    std::vector<symbol_exprt> result;
    // The instance prefix is the port's name minus its last component.
    const auto& id = id2string(port.get_identifier());
    auto dot = id.rfind('.');
    irep_idt instance =
        dot == std::string::npos ? irep_idt{} : irep_idt{id.substr(0, dot)};
    collectSymbols(parentExpr, instance, result);
    return result;
}

std::vector<irep_idt> PortBinding::parentSymbolNames() const {
    const auto& portName = id2string(port.get_identifier());
    const auto dot = portName.rfind('.');
    const auto prefix = dot == std::string::npos ? "." : portName.substr(0, dot + 1);
    std::vector<irep_idt> result;
    std::unordered_set<irep_idt, irep_id_hash> names;
    std::unordered_set<const void*> seen;
    std::vector<const exprt*> pending{&parentExpr};
    while (!pending.empty()) {
        const auto& node = *pending.back();
        pending.pop_back();
        if (node.id() == ID_symbol) {
            const auto& id = node.get(ID_identifier);
            if (id2string(id).compare(0, prefix.size(), prefix) != 0 && names.insert(id).second)
                result.push_back(id);
        } else if (!node.operands().empty() && seen.insert(&node.read()).second) {
            for (auto child = node.operands().rbegin(); child != node.operands().rend(); ++child)
                pending.push_back(&*child);
        }
    }
    return result;
}

std::vector<InstanceEdge> connectionMap(
    const transition_systemt& transitionSystem) {
    const auto& symbolTable = transitionSystem.symbol_table;
    const irep_idt top = transitionSystem.main_symbol->name;
    const auto symbolNames = symbolTable.sorted_symbol_names();

    // Elaboration puts descendant instance symbols in the top module's table.
    std::vector<InstanceEdge> result;
    std::map<std::string, std::string> enclosingScopes;

    for (const auto& identifier : symbolNames) {
        const auto& symbol = symbolTable.lookup_ref(identifier);
        if (symbol.type.id() != ID_verilog_module_instance) continue;
        if (symbol.module != top) continue;

        // Report the original module behind the `<instance>$module` copy.
        auto instanceModule = symbol.value.get(ID_module);

        // The design top itself has no per-instance copy and is not an edge.
        const std::string copySuffix = "$module";
        const auto& instanceModuleId = id2string(instanceModule);
        if (instanceModuleId.size() < copySuffix.size() ||
            instanceModuleId.compare(instanceModuleId.size() -
                                         copySuffix.size(),
                                     copySuffix.size(), copySuffix) != 0)
            continue;
        const auto* moduleSymbol = symbolTable.lookup(instanceModule);
        auto moduleName =
            moduleSymbol != nullptr && !moduleSymbol->pretty_name.empty()
                ? moduleSymbol->pretty_name
                : instanceModule;

        InstanceEdge edge;
        edge.instance = frontend::displayName(id2string(identifier));
        edge.module = frontend::displayName(id2string(moduleName));
        // Strip the complete base_name, including escaped dots. Generate
        // scopes are not module instances.
        const auto suffix = "." + id2string(symbol.base_name);
        if (edge.instance.size() > suffix.size() &&
            edge.instance.compare(edge.instance.size() - suffix.size(), suffix.size(), suffix) == 0)
            enclosingScopes.emplace(edge.instance, edge.instance.substr(0, edge.instance.size() - suffix.size()));
        if (moduleSymbol != nullptr && moduleSymbol->type.id() == ID_module)
            for (const auto& port : to_module_type(moduleSymbol->type).ports()) {
                const auto name = frontend::displayName(id2string(port.identifier()));
                const auto local = name.substr(name.rfind('.') + 1);
                edge.ports.insert(local);
                if (port.input()) edge.inputs.insert(local);
            }

        result.push_back(std::move(edge));
    }

    std::map<std::string, std::size_t> instances;
    for (std::size_t i = 0; i < result.size(); ++i)
        instances.emplace(result[i].instance, i);
    // Prefix parents sort before descendants, regardless of symbol-table order.
    for (const auto& [name, index] : instances) {
        auto scope = enclosingScopes.find(name);
        if (scope == enclosingScopes.end()) continue;
        auto parent = scope->second;
        while (!parent.empty() && instances.count(parent) == 0) {
            const auto dot = parent.rfind('.');
            parent = dot == std::string::npos ? std::string() : parent.substr(0, dot);
        }
        result[index].parentInstance = parent;
        result[index].scope = scope->second == parent ? std::string() : scope->second;
        if (!parent.empty()) result[index].depth = result[instances.at(parent)].depth + 1;
    }

    // Index parameters once, then find each instance's direct members by prefix.
    {
        std::vector<std::string> names;
        names.reserve(symbolTable.symbols.size());
        for (const auto& identifier : symbolNames) {
            const auto& symbol = symbolTable.lookup_ref(identifier);
            if (symbol.type.id() == ID_verilog_module_instance) continue;
            // Localparams and enum names cannot appear in a #(...) override.
            if (!symbol.is_macro || symbol.is_file_local) continue;
            if (!numeric_cast<mp_integer>(symbol.value).has_value()) continue;
            names.push_back(frontend::displayName(id2string(identifier)));
        }
        std::sort(names.begin(), names.end());

        for (auto& edge : result) {
            const auto prefix = edge.instance + '.';
            auto first = std::lower_bound(names.begin(), names.end(), prefix);

            for (auto it = first; it != names.end(); ++it) {
                if (it->compare(0, prefix.size(), prefix) != 0) break;

                auto member = it->substr(prefix.size());
                if (member.find('.') != std::string::npos) continue;

                const auto* symbol =
                    symbolTable.lookup("Verilog::$root." + *it);
                if (symbol == nullptr) continue;

                edge.parameters.emplace_back(
                    member,
                    integer2string(*numeric_cast<mp_integer>(symbol->value)));
            }
        }
    }

    // Route invariant equalities to their owning instances in one pass.
    // Inputs are `port == value`; outputs are `value == port`.
    {
        forEachConjunct(
            transitionSystem.trans_expr.invar(),
            [&](const exprt& conjunct) {
                if (conjunct.id() != ID_equal) return;

                const auto& equality = to_equal_expr(conjunct);

                // Exact owner lookup: a neighbouring sorted key may be a sibling.
                auto owner = [&](const exprt& side) -> std::size_t {
                    if (side.id() != ID_symbol) return result.size();

                    auto shown = frontend::displayName(
                        id2string(to_symbol_expr(side).get_identifier()));

                    auto dot = shown.rfind('.');
                    if (dot == std::string::npos) return result.size();

                    auto found = instances.find(shown.substr(0, dot));
                    return found == instances.end() ? result.size()
                                                     : found->second;
                };

                // Both sides can contribute bindings to different edges.
                auto record = [&](exprt port, exprt parentSide,
                                  bool isOutput) {
                    // A same-width signedness cast is bitwise reversible:
                    // `net == (T_net) port` becomes `port == (T_port) net`.
                    if (port.id() == ID_typecast &&
                        to_typecast_expr(port).op().id() == ID_symbol) {
                        const auto& inner = to_typecast_expr(port).op();
                        auto bits = [](const typet& t) {
                            return t.id() == ID_unsignedbv || t.id() == ID_signedbv;
                        };
                        if (bits(inner.type()) && bits(port.type()) &&
                            to_bitvector_type(inner.type()).get_width() ==
                                to_bitvector_type(port.type()).get_width()) {
                            parentSide = typecast_exprt{parentSide, inner.type()};
                            exprt symbol = inner;
                            port = std::move(symbol);
                        }
                    }
                    auto index = owner(port);
                    if (index == result.size()) return;

                    // A nested instance's own handle is not a port.
                    const auto* portSymbol = symbolTable.lookup(
                        to_symbol_expr(port).get_identifier());
                    if (portSymbol != nullptr &&
                        portSymbol->type.id() == ID_verilog_module_instance)
                        return;

                    result[index].bindings.emplace_back(
                        to_symbol_expr(port), parentSide, isOutput,
                        classify(parentSide));
                };

                record(equality.lhs(), equality.rhs(), false);
                record(equality.rhs(), equality.lhs(), true);
            });
    }

    return result;
}

namespace {

// Relative names and type digests for the entire subtree, not just its ports.
std::set<std::string> signalsUnder(const transition_systemt& system,
                                   const std::string& prefix) {
    std::set<std::string> result;

    for (const auto& identifier : system.symbol_table.sorted_symbol_names()) {
        const auto& symbol = system.symbol_table.lookup_ref(identifier);
        if (symbol.type.id() == ID_verilog_module_instance) continue;

        auto shown = frontend::displayName(id2string(identifier));
        if (shown.compare(0, prefix.size(), prefix) != 0) continue;
        auto entry = shown.substr(prefix.size()) + '\t' + typeDigest(symbol.type);
        // Parameter values may differ without changing any signal names or types.
        if (symbol.is_macro) entry += '\t' + asString(symbol.value);
        result.insert(entry);
    }

    return result;
}

}  // namespace

std::string elaborationMismatch(const transition_systemt& parent,
                                const InstanceEdge& edge,
                                const transition_systemt& child,
                                const std::string& childTop) {
    auto standalone =
        signalsUnder(child, frontend::displayName(childTop) + '.');
    auto instantiated = signalsUnder(parent, edge.instance + '.');

    if (instantiated == standalone) return {};

    // Report a concrete difference from each side, as well as the counts.
    std::string message = "elaborated on its own, `" + edge.module +
                          "' is not the module `" + edge.instance +
                          "' is: it has " + std::to_string(standalone.size()) +
                          " signals against " +
                          std::to_string(instantiated.size()) +
                          ". Almost certainly the standalone elaboration took "
                          "the module's default parameters";

    auto describe = [](const std::string& entry) {
        auto tab = entry.find('\t');
        if (tab == std::string::npos) return entry;
        auto type = entry.substr(tab + 1);
        auto value = type.find('\t');
        if (value != std::string::npos)
            type = type.substr(0, value) + " = " + type.substr(value + 1);
        return entry.substr(0, tab) + "' as " + type + ", `" +
               entry.substr(0, tab);
    };

    for (const auto& signal : instantiated)
        if (standalone.count(signal) == 0) {
            message += "; the instance has `" + describe(signal) +
                       "' and it does not";
            break;
        }

    for (const auto& signal : standalone)
        if (instantiated.count(signal) == 0) {
            message += "; it has `" + describe(signal) +
                       "' and the instance does not";
            break;
        }

    return message;
}

void writeJson(const std::vector<InstanceEdge>& edges, std::ostream& out) {
    out << "{\n \"edges\": [\n";
    for (std::size_t e = 0; e < edges.size(); e++) {
        const auto& edge = edges[e];
        out << "  {\n";
        out << "   \"instance\": \"" << escape(edge.instance) << "\",\n";
        out << "   \"module\": \"" << escape(edge.module) << "\",\n";
        out << "   \"bindings\": [\n";
        for (std::size_t b = 0; b < edge.bindings.size(); b++) {
            const auto& binding = edge.bindings[b];
            out << "    {\"port\": \""
                << escape(frontend::displayName(
                       id2string(binding.port.get_identifier())))
                << "\", \"direction\": \"" << (binding.isOutput ? "out" : "in")
                << "\", \"kind\": \"" << toString(binding.kind)
                << "\", \"parent_expr\": \""
                // Computed expressions stay empty, as in the database writer.
                << (binding.kind == BindingKind::Computed ? std::string{} :
                    escape(asString(binding.parentExpr)))
                << "\", \"parent_symbols\": [";
            auto symbols = binding.parentSymbols();
            for (std::size_t s = 0; s < symbols.size(); s++) {
                if (s != 0) out << ", ";
                out << "\""
                    << escape(frontend::displayName(
                           id2string(symbols[s].get_identifier())))
                    << "\"";
            }
            out << "]";
            if (binding.kind != BindingKind::Constant) {
                const auto facts=constantBindingParts(binding);
                if (!facts.empty()) {
                    out << ", \"constant_facts\": [";
                    for (std::size_t i=0; i<facts.size(); ++i) {
                        if (i) out << ',';
                        out << json_irept(false).convert_from_irep(facts[i].relation());
                    }
                    out << ']';
                }
            }
            out << "}" << (b + 1 == edge.bindings.size() ? "\n" : ",\n");
        }
        out << "   ]\n" << (e + 1 == edges.size() ? "  }\n" : "  },\n");
    }
    out << " ]\n}\n";
}

void writeText(const std::vector<InstanceEdge>& edges, std::ostream& out) {
    std::size_t totals[4] = {0, 0, 0, 0};

    for (const auto& edge : edges) {
        out << edge.instance;
        if (!edge.module.empty()) out << " : " << edge.module;
        out << '\n';

        for (const auto& binding : edge.bindings) {
            totals[static_cast<int>(binding.kind)]++;
            out << "  " << (binding.isOutput ? "out " : "in  ")
                << frontend::displayName(
                       id2string(binding.port.get_identifier()))
                << (binding.isOutput ? "  ->  " : "  <-  ")
                << asString(binding.parentExpr) << "   ["
                << toString(binding.kind) << "]\n";
        }
        out << '\n';
    }

    out << "bindings: " << totals[0] << " whole, " << totals[1] << " partial, "
        << totals[2] << " constant, " << totals[3] << " computed\n";
}

}  // namespace lowering
}  // namespace veval
