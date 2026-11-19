#pragma once

#include <algorithm>
#include <cstddef>

namespace veval::util {
template<class Build, class Consume>
void parallelBatches(std::size_t count, std::size_t batchSize, std::size_t,
                     Build build, Consume consume) {
    for (std::size_t at = 0; at < count; at += batchSize)
        consume(at, build(at, std::min(count, at + batchSize)));
}
} // namespace veval::util
