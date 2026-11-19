#ifndef VEVAL_LOWERING_SYMBOLIC_BITREWRITING_H
#define VEVAL_LOWERING_SYMBOLIC_BITREWRITING_H

#include <util/mp_arith.h>
#include <util/std_expr.h>

#include <map>
#include <vector>

namespace veval {
namespace lowering {

// One run of a parent signal that a child port carries:
// `port[portLo +: width] == sig[sigLo +: width]'. Direction does not matter --
// it is an equality among the state constraints either way.
//
// A binding is a LIST of these, because a port is often a concatenation of
// pieces of one bundle rather than one contiguous slice of it. On PLENA every
// partial binding into a datapath module is such a concatenation, so handling
// only the single-run case carried almost nothing.
//
// A run belongs to ONE signal. A port bound to `{s_1, s_0}' therefore has one
// run per member, each recorded against the member it carries, and the source
// signal of every run is on the record -- see PropertyLanding::ClaimedRebase, which
// is what the certificate exports.
//
// Public because a certificate has to state the arithmetic it claims, so an
// outside checker can redo it rather than believe it.
struct Segment {
    mp_integer portLo;
    mp_integer sigLo;
    mp_integer width;
};

namespace symbolic {

struct ClaimedRebase {
    symbol_exprt name;
    symbol_exprt port;
    std::vector<Segment> segments;
    // The fixed layout covers every source bit. A read can use a
    // contiguous field or concatenate several fields, with a deterministic
    // lowest-position choice for duplicated bits. This is coverage, not
    // a bijection or proof that a signed whole expression is printable.
    bool wholeSignal = false;
    // ... and a read of the signal as a whole was in fact rewritten that
    // way, rather than every read of it naming bits explicitly.
    //
    // Coverage above is about the binding; this records whether an
    // actual whole-value read was rebuilt. Exact unsigned contiguous reads
    // are normalized during the inverse check. Signed values/selects are
    // kept distinct, and unsupported signing is refused.
    bool wholeRead = false;
};
// Width and value-preserving expression rewrites used during lowering.
mp_integer bitWidth(const typet& type);
exprt complementOf(const exprt& expression);
bool rebase(exprt& expression, const irep_idt& source,
            const symbol_exprt& port, const std::vector<Segment>& segments,
            bool wholeSignal, bool* wholeRead = nullptr, bool printing = false);
bool signedWholeRead(const exprt& expression, const irep_idt& source);

using RebasesByPort = std::map<irep_idt, std::vector<const ClaimedRebase*>>;
bool unrebase(exprt& expression, const RebasesByPort& byPort);

void normalizeContiguousReads(exprt& expression);
void dropWidthPreservingCasts(exprt& expression);
void simplifyComplements(exprt& expression);
void renameAtoms(exprt& expression, const std::map<irep_idt, irep_idt>& rename);
void complementAtoms(exprt& expression,
                     const std::map<irep_idt, irep_idt>& complemented);

}  // namespace symbolic
}  // namespace lowering
}  // namespace veval

#endif
