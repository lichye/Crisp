#ifndef VEVAL_GENERATION_PROFILE_H
#define VEVAL_GENERATION_PROFILE_H

// Diagnostic only: no profile data is read by lowering or checking.
#include "util/database/JsonEscape.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace veval::profile {
using Clock = std::chrono::steady_clock;
inline double seconds(Clock::duration value) {
    return std::chrono::duration<double>(value).count();
}
inline std::string quoted(const std::string& value) {
    return "\"" + veval::util::jsonEscape(value) + "\"";
}
struct ProcessMemory {
    unsigned long long rssKiB = 0, hwmKiB = 0;
    bool haveRss = false, haveHwm = false;
};
inline ProcessMemory processMemory() noexcept {
    ProcessMemory memory;
    auto* status = std::fopen("/proc/self/status", "r");
    if (!status) return memory;
    char line[256];
    while (std::fgets(line, sizeof(line), status)) {
        unsigned long long value;
        if (std::sscanf(line, "VmRSS: %llu kB", &value) == 1) {
            memory.rssKiB = value;
            memory.haveRss = true;
        } else if (std::sscanf(line, "VmHWM: %llu kB", &value) == 1) {
            memory.hwmKiB = value;
            memory.haveHwm = true;
        }
        if (memory.haveRss && memory.haveHwm) break;
    }
    std::fclose(status);
    return memory;
}
struct Phase;
struct Sink;
inline thread_local Sink* active = nullptr;
struct Metrics {
    std::map<std::string, std::uint64_t> counts;
    std::map<std::string, double> measured;
    bool failed = false;
};
inline thread_local Metrics* collected = nullptr;
struct CollectWorkerMetrics {
    Sink* previousSink = active;
    Metrics* previousMetrics = collected;
    explicit CollectWorkerMetrics(Metrics* metrics) { active = nullptr; collected = metrics; }
    ~CollectWorkerMetrics() { active = previousSink; collected = previousMetrics; }
};
struct Sink {
    std::ofstream file;
    std::uint64_t sequence = 0, nextCall = 0;
    bool failed = false;
    void fail(const char* reason) noexcept {
        if (!failed) std::fprintf(stderr, "generation profile incomplete: %s\n", reason);
        failed = true;
    }
    const Clock::time_point started = Clock::now();
    std::vector<Phase*> stack;
    Sink* previous = active;
    explicit Sink(const std::string& path) {
        if (path.empty()) return;
        if (std::filesystem::exists(path))
            throw std::runtime_error("generation profile already exists: " + path);
        const auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        file.open(path);
        if (!file) throw std::runtime_error("cannot open generation profile: " + path);
        active = this;
    }
    ~Sink() { if (active == this) active = previous; }
};
inline bool enabled() { return active != nullptr && !active->failed; }

struct Phase {
    Sink* sink = active;
    const char* name;
    std::string subject;
    Clock::time_point started, lastProgress;
    std::uint64_t id = 0, parent = 0;
    double childSeconds = 0;
    int exceptions = std::uncaught_exceptions();
    bool stopped = false;
    std::map<std::string, std::uint64_t> counts;
    std::map<std::string, double> measured;
    Phase(const char* phase, const std::string& target = "") : name(phase) {
        if (!enabled()) { sink = nullptr; return; }
        try {
        subject = target;
        started = lastProgress = Clock::now();
        id = ++sink->nextCall;
        if (!sink->stack.empty()) parent = sink->stack.back()->id;
        sink->stack.push_back(this);
        event("begin");
        } catch (...) {
            sink->fail("diagnostic allocation/write failure");
            if (!sink->stack.empty() && sink->stack.back() == this) sink->stack.pop_back();
            sink = nullptr;
        }
    }
    Phase(const Phase&) = delete;
    Phase& operator=(const Phase&) = delete;
    void event(const char* kind) {
        if (sink->failed) return;
        const auto now = Clock::now();
        auto& out = sink->file;
        out << std::setprecision(15)
            << "{\"schema\":\"consflow-generation-profile/1\",\"seq\":"
            << ++sink->sequence << ",\"pid\":" << getpid()
            << ",\"run_id\":" << quoted(std::to_string(getpid()) + ":" +
                std::to_string(sink->started.time_since_epoch().count()))
            << ",\"call_id\":" << id << ",\"parent_call_id\":" << parent
            << ",\"phase\":" << quoted(name) << ",\"subject\":" << quoted(subject)
            << ",\"event\":" << quoted(kind)
            << ",\"monotonic_s\":" << seconds(now.time_since_epoch())
            << ",\"elapsed_s\":" << seconds(now - started)
            << ",\"process_elapsed_s\":" << seconds(now - sink->started);
        const auto memory = processMemory();
        if (memory.haveRss) out << ",\"rss_kib\":" << memory.rssKiB;
        if (memory.haveHwm) out << ",\"hwm_kib\":" << memory.hwmKiB;
        if (std::string(kind) == "end")
            out << ",\"inclusive_s\":" << seconds(now - started)
                << ",\"child_scope_s\":" << childSeconds
                << ",\"exclusive_s\":" << std::max(0.0, seconds(now - started) - childSeconds)
                << ",\"status\":" << quoted(std::uncaught_exceptions() > exceptions
                                                  ? "unwinding" : "returned");
        out << ",\"counters\":{";
        bool first = true;
        for (const auto& entry : counts) {
            if (!first) out << ','; first = false;
            out << quoted(entry.first) << ':' << entry.second;
        }
        out << "},\"aggregate_inclusive_s\":{";
        first = true;
        for (const auto& entry : measured) {
            if (!first) out << ','; first = false;
            out << quoted(entry.first) << ':' << entry.second;
        }
        out << "}}\n";
        out.flush();
        if (!out) sink->fail("write/flush failed");
    }
    void progress() {
        if (!sink || sink->failed || seconds(Clock::now() - lastProgress) < 5) return;
        lastProgress = Clock::now();
        try { event("progress"); }
        catch (...) { sink->fail("diagnostic progress write failed"); }
    }
    void stop() {
        if (!sink || stopped) return;
        stopped = true;
        try { event("end"); }
        catch (...) { sink->fail("diagnostic end write failed"); }
        sink->stack.pop_back();
        if (!sink->stack.empty())
            sink->stack.back()->childSeconds += seconds(Clock::now() - started);
    }
    ~Phase() { stop(); }
};
inline void count(const char* name, std::uint64_t value = 1) {
    if (collected) {
        try { collected->counts[name] += value; }
        catch (...) { collected->failed = true; }
        return;
    }
    if (!enabled() || active->stack.empty()) return;
    try { active->stack.back()->counts[name] += value; }
    catch (...) { active->fail("diagnostic counter allocation failed"); }
}
struct Measure {
    Phase* phase = enabled() && !active->stack.empty() ? active->stack.back() : nullptr;
    Metrics* metrics = collected;
    const char* name;
    Clock::time_point started;
    explicit Measure(const char* label) : name(label) {
        if (phase || metrics) started = Clock::now();
    }
    void stop() {
        if (metrics) {
            try {
                metrics->measured[name] += seconds(Clock::now() - started);
                ++metrics->counts[name];
            } catch (...) { metrics->failed = true; }
            metrics = nullptr;
        }
        if (!phase) return;
        try {
            phase->measured[name] += seconds(Clock::now() - started);
            ++phase->counts[name];
        } catch (...) { phase->sink->fail("diagnostic measurement allocation failed"); }
        phase = nullptr;
    }
    ~Measure() { stop(); }
};
inline void merge(const std::vector<Metrics>& workers) {
    if (!enabled() || active->stack.empty()) return;
    auto& phase = *active->stack.back();
    for (const auto& worker : workers) {
        if (worker.failed) active->fail("worker diagnostic allocation failed");
        try {
            for (const auto& [name, value] : worker.counts) phase.counts[name] += value;
            for (const auto& [name, value] : worker.measured) phase.measured[name] += value;
        } catch (...) { active->fail("worker diagnostic merge failed"); }
    }
}
template<class F> decltype(auto) timed(const char* name, F&& function) {
    if (!enabled()) return std::forward<F>(function)();
    Phase phase(name);
    return std::forward<F>(function)();
}
} // namespace veval::profile
#endif
