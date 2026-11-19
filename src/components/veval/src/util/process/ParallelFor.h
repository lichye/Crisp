#pragma once

#include <cstddef>
#include <iostream>

namespace veval::util {
// Normalize compatibility options at the CLI boundary, before spawning stages.
inline std::size_t effectiveGenerationThreads(std::size_t requested) {
    if (requested > 1) std::cerr << "veval 当前单线程运行\n";
    return 1;
}

// Shared CBMC expressions and interned identifiers are owned by one thread.
inline std::size_t workerCount(std::size_t count, std::size_t) {
    return count ? 1 : 0;
}

template<class Work>
void parallelFor(std::size_t count, std::size_t, Work work) {
    for (std::size_t i = 0; i < count; ++i) work(i, 0);
}
} // namespace veval::util
