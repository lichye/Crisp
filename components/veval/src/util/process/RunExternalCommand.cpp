#include "RunExternalCommand.h"
#include "util/process/ChildProcess.h"
#include "util/process/ProcessGroup.h"
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <fcntl.h>
#include <stdexcept>
#include <sys/wait.h>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace veval::util::command_runner {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
namespace {
void need(bool ok, const std::string& why) {
    if (!ok) throw std::runtime_error(why);
}
volatile sig_atomic_t stopSignal = 0;
void stop(int value) { stopSignal = value; }
}
std::string read(const fs::path& path) {
    std::ifstream in(path);
    need(bool(in), "Cannot read " + path.string());
    return {std::istreambuf_iterator<char>(in), {}};
}
std::string env(const char* key, const std::string& fallback) {
    const auto* value = std::getenv(key);
    return value && *value ? value : fallback;
}
int cancellationSignal() { return stopSignal; }
void installCancellationHandlers() {
    struct sigaction action{}; action.sa_handler = stop; sigemptyset(&action.sa_mask);
    for (int signal : {SIGINT, SIGTERM, SIGHUP})
        need(::sigaction(signal, &action, nullptr) == 0, "Cannot install task cancellation handler");
}

// Commands use argv vectors. Process-group cleanup also runs on timeout or cancellation.
int command(const std::vector<std::string>& arguments, const fs::path& log,
            double seconds, bool progress, bool cancellable,
            const fs::path& directory) {
    need(!arguments.empty(), "Empty command");
    std::vector<char*> argv;
    for (const auto& arg : arguments) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    need(fd >= 0, "Cannot open command log: " + log.string());
    std::string descriptorError;
    if (!util::moveAboveStandardDescriptors(fd, descriptorError, "task log")) {
        ::close(fd); throw std::runtime_error(descriptorError);
    }
#ifdef __APPLE__
    const auto child = util::forkProofContext();
#else
    const auto child = ::fork();
#endif
    if (child == 0) {
        if (!directory.empty() && ::chdir(directory.c_str())) ::_exit(127);
#ifndef __APPLE__
        if (::setpgid(0, 0)) ::_exit(127);
#endif
        if (::dup2(fd, STDOUT_FILENO) < 0 || ::dup2(fd, STDERR_FILENO) < 0) ::_exit(127);
        if (fd > STDERR_FILENO) ::close(fd);
        ::execvp(argv[0], argv.data()); ::_exit(127);
    }
    ::close(fd); need(child > 0, "Cannot fork command");
    std::ifstream tail; std::string pending;
    if (progress) tail.open(log);
    auto forward = [&] {
        if (cancellable && stopSignal) throw std::runtime_error("Task interrupted");
        if (!progress) return;
        tail.clear(); char block[4096];
        while (tail.read(block, sizeof block) || tail.gcount()) pending.append(block, static_cast<size_t>(tail.gcount()));
        size_t end;
        while ((end = pending.find('\n')) != pending.npos) {
            const auto line = pending.substr(0, end); pending.erase(0, end + 1);
            const auto message = line.find("## ");
            if (line.find("engine_") != line.npos && message != line.npos)
                std::cout << "SBY " << line.substr(message) << '\n' << std::flush;
        }
    };
    // Zero explicitly requests no execution deadline. Existing callers keep
    // their finite defaults; check delegates budgeting to its whole task.
    const auto deadline = seconds == 0 ? Clock::time_point::max() :
        Clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000));
    const auto status = util::waitForProofContext(child, deadline,
        std::chrono::milliseconds(25), forward);
    forward();
    need(status.error.empty(), status.error);
    if (status.timedOut) return 124;
    return WIFEXITED(status.status) ? WEXITSTATUS(status.status) : 128 + WTERMSIG(status.status);
}
std::string query(const std::vector<std::string>& args, const fs::path& log, double seconds, bool cancellable) {
    const auto code = command(args, log, seconds, false, cancellable);
    need(code == 0, "Command failed (" + std::to_string(code) + "): " + read(log));
    return read(log);
}
} // namespace veval::util::command_runner
