// Settings for property generation, parsed from the supported TOML subset.
// Explicit file values override CLI defaults; omitted values remain unset so
// the run can report their origin. Historical [engine] fields are metadata for
// compatibility. Checking methods are selected by the .sby/check workflow.

#ifndef VEVAL_CONFIG_H
#define VEVAL_CONFIG_H

#include <iosfwd>
#include <optional>
#include <string>

namespace veval {

// Only what the file MENTIONED. Absent stays absent all the way through, so
// the effective record can say which values a file decided and which ones
// fell through to the flag or to the tool's own default.
struct GenerationConfig {
    std::optional<bool> symEval;
    std::optional<unsigned long> symEvalConeCap;
    std::optional<double> symEvalTimeoutSeconds;
    std::optional<std::string> environmentProfile;
    // single-path (default), split (all landings), or root (the whole DUT).
    std::optional<std::string> placement;

    // Parsed, validated, recorded -- and not used. The engine adapter is a
    // later spec; these exist so a settings file written today does not have
    // to be rewritten when it lands, and so that a file naming an engine is
    // rejected now rather than half-obeyed.
    std::optional<std::string> engineName;
    std::optional<double> engineTimeoutSeconds;
    std::optional<double> engineMemoryGb;
    std::optional<std::string> engineLadder;

    // Where it was read from, for the effective record.
    std::string path;
};

// Throws std::runtime_error naming the file and the line for anything the
// subset does not admit.
GenerationConfig readGenerationConfig(const std::string& path);

}  // namespace veval

#endif  // VEVAL_CONFIG_H
