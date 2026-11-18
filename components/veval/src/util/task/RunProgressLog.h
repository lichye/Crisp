#pragma once
#include "TaskInputAndExecution.h"

namespace veval::workflow {
double wallTime();
class Journal {
    Process &process;
    double epochStarted = monotonic();
    double priorActive = 0;
public:
    J value;
    Journal(Process &, const J &configuration, bool resume,
            double invocationStarted = monotonic(), double invocationWall = wallTime());
    void flush();
    bool done(const std::string &stage) const;
    void finish(const std::string &stage, const std::vector<fs::path> &files);
    void verify(const J &identities) const;
    void freezeInputs(const J &identities);
    void status(const std::string &, const std::string &reason = {});
    double elapsed() const;
};
std::vector<fs::path> filesUnder(const fs::path &);
}
