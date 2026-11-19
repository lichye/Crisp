// Construct the environment recorded in each generated module specification.
//
// Reset contracts constrain the initial state only. Constant input connections
// and recognized combinational clamps supply facts from the parent wiring.
// Unrecognized logic supplies no inferred assumption; uninitialized state is
// not silently zeroed. Transported assumptions are included in the environment
// fingerprint used when reading saved results. This file does not solve checks.

#ifndef VEVAL_ENVIRONMENT_ENVIRONMENT_H
#define VEVAL_ENVIRONMENT_ENVIRONMENT_H

#include "lowering/connectivity/ConnectionMap.h"
#include "lowering/propagation/PropertyLowering.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

class namespacet;
class transition_systemt;

namespace veval {
namespace environment {

// One premise using instance-qualified names, e.g. top.u_c0.port.
struct Premise {
    enum class When {
        Initial,  // conjoined onto init(): true at the initial instant
        Always,   // conjoined onto invar(): true in every state
    };

    // Tie-offs are also emitted as sigma_tie_N statements in the product.
    enum class Source { ResetContract, TieOff, Clamp };

    When when = When::Always;
    Source source = Source::TieOff;
    exprt constraint = nil_exprt{};
    // Product comment using module-local names.
    std::string shown;
    // Stable fingerprint input using full hierarchical names.
    std::string digestLine;
};

// Environment facts for one instance.
struct InstanceEnvironment {
    std::vector<Premise> premises;
    // Sorted lines for all premises and transported assumptions. Its fingerprint
    // invalidates saved verdicts when the environment changes.
    std::string digest;
};

// Strip the outer always from a reset contract; reject remaining temporal operators.
std::optional<exprt> resetContractStatePredicate(const exprt& body);

// Collect resets and assumptions from landings, constants from bindings, and
// clamps from the parent invariant. Keyed by instance path.
std::map<std::string, InstanceEnvironment> environment(
    const std::vector<lowering::PropertyLanding>& transports,
    const std::vector<lowering::InstanceEdge>& edges,
    const transition_systemt& system, const namespacet& ns);

}  // namespace environment
}  // namespace veval

#endif  // VEVAL_ENVIRONMENT_ENVIRONMENT_H
