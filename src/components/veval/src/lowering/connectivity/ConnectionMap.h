// Port bindings recovered from elaborated invariant equalities.
// Lowering uses them to re-express parent signals in each child instance.

#ifndef VEVAL_LOWERING_CONNECTIONMAP_H
#define VEVAL_LOWERING_CONNECTIONMAP_H

#include "lowering/Symbolic/SignalAliases.h"

#include <util/std_expr.h>

#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <set>
#include <vector>

class transition_systemt;

namespace veval {
namespace lowering {

// How much of the parent's side a child port sees.
enum class BindingKind {
    Whole,     // `.p(sig)` — transports unchanged
    Partial,   // `.p(sig[3:0])`, a concatenation — transports over those bits
    Constant,  // `.p(1'b0)` — the property simplifies
    Computed,  // `.p(a & b)` — no direct binding substitution
};

std::string toString(BindingKind);

struct PortBinding {
    PortBinding(symbol_exprt _port, exprt _parentExpr, bool _isOutput,
                BindingKind _kind)
        : port(std::move(_port)),
          parentExpr(std::move(_parentExpr)),
          isOutput(_isOutput),
          kind(_kind) {}

    // Hierarchical name of the child's port, e.g. `top.u_c0.valid`.
    symbol_exprt port;
    // What the parent binds it to.
    exprt parentExpr;
    // True when the port drives the parent, false when the parent drives it.
    bool isOutput;
    BindingKind kind;

    // The parent-side symbols this binding mentions.
    std::vector<symbol_exprt> parentSymbols() const;

    // Distinct identifiers in first-occurrence order. Layouts and clock
    // matching instead need parentSymbols()'s occurrence multiplicity.
    std::vector<irep_idt> parentSymbolNames() const;
};

// A whole constant input or a contiguous constant bit range of an input.
// Ranges use zero-based packed bit positions, as do generated product ports.
struct ConstantBinding {
    symbol_exprt port;
    exprt value;
    std::optional<std::pair<std::size_t, std::size_t>> slice; // low bit, width
    exprt subject() const;
    exprt relation() const;
    std::string localName() const;
};

// Syntactic constants in a typed connection, after its width/sign conversion.
// Does not infer facts about signal values or module behaviour.
std::vector<ConstantBinding> constantBindingParts(const PortBinding&);

struct InstanceEdge {
    // Hierarchical name of the instance, e.g. `top.u_c0`.
    std::string instance;
    // Enclosing module instance, empty directly below the design top.
    // Escaped dots and generate scopes do not count as module boundaries.
    std::string parentInstance;
    std::string scope;
    std::size_t depth = 1;
    // Original module name, before per-instance elaboration.
    std::string module;
    std::vector<PortBinding> bindings;

    // Actual parameter values as decimal strings, for standalone elaboration.
    // Excludes localparams, which cannot appear in a #(...) override.
    std::vector<std::pair<std::string, std::string>> parameters;

    // Declared port names; bindings may also contain internal assignments.
    std::set<std::string> ports;
    std::set<std::string> inputs;
};

// Compare names, types and parameter values with a standalone elaboration.
// Returns an empty string on agreement, otherwise a description of a difference.
// This optional diagnostic walks both subtrees; it is not cached per edge.
std::string elaborationMismatch(const transition_systemt& parent,
                                const InstanceEdge&,
                                const transition_systemt& child,
                                const std::string& childTop);

// One edge per elaborated module instance below the design top.
std::vector<InstanceEdge> connectionMap(const transition_systemt&);

void writeJson(const std::vector<InstanceEdge>&, std::ostream&);
void writeText(const std::vector<InstanceEdge>&, std::ostream&);

}  // namespace lowering
}  // namespace veval

#endif  // VEVAL_LOWERING_CONNECTIONMAP_H
