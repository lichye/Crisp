#include "LandingEligibility.h"
#include "PropertyLowering.h"

#include <stdexcept>

namespace veval::lowering {
namespace {
LandingRefusal translationRefusal(const PropertyLanding& row) {
    if (row.result == LoweringCoverage::Blocked) return LandingRefusal::Blocked;
    if (!row.certified) return LandingRefusal::MissingMonitor;
    if (!row.roundTrips) return LandingRefusal::RoundTripFailed;
    return LandingRefusal::None;
}

LandingRefusal refusal(const PropertyLanding& row, LandingUse use) {
    if (!row.placed) return LandingRefusal::Unplaced;
    const auto translated = translationRefusal(row);
    if (translated != LandingRefusal::None) return translated;
    if (use == LandingUse::Exact || use == LandingUse::Obligation) {
        if (row.result != LoweringCoverage::Full) return LandingRefusal::Partial;
    }
    if (use == LandingUse::Exact) return LandingRefusal::None;

    // Initial-instant facts must never become always-assumptions.
    if (row.isResetContract) return LandingRefusal::ResetContract;
    if ((row.abortDropped || row.monitor.hasLocalVariables) &&
        (use == LandingUse::Assumption || use == LandingUse::SelfAssumption))
        return LandingRefusal::NotAssumption;
    const bool assumed = row.isAssumption || row.isAssumptionHere;
    switch (use) {
        case LandingUse::Obligation:
            return assumed ? LandingRefusal::AssumptionOnly : LandingRefusal::None;
        case LandingUse::Assumption:
            return assumed ? LandingRefusal::None : LandingRefusal::NotAssumption;
        case LandingUse::SelfAssumption:
            return row.mentionsInput ? LandingRefusal::MentionsInput : LandingRefusal::None;
        case LandingUse::Exact:
            break;
    }
    throw std::logic_error("invalid landing use");
}
} // namespace

LandingAdmission admitLanding(const PropertyLanding& row, LandingUse use) {
    return LandingAdmission(row, refusal(row, use));
}

sva::Monitor LandingAdmission::forChild(const std::string& childScope) const {
    if (!*this)
        throw std::logic_error(std::string("refused landing: ") + toString(refusal_));
    return rerootToChild(*row_, childScope);
}

AssumeGrade assumeGrade(const PropertyLanding& row) {
    if (row.abortDropped || row.monitor.hasLocalVariables) return AssumeGrade::Refused;
    if (translationRefusal(row) != LandingRefusal::None) return AssumeGrade::Refused;
    // A certified partial monitor carries a sound weaker constraint, but
    // cannot establish the full source assertion. Coverage and use differ.
    return row.result == LoweringCoverage::Full ? AssumeGrade::ClaimedSyntactic
                                               : AssumeGrade::SoundAsConstraint;
}

bool usableAsAssumption(const PropertyLanding& row) {
    return bool(admitLanding(row, LandingUse::Assumption));
}

const char* toString(LandingRefusal reason) {
    switch (reason) {
        case LandingRefusal::None: return "admitted";
        case LandingRefusal::Unplaced: return "not placed here";
        case LandingRefusal::Blocked: return "transport blocked";
        case LandingRefusal::MissingMonitor: return "no transported monitor";
        case LandingRefusal::RoundTripFailed: return "round trip failed";
        case LandingRefusal::Partial: return "incomplete transport";
        case LandingRefusal::ResetContract: return "initial-state contract";
        case LandingRefusal::AssumptionOnly: return "assumed here";
        case LandingRefusal::NotAssumption: return "not an assumption here";
        case LandingRefusal::MentionsInput: return "requires contextual inputs";
    }
    throw std::logic_error("invalid landing refusal");
}
} // namespace veval::lowering
