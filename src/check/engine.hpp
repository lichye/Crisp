#pragma once

#include "front/front.hpp"

#include <set>
#include <string>
#include <string_view>

namespace crisp::check {

// Events travel over the task's pipe. Announce assertions before solving so
// their names survive a timeout or a failure in the engine.
struct Reporter {
    int fd;
    void send(std::size_t index, const char* status, const std::string& detail = {}) const;
};

// lower runs veval's lowering (spec 25) instead of the engine.
enum class EngineRun { validate_reset, solve, lower };

// Returns false when the task's undriven policy rejects the design. A solve
// run leaves out the assertions named in skip (veval decided them).
bool run_engine(const front::Task&, const std::filesystem::path& scratch, Reporter, EngineRun,
                const std::set<std::string>& skip = {});

// The wildcards of properties PATTERN (spec 20).
bool pattern_matches(std::string_view pattern, std::string_view name);

// veval's lowering: announces the selected assertions, reports assumptions an
// instance is checked without ("missing"), and writes one product per landing
// instance into scratch/veval (lower.cpp).
void lower_for_veval(const front::Task&, const std::filesystem::path& scratch, Reporter);

} // namespace crisp::check
