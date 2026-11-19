#pragma once

#include "lowering/propagation/PropertyLowering.h"

namespace veval::lowering {
struct AssumptionCopy {
    symbol_exprt from, to;
    PropertyLanding landing;
};

// Additional representations of an authored assumption when several input
// ports bind the same whole parent signal. Each copy changes one port; no
// Cartesian product of multi-signal representations is enumerated.
std::vector<AssumptionCopy> copyPortAssumptions(const PropertyLanding&);
}
