#pragma once
#include "../../util/task/CommandArgumentsAndFiles.h"

namespace veval::verification {
// Admission uses an existing aggregate cgroup limit; it never substitutes a
// per-process RLIMIT for a pool limit or changes the host's resource policy.
struct Pool {
    unsigned workers = 1;
    double estimate = 0, reserve = 0, peak = 0;
    fs::path group;
    unsigned long long limit = 0;
    explicit Pool(const Arguments &);
    void sample();
    // `workers' are the running adapters; each is charged for its whole
    // process tree (the adapter and the solver it starts).
    bool admit(const std::vector<int> &workers);
};
}
