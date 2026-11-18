// Shared SVA representation and violation-monitor construction. Lowering
// rewrites design observations; the SBY frontend emits the same encoding.
// This component does not run a model checker.
//
// General temporal conversion uses external Spot; bounded safety and
// property-local variables also have specialized encodings.

#ifndef VEVAL_SVA_BUECHIMONITOR_H
#define VEVAL_SVA_BUECHIMONITOR_H

#include <util/std_expr.h>

#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <vector>

class message_handlert;
class transition_systemt;

namespace veval {
namespace sva {

// The first four-state-sensitive construct, or empty for the supported
// two-state subset. This inspection never builds a monitor or starts Spot.
std::string xzConstruct(const exprt&);

// Share the default state expression across empty monitors. IREP copy-on-write
// keeps later edits independent.
inline const symbol_exprt& emptyMonitorState() {
    static const symbol_exprt state{irep_idt{}, typet{}};
    return state;
}

struct Monitor {
    // The property this monitor came from, as the design names it.
    std::string property;

    // Labels starting with reset_contract denote initial-state environment facts,
    // not ordinary always-assumptions or proof obligations.
    bool isResetContract = false;

    // Whether the source entry was an assume rather than an assert.
    bool isAssumption = false;
    // Readable form of sourceProperty.
    std::string sourceExpr;

    // Printable SVA in the landing's names. Design-symbol substitutions apply
    // to this tree and to the monitor; round-trip checks keep them consistent.
    exprt sourceProperty = nil_exprt{};

    // Original SVA shape, carried under the same substitutions. The cover pass
    // needs the antecedent before normalization rewrites implications.
    exprt originalProperty = nil_exprt{};

    // The automaton's own state. Not a design signal, and never substituted.
    symbol_exprt stateSymbol = emptyMonitorState();
    // Initial and transition constraints. An abort resets the monitor on every
    // cycle where its condition holds.
    exprt init, trans;
    // Violation predicate, masked off while an abort condition holds.
    exprt errorSignal;
    // Non-false when the property has a liveness part.
    exprt livenessSignal;

    // Upper bound on encoded states, saturated at size_t's maximum. Safety
    // monitors use one bit per NFA state, representing a set of active runs.
    std::size_t stateCount() const;

    // Every accepting state has an unconditional self-loop, so the property
    // is decided by reachability alone.
    bool isSafetyOnly() const { return livenessSignal.is_false(); }

    // Symbols in disable-iff guards, kept separate from body observations. An
    // unavailable guard may only be dropped under the assertion-strengthening rule.
    std::vector<symbol_exprt> abortAtoms;

    // Refresh disable-iff observations after a symbolic property rewrite.
    void refreshAbortAtoms();

    // The assertion's abort was removed at an earlier binding. This is a
    // stronger obligation, never an automatically granted assumption.
    bool abortDropped = false;

    // This assertion monitor guesses one launch/antecedent match. Negating
    // its error is not an exact environment constraint over all launches.
    bool hasLocalVariables = false;

    // Design signals the atomic propositions mention, in a stable order.
    // These, and only these, are what a transport substitutes.
    std::vector<symbol_exprt> atomSymbols() const;
};

inline const Monitor& emptyMonitor() {
    static const Monitor monitor;
    return monitor;
}

struct UnsupportedProperty {
    std::string property;
    std::string reason;
    // Preserve the source entry's role even when conversion fails.
    bool isAssumption = false;
    bool isResetContract = false;
};

struct MonitorSet {
    std::vector<Monitor> monitors;
    std::vector<UnsupportedProperty> unsupported;
    // Source posedge clock by property, using design-qualified names. Stored
    // once here rather than copied into every landing's monitor.
    std::map<std::string, std::string> samplingClocks;
};

// Whether the final component of a hierarchical label starts with reset_contract.
bool resetContractLabel(const std::string& name);

// Whether the supported encoding reports failure no earlier than the SVA
// attempt's window end, so disable-iff checks every required cycle.
bool abortWindowExact(const exprt& property);

// Remove the exact outer abort encoding installed by monitors(). Refuse
// assumptions, reset contracts, and any monitor whose encoding disagrees.
std::optional<Monitor> withoutAbort(const Monitor& monitor);

// Warn about assumptions rejected by the separate EBMC k-induction route.
// Its admitted shape is always/AG/G over a state predicate; use normalized
// SVA as input. This warning does not invoke an embedded solver or fully
// characterize other external EBMC methods.
bool engineDropsAsAssumption(const exprt& normalized);

// Convert every property of the design into a monitor.
MonitorSet monitors(const transition_systemt&, message_handlert&);

// Build a monitor for "the Boolean antecedent is never reached". A bounded
// refutation witnesses reachability. Reject non-implications or temporal antecedents.
std::optional<Monitor> antecedentCoverMonitor(const Monitor&,
                                              message_handlert&,
                                              std::string& refusal);

// Keep names containing any pattern. An empty pattern list keeps everything;
// a nonempty list matching no converted or unsupported entry is an error.
MonitorSet filter(MonitorSet, const std::vector<std::string>& patterns);

// Select exact assertion labels while retaining converted assumptions and
// reset contracts. Unsupported entries require an explicitly selected label.
// A label absent from the inventory is an error.
MonitorSet selectAssertions(MonitorSet, const std::vector<std::string>& labels);

void writeText(const MonitorSet&, std::ostream&);
void writeJson(const MonitorSet&, std::ostream&);

}  // namespace sva
}  // namespace veval

#endif  // VEVAL_SVA_BUECHIMONITOR_H
