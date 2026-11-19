#include "util/database/JsonEscape.h"
#include "ReachabilityReport.h"

#include <algorithm>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace veval {
namespace lowering {

namespace {

std::string escape(const std::string& text) {
    return veval::util::jsonEscape(text);
}

// A symeval hop re-expresses the entry before ordinary binding propagation.
std::string via(const PropertyLanding& transport) {
    for (const auto& edge : transport.flowEdges)
        if (edge.kind == "symeval") return "symeval";
    return "binding";
}

// No observation arrived and no stop was recorded. Empty fates are not untouched.
bool untouched(const PropertyLanding& transport) {
    for (const auto& fate : transport.fates) {
        if (fate.reached) return false;
        if (!fate.reasons().empty()) return false;
    }
    return !transport.fates.empty();
}

// Prefer the furthest-reaching record for the same authored entry and instance.
int rank(const PropertyLanding& transport) {
    switch (transport.result) {
        case LoweringCoverage::Full:
            return 3;
        case LoweringCoverage::Partial:
            return 2;
        case LoweringCoverage::Blocked:
            return untouched(transport) ? 0 : 1;
    }
    return 0;
}

// Attribute derived monitors to their original specification entry.
const std::string& authoredEntry(const PropertyLanding& transport) {
    return transport.evidenceOrigin.empty() ? transport.property
                                            : transport.evidenceOrigin;
}

}  // namespace

std::vector<ReachRow> reachReport(const std::vector<PropertyLanding>& transports) {
    // Authored entries cover all instances; derived entries need not.
    std::set<std::string> instances;
    for (const auto& transport : transports)
        if (transport.evidenceOrigin.empty())
            instances.insert(transport.instance);

    // (entry, instance) -> the record that got furthest.
    std::map<std::string, std::map<std::string, const PropertyLanding*>> best;
    std::map<std::string, bool> isAssume;
    for (const auto& transport : transports) {
        const auto& entry = authoredEntry(transport);
        // Keep the same instance set for every entry's coverage denominator.
        if (!instances.count(transport.instance)) continue;
        auto& row = best[entry];
        // Authored keywords override the fallback from a derived monitor.
        if (transport.evidenceOrigin.empty())
            isAssume[entry] = transport.isAssumption;
        else
            isAssume.emplace(entry, transport.isAssumption);
        auto found = row.find(transport.instance);
        if (found == row.end() || rank(transport) > rank(*found->second))
            row[transport.instance] = &transport;
    }

    std::vector<ReachRow> report;
    for (const auto& [entry, landings] : best) {
        ReachRow row;
        row.entry = entry;
        row.kind = isAssume[entry] ? "assume" : "property";
        row.designInstances = instances.size();

        for (const auto& [instance, transport] : landings) {
            switch (transport->result) {
                case LoweringCoverage::Full:
                    row.full.push_back(
                        {instance, transport->depth, via(*transport)});
                    break;
                case LoweringCoverage::Partial: {
                    ReachRow::Partial partial;
                    partial.instance = instance;
                    partial.depth = transport->depth;
                    partial.arrived = transport->arrived();
                    partial.dropped = transport->dropped();
                    if (transport->isAssumption)
                        partial.grade = toString(assumeGrade(*transport));
                    row.partial.push_back(std::move(partial));
                    break;
                }
                case LoweringCoverage::Blocked:
                    if (untouched(*transport)) {
                        row.untouched++;
                        break;
                    }
                    row.blocked.push_back({instance, transport->depth,
                                           stopReason(*transport),
                                           stopReasons(*transport)});
                    break;
            }
        }
        report.push_back(std::move(row));
    }
    return report;
}

void writeJson(const std::vector<ReachRow>& report, std::ostream& out) {
    out << "{\n \"schema\": \"consflow-reach/1\",\n \"entries\": [\n";
    for (std::size_t i = 0; i < report.size(); i++) {
        const auto& row = report[i];

        // Every design instance must occur in exactly one coverage bucket.
        const auto counted =
            row.full.size() + row.partial.size() + row.blocked.size() +
            row.untouched;
        if (counted != row.designInstances) {
            std::ostringstream message;
            message << "reach.json: " << row.entry << " accounts for "
                    << counted << " instance(s) (" << row.full.size()
                    << " full + " << row.partial.size() << " partial + "
                    << row.blocked.size() << " blocked + " << row.untouched
                    << " untouched) but the design has " << row.designInstances;
            throw std::runtime_error(message.str());
        }

        out << "  {\"entry\": \"" << escape(row.entry) << "\", \"kind\": \""
            << row.kind << "\", \"design_instances\": " << row.designInstances
            << ",\n   \"full\": [";
        for (std::size_t f = 0; f < row.full.size(); f++)
            out << (f ? ",\n            " : "")
                << "{\"instance\": \"" << escape(row.full[f].instance)
                << "\", \"depth\": " << row.full[f].depth << ", \"via\": \""
                << row.full[f].via << "\"}";
        out << "],\n   \"partial\": [";
        for (std::size_t p = 0; p < row.partial.size(); p++) {
            const auto& partial = row.partial[p];
            out << (p ? ",\n               " : "") << "{\"instance\": \""
                << escape(partial.instance) << "\", \"depth\": "
                << partial.depth << ", \"arrived\": [";
            for (std::size_t a = 0; a < partial.arrived.size(); a++)
                out << (a ? ", " : "") << "\"" << escape(partial.arrived[a])
                    << "\"";
            out << "], \"dropped\": [";
            for (std::size_t d = 0; d < partial.dropped.size(); d++) {
                out << (d ? ", " : "") << "{\"signal\": \""
                    << escape(partial.dropped[d].signal) << "\", \"reason\": \""
                    << partial.dropped[d].reason << "\", \"reasons\": [";
                for (std::size_t r = 0; r < partial.dropped[d].reasons.size();
                     r++)
                    out << (r ? ", " : "") << "\""
                        << partial.dropped[d].reasons[r] << "\"";
                out << "]}";
            }
            out << "]";
            if (!partial.grade.empty())
                out << ", \"grade\": \"" << partial.grade << "\"";
            out << "}";
        }
        out << "],\n   \"blocked\": [";
        for (std::size_t b = 0; b < row.blocked.size(); b++) {
            out << (b ? ",\n               " : "") << "{\"instance\": \""
                << escape(row.blocked[b].instance) << "\", \"depth\": "
                << row.blocked[b].depth << ", \"reason\": \""
                << row.blocked[b].reason << "\", \"reasons\": [";
            for (std::size_t r = 0; r < row.blocked[b].reasons.size(); r++)
                out << (r ? ", " : "") << "\"" << row.blocked[b].reasons[r]
                    << "\"";
            out << "]}";
        }
        out << "],\n   \"untouched\": " << row.untouched << "}"
            << (i + 1 == report.size() ? "\n" : ",\n");
    }
    out << " ]\n}\n";
}

}  // namespace lowering
}  // namespace veval
