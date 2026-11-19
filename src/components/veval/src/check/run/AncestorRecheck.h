#pragma once
#include "SelectDeepestBranches.h"

namespace veval::verification {
// Only a real counterexample starts another, independent check. Top is an
// explicit strategy; gaps in executable landings do not change ancestry.
inline std::string ancestorRecheck(
    const SourceInstance &current, const std::string &outcome, const std::string &verdict,
    const std::map<SourceInstance, std::string> &jobs,
    const std::map<std::string, std::string> &parents, const std::string &top) {
    if (outcome != "completed" || verdict != "refuted" || current.second == top) return {};
    auto parent = parents.find(current.second);
    std::set<std::string> visited{current.second};
    while (parent != parents.end() && !parent->second.empty() && parent->second != top) {
        if (!visited.insert(parent->second).second) return {};
        const auto job = jobs.find({current.first, parent->second});
        if (job != jobs.end()) return job->second;
        parent = parents.find(parent->second);
    }
    return {};
}
}
