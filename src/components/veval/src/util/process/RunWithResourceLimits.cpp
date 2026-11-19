#include "util/process/CurrentExecutablePath.h"
#include "RunWithResourceLimits.h"
#include "util/process/ProcessSession.h"
#include "util/process/ChildProcess.h"
#include "util/process/ProcessPipeCompatibility.h"
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace veval::verification {
std::string program(const std::string &name, const std::string &prefix) {
    if (name.find('/') != name.npos) {
        if (::access(name.c_str(), X_OK) == 0) return absolute(name);
        throw std::runtime_error("executable unavailable: " + name);
    }
    auto path = prefix + (prefix.empty() ? "" : ":") + (std::getenv("PATH") ? std::getenv("PATH") : "");
    std::istringstream paths(path); std::string part;
    while (std::getline(paths, part, ':')) {
        auto p = fs::path(part.empty() ? "." : part) / name;
        if (::access(p.c_str(), X_OK) == 0) return absolute(p.string());
    }
    throw std::runtime_error("executable unavailable: " + name);
}
fs::path helper(const std::string &name, const std::string &environment) {
    if (!environment.empty()) if (auto p = std::getenv(environment.c_str())) return program(p);
    if (auto p = std::getenv("CONSFLOW_BUILD_DIR")) return program((fs::path(p) / name).string());
    return program((veval::util::currentExecutable().parent_path() / name).string());
}
Execution execute(const std::vector<std::string> &args, const fs::path &log,
                  const fs::path &cwd, double deadline, unsigned long long memoryBytes,
                  double cleanupSeconds, bool shareSupervisorSession, const fs::path &stderrLog,
                  bool (*externalCancellation)()) {
    if (args.empty() || std::isnan(deadline) || !std::isfinite(cleanupSeconds) || cleanupSeconds <= 0)
        throw std::runtime_error("invalid process request");
#ifndef __APPLE__
    if (shareSupervisorSession) throw std::runtime_error("shared supervisor session is only supported on macOS");
#else
    if (shareSupervisorSession) {
        if (::getsid(0) != ::getpid())
            throw std::runtime_error("shared supervisor must be the session leader");
        std::size_t members = 0;
        std::string error;
        if (!util::inspectSessionExcept(::getpid(), ::getpid(), members, error))
            throw std::runtime_error("cannot inspect supervisor session: " + error);
        if (members)
            throw std::runtime_error("shared supervisor session already has other members");
    }
    if (memoryBytes) throw std::runtime_error("process-tree memory limits are not supported on macOS");
#endif
    auto isCancelled = [&] { return cancelled() || (externalCancellation && externalCancellation()); };
    Execution result; const auto start = monotonic();
    std::string descriptorError;
    if (!util::ensureWaitableChildren(descriptorError)) throw std::runtime_error(descriptorError);
    if (isCancelled() || start >= deadline) {
        result.stopped = isCancelled(); result.timedOut = !result.stopped; result.cleanupComplete = true; return result;
    }
    fs::create_directories(log.parent_path());
    if (!stderrLog.empty()) fs::create_directories(stderrLog.parent_path());
    int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("cannot open process log: " + log.string());
    if (!util::moveAboveStandardDescriptors(fd, descriptorError, "process log")) {
        ::close(fd); throw std::runtime_error(descriptorError);
    }
    int errFd = fd;
    if (!stderrLog.empty()) {
        errFd = ::open(stderrLog.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (errFd < 0) { ::close(fd); throw std::runtime_error("cannot open process error log: " + stderrLog.string()); }
        if (!util::moveAboveStandardDescriptors(errFd, descriptorError, "process error log")) {
            ::close(errFd); ::close(fd); throw std::runtime_error(descriptorError);
        }
    }
    int barrier[2];
    if (util::closeOnExecPair(barrier, true)) { if (errFd != fd) ::close(errFd); ::close(fd); throw std::runtime_error("cannot create launch barrier"); }
    if (!util::moveAboveStandardDescriptors(barrier[0], descriptorError, "launch barrier") ||
        !util::moveAboveStandardDescriptors(barrier[1], descriptorError, "launch barrier")) {
        if (errFd != fd) ::close(errFd); ::close(fd); ::close(barrier[0]); ::close(barrier[1]); throw std::runtime_error(descriptorError);
    }
    std::vector<char *> argv; for (const auto &s : args) argv.push_back(const_cast<char *>(s.c_str())); argv.push_back(nullptr);
    const auto pid = ::fork();
    if (pid == 0) {
        ::close(barrier[0]);
        if ((!shareSupervisorSession && ::setsid() < 0) || (!cwd.empty() && ::chdir(cwd.c_str())) || ::dup2(fd, 1) < 0 || ::dup2(errFd, 2) < 0) ::_exit(126);
        if (errFd != fd) ::close(errFd);
        ::close(fd);
        if (memoryBytes) {
            struct rlimit limit{static_cast<rlim_t>(memoryBytes), static_cast<rlim_t>(memoryBytes)};
            if (::setrlimit(RLIMIT_AS, &limit)) ::_exit(126);
        }
#ifdef __APPLE__
        if (::signal(SIGCONT, SIG_DFL) == SIG_ERR) ::_exit(126);
#endif
        const char ready = 'R'; if (::send(barrier[1], &ready, 1, MSG_NOSIGNAL) != 1) ::_exit(126);
        char go = 0; ssize_t n;
        do { n = ::read(barrier[1], &go, 1); } while (n < 0 && errno == EINTR);
        if (n != 1 || go != 'G') ::_exit(126);
        ::close(barrier[1]);
        ::signal(SIGINT, SIG_DFL); ::signal(SIGTERM, SIG_DFL);
        ::execvp(argv[0], argv.data()); ::_exit(127);
    }
    if (errFd != fd) ::close(errFd);
    ::close(fd); ::close(barrier[1]);
    if (pid < 0) { ::close(barrier[0]); throw std::runtime_error("cannot fork process"); }
    bool launched = false, done = false;
    // Before the acknowledgement the child cannot spawn descendants. Holding
    // the direct child unreaped prevents its PID/session identifier being reused.
    while (!launched && !done) {
        if (isCancelled() || monotonic() >= deadline) {
            result.stopped = isCancelled(); result.timedOut = !result.stopped;
            ::kill(pid, SIGKILL); done = true; break;
        }
        pollfd pfd{barrier[0], POLLIN | POLLHUP, 0};
        const auto n = ::poll(&pfd, 1, 20);
        if (n < 0 && errno != EINTR) { result.error = "launch barrier poll failed"; ::kill(pid,SIGKILL); done = true; break; }
        if (n > 0) {
            char ready; const auto count = ::read(barrier[0], &ready, 1);
            if (count == 1 && ready == 'R') {
#ifdef __APPLE__
                const bool readyForLaunch = shareSupervisorSession
                    ? util::probeSessionMember(pid, ::getpid(), result.error)
                    : util::probePidfd(pid, pid, result.error);
#else
                const bool readyForLaunch = util::probePidfd(pid, pid, result.error);
#endif
                if (!readyForLaunch) { ::kill(pid,SIGKILL); done = true; break; }
                const char go='G';
                if (::send(barrier[0], &go, 1, MSG_NOSIGNAL) != 1) {result.error="launch barrier send failed";::kill(pid,SIGKILL);done=true;break;}
                launched = true;
            } else if (count == 0) done = true;
            else if (count < 0 && errno != EINTR) {result.error="launch barrier read failed";::kill(pid,SIGKILL);done=true;break;}
        }
    }
    ::close(barrier[0]);
    while (launched && !done) {
        siginfo_t info{};
        if (::waitid(P_PID, pid, &info, WEXITED | WNOHANG | WNOWAIT) < 0) {
            if (errno == EINTR) continue;
            result.error = "waitid failed: " + std::string(std::strerror(errno)); break;
        }
        if (info.si_pid == pid) {done = true;break;}
        if (isCancelled() || monotonic() >= deadline) {
            result.stopped = isCancelled(); result.timedOut = !result.stopped; break;
        }
        ::poll(nullptr,0,20);
    }
    result.solving=monotonic()-start;
    // Give a native child time to cancel its own nested solver session before
    // reclaiming the outer session. This grace never extends solving time.
    if (launched && !done && (result.stopped || result.timedOut)) {
        ::kill(pid, SIGTERM);
        const auto graceEnd = monotonic() + cleanupSeconds;
        while (monotonic() < graceEnd) {
            siginfo_t info{};
            if (::waitid(P_PID, pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == pid) break;
            ::poll(nullptr, 0, 20);
        }
    }
    const double cleanupEnd = monotonic() + cleanupSeconds;
    if (launched) {
        while (true) {
            size_t active = 0; std::string error;
#ifdef __APPLE__
            const bool known = shareSupervisorSession
                ? util::inspectSessionExcept(::getpid(), ::getpid(), active, error)
                : util::inspectSession(pid,active,error);
#else
            const bool known = util::inspectSession(pid,active,error);
#endif
            if (known && !active) { result.cleanupComplete=true;break; }
            if (monotonic() >= cleanupEnd) {result.error="process cleanup unconfirmed: "+error;break;}
            // On ordinary completion this also reclaims children left behind
            // by an exited wrapper. No unrelated session is signalled.
#ifdef __APPLE__
            const bool signalled = shareSupervisorSession
                ? util::signalSessionExcept(::getpid(), ::getpid(), SIGKILL, error)
                : util::signalSession(pid,SIGKILL,error);
#else
            const bool signalled = util::signalSession(pid,SIGKILL,error);
#endif
            if (!signalled) result.error=error;
            ::poll(nullptr,0,20);
        }
    } else result.cleanupComplete = true;
    int status = 0; struct rusage usage{};
    // Never turn an unkillable child into an unbounded final wait.
    while (true) {
        const auto waited=::wait4(pid,&status,WNOHANG,&usage);
        if (waited==pid) {result.exitCode=WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
#ifdef __APPLE__
            result.maxRssKb=usage.ru_maxrss/1024;
#else
            result.maxRssKb=usage.ru_maxrss;
#endif
            break;}
        if (waited<0 && errno!=EINTR) {result.cleanupComplete=false;result.error="wait4 failed";break;}
        if (monotonic() >= cleanupEnd) {result.cleanupComplete=false;result.error="process leader did not exit before cleanup deadline";break;}
        ::poll(nullptr,0,20);
    }
    result.elapsed=monotonic()-start;
    result.cleanup=result.elapsed-result.solving;
    return result;
}
} // namespace veval::verification
