// Runtime selection of transported environment assumptions.
#ifndef VEVAL_LOWERING_ENVIRONMENTPROFILE_H
#define VEVAL_LOWERING_ENVIRONMENTPROFILE_H

#include <algorithm>
#include <string>
#include <vector>

namespace veval {
namespace lowering {

enum class EnvironmentProfileMode { Full, Isolation, Allowlist };

struct EnvironmentAssumption {
    std::string instance;
    std::string property;
};

struct EnvironmentProfile {
    EnvironmentProfileMode mode = EnvironmentProfileMode::Full;
    std::vector<EnvironmentAssumption> assumptions;

    bool usesAssumption(const std::string& instance,
                        const std::string& property) const {
        if (mode == EnvironmentProfileMode::Full) return true;
        if (mode == EnvironmentProfileMode::Isolation) return false;
        return std::any_of(assumptions.begin(), assumptions.end(),
                           [&](const auto& listed) {
                               return listed.instance == instance &&
                                      listed.property == property;
                           });
    }

    bool isFull() const { return mode == EnvironmentProfileMode::Full; }

    std::string name() const {
        switch (mode) {
            case EnvironmentProfileMode::Full: return "full";
            case EnvironmentProfileMode::Isolation: return "isolation";
            case EnvironmentProfileMode::Allowlist: return "allowlist";
        }
        return "full";
    }
};

}  // namespace lowering
}  // namespace veval

#endif  // VEVAL_LOWERING_ENVIRONMENTPROFILE_H
