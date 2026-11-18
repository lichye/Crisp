#pragma once

#include <string>

namespace veval::sva { struct Monitor; }

namespace veval::lowering {
struct PropertyLanding;

// Exact checks the transported sentence, irrespective of its authored role.
// Obligation additionally excludes assumptions and initial-state contracts.
// SelfAssumption is the module-only specification used by entailment studies.
enum class LandingUse { Exact, Obligation, Assumption, SelfAssumption };
enum class LandingRefusal {
    None, Unplaced, Blocked, MissingMonitor, RoundTripFailed, Partial,
    ResetContract, AssumptionOnly, NotAssumption, MentionsInput
};
const char* toString(LandingRefusal);

// A borrowed view of an admitted landing. The transport must outlive this
// view and remain unchanged. A refused view cannot produce a child monitor.
// Printability is deliberately separate: native monitor proofs need no SVA.
class LandingAdmission {
public:
    explicit operator bool() const { return refusal_ == LandingRefusal::None; }
    LandingRefusal refusal() const { return refusal_; }
    sva::Monitor forChild(const std::string& childScope) const;

private:
    friend LandingAdmission admitLanding(const PropertyLanding&, LandingUse);
    LandingAdmission(const PropertyLanding& row, LandingRefusal refusal)
        : row_(&row), refusal_(refusal) {}
    const PropertyLanding* row_;
    LandingRefusal refusal_;
};

LandingAdmission admitLanding(const PropertyLanding&, LandingUse);
// Do not permit a borrowed view of a temporary transport.
LandingAdmission admitLanding(PropertyLanding&&, LandingUse) = delete;
LandingAdmission admitLanding(const PropertyLanding&&, LandingUse) = delete;
} // namespace veval::lowering
