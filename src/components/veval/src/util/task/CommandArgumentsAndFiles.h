#pragma once
#include "util/database/Json.h"
#include <map>
#include <vector>

namespace veval::verification {
using namespace veval::result_validation;
std::string read(const fs::path &);
void write(const fs::path &, const std::string &);
std::string digest(const std::string &);
std::string fileDigest(const fs::path &);
std::string identity(J);
std::string absolute(const std::string &);
J real(double);
struct Arguments {
    std::map<std::string, std::vector<std::string>> values;
    std::vector<std::string> positional;
    Arguments(int argc, char **argv, const std::vector<std::string> &flags,
              const std::vector<std::string> &options);
    bool has(const std::string &) const;
    std::string get(const std::string &, const std::string &fallback = "") const;
    const std::vector<std::string> &all(const std::string &) const;
    std::string required(const std::string &) const;
};
} // namespace veval::verification
