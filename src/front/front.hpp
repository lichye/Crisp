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
enum class RtlMode { strict, relaxed };
enum class Frontend { native, goto_vv };
enum class Polarity { low, high };

std::string_view spelling(Mode);
std::string_view spelling(Level);
std::string_view spelling(ToolUse);
std::string_view spelling(Undriven);
std::string_view spelling(RtlMode);
std::string_view spelling(Frontend);

struct Reset {
    std::string signal;
    Polarity polarity;
};
struct ClockGateContract {
    std::filesystem::path file;
    bool start_reset;
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
    // SMART and veval run only when written (spec 25, 1.9; spec 1 table).
    Setting<ToolUse> smart{ToolUse::off, {}}, veval{ToolUse::off, {}};
    Setting<Undriven> undriven{Undriven::seq, {}};
    // strict: slang checks the RTL before EBMC and SMART (spec 23).
    Setting<RtlMode> rtl_mode{RtlMode::strict, {}};
    Setting<Frontend> frontend{Frontend::native, {}};
    std::optional<Setting<ClockGateContract>> clock_gate_contract;
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
