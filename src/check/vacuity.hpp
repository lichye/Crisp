#pragma once

#include "engine.hpp"

#include <ebmc/ebmc_properties.h>

#include <functional>
#include <map>
#include <set>

namespace crisp::check {

// Keep the implication printable as SVA when veval transports an assertion.
std::optional<exprt> local_implication(exprt original);
using ReportOriginal = std::function<void(const transition_systemt&, const ebmc_propertiest&, ebmc_propertiest::propertyt&)>;
using ReportVacuity = std::function<void(const irep_idt&, const char*, const std::string&)>;
void check_antecedents(const cmdlinet&, transition_systemt&, ebmc_propertiest&, const front::Task&,
                       Deadline, message_handlert&, const ReportOriginal&, const ReportVacuity&);

using Probes = std::map<irep_idt, ebmc_propertiest::propertyt>;
using Targets = std::set<irep_idt>;
using ExactResult = std::function<void(const ebmc_propertiest::propertyt&)>;
using AfterAssertions = std::function<Targets(ebmc_propertiest&)>;

inline bool refuted(const ebmc_propertiest::propertyt& p) {
    return p.is_refuted() || p.status == ebmc_propertiest::propertyt::statust::REFUTED_WITH_BOUND;
}

// A finite prefix can seed an independent antecedent search. The original
// assertion's published counterexample always requires a certified trace.
enum class TraceRequirement { finite_prefix, certified };
std::optional<trans_tracet> reconstruct_counterexample(
    const cmdlinet&, const transition_systemt&, const ebmc_propertiest&, const ebmc_propertiest::propertyt&,
    TraceRequirement, message_handlert&);

// Both batches use one unwind. The callback publishes original results and
// returns just the antecedents still needing an exact bounded query.
void bounded_checks(const cmdlinet&, transition_systemt&, ebmc_propertiest& assertions,
                    Probes&, std::size_t bound, const AfterAssertions&, const ExactResult&, message_handlert&);

} // namespace crisp::check
