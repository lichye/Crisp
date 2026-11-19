// Sampling: is a property's clocking event one the model can honour?
//
// The vendored frontend discards event controls: every flop steps on one
// global clock and every property is checked at every step. So a property
// written `@(negedge clk)', `@(clk)', with a nested second event, or
// `@(posedge clk2)' where clk2 clocks nothing in the design, was checked as a
// different property than written (srcProof/Differential/Sva S7). The source
// statement is read back by label and location, as FieldLowering does, and
// such a property is refused. When the statement cannot be located uniquely
// nothing is said: the check refuses only on positive evidence.
#ifndef VEVAL_SVA_SAMPLING_H
#define VEVAL_SVA_SAMPLING_H

#include <util/expr.h>
#include <util/irep.h>

#include <memory>
#include <string>

class transition_systemt;

namespace veval {
namespace sva {

class SamplingCheck {
public:
    explicit SamplingCheck(const transition_systemt&);
    ~SamplingCheck();
    // Why this property cannot be checked as written, or "" when it can (or
    // when the source statement could not be located).
    std::string refusal(const irep_idt& property, const exprt& original);
    // The net of an explicit `@(posedge X)' event, as the design names it
    // (`top.clk'), or "" when the statement or a single such event is not
    // located. Used to find each instance's port carrying that clock.
    std::string clock(const irep_idt& property, const exprt& original);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace sva
}  // namespace veval

#endif
