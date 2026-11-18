#include "TaskInputAndExecution.h"
#ifdef __APPLE__
#include "util/task/CommandArgumentsAndFiles.h"
#endif
#include "util/process/RunExternalCommand.h"
#include "util/files/AtomicOutput.h"
#include "util/files/Sha256.h"
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <unistd.h>

namespace veval::workflow {
namespace task = veval::util::command_runner;
void require(bool ok, const std::string &reason) {
    if (!ok) throw std::runtime_error(reason);
}
std::string readText(const fs::path &path) { return task::read(path); }
void writeText(const fs::path &path, const std::string &value) {
    fs::create_directories(path.parent_path());
    veval::util::writeAtomic(path.string(), [&](std::ostream &s) { s << value; });
}
std::string hashFile(const fs::path &path, const fs::path &scratch) {
    if (fs::file_size(path) <= 8 * 1024 * 1024)
        return veval::sha256(readText(path));
#ifdef __APPLE__
    (void)scratch;
    return veval::verification::fileDigest(path);
#else
    fs::create_directories(scratch);
    auto result = task::query({"sha256sum", "--", fs::absolute(path).string()}, scratch / "hash.log", 0, true);
    auto digest = result.substr(0, 64);
    require(std::regex_match(digest, std::regex("[a-f0-9]{64}")), "invalid SHA256 receipt: " + path.string());
    return digest;
#endif
}
J fileIdentity(const fs::path &path, const fs::path &scratch) {
    return object({{"path", str(fs::absolute(path).lexically_normal().string())},
        {"resolved_path", str(fs::canonical(path).string())}, {"sha256", str(hashFile(path, scratch))}});
}
std::string executable(const std::string &name) {
    if (name.find('/') != std::string::npos) {
        require(::access(name.c_str(), X_OK) == 0, "executable unavailable: " + name);
        return fs::canonical(name).string();
    }
    const auto path = task::env("PATH");
    std::istringstream paths(path); std::string entry;
    while (std::getline(paths, entry, ':')) {
        auto candidate = fs::path(entry.empty() ? "." : entry) / name;
        if (::access(candidate.c_str(), X_OK) == 0) return fs::absolute(candidate).string();
    }
    throw std::runtime_error("required executable unavailable: " + name);
}
std::vector<std::string> sbyCommand() { return {executable("sby")}; }
std::string nativeHelper(const fs::path &binary, const std::string &name, const char *environment) {
    const auto configured = task::env(environment);
    const auto path = configured.empty() ? binary.parent_path() / name : fs::path(configured);
    require(::access(path.c_str(), X_OK) == 0, "native helper unavailable: " + path.string());
    return fs::absolute(path).string();
}
std::vector<std::string> strings(const J &v) {
    std::vector<std::string> out;
    for (const auto &item : items(v)) out.push_back(text(item));
    return out;
}
J stringArray(const std::vector<std::string> &v) {
    J out = list(); for (const auto &s : v) append(out, str(s)); return out;
}
double numeric(const J &v, double fallback) { return v.is_number() ? std::stod(v.value) : fallback; }
J jsonFile(const fs::path &path) { return parse(readText(path)); }
bool interrupted() { return task::cancellationSignal() != 0; }
}
