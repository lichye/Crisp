#include "run.hpp"
#include "engine.hpp"
#include "witness.hpp"

#include <util/process/ChildProcess.h>
#include <util/process/CurrentExecutablePath.h>
#include <util/process/ProcessGroup.h>
#include <check/run/AncestorRecheck.h>
#include <check/run/SelectDeepestBranches.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>

#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

namespace crisp::check {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::system_error os_error(const char* operation) {
    return std::system_error(errno, std::generic_category(), operation);
}

struct Scratch {
    fs::path path;
    Scratch(const fs::path& directory, const char* name) {
        auto pattern = fs::absolute(directory / name).string();
        if (!::mkdtemp(pattern.data())) throw os_error("create scratch directory");
        path = pattern;
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

// Keep the object in the run's filesystem without a directory entry. The
// compiler and engine inherit its descriptor, so even a killed parent leaves
// no object behind once the last process holding it exits.
class CompiledVv {
public:
    explicit CompiledVv(const fs::path& directory) {
#ifdef __linux__
        fd_ = ::open(directory.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
        if (fd_ < 0) throw os_error("create temporary goto-vv object");
#else
        auto name = (directory / ".goto-vv.XXXXXX").string();
        fd_ = ::mkstemp(name.data());
        if (fd_ < 0) throw os_error("create temporary goto-vv object");
        if (::unlink(name.c_str()) < 0) {
            const auto error = os_error("unlink temporary goto-vv object");
            ::close(fd_);
            throw error;
        }
#endif
        std::string error;
        if (!veval::util::moveAboveStandardDescriptors(fd_, error, "goto-vv object")) {
            ::close(fd_);
            throw std::runtime_error(error);
        }
    }
    ~CompiledVv() { ::close(fd_); }
    CompiledVv(const CompiledVv&) = delete;
    CompiledVv& operator=(const CompiledVv&) = delete;

    int fd() const { return fd_; }
    fs::path path() const {
#ifdef __linux__
        return fs::path("/proc/self/fd") / std::to_string(fd_);
#else
        return fs::path("/dev/fd") / std::to_string(fd_);
#endif
    }

private:
    int fd_;
};

void enter_solver_tmp(const fs::path& directory) {
    fs::current_path(directory);
    if (::setenv("TMPDIR", directory.c_str(), 1) < 0) throw os_error("set TMPDIR for EBMC");
}

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

[[noreturn]] void worker(const front::Task& task, const fs::path& scratch, const fs::path& tmp, Reporter reporter,
                         EngineRun operation, Deadline deadline, const std::set<std::string>& skip, const fs::path& compiled) {
    int status = 2;
    try {
#ifndef __APPLE__
        if (::setpgid(0, 0) < 0) throw os_error("isolate task process");
#endif
        enter_solver_tmp(tmp);
        if (!std::freopen((scratch / "diagnostics.log").c_str(), "w", stderr) || ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
            throw os_error("redirect EBMC diagnostics");
        const struct rlimit no_core{0, 0};
        if (::setrlimit(RLIMIT_CORE, &no_core) < 0) throw os_error("disable task core dumps");
        if (task.ram_mib) set_memory_limit(task.ram_mib->value);
        if (run_engine(task, scratch, reporter, operation, deadline, skip, compiled)) status = 0;
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
                                 const std::set<std::string>& skip = {}, const fs::path& compiled = {}) {
    // The supervising process removes TMPDIR and any relative solver dumps
    // after reaping the group, even when the worker cannot run its destructors.
    Scratch tmp(scratch, "tmp.XXXXXX");
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
        worker(task, scratch, tmp.path, {pipe.fd[1]}, operation, deadline, skip, compiled);
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
    // veval lowering and the kind/PDR Büchi conversion both use Spot.
    const bool veval = task.veval.value != front::ToolUse::off;
    const bool buechi = task.mode.value == front::Mode::kind || task.mode.value == front::Mode::pdr;
    if ((veval || buechi) && !on_path("ltl2tgba")) {
        const auto& source = (veval ? task.veval.source : task.mode.source).value();
        const auto option = veval ? "veval " + std::string(front::spelling(task.veval.value)) :
                                   "mode " + std::string(front::spelling(task.mode.value));
        throw std::runtime_error(source.file.string() + ":" + std::to_string(source.line) + ": [options] " + option +
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
    std::string vacuity = "unchecked";
    bool has_antecedent = false;
    std::string_view vacuity_value() const {
        if (status == "refuted") return "triggered";
        return vacuity;
    }
};

struct TaskEvents {
    std::vector<Property> properties;
    std::vector<std::string> missing; // veval: "instance\tassumption\treason"
    std::vector<std::string> resets;  // veval: "instance\tport on the net of [design] reset"
    std::vector<std::string> undriven;
    std::map<std::size_t, std::string> witness_unavailable;
    bool primary_complete = false;
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
        else if (status == "witness-unavailable")
            result.witness_unavailable.insert_or_assign(index, detail);
        else if (status == "primary-complete")
            result.primary_complete = true;
        else if (status == "pending" && index == properties.size())
            properties.push_back({detail, status, {}});
        else if (status == "vacuity")
            properties.at(index).vacuity = detail;
        else if (status == "antecedent")
            properties.at(index).has_antecedent = true;
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

// Only the parent writes this run's file. A flushed, newline-terminated row
// is available to readers even if the next worker and its parent are killed.
class TaskProgress {
    std::string path;
    std::unique_ptr<FILE, decltype(&std::fclose)> stream{nullptr, &std::fclose};

    void append(const std::string& line) {
        if (std::fwrite(line.data(), 1, line.size(), stream.get()) != line.size() || std::fflush(stream.get()) != 0)
            throw std::system_error(errno, std::generic_category(), "write " + path);
    }

public:
    explicit TaskProgress(const fs::path& dir) : path((dir / "progress-XXXXXX.tsv").string()) {
        std::error_code error;
        fs::create_directories(dir, error);
        if (error) throw std::system_error(error, "create " + path);
        const int fd = ::mkstemps(path.data(), 4);
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "create " + path);
        stream.reset(::fdopen(fd, "w"));
        if (!stream) {
            const int reason = errno;
            ::close(fd);
            throw std::system_error(reason, std::generic_category(), "open " + path);
        }
        append("stage\tproperty\tinstance\tstatus\tdetail\tvacuity\n");
        std::cout << "  progress  " << path << std::endl;
    }

    std::string artifact_tag() const { return fs::path(path).stem().string(); }

    void record(std::string_view stage, const Property& p, std::string_view instance = {}) {
        std::string row;
        const std::string_view fields[] = {stage, p.name, instance, p.status, p.detail, p.vacuity_value()};
        for (auto field : fields) {
            for (char c : field) {
                switch (c) {
                case '\\': row += "\\\\"; break;
                case '\t': row += "\\t"; break;
                case '\r': row += "\\r"; break;
                case '\n': row += "\\n"; break;
                default: row += c;
                }
            }
            row += '\t';
        }
        row.back() = '\n';
        append(row);
    }
};

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
// For a cross-filesystem move, finish a local copy before the rename.
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
fs::path component_executable(const char* env, const char* name, const char* source_path) {
    if (const char* path = std::getenv(env))
        return fs::absolute(path);

    const auto bin = veval::util::currentExecutable().parent_path();
    const auto sibling = bin / name;
    if (fs::is_regular_file(sibling)) return sibling;
    return bin.parent_path() / source_path;
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
    const auto executable = component_executable("CRISP_SLANG", "slang", "src/components/slang/build/bin/slang");
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

bool compile_vv(const front::Task& task, const fs::path& scratch, const CompiledVv& object, Deadline deadline) {
    const auto executable = component_executable("CRISP_GOTO_VV", "goto-vv", "src/components/hw-cbmc/src/goto-vv/build/goto-vv");
    const auto& source = *task.frontend.source;
    const auto failure = [&] {
        std::cerr << "crisp: " << source.file.string() << ':' << source.line << ": goto-vv failed\n";
    };

    std::vector<std::string> args{executable.string(), "compile", object.path().string(), "-mfcu"};
    if (task.top) args.insert(args.end(), {"--top", task.top->value});
    for (const auto& dir : task.includes) args.insert(args.end(), {"-I", dir.value.string()});
    for (const auto& define : task.defines)
        args.insert(args.end(), {"-D", define.value.name + (define.value.value ? "=" + *define.value.value : "")});
    if (task.rtl_mode.value == front::RtlMode::relaxed)
        args.insert(args.end(), {"--allow-enum-literal-width-mismatch", "--allow-use-before-declare",
                                 "--allow-initial-always-ff-drivers"});
    for (const auto& file : task.files) args.push_back(file.value.string());

    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    Pipe diagnostics;
    std::string error;
    for (int& fd : diagnostics.fd)
        if (!veval::util::moveAboveStandardDescriptors(fd, error, "goto-vv diagnostics")) throw std::runtime_error(error);
    if (::fcntl(diagnostics.fd[0], F_SETFL, O_NONBLOCK) < 0) throw os_error("configure goto-vv diagnostics");
    std::cout.flush();
    std::cerr.flush();
    const auto pid = veval::util::forkProofContext();
    if (pid < 0) throw os_error("start goto-vv");
    if (pid == 0) {
        diagnostics.close_end(0);
#ifndef __APPLE__
        if (::setpgid(0, 0) < 0) ::_exit(126);
#endif
        if (::dup2(diagnostics.fd[1], STDERR_FILENO) < 0) ::_exit(126);
        diagnostics.close_end(1);
        try {
            if (::fcntl(object.fd(), F_SETFD, 0) < 0) throw os_error("pass goto-vv object to compiler");
            if (::setenv("TMPDIR", scratch.c_str(), 1) < 0) throw os_error("set goto-vv TMPDIR");
            if (task.ram_mib) set_memory_limit(task.ram_mib->value);
            if (::chdir(task.salt_file.parent_path().c_str()) < 0) throw os_error("enter the .salt directory");
            ::execv(executable.c_str(), argv.data());
            std::perror("exec goto-vv");
        } catch (const std::exception& exception) {
            std::fprintf(stderr, "%s\n", exception.what());
        }
        ::_exit(126);
    }

    diagnostics.close_end(1);
    std::string log;
    const auto drain = [&] { diagnostics.drain(log); };
    const auto wait = veval::util::waitForProofContext(pid, deadline, std::chrono::milliseconds(5), drain);
    drain();
    if (!wait.error.empty()) throw std::runtime_error(wait.error);
    if (!wait.timedOut && WIFEXITED(wait.status) && WEXITSTATUS(wait.status) == 0) {
        std::cerr << log;
        return true;
    }
    failure();
    std::cerr << log;
    if (wait.timedOut) std::cerr << "goto-vv exceeded the task timeout\n";
    else if (WIFSIGNALED(wait.status)) std::cerr << "goto-vv terminated by signal " << WTERMSIG(wait.status) << '\n';
    return false;
}

void mine(const front::Task& task, const fs::path& scratch, Clock::time_point deadline) {
    const auto executable = component_executable("CRISP_SMART", "smart", "src/components/smartVerilog/build/smart");
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
    constexpr auto at_start = std::regex_constants::match_continuous;
    std::vector<VevalJob> jobs;
    for (const auto& file : files) {
        std::ifstream in(file);
        VevalJob product;
        product.product = fs::absolute(file);
        std::vector<std::pair<std::string, std::string>> labels; // (source property, label)
        std::string source;
        std::smatch match;
        for (std::string line; std::getline(in, line);) {
            if (std::regex_search(line, match, instance, at_start)) product.instance = match[1];
            else if (std::regex_search(line, match, depth, at_start)) product.depth = std::stoul(match[1]);
            else if (product.wrapper.empty() && std::regex_search(line, match, module, at_start)) product.wrapper = match[1];
            else if (std::regex_search(line, match, from, at_start)) source = match[1];
            else if (std::regex_search(line, match, assertion, at_start)) labels.emplace_back(source, match[1]);
            else if (std::regex_search(line, match, premise, at_start)) product.premises.push_back(match[1]);
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

std::string last_diagnostic(const fs::path& file) {
    std::ifstream in(file);
    std::string latest;
    for (std::string line; std::getline(in, line);)
        if (!line.empty()) latest = std::move(line);
    return latest;
}

struct LocalVacuity {
    front::Task task;
    fs::path directory;
};

struct VevalResult {
    std::vector<Property> lines;   // one per assertion veval saw, in source order
    std::set<std::string> decided; // conclusive local verdicts
    std::map<std::string, LocalVacuity> vacuity;
    bool failed = false, timed_out = false;
};

struct LocalPremises {
    std::optional<front::Reset> initial_reset;
    std::vector<std::string> clamps;
    std::string unsupported;
};

LocalPremises local_premises(const VevalJob& job, const front::Task& task,
                             const std::map<std::string, std::string>& reset_ports) {
    // A reset premise sets the initial reset. A clamp applies in every state.
    static const std::regex reset(R"(^reset asserted at the initial instant, from `[^']*' \(predicate: ([!~]?)(\w+)\)$)"),
        clamp(R"(^(\w+) <= (\S+) \(the parent drives it through a clamp\)$)");
    LocalPremises result;
    for (const auto& premise : job.premises) {
        std::smatch match;
        if (std::regex_match(premise, match, reset) && !result.initial_reset)
            result.initial_reset = front::Reset{match[2], match[1].length() ? front::Polarity::low : front::Polarity::high};
        else if (std::regex_match(premise, match, clamp))
            result.clamps.push_back(match[1].str() + " <= " + match[2].str());
        else {
            result.unsupported = "premise `" + premise + "'";
            break;
        }
    }
    if (!result.clamps.empty() && job.clock.empty()) result.unsupported = "clamps without a sampling clock";
    // [design] reset reaches this instance through one of its inputs.
    if (const auto port = reset_ports.find(job.instance); task.reset && port != reset_ports.end()) {
        const front::Reset salt_reset{port->second, task.reset->value.polarity};
        if (result.initial_reset && (result.initial_reset->signal != salt_reset.signal ||
                                     result.initial_reset->polarity != salt_reset.polarity))
            result.unsupported = "two different resets (" + result.initial_reset->signal + " and " + salt_reset.signal + ")";
        result.initial_reset = salt_reset;
    }
    return result;
}

// Lower in one worker; then check each landing in its own worker, deepest
// first, moving up one level after a local counterexample (algorithm 1).
VevalResult run_veval(const front::Task& task, const fs::path& scratch, Clock::time_point deadline,
                      const std::string& timeout, TaskProgress& progress) {
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
    std::map<std::size_t, std::deque<std::size_t>, std::greater<>> work;
    const auto enqueue = [&](std::size_t index) { work[jobs[index].depth].push_back(index); };
    for (const auto& leaf : veval::verification::deepestBranchLandings(candidates, parents))
        enqueue(std::stoul(ids.at(leaf)));

    std::map<std::string, Property> decided;
    std::map<std::string, std::vector<std::string>> notes;
    std::size_t checks = 0;
    while (!work.empty()) {
        const auto next = work.begin();
        const auto& job = jobs[next->second.front()];
        next->second.pop_front();
        if (next->second.empty()) work.erase(next);
        if (decided.count(job.property)) continue;
        if (Clock::now() >= deadline) {
            result.timed_out = true;
            break;
        }
        const auto [initial_reset, clamps, unsupported] = local_premises(job, task, reset_ports);
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
        const auto wait = run_worker(local, dir, deadline, local_events, EngineRun::solve_primary);
        const auto checked = read_events(local_events);
        std::string status = "error", detail = "EBMC returned no result";
        if (checked.properties.size() == 1 && checked.properties.front().status != "pending") {
            status = checked.properties.front().status;
            detail = checked.properties.front().detail;
        }
        const auto context = without(job.instance);
        const bool has_result = status == "proved" || status == "passed" || status == "refuted";
        if (wait.timedOut && !has_result) {
            result.timed_out = true;
            progress.record("local", {job.property, "unknown", timeout + context}, job.instance);
            break;
        }
        if (!has_result && (!WIFEXITED(wait.status) || WEXITSTATUS(wait.status) != 0)) {
            status = "error";
            detail = "EBMC failed: " + last_diagnostic(dir / "diagnostics.log");
        }
        if (status == "proved") {
            decided[job.property] = {job.property, "proved", "proved in " + job.instance + " (veval)" + context};
        } else if (status == "passed") {
            decided[job.property] = {job.property, "passed",
                                     "passed in " + job.instance + " (veval), " + detail + context};
        } else if (status == "refuted") {
            const auto wave = task_output_dir(task) / "veval" / file_name(job.property) / (file_name(job.instance) + ".vcd");
            if (!detail.empty()) {
                detail = "local counterexample in " + job.instance + " (" + detail + ")" + context;
                notes[job.property].push_back(detail);
            } else if (publish_file(dir / "0.vcd", wave, deadline)) {
                detail = "local counterexample in " + job.instance + " " + wave.string() + context;
                notes[job.property].push_back(detail);
            } else {
                detail = timeout + " publishing local VCD" + context;
            }
            const auto parent = veval::verification::ancestorRecheck({job.property, job.instance}, "completed",
                                                                     "refuted", ids, parents, task.top->value);
            if (!parent.empty()) enqueue(std::stoul(parent));
        } else {
            notes[job.property].push_back(job.instance + ": no local conclusion (" + detail + ")" + context);
            detail += context;
        }
        if (const auto found = decided.find(job.property); found != decided.end()) {
            progress.record("local", found->second, job.instance);
            if (checked.properties.front().has_antecedent)
                result.vacuity.emplace(job.property, LocalVacuity{std::move(local), dir});
            else progress.record("final", found->second);
        } else {
            progress.record("local", {job.property, status, detail}, job.instance);
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
        // In only mode this is the CLI's final decision. A deadline alone
        // supplies no evidence for landings that never returned a result.
        if (task.veval.value == front::ToolUse::only &&
            (!result.timed_out || p.status == "unlowered" || !notes[p.name].empty()))
            progress.record("final", result.lines.back());
    }
    return result;
}

void finish_local_vacuity(VevalResult& result, Deadline deadline, TaskProgress& progress) {
    for (auto& line : result.lines) {
        const auto found = result.vacuity.find(line.name);
        if (found == result.vacuity.end()) continue;
        if (Clock::now() < deadline) {
            const auto& job = found->second;
            std::string events;
            try {
                const auto directory = job.directory / "vacuity";
                fs::create_directories(directory);
                run_worker(job.task, directory, deadline, events, EngineRun::vacuity);
            } catch (const std::exception& error) {
                std::cerr << "crisp: warning: vacuity query failed for " << line.name << ": " << error.what() << '\n';
            }
            auto checked = read_events(events);
            if (checked.properties.size() == 1) line.vacuity = checked.properties.front().vacuity;
        }
        progress.record("final", line);
    }
}

void print_line(const Property& p, Summary& summary, Clock::time_point start) {
    if (p.status == "proved") ++summary.proved;
    else if (p.status == "passed") ++summary.passed;
    else if (p.status == "refuted") ++summary.refuted;
    else ++summary.unknown;
    const auto vacuity = p.vacuity_value();
    if (vacuity == "triggered") ++summary.triggered;
    else if (vacuity == "never triggered") ++summary.never_triggered;
    else if (vacuity.rfind("not triggered within ", 0) == 0) ++summary.not_triggered;
    else ++summary.unchecked;
    const std::chrono::duration<double> elapsed = Clock::now() - start;
    std::cout << "  " << p.name << "  " << p.status << "  " << std::fixed << std::setprecision(3) << elapsed.count()
              << " s";
    if (!p.detail.empty()) std::cout << "  " << p.detail;
    std::cout << "  " << vacuity << '\n';
}
} // namespace

void run_shallow_bmc(Deadline deadline,
                        const std::function<void(const Witness&)>& search, const Witness& observed) {
    if (Clock::now() >= deadline) return;
    Scratch tmp(fs::current_path(), "tmp.XXXXXX");
    Pipe pipe;
    if (::fcntl(pipe.fd[0], F_SETFL, O_NONBLOCK) < 0) throw os_error("configure witness pipe");
#ifdef __linux__
    const auto parent = ::getpid();
#endif
    const auto pid = ::fork();
    if (pid < 0) throw os_error("start witness search");
    if (pid == 0) {
        pipe.close_end(0);
        if (::setpgid(0, 0) < 0) ::_exit(2);
#ifdef __linux__
        // This group is nested inside the task. If its supervising worker
        // dies first, kill the entire group, including an external SMT solver.
        if (::signal(SIGUSR1, [](int) { ::kill(0, SIGKILL); }) == SIG_ERR ||
            ::prctl(PR_SET_PDEATHSIG, SIGUSR1) < 0 || ::getppid() != parent) ::_exit(2);
#endif
        try {
            enter_solver_tmp(tmp.path);
            search([&](std::size_t index) {
                if (Clock::now() >= deadline) ::_exit(0);
                ssize_t count;
                do { count = ::write(pipe.fd[1], &index, sizeof(index)); } while (count < 0 && errno == EINTR);
                if (count != sizeof(index)) ::_exit(2);
            });
            ::_exit(0);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "shallow BMC: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "shallow BMC: engine failure\n");
        }
        std::fflush(stderr);
        ::_exit(1);
    }
    pipe.close_end(1);
    // Each index is one atomic write, smaller than PIPE_BUF. Drain at every
    // poll and once after reaping to retain hits that preceded cancellation.
    auto drain = [&] {
        std::size_t index;
        for (;;) {
            const auto count = ::read(pipe.fd[0], &index, sizeof(index));
            if (count == sizeof(index)) observed(index);
            else if (count == 0 || (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) return;
            else if (count >= 0 || errno != EINTR) throw std::runtime_error("invalid witness result");
        }
    };
    const auto wait = veval::util::waitForProcessGroup(pid, deadline, std::chrono::milliseconds(1), drain);
    drain();
    if (!wait.error.empty()) throw std::runtime_error(wait.error);
    if (wait.timedOut) {
        std::fprintf(stderr, "shallow BMC: timed out\n");
        std::fflush(stderr);
    } else if (WIFSIGNALED(wait.status)) {
        std::fprintf(stderr, "shallow BMC: terminated by signal %d\n", WTERMSIG(wait.status));
        std::fflush(stderr);
    }
}

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
    const auto output = task_output_dir(task);
    TaskProgress progress(output);
    check_options(task);
    // Generated Verilog refers to files in scratch. Keep the SALT stem out
    // of those paths, since it may contain characters such as a newline.
    Scratch scratch(output.parent_path(), ".crisp-ebmc.XXXXXX");
    std::string error, events;
    if (!veval::util::ensureWaitableChildren(error)) throw std::runtime_error(error);
    std::optional<CompiledVv> compiled;
    if (task.frontend.value == front::Frontend::goto_vv) {
        compiled.emplace(scratch.path);
        if (!compile_vv(task, scratch.path, *compiled, deadline)) {
            summary.error = true;
            return;
        }
    } else if (task.rtl_mode.value == front::RtlMode::strict && !check_rtl(task, scratch.path, deadline)) {
        summary.error = true;
        return;
    }
    const auto timeout = "timeout after " + std::to_string(seconds) + " s";
    // veval on / only (spec 25).
    VevalResult local;
    if (task.veval.value != front::ToolUse::off) {
        local = run_veval(task, scratch.path, deadline, timeout, progress);
        if (local.failed) {
            summary.error = true;
            std::cerr << "crisp: veval could not lower the design\n";
            std::ifstream log(scratch.path / "diagnostics.log");
            if (log.peek() != std::ifstream::traits_type::eof()) std::cerr << log.rdbuf();
            return;
        }
        if (task.veval.value == front::ToolUse::only) {
            finish_local_vacuity(local, deadline, progress);
            for (const auto& line : local.lines) print_line(line, summary, start);
            if (local.lines.empty()) {
                summary.incomplete = true;
                std::cout << "  no assertions\n";
            }
            return;
        }
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
        const auto operation = lasso_json_enabled() ? EngineRun::solve_with_lasso : EngineRun::solve;
        return run_worker(task, scratch.path, deadline, events, operation, local.decided,
                          compiled ? compiled->path() : fs::path{});
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
        if (!reports.primary_complete) summary.error = true;
        if (WIFSIGNALED(wait.status))
            std::cerr << "crisp: EBMC terminated by signal " << WTERMSIG(wait.status) << '\n';
    }
    if (properties.empty() && !failed && local.decided.empty()) {
        summary.incomplete = true;
        std::cout << (wait.timedOut ? "  unknown  " + timeout + " before assertion discovery\n" : "  no assertions\n");
    }
    for (std::size_t i = 0; i < properties.size(); ++i) {
        auto& p = properties[i];
        // Discovery alone carries no assertion result.
        const bool reported = p.status != "pending";
        if (p.status == "refuted" && p.detail.empty()) {
            auto dest = wave_path(task, p.name);
            if (publish_file(scratch.path / (std::to_string(i) + ".vcd"), dest, deadline)) {
                p.detail = dest.string();
                if (lasso_json_enabled()) {
                    const auto from = scratch.path / (std::to_string(i) + ".witness.json");
                    const auto name = file_name(p.name) + "." + progress.artifact_tag() + ".witness.json";
                    const auto certificate = dest.parent_path() / name;
                    try {
                        if (fs::is_regular_file(from)) {
                            if (publish_file(from, certificate, deadline))
                                std::cout << "  witness  " << certificate.string() << '\n';
                            else
                                std::cerr << "crisp: warning: witness export timed out for " << p.name << '\n';
                        } else if (auto why = reports.witness_unavailable.find(i);
                                   why != reports.witness_unavailable.end()) {
                            std::cerr << "crisp: warning: no witness export for " << p.name << ": "
                                      << why->second << '\n';
                        }
                    } catch (const std::exception& error) {
                        std::cerr << "crisp: warning: witness export failed for " << p.name << ": "
                                  << error.what() << '\n';
                    }
                }
            } else { p.status = "unknown"; p.detail = timeout; }
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
        if (reported) progress.record("final", p);
        print_line(p, summary, start);
    }
    if (diagnostics) {
        std::ifstream log(scratch.path / "diagnostics.log");
        if (log.peek() != std::ifstream::traits_type::eof()) std::cerr << log.rdbuf();
        else if (failed && !rejected) std::cerr << "crisp: EBMC failed during verification\n";
    }
    finish_local_vacuity(local, deadline, progress);
    for (const auto& line : local.lines)
        if (local.decided.count(line.name)) print_line(line, summary, start);
}
} // namespace crisp::check
