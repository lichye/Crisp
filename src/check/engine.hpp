#pragma once

#include "front/front.hpp"

namespace crisp::check {

// Events travel over the task's pipe. Announce assertions before solving so
// their names survive a timeout or a failure in the engine.
struct Reporter {
    int fd;
    void send(std::size_t index, const char* status, const std::string& detail = {}) const;
};

// Returns false when the task's undriven policy rejects the design.
bool run_engine(const front::Task&, const std::filesystem::path& scratch, Reporter);

} // namespace crisp::check
