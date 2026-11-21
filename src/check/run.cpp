#include "run.hpp"
#include "engine.hpp"

#include <util/process/ChildProcess.h>
#include <util/process/CurrentExecutablePath.h>
#include <util/process/ProcessGroup.h>
#include <check/run/AncestorRecheck.h>
#include <check/run/SelectDeepestBranches.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>

#include <unistd.h>

namespace crisp::check {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::system_error os_error(const char* operation) {
    return std::system_error(errno, std::generic_category(), operation);
}

struct Scratch {
    fs::path path;
    Scratch() {
        auto pattern = (fs::temp_directory_path() / "crisp-ebmc.XXXXXX").string();
        if (!::mkdtemp(pattern.data())) throw os_error("create task directory");
        path = fs::absolute(pattern);
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

struct Pipe {
    int fd[2] = {-1, -1};
    Pipe() {
        if (::pipe(fd) < 0) throw os_error("create result pipe");
    }
    ~Pipe() {
        for (int end : fd) if (end >= 0) ::close(end);
    }
    void close_end(int n) {
        ::close(fd[n]);
        fd[n] = -1;
    }
    void drain(std::string& events) const {
        char block[8192];
        for (;;) {
            const auto count = ::read(fd[0], block, sizeof(block));
            if (count > 0) events.append(block, static_cast<std::size_t>(count));
            else if (count == 0 || errno == EAGAIN || errno == EWOULDBLOCK) return;
            else if (errno != EINTR) throw os_error("read result pipe");
        }
    }
};

void set_memory_limit(std::uint64_t mib) {
    struct rlimit limit;
    if (::getrlimit(RLIMIT_AS, &limit) < 0) throw os_error("read memory limit");
    constexpr rlim_t bytes_per_mib = 1024 * 1024;
    if (mib <= std::numeric_limits<rlim_t>::max() / bytes_per_mib) {
        limit.rlim_cur = std::min(limit.rlim_cur, static_cast<rlim_t>(mib) * bytes_per_mib);
        if (::setrlimit(RLIMIT_AS, &limit) < 0) throw os_error("set memory limit");
    }
}

[[noreturn]] void worker(const front::Task& task, const fs::path& scratch, Reporter reporter,
                         EngineRun operation, const std::set<std::string>& skip) {
    int status = 2;
    try {
#ifndef __APPLE__
        if (::setpgid(0, 0) < 0) throw os_error("isolate task process");
#endif
        fs::current_path(scratch); // Keep any scratch files created by IC3 private to this task.
        // EBMC writes SMT formulas to TMPDIR; they go with the task's scratch directory.
        if (::setenv("TMPDIR", scratch.c_str(), 1) < 0) throw os_error("set TMPDIR for EBMC");
        if (!std::freopen("diagnostics.log", "w", stderr) || ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
            throw os_error("redirect EBMC diagnostics");
        const struct rlimit no_core{0, 0};
        if (::setrlimit(RLIMIT_CORE, &no_core) < 0) throw os_error("disable task core dumps");
        if (task.ram_mib) set_memory_limit(task.ram_mib->value);
        if (run_engine(task, scratch, reporter, operation, skip)) status = 0;
    } catch (const std::bad_alloc&) {
        std::fputs("EBMC exhausted available memory\n", stderr);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
    } catch (...) {
        std::fputs("unhandled EBMC error\n", stderr);
    }
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    ::_exit(status);
}

veval::util::GroupWait run_worker(const front::Task& task, const fs::path& scratch,
                                 Clock::time_point deadline, std::string& events, EngineRun operation,
                                 const std::set<std::string>& skip = {}) {
    Pipe pipe;
    std::string error;
    for (int& fd : pipe.fd)
        if (!veval::util::moveAboveStandardDescriptors(fd, error, "result pipe")) throw std::runtime_error(error);
    if (::fcntl(pipe.fd[0], F_SETFL, O_NONBLOCK) < 0) throw os_error("configure result pipe");
    std::cout.flush();
    std::cerr.flush();
    const auto pid = veval::util::forkProofContext();
    if (pid < 0) throw os_error("start EBMC");
    if (pid == 0) {
        pipe.close_end(0);
        worker(task, scratch, {pipe.fd[1]}, operation, skip);
    }
    pipe.close_end(1);
    const auto wait = veval::util::waitForProofContext(pid, deadline, std::chrono::milliseconds(5),
                                                       [&] { pipe.drain(events); });
    pipe.drain(events);
    if (!wait.error.empty()) throw std::runtime_error(wait.error);
    return wait;
}

// The directories of PATH, searched as by execvp.
bool on_path(const std::string& program) {
    const char* path = std::getenv("PATH");
    std::string_view list = path ? path : "";
    while (true) {
        const auto end = list.find(':');
        const auto dir = list.substr(0, end);
        const auto file = (dir.empty() ? fs::path(".") : fs::path(dir)) / program;
        if (::access(file.c_str(), X_OK) == 0 && fs::is_regular_file(file)) return true;
        if (end == list.npos) return false;
        list.remove_prefix(end + 1);
    }
}

void check_options(const front::Task& task) {
    if (!task.params.empty()) {
        const auto& param = task.params.front();
        throw std::runtime_error(param.source->file.string() + ":" + std::to_string(param.source->line) +
                                 ": param " + param.value.name + " is not supported");
    }
    if (task.clock) std::cout << "  clock " << task.clock->value << " is recorded only\n";
    if (task.level.value == front::Level::word && !on_path("yices-smt2")) {
        const auto& level = task.level.source.value();
        throw std::runtime_error(level.file.string() + ":" + std::to_string(level.line) +
                                 ": [options] level word: yices-smt2 not found in PATH; "
                                 "install yices or add its directory to PATH");
    }
    // veval encodes temporal properties with Spot's ltl2tgba (spec 25).
    if (task.veval.value != front::ToolUse::off && !on_path("ltl2tgba")) {
        const auto& veval = task.veval.source.value();
        throw std::runtime_error(veval.file.string() + ":" + std::to_string(veval.line) + ": [options] veval " +
                                 std::string(front::spelling(task.veval.value)) +
                                 ": ltl2tgba not found in PATH; install Spot "
                                 "(https://spot.lre.epita.fr/install.html) or add its bin directory to PATH");
    }
    if (task.smart.value == front::ToolUse::only)
        throw std::runtime_error("smart only is not supported");
    if (task.smart.value == front::ToolUse::on &&
        (!task.includes.empty() || !task.defines.empty()))
        throw std::runtime_error("smart on does not support [design] include or define yet");
}

struct Property {
    std::string name, status, detail;
};

struct TaskEvents {
    std::vector<Property> properties;
    std::vector<std::string> missing; // veval: "instance\tassumption\treason"
    std::vector<std::string> resets;  // veval: "instance\tport on the net of [design] reset"
    std::vector<std::string> undriven;
};

TaskEvents read_events(const std::string& events) {
    TaskEvents result;
    auto& properties = result.properties;
    std::istringstream input(events);
    std::size_t index;
    std::string status, detail;
    // A killed worker may have written only part of its final record.
    while (input >> index >> status >> std::quoted(detail) && input.get() == '\n') {
        if (status == "undriven")
            result.undriven.push_back(detail);
        else if (status == "missing")
            result.missing.push_back(detail);
        else if (status == "reset")
            result.resets.push_back(detail);
        else if (status == "pending" && index == properties.size())
            properties.push_back({detail, status, {}});
        else {
            auto& property = properties.at(index);
            property.status = status;
            property.detail = detail;
        }
    }
    return result;
}

fs::path task_output_dir(const front::Task& task) {
    auto dir = task.salt_file.stem().string();
    if (task.name) dir += "_" + task.name->value;
    if (dir == "." || dir == "..")
        throw std::runtime_error("cannot use '" + dir + "' as an output directory name");
    return fs::path("crisp-out") / dir;
}

std::string file_name(const std::string& name) {
    std::string file;
    for (char c : name) {
        if (c == '/') file += "%2F";
        else if (c == '%') file += "%25";
        else file += c;
    }
    return file;
}

fs::path wave_path(const front::Task& task, const std::string& name) {
    return task_output_dir(task) / (file_name(name) + ".vcd");
}

// Rename publishes a complete trace and replaces an existing file atomically.
// When TMPDIR is on another filesystem, finish a local copy before the rename.
bool publish_file(const fs::path& from, const fs::path& to, Clock::time_point deadline) {
    fs::create_directories(to.parent_path());
    std::error_code error;
    fs::rename(from, to, error);
    if (!error) return true;
    if (error != std::errc::cross_device_link) throw fs::filesystem_error("publish VCD", from, to, error);
    auto staging = (to.parent_path() / ".crisp-vcd.XXXXXX").string();
    const int fd = ::mkstemp(staging.data());
    if (fd < 0) throw os_error("create temporary VCD");
    ::close(fd);
    try {
        std::ifstream in(from, std::ios::binary);
        in.exceptions(std::ios::badbit);
        if (!in) throw std::runtime_error("could not open " + from.string());
        std::ofstream out(staging, std::ios::binary);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        char block[64 * 1024];
        while (in.read(block, sizeof(block)) || in.gcount()) {
            if (Clock::now() >= deadline) {
                fs::remove(staging);
                return false;
            }
            out.write(block, in.gcount());
        }
        out.close();
        fs::rename(staging, to);
    } catch (...) {
        fs::remove(staging, error);
        throw;
    }
    return true;
}
fs::path smart_executable() {
    if (const char* path = std::getenv("CRISP_SMART"))
        return fs::absolute(path);

    const auto bin = veval::util::currentExecutable().parent_path();
    const auto sibling = bin / "smart";
    if (fs::is_regular_file(sibling)) return sibling;
    return bin.parent_path() / "src/components/smartVerilog/build/smart";
}

fs::path slang_executable() {
    if (const char* path = std::getenv("CRISP_SLANG"))
        return fs::absolute(path);

    const auto bin = veval::util::currentExecutable().parent_path();
    const auto sibling = bin / "slang";
    if (fs::is_regular_file(sibling)) return sibling;
    return bin.parent_path() / "src/components/slang/build/bin/slang";
}

// "<.salt>:<line>:" of the rtl-mode line, or of the task when the key is not written.
std::string rtl_mode_location(const front::Task& task) {
    const auto& source = task.rtl_mode.source ? task.rtl_mode.source : task.name ? task.name->source : std::nullopt;
    if (source) return source->file.string() + ":" + std::to_string(source->line) + ":";
    return task.salt_file.string() + ":";
}

// slang's error diagnostics: each starts with "<file>:<line>:<col>: error: " or
// "error: " and continues with the source excerpt; notes after an error belong to it.
std::string slang_errors(const std::string& log) {
    static const std::regex header(R"(^(?:\S.*:\d+:\d+: )?(error|fatal error|warning|note): .*)");
    std::istringstream lines(log);
    std::string kept, line;
    bool keep = false;
    for (std::smatch match; std::getline(lines, line);) {
        if (std::regex_match(line, match, header)) {
            const auto severity = match[1].str();
            keep = severity == "error" || severity == "fatal error" || (keep && severity == "note");
        }
        if (keep) kept += line + '\n';
    }
    return kept.empty() ? log : kept;
}

// rtl-mode strict (spec 23): slang parses and fully elaborates exactly the input
// that EBMC gets, within the task's timeout and memory limit. Returns false after
// printing slang's errors.
bool check_rtl(const front::Task& task, const fs::path& scratch, Clock::time_point deadline) {
    const auto executable = slang_executable();
    if (::access(executable.c_str(), X_OK) != 0 || !fs::is_regular_file(executable))
        throw std::runtime_error(rtl_mode_location(task) + " rtl-mode strict: slang not found at " + executable.string() +
                                 ". Run 'make slang' or set CRISP_SLANG, or write rtl-mode relaxed to skip the check");

    std::vector<std::string> args{executable.string(), "--quiet", "--single-unit"};
    for (const auto& dir : task.includes) args.insert(args.end(), {"-I", dir.value.string()});
    for (const auto& define : task.defines)
        args.insert(args.end(), {"-D", define.value.name + (define.value.value ? "=" + *define.value.value : "")});
    if (task.top) args.insert(args.end(), {"--top", task.top->value});
    for (const auto& file : task.files) args.push_back(fs::absolute(file.value).string());

    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    const auto log_path = scratch / "slang.log";
    std::cout.flush();
    std::cerr.flush();

    const auto pid = veval::util::forkProofContext();
    if (pid < 0) throw os_error("start slang");
    if (pid == 0) {
#ifndef __APPLE__
        if (::setpgid(0, 0) < 0) ::_exit(126);
#endif
        try {
            if (task.ram_mib) set_memory_limit(task.ram_mib->value);
            if (!std::freopen(log_path.c_str(), "w", stdout) || ::dup2(STDOUT_FILENO, STDERR_FILENO) < 0)
                ::_exit(126);
            // slang prints source paths relative to its working directory: use the .salt's.
            if (::chdir(task.salt_file.parent_path().c_str()) < 0) std::perror("enter the .salt directory");
            ::execv(executable.c_str(), argv.data());
            std::perror("exec slang");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
        }
        ::_exit(126);
    }

    const auto wait = veval::util::waitForProofContext(pid, deadline, std::chrono::milliseconds(5));
    if (!wait.error.empty()) throw std::runtime_error(wait.error);
    if (wait.timedOut) throw std::runtime_error("the strict RTL check (slang) exceeded the task timeout");
    if (WIFEXITED(wait.status) && WEXITSTATUS(wait.status) == 0) return true;

    std::ifstream in(log_path);
    std::ostringstream log;
    log << in.rdbuf();
    std::cerr << "crisp: " << rtl_mode_location(task)
              << " strict RTL check failed (slang); write rtl-mode relaxed to skip it\n";
    if (WIFSIGNALED(wait.status)) std::cerr << "crisp: slang terminated by signal " << WTERMSIG(wait.status) << '\n';
    std::cerr << slang_errors(log.str());
    return false;
}

void mine(const front::Task& task, const fs::path& scratch, Clock::time_point deadline) {
    const auto executable = smart_executable();
    if (::access(executable.c_str(), X_OK) != 0)
        throw std::runtime_error("SMART executable not found at " + executable.string() +
                                 ". Run 'make smart' or set CRISP_SMART");

    const auto temporary_output = scratch / "smart_assertion.sv";
    std::vector<std::string> args{executable.string(), "--no-discover-siblings",
                                  "--workdir", (scratch / "smart").string(),
                                  "--output", temporary_output.string()};
    if (task.top) args.insert(args.end(), {"--top", task.top->value});
    if (task.clock) args.insert(args.end(), {"--clock", task.clock->value});
    if (task.reset) args.insert(args.end(), {"--reset", task.reset->value.signal +
        (task.reset->value.polarity == front::Polarity::low ? ":0" : ":1")});
    if (task.thread.value) args.insert(args.end(), {"--jobs", std::to_string(task.thread.value)});
    for (const auto& file : task.files) args.push_back(fs::absolute(file.value).string());

    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    std::cout.flush();
    std::cerr.flush();

    const auto pid = veval::util::forkProofContext();
    if (pid < 0) throw os_error("start SMART");
    if (pid == 0) {
#ifndef __APPLE__
        if (::setpgid(0, 0) < 0) ::_exit(126);
#endif
        try {
            if (task.ram_mib) set_memory_limit(task.ram_mib->value);
            ::execv(executable.c_str(), argv.data());
            std::perror("exec SMART");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
        }
        ::_exit(126);
    }

    const auto wait = veval::util::waitForProofContext(pid, deadline, std::chrono::milliseconds(5));
    if (!wait.error.empty()) throw std::runtime_error(wait.error);
    if (wait.timedOut) throw std::runtime_error("SMART exceeded the task timeout");
    if (!WIFEXITED(wait.status) || WEXITSTATUS(wait.status) != 0)
        throw std::runtime_error("SMART mining failed");

    const auto output = task_output_dir(task) / "smart_assertion.sv";
    if (!publish_file(temporary_output, output, deadline))
        throw std::runtime_error("SMART output exceeded the task timeout");
    std::cout << "  SMART assertions  " << output.string() << '\n';
}

// ---- veval (spec 25) ----

// One assertion of a veval product: a source property carried to an instance.
struct VevalJob {
    std::string property, instance, wrapper, label;
    fs::path product;
    std::size_t depth = 0;
    std::string clock;                 // the product's sampling clock
    std::vector<std::string> premises; // the local environment veval derived (reset, clamps)
};

// veval writes one SystemVerilog product per instance. Its comments name the
// instance and, before each assertion, the source property it carries.
std::vector<VevalJob> read_products(const fs::path& dir) {
    std::vector<fs::path> files;
    if (fs::is_directory(dir))
        for (const auto& entry : fs::directory_iterator(dir)) {
            const auto name = entry.path().filename().string();
            if (name.size() > 8 && name.compare(name.size() - 8, 8, "_spec.sv") == 0) files.push_back(entry.path());
        }
    std::sort(files.begin(), files.end());
    static const std::regex instance(R"(^// instance:\s*(\S+))"), depth(R"(^// depth:\s*(\d+))"),
        module(R"(^module\s+(\w+))"), from(R"(^\s*// from:\s*(\S+))"),
        assertion(R"(^\s*(\w+):\s*assert\s+property\b)"), premise(R"(^\s*// premise: (.*)$)"),
        clock(R"(@\(posedge\s+(\w+)\))");
    std::vector<VevalJob> jobs;
    for (const auto& file : files) {
        std::ifstream in(file);
        VevalJob product;
        product.product = fs::absolute(file);
        std::vector<std::pair<std::string, std::string>> labels; // (source property, label)
        std::string source;
        std::smatch match;
        for (std::string line; std::getline(in, line);) {
            if (std::regex_search(line, match, instance)) product.instance = match[1];
            else if (std::regex_search(line, match, depth)) product.depth = std::stoul(match[1]);
            else if (product.wrapper.empty() && std::regex_search(line, match, module)) product.wrapper = match[1];
            else if (std::regex_search(line, match, from)) source = match[1];
            else if (std::regex_search(line, match, assertion)) labels.emplace_back(source, match[1]);
            else if (std::regex_search(line, match, premise)) product.premises.push_back(match[1]);
            // Internal observations are written as u_dut.<name>, which the engine reads directly.
            if (product.clock.empty() && std::regex_search(line, match, clock)) product.clock = match[1];
        }
        for (const auto& [property, label] : labels) {
            auto job = product;
            job.property = property;
            job.label = label;
            jobs.push_back(job);
        }
    }
    return jobs;
}

std::string first_line(const fs::path& file) {
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    return line;
}

struct VevalResult {
    std::vector<Property> lines;   // one per assertion veval saw, in source order
    std::set<std::string> decided; // proved or passed by a local check
    bool failed = false, timed_out = false;
};

// Lower in one worker; then check each landing in its own worker, deepest
// first, moving up one level after a local counterexample (algorithm 1).
VevalResult run_veval(const front::Task& task, const fs::path& scratch, Clock::time_point deadline,
                      const std::string& timeout) {
    VevalResult result;
    std::string events;
    const auto lowered = run_worker(task, scratch, deadline, events, EngineRun::lower);
    const auto lowering = read_events(events);
    if (lowered.timedOut) {
        result.timed_out = true;
        for (const auto& p : lowering.properties) result.lines.push_back({p.name, "unknown", timeout + " (veval)"});
        return result;
    }
    if (!WIFEXITED(lowered.status) || WEXITSTATUS(lowered.status) != 0) {
        result.failed = true;
        return result;
    }
    std::map<std::string, std::string> missing; // instance -> lowered assumptions the check lacks
    for (const auto& record : lowering.missing) {
        const auto tab = record.find('\t'), second = record.find('\t', tab + 1);
        auto& list = missing[record.substr(0, tab)];
        list += (list.empty() ? "" : ", ") + record.substr(tab + 1, second - tab - 1) + " (" +
                record.substr(second + 1) + ")";
    }
    std::map<std::string, std::string> reset_ports; // instance -> its input on the net of [design] reset
    for (const auto& record : lowering.resets) {
        const auto tab = record.find('\t');
        reset_ports[record.substr(0, tab)] = record.substr(tab + 1);
    }
    const auto without = [&](const std::string& instance) {
        std::string text;
        if (const auto found = missing.find(instance); found != missing.end())
            text += "; checked without assumption " + found->second;
        if (task.reset && !reset_ports.count(instance))
            text += "; checked without reset: no input of " + instance + " is connected to " + task.reset->value.signal;
        return text;
    };

    const auto jobs = read_products(scratch / "veval");
    using veval::verification::SourceInstance;
    std::map<SourceInstance, std::string> ids;
    std::set<SourceInstance> candidates;
    std::set<std::string> instances;
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        ids[{jobs[i].property, jobs[i].instance}] = std::to_string(i);
        candidates.insert({jobs[i].property, jobs[i].instance});
        instances.insert(jobs[i].instance);
    }
    std::map<std::string, std::string> parents; // nearest enclosing instance with a landing
    for (const auto& instance : instances) {
        for (auto up = instance; up.rfind('.') != std::string::npos;) {
            up.erase(up.rfind('.'));
            if (instances.count(up)) {
                parents[instance] = up;
                break;
            }
        }
    }
    std::vector<std::size_t> work;
    for (const auto& leaf : veval::verification::deepestBranchLandings(candidates, parents))
        work.push_back(std::stoul(ids.at(leaf)));

    std::map<std::string, Property> decided;
    std::map<std::string, std::vector<std::string>> notes;
    std::size_t checks = 0;
    while (!work.empty()) {
        const auto next = std::max_element(work.begin(), work.end(),
                                           [&](auto a, auto b) { return jobs[a].depth < jobs[b].depth; });
        const auto& job = jobs[*next];
        work.erase(next);
        if (decided.count(job.property)) continue;
        if (Clock::now() >= deadline) {
            result.timed_out = true;
            break;
        }
        // veval's local environment (spec 25, 2.4): a reset asserted at the initial
        // instant becomes Crisp's reset; a clamp becomes an assumption in every state.
        static const std::regex reset(R"(^reset asserted at the initial instant, from `[^']*' \(predicate: ([!~]?)(\w+)\)$)"),
            clamp(R"(^(\w+) <= (\S+) \(the parent drives it through a clamp\)$)");
        std::optional<front::Reset> initial_reset;
        std::vector<std::string> clamps;
        std::string unsupported;
        for (const auto& premise : job.premises) {
            std::smatch match;
            if (std::regex_match(premise, match, reset) && !initial_reset)
                initial_reset = front::Reset{match[2], match[1].length() ? front::Polarity::low : front::Polarity::high};
            else if (std::regex_match(premise, match, clamp))
                clamps.push_back(match[1].str() + " <= " + match[2].str());
            else {
                unsupported = "premise `" + premise + "'";
                break;
            }
        }
        if (!clamps.empty() && job.clock.empty()) unsupported = "clamps without a sampling clock";
        // [design] reset reaches this instance through one of its inputs.
        if (const auto port = reset_ports.find(job.instance); task.reset && port != reset_ports.end()) {
            const front::Reset salt_reset{port->second, task.reset->value.polarity};
            if (initial_reset && (initial_reset->signal != salt_reset.signal || initial_reset->polarity != salt_reset.polarity))
                unsupported = "two different resets (" + initial_reset->signal + " and " + salt_reset.signal + ")";
            initial_reset = salt_reset;
        }
        if (!unsupported.empty()) {
            notes[job.property].push_back(job.instance + ": no local conclusion, Crisp cannot state its " + unsupported);
            continue;
        }
        const auto dir = scratch / "veval" / ("check" + std::to_string(checks++));
        fs::create_directories(dir);
        auto product = job.product;
        if (!clamps.empty()) {
            // A copy of the product with the clamps as assumptions, before the wrapper's endmodule.
            std::ifstream in(job.product);
            std::ostringstream text;
            text << in.rdbuf();
            auto body = text.str();
            const auto end = body.rfind("endmodule");
            std::string assumptions;
            for (std::size_t i = 0; i < clamps.size(); ++i)
                assumptions += "  crisp_clamp_" + std::to_string(i) + ": assume property (@(posedge " + job.clock + ") " +
                               clamps[i] + ");\n";
            body.insert(end == std::string::npos ? body.size() : end, assumptions);
            product = dir / job.product.filename();
            std::ofstream(product) << body;
        }
        auto local = task;
        local.files.push_back({product, task.veval.source});
        local.top = front::Setting<std::string>{job.wrapper, task.veval.source};
        local.reset.reset();
        if (initial_reset) local.reset = front::Setting<front::Reset>{*initial_reset, task.veval.source};
        local.clock.reset();
        local.params.clear();
        local.properties = {{job.wrapper + "." + job.label, task.veval.source}};
        local.smart = {front::ToolUse::off, {}};
        local.veval = {front::ToolUse::off, {}};
        std::string local_events;
        const auto wait = run_worker(local, dir, deadline, local_events, EngineRun::solve);
        if (wait.timedOut) {
            result.timed_out = true;
            break;
        }
        const auto checked = read_events(local_events);
        std::string status = "error", detail = "EBMC returned no result";
        if (checked.properties.size() == 1 && checked.properties.front().status != "pending") {
            status = checked.properties.front().status;
            detail = checked.properties.front().detail;
        }
        if (!WIFEXITED(wait.status) || WEXITSTATUS(wait.status) != 0) {
            status = "error";
            detail = "EBMC failed: " + first_line(dir / "diagnostics.log");
        }
        if (status == "proved") {
            decided[job.property] = {job.property, "proved", "proved in " + job.instance + " (veval)" + without(job.instance)};
        } else if (status == "passed") {
            decided[job.property] = {job.property, "passed",
                                     "passed in " + job.instance + " (veval), " + detail + without(job.instance)};
        } else if (status == "refuted") {
            const auto wave = task_output_dir(task) / "veval" / file_name(job.property) / (file_name(job.instance) + ".vcd");
            fs::create_directories(wave.parent_path());
            if (publish_file(dir / "0.vcd", wave, deadline))
                notes[job.property].push_back("local counterexample in " + job.instance + " " + wave.string() +
                                              without(job.instance));
            const auto parent = veval::verification::ancestorRecheck({job.property, job.instance}, "completed",
                                                                     "refuted", ids, parents, task.top->value);
            if (!parent.empty()) work.push_back(std::stoul(parent));
        } else {
            notes[job.property].push_back(job.instance + ": no local conclusion (" + detail + ")" + without(job.instance));
        }
    }

    for (const auto& p : lowering.properties) {
        if (const auto found = decided.find(p.name); found != decided.end()) {
            result.lines.push_back(found->second);
            result.decided.insert(p.name);
            continue;
        }
        std::string detail;
        if (p.status == "unlowered") detail = p.detail;
        for (const auto& note : notes[p.name]) detail += (detail.empty() ? "" : "; ") + note;
        if (result.timed_out) detail = timeout + (detail.empty() ? "" : "; " + detail);
        else if (detail.empty()) detail = "no landing";
        result.lines.push_back({p.name, "unknown", "veval: " + detail});
    }
    return result;
}

void print_line(const Property& p, Summary& summary, Clock::time_point start) {
    if (p.status == "proved") ++summary.proved;
    else if (p.status == "passed") ++summary.passed;
    else if (p.status == "refuted") ++summary.refuted;
    else ++summary.unknown;
    const std::chrono::duration<double> elapsed = Clock::now() - start;
    std::cout << "  " << p.name << "  " << p.status << "  " << std::fixed << std::setprecision(3) << elapsed.count()
              << " s";
    if (!p.detail.empty()) std::cout << "  " << p.detail;
    std::cout << '\n';
}
} // namespace

void Reporter::send(std::size_t index, const char* status, const std::string& detail) const {
    std::ostringstream record;
    record << index << ' ' << status << ' ' << std::quoted(detail) << '\n';
    const auto text = record.str();
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto count = ::write(fd, text.data() + offset, text.size() - offset);
        if (count > 0) offset += static_cast<std::size_t>(count);
        else if (count < 0 && errno != EINTR) throw os_error("send EBMC result");
    }
}

void run(const front::Task& task, Summary& summary, bool multiple_tasks) {
    const auto start = Clock::now();
    const auto max_seconds = std::chrono::duration_cast<std::chrono::seconds>(Clock::time_point::max() - start);
    const auto seconds = std::min<std::uint64_t>(task.timeout_seconds.value, max_seconds.count());
    const auto deadline = start + std::chrono::seconds(seconds);
    check_options(task);
    Scratch scratch;
    std::string error, events;
    if (!veval::util::ensureWaitableChildren(error)) throw std::runtime_error(error);
    // Before SMART and EBMC: illegal RTL stops the task unless rtl-mode relaxed (spec 23).
    if (task.rtl_mode.value == front::RtlMode::strict && !check_rtl(task, scratch.path, deadline)) {
        summary.error = true;
        return;
    }
    const auto timeout = "timeout after " + std::to_string(seconds) + " s";
    // veval on / only (spec 25).
    std::set<std::string> decided;
    if (task.veval.value != front::ToolUse::off) {
        auto result = run_veval(task, scratch.path, deadline, timeout);
        if (result.failed) {
            summary.error = true;
            std::cerr << "crisp: veval could not lower the design\n";
            std::ifstream log(scratch.path / "diagnostics.log");
            if (log.peek() != std::ifstream::traits_type::eof()) std::cerr << log.rdbuf();
            return;
        }
        const bool only = task.veval.value == front::ToolUse::only;
        for (const auto& line : result.lines)
            if (only || result.decided.count(line.name)) print_line(line, summary, start);
        if (only) {
            if (result.lines.empty()) {
                summary.incomplete = true;
                std::cout << "  no assertions\n";
            }
            return;
        }
        decided = std::move(result.decided); // the whole design checks the rest
    }
    const auto wait = [&] {
        if (task.smart.value == front::ToolUse::on) {
            // SMART also consumes reset and runs checks. Validate before starting it.
            if (task.reset) {
                const auto validated = run_worker(task, scratch.path, deadline, events, EngineRun::validate_reset);
                if (validated.timedOut || !WIFEXITED(validated.status) || WEXITSTATUS(validated.status) != 0)
                    return validated;
                events.clear();
            }
            mine(task, scratch.path, deadline);
        }
        return run_worker(task, scratch.path, deadline, events, EngineRun::solve, decided);
    }();
    auto reports = read_events(events);
    const bool reject_undriven = task.undriven.value == front::Undriven::error;
    const bool rejected = reject_undriven && !reports.undriven.empty();
    const bool failed = rejected || (!wait.timedOut && (!WIFEXITED(wait.status) || WEXITSTATUS(wait.status) != 0));
    for (const auto& detail : reports.undriven) {
        std::cerr << (reject_undriven ? "crisp: error: " : "crisp: warning: ");
        if (multiple_tasks && task.name) std::cerr << '[' << task.name->value << "] ";
        std::cerr << detail << '\n';
    }
    auto& properties = reports.properties;
    bool diagnostics = failed;
    if (failed) {
        summary.error = true;
        if (!wait.timedOut && WIFSIGNALED(wait.status))
            std::cerr << "crisp: EBMC terminated by signal " << WTERMSIG(wait.status) << '\n';
    }
    if (properties.empty() && !failed && decided.empty()) {
        summary.incomplete = true;
        std::cout << (wait.timedOut ? "  unknown  " + timeout + " before assertion discovery\n" : "  no assertions\n");
    }
    for (std::size_t i = 0; i < properties.size(); ++i) {
        auto& p = properties[i];
        if (p.status == "refuted") {
            auto dest = wave_path(task, p.name);
            if (publish_file(scratch.path / (std::to_string(i) + ".vcd"), dest, deadline))
                p.detail = dest.string();
            else { p.status = "unknown"; p.detail = timeout; }
        }
        if (p.status == "pending") {
            p.status = wait.timedOut || failed ? "unknown" : "error";
            p.detail = wait.timedOut ? timeout : "EBMC returned no result";
        }
        if (p.status == "error") {
            summary.error = diagnostics = true;
            std::cerr << "crisp: " << p.name << ": " << p.detail << '\n';
            p.status = "unknown";
        }
        print_line(p, summary, start);
    }
    if (diagnostics) {
        std::ifstream log(scratch.path / "diagnostics.log");
        if (log.peek() != std::ifstream::traits_type::eof()) std::cerr << log.rdbuf();
        else if (failed && !rejected) std::cerr << "crisp: EBMC failed during verification\n";
    }
}
} // namespace crisp::check
