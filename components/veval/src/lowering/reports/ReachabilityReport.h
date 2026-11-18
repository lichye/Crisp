// Per-authored-entry coverage, checked on export:
// full + partial + blocked + untouched == design_instances.
// Untouched means no observation arrived and no stop reason was recorded.

#ifndef VEVAL_LOWERING_REACHREPORT_H
#define VEVAL_LOWERING_REACHREPORT_H

#include <iosfwd>
#include <string>
#include <vector>

#include "lowering/propagation/PropertyLowering.h"

namespace veval {
namespace lowering {

struct ReachRow {
    // The authored entry, as the boundary specification names it.
    std::string entry;
    // Authored keyword (assume / property), not the per-landing direction.
    std::string kind;
    std::size_t designInstances = 0;

    struct Landing {
        std::string instance;
        std::size_t depth = 0;
        // binding / symeval, according to the recorded route.
        std::string via;
    };

    struct Partial {
        std::string instance;
        std::size_t depth = 0;
        std::vector<std::string> arrived;
        std::vector<PropertyLanding::Dropped> dropped;
        // Assume-only: sound-as-constraint / claimed-syntactic / refused.
        std::string grade;
    };

    struct Blocked {
        std::string instance;
        std::size_t depth = 0;
        std::string reason;
        std::vector<std::string> reasons;
    };

    std::vector<Landing> full;
    std::vector<Partial> partial;
    std::vector<Blocked> blocked;
    std::size_t untouched = 0;
};

// Rows sorted by authored entry, with landings sorted by instance.
std::vector<ReachRow> reachReport(const std::vector<PropertyLanding>&);

// Throws std::runtime_error if a row's partition does not match designInstances.
void writeJson(const std::vector<ReachRow>&, std::ostream&);

}  // namespace lowering
}  // namespace veval

#endif  // VEVAL_LOWERING_REACHREPORT_H
