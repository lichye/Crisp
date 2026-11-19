#pragma once
#include <map>
#include <set>
#include <string>
#include <utility>

namespace veval::verification {
using SourceInstance = std::pair<std::string, std::string>;

// A landing is redundant only when the same source has a deeper check on
// that branch. Use recorded parents, not path spelling or global depth.
inline std::set<SourceInstance> deepestBranchLandings(
    const std::set<SourceInstance> &candidates,
    const std::map<std::string, std::string> &parents) {
    auto leaves = candidates;
    std::set<SourceInstance> visited;
    for (const auto &[source, instance] : candidates) {
        auto parent = parents.find(instance);
        while (parent != parents.end() && !parent->second.empty()) {
            const SourceInstance ancestor{source, parent->second};
            if (!visited.insert(ancestor).second) break;
            leaves.erase(ancestor);
            parent = parents.find(parent->second);
        }
    }
    return leaves;
}
}
