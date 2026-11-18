#ifndef VEVAL_LOWERING_SYMBOLIC_PROPERTYSUBSTITUTION_H
#define VEVAL_LOWERING_SYMBOLIC_PROPERTYSUBSTITUTION_H

#include "BitRewriting.h"
#include "frontend/sva/PropertyMonitor.h"

#include <map>
#include <optional>
#include <vector>

namespace veval {
namespace lowering {
namespace symbolic {

// Substitute design signals throughout the atomic propositions. The state
// symbol, the states and the transition structure are left alone, which is
// what makes the result the same automaton over a renamed alphabet.
//
// Symbols absent from `substitution` are kept as they are; it is the
// caller's job to decide whether a monitor that still mentions signals
// outside the child is usable.
sva::Monitor substituteAtoms(const sva::Monitor&,
                        const std::map<irep_idt, exprt>& substitution);

// Apply exact expression normalization to the lowered property and its monitor.
void simplifyComplements(sva::Monitor& monitor);
bool sameMonitor(const sva::Monitor& left, const sva::Monitor& right,
                 bool complements = false);
bool usesComplement(const std::map<irep_idt, exprt>& substitution);

// Reverse one lowering edge and compare with the source property.
std::optional<sva::Monitor> undoHop(
    const sva::Monitor& landed, const std::map<irep_idt, exprt>& substitution,
    const std::vector<ClaimedRebase>& rebased);
bool roundTripsBack(const sva::Monitor& arrived, const sva::Monitor& landed,
                    const std::map<irep_idt, exprt>& substitution,
                    const std::vector<ClaimedRebase>& rebased);

}  // namespace symbolic
}  // namespace lowering
}  // namespace veval

#endif
