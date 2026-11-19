// Explicit netlist rewrite used by `veval freex`. Each X/Z/don't-care bit in
// a sized literal becomes a fresh undriven wire, giving it a free value on
// every cycle. Defined bits keep their values. Wires are not shared between
// bits or occurrences. This rewrite is not part of default lowering/checking.

#ifndef VEVAL_FRONTEND_FREEDONTCARE_H
#define VEVAL_FRONTEND_FREEDONTCARE_H

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace veval {
namespace frontend {

struct FreeDontCareResult {
    // Literals rewritten.
    std::size_t literals = 0;
    // Wires introduced: one per bit, so a 32-bit don't-care contributes 32.
    std::size_t wires = 0;
    // Modules that gained a declaration block.
    std::size_t modules = 0;
    // Names of the introduced free wires, in insertion order.
    std::vector<std::string> names;
};

// Rewrite a Yosys-emitted netlist from `in' into `out'. Insert declarations
// after each complete module header. Unsupported literal forms are rejected.
FreeDontCareResult freeDontCares(std::istream& in, std::ostream& out,
                                 const std::string& prefix = "veval_dc");

}  // namespace frontend
}  // namespace veval

#endif  // VEVAL_FRONTEND_FREEDONTCARE_H
