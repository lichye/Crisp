#include "run.hpp"
#include "engine.hpp"

#include <util/process/ChildProcess.h>
#include <util/process/ProcessGroup.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>

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

[[noreturn]] void worker(const front::Task& task, const fs::path& scratch, Reporter reporter) {
    int status = 2;
    try {
#ifndef __APPLE__
        if (::setpgid(0, 0) < 0) throw os_error("isolate task process");
#endif
        fs::current_path(scratch); // Keep any scratch files created by IC3 private to this task.
        if (!std::freopen("diagnostics.log", "w", stderr) || ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
            throw os_error("redirect EBMC diagnostics");
        const struct rlimit no_core{0, 0};
        if (::setrlimit(RLIMIT_CORE, &no_core) < 0) throw os_error("disable task core dumps");
        if (task.ram_mib) set_memory_limit(task.ram_mib->value);
        run_engine(task, scratch, reporter);
        status = 0;
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

void check_options(const front::Task& task) {
    if (!task.params.empty()) {
        const auto& param = task.params.front();
        throw std::runtime_error(param.source->file.string() + ":" + std::to_string(param.source->line) +
                                 ": param " + param.value.name + " is not supported");
    }
    if (task.clock) std::cout << "  clock " << task.clock->value << " is recorded only\n";
    auto tool = [](const char* name, front::ToolUse setting) {
        if (setting == front::ToolUse::only)
            throw std::runtime_error(std::string(name) + " only is not supported");
        if (setting == front::ToolUse::on)
            std::cout << "  " << name << " is not connected; continuing with EBMC\n";
    };
    tool("smart", task.smart.value);
    tool("veval", task.veval.value);
}

struct Property {
    std::string name, status, detail;
};

struct TaskEvents {
    std::vector<Property> properties;
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

fs::path wave_path(const front::Task& task, const std::string& name) {
    auto dir = task.salt_file.stem().string();
    if (task.name) dir += "_" + task.name->value;
    if (dir == "." || dir == "..")
        throw std::runtime_error("cannot use '" + dir + "' as a VCD directory name");
    std::string file;
    for (char c : name) {
        if (c == '/') file += "%2F";
        else if (c == '%') file += "%25";
        else file += c;
    }
    return fs::path("crisp-out") / dir / (file + ".vcd");
}

// Rename publishes a complete trace and replaces an existing file atomically.
// When TMPDIR is on another filesystem, finish a local copy before the rename.
bool publish_wave(const fs::path& from, const fs::path& to, Clock::time_point deadline) {
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

void run(const front::Task& task, Summary& summary) {
    const auto start = Clock::now();
    const auto max_seconds = std::chrono::duration_cast<std::chrono::seconds>(Clock::time_point::max() - start);
    const auto seconds = std::min<std::uint64_t>(task.timeout_seconds.value, max_seconds.count());
    const auto deadline = start + std::chrono::seconds(seconds);
    check_options(task);
    Scratch scratch;
    Pipe pipe;
    std::string error, events;
    if (!veval::util::ensureWaitableChildren(error)) throw std::runtime_error(error);
    for (int& fd : pipe.fd)
        if (!veval::util::moveAboveStandardDescriptors(fd, error, "result pipe")) throw std::runtime_error(error);
    if (::fcntl(pipe.fd[0], F_SETFL, O_NONBLOCK) < 0) throw os_error("configure result pipe");
    std::cout.flush();
    std::cerr.flush();
    const auto pid = veval::util::forkProofContext();
    if (pid < 0) throw os_error("start EBMC");
    if (pid == 0) {
        pipe.close_end(0);
        worker(task, scratch.path, {pipe.fd[1]});
    }
    pipe.close_end(1);
    const auto wait = veval::util::waitForProofContext(pid, deadline, std::chrono::milliseconds(5),
                                                       [&] { pipe.drain(events); });
    pipe.drain(events);
    if (!wait.error.empty()) throw std::runtime_error(wait.error);
    const bool failed = !wait.timedOut && (!WIFEXITED(wait.status) || WEXITSTATUS(wait.status) != 0);
    auto reports = read_events(events);
    for (const auto& warning : reports.undriven) std::cerr << "crisp: warning: " << warning << '\n';
    auto& properties = reports.properties;
    const auto timeout = "timeout after " + std::to_string(seconds) + " s";
    bool diagnostics = failed;
    if (failed) {
        summary.error = true;
        if (WIFSIGNALED(wait.status))
            std::cerr << "crisp: EBMC terminated by signal " << WTERMSIG(wait.status) << '\n';
    }
    if (properties.empty() && !failed) {
        summary.incomplete = true;
        std::cout << (wait.timedOut ? "  unknown  " + timeout + " before assertion discovery\n" : "  no assertions\n");
    }
    for (std::size_t i = 0; i < properties.size(); ++i) {
        auto& p = properties[i];
        if (p.status == "refuted") {
            auto dest = wave_path(task, p.name);
            if (publish_wave(scratch.path / (std::to_string(i) + ".vcd"), dest, deadline))
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
        if (p.status == "proved") ++summary.proved;
        else if (p.status == "passed") ++summary.passed;
        else if (p.status == "refuted") ++summary.refuted;
        else ++summary.unknown;
        const std::chrono::duration<double> elapsed = Clock::now() - start;
        std::cout << "  " << p.name << "  " << p.status << "  " << std::fixed << std::setprecision(3)
                  << elapsed.count() << " s";
        if (!p.detail.empty()) std::cout << "  " << p.detail;
        std::cout << '\n';
    }
    if (diagnostics) {
        std::ifstream log(scratch.path / "diagnostics.log");
        if (log.peek() != std::ifstream::traits_type::eof()) std::cerr << log.rdbuf();
        else if (failed) std::cerr << "crisp: EBMC failed during verification\n";
    }
}
} // namespace crisp::check
