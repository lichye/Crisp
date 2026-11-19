#ifndef VEVAL_COMBINATIONAL_DEFINITIONS_H
#define VEVAL_COMBINATIONAL_DEFINITIONS_H

#include <util/std_expr.h>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

class transition_systemt;

namespace veval::lowering::symbolic {

// Same-cycle definitions borrowed from the elaborated design. Never read trans()
// or expand a register, the target module, or one of its descendants.
class CombinationalDefinitions {
public:
    CombinationalDefinitions(const transition_systemt&,
                             const std::map<irep_idt, irep_idt>& aliases);

    std::map<irep_idt, exprt> expand(
        const std::vector<symbol_exprt>& observations,
        const std::string& target,
        const std::function<bool(const symbol_exprt&)>& available) const;

private:
    struct Definition { irep_idt symbol; const exprt* value; };
    const transition_systemt& design;
    const std::map<irep_idt, irep_idt>& aliases;
    std::map<irep_idt, std::vector<Definition>> definitions;
    std::map<exprt, std::vector<Definition>> memberDefinitions;
    std::map<irep_idt, std::string> owners;
    std::set<irep_idt> stateClasses;
    irep_idt key(const irep_idt&) const;
    exprt memberKey(exprt) const;
};

} // namespace veval::lowering::symbolic
#endif
