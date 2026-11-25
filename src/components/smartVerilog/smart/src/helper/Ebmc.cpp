#include "Ebmc.h"

#include "Shell.h"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace smart::helper {
namespace {
namespace fs = std::filesystem;

int run(const std::string& command, int timeoutSeconds)
{
    const auto parent = getpid();
    const pid_t pid = fork();
    if(pid < 0)
        return -2;
    if(pid == 0)
    {
#ifdef __linux__
        if(prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent)
            _exit(127);
#endif
        if(timeoutSeconds > 0)
            setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
        _exit(127);
    }
    if(timeoutSeconds > 0)
        setpgid(pid, pid);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(timeoutSeconds);
    int status = 0;
    for(;;)
    {
        const auto waited = waitpid(pid, &status, timeoutSeconds > 0 ? WNOHANG : 0);
        if(waited == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
        if(waited < 0)
        {
            if(errno == EINTR)
                continue;
            return -2;
        }
        if(std::chrono::steady_clock::now() >= deadline)
        {
            kill(-pid, SIGKILL);
            while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

std::string attribute(const std::string& line, const std::string& name)
{
    const auto key = " " + name + "=\"";
    const auto at = line.find(key);
    if(at == std::string::npos)
        return {};
    const auto start = at + key.size();
    const auto end = line.find('"', start);
    return end == std::string::npos ? std::string{} : line.substr(start, end - start);
}

}  // namespace

const char* statusName(EbmcStatus status)
{
    switch (status) {
    case EbmcStatus::Verified: return "verified";
    case EbmcStatus::Refuted: return "refuted";
    case EbmcStatus::Unknown: return "unknown";
    case EbmcStatus::TimedOut: return "timed-out";
    case EbmcStatus::Error: return "error";
    }
    return "error";
}

std::string ebmcExecutable()
{
    if(const auto* overridePath = std::getenv("SMART_EBMC"))
        return overridePath;
    return SMART_EBMC_PATH;
}

bool allowConstantOobRead()
{
    const auto* value = std::getenv("SMART_ALLOW_CONSTANT_OOB_READ");
    return value && std::string(value) == "1";
}

std::string ebmcCommand(const std::string& designFile, const EbmcOptions& options)
{
    std::ostringstream command;
    command << shellQuote(options.ebmc.empty() ? ebmcExecutable() : options.ebmc);
    if(options.designFileIndex > options.extraFiles.size())
        throw std::runtime_error("invalid EBMC design file index");
    auto files = options.extraFiles;
    files.insert(files.begin() + options.designFileIndex, designFile);
    for(const auto& file : files)
        command << ' ' << shellQuote(file);
    command << " -D FORMAL --systemverilog";
    if(allowConstantOobRead())
        command << " --allow-constant-oob-read";
    if(options.unbounded)
        command << " --k-induction";
    else
        command << " --bound " << options.bound;
    if(!options.topModule.empty())
        command << " --top " << shellQuote(options.topModule);
    return command.str();
}

std::string freshPropertyLabel(const std::string& source,
                               const std::vector<std::string>& extraFiles)
{
    std::ostringstream contents;
    contents << source;
    for(const auto& file : extraFiles)
    {
        std::ifstream in(file);
        if(!in)
            throw std::runtime_error("cannot read " + file);
        contents << in.rdbuf();
    }

    const auto text = contents.str();
    std::string label = "__crisp_smart_property";
    while(text.find(label) != std::string::npos)
        label += '_';
    return label;
}

EbmcResult checkProperty(const std::string& designFile, const EbmcOptions& options,
                         const std::string& label, const std::string& trace,
                         const std::string& preferredScope)
{
    const auto log = designFile + ".ebmc.log";
    static std::atomic<unsigned> traceNumber{0};
    const auto prefix = designFile + ".trace." + std::to_string(getpid()) + "." +
                        std::to_string(traceNumber.fetch_add(1, std::memory_order_relaxed));
    auto command = "exec " + ebmcCommand(designFile, options) + " --xml-ui";
    if(!trace.empty())
        command += " --vcd " + shellQuote(prefix);
    command += " > " + shellQuote(log) + " 2>&1";
    const auto exitCode = run(command, options.timeoutSeconds);
    if(exitCode == -1)
        return {EbmcStatus::TimedOut, log};
    if(exitCode != 0 && exitCode != 10)
        return {EbmcStatus::Error, "EBMC exited with status " + std::to_string(exitCode) +
                                   " (" + log + ")"};

    const auto suffix = "." + label;
    const std::string root = "Verilog::$root.";
    std::map<std::string, std::string> refuted;
    std::size_t found = 0;
    bool preferredFound = false;
    const auto preferredName = preferredScope + suffix;
    bool unknown = false;
    std::ifstream results(log);
    for(std::string line; std::getline(results, line);)
    {
        if(line.rfind("<result ", 0) != 0)
            continue;
        const auto name = attribute(line, "property");
        if(name.rfind(root, 0) != 0 || name.size() <= suffix.size() ||
           name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        ++found;
        const auto display = name.substr(root.size());
        preferredFound = preferredFound || display == preferredName;
        const auto status = attribute(line, "status");
        if(status == "PROVED" ||
           (!options.unbounded && status == "PROVED up to bound " + std::to_string(options.bound)))
            continue;
        if(status == "REFUTED" || status.rfind("REFUTED up to bound ", 0) == 0)
        {
            refuted.emplace(display, prefix + "." + display + "_counterexample.vcd");
        }
        else if(status.rfind("UNKNOWN", 0) == 0 || status.rfind("INCONCLUSIVE", 0) == 0)
            unknown = true;
        else
            return {EbmcStatus::Error, status + " for " + name + " (" + log + ")"};
    }

    if(found == 0 || results.bad())
        return {EbmcStatus::Error, "no result for " + label + " (" + log + ")"};
    if(!preferredScope.empty() && !preferredFound)
        return {EbmcStatus::Error, "no result for " + preferredName + " (" + log + ")"};

    if(!refuted.empty())
    {
        if(trace.empty())
            return {EbmcStatus::Refuted, {}};
        const auto selected = preferredScope.empty()
            ? (found == 1 ? refuted.begin() : refuted.end())
            : refuted.find(preferredName);
        if(selected == refuted.end())
            return {EbmcStatus::Refuted, "no counterexample for the mined instance"};
        std::error_code error;
        fs::rename(selected->second, trace, error);
        if(error)
            return {EbmcStatus::Error, "missing EBMC counterexample " + selected->second + " (" + log + ")"};
        return {EbmcStatus::Refuted, {}, trace};
    }
    if(unknown)
        return {EbmcStatus::Unknown, log};
    std::error_code ignored;
    fs::remove(log, ignored);
    return {EbmcStatus::Verified, {}};
}

}  // namespace smart::helper
