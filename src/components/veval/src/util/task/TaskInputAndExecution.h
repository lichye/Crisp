#pragma once
#include "util/database/Json.h"
#include <limits>
#include <sys/types.h>

namespace veval::workflow {
using namespace veval::result_validation;
void require(bool ok, const std::string &reason);
std::string readText(const fs::path &);
void writeText(const fs::path &, const std::string &);
std::string hashFile(const fs::path &, const fs::path &scratch);
J fileIdentity(const fs::path &, const fs::path &scratch);
std::string executable(const std::string &name);
std::string nativeHelper(const fs::path &binary, const std::string &name, const char *environment);
std::vector<std::string> sbyCommand();
std::vector<std::string> strings(const J &);
J stringArray(const std::vector<std::string> &);
double numeric(const J &, double fallback = 0);
J jsonFile(const fs::path &);
bool interrupted();

struct Process {
    fs::path root, out;
    double timeOrigin = monotonic();
    double deadline = std::numeric_limits<double>::infinity();
    pid_t parentToMonitor = 0;
    // Linux samples the process tree's RSS plus swap; zero adds no limit.
    uint64_t memoryBytes = 0;
    // Every invocation has its own receipt and an independent supervisor.
    J run(const std::string &stage, const std::vector<std::string> &argv,
          const fs::path &cwd = {});
    void reconcile();
};
struct Input {
    fs::path source, out;
    std::string task, mode, method, solver, top;
    unsigned depth = 20;
    double timeout = 0;
    bool multiclock = false;
    // Explicit veval_ladder depths; empty means the one `depth' rung.
    std::vector<unsigned> ladder;
    J units = list(), options = object();
};
Input prepareTask(Process &, const fs::path &source, const std::string &task);
void verifyTask(Process &, const Input &, const std::string &requestedTask);
J describeInput(const Input &);
Input restoreInput(const J &, const fs::path &out);
// The Job ladder for this task: [[name, mode, depth, null], ...].
J ladderJson(const Input &);
// Strategy, placement and engine settings in force, with the origin of
// each (cli, sby or default). Conflicting choices are refused here, before
// any generation starts.
J effectiveConfiguration(const Input &, const J &configuration);
// The task file SBY stages from: identical but for the veval_* options.
std::string withoutVevalOptions(const std::string &);
}
