#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <util/json.h>
#include <vector>

namespace veval::result_validation {
using J = jsont;
namespace fs = std::filesystem;
J str(const std::string &s);
J num(double n);
// Resource budgets use null for infinity, never a non-JSON numeric literal.
J finiteOrNull(double n);
J boolean(bool b);
J object(std::initializer_list<std::pair<const std::string, J>> values = {});
J list(std::initializer_list<J> values = {});
const json_arrayt &items(const J &j);
const json_objectt &members(const J &j);
J &put(J &j, const std::string &key, J value);
void append(J &j, J value);
std::string text(const J &j, const std::string &fallback = "");
std::string canonical(const J &j);
bool equal(const J &a, const J &b);
bool truth(const J &j);
J parse(const std::string &s);
// Resolve explicitly relocated experiment paths without rewriting saved evidence.
std::string relocatedPath(const std::string &s);
J relocatedPaths(J value);
std::string trim(std::string s);
std::string spaces(const std::string &s);
std::vector<std::string> words(const std::string &s);
double monotonic();
void installCancellationHandlers();
bool cancelled();
struct Expired : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Budget {
    double started = monotonic(), deadline = 0;
    explicit Budget(double seconds) : deadline(started + seconds) {}
    void check() const;
    double left() const;
};
class Files {
    std::map<std::string, J> documents;
    std::map<std::string, std::set<std::string>> openedInputCache;

  public:
    Budget *budget = nullptr;
    std::string read(const fs::path &path);
    J json(const fs::path &path);
    J ref(const fs::path &path);
    J identities() const;
    // Accept legacy inline streams and current shared files. Only the small
    // opened-file set is cached, not the expanded hardware text.
    std::set<std::string> openedInputs(const J &stream);
};
// Strict JSON framing, then linear decoding into CBMC's JSON value type.
// Large root arrays are consumed a record at a time, never as a giant DOM.
void streamObject(const fs::path &path, Budget &budget,
                  const std::function<void(const std::string &, J &&)> &scalar,
                  const std::function<void(const std::string &, J &&)> &row);
void rewriteObjectRows(const fs::path &path, const std::function<void(const std::string &, J &)> &row);
void streamArray(const fs::path &path, Budget &budget, const std::function<void(J &&)> &row);
void save(const fs::path &path, const J &j);
} // namespace veval::result_validation
