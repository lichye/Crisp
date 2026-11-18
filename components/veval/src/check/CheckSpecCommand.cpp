#include "util/process/CurrentExecutablePath.h"
#include "util/cli/CheckCommands.h"
#include "check/run/RunPropertyChecks.h"
#include "util/process/RunExternalCommand.h"
#include <cmath>
#include <fcntl.h>
#include <iostream>
#include <sys/file.h>
#include <unistd.h>
#include <cstdlib>

extern char **environ;

namespace veval::workflow {
namespace {
double positive(const std::string &value) {
    size_t used = 0; double number = std::stod(value, &used);
    require(used == value.size() && std::isfinite(number) && number > 0, "expected positive finite number: " + value);
    return number;
}
}
static int run(int argc, char **argv) {
    const auto started = monotonic();
    const auto startedWall = wallTime();
    if (argc == 3 && std::string(argv[2]) == "--help") {
        std::cout << "veval check DESIGN.sby [TASK] [--out DIR] [--resume]\n"
                     "  --wall-cap S         cumulative active limit, tightened by SBY timeout\n"
                     "  --whole-design PATH  assert the inferred single root matches PATH\n"
                     "  --lowering DIR       reuse a veval lower bundle (default: shared sibling cache)\n"
                     "  --strategy auto|deepest|all-depth|top  which landings to check (default: auto)\n"
                     "  --placement single-path|split  module landing policy (default: single-path)\n"
                     "  Strategy and placement can also be set in [options] as\n"
                     "  veval_strategy and veval_placement; a command-line value wins.\n"
                     "  auto/deepest check the deepest landing on every generated branch.\n"
                     "  auto also includes Top-only cases; deepest excludes Top.\n"
                     "  After a counterexample, auto rechecks ancestors below Top, stopping\n"
                     "  at a proof or an inconclusive check. Use top for the starting DUT.\n"
                     "  Results describe the checked module; no source-level lifting is performed.\n"
                     "  --property LABEL     check only these source assertions (repeatable); every\n"
                     "                       source assumption and the reset contract are kept\n";
#ifdef __APPLE__
        std::cout << "macOS supervises one session per stage and applies no aggregate memory cap.\n";
#endif
        return 0;
    }
    require(argc >= 3, "usage: veval check DESIGN.sby [TASK] [--out DIR]");
    const auto source = fs::canonical(argv[2]);
    require(source.extension() == ".sby", "veval check expects a .sby file");
    Process process; process.root = fs::path(VEVAL_WORKFLOW_ROOT);
    // Unset strategy/placement fall back to the task, then to defaults.
    std::string selected, whole, placement, strategy, lowering;
    std::vector<std::string> chosen;
    double wall = 0; bool resume = false;
    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&] { require(i + 1 < argc, "missing value for " + arg); return std::string(argv[++i]); };
        if (arg == "--out") process.out = fs::absolute(next()).lexically_normal();
        else if (arg == "--resume") resume = true;
        else if (arg == "--inprocess-prove") throw std::runtime_error("the built-in EBMC solver has been removed; use the SBY check workflow");
        else if (arg == "--wall-cap") wall = positive(next());
        else if (arg == "--whole-design") whole = next();
        else if (arg == "--lowering") lowering = fs::absolute(next()).lexically_normal().string();
        else if (arg == "--reuse") throw std::runtime_error("proof reuse/lifting has been removed; omit --reuse");
        else if (arg == "--placement") { placement = next(); require(placement == "single-path" || placement == "split", "invalid placement"); }
        else if (arg == "--property") {
            chosen.push_back(next());
            require(!chosen.back().empty() && chosen.back().find_first_of(". \t") == std::string::npos,
                    "--property takes a bare source label");
        }
        else if (arg == "--strategy") {
            strategy = next();
            require(strategy == "auto" || strategy == "deepest" || strategy == "all-depth" || strategy == "top", "invalid strategy");
        }
        else { require(!arg.empty() && arg[0] != '-' && selected.empty(), "unexpected argument: " + arg); selected = arg; }
    }
    require(!resume || !process.out.empty(), "--resume requires --out DIR");
    if (process.out.empty()) process.out = process.root / "experiment/veval-check" /
        (source.stem().string() + "-" + std::to_string(::getpid()));
    require(resume ? fs::is_directory(process.out) : !fs::exists(process.out),
            resume ? "resume directory is missing" : "output directory already exists; use --resume or a new --out");
    fs::create_directories(process.out);
    int lock = ::open((process.out / "lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    require(lock >= 0 && ::flock(lock, LOCK_EX | LOCK_NB) == 0, "another check owns this directory");
    struct Guard { int fd; ~Guard() { ::close(fd); } } guard{lock};
    // Shutdown still has to read receipts and publish completed local checks.
    veval::util::command_runner::installCancellationHandlers();
    process.reconcile();
    const auto binary = veval::util::currentExecutable();
    // Use one explicit tool search path in both task expansion and the adapter.
    const auto oss = veval::util::command_runner::env("OSS_CAD_BIN", veval::util::command_runner::env("HOME") + "/.local/oss-cad-suite/bin");
    ::setenv("OSS_CAD_BIN", oss.c_str(), 1);
    const auto path = oss + ":" + veval::util::command_runner::env("PATH");
    ::setenv("PATH", path.c_str(), 1);
    for (const auto &helper : std::vector<std::pair<std::string, std::string>>{
        {"CONSFLOW_SVA_MONITOR", "consflow_sva_monitor"}, {"CONSFLOW_YOSYS_IDENTIFIER", "consflow_yosys_identifier"},
        {"CONSFLOW_OBSERVATION", "consflow_observation.so"}, {"CONSFLOW_LIVE2SAFETY", "consflow_live2safety.so"},
        {"CONSFLOW_LIVE_TRACE", "consflow_live_trace"}}) {
        if (veval::util::command_runner::env(helper.first.c_str()).empty())
            ::setenv(helper.first.c_str(), (binary.parent_path() / helper.second).c_str(), 1);
    }
    ::setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
    J environment = object();
    for (char **entry = environ; *entry; ++entry) {
        std::string value(*entry); const auto separator = value.find('='); auto key = value.substr(0, separator);
        if (key.rfind("CONSFLOW_", 0) == 0 || key.rfind("LD_", 0) == 0 || key == "PATH" ||
            key == "OSS_CAD_BIN" || key == "PYTHON" || key == "PYTHONPATH")
            put(environment, key, str(value.substr(separator + 1)));
    }
    J configuration = object({{"source", str(source.string())}, {"task", str(selected)},
        {"source_sha256", str(hashFile(source, process.out / "integrity"))},
        {"out", str(process.out.string())}, {"binary", fileIdentity(binary, process.out / "integrity")},
        {"environment", environment},
        {"result_scope", str("module")}, {"strategy", str(strategy)}, {"properties", stringArray(chosen)},
        {"whole_design", str(whole)}, {"placement", str(placement)}, {"inprocess_prove", boolean(false)},
        {"lowering",str(lowering)},
        {"wall_cap_s", num(wall)}});
    Journal journal(process, configuration, resume, started, startedWall);
    process.timeOrigin = monotonic() - journal.elapsed();
    if (wall) process.deadline = monotonic() + wall - journal.elapsed();
    std::cout << "veval check: " << source << " -> " << process.out << std::endl;
    return workflow(process, journal);
}
int main(int argc, char **argv) {
    try { return run(argc, argv); }
    catch (const std::exception &error) {
        std::cerr << "veval check: " << error.what() << '\n';
        return interrupted() ? 130 : 2;
    }
}
}
