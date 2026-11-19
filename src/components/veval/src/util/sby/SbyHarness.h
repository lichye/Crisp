#pragma once
#include "../task/CommandArgumentsAndFiles.h"
#include <functional>
#include <set>

namespace veval::verification {
struct Unsupported : std::runtime_error { using std::runtime_error::runtime_error; };
size_t balancedEnd(const std::string &, size_t);
std::string stripParens(std::string);
// Drop the trailing block of comment and blank lines from a product prelude.
std::string stripTrailingComments(const std::string &);
struct SbyProduct {
    fs::path path;
    std::string instance, module, prelude;
    std::map<std::string,std::string> functions;
    std::map<std::string, unsigned> ports;
    // Inputs a clock-event product ties to the design's own value: wrapper
    // wires (`wire test_mode = 1'b0;') that its sentences may name.
    std::map<std::string, unsigned> tiedWires;
    std::set<std::string> signedPorts;
    J blocks = list(), premises = list(), observations = list();
    // The `// sampling:' record of a clock-event product (veval transport
    // --multiclock): source net, sampling port, basis and the tied inputs.
    J sampling;
    explicit SbyProduct(const fs::path &);
};
// Read one explicit rising-edge sampling event and validate its declared width.
std::string samplingClock(const SbyProduct&, const std::string& body);
struct Encoding {
    std::vector<std::string> declarations;
    std::set<std::string> generated;
    std::string guard, expr, mode = "safety";
    unsigned delay = 0;
};
using NativeEncoder = std::function<Encoding(const std::string &, const std::string &, size_t)>;
J buildWrapper(const SbyProduct &, const std::set<std::string> &, const NativeEncoder & = {},
               bool multiclock = false);
} // namespace veval::verification
