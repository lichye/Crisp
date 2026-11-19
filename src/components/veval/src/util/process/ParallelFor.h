#pragma once
#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace veval::util {
inline std::size_t workerCount(std::size_t count, std::size_t requested) {
    return std::min(count, std::max(std::size_t{1}, requested));
}

// Each index runs once; workers own their scratch state and output slots.
// The caller is worker zero. All workers join before exceptions propagate.
template<class Work>
void parallelFor(std::size_t count, std::size_t requested, Work work) {
    const auto workers = workerCount(count, requested);
    if (workers <= 1) {
        for (std::size_t i = 0; i < count; ++i) work(i, 0);
        return;
    }
    std::atomic<std::size_t> next{0};
    std::atomic<bool> cancelled{false};
    std::exception_ptr failure;
    std::mutex mutex;
    auto run = [&](std::size_t worker) {
        try {
            while (!cancelled.load(std::memory_order_relaxed)) {
                const auto i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= count) break;
                work(i, worker);
            }
        } catch (...) {
            std::lock_guard lock(mutex);
            if (!failure) failure = std::current_exception();
            cancelled.store(true, std::memory_order_relaxed);
        }
    };
    std::vector<std::thread> threads;
    threads.reserve(workers - 1);
    try {
        for (std::size_t i = 1; i < workers; ++i) threads.emplace_back(run, i);
    } catch (...) {
        cancelled.store(true, std::memory_order_relaxed);
        for (auto &thread : threads) thread.join();
        throw;
    }
    run(0);
    for (auto &thread : threads) thread.join();
    if (failure) std::rethrow_exception(failure);
}
} // namespace veval::util
