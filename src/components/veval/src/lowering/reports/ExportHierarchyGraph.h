// Export hierarchy, environments and routes from existing lowering records.
// This view does not recompute bindings or infer source-level verdicts.

#ifndef VEVAL_LOWERING_GRAPHEXPORT_H
#define VEVAL_LOWERING_GRAPHEXPORT_H

#include <iosfwd>
#include <string>
#include <vector>

#include "lowering/connectivity/ConnectionMap.h"
#include "lowering/propagation/PropertyLowering.h"

namespace veval {
namespace lowering {

struct DesignGraph {
    struct Premise {
        std::string assume;
        // How that assume itself got here: full / partial.
        std::string arrival;
        std::string grade;
    };

    struct Node {
        std::string instance;
        std::string module;
        std::size_t depth = 0;
        // Nearest enclosing module instance; empty directly below the boundary.
        std::string parent;
        // Intervening generate-scope path, empty when there is none.
        std::string scope;
        std::size_t ports = 0;
        // Usable assumptions, shared by all landings at this instance.
        std::vector<Premise> premises;
        // full / isolation / allowlist; an empty list may be a profile choice.
        std::string environmentProfile;
        // Design constant-input equalities, separate from authored assumptions.
        std::vector<exprt> fixedBindings;
    };

    struct Edge {
        std::string parent;
        std::string child;
        std::size_t portsBound = 0;
        // Packed widths only; structured types without a packed width count zero.
        std::size_t bitsBound = 0;
        // Non-Whole bindings, including constants, slices and computed terms.
        std::size_t bindingsUnsupported = 0;
    };

    struct Hop {
        // The instance this hop arrives at.
        std::string edge;
        // binding / symeval / abort-dropped.
        std::string kind;
        // A binding hop can compose several hierarchy edges; span counts them.
        bool composite = false;
        std::size_t span = 0;
        // Binding pairs or the referenced symeval record id.
        std::string relation;
        // Existing hop explanation, empty for a binding hop.
        std::string detail;
    };

    struct Record {
        std::string entry;
        std::string instance;
        std::string result;
        std::vector<Hop> route;
        std::string stopReason;
    };

    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::vector<Record> records;
    // Records with no reached observation or coded stop: counted, not emitted.
    std::size_t untouchedRecords = 0;
};

// Shared by graph export and local-spec output so both report the same route.
std::vector<DesignGraph::Hop> routeOf(const PropertyLanding&);

DesignGraph designGraph(const std::vector<InstanceEdge>&,
                        const std::vector<PropertyLanding>&,
                        const std::string& environmentProfile);

void writeGraphJson(const DesignGraph&, std::ostream&);

}  // namespace lowering
}  // namespace veval

#endif  // VEVAL_LOWERING_GRAPHEXPORT_H
