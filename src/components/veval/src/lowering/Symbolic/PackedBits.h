#ifndef VEVAL_LOWERING_PACKEDBITS_H
#define VEVAL_LOWERING_PACKEDBITS_H

#include <util/arith_tools.h>
#include <util/bitvector_expr.h>
#include <util/bitvector_types.h>
#include <util/std_expr.h>
#include <vector>

namespace veval {
namespace lowering {
struct SelectedBit {
    symbol_exprt port;
    mp_integer position;
};
// Input is least-significant source bit first. Return packed slices MSB first.
// This is reconstruction only; callers independently establish correspondence
// and retain their own expression signedness/printing policy.
inline exprt::operandst packedSlices(const std::vector<SelectedBit>& selected,
                                    const typet& indexType, bool boolean) {
    struct Run { symbol_exprt port; mp_integer low, width; };
    std::vector<Run> runs;
    for (const auto& bit : selected) {
        if (!runs.empty() && runs.back().port == bit.port &&
            runs.back().low + runs.back().width == bit.position)
            ++runs.back().width;
        else runs.push_back({bit.port, bit.position, 1});
    }
    exprt::operandst pieces;
    for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
        const auto index = from_integer(it->low, indexType);
        if (boolean) pieces.push_back(extractbit_exprt{it->port, index});
        else pieces.push_back(extractbits_exprt{it->port, index,
            unsignedbv_typet{numeric_cast_v<std::size_t>(it->width)}});
    }
    return pieces;
}
}
}
#endif
