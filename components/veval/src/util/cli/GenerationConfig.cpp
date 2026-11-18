#include "GenerationConfig.h"

#include <cmath>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace veval {

namespace {

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        begin++;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        end--;
    return text.substr(begin, end - begin);
}

// A comment runs to the end of the line, except inside a quoted value: a
// ladder like "prove:5,prove:10" has no `#' in it today and a path might.
std::string dropComment(const std::string& line) {
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); i++) {
        if (line[i] == '"') quoted = !quoted;
        if (line[i] == '#' && !quoted) return line.substr(0, i);
    }
    return line;
}

[[noreturn]] void fail(const std::string& path, std::size_t line,
                       const std::string& what) {
    std::ostringstream message;
    message << path << ':' << line << ": " << what;
    throw std::runtime_error(message.str());
}

bool asBool(const std::string& path, std::size_t line, const std::string& key,
            const std::string& value) {
    if (value == "true") return true;
    if (value == "false") return false;
    fail(path, line, key + " takes true or false, not `" + value + "'");
}

std::string asString(const std::string& path, std::size_t line,
                     const std::string& key, const std::string& value) {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        fail(path, line, key + " takes a quoted string, not `" + value + "'");
    const auto body = value.substr(1, value.size() - 2);
    if (body.find('"') != std::string::npos)
        fail(path, line, key + ": no escapes in this subset");
    return body;
}

double asNumber(const std::string& path, std::size_t line,
                const std::string& key, const std::string& value) {
    try {
        std::size_t used = 0;
        const double number = std::stod(value, &used);
        if (used != value.size()) throw std::invalid_argument("trailing");
        // nan and inf parse, and a nan reached effective_config.json as
        // invalid JSON; hex (0x10) is not TOML's decimal either.
        if (!std::isfinite(number) || value.find_first_of("xX") != std::string::npos)
            throw std::invalid_argument("not a decimal number");
        return number;
    } catch (const std::exception&) {
        fail(path, line, key + " takes a number, not `" + value + "'");
    }
}

double asNonNegative(const std::string& path, std::size_t line,
                     const std::string& key, const std::string& value) {
    const double number = asNumber(path, line, key, value);
    if (number < 0) fail(path, line, key + " cannot be negative");
    return number;
}

unsigned long asCount(const std::string& path, std::size_t line,
                      const std::string& key, const std::string& value) {
    const double number = asNonNegative(path, line, key, value);
    if (number != static_cast<double>(static_cast<unsigned long>(number)))
        fail(path, line, key + " takes a whole number");
    return static_cast<unsigned long>(number);
}

}  // namespace

GenerationConfig readGenerationConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot read --config " + path);

    GenerationConfig config;
    config.path = path;

    std::string table;
    std::string text;
    std::size_t number = 0;

    while (std::getline(file, text)) {
        number++;
        const auto line = trim(dropComment(text));
        if (line.empty()) continue;

        if (line.front() == '[') {
            if (line.back() != ']')
                fail(path, number, "unterminated table header");
            table = trim(line.substr(1, line.size() - 2));
            if (table != "engine")
                fail(path, number, "unknown table [" + table + "]; the only "
                                   "table is [engine]");
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos)
            fail(path, number, "expected `key = value'");
        const auto key = trim(line.substr(0, equals));
        const auto value = trim(line.substr(equals + 1));
        if (key.empty()) fail(path, number, "empty key");
        if (value.empty()) fail(path, number, key + " has no value");

        // Duplicates are an error for the same reason unknown keys are: two
        // lines setting one valve means the reader cannot tell from the file
        // which one the run used.
        auto once = [&](bool alreadySet) {
            if (alreadySet) fail(path, number, key + " is set twice");
        };

        if (table.empty()) {
            if (key == "infer_mappings" || key == "symeval") {
                once(config.symEval.has_value());
                config.symEval = asBool(path, number, key, value);
            } else if (key == "mapping_cone_cap" || key == "symeval_cone_cap") {
                once(config.symEvalConeCap.has_value());
                config.symEvalConeCap = asCount(path, number, key, value);
            } else if (key == "mapping_timeout" || key == "symeval_timeout") {
                once(config.symEvalTimeoutSeconds.has_value());
                config.symEvalTimeoutSeconds =
                    asNonNegative(path, number, key, value);
            } else if (key == "environment_profile") {
                once(config.environmentProfile.has_value());
                const auto profile = asString(path, number, key, value);
                if (profile != "full" && profile != "isolation" &&
                    profile != "allowlist")
                    fail(path, number,
                         "environment_profile must be full, isolation, or "
                         "allowlist, not `" + profile + "'");
                config.environmentProfile = profile;
            } else if (key == "placement") {
                once(config.placement.has_value());
                const auto placement = asString(path, number, key, value);
                if (placement != "single-path" && placement != "split")
                    fail(path, number,
                         "placement must be single-path or split, not `" +
                             placement + "'");
                config.placement = placement;
            } else {
                fail(path, number, "unknown key `" + key + "'");
            }
            continue;
        }

        if (key == "name") {
            once(config.engineName.has_value());
            config.engineName = asString(path, number, "engine.name", value);
        } else if (key == "timeout") {
            once(config.engineTimeoutSeconds.has_value());
            config.engineTimeoutSeconds =
                asNonNegative(path, number, "engine.timeout", value);
        } else if (key == "memory") {
            once(config.engineMemoryGb.has_value());
            config.engineMemoryGb =
                asNonNegative(path, number, "engine.memory", value);
        } else if (key == "ladder") {
            once(config.engineLadder.has_value());
            config.engineLadder = asString(path, number, "engine.ladder", value);
        } else {
            fail(path, number, "unknown key `engine." + key + "'");
        }
    }

    return config;
}

}  // namespace veval
