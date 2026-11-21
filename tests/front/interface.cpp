#include "front/front.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace crisp::front;
namespace fs = std::filesystem;

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

void write(const fs::path& file, const std::string& contents) {
    std::ofstream out(file, std::ios::binary);
    out << contents;
    out.close();
    require(bool(out), "could not write test input " + file.string());
}

std::vector<Task> ready(const fs::path& file, const std::vector<std::string>& names = {}) {
    auto result = prepare_tasks(file, names);
    if (const auto* error = std::get_if<FrontError>(&result))
        throw std::runtime_error(file.string() + ": " + error->message);
    return std::get<std::vector<Task>>(std::move(result));
}

void error_at(const fs::path& file, const std::vector<std::string>& names,
              std::optional<std::size_t> line, const std::string& mention) {
    auto result = prepare_tasks(file, names);
    const auto* error = std::get_if<FrontError>(&result);
    require(error && error->file == file && error->line == line &&
            error->message.find(mention) != std::string::npos, "wrong diagnostic for " + file.string());
}

template<class T>
void origin(const Setting<T>& field, const fs::path& salt, std::size_t line) {
    require(field.source && field.source->file == salt && field.source->line == line,
            "wrong source, expected " + salt.string() + ":" + std::to_string(line));
}

struct FifoLines {
    std::size_t first_name, second_name, thread, ram, timeout, mode, smart, veval;
    std::size_t first_file, second_file, top, clock, reset;
};

// 9.1, 9.4, 9.12 use the same complete task-data oracle.
void check_fifo(const fs::path& file, std::uint64_t cpus, const FifoLines& ln) {
    const auto tasks = ready(file);
    require(tasks.size() == 2, "fifo task count");
    const auto salt = fs::absolute(file).lexically_normal();
    for (const auto& task : tasks) {
        require(task.salt_file == salt && task.files.size() == 2, "fifo paths");
        require(task.files[0].value == salt.parent_path() / "rtl/fifo.sv", "first source file");
        require(task.files[1].value == salt.parent_path() / "props/fifo_props.sv", "second source file");
        origin(task.files[0], salt, ln.first_file);
        origin(task.files[1], salt, ln.second_file);
        require(task.top.value().value == "fifo" && task.clock.value().value == "clk", "fifo design");
        require(task.reset.value().value.signal == "rst_n" &&
                task.reset->value.polarity == Polarity::low, "fifo reset");
        origin(*task.top, salt, ln.top);
        origin(*task.clock, salt, ln.clock);
        origin(*task.reset, salt, ln.reset);
        require(task.includes.empty() && task.defines.empty() && task.params.empty(), "fifo design lists");
        require(task.level.value == Level::automatic && !task.level.source && !task.depth, "fifo level/depth");
        require(task.undriven.value == Undriven::seq && !task.undriven.source, "default undriven policy");
    }
    const auto& first = tasks[0];
    require(first.name.value().value == "task1", "first task name");
    origin(*first.name, salt, ln.first_name);
    require(first.mode.value == Mode::bdd && first.smart.value == ToolUse::on &&
            first.veval.value == ToolUse::off, "task1 options");
    origin(first.mode, salt, ln.mode);
    origin(first.smart, salt, ln.smart);
    origin(first.veval, salt, ln.veval);
    require(first.thread.value == 16 && first.ram_mib.value().value == 32768 &&
            first.timeout_seconds.value == 600, "task1 resources");
    origin(first.thread, salt, ln.thread);
    origin(*first.ram_mib, salt, ln.ram);
    origin(first.timeout_seconds, salt, ln.timeout);

    const auto& second = tasks[1];
    require(second.name.value().value == "task2", "second task name");
    origin(*second.name, salt, ln.second_name);
    require(second.mode.value == Mode::automatic && !second.mode.source &&
            second.smart.value == ToolUse::on && !second.smart.source &&
            second.veval.value == ToolUse::on && !second.veval.source, "task2 default options");
    require(second.thread.value == cpus && !second.thread.source && !second.ram_mib &&
            second.timeout_seconds.value == 60 && !second.timeout_seconds.source, "task2 default resources");
}

void positive_cases(const fs::path& dir, std::uint64_t cpus) {
    const auto fifo = dir / "fifo.salt";
    const FifoLines block{5, 6, 10, 11, 12, 16, 17, 18, 21, 22, 25, 26, 27};
    check_fifo(fifo, cpus, block);
    const auto selected = ready(fifo, {"task2"});
    require(selected.size() == 1 && selected.front().name.value().value == "task2", "9.2 selection");

    const std::string tasks = "[tasks]\ntask1\ntask2\n";
    const std::string files = "[files]\nrtl/fifo.sv\n";
    const auto input = dir / "settings.salt";
    write(input, tasks + "[resource]\ntask1:thread 16\ntask1: ram 32\ntask1:timeout 600\n"
          "[options]\ntask1:mode bdd\ntask1: smart on\ntask1:veval off\n" + files +
          "props/fifo_props.sv\n[design]\ntop fifo\nclock clk\nreset rst_n low\n");
    check_fifo(input, cpus, {2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 16, 17, 18});

    write(input, tasks + "[resource]\ntask1:\nthread 4\n--\ntimeout 30\n" + files);
    auto pair = ready(input);
    require(pair.size() == 2 && pair[0].thread.value == 4 && pair[1].thread.value == cpus &&
            pair[0].timeout_seconds.value == 30 && pair[1].timeout_seconds.value == 30, "9.5 block terminator");
    write(input, tasks + "[resource]\ntask1:\nthread 4\n" + files);
    pair = ready(input);
    require(pair.size() == 2 && pair[0].files.size() == 1 && pair[1].files.size() == 1 &&
            pair[0].files[0].value == pair[1].files[0].value, "9.5 section ends block");

    for (const auto* settings : {"timeout 100\ntask1: timeout 5\n", "task1: timeout 5\ntimeout 100\n"}) {
        write(input, tasks + "[resource]\n" + settings + files);
        pair = ready(input);
        require(pair.size() == 2 && pair[0].timeout_seconds.value == 5 &&
                pair[1].timeout_seconds.value == 100, "9.6 scope priority");
    }
    const auto salt = fs::absolute(input).lexically_normal();
    const auto check_undriven = [&](const std::string& settings, std::size_t task_line, std::size_t common_line) {
        write(input, tasks + "[options]\n" + settings + files);
        const auto pair = ready(input);
        require(pair.size() == 2 && pair[0].undriven.value == Undriven::error &&
                pair[1].undriven.value == Undriven::constant, "task overrides the common undriven policy");
        origin(pair[0].undriven, salt, task_line);
        origin(pair[1].undriven, salt, common_line);
    };
    check_undriven("undriven const\ntask1: undriven error\n", 6, 5);
    check_undriven("task1: undriven error\nundriven const\n", 5, 6);
    check_undriven("task1:\nundriven error\n--\nundriven const\n", 6, 8);

    // properties PATTERN: the entries for all tasks and for one task merge in file order (5.1.3).
    const auto check_properties = [&](const std::string& settings, const std::vector<std::string>& first,
                                      const std::vector<std::size_t>& first_lines, std::size_t common_line) {
        write(input, tasks + "[options]\n" + settings + files);
        const auto pair = ready(input);
        require(pair.size() == 2 && pair[0].properties.size() == first.size() &&
                pair[1].properties.size() == 1 && pair[1].properties[0].value == "top.*", "properties merge");
        for (std::size_t i = 0; i < first.size(); ++i) {
            require(pair[0].properties[i].value == first[i], "properties merge order");
            origin(pair[0].properties[i], salt, first_lines[i]);
        }
        origin(pair[1].properties[0], salt, common_line);
    };
    check_properties("properties top.*\ntask1: properties top.u_dut.**\n", {"top.*", "top.u_dut.**"}, {5, 6}, 5);
    check_properties("task1: properties top.u_dut.**\nproperties top.*\n", {"top.u_dut.**", "top.*"}, {5, 6}, 6);
    write(input, files);
    require(ready(input).at(0).properties.empty(), "no properties selects all assertions");

    write(input, files);
    const auto unnamed = ready(input);
    require(unnamed.size() == 1 && !unnamed[0].name, "9.7 unnamed task");
    write(input, files + "[resource]\nram 512M\ntimeout 10m\n");
    auto units = ready(input).at(0);
    require(units.ram_mib.value().value == 512 && units.timeout_seconds.value == 600, "9.8 MiB/minutes");
    write(input, files + "[resource]\nram 2G\ntimeout 2h\n");
    units = ready(input).at(0);
    require(units.ram_mib.value().value == 2048 && units.timeout_seconds.value == 7200, "9.8 GiB/hours");

    const auto missing = dir / "missing-sources.salt";
    write(missing, tasks + "[files]\ntask1: rtl/fifo.sv\n");
    error_at(missing, {"task2"}, {}, "task2");
    check_fifo(dir / "fifo-crlf.salt", cpus, block);
}

// Each 9.9 input is a separate file shared by the API and CLI tests.
struct InvalidCase {
    const char* filename;
    const char* input;
    std::size_t line;
    const char* mention;
};
const InvalidCase invalid[] = {
    {"01-section", "[engines]\n", 1, "engines"},
    {"02-repeat-section", "[files]\nrtl/fifo.sv\n[files]\n", 3, "files"},
    {"03-key", "[options]\nother on\n", 2, "other"},
    {"04-mode", "[options]\nmode bddd\n", 2, "auto, bdd, bmc, kind, pdr"},
    {"05-level", "[files]\nrtl/fifo.sv\n[options]\nmode bdd\nlevel word\n", 5, "level"},
    {"06-only", "[files]\nrtl/fifo.sv\n[options]\nsmart only\nveval only\n", 5, "only"},
    {"07-depth", "[files]\nrtl/fifo.sv\n[options]\nmode bdd\ndepth 10\n", 5, "depth"},
    {"08-unknown-task", "[tasks]\ntask1\n[options]\ntask2:mode bdd\n", 4, "task2"},
    {"09-negated-task", "[tasks]\ntask1\n[options]\n~task1: mode bdd\n", 4, "~task1"},
    {"10-repeat-key", "[resource]\ntimeout 30\ntimeout 60\n", 3, "timeout"},
    {"11-missing-file", "[files]\nabsent.sv\n", 2, "absent.sv"},
    {"12-extension", "[files]\nexists.txt\n", 2, ".sv"},
    {"13-polarity", "[design]\nreset rst_n\n", 2, "reset"},
    {"14-no-tasks", "[options]\ntask1:\nmode bdd\n", 2, "task1"},
    {"15-separator", "[files]\n--\n", 2, "--"},
    {"16-empty-tasks", "[files]\nrtl/fifo.sv\n[tasks]\n", 3, "tasks"},
    {"17-preamble", "top fifo\n[files]\nrtl/fifo.sv\n", 1, "section"},
    {"18-undriven", "[options]\nundriven unknown\n", 2, "expected seq, const, error"},
    {"19-undriven-duplicate", "[options]\nundriven seq\nundriven const\n", 3, "duplicate setting"},
    {"20-undriven-missing", "[options]\nundriven\n", 2, "expected one value"},
    {"21-undriven-extra", "[options]\nundriven seq const\n", 2, "expected one value"},
    {"22-properties-missing", "[options]\nproperties\n", 2, "properties: expected one PATTERN"},
    {"23-properties-extra", "[options]\nproperties top.* top.**\n", 2, "properties: expected one PATTERN"},
    {"24-properties-duplicate", "[tasks]\ntask1\n[files]\nrtl/fifo.sv\n[options]\nproperties top.*\ntask1: properties top.*\n",
     7, "duplicate pattern 'top.*'"},
    {"25-properties-smart-only", "[files]\nrtl/fifo.sv\n[options]\nproperties top.*\nsmart only\n", 5,
     "smart only requires properties to be omitted"},
    {"26-properties-veval-only", "[files]\nrtl/fifo.sv\n[options]\nveval only\nproperties top.*\n", 5,
     "veval only requires properties to be omitted"},
    {"27-level-word-default-mode", "[files]\nrtl/fifo.sv\n[options]\nlevel word\n", 4,
     "mode auto conflicts with level word"},
    {"28-level-word-mode-auto", "[files]\nrtl/fifo.sv\n[options]\nlevel word\nmode auto\n", 5,
     "mode auto conflicts with level word"}
};

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 4, "expected fixture directory, CLI expectations file and CPU count");
        const fs::path dir = argv[1];
        const std::uint64_t cpus = *argv[3] ? std::stoull(argv[3]) : std::max(1u, std::thread::hardware_concurrency());
        positive_cases(dir, cpus);
        std::ofstream expectations(argv[2]);
        for (const auto& test : invalid) {
            const auto file = dir / (std::string(test.filename) + ".salt");
            write(file, test.input);
            error_at(file, {}, test.line, test.mention);
            expectations << file.string() << '\t' << test.line << '\t' << test.mention << '\n';
        }
        expectations.close();
        require(bool(expectations), "could not write CLI expectations");
        std::cout << "Front API tests passed.\n";
    } catch (const std::exception& error) {
        std::cerr << "Front API test failed: " << error.what() << '\n';
        return 1;
    }
}
