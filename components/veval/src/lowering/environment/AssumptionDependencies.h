// Track which asserted copies justify assumptions at each module.
//
// Pair copies by the source sentence, not just the label. Preserve whether an
// assumption is authored, internally driven, or used at its own producer.
// Saved verdicts and environment fingerprints determine the reported premise
// status; this pass does not invoke a solver or prove composition by itself.
// Output schema names (discharge.json and premise tiers) remain compatible.

#ifndef VEVAL_LOWERING_DISCHARGE_H
#define VEVAL_LOWERING_DISCHARGE_H

#include "lowering/output/WriteLocalSpecs.h"
#include "lowering/propagation/PropertyLowering.h"

#include <cstddef>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

class namespacet;

namespace veval {
namespace lowering {

// One asserted copy of the property, and what an engine said about it.
struct DischargeSite {
    // Instance responsible for this asserted copy.
    std::string instance;
    // proved, refuted, bounded-clean or unknown; stale results become unknown.
    std::string status;
    std::string detail;
    // May differ from the assumption's label when source sentences match.
    std::string property;
};

// One assumed copy and its supporting assertions.
struct AssumptionDependency {
    std::string property;
    // Instance using this assumption.
    std::string instance;
    // Asserted copies, sorted by instance.
    std::vector<DischargeSite> discharge;
    // `proved', `refuted', `undecided' or `host-promise'.
    std::string classification;
    // Unasserted internal drivers. Advisory: affects wording, not classification.
    std::vector<std::string> internallyDriven;
    // This consumer itself drives an observed signal. Unlike internallyDriven,
    // this weakens the premise tier: the check assumes its own obligation.
    bool grantedOverDriven = false;
};

struct AssumptionDependencyReport {
    // Both maps use proofKey(instance, property).
    std::map<std::string, AssumptionDependency> records;
    // Decided assert landings only:
    // `unconditional', `conditional-refuted', `conditional-undecided' or
    // `conditional-granted'.
    std::map<std::string, std::string> premiseTier;

    std::size_t proved = 0;
    std::size_t refuted = 0;
    std::size_t undecided = 0;
    std::size_t hostPromise = 0;
    // Records with internallyDriven / grantedOverDriven, respectively.
    std::size_t drivenElsewhere = 0;
    std::size_t grantedHere = 0;

    std::size_t unconditional = 0;
    std::size_t conditionalRefuted = 0;
    std::size_t conditionalUndecided = 0;
    std::size_t conditionalGranted = 0;

    // The record for one landing, or nullptr.
    const AssumptionDependency* at(const std::string& instance,
                                  const std::string& property) const;
};

// Pair every assumed copy with its asserted copies.
//
// Reuse saved verdicts only if property and environment fingerprints still match.
AssumptionDependencyReport buildAssumptionDependencies(
    const std::vector<PropertyLanding>& transports,
    const std::map<std::string, ProofLine>& verdicts,
    const std::map<std::string, std::string>& environmentFingerprint,
    const namespacet& ns);

// Body of the generated `proof:' comment for an assumed copy.
std::string describeAssumptionDependency(const AssumptionDependency&);

// Serialize the pairing to discharge.json.
void writeJson(const AssumptionDependencyReport&, std::ostream&);
// Print counts; mention discharge.json only when recorded is true.
void writeText(const AssumptionDependencyReport&, std::ostream&, bool recorded = true);

}  // namespace lowering
}  // namespace veval

#endif  // VEVAL_LOWERING_DISCHARGE_H
