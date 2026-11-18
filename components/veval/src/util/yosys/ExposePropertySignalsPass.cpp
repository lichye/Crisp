// Read-only observation routing in the live elaborated design. Only selected
// bindings leave this pass; whole-design comparison belongs in development tests.
#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/celltypes.h"
#include "libs/json11/json11.hpp"
#include <fstream>
#include <memory>
#include <regex>
#include <set>

USING_YOSYS_NAMESPACE
#include "YosysApiCompatibility.h"
PRIVATE_NAMESPACE_BEGIN
using Json = json11::Json;

void need(bool ok, const std::string &why) {
    if (!ok) throw std::runtime_error(why);
}
std::string name(IdString id) { return veval_yosys::unescaped(id); }
std::string text(const Json &j) {
    return j.is_string() ? j.string_value() : j.dump();
}
bool boundary(char c) {
    return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '.');
}
bool matches(const std::string &body, size_t pos, const std::string &reference) {
    auto end = pos + reference.size();
    return body.compare(pos, reference.size(), reference) == 0 &&
        (end == body.size() || boundary(body[end]));
}

struct Edge { Module *parent; Cell *cell; Module *child; };
struct Location {
    Module *module;
    Wire *wire;
    RTLIL::Memory *memory;
    std::vector<Edge> chain;
};

// Dots may belong to an exact cell or wire name. Enumerate complete bindings,
// rather than guessing by basename or greedily splitting the hierarchy path.
void resolve(Design *design, Module *module, const std::string &path,
             std::vector<Edge> chain, std::vector<Location> &found) {
    need(chain.size() < 256, "recursive/deep observation hierarchy");
    auto id = veval_yosys::identifier(path);
    auto memory = module->memories.find(id);
    if (auto wire = module->wire(id)) found.push_back({module, wire, nullptr, chain});
    if (memory != module->memories.end()) found.push_back({module, nullptr, memory->second, chain});
    for (size_t pos = path.find('.'); pos != std::string::npos; pos = path.find('.', pos + 1)) {
        auto cell = module->cell(veval_yosys::identifier(path.substr(0, pos)));
        if (!cell) continue;
        auto child = design->module(cell->type);
        if (!child) continue;
        auto next = chain;
        next.push_back({module, cell, child});
        resolve(design, child, path.substr(pos + 1), std::move(next), found);
    }
}
Location locate(Design *design, Module *root, const std::string &path) {
    std::vector<Location> found;
    resolve(design, root, path, {}, found);
    need(found.size() == 1, "observation path has " + std::to_string(found.size()) +
         " complete actual cell-key bindings: " + path);
    return found.front();
}

struct Drivers {
    SigMap aliases;
    pool<SigBit> bits;
    Drivers(Module *module, const CellTypes &types) : aliases(module) {
        for (auto wire : module->wires())
            if (wire->port_input) for (auto bit : aliases(wire)) bits.insert(bit);
        for (auto cell : module->cells())
            for (auto &port : cell->connections())
                if (types.cell_output(cell->type, port.first))
                    for (auto bit : aliases(port.second)) bits.insert(bit);
    }
    bool driven(SigSpec signal) {
        for (auto bit : aliases(signal)) {
            if (bit.wire ? !bits.count(bit) : (bit.data != State::S0 && bit.data != State::S1)) return false;
        }
        return true;
    }
};

// Compare decimal metadata at the parameter's actual width, including signed
// and wider-than-host-word values. No host integer narrowing is involved.
bool sameParameter(const Const &actual, std::string decimal) {
    if (!actual.is_fully_def() || !std::regex_match(decimal, std::regex("-?[0-9]+"))) return false;
    bool negative = decimal[0] == '-';
    if (negative) decimal.erase(0, 1);
    std::vector<State> bits;
    for (int i = 0; i < GetSize(actual); ++i) {
        int remainder = 0;
        for (auto &digit : decimal) {
            int n = remainder * 10 + digit - '0';
            digit = char('0' + n / 2);
            remainder = n % 2;
        }
        bits.push_back(remainder ? State::S1 : State::S0);
    }
    if (negative) {
        bool carry = true;
        for (auto &bit : bits) {
            bool n = bit == State::S0;
            bit = n != carry ? State::S1 : State::S0;
            carry = carry && n;
        }
    }
    return actual == Const(bits);
}

struct Observation : public Pass {
    Observation() : Pass("consflow_observe", "route selected read-only observations without netlist snapshots") {}
    void help() override { log("    consflow_observe REQUEST.json BINDINGS.json\n"); }
    void execute(std::vector<std::string> args, Design *design) override {
        veval_yosys::Scope scope(design);
        if (args.size() != 3) log_cmd_error("Expected a small request and binding output path.\n");
        try {
            // Custom passes receive quoted filename tokens verbatim. Decode
            // the string spelling emitted by native launchers; raw paths from
            // the historical launcher remain accepted.
            for (size_t i : {size_t(1),size_t(2)}) if (!args[i].empty() && args[i][0]=='"') {
                std::string error;
                const auto value=Json::parse(args[i],error);
                need(error.empty() && value.is_string(),"invalid quoted observation path");
                args[i]=value.string_value();
            }
            std::ifstream input(args[1]);
            need(bool(input), "cannot read observation request");
            std::string source{std::istreambuf_iterator<char>(input), {}}, error;
            auto request = Json::parse(source, error);
            need(error.empty(), "invalid observation request: " + error);
            auto rootName = text(request["module"]), instance = text(request["instance"]);
            auto root = design->module(veval_yosys::identifier(rootName));
            need(root != nullptr, "observation root module is absent");
            const auto &observations = request["observations"].array_items();
            std::set<size_t> selected;
            // Select exactly the references consumed by this assertion and its
            // assumptions. Dynamic or unrecorded elements remain unsupported.
            for (const auto &body : request["bodies"].array_items()) {
                const auto sentence = text(body["body"]);
                for (size_t pos = sentence.find("u_dut."); pos != std::string::npos;
                     pos = sentence.find("u_dut.", pos)) {
                    if (pos && !boundary(sentence[pos - 1])) { pos += 6; continue; }
                    std::vector<size_t> found;
                    for (size_t i = 0; i < observations.size(); ++i)
                        if (matches(sentence, pos, text(observations[i]["reference"]))) found.push_back(i);
                    need(found.size() == 1, "internal reference has no unique typed observation: " + sentence.substr(pos, 100));
                    selected.insert(found[0]);
                    pos += text(observations[found[0]]["reference"]).size();
                }
            }
            dict<Module*, int> incoming;
            for (auto module : design->modules()) for (auto cell : module->cells())
                if (auto child = design->module(cell->type)) incoming[child]++;
            auto checkChain = [&](const Location &loc) {
                for (auto edge : loc.chain)
                    need(incoming.at(edge.child) == 1, "shared observation module unsupported: " + name(edge.child->name));
            };
            // Map only selected memories, once, in the existing design. Native
            // memory_map preserves RTL semantics and supplies exact element wires.
            RTLIL::Selection memories(false, false, design);
            Json::array conversions;
            std::set<std::pair<IdString, IdString>> mapped;
            for (auto i : selected) {
                const auto &item = observations[i];
                const auto relative = text(item["relative"]);
                const bool array = text(item["schema"]) == "consflow-observation/2";
                need(array || text(item["schema"]) == "consflow-observation/1", "unsupported observation schema");
                need(text(item["module"]) == rootName && text(item["instance"]) == instance &&
                     text(item["reference"]) == "u_dut." + relative, "observation identity mismatch");
                const auto base = array ? text(item["base_relative"]) : relative;
                need(std::regex_match(base, std::regex("([A-Za-z_][A-Za-z_0-9$]*(\\[[0-9]+\\])?\\.)*[A-Za-z_][A-Za-z_0-9$]*")),
                     "unsupported observation path: " + base);
                need(text(item["symbol"]) == "Verilog::$root." + instance + "." + base, "observation symbol mismatch");
                if (!array) continue;
                const auto &a = item["array"];
                int size = a["size"].int_value(), offset = a["offset"].int_value(), index = a["internal_index"].int_value();
                need(size > 0 && size < 1000000 && index >= 0 && index < size &&
                     offset > -1000000 && offset < 1000000, "unsupported array range");
                int declared = offset + (a["increasing"].bool_value() ? index : size - 1 - index);
                need(a["declared_index"].int_value() == declared && relative == base + "[" + std::to_string(declared) + "]",
                     "array declared/internal index disagreement");
                std::vector<Location> locations;
                resolve(design, root, base, {}, locations);
                if (locations.empty()) { checkChain(locate(design, root, relative)); continue; }
                need(locations.size() == 1 && locations[0].memory, "array base is not a unique compiled memory");
                auto loc = locations[0];
                checkChain(loc);
                auto mem = loc.memory;
                need(mem->width == item["width"].int_value() && mem->size == size && mem->start_offset == offset,
                     "compiled memory layout differs from typed array");
                if (mapped.insert({loc.module->name, mem->name}).second) {
                    memories.select(loc.module, mem);
                    conversions.push_back(Json::object{{"module", name(loc.module->name)}, {"memory", name(mem->name)}});
                }
            }
            if (!mapped.empty()) Pass::call_on_selection(design, memories, "memory_map");
            CellTypes types(design);
            std::map<Module*, std::unique_ptr<Drivers>> drivers;
            auto driven = [&](Module *module, SigSpec signal) {
                auto &entry = drivers[module];
                if (!entry) entry.reset(new Drivers(module, types));
                return entry->driven(signal);
            };
            Json::array mapping, additions;
            std::set<Module*> edited;
            for (auto i : selected) {
                const auto &item = observations[i];
                auto relative = text(item["relative"]);
                auto loc = locate(design, root, relative);
                checkChain(loc);
                auto wire = loc.wire;
                need(wire != nullptr, "observation is not a scalar element/wire: " + relative);
                need(wire->width > 0 && wire->width == item["width"].int_value() &&
                     wire->start_offset == item["offset"].int_value() && wire->upto == item["increasing"].bool_value() &&
                     wire->is_signed == item["signed"].bool_value(), "observation leaf type/range differs: " + relative);
                need(driven(loc.module, wire), "observation leaf contains undriven/unknown bits: " + relative);
                need(!loc.chain.empty() || (!wire->port_input && !wire->port_output), "observation is already a boundary port");
                if (wire->port_input && !loc.chain.empty()) {
                    auto edge = loc.chain.back();
                    need(edge.cell->hasPort(wire->name) && GetSize(edge.cell->getPort(wire->name)) == wire->width &&
                         driven(edge.parent, edge.cell->getPort(wire->name)), "nested input has disconnected/unknown parent connection: " + relative);
                }
                for (const auto &parameter : item["parameters"].object_items()) {
                    auto it = root->parameter_default_values.find(veval_yosys::identifier(parameter.first));
                    need(it != root->parameter_default_values.end() && sameParameter(it->second, text(parameter.second)),
                         "boundary parameter differs or is absent: " + parameter.first);
                }
                auto outputName = "veval_observe_h" + std::to_string(mapping.size());
                auto outputId = veval_yosys::identifier(outputName);
                auto output = [&](Module *module) {
                    need(!module->count_id(outputId), "observation output name collision: " + outputName);
                    auto added = module->addWire(outputId, wire->width);
                    added->start_offset = wire->start_offset;
                    added->is_signed = wire->is_signed;
                    added->upto = wire->upto;
                    added->port_output = true;
                    added->port_id = GetSize(module->ports) + 1;
                    module->ports.push_back(outputId);
                    edited.insert(module);
                    return added;
                };
                loc.module->connect(output(loc.module), wire);
                additions.push_back(Json::object{{"module", name(loc.module->name)}, {"output", outputName}, {"object", name(wire->name)}});
                for (auto it = loc.chain.rbegin(); it != loc.chain.rend(); ++it) {
                    need(!it->cell->hasPort(outputId), "observation child-port collision");
                    it->cell->setPort(outputId, output(it->parent));
                    additions.push_back(Json::object{{"module", name(it->parent->name)}, {"output", outputName},
                        {"cell", name(it->cell->name)}, {"child_output", outputName}});
                }
                Json::array chain;
                for (auto edge : loc.chain) chain.push_back(Json::object{{"parent_module", name(edge.parent->name)},
                    {"cell", name(edge.cell->name)}, {"module", name(edge.child->name)}});
                auto row = item.object_items();
                row["actual"] = Json::object{{"width", wire->width}, {"signed", wire->is_signed},
                    {"offset", wire->start_offset}, {"increasing", wire->upto}};
                row["chain"] = chain;
                row["leaf_module"] = name(loc.module->name);
                row["leaf_object"] = name(wire->name);
                row["leaf_kind"] = wire->port_input ? "input" : wire->port_output ? "output" : "internal";
                row["output"] = outputName;
                mapping.push_back(row);
            }
            for (auto module : edited) module->fixup_ports();
            std::ofstream out(args[2]);
            out << Json(Json::object{{"schema", "consflow-observation-binding/1"}, {"module", rootName},
                {"instance", instance}, {"mapping", mapping}, {"additions", additions}, {"memory_map", conversions}}).dump() << '\n';
            need(bool(out), "cannot write observation bindings");
            log("Routed %zu read-only observations; mapped %zu selected memories.\n", mapping.size(), mapped.size());
        } catch (const std::exception &e) {
            log_cmd_error("Observation refused: %s.\n", e.what());
        }
    }
} Observation;
PRIVATE_NAMESPACE_END
