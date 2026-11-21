#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace crisp::front {

struct SourceLocation {
    std::filesystem::path file;
    std::size_t line;
};

template<class T>
struct Setting {
    T value;
    std::optional<SourceLocation> source;
};

enum class Mode { automatic, bdd, bmc, kind, pdr };
enum class Level { automatic, word, bit };
enum class ToolUse { on, off, only };
enum class Undriven { seq, constant, error };
enum class Polarity { low, high };

std::string_view spelling(Mode);
std::string_view spelling(Level);
std::string_view spelling(ToolUse);
std::string_view spelling(Undriven);

struct Reset {
    std::string signal;
    Polarity polarity;
};
struct Define {
    std::string name;
    std::optional<std::string> value;
};
struct Parameter {
    std::string name;
    std::string value;
};

struct Task {
    std::optional<Setting<std::string>> name;
    std::filesystem::path salt_file;
    std::vector<Setting<std::filesystem::path>> files;
    std::optional<Setting<std::string>> top, clock;
    std::optional<Setting<Reset>> reset;
    std::vector<Setting<std::filesystem::path>> includes;
    std::vector<Setting<Define>> defines;
    std::vector<Setting<Parameter>> params;
    Setting<Mode> mode{Mode::automatic, {}};
    Setting<Level> level{Level::automatic, {}};
    Setting<ToolUse> smart{ToolUse::on, {}}, veval{ToolUse::on, {}};
    Setting<Undriven> undriven{Undriven::seq, {}};
    std::optional<Setting<std::uint64_t>> depth;
    // [options] properties PATTERN, merged in file order; empty selects all assertions.
    std::vector<Setting<std::string>> properties;
    Setting<std::uint64_t> thread{0, {}}, timeout_seconds{60, {}};
    std::optional<Setting<std::uint64_t>> ram_mib;
};

struct FrontError {
    std::filesystem::path file;
    std::optional<std::size_t> line;
    std::string message;
};

using Result = std::variant<std::vector<Task>, FrontError>;
Result prepare_tasks(const std::filesystem::path& salt_path,
                     const std::vector<std::string>& requested_names = {});

} // namespace crisp::front
