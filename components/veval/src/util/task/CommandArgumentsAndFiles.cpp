#include "CommandArgumentsAndFiles.h"
#include "util/files/AtomicOutput.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <memory>
#include <openssl/evp.h>
#include <sstream>

namespace veval::verification {
std::string read(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + p.string());
    std::ostringstream out;
    out << f.rdbuf();
    if (f.bad()) throw std::runtime_error("read failed: " + p.string());
    return out.str();
}
void write(const fs::path &p, const std::string &s) {
    if (!p.parent_path().empty()) fs::create_directories(p.parent_path());
    util::writeAtomic(p.string(), [&](std::ostream &out) { out << s; });
}
class Digest {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{EVP_MD_CTX_new(), EVP_MD_CTX_free};
public:
    Digest() {
        if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
            throw std::runtime_error("cannot initialize SHA256");
    }
    void add(const char *s, size_t n) {
        if (EVP_DigestUpdate(ctx.get(), s, n) != 1)
            throw std::runtime_error("SHA256 update failed");
    }
    std::string finish() {
        unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned length = 0;
        if (EVP_DigestFinal_ex(ctx.get(), bytes, &length) != 1 || length != 32)
            throw std::runtime_error("SHA256 finish failed");
        std::ostringstream out;
        for (unsigned i = 0; i < length; ++i)
            out << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]);
        return out.str();
    }
};
std::string digest(const std::string &s) { Digest d; d.add(s.data(), s.size()); return d.finish(); }
std::string fileDigest(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot hash " + p.string());
    Digest d; std::array<char, 65536> buffer;
    while (f) {
        // Expired, not a plain error: a cancelled attempt must stay retryable,
        // and the adapter records a plain error as a final `error' Result.
        if (cancelled()) throw Expired("cancelled while hashing " + p.string());
        f.read(buffer.data(), buffer.size());
        d.add(buffer.data(), static_cast<size_t>(f.gcount()));
    }
    if (!f.eof()) throw std::runtime_error("hash read failed: " + p.string());
    return d.finish();
}
std::string identity(J job) {
    J body = object();
    for (const auto &entry : members(job))
        if (entry.first != "job_id") put(body, entry.first, entry.second);
    return digest(canonical(body));
}
std::string absolute(const std::string &s) { return fs::absolute(s).lexically_normal().string(); }
J real(double n) {
    // The existing Job contract preserves JSON float spelling in its digest.
    J result = num(n);
    if (result.value.find_first_of(".eE") == std::string::npos) result.value += ".0";
    return result;
}
Arguments::Arguments(int argc, char **argv, const std::vector<std::string> &flags,
                     const std::vector<std::string> &options) {
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (key == "--") { while (++i < argc) positional.emplace_back(argv[i]); break; }
        if (std::find(flags.begin(), flags.end(), key) != flags.end()) values[key].push_back("true");
        else if (std::find(options.begin(), options.end(), key) != options.end()) {
            if (++i == argc) throw std::runtime_error("missing value for " + key);
            values[key].emplace_back(argv[i]);
        } else if (key.rfind("--", 0) == 0) throw std::runtime_error("unknown option " + key);
        else positional.push_back(key);
    }
}
bool Arguments::has(const std::string &s) const { return values.count(s); }
const std::vector<std::string> &Arguments::all(const std::string &s) const {
    static const std::vector<std::string> empty;
    auto p = values.find(s); return p == values.end() ? empty : p->second;
}
std::string Arguments::get(const std::string &s, const std::string &fallback) const {
    auto &v = all(s); return v.empty() ? fallback : v.back();
}
std::string Arguments::required(const std::string &s) const {
    auto value = get(s); if (value.empty()) throw std::runtime_error("required option " + s); return value;
}
} // namespace veval::verification
