#include "util/process/CurrentExecutablePath.h"
#include "../task/TaskInputAndExecution.h"
#include "util/cli/CheckCommands.h"
#include "../task/RunProgressLog.h"
#include "util/process/RunExternalCommand.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include "util/process/RunWithResourceLimits.h"
namespace veval::workflow {
namespace {
// The supervisor keeps running when its caller is killed. On macOS an
// orphan is reparented, so this comparison cannot accept a recycled PID.
pid_t stageOwner = 0;
bool stageCancelled() {
    return interrupted() || ::getppid() != stageOwner;
}
}
int executeOwned(int, char **) { throw std::runtime_error("Linux stage launcher unavailable on macOS"); }
int superviseCommand(int, char **) { throw std::runtime_error("nested check supervision unavailable on macOS"); }
void Process::reconcile() {
    for (const auto &path : filesUnder(out / "commands")) {
        if (path.filename() != "command.json") continue;
        const auto record = jsonFile(path);
        if (text(record["schema"]) == "veval-check-command/1" && !record["cleanup_confirmed"].is_true())
            throw std::runtime_error("cannot confirm cleanup of an interrupted macOS stage: " + path.string());
    }
}
J Process::run(const std::string &stage, const std::vector<std::string> &argv, const fs::path &cwd) {
    require(!argv.empty(), "empty stage command");
    const auto ownerExited = [&] { return parentToMonitor > 0 && ::getppid() != parentToMonitor; };
    require(!interrupted() && !ownerExited(), "verification interrupted");
    require(memoryBytes == 0, "process-tree memory limits are unavailable on macOS");
    if (deadline <= monotonic()) throw Expired("task wall deadline exhausted");
    const auto started = monotonic();
    const auto serial = std::to_string(::getpid()) + "-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory = out / "commands" / serial;
    fs::create_directories(directory);
    const auto working = cwd.empty() ? root : cwd;
    const bool limited = std::isfinite(deadline);
    J record = object({{"schema", str("veval-check-command/1")}, {"stage", str(stage)},
                       {"argv", stringArray(argv)}, {"directory", str(directory.string())},
                       {"cwd", str(working.string())}, {"started_monotonic", num(started)},
                       {"timeout_s", limited ? num(deadline - started) : J()},
                       {"task_deadline_monotonic", limited ? num(deadline) : J()},
                       {"timeout_scope", str(limited ? "task" : "none")},
                       {"memory_max_bytes", J()}, {"supervision_scope", str("session")},
                       {"status", str("starting")},
                       {"cleanup_confirmed", boolean(false)}});
    save(directory / "command.json", record);
    const auto owner = ::getpid();
    const auto supervisor = ::fork();
    if (supervisor == 0) {
        stageOwner = owner;
        try {
            const auto result = verification::execute(argv, directory / "stdout.log", working,
                deadline, 0, 5, false, directory / "stderr.log", stageCancelled);
            put(record, "cleanup_confirmed", boolean(result.cleanupComplete));
            put(record, "wall_s", num(result.elapsed));
            put(record, "exit_code", num(result.exitCode));
            put(record, "memory_peak_bytes", J());
            put(record, "single_process_peak_kb", num(result.maxRssKb));
            const char *status = "failed";
            if (result.cleanupComplete) {
                if (result.stopped || stageCancelled()) status = "interrupted";
                else if (result.timedOut) status = "timeout";
                else if (result.exitCode == 0) status = "completed";
            }
            put(record, "status", str(status));
            if (!result.error.empty()) put(record, "reason", str(result.error));
            save(directory / "command.json", record);
            ::_exit(result.cleanupComplete ? 0 : 2);
        } catch (const std::exception &e) {
            put(record, "status", str("failed"));
            put(record, "reason", str(e.what()));
            try { save(directory / "command.json", record); } catch (...) {}
            ::_exit(2);
        }
    }
    require(supervisor > 0, "cannot fork stage supervisor");
    bool forwarded = false;
    int status = 0;
    for (;;) {
        const auto waited = ::waitpid(supervisor, &status, WNOHANG);
        if (waited == supervisor) break;
        if (waited < 0 && errno == EINTR) continue;
        require(waited == 0, "cannot wait for stage supervisor");
        if ((interrupted() || ownerExited()) && !forwarded) {
            ::kill(supervisor, SIGTERM);
            forwarded = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    record = jsonFile(directory / "command.json");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0 && record["cleanup_confirmed"].is_true(),
        "cannot confirm cleanup of " + stage + "; see " + directory.string());
    return record;
}
}
#else
#include "util/process/ChildProcess.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <set>

namespace veval::workflow {
namespace task = veval::util::command_runner;
namespace {
struct Fd {
    int value;
    explicit Fd(int n) : value(n) {}
    ~Fd() { if (value >= 0) ::close(value); }
};
void closeInheritedDescriptors(int stageLock) {
    // The forked supervisor does not exec: explicitly release the caller's
    // database descriptors and run lock. Otherwise killing the caller leaves
    // its run locked until the supervisor finishes cleaning up, blocking resume.
    DIR *directory = ::opendir("/proc/self/fd");
    require(directory != nullptr, "cannot inspect inherited stage descriptors");
    const int listing = ::dirfd(directory);
    while (const auto *entry = ::readdir(directory)) {
        char *end = nullptr;
        const long parsed = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end || parsed < 0) continue;
        const int fd = static_cast<int>(parsed);
        if (fd == stageLock || fd == listing) continue;
        // A caller with closed stdin can have its run lock at descriptor 0.
        if (fd > STDERR_FILENO || (::fcntl(fd, F_GETFD) & FD_CLOEXEC)) ::close(fd);
    }
    ::closedir(directory);
}
std::string birth(pid_t pid) {
    std::ifstream stream("/proc/" + std::to_string(pid) + "/stat"); std::string value;
    if (!std::getline(stream, value)) return {};
    const auto end = value.rfind(')'); if (end == value.npos) return {};
    const auto fields = words(value.substr(end + 1));
    return fields.size() > 19 && fields[0] != "Z" && fields[0] != "X" && fields[0] != "x" ? fields[19] : "";
}
std::vector<pid_t> children(pid_t pid) {
    std::set<pid_t> result;
    std::error_code error;
    const auto directory = fs::path("/proc") / std::to_string(pid) / "task";
    fs::directory_iterator it(directory, error);
    if (error == std::errc::no_such_file_or_directory || error == std::errc::no_such_process) return {};
    require(!error, "cannot read owned process threads: " + error.message());
    for (const auto &thread : it) {
        std::ifstream stream(thread.path() / "children");
        if (!stream) {
            require(!fs::exists(thread.path()), "cannot read owned process children: " + thread.path().string());
            continue;
        }
        pid_t child;
        while (stream >> child) if (child > 0) result.insert(child);
    }
    return {result.begin(), result.end()};
}
uint64_t residentTree() {
    // A sampled sum, not cgroup accounting: shared pages may be counted twice.
    // Include swap so swapping cannot evade the requested working-set limit.
    auto pending = children(::getpid()); std::set<pid_t> seen; uint64_t bytes = 0;
    for (size_t i = 0; i < pending.size(); ++i) {
        const auto pid = pending[i]; if (!seen.insert(pid).second) continue;
        std::ifstream stream("/proc/" + std::to_string(pid) + "/status");
        std::string row;
        while (std::getline(stream, row)) {
            if (row.rfind("VmRSS:", 0) != 0 && row.rfind("VmSwap:", 0) != 0) continue;
            std::istringstream fields(row); std::string key; uint64_t kb = 0;
            if (fields >> key >> kb) bytes += kb * 1024;
        }
        const auto nested = children(pid); pending.insert(pending.end(), nested.begin(), nested.end());
    }
    return bytes;
}
// Only unreaped, direct children are signalled. Their PIDs cannot be reused.
// Killing their parents reparents even double-forked/new-session descendants
// to this dedicated subreaper, so a later sweep owns those children as well.
void signalChildren(int signal) {
    for (auto pid : children(::getpid())) {
        siginfo_t info{};
        if (::waitid(P_PID, pid, &info, WEXITED | WNOHANG | WNOWAIT) < 0) {
            if (errno == ECHILD || errno == EINTR) continue;
            throw std::runtime_error("cannot inspect owned child");
        }
        if (!info.si_pid && ::kill(pid, signal) < 0 && errno != ESRCH)
            throw std::runtime_error("cannot signal owned child");
    }
}
struct Children {
    pid_t command = -1;
    int exitCode = 125;
    long maxRssKb = 0;
    bool commandDone = false;
    bool reap() {
        for (;;) {
            int status = 0; rusage usage{};
            const auto pid = ::wait4(-1, &status, WNOHANG, &usage);
            if (pid < 0 && errno == EINTR) continue;
            if (pid < 0 && errno == ECHILD) return true;
            require(pid >= 0, "cannot reap owned process tree");
            if (!pid) return false;
            maxRssKb = std::max(maxRssKb, usage.ru_maxrss);
            if (pid == command) {
                commandDone = true;
                exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
        }
    }
    bool cleanup() {
        const auto end = monotonic() + 5;
        do {
            if (reap()) return true;
            signalChildren(SIGKILL);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (monotonic() < end);
        return reap();
    }
};
int superviseStage(J record, const fs::path &directory, const std::vector<std::string> &argv,
                   const fs::path &cwd, pid_t owner, const std::string &ownerBirth,
                   double deadline, uint64_t memoryBytes) {
    Children tree; bool timeout = false, cancelled = false, memoryExceeded = false, cleaned = false;
    uint64_t peak = 0; std::string failure;
    const auto ownerGone = [&] { return ::getppid() != owner || birth(owner) != ownerBirth; };
    try {
        task::installCancellationHandlers();
        require(::setsid() >= 0, "cannot create stage supervisor session");
        require(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "Linux child subreaping is required (kernel 3.4 or newer)");
        put(record, "supervisor_pid", num(::getpid()));
        put(record, "supervisor_birth", str(birth(::getpid())));
        put(record, "status", str("running"));
        save(directory / "command.json", record);
        if (ownerGone() || interrupted()) cancelled = true;
        else if (monotonic() >= deadline) timeout = true;
        else {
            Fd stdoutFd(::open((directory / "stdout.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
            Fd stderrFd(::open((directory / "stderr.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
            require(stdoutFd.value >= 0 && stderrFd.value >= 0, "cannot open stage output logs");
            std::string descriptorError;
            require(util::moveAboveStandardDescriptors(stdoutFd.value, descriptorError, "stage stdout"), descriptorError);
            require(util::moveAboveStandardDescriptors(stderrFd.value, descriptorError, "stage stderr"), descriptorError);
            std::vector<char *> arguments;
            for (const auto &arg : argv) arguments.push_back(const_cast<char *>(arg.c_str()));
            arguments.push_back(nullptr);
            tree.command = ::fork(); require(tree.command >= 0, "cannot fork stage command");
            if (!tree.command) {
                if (birth(owner) != ownerBirth) ::_exit(143);
                if (::setsid() < 0 || ::chdir(cwd.c_str()) || ::dup2(stdoutFd.value, 1) < 0 || ::dup2(stderrFd.value, 2) < 0) ::_exit(126);
                ::close(stdoutFd.value); ::close(stderrFd.value);
                for (int signal : {SIGINT, SIGTERM, SIGHUP}) ::signal(signal, SIG_DFL);
                // fork/exec inherits the caller's environment, ulimits and cgroup.
                // No environment values are copied into command lines or logs.
                ::execvp(arguments[0], arguments.data());
                ::perror("veval: cannot execute stage"); ::_exit(127);
            }
            put(record, "command_pid", num(tree.command));
            save(directory / "command.json", record);
            for (;;) {
                peak = std::max(peak, residentTree());
                tree.reap(); if (tree.commandDone) break;
                cancelled = interrupted() || ownerGone(); timeout = monotonic() >= deadline;
                memoryExceeded = memoryBytes && peak > memoryBytes;
                if (cancelled || timeout || memoryExceeded) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!tree.commandDone && !memoryExceeded) {
                // Let the adapter cancel its solvers before reclaiming the tree.
                ::kill(tree.command, SIGTERM);
                const auto grace = monotonic() + 2;
                while (!tree.commandDone && monotonic() < grace) {
                    tree.reap(); std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
        }
    } catch (const std::exception &e) { failure = e.what(); }
    try { cleaned = tree.cleanup(); }
    catch (const std::exception &e) { failure += std::string("; cleanup: ") + e.what(); }
    const auto result = memoryExceeded ? "memory-limit" : timeout ? "timeout" : cancelled ? "interrupted" :
        failure.empty() && cleaned && tree.exitCode == 0 ? "success" : "exit-code";
    put(record, "result", str(result));
    put(record, "status", str(cancelled ? "interrupted" : timeout ? "timeout" : std::string(result) == "success" ? "completed" : "failed"));
    put(record, "exit_code", num(tree.exitCode));
    put(record, "cleanup_confirmed", boolean(cleaned));
    put(record, "wall_s", num(monotonic() - numeric(record["started_monotonic"])));
    put(record, "memory_peak_bytes", str(std::to_string(peak)));
    put(record, "single_process_peak_kb", num(tree.maxRssKb));
    if (!failure.empty()) put(record, "reason", str(failure));
    save(directory / "command.json", record);
    return cleaned ? 0 : 2;
}
} // namespace
int executeOwned(int, char **) { throw std::runtime_error("obsolete systemd stage invocation; start a new native run"); }
void Process::reconcile() {
    auto paths = filesUnder(out / "commands");
    for (const auto &path : paths) {
        if (path.filename() != "command.json") continue;
        auto record = jsonFile(path);
        if (text(record["schema"]) != "veval-check-command/1" || record["cleanup_confirmed"].is_true()) continue;
        require(text(record["supervision_scope"]) == "process-tree", "unfinished legacy stage; stop its old runtime before starting a new run: " + path.string());
        Fd lock(::open((path.parent_path() / "process.lock").c_str(), O_RDWR | O_CLOEXEC));
        require(lock.value >= 0, "missing native supervisor lock: " + path.string());
        const auto end = monotonic() + 10;
        while (::flock(lock.value, LOCK_EX | LOCK_NB) != 0) {
            require(errno == EWOULDBLOCK || errno == EINTR, "cannot inspect native supervisor lock");
            require(!interrupted() && monotonic() < end, "stage supervisor is still active: " + path.string());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        record = jsonFile(path);
        // The supervisor publishes its identity before it can fork a command.
        if (record["supervisor_pid"].is_null()) {
            put(record, "status", str("interrupted")); put(record, "cleanup_confirmed", boolean(true));
            put(record, "timing_incomplete", boolean(true)); save(path, record);
        }
        require(record["cleanup_confirmed"].is_true(), "native supervisor exited without confirmed cleanup: " + path.string());
    }
}
int superviseCommand(int argc, char **argv) {
    require(argc >= 6 && std::string(argv[4]) == "--", "invalid native supervision request");
    task::installCancellationHandlers();
    Process process; process.out = fs::absolute(argv[2]); process.root = fs::canonical(argv[3]);
    require(!fs::exists(process.out), "native supervision directory already exists");
    fs::create_directories(process.out);
    auto receipt = process.run("external-command", std::vector<std::string>(argv + 5, argv + argc));
    const bool success = text(receipt["status"]) == "completed";
    if (success) put(receipt, "status", str("success"));
    save(process.out / "command.json", receipt);
    return success ? 0 : 2;
}
J Process::run(const std::string &stage, const std::vector<std::string> &argv, const fs::path &cwd) {
    require(!argv.empty(), "empty stage command");
    const auto ownerGone = [&] { return parentToMonitor > 0 && ::getppid() != parentToMonitor; };
    require(!interrupted() && !ownerGone(), "verification interrupted");
    if (deadline <= monotonic()) throw Expired("task wall deadline exhausted");
    std::string error; require(util::ensureWaitableChildren(error), error);
    const auto started = monotonic();
    const auto owner = ::getpid();
    const auto ownerBirth = birth(owner); require(!ownerBirth.empty(), "cannot identify stage owner");
    const auto serial = std::to_string(owner) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory = out / "commands" / serial; fs::create_directories(directory);
    Fd lock(::open((directory / "process.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
    require(lock.value >= 0 && ::flock(lock.value, LOCK_EX | LOCK_NB) == 0, "cannot lock native stage");
    const auto working = cwd.empty() ? root : cwd;
    const bool limited = std::isfinite(deadline);
    J record = object({{"schema", str("veval-check-command/1")}, {"stage", str(stage)}, {"argv", stringArray(argv)},
        {"directory", str(directory.string())}, {"cwd", str(working.string())}, {"started_monotonic", num(started)},
        {"owner_pid", str(std::to_string(owner))}, {"owner_birth", str(ownerBirth)},
        {"supervision_scope", str("process-tree")}, {"memory_accounting", str("sampled-tree-rss-plus-swap")},
        {"memory_sample_interval_s", num(.05)}, {"memory_limit_mode", str(memoryBytes ? "sampled" : "inherited")},
        {"memory_max_bytes", memoryBytes ? num(memoryBytes) : J()},
        {"timeout_s", limited ? num(deadline - started) : J()}, {"task_deadline_monotonic", limited ? num(deadline) : J()},
        {"timeout_scope", str(limited ? "task" : "none")}, {"status", str("starting")}, {"cleanup_confirmed", boolean(false)}});
    save(directory / "command.json", record);
    const auto supervisor = ::fork();
    if (supervisor == 0) {
        int code = 2;
        try {
            closeInheritedDescriptors(lock.value);
            code = superviseStage(record, directory, argv, working, owner, ownerBirth, deadline, memoryBytes);
        }
        catch (const std::exception &e) { std::cerr << "veval stage supervisor: " << e.what() << '\n'; }
        ::_exit(code);
    }
    require(supervisor > 0, "cannot fork stage supervisor");
    bool forwarded = false; int status = 0;
    for (;;) {
        const auto waited = ::waitpid(supervisor, &status, WNOHANG);
        if (waited == supervisor) break;
        if (waited < 0 && errno == EINTR) continue;
        require(waited == 0, "cannot wait for stage supervisor");
        if ((interrupted() || ownerGone()) && !forwarded) { ::kill(supervisor, SIGTERM); forwarded = true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    record = jsonFile(directory / "command.json");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0 && record["cleanup_confirmed"].is_true(), "cannot confirm cleanup of " + stage + "; see " + directory.string());
    return record;
}
} // namespace veval::workflow
#endif
