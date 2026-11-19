// The shared frontend uses the CBMC run interface to call Spot. Each
// invocation must release its capture pipe, including failed redirections.
#include "ChildProcess.h"
#include "ProcessPipeCompatibility.h"
#include <util/run.h>

#include <array>
#include <exception>
#include <ostream>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

namespace {
struct CapturePipe {
    int readFd = -1;
    int writeFd = -1;
    ~CapturePipe() {
        if (readFd >= 0) close(readFd);
        if (writeFd >= 0) close(writeFd);
    }
    bool open() {
        int ends[2];
        if (veval::util::closeOnExecPair(ends, false) < 0) return false;
        readFd = ends[0];
        writeFd = ends[1];
        std::string error;
        return veval::util::moveAboveStandardDescriptors(readFd, error, "capture pipe") &&
               veval::util::moveAboveStandardDescriptors(writeFd, error, "capture pipe");
    }
};

int runFrontend(const std::string& program, const std::vector<std::string>& argv,
                const std::string& input, const std::string& output,
                const std::string& error, std::ostream* capture) {
    CapturePipe pipe;
    if (capture && !pipe.open()) return 1;
    std::vector<char*> args;
    for (const auto& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) return 1;
    auto redirect = [&](int fd, const std::string& path, int flags) {
        return path.empty() || posix_spawn_file_actions_addopen(&actions, fd, path.c_str(), flags, 0666) == 0;
    };
    const auto writeFlags = O_WRONLY | O_CREAT | O_TRUNC;
    const bool ready = redirect(STDIN_FILENO, input, O_RDONLY) &&
                       redirect(STDERR_FILENO, error, writeFlags) &&
                       (capture ? posix_spawn_file_actions_adddup2(&actions, pipe.writeFd, STDOUT_FILENO) == 0
                                : redirect(STDOUT_FILENO, output, writeFlags));
    pid_t child;
    const int failed = ready ? posix_spawnp(&child, program.c_str(), &actions, nullptr, args.data(), environ) : 1;
    posix_spawn_file_actions_destroy(&actions);
    if (failed) return 1;

    std::exception_ptr streamError;
    bool readError = false;
    if (capture) {
        close(pipe.writeFd);
        pipe.writeFd = -1;
        std::array<char, 16384> buffer;
        for (;;) {
            const auto count = read(pipe.readFd, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { readError = count < 0; break; }
            // Drain and reap the child even if the destination stream throws.
            if (!streamError) {
                try { capture->write(buffer.data(), count); }
                catch (...) { streamError = std::current_exception(); }
            }
        }
        close(pipe.readFd);
        pipe.readFd = -1;
    }
    int status;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return 1;
    if (streamError) std::rethrow_exception(streamError);
    if (readError) return 1;
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
} // namespace

int run(const std::string& program, const std::vector<std::string>& args,
        const std::string& input, std::ostream& output, const std::string& error) {
    return runFrontend(program, args, input, {}, error, &output);
}

int run(const std::string& program, const std::vector<std::string>& args,
        const std::string& input, const std::string& output, const std::string& error) {
    return runFrontend(program, args, input, output, error, nullptr);
}

int run(const std::string& program, const std::vector<std::string>& args) {
    return run(program, args, {}, {}, {});
}
