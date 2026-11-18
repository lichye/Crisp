// Signal reachability through whole bindings and aliases, without a property.
// A reached signal alone does not establish that a full property can be lowered.

#ifndef VEVAL_LOWERING_REACH_H
#define VEVAL_LOWERING_REACH_H

#include <iosfwd>
#include <map>
#include <string>
#include <vector>

#include "ConnectionMap.h"

namespace veval {
namespace lowering {

struct Reached {
    // Arriving signals, named in the original boundary's scope.
    std::vector<std::string> from;

    // Instance path, e.g. `plena.instr_mem_inst.tl_master_inst'.
    std::string instance;
    // The module the designer wrote.
    std::string module;
    // How deep below the boundary, 1 being a direct child.
    std::size_t depth = 1;
    // Number of distinct boundary signals arriving whole.
    std::size_t signals = 0;
};

struct ReachResult {
    // Signals of the boundary itself that the walk started from.
    std::size_t boundarySignals = 0;
    std::vector<Reached> reached;
};

// Start from the ports of the named instance. `internal' also includes its
// directly declared signals. Only descendants of this boundary are reported.
ReachResult reach(const transition_systemt&, const std::string& boundary,
                  const std::vector<InstanceEdge>&,
                  const std::map<irep_idt, irep_idt>& aliases,
                  bool internal = false);

// The boundary signals that arrive at one instance, and nothing else.
void writeTo(const ReachResult&, const std::string& instance, std::ostream&);

void writeText(const ReachResult&, std::ostream&);
void writeJson(const ReachResult&, std::ostream&);

}  // namespace lowering
}  // namespace veval

#endif  // VEVAL_LOWERING_REACH_H
