#include "parse.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <thread>

namespace crisp::front {
namespace {

namespace fs = std::filesystem;
using detail::Diagnostic;

// The kernel prints a sorted list of disjoint ranges, for example 0-3,8,10-11.
std::optional<std::uint64_t> count_cpus(std::string_view list) {
    std::uint64_t total = 0;
    const auto take_number = [&](std::uint64_t& value) {
        const auto parsed = std::from_chars(list.data(), list.data() + list.size(), value);
        if (parsed.ec != std::errc{}) return false;
        list.remove_prefix(static_cast<std::size_t>(parsed.ptr - list.data()));
        return true;
    };
    while (!list.empty()) {
        std::uint64_t first;
        if (!take_number(first)) return {};
        auto last = first;
        if (!list.empty() && list.front() == '-') {
            list.remove_prefix(1);
            if (!take_number(last) || last < first) return {};
        }
        if (last - first >= std::numeric_limits<std::uint64_t>::max() - total) return {};
        total += last - first + 1;
        if (list.empty()) return total;
        if (list.front() != ',') return {};
        list.remove_prefix(1);
    }
    return {};
}

std::uint64_t available_cpus() {
    std::ifstream status("/proc/self/status");
    const std::string key = "Cpus_allowed_list:";
    for (std::string line; std::getline(status, line);) {
        if (line.compare(0, key.size(), key) != 0) continue;
        const auto start = line.find_first_not_of(" \t", key.size());
        if (start != line.npos) {
            if (auto count = count_cpus(std::string_view(line).substr(start))) return *count;
        }
        break;
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

template<class T>
void inherit(Setting<T>& field, const Setting<T>& common) {
    if (!field.source) field = common;
}

template<class T>
void inherit(std::optional<Setting<T>>& field, const std::optional<Setting<T>>& common) {
    if (!field) field = common;
}

template<class T>
void merge_list(std::vector<Setting<T>>& local, const std::vector<Setting<T>>& common) {
    std::vector<Setting<T>> merged;
    merged.reserve(local.size() + common.size());
    std::merge(local.begin(), local.end(), common.begin(), common.end(), std::back_inserter(merged),
               [](const auto& a, const auto& b) { return a.source->line < b.source->line; });
    local = std::move(merged);
}

void merge(Task& task, const Task& common) {
    inherit(task.top, common.top);
    inherit(task.clock, common.clock);
    inherit(task.reset, common.reset);
    inherit(task.mode, common.mode);
    inherit(task.level, common.level);
    inherit(task.smart, common.smart);
    inherit(task.veval, common.veval);
    inherit(task.undriven, common.undriven);
    inherit(task.depth, common.depth);
    inherit(task.thread, common.thread);
    inherit(task.timeout_seconds, common.timeout_seconds);
    inherit(task.ram_mib, common.ram_mib);
    merge_list(task.files, common.files);
    merge_list(task.includes, common.includes);
    merge_list(task.defines, common.defines);
    merge_list(task.params, common.params);
    merge_list(task.properties, common.properties);
}

[[noreturn]] void conflict(const std::optional<SourceLocation>& a, const std::optional<SourceLocation>& b,
                           const std::string& message) {
    // At least one side of every conflict is explicit.
    const std::size_t line = std::max(a ? a->line : 0, b ? b->line : 0);
    throw Diagnostic{line, "[options] " + message};
}

void check_options(Task& task) {
    const auto mode = task.mode.value;
    if ((mode == Mode::bdd || mode == Mode::pdr) && task.level.value == Level::word)
        conflict(task.mode.source, task.level.source, "mode " + std::string(spelling(mode)) +
                 " conflicts with level word; expected level auto or bit");
    if (task.smart.value == ToolUse::only && task.veval.value == ToolUse::only)
        conflict(task.smart.source, task.veval.source, "smart only and veval only conflict; expected at most one tool set to only");
    if (task.smart.value == ToolUse::only || task.veval.value == ToolUse::only) {
        const bool smart_only = task.smart.value == ToolUse::only;
        const auto& only = smart_only ? task.smart.source : task.veval.source;
        const std::string prefix = smart_only ? "smart only" : "veval only";
        if (task.mode.source) conflict(only, task.mode.source, prefix + " requires mode to be omitted");
        if (task.level.source) conflict(only, task.level.source, prefix + " requires level to be omitted");
        if (task.depth) conflict(only, task.depth->source, prefix + " requires depth to be omitted");
        if (!task.properties.empty())
            conflict(only, task.properties.front().source, prefix + " requires properties to be omitted");
    }
    const bool has_depth = mode == Mode::bmc || mode == Mode::kind;
    if (task.depth && !has_depth)
        conflict(task.depth->source, task.mode.source, "depth requires mode bmc or kind");
    if (!task.depth && has_depth) task.depth = Setting<std::uint64_t>{20, {}};
}

// After merging, the same PATTERN may not appear twice (5.1.3).
void check_patterns(const std::vector<Setting<std::string>>& patterns) {
    std::set<std::string> seen;
    for (const auto& pattern : patterns) {
        if (!seen.insert(pattern.value).second)
            throw Diagnostic{pattern.source->line, "[options] properties: duplicate pattern '" + pattern.value + "'"};
    }
}

template<class Named>
void check_names(const std::vector<Setting<Named>>& entries, const char* key) {
    std::set<std::string> seen;
    for (const auto& entry : entries) {
        if (!seen.insert(entry.value.name).second)
            throw Diagnostic{entry.source->line, std::string("[design] duplicate ") + key + " '" + entry.value.name + "'"};
    }
}

void check_paths(Task& task) {
    if (task.files.empty()) {
        const auto name = task.name ? "task '" + task.name->value + "'" : "the default task";
        throw Diagnostic{{}, "[files] no source files for " + name};
    }
    const auto base = task.salt_file.parent_path();
    std::error_code ec;
    for (auto current = task.files.begin(); current != task.files.end(); ++current) {
        auto& path = current->value;
        path = (base / path).lexically_normal();
        if (path.string().find('\0') != std::string::npos || !fs::is_regular_file(path, ec) ||
            !std::ifstream(path, std::ios::binary))
            throw Diagnostic{current->source->line, "[files] expected a readable regular file at '" + path.string() + "'"};
        // equivalent also recognizes hard links. Keep the user's spelling in the task.
        for (auto previous = task.files.begin(); previous != current; ++previous) {
            if (fs::equivalent(previous->value, path, ec))
                throw Diagnostic{current->source->line, "[files] duplicate source file '" + path.string() + "'"};
            if (ec) throw Diagnostic{current->source->line, "[files] cannot compare source file '" + path.string() + "'"};
        }
    }
    for (auto& entry : task.includes) {
        entry.value = (base / entry.value).lexically_normal();
        if (entry.value.string().find('\0') != std::string::npos || !fs::is_directory(entry.value, ec))
            throw Diagnostic{entry.source->line, "[design] include: expected a directory at '" + entry.value.string() + "'"};
    }
}

void check_selection(const std::vector<Task>& named, const std::vector<std::string>& requested) {
    std::set<std::string> available;
    std::string list;
    for (const auto& task : named) {
        const auto& name = task.name->value;
        available.insert(name);
        if (!list.empty()) list += ", ";
        list += name;
    }
    for (const auto& name : requested) {
        if (!available.count(name)) {
            if (list.empty()) list = "(none; the default task is unnamed)";
            throw Diagnostic{{}, "unknown task '" + name + "'; available tasks: " + list};
        }
    }
}

} // namespace

Result prepare_tasks(const fs::path& salt_path, const std::vector<std::string>& requested_names) {
    try {
        const auto input = salt_path.string();
        if (input.size() < 5 || input.compare(input.size() - 5, 5, ".salt") != 0)
            throw Diagnostic{{}, "expected a task file ending in .salt"};
        const auto absolute = fs::absolute(salt_path).lexically_normal();
        auto document = detail::parse(salt_path, absolute);
        check_selection(document.named, requested_names);
        const std::set<std::string> selected(requested_names.begin(), requested_names.end());
        if (document.named.empty()) document.named.emplace_back();

        std::optional<std::uint64_t> default_threads;
        std::vector<Task> tasks;
        for (auto& task : document.named) {
            if (!selected.empty() && !selected.count(task.name->value)) continue;
            task.salt_file = absolute;
            merge(task, document.common);
            check_options(task);
            check_names(task.defines, "define");
            check_names(task.params, "param");
            check_patterns(task.properties);
            check_paths(task);
            if (!task.thread.source) {
                if (!default_threads) default_threads = available_cpus();
                task.thread.value = *default_threads;
            }
            tasks.push_back(std::move(task));
        }
        return tasks;
    } catch (const Diagnostic& error) {
        return FrontError{salt_path, error.line, error.message};
    } catch (const fs::filesystem_error& error) {
        return FrontError{salt_path, {}, "cannot resolve task file '" + salt_path.string() + "': " + error.code().message()};
    }
}

} // namespace crisp::front
