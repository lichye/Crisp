#ifndef VEVAL_LOWERING_SYMBOLIC_SIGNALALIASES_H
#define VEVAL_LOWERING_SYMBOLIC_SIGNALALIASES_H

#include <util/irep.h>
#include <map>

class transition_systemt;

namespace veval {
namespace lowering {

// Signals that are the same net under different names, as symbol -> the
// representative of its class.
//
// Sigma sees port bindings and nothing else, and a designer almost never
// hands a child's port straight to the boundary. PLENA writes
//
//     instr_mem_inst   .instr_tl_a_valid_o (instr_tl_a_valid),
//     assign instr_mem_tl_a_valid_o = instr_tl_a_valid;
//
// so the chip's output IS the child's output port, one rename away. Without
// following the rename, transport reports "does not reach this instance" --
// truthfully in its own terms and quite wrongly about the design. On PLENA
// that was 44 of 46 properties.
//
// The classes are the connected components of the symbol-to-symbol
// equalities among the state constraints. Those hold at every step, so a
// name in a class denotes the same value as any other.
std::map<irep_idt, irep_idt> signalAliases(const transition_systemt&);

}  // namespace lowering
}  // namespace veval

#endif
