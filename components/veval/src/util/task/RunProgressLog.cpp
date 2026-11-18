#include "RunProgressLog.h"
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/time.h>
#endif
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace veval::workflow {
std::vector<fs::path> filesUnder(const fs::path &root) {
    std::vector<fs::path> paths;
    if (fs::is_regular_file(root)) paths.push_back(root);
    else if (fs::is_directory(root)) for (const auto &entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file()) paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end());
    return paths;
}
static std::string utc() {
    auto now = std::time(nullptr); std::tm t{}; gmtime_r(&now, &t);
    std::ostringstream s; s << std::put_time(&t, "%Y-%m-%dT%H:%M:%SZ"); return s.str();
}
static std::string bootIdentity() {
#ifdef __APPLE__
    timeval boot{};
    size_t length = sizeof boot;
    require(::sysctlbyname("kern.boottime", &boot, &length, nullptr, 0) == 0 && length == sizeof boot,
            "cannot read macOS boot identity");
    return std::to_string(boot.tv_sec) + "." + std::to_string(boot.tv_usec);
#else
    return trim(readText("/proc/sys/kernel/random/boot_id"));
#endif
}
double wallTime() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
Journal::Journal(Process &p, const J &configuration, bool resume, double invocationStarted, double invocationWall)
    : process(p), epochStarted(invocationStarted) {
    if (resume) {
        value = jsonFile(process.out / "manifest.json");
        require(text(value["schema"]) == "veval-check/1", "unsupported check manifest");
        require(equal(value["configuration"], configuration), "run configuration changed; choose a new --out directory");
        verify(value["inputs"]);
        for (const auto &stage : members(value["stages"])) verify(stage.second["outputs"]);
        // Never below zero: a corrupted negative value would give budget back.
        priorActive = std::max(0.0, numeric(value["active_s"]));
        if (text(value["status"]) == "running") {
            // An unclean exit has no trustworthy end-of-invocation timestamp.
            // Conservatively charge the gap; never give spent budget back.
            const bool sameBoot = text(value["boot_id"]) == bootIdentity();
            if (sameBoot) {
                // This invocation already charges startup and verification via
                // epochStarted. Charge the uncertain gap only up to its start.
                if (value["checkpoint_monotonic"].is_number())
                    priorActive += std::max(0.0, invocationStarted - numeric(value["checkpoint_monotonic"]));
                else priorActive += numeric(value["total_budget_s"]);
            }
            else {
                const auto gap = invocationWall - numeric(value["checkpoint_unix_s"], -1);
                // Wall time survives reboot. If absent or moved backwards, do
                // not invent a smaller elapsed interval from an unsafe clock.
                if (value["checkpoint_unix_s"].is_number() && gap >= 0)
                    priorActive += gap;
                else priorActive += numeric(value["total_budget_s"]);
            }
            put(value, "timing_incomplete", boolean(true));
        }
    } else {
        value = object({{"schema", str("veval-check/1")}, {"configuration", configuration},
            {"started_utc", str(utc())}, {"inputs", list()}, {"stages", object()},
            {"epochs", list()}, {"events", list()}, {"status", str("running")},
            {"boot_id", str(bootIdentity())},
            {"active_s", num(0)}, {"timing_incomplete", boolean(false)}});
    }
    put(value, "boot_id", str(bootIdentity()));
    append(value.make_object()["epochs"], object({{"started_utc", str(utc())}, {"resume", boolean(resume)},
        {"prior_active_s", num(priorActive)}}));
    status("running");
}
double Journal::elapsed() const { return priorActive + monotonic() - epochStarted; }
void Journal::flush() {
    put(value, "active_s", num(elapsed()));
    put(value, "checkpoint_monotonic", num(monotonic()));
    put(value, "checkpoint_unix_s", num(wallTime()));
    J commands = list();
    const auto directory = process.out / "commands";
    if (fs::exists(directory)) for (const auto &entry : fs::directory_iterator(directory)) {
        const auto receipt = entry.path() / "command.json";
        if (fs::is_regular_file(receipt)) append(commands, jsonFile(receipt));
    }
    put(value, "commands", commands);
    save(process.out / "manifest.json", value);
}
bool Journal::done(const std::string &stage) const { return value["stages"][stage]["complete"].is_true(); }
void Journal::verify(const J &identities) const {
    for (const auto &identity : items(identities)) {
        auto path = text(identity["path"]);
        require(fs::exists(path) && fs::canonical(path).string() == text(identity["resolved_path"], path),
                "input/evidence path changed: " + path + "; choose a new --out directory");
        if (identity["directory"].is_true()) {
            std::vector<std::string> names;
            for (const auto &file : filesUnder(path)) names.push_back(file.lexically_relative(path).string());
            require(equal(stringArray(names), identity["files"]), "input directory contents changed: " + path + "; choose a new --out directory");
            continue;
        }
        require(fs::is_regular_file(path) && hashFile(path, process.out / "integrity") == text(identity["sha256"]),
                "input/evidence changed: " + path + "; choose a new --out directory");
    }
}
void Journal::freezeInputs(const J &identities) {
    verify(identities); put(value, "inputs", identities); flush();
}
void Journal::finish(const std::string &stage, const std::vector<fs::path> &files) {
    J refs = list();
    for (const auto &file : files) append(refs, fileIdentity(file, process.out / "integrity"));
    put(value.make_object()["stages"], stage, object({{"complete", boolean(true)},
        {"active_s", num(elapsed())}, {"outputs", refs}}));
    flush();
}
void Journal::status(const std::string &status, const std::string &reason) {
    put(value, "status", str(status)); put(value, "reason", reason.empty() ? J() : str(reason));
    if (status != "running") put(value, "finished_utc", str(utc()));
    else put(value, "finished_utc", J());
    flush();
}
}
