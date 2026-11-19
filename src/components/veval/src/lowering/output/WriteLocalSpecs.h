// Write instance-specific SVA products and optional Tier2 explanation files.
// Only admitted, printable landings become properties; fixed design inputs are
// recorded separately. Generation does not invoke a verification engine.

#ifndef VEVAL_LOWERING_MODULESPEC_H
#define VEVAL_LOWERING_MODULESPEC_H

#include "lowering/environment/AssumptionSelection.h"
#include "lowering/propagation/PropertyLowering.h"

#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <vector>

class namespacet;

namespace veval {
namespace lowering {

// Shared SVA rendering; nullopt means the shape cannot be printed.
std::optional<std::string> renderProperty(const exprt&, const namespacet&);
std::optional<std::string> renderProperty(const exprt& expression,
                                          const exprt& original,
                                          const namespacet& ns);
// Explicit clock override, otherwise infer an input named clock or starting with clk.
std::string productClockPort(const InstanceEdge&, const std::string& overridePort = {});

struct AssumptionDependencyReport;

// Record a field premise omitted by the selected environment profile.
struct FieldProductOmission {
    std::string recordId, instance, property, sourceProperty, productFile, profile;
};

// Tier2 counts describe classification even when notes are disabled.
struct ModuleSpecStats {
    std::size_t files = 0;         // <module>_spec.sv written
    std::size_t notes = 0;         // Tier2/<module>_notes.md written
    std::size_t tier1 = 0;         // properties written out
    std::size_t designStated = 0;  // sigma tie-offs; separate denominator
    std::size_t tier2Partial = 0;  // arrived, but not whole: pointer entries
    std::size_t tier2Blocked = 0;  // individually listed binding-shape refusals
    // Blocked rows counted by class instead of individually listed; excludes
    // rows with no arrival or attribution evidence.
    std::size_t tier2Aggregated = 0;
    // Total class breakdown, including unrelated rows. This is a separate population.
    std::size_t tier2Counted = 0;
    // Products requiring declarations from another compilation unit.
    std::size_t prerequisite = 0;
    // Generated files removed because this call no longer produces them.
    std::size_t stale = 0;
    // Actual output paths in publication order; callers must not reconstruct filenames.
    std::vector<std::string> specFiles;
    std::vector<FieldProductOmission> fieldOmissions;
};

// Saved verdict annotations imported when regenerating products. These fields
// preserve earlier records; the current writer does not run the old engines.
struct ProofLine {
    // proved is unbounded; bounded-clean only covers a finite prefix.
    // unsupported and assume-only are not verdicts.
    std::string status = "unknown";
    std::string detail;
    // Property and environment fingerprints must both match before reusing an annotation.
    std::string content;
    std::string environment;
    // Optional trace path; not part of either fingerprint.
    std::string trace;
    // Legacy premise, engine and escalation annotations.
    std::string premiseTier;
    std::string engine;
    std::string escalatedFrom;
    // Optional provenance for through-logic transformations.
    std::string route;
    std::string symEvalRecord;
    std::string evidenceOrigin;
    std::string symEvalAntecedent;
    // Recorded cost and assumption selection; absent cost means no saved measurement.
    std::optional<double> seconds;
    std::optional<long> peakRssKb;
    std::vector<std::string> assumptionsInForce;
    std::vector<std::string> assumptionsDropped;
};

// Key a saved annotation by instance and source property.
std::string proofKey(const std::string& instance, const std::string& property);

// Shared admission test for product generation and result views.
bool isTier1(const PropertyLanding&);

// Fingerprint the exact local SVA, including local-variable declarations.
// Empty when the property cannot be rendered.
std::string propertyFingerprint(const PropertyLanding&, const namespacet& ns);

// Source sentence in boundary names, available only on a direct route.
// An empty result does not establish identity with another empty result.
std::string boundaryPropertyText(const PropertyLanding&, const namespacet& ns);

// Whole-port and bit-range tie-offs on this instance's declared inputs.
std::vector<ConstantBinding> constantInputBindings(const InstanceEdge&,
                                                   const namespacet& ns);

// Persistent 64-bit FNV-1a fingerprint, rendered as sixteen hex digits.
std::string fingerprintOf(const std::string& text);

struct ModuleSpecOptions {
    // Local sampling input; empty selects inference.
    std::string clockPort;
    // Clock-event products and source sampling nets keyed by property.
    bool multiclock = false;
    std::map<std::string, std::string> samplingClocks;
    // Filter exports only; keep the full transport graph and source inventory.
    std::string instanceOnly;
    std::string outDir;
    // Optional explanation files; classification and counts are always computed.
    bool tier2Notes = false;
    // Empty uses outDir/Tier2. References below are relative to the file carrying them.
    std::string tier2Dir;
    std::string tier2Reference;
    std::string productReferenceFromTier2;
    // Source boundary and, when available, its declaration file.
    std::string boundary;
    std::string boundarySpecPath;
    // Nonempty adds a pointer to the invocation saved in the run log.
    std::string command;
    // Zero falls back to counting properties in the supplied transports.
    std::size_t boundaryProperties = 0;
    // Saved annotations keyed by proofKey(); missing obligations print unknown.
    std::map<std::string, ProofLine> proofs;
    // Per-instance environment fingerprints and human-readable premise descriptions.
    std::map<std::string, std::string> environmentFingerprint;
    std::map<std::string, std::vector<std::string>> environmentShown;
    // Controls which transported assumptions are emitted.
    EnvironmentProfile environmentProfile;
    // Optional assumption-dependency descriptions for the printed proof comments.
    const AssumptionDependencyReport* discharge = nullptr;
    // Input order and include paths used to resolve package/$unit prerequisites.
    std::vector<std::string> inputFiles;
    std::vector<std::string> includePaths;
};

// Publish products atomically, remove stale generated files, and return counts.
// Publication or directory synchronization failures throw.
ModuleSpecStats writeModuleSpecs(const std::vector<PropertyLanding>& transports,
                                 const std::map<irep_idt, irep_idt>& aliases,
                                 const namespacet& ns,
                                 const ModuleSpecOptions& options);

}  // namespace lowering
}  // namespace veval

#endif  // VEVAL_LOWERING_MODULESPEC_H
