#include "SignalReachability.h"

#include "frontend/rtl/ElaboratedDesign.h"

#include <ebmc/transition_system.h>
#include <util/std_expr.h>
#include <util/symbol_table.h>

#include <algorithm>
#include <deque>
#include <ostream>
#include <set>

namespace veval {
namespace lowering {

namespace {

std::string display(const irep_idt& identifier) {
    return frontend::displayName(id2string(identifier));
}

}  // namespace

ReachResult reach(const transition_systemt& system,
                  const std::string& boundary,
                  const std::vector<InstanceEdge>& edges,
                  const std::map<irep_idt, irep_idt>& aliases,
                  bool internal) {
    auto canonical = [&aliases](const irep_idt& name) {
        auto it = aliases.find(name);
        return it == aliases.end() ? name : it->second;
    };

    // Internal signals join the boundary only when explicitly requested.
    const auto topPrefix = frontend::displayName(boundary) + '.';
    std::size_t boundaryDepth = 0;
    for (const auto& edge : edges)
        if (edge.instance + '.' == topPrefix) boundaryDepth = edge.depth;

    // Preserve all boundary names for each alias class, not just one spelling.
    std::map<irep_idt, std::set<std::string>> origin;

    std::set<irep_idt> frontier;
    ReachResult result;

    for (const auto& identifier : system.symbol_table.sorted_symbol_names()) {
        const auto& symbol = system.symbol_table.lookup_ref(identifier);
        if (symbol.type.id() == ID_verilog_module_instance) continue;
        if (symbol.is_macro) continue;
        if (!internal && !symbol.is_input && !symbol.is_output) continue;

        auto shown = display(identifier);
        if (shown.compare(0, topPrefix.size(), topPrefix) != 0) continue;

        // Directly in the top module: the instance path stops here.
        auto member = shown.substr(topPrefix.size());
        if (member.find('.') != std::string::npos) continue;

        const auto name = canonical(identifier);
        frontier.insert(name);
        origin[name].insert(member);
        result.boundarySignals++;
    }

    // Hierarchy order determines report order, not propagation order.
    std::vector<const InstanceEdge*> ordered;
    ordered.reserve(edges.size());
    for (const auto& edge : edges) ordered.push_back(&edge);
    std::sort(ordered.begin(), ordered.end(),
              [](const InstanceEdge* a, const InstanceEdge* b) {
                  return a->depth < b->depth;
              });

    // Follow whole bindings below the boundary to a fixed point. Sibling
    // outputs can feed earlier-sorted siblings, so one depth-ordered pass fails.
    struct Use {
        std::size_t edge;
        irep_idt port;
    };
    std::map<irep_idt, std::vector<Use>> uses;
    for (std::size_t e = 0; e < ordered.size(); ++e) {
        const auto* edge = ordered[e];
        if (edge->instance.compare(0, topPrefix.size(), topPrefix) != 0)
            continue;
        for (const auto& binding : edge->bindings) {
            if (binding.kind != BindingKind::Whole) continue;
            auto symbols = binding.parentSymbols();
            if (symbols.size() != 1) continue;
            uses[canonical(symbols.front().get_identifier())].push_back(
                {e, canonical(binding.port.get_identifier())});
        }
    }
    std::vector<std::set<std::string>> arrived(ordered.size());
    std::deque<irep_idt> work(frontier.begin(), frontier.end());
    while (!work.empty()) {
        const auto parent = work.front();
        work.pop_front();
        const auto found = uses.find(parent);
        if (found == uses.end()) continue;
        const auto names = origin[parent];
        for (const auto& use : found->second) {
            arrived[use.edge].insert(names.begin(), names.end());
            auto& target = origin[use.port];
            const auto before = target.size();
            target.insert(names.begin(), names.end());
            if (frontier.insert(use.port).second || target.size() != before)
                work.push_back(use.port);
        }
    }

    for (std::size_t e = 0; e < ordered.size(); ++e) {
        const auto* edge = ordered[e];
        const auto& arrivedFrom = arrived[e];
        if (!arrivedFrom.empty()) {
            Reached reached;
            reached.instance = edge->instance;
            reached.module = edge->module;
            reached.depth = edge->depth - boundaryDepth;
            reached.signals = arrivedFrom.size();
            reached.from.assign(arrivedFrom.begin(), arrivedFrom.end());
            result.reached.push_back(std::move(reached));
        }
    }

    return result;
}

void writeTo(const ReachResult& result, const std::string& instance,
             std::ostream& out) {
    for (const auto& reached : result.reached) {
        if (reached.instance != instance) continue;

        out << reached.instance << "  (" << reached.module << ")\n";
        out << reached.from.size()
            << " boundary signal(s) arrive here. A property built only out of\n"
               "these travels this far; one that mentions anything else does "
               "not.\n\n";
        for (const auto& signal : reached.from) out << "  " << signal << "\n";
        return;
    }

    out << "no boundary signal reaches " << instance << "\n";
}

void writeText(const ReachResult& result, std::ostream& out) {
    out << result.boundarySignals << " signal(s) at the boundary\n";
    out << result.reached.size() << " instance(s) reachable from them\n\n";

    std::set<std::string> modules;
    std::map<std::size_t, std::size_t> byDepth;

    for (const auto& reached : result.reached) {
        // Count source modules rather than per-instance specializations.
        auto source = reached.module.substr(0, reached.module.find('$'));
        modules.insert(source);
        byDepth[reached.depth]++;
    }

    out << modules.size() << " distinct module(s)\n\n";

    out << "reachable instances by depth (1 = a direct child of the top):\n";
    for (const auto& entry : byDepth)
        out << "  depth " << entry.first << ": " << entry.second << "\n";
    out << "\n";

    // Show the instances reached by the most boundary signals first.
    auto sorted = result.reached;
    std::sort(sorted.begin(), sorted.end(),
              [](const Reached& a, const Reached& b) {
                  return a.signals > b.signals;
              });

    out << "the twenty instances the most of the boundary reaches:\n";
    out << "  SIGNALS  DEPTH  INSTANCE\n";
    for (std::size_t i = 0; i < sorted.size() && i < 20; i++)
        out << "  " << sorted[i].signals << "\t " << sorted[i].depth << "\t"
            << sorted[i].instance << "  (" << sorted[i].module << ")\n";

    out << "\nAn upper bound. Reaching an instance means SOME signal of the "
           "boundary\narrives there, not that any particular property will -- "
           "a property naming\ntwo signals travels only as far as both do.\n";
}

void writeJson(const ReachResult& result, std::ostream& out) {
    out << "{\n \"boundary_signals\": " << result.boundarySignals << ",\n";
    out << " \"reached\": [\n";

    bool first = true;
    for (const auto& reached : result.reached) {
        if (!first) out << ",\n";
        first = false;
        out << "  {\"instance\": \"" << reached.instance << "\", \"module\": \""
            << reached.module << "\", \"depth\": " << reached.depth
            << ", \"signals\": " << reached.signals << ", \"from\": [";
        for (std::size_t i = 0; i < reached.from.size(); i++) {
            if (i != 0) out << ", ";
            out << '"' << reached.from[i] << '"';
        }
        out << "]}";
    }

    out << "\n ]\n}\n";
}

}  // namespace lowering
}  // namespace veval
