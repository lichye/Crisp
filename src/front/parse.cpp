#include "parse.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace crisp::front {
namespace {

using detail::Diagnostic;
namespace fs = std::filesystem;
using Words = std::vector<std::string>;
constexpr std::string_view mode_names[]{"auto", "bdd", "bmc", "kind", "pdr"};
constexpr std::string_view undriven_names[]{"seq", "const", "error"};
constexpr std::string_view level_names[]{"auto", "word", "bit"};
constexpr std::string_view tool_names[]{"on", "off", "only"};
constexpr std::string_view whitespace = " \t\r\n\f\v";

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(whitespace);
    if (first == text.npos) return {};
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

Words tokens(std::string_view line) {
    Words result;
    while (!line.empty()) {
        const auto end = line.find_first_of(whitespace);
        result.emplace_back(line.substr(0, end));
        if (end == line.npos) break;
        line = trim(line.substr(end));
    }
    return result;
}

bool valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        if (lead < 0x80) continue;
        unsigned int extra, value, minimum;
        if (lead >= 0xc2 && lead <= 0xdf) {
            extra = 1; value = lead & 0x1f; minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            extra = 2; value = lead & 0x0f; minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            extra = 3; value = lead & 7; minimum = 0x10000;
        } else {
            return false;
        }
        if (extra > text.size() - i) return false;
        for (unsigned int n = 0; n < extra; ++n) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}

bool identifier(std::string_view s, bool allow_dollar = false) {
    const auto initial = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    return !s.empty() && initial(s.front()) && std::all_of(s.begin() + 1, s.end(), [&](char c) {
        return initial(c) || (c >= '0' && c <= '9') || (allow_dollar && c == '$');
    });
}

struct Line {
    std::string section;
    std::string text;
    std::size_t number;
};

// All diagnostics here acquire their display filename at the public API boundary.
struct SettingLine {
    const Line& row;
    const Words& words;
    SourceLocation source;

    [[noreturn]] void fail(const std::string& message) const {
        throw Diagnostic{row.number, "[" + row.section + "] " + words.front() + ": " + message};
    }
    void expect(std::size_t count, const char* syntax) const {
        if (words.size() != count) fail(std::string("expected ") + syntax);
    }
    template<class T>
    void set(Setting<T>& field, T value) const {
        if (field.source) fail("duplicate setting in the same scope");
        field = {std::move(value), source};
    }
    template<class T>
    void set(std::optional<Setting<T>>& field, T value) const {
        if (field) fail("duplicate setting in the same scope");
        field = Setting<T>{std::move(value), source};
    }
};

template<class Enum, std::size_t Count>
Enum choice(const SettingLine& line, const std::string_view (&names)[Count]) {
    line.expect(2, "one value");
    const auto found = std::find(std::begin(names), std::end(names), line.words[1]);
    if (found != std::end(names)) return static_cast<Enum>(found - names);
    std::string allowed;
    for (auto name : names) {
        if (!allowed.empty()) allowed += ", ";
        allowed += name;
    }
    line.fail("unknown value '" + line.words[1] + "'; expected " + allowed);
}

std::uint64_t quantity(const SettingLine& line) {
    line.expect(2, "one positive decimal integer with the unit, if any, attached");
    const auto& key = line.words[0];
    std::string_view number = line.words[1];
    std::uint64_t factor = 1;
    const char* units = "";
    if (key == "ram") {
        units = " (M, G or no unit for G)";
        factor = 1024;
        if (number.back() == 'M' || number.back() == 'G') {
            if (number.back() == 'M') factor = 1;
            number.remove_suffix(1);
        }
    } else if (key == "timeout") {
        units = " (s, m, h or no unit for seconds)";
        const char unit = number.back();
        if (unit == 's' || unit == 'm' || unit == 'h') {
            if (unit == 'm') factor = 60;
            if (unit == 'h') factor = 3600;
            number.remove_suffix(1);
        }
    }
    std::uint64_t value = 0;
    const auto end = number.data() + number.size();
    const auto parsed = std::from_chars(number.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0 ||
        value > std::numeric_limits<std::uint64_t>::max() / factor)
        line.fail("invalid value '" + line.words[1] + "'; expected a positive decimal integer" +
                  units + ", at most 18446744073709551615 after conversion");
    return value * factor;
}

void read_design(const SettingLine& line, Task& scope) {
    const auto& w = line.words;
    const auto& key = w[0];
    if (key == "include") {
        line.expect(2, "include DIR");
        scope.includes.push_back({w[1], line.source});
        return;
    }
    if (key == "reset") line.expect(3, "reset NAME low|high");
    else if (key == "param") line.expect(3, "param NAME VALUE");
    else if (key == "define") {
        if (w.size() != 2 && w.size() != 3) line.fail("expected define NAME [VALUE]");
    } else if (key == "top" || key == "clock") line.expect(2, "one signal or module name");
    else line.fail("unknown key; expected top, clock, reset, include, define, param");

    if (!identifier(w[1], true)) line.fail("invalid name '" + w[1] + "'; expected [A-Za-z_][A-Za-z0-9_$]*");
    if (key == "top") line.set(scope.top, w[1]);
    else if (key == "clock") line.set(scope.clock, w[1]);
    else if (key == "reset") {
        if (w[2] != "low" && w[2] != "high") line.fail("invalid polarity '" + w[2] + "'; expected low or high");
        line.set(scope.reset, Reset{w[1], w[2] == "low" ? Polarity::low : Polarity::high});
    } else if (key == "define") {
        Define define{w[1], {}};
        if (w.size() == 3) define.value = w[2];
        scope.defines.push_back({std::move(define), line.source});
    } else {
        scope.params.push_back({{w[1], w[2]}, line.source});
    }
}

void read_setting(const SettingLine& line, Task& scope) {
    const auto& key = line.words[0];
    if (line.row.section == "files") {
        line.expect(1, "one source file path without whitespace");
        const fs::path path(key);
        const auto ext = path.extension();
        if (ext != ".v" && ext != ".sv" && ext != ".vh" && ext != ".svh")
            line.fail("expected a source file with extension .v, .sv, .vh or .svh");
        scope.files.push_back({path, line.source});
    } else if (line.row.section == "resource") {
        if (key == "thread") line.set(scope.thread, quantity(line));
        else if (key == "ram") line.set(scope.ram_mib, quantity(line));
        else if (key == "timeout") line.set(scope.timeout_seconds, quantity(line));
        else line.fail("unknown key; expected thread, ram, timeout");
    } else if (line.row.section == "options") {
        if (key == "mode") line.set(scope.mode, choice<Mode>(line, mode_names));
        else if (key == "level") line.set(scope.level, choice<Level>(line, level_names));
        else if (key == "smart") line.set(scope.smart, choice<ToolUse>(line, tool_names));
        else if (key == "veval") line.set(scope.veval, choice<ToolUse>(line, tool_names));
        else if (key == "depth") line.set(scope.depth, quantity(line));
        else if (key == "undriven") line.set(scope.undriven, choice<Undriven>(line, undriven_names));
        else if (key == "properties") {
            line.expect(2, "one PATTERN without whitespace");
            scope.properties.push_back({line.words[1], line.source});
        }
        else line.fail("unknown key; expected mode, level, smart, veval, depth, undriven, properties");
    } else {
        read_design(line, scope);
    }
}

} // namespace

std::string_view spelling(Mode value) { return mode_names[static_cast<std::size_t>(value)]; }
std::string_view spelling(Level value) { return level_names[static_cast<std::size_t>(value)]; }
std::string_view spelling(ToolUse value) { return tool_names[static_cast<std::size_t>(value)]; }
std::string_view spelling(Undriven value) { return undriven_names[static_cast<std::size_t>(value)]; }

detail::Document detail::parse(const fs::path& input, const fs::path& absolute) {
    std::ifstream stream(input, std::ios::binary);
    if (!stream) throw Diagnostic{{}, "cannot open task file '" + input.string() + "'"};
    Document document;
    std::map<std::string, std::size_t> task_index, headers;
    std::vector<Line> settings;
    std::string section, buffer;
    std::size_t number = 0;
    while (std::getline(stream, buffer)) {
        ++number;
        if (!valid_utf8(buffer)) throw Diagnostic{number, "expected UTF-8 text"};
        const auto text = trim(buffer);
        if (text.empty() || text.front() == '#') continue;
        if (text.front() == '[' && text.back() == ']') {
            section = text.substr(1, text.size() - 2);
            if (section != "tasks" && section != "resource" && section != "options" &&
                section != "files" && section != "design")
                throw Diagnostic{number, "unknown section [" + section + "]; expected tasks, resource, options, files, design"};
            if (!headers.emplace(section, number).second)
                throw Diagnostic{number, "duplicate [" + section + "] section"};
        } else if (section.empty()) {
            throw Diagnostic{number, "expected a section header before the first setting"};
        } else if (section == "tasks") {
            if (!identifier(text)) throw Diagnostic{number, "[tasks] expected one task name matching [A-Za-z_][A-Za-z0-9_]*"};
            std::string name(text);
            if (!task_index.emplace(name, document.named.size()).second)
                throw Diagnostic{number, "[tasks] duplicate task '" + name + "'"};
            Task task;
            task.name = Setting<std::string>{std::move(name), SourceLocation{absolute, number}};
            document.named.push_back(std::move(task));
        } else {
            settings.push_back({section, std::string(text), number});
        }
    }
    if (stream.bad()) throw Diagnostic{{}, "cannot read task file '" + input.string() + "'"};
    if (headers.count("tasks") && document.named.empty())
        throw Diagnostic{headers.at("tasks"), "[tasks] expected at least one task name"};

    // Task names may be declared below their uses. Resolve selectors only once
    // all declarations are known, retaining the source order of settings.
    Task* block = nullptr;
    section.clear();
    for (const auto& row : settings) {
        if (section != row.section) {
            section = row.section;
            block = nullptr;
        }
        std::string_view text = row.text;
        if (text == "--") {
            if (!block) throw Diagnostic{row.number, "[" + section + "] -- outside a task block"};
            block = nullptr;
            continue;
        }
        Task* scope = block ? block : &document.common;
        const auto colon = text.find(':');
        if (colon != text.npos) {
            const auto prefix = text.substr(0, colon);
            if (!prefix.empty() && prefix.front() == '~' && identifier(prefix.substr(1)))
                throw Diagnostic{row.number, "[" + section + "] negated task selector '" + std::string(prefix) + "' is invalid"};
            if (identifier(prefix)) {
                const auto task = task_index.find(std::string(prefix));
                if (task == task_index.end())
                    throw Diagnostic{row.number, "[" + section + "] undeclared task '" + std::string(prefix) + "'"};
                scope = &document.named[task->second];
                text = trim(text.substr(colon + 1));
                block = nullptr;
                if (text.empty()) {
                    block = scope;
                    continue;
                }
            }
        }
        const auto words = tokens(text);
        read_setting({row, words, {absolute, row.number}}, *scope);
    }
    return document;
}

} // namespace crisp::front
