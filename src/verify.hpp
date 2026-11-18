#pragma once

#include "util/database/Json.h"
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace crisp {
namespace fs = std::filesystem;
using veval::result_validation::J;

struct Options {
    std::vector<fs::path> design;
    fs::path spec;
    std::string top;
    bool split = true, lemmas = true;
    int depth = 5;
    double seconds = 60;
};

struct Tools {
    fs::path veval, smart;
};

class Deadline {
public:
    Deadline(double seconds, fs::path supervisor);
    bool expired() const;
    std::string run(const std::vector<std::string>& command, const fs::path& cwd,
                    const fs::path& logs) const;
private:
    double end;
    fs::path supervisor;
};

struct Predicate {
    std::string expression, formula;
    std::set<std::string> wires;
};
using Widths = std::map<std::string, unsigned>;
Widths modelWires(const fs::path& model, const std::string& top);
Predicate bindPredicate(const std::string& expression, const Widths& widths);

struct Solver {
    fs::path model;
    std::string modelHash;
    J tools;
    int depth;
    const Deadline& budget;
    std::string attempt(const fs::path& directory, const std::vector<Predicate>& predicates,
                        bool retry, std::ostream& log) const;
};

struct MiningResult {
    std::string status;
    std::vector<std::string> candidates;
};
using Miner = std::function<MiningResult(const J&, const fs::path&, const fs::path&,
                                         const Deadline&, std::ostream&)>;
MiningResult mine(const J& job, const fs::path& directory, const fs::path& smart,
                  const Deadline& budget, std::ostream& log);
int verify(const Options& options, const Tools& tools, const fs::path& work,
           std::ostream& log, const Miner& miner = mine);
} // namespace crisp
