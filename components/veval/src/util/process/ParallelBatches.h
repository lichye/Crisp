#pragma once
#include <algorithm>
#include <deque>
#include <future>
#include <thread>
#include <type_traits>

namespace veval::util {
// Bounded read-only producers; the caller consumes batches in input order.
// The caller counts toward the requested thread limit.
// Builders must not mutate shared IR or intern new identifiers.
template<class Build, class Consume>
void parallelBatches(std::size_t count, std::size_t batchSize, std::size_t workers,
                     Build build, Consume consume) {
    if (!count) return;
    workers = std::min({workers, std::size_t(std::max(1u, std::thread::hardware_concurrency())),
                        (count + batchSize - 1) / batchSize});
    if (workers <= 1) {
        for (std::size_t at = 0; at < count; at += batchSize)
            consume(at, build(at, std::min(count, at + batchSize)));
        return;
    }
    using Result = std::invoke_result_t<Build, std::size_t, std::size_t>;
    std::deque<std::future<Result>> pending;
    std::size_t next = 0;
    auto launch = [&] {
        const auto begin = next, end = std::min(count, begin + batchSize);
        pending.push_back(std::async(std::launch::async, [&, begin, end] { return build(begin, end); }));
        next = end;
    };
    for (std::size_t i = 1; i < workers; ++i) launch();
    for (std::size_t at = 0; !pending.empty(); at += batchSize) {
        auto result = pending.front().get();
        pending.pop_front();
        if (next < count) launch();
        consume(at, std::move(result));
    }
}
} // namespace veval::util
