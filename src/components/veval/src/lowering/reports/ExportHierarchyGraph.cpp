#include "util/database/JsonEscape.h"
#include "ExportHierarchyGraph.h"

#include "frontend/rtl/ElaboratedDesign.h"

#include <util/bitvector_types.h>
#include <util/std_expr.h>
#include <util/json_irep.h>

#include <algorithm>
#include <map>
#include <ostream>
#include <set>

namespace veval {
namespace lowering {

namespace {

std::string escape(const std::string& text) {
    return veval::util::jsonEscape(text);
}

std::string display(const irep_idt& identifier) {
    return frontend::displayName(id2string(identifier));
}

std::size_t widthOf(const typet& type) {
    if (type.id() == ID_bool) return 1;
    if (!can_cast_type<bitvector_typet>(type)) return 0;
    return to_bitvector_type(type).get_width();
}

}  // namespace

std::vector<DesignGraph::Hop> routeOf(const PropertyLanding& transport) {
    std::vector<DesignGraph::Hop> route;
    for (const auto& flow : transport.flowEdges)
        route.push_back({transport.instance, flow.kind, false, 0, flow.record,
                         flow.detail});

    if (transport.abortDropped)
        route.push_back({transport.instance, "abort-dropped", false, 0, {},
                         "stronger assertion body; target implies source; counterexamples require replay"});

    if (transport.result == LoweringCoverage::Blocked) return route;

    std::string relation;
    for (const auto& fate : transport.fates) {
        if (!fate.reached || fate.childPort.empty()) continue;
        if (!relation.empty()) relation += "; ";
        relation += fate.parentSignal + " -> " + fate.childPort;
    }
    DesignGraph::Hop hop;
    hop.edge = transport.instance;
    hop.kind = "binding";
    // Bindings are composed from the boundary, so one hop can span many levels.
    hop.span = transport.depth;
    hop.composite = transport.depth > 1;
    hop.relation = relation;
    route.push_back(std::move(hop));
    return route;
}

DesignGraph designGraph(const std::vector<InstanceEdge>& edges,
                        const std::vector<PropertyLanding>& transports,
                        const std::string& environmentProfile) {
    DesignGraph graph;

    for (const auto& edge : edges) {
        DesignGraph::Node node;
        node.instance = edge.instance;
        node.module = edge.module;
        node.depth = edge.depth;
        node.parent = edge.parentInstance;
        node.scope = edge.scope;

        DesignGraph::Edge wire;
        wire.parent = node.parent;
        wire.child = edge.instance;

        std::set<std::string> ports;
        const auto prefix = edge.instance + ".";
        for (const auto& binding : edge.bindings) {
            const auto name = display(binding.port.get_identifier());
            if (!binding.isOutput &&
                name.compare(0, prefix.size(), prefix) == 0 &&
                name.find('.', prefix.size()) == std::string::npos)
                for (const auto& fact : constantBindingParts(binding))
                    node.fixedBindings.push_back(fact.relation());
            // A bidirectional binding still counts as one port.
            if (!ports.insert(name).second) continue;
            wire.portsBound++;
            wire.bitsBound += widthOf(binding.port.type());
            if (binding.kind != BindingKind::Whole) wire.bindingsUnsupported++;
        }
        node.ports = ports.size();
        node.environmentProfile = environmentProfile;

        graph.nodes.push_back(std::move(node));
        graph.edges.push_back(std::move(wire));
    }

    std::map<std::string, std::vector<DesignGraph::Premise>> premises;
    for (const auto& transport : transports) {
        if (usableAsAssumption(transport))
            premises[transport.instance].push_back(
                {transport.property, toString(transport.result),
                 transport.isAssumption ? toString(assumeGrade(transport))
                                        : std::string()});

        // Count untouched records without emitting empty routes.
        const bool anyCoded = std::any_of(
            transport.fates.begin(), transport.fates.end(),
            [](const SignalFate& fate) {
                return fate.reached || !fate.reasons().empty();
            });
        if (!anyCoded) {
            graph.untouchedRecords++;
            continue;
        }

        DesignGraph::Record record;
        record.entry = transport.evidenceOrigin.empty()
                           ? transport.property
                           : transport.evidenceOrigin;
        record.instance = transport.instance;
        record.result = toString(transport.result);
        record.stopReason = stopReason(transport);

        // Use the recorded route, not an inferred path through hierarchy nodes.
        record.route = routeOf(transport);
        graph.records.push_back(std::move(record));
    }

    for (auto& [instance, list] : premises) {
        std::sort(list.begin(), list.end(),
                  [](const DesignGraph::Premise& a,
                     const DesignGraph::Premise& b) {
                      return a.assume < b.assume;
                  });
        list.erase(std::unique(list.begin(), list.end(),
                               [](const DesignGraph::Premise& a,
                                  const DesignGraph::Premise& b) {
                                   return a.assume == b.assume;
                               }),
                   list.end());
    }

    for (auto& node : graph.nodes) {
        auto found = premises.find(node.instance);
        if (found == premises.end()) continue;
        node.premises = found->second;
    }

    std::sort(graph.nodes.begin(), graph.nodes.end(),
              [](const DesignGraph::Node& a, const DesignGraph::Node& b) {
                  return a.instance < b.instance;
              });
    std::sort(graph.edges.begin(), graph.edges.end(),
              [](const DesignGraph::Edge& a, const DesignGraph::Edge& b) {
                  return a.child < b.child;
              });
    std::sort(graph.records.begin(), graph.records.end(),
              [](const DesignGraph::Record& a, const DesignGraph::Record& b) {
                  return a.entry == b.entry ? a.instance < b.instance
                                            : a.entry < b.entry;
              });
    return graph;
}

void writeGraphJson(const DesignGraph& graph, std::ostream& out) {
    out << "{\n \"schema\": \"consflow-graph/1\",\n"
        << " \"untouched_records\": " << graph.untouchedRecords << ",\n"
        << " \"nodes\": [\n";
    for (std::size_t i = 0; i < graph.nodes.size(); i++) {
        const auto& node = graph.nodes[i];
        out << "  {\"instance\": \"" << escape(node.instance)
            << "\", \"module\": \"" << escape(node.module)
            << "\", \"depth\": " << node.depth << ", \"parent\": \""
            << escape(node.parent) << "\", \"scope\": \""
            << escape(node.scope) << "\", \"ports\": " << node.ports
            << ", \"environment_profile\": \""
            << escape(node.environmentProfile) << "\", \"premises\": [";
        for (std::size_t p = 0; p < node.premises.size(); p++) {
            out << (p ? ", " : "") << "{\"assume\": \""
                << escape(node.premises[p].assume) << "\", \"arrival\": \""
                << node.premises[p].arrival << "\"";
            if (!node.premises[p].grade.empty())
                out << ", \"grade\": \"" << node.premises[p].grade << "\"";
            out << "}";
        }
        out << "], \"fixed_bindings\": [";
        for (std::size_t b = 0; b < node.fixedBindings.size(); ++b) {
            if (b) out << ',';
            out << json_irept(false).convert_from_irep(node.fixedBindings[b]);
        }
        out << "]}" << (i + 1 == graph.nodes.size() ? "\n" : ",\n");
    }
    out << " ],\n \"edges\": [\n";
    for (std::size_t i = 0; i < graph.edges.size(); i++) {
        const auto& edge = graph.edges[i];
        out << "  {\"parent\": \"" << escape(edge.parent)
            << "\", \"child\": \"" << escape(edge.child)
            << "\", \"ports_bound\": " << edge.portsBound
            << ", \"bits_bound\": " << edge.bitsBound
            << ", \"bindings_unsupported\": " << edge.bindingsUnsupported
            << "}" << (i + 1 == graph.edges.size() ? "\n" : ",\n");
    }
    out << " ],\n \"records\": [\n";
    for (std::size_t i = 0; i < graph.records.size(); i++) {
        const auto& record = graph.records[i];
        out << "  {\"entry\": \"" << escape(record.entry)
            << "\", \"instance\": \"" << escape(record.instance)
            << "\", \"result\": \"" << record.result
            << "\", \"stop_reason\": \"" << escape(record.stopReason)
            << "\",\n   \"route\": [";
        for (std::size_t r = 0; r < record.route.size(); r++) {
            out << (r ? ", " : "") << "{\"edge\": \""
                << escape(record.route[r].edge) << "\", \"kind\": \""
                << escape(record.route[r].kind) << "\", \"relation\": \""
                << escape(record.route[r].relation) << "\"";
            if (record.route[r].kind == "binding")
                out << ", \"composite\": "
                    << (record.route[r].composite ? "true" : "false")
                    << ", \"span\": " << record.route[r].span;
            if (!record.route[r].detail.empty())
                out << ", \"detail\": \"" << escape(record.route[r].detail)
                    << "\"";
            out << "}";
        }
        out << "]}" << (i + 1 == graph.records.size() ? "\n" : ",\n");
    }
    out << " ]\n}\n";
}

}  // namespace lowering
}  // namespace veval
