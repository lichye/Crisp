// Lower source properties through instance bindings; report coverage and usable landings.

#ifndef VEVAL_LOWERING_PROPERTYTRANSPORT_H
#define VEVAL_LOWERING_PROPERTYTRANSPORT_H

#include "frontend/sva/PropertyMonitor.h"
#include "lowering/Symbolic/BitRewriting.h"
#include "lowering/Symbolic/CombinationalDefinitions.h"
#include "lowering/connectivity/ConnectionMap.h"

#include <util/mp_arith.h>

#include <iosfwd>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <unordered_map>

namespace veval {
namespace lowering {

// Observation coverage, separate from monitor validity and landing eligibility.
// A Partial row can include missing signals and have no usable monitor.
enum class LoweringCoverage {
    Full,
    Partial,
    Blocked,
};

std::string toString(LoweringCoverage);

// Translation quality for assumptions: exact, certified weaker constraint, or refused.
// This does not establish that the source assumption holds in the design.
enum class AssumeGrade {
    ClaimedSyntactic,
    SoundAsConstraint,
    Refused,
};

std::string toString(AssumeGrade);

// Refusal reasons. Registered/CrossClock describe bounded dependency witnesses;
// they never authorize retiming a property. All applicable reasons are retained.
enum class ReasonCode {
    BlackboxInterface,
    DisableIffIncompatible,
    Constant,
    NoSameCycleWitness,
    Registered,
    CrossClock,
    DynamicSelect,
    MultiSignalAggregate,
    WidthType,
    ReadOutsideSlice,
    ComputedComb,
};

std::string toString(ReasonCode);

// Same-cycle binding facts from invar(); sequential attribution is kept separate.
struct EdgeFact {
    enum class Kind {
        None,
        AlreadyLocal,
        Whole,
        SliceCarried,
        SliceOutside,
        SliceRefused,
    };
    Kind kind = Kind::None;

    // Same bits under a different declared type.
    bool retyped = false;

    // The inverse binding uses a bitwise complement.
    bool complemented = false;

    bool tiedOff = false;

    bool parentComputes = false;

    // Failed binding side conditions when kind == SliceRefused.
    std::vector<ReasonCode> shapeCodes;

    bool blackboxTarget = false;
};

// Sequential attribution for observations with no usable same-cycle binding.
// These facts explain refusals and are never used for substitution.
enum class FallFact {
    NotAsked,
    NoRelation,
    Registered,
    CrossClock,
};

// Abort guards need their own refusal reason when their signal cannot arrive.
enum class RoleFact {
    Observation,
    AbortAtom,
};

// One source observation at one instance. childPort is empty when it did not arrive.
struct SignalFate {
    std::string parentSignal;
    std::string childPort;
    BindingKind through = BindingKind::Computed;
    bool reached = false;
    std::string blockedBy;

    EdgeFact edge;
    FallFact fall = FallFact::NotAsked;
    RoleFact role = RoleFact::Observation;

    // True when this instance drives the observation rather than receiving it.
    bool driven = false;

    std::string witness;
    std::string witnessKind;

    // Reasons and route tags are derived from edge/fall/role, not accumulated state.
    std::vector<ReasonCode> reasons() const;

    std::vector<std::string> path() const;
};

// Highest-precedence reason; an empty list falls back to ComputedComb.
ReasonCode primaryReason(const std::vector<ReasonCode>&);

// Name convention used only for contextual-arrival reporting: clk, clock, clk_i,
// rst, reset, rst_n, rst_ni, resetn, rstn. It is not clock-domain inference.
bool globalClockOrReset(const std::string& shownName);

// Default rows share an immutable empty connection table.
inline const std::shared_ptr<const InstanceEdge>& emptyTransportEdge() {
    static const auto edge = std::make_shared<const InstanceEdge>();
    return edge;
}

struct PropertyLanding {
    struct FlowEdge {
        std::string kind;
        std::string record;
        std::string detail;
    };

    std::string property;
    // The source keyword; independent of the port-direction classification below.
    bool isAssumption = false;

    // Initial-instant environment fact, never an ordinary always-assumption.
    bool isResetContract = false;

    // Stronger assertion body with an unavailable abort guard removed. A proof may
    // support the guarded source; a local counterexample need not refute it.
    bool abortDropped = false;

    // All observations arrive through inputs. Abort-free strengthening and
    // property-local variables are excluded from this direction-based assumption.
    bool isAssumptionHere = false;

    // At least one observation arrives through an input, excluding clock/reset names.
    bool mentionsInput = false;

    // Placement may suppress an otherwise representable landing. Coverage is retained.
    bool placed = true;
    std::string stoppedAt;
    std::string stopReason;
    // Instance depth: 1 is an immediate child of the source boundary.
    std::size_t depth = 1;
    // Hierarchy depth of the preceding arrival; prefers routes from shallower scopes.
    std::size_t routeDepth = 0;
    std::string instance;
    std::string module;

    LoweringCoverage result = LoweringCoverage::Blocked;

    // A monitor was constructed through admitted bindings. This is not a proof.
    bool certified = false;

    // Undo the entire route and compare with the source; width-changing casts matter.
    bool roundTrips = false;
    // The same check against the immediate parent, retained for diagnosis.
    bool roundTripsOneHop = false;

    // The source SVA can express all bit rebases performed on the monitor.
    bool rebasePrintable = true;

    // Child monitor and incoming source monitor; meaningful only when certified.
    sva::Monitor monitor = sva::emptyMonitor();

    sva::Monitor source = sva::emptyMonitor();

    // Recorded substitutions/rebases for round trips and diagnostics.
    std::map<irep_idt, exprt> claimedSubstitution;
    // Ancestor combinational definitions expanded before this edge's binding
    // substitution. The round trip compares against this expanded source.
    std::shared_ptr<const std::map<irep_idt, exprt>> expandedDefinitions;
    using ClaimedRebase = symbolic::ClaimedRebase;
    std::vector<ClaimedRebase> claimedRebases;

    std::vector<SignalFate> fates;

    // Shared original bindings, including the instance-specific parameter context.
    std::shared_ptr<const InstanceEdge> edge = emptyTransportEdge();

    // Provenance for optional through-logic transformations; empty for binding-only paths.
    std::vector<FlowEdge> flowEdges;
    std::string evidenceOrigin;

    std::vector<std::string> unreachable() const;

    // Arrived and dropped partition the observations.
    std::vector<std::string> arrived() const;

    struct Dropped {
        std::string signal;
        std::string reason;
        std::vector<std::string> reasons;
    };
    std::vector<Dropped> dropped() const;
};

// Derive translation quality from coverage, certification and round-trip status.
AssumeGrade assumeGrade(const PropertyLanding&);

// Shared spelling for a missing binding with no more specific reason code.
extern const char* const kNoPortBinding;

// Highest-precedence dropped-signal reason, or no-port-binding if none has a code.
// Empty when nothing was dropped.
std::string stopReason(const PropertyLanding&);

// Unique reason codes in precedence order; excludes the no-port-binding fallback.
std::vector<std::string> stopReasons(const PropertyLanding&);

// Dependency maps are target <- sources; delayedDrives reads trans(),
// combinationalDrives reads invar(). Both serve attribution, not rewriting.
using DelayedDrives = std::map<irep_idt, std::set<irep_idt>>;

DelayedDrives delayedDrives(const transition_systemt&);

// Representative -> members, built once to avoid rescanning aliases per observation.
using AliasClasses = std::map<irep_idt, std::vector<irep_idt>>;

AliasClasses aliasClasses(const std::map<irep_idt, irep_idt>& aliases);

// Instance -> alias-class representative of its clock net, read from port bindings.
std::map<std::string, std::string> clockNets(
    const std::vector<InstanceEdge>&,
    const std::map<irep_idt, irep_idt>& aliases);

DelayedDrives combinationalDrives(const transition_systemt&);

// Forward attribution index: source -> sorted unique destinations.
using ForwardDrives =
    std::unordered_map<irep_idt, std::vector<irep_idt>, irep_id_hash>;
struct Attribution {
    ForwardDrives delayed;
    ForwardDrives combinational;
    std::map<std::string, std::string> clocks;

    bool empty() const { return delayed.empty() && combinational.empty(); }
};

// Workers share immutable IR and already-interned IDs. No concurrent IR mutation
// or name interning is allowed; all workers join before returning. Zero is serial.
Attribution attributionInputs(const transition_systemt&,
                              const std::vector<InstanceEdge>&,
                              const std::map<irep_idt, irep_idt>& aliases,
                              std::size_t workers = 1);

// Instances without internal symbols, children or local invar()/trans() constraints.
using BlackboxSet = std::set<std::string>;

BlackboxSet blackboxInstances(const transition_systemt&,
                             const std::vector<InstanceEdge>&,
                             std::size_t threads = 1);

// Bounded dependency reach, cached per source signal across all target instances.
struct SequentialReach {
    // Fewest clocked stages, recording only paths that cross at least one register.
    std::map<irep_idt, std::size_t> stages;
    // Full witness paths include same-cycle prefixes; predecessor-only reconstruction
    // can lose those prefixes or form cycles at register feedback.
    std::map<irep_idt, std::vector<irep_idt>> route;
    // Final register on each route, used to identify its clock domain.
    std::map<irep_idt, irep_idt> lastRegister;
    // Alias-class representative -> name reached in the fewest stages.
    std::map<irep_idt, irep_idt> byClass;
};

using ReachCache = std::map<irep_idt, SequentialReach>;

// Total dependency hops, including combinational edges; this bounds attribution only.
constexpr std::size_t maxSequentialHops = 4;

// Diagnostic override of the default bound.
extern std::size_t sequentialHops;

// Default placement follows the sole full, non-reader child at each level.
// Shared sibling landings stop the chain. Authored assumes and reset contracts
// are exempt; false retains all landings (split placement).
extern bool singlePathPlacement;

struct StoppedAbove {
    std::string property;
    std::string instance;   // the removed landing
    std::string stoppedAt;  // where the property stays; "" is the top
    std::string reason;     // shared-by-siblings, not-established-here, off-path
};

// Mark off-path landings without changing their coverage; optionally record stops.
void placeSinglePath(std::vector<PropertyLanding>&, std::vector<StoppedAbove>* = nullptr);

SequentialReach sequentialReach(const irep_idt& key,
                                const std::map<irep_idt, irep_idt>& aliases,
                                const AliasClasses&, const Attribution&);

// Rewrite one edge using same-cycle aliases. Attribution only explains refusals.
// Callers may supply shared alias/reach caches; empty arguments create local ones.
PropertyLanding transportDown(const sva::Monitor&, const InstanceEdge&,
                        const std::map<irep_idt, irep_idt>& aliases,
                        const Attribution& attribution = {},
                        const AliasClasses& classes = {},
                        ReachCache* cache = nullptr,
                        const BlackboxSet& blackboxes = {});

// Require an admitted, round-tripping landing with an authored or directional assume.
// Reset contracts, abort-free strengthening and property-local variables are excluded.
bool usableAsAssumption(const PropertyLanding&);

// Undo hops from the landing toward the boundary and compare with the source.
bool roundTripsToTop(const sva::Monitor& original,
                     const std::vector<const PropertyLanding*>& hops);

// An authored assumption over a signal the target drives. Report it without
// changing the user's assumption into an obligation.
struct AuthoredAssumeOverDriven {
    std::string property;
    std::string instance;
    std::string module;
    std::string signal;
};

// Sorted by property then driving instance; signal names one driven observation.
std::vector<AuthoredAssumeOverDriven> authoredAssumesOverDriven(
    const std::vector<PropertyLanding>&);

// Emit informational warnings; recorded controls whether a manifest is referenced.
void warnAuthoredAssumesOverDriven(
    const std::vector<AuthoredAssumeOverDriven>&, std::ostream&,
    bool recorded = true);

// Rename instance-scoped atoms to a standalone child top. Other atoms are unchanged.
sva::Monitor rerootToChild(const PropertyLanding&, const std::string& childTop);

// Try all edges from the source, then propagate full arrivals further down.
// Keep one row per property/instance, apply placement, and optionally record stops.
std::vector<PropertyLanding> lowerProperties(const sva::MonitorSet&,
                                    const std::vector<InstanceEdge>&,
                                    const std::map<irep_idt, irep_idt>& aliases,
                                    const Attribution& attribution = {},
                                    const BlackboxSet& blackboxes = {},
                                    std::vector<StoppedAbove>* stopped = nullptr,
                                    std::size_t threads = 1,
                                    const symbolic::CombinationalDefinitions* definitions = nullptr);

void writeText(const std::vector<PropertyLanding>&, std::ostream&);
// Include frontend refusals even though they generated no landing.
// The driven/stopped lists are emitted only when nonempty.
void writeJson(const std::vector<PropertyLanding>&, std::ostream&,
               const std::vector<sva::UnsupportedProperty>& refused = {},
               const std::vector<AuthoredAssumeOverDriven>& driven = {},
               const std::vector<StoppedAbove>& stopped = {});

}  // namespace lowering
}  // namespace veval

#endif  // VEVAL_LOWERING_PROPERTYTRANSPORT_H
