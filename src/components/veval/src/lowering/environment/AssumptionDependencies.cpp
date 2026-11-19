#include "AssumptionDependencies.h"
#include "util/database/JsonEscape.h"

#include <util/namespace.h>

#include <algorithm>
#include <ostream>
#include <set>

namespace veval {
namespace lowering {

namespace {

std::string escape(const std::string& text) {
    return veval::util::jsonEscape(text);
}

// Serialized status names are shared by reports and generated comments.
const char* const kProved = "proved";
const char* const kRefuted = "refuted";
const char* const kUndecided = "undecided";
const char* const kHostPromise = "host-promise";

const char* const kUnconditional = "unconditional";
const char* const kConditionalRefuted = "conditional-refuted";
const char* const kConditionalUndecided = "conditional-undecided";
const char* const kConditionalGranted = "conditional-granted";

// Use the same obligation filter as the product writer.
bool assertCopy(const PropertyLanding& transport) {
    return !transport.isResetContract && isTier1(transport) &&
           !usableAsAssumption(transport);
}

// A non-input observation is driven by this instance.
bool namesDriven(const PropertyLanding& transport) {
    return transport.placed && transport.result != LoweringCoverage::Blocked &&
           !transport.isResetContract && transport.roundTrips &&
           !transport.isAssumptionHere;
}

std::string moreSites(std::size_t others) {
    if (others == 0) return {};
    return " (+" + std::to_string(others) +
           (others == 1 ? " more site)" : " more sites)");
}

std::string plural(std::size_t count, const std::string& one,
                   const std::string& many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

// Pair equal source sentences across labels. If printing fails, retain the
// label as the key; never merge two unprintable sentences.
std::map<std::string, std::string> obligationKeys(
    const std::vector<PropertyLanding>& transports, const namespacet& ns) {
    std::map<std::string, std::string> sentence;
    for (const auto& transport : transports) {
        if (transport.isResetContract) continue;
        if (sentence.count(transport.property) != 0) continue;
        auto text = boundaryPropertyText(transport, ns);
        if (text.empty()) continue;
        sentence.emplace(transport.property, std::move(text));
    }

    std::map<std::string, std::string> keys;
    for (const auto& transport : transports) {
        if (keys.count(transport.property) != 0) continue;
        auto found = sentence.find(transport.property);
        keys.emplace(transport.property,
                     found == sentence.end()
                         ? transport.property
                         : "content:" + fingerprintOf(found->second));
    }
    return keys;
}

}  // namespace

const AssumptionDependency* AssumptionDependencyReport::at(
    const std::string& instance, const std::string& property) const {
    auto found = records.find(proofKey(instance, property));
    return found == records.end() ? nullptr : &found->second;
}

AssumptionDependencyReport buildAssumptionDependencies(
    const std::vector<PropertyLanding>& transports,
    const std::map<std::string, ProofLine>& verdicts,
    const std::map<std::string, std::string>& environmentFingerprint,
    const namespacet& ns) {
    AssumptionDependencyReport report;

    // Accept saved results only when both property and environment still match.
    auto settle = [&](const PropertyLanding& transport) {
        DischargeSite site{transport.instance, "unknown",
                           "not attempted: no engine has been asked; run "
                           "with --prove"};
        auto found =
            verdicts.find(proofKey(transport.instance, transport.property));
        if (found == verdicts.end()) return site;
        if (found->second.content != propertyFingerprint(transport, ns)) {
            site.detail =
                "not attempted: property text changed since last verdict";
            return site;
        }
        auto environment = environmentFingerprint.find(transport.instance);
        const auto& here = environment == environmentFingerprint.end()
                               ? std::string()
                               : environment->second;
        if (found->second.environment != here) {
            site.detail =
                "not attempted: the environment it was proved in changed "
                "since last verdict";
            return site;
        }
        site.status = found->second.status;
        site.detail = found->second.detail;
        if (transport.abortDropped && site.status == "refuted") {
            site.status = "unknown";
            site.detail = "abort-free body refuted; the guarded source premise remains unresolved";
        }
        return site;
    };

    // Pair by sentence; records remain addressable by instance and source label.
    const auto keys = obligationKeys(transports, ns);
    auto keyOf = [&keys](const PropertyLanding& transport) -> const std::string& {
        return keys.at(transport.property);
    };

    // Collect asserted sites, unasserted drivers, and premises granted at a driver.
    std::map<std::string, std::map<std::string, DischargeSite>> byProperty;
    std::map<std::string, std::set<std::string>> drivenNoAssert;
    std::map<std::string, DischargeSite> settled;
    std::set<std::string> grantedOverDriven;
    for (const auto& transport : transports) {
        if (!transport.isResetContract && usableAsAssumption(transport) &&
            namesDriven(transport))
            grantedOverDriven.insert(
                proofKey(transport.instance, transport.property));

        if (assertCopy(transport)) {
            auto site = settle(transport);
            site.property = transport.property;
            settled.emplace(proofKey(transport.instance, transport.property),
                            site);
            // Multiple routes to one instance share one verdict.
            byProperty[keyOf(transport)].emplace(transport.instance,
                                                 std::move(site));
        } else if (namesDriven(transport)) {
            // A driven but unasserted copy is advisory; it cannot discharge a premise.
            drivenNoAssert[keyOf(transport)].insert(transport.instance);
        }
    }

    // Record exactly the assumptions used in generated checks.
    for (const auto& transport : transports) {
        if (transport.isResetContract) continue;
        if (!usableAsAssumption(transport)) continue;

        // Multiple routes share one record; keep the first landing's metadata.
        const auto key = proofKey(transport.instance, transport.property);
        auto [entry, inserted] = report.records.try_emplace(key);
        if (!inserted) continue;
        auto& record = entry->second;
        record.property = transport.property;
        record.instance = transport.instance;
        record.grantedOverDriven = grantedOverDriven.count(key) != 0;

        auto found = byProperty.find(keyOf(transport));
        bool anyRefuted = false, anyProved = false;
        if (found != byProperty.end()) {
            record.discharge.reserve(found->second.size());
            for (const auto& [instance, site] : found->second) {
                record.discharge.push_back(site);
                if (site.status == kRefuted) anyRefuted = true;
                // Only an unbounded proof discharges a premise.
                if (site.status == kProved) anyProved = true;
            }
        }
        record.classification = record.discharge.empty() ? kHostPromise
                                : anyRefuted             ? kRefuted
                                : anyProved              ? kProved
                                                         : kUndecided;

        auto driven = drivenNoAssert.find(keyOf(transport));
        if (driven != drivenNoAssert.end())
            record.internallyDriven.assign(driven->second.begin(),
                                           driven->second.end());

        if (record.classification == kProved) report.proved++;
        else if (record.classification == kRefuted) report.refuted++;
        else if (record.classification == kUndecided) report.undecided++;
        else report.hostPromise++;
        if (!record.internallyDriven.empty()) report.drivenElsewhere++;
        if (record.grantedOverDriven) report.grantedHere++;
    }

    // All checks at one instance use the same premises.
    std::map<std::string, std::vector<const AssumptionDependency*>> premisesAt;
    for (const auto& [key, record] : report.records)
        premisesAt[record.instance].push_back(&record);

    // A proved premise must have a non-circular supporting proof. Compute the
    // least fixed point, starting with sites that need no other proved premise.
    std::set<std::string> grounded;
    // This detects cycles only. Other conditional tiers are reported locally,
    // not propagated transitively through the graph.
    auto premiseGrounded = [&](const AssumptionDependency& premise) {
        if (premise.classification != kProved || premise.grantedOverDriven) return true;
        for (const auto& site : premise.discharge)
            if (site.status == kProved && grounded.count(proofKey(site.instance, site.property)))
                return true;
        return false;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& [key, site] : settled) {
            if (site.status != kProved || grounded.count(key)) continue;
            auto premises = premisesAt.find(site.instance);
            const bool ready = premises == premisesAt.end() ||
                std::all_of(premises->second.begin(), premises->second.end(),
                            [&](const auto* premise) { return premiseGrounded(*premise); });
            if (ready) {
                grounded.insert(key);
                changed = true;
            }
        }
    }

    // Compute each instance's tier once, then apply it to its decided assertions.
    auto tierFor = [&](const std::string& instance) -> std::string {
        const auto premises = premisesAt.find(instance);
        if (premises == premisesAt.end()) return kUnconditional;
        bool anyRefuted = false, anyUndecided = false, anyGranted = false;
        for (const auto* premise : premises->second) {
            if (premise->classification == kRefuted) anyRefuted = true;
            else if (premise->classification == kUndecided || !premiseGrounded(*premise))
                anyUndecided = true;
            if (premise->grantedOverDriven) anyGranted = true;
        }

        // Tier precedence: refuted > undecided > granted > unconditional.
        return anyRefuted   ? kConditionalRefuted
             : anyUndecided ? kConditionalUndecided
             : anyGranted   ? kConditionalGranted
                            : kUnconditional;
    };
    std::map<std::string, std::string> tierAt;
    for (const auto& [key, site] : settled) {
        // An unknown result has no premise tier.
        if (site.status == "unknown") continue;
        auto [entry, inserted] = tierAt.try_emplace(site.instance);
        if (inserted) entry->second = tierFor(site.instance);
        const auto& tier = entry->second;
        report.premiseTier.emplace(key, tier);
        if (tier == kUnconditional) report.unconditional++;
        else if (tier == kConditionalRefuted) report.conditionalRefuted++;
        else if (tier == kConditionalGranted) report.conditionalGranted++;
        else report.conditionalUndecided++;
    }

    return report;
}

std::string describeAssumptionDependency(const AssumptionDependency& record) {
    // Also accept hand-built records with no sites.
    if (record.classification == kHostPromise || record.discharge.empty()) {
        // Distinguish a host promise from an assumption over an internally driven net.
        if (record.grantedOverDriven)
            return "assumed here; trusted (author's assumption over a net "
                   "this instance drives)";
        if (!record.internallyDriven.empty())
            return "assumed here; trusted (author's assumption)";
        return "assumed here; trusted (host promise, no internal owner)";
    }

    // Name a site that supplied the classification; a refutation takes precedence.
    const DischargeSite* decisive = &record.discharge.front();
    for (const auto& site : record.discharge)
        if ((record.classification == kRefuted && site.status == kRefuted) ||
            (record.classification == kProved && site.status == kProved)) {
            decisive = &site;
            break;
        }

    // Keep the aside separate so the product writer can wrap it as one unit.
    const auto granted =
        record.grantedOverDriven
            ? std::string(" (granted at a driver of what it names)")
            : std::string();

    const auto others = moreSites(record.discharge.size() - 1);
    if (record.classification == kRefuted)
        return "assumed here; discharged at " + decisive->instance +
               ": REFUTED" + others + granted;
    if (record.classification == kProved)
        return "assumed here; discharged at " + decisive->instance +
               ": proved" + others + granted;
    return "assumed here; undecided at " + decisive->instance + others +
           granted;
}

void writeJson(const AssumptionDependencyReport& report, std::ostream& out) {
    // Sort by property, then instance for stable report output.
    std::vector<const AssumptionDependency*> ordered;
    ordered.reserve(report.records.size());
    for (const auto& [key, record] : report.records) ordered.push_back(&record);
    std::sort(ordered.begin(), ordered.end(),
              [](const AssumptionDependency* a, const AssumptionDependency* b) {
                  return a->property == b->property
                             ? a->instance < b->instance
                             : a->property < b->property;
              });

    out << "{\n \"discharge\": [\n";
    for (std::size_t i = 0; i < ordered.size(); i++) {
        const auto& record = *ordered[i];
        out << "  {\"property\": \"" << escape(record.property)
            << "\", \"instance\": \"" << escape(record.instance)
            << "\", \"classification\": \"" << escape(record.classification)
            << "\", \"discharge\": [";
        for (std::size_t s = 0; s < record.discharge.size(); s++)
            out << (s ? ", " : "") << "{\"instance\": \""
                << escape(record.discharge[s].instance) << "\", \"status\": \""
                << escape(record.discharge[s].status) << "\", \"detail\": \""
                << escape(record.discharge[s].detail) << "\"}";
        out << "], \"internally_driven\": [";
        for (std::size_t d = 0; d < record.internallyDriven.size(); d++)
            out << (d ? ", " : "") << "\"" << escape(record.internallyDriven[d])
                << "\"";
        out << "], \"granted_over_driven\": "
            << (record.grantedOverDriven ? "true" : "false");
        out << "}" << (i + 1 == ordered.size() ? "\n" : ",\n");
    }
    out << " ],\n \"premise_tiers\": [\n";
    std::size_t written = 0;
    for (const auto& [key, tier] : report.premiseTier) {
        auto tab = key.find('\t');
        out << "  {\"instance\": \"" << escape(key.substr(0, tab))
            << "\", \"property\": \""
            << escape(tab == std::string::npos ? std::string()
                                               : key.substr(tab + 1))
            << "\", \"tier\": \"" << escape(tier) << "\"}"
            << (++written == report.premiseTier.size() ? "\n" : ",\n");
    }
    out << " ],\n \"summary\": {\"proved\": " << report.proved
        << ", \"refuted\": " << report.refuted << ", \"undecided\": "
        << report.undecided << ", \"host_promise\": " << report.hostPromise
        << ", \"driven_elsewhere\": " << report.drivenElsewhere
        << ", \"granted_over_driven\": " << report.grantedHere
        << ", \"unconditional\": " << report.unconditional
        << ", \"conditional_refuted\": " << report.conditionalRefuted
        << ", \"conditional_undecided\": " << report.conditionalUndecided
        << ", \"conditional_granted\": " << report.conditionalGranted
        << "}\n}\n";
}

void writeText(const AssumptionDependencyReport& report, std::ostream& out,
               bool recorded) {
    if (report.records.empty() && report.premiseTier.empty()) return;

    out << "discharge: " << plural(report.records.size(), "assumed landing",
                                   "assumed landings")
        << " -- " << report.proved << " proved, " << report.refuted
        << " REFUTED, " << report.undecided << " undecided, "
        << report.hostPromise << " host promise"
        << (report.hostPromise == 1 ? "" : "s") << "\n";
    if (report.drivenElsewhere != 0) {
        out << "discharge: " << report.drivenElsewhere
            << " of those name a net an instance inside the design drives "
               "and was never asked to assert -- trusted as the author's "
               "assumption";
        out << (recorded ? ", listed under `internally_driven' in "
                           "discharge.json\n"
                         : "\n");
    }
    if (report.grantedHere != 0)
        out << "discharge: " << report.grantedHere
            << " of those are granted AT A DRIVER of what they name -- no "
               "verdict at those instances is unconditional"
            << (recorded ? ", flagged `granted_over_driven' in "
                           "discharge.json\n"
                         : "\n");
    out << "premise tiers: "
        << plural(report.premiseTier.size(), "decided verdict",
                  "decided verdicts")
        << " -- " << report.unconditional << " unconditional, "
        << report.conditionalRefuted << " conditional-refuted, "
        << report.conditionalUndecided << " conditional-undecided, "
        << report.conditionalGranted << " conditional-granted\n";
}

}  // namespace lowering
}  // namespace veval
