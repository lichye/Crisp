#include "front/front.hpp"
#include "check/run.hpp"

#include <iostream>

namespace {
using namespace crisp::front;

constexpr char usage[] =
    "Usage: crisp FILE.salt [TASK...]\n"
    "       crisp --help\n"
    "       crisp --version\n";

void print_task(const Task& task) {
    std::cout << (task.name ? task.name->value : "(default task)") << '\n';
    std::cout << "  files";
    for (const auto& file : task.files) std::cout << "  " << file.value.string();
    std::cout << '\n';
    if (task.top) std::cout << "  top      " << task.top->value << '\n';
    if (task.clock) std::cout << "  clock    " << task.clock->value << '\n';
    if (task.reset) {
        const auto& reset = task.reset->value;
        std::cout << "  reset    " << reset.signal << (reset.polarity == Polarity::low ? " low\n" : " high\n");
    }
    for (const auto& dir : task.includes) std::cout << "  include  " << dir.value.string() << '\n';
    for (const auto& define : task.defines) {
        std::cout << "  define   " << define.value.name;
        if (define.value.value) std::cout << ' ' << *define.value.value;
        std::cout << '\n';
    }
    for (const auto& param : task.params)
        std::cout << "  param    " << param.value.name << ' ' << param.value.value << '\n';

    std::cout << "  options  mode " << spelling(task.mode.value) << ", level " << spelling(task.level.value)
              << ", smart " << spelling(task.smart.value) << ", veval " << spelling(task.veval.value)
              << ", undriven " << spelling(task.undriven.value);
    if (task.depth) std::cout << ", depth " << task.depth->value;
    if (!task.properties.empty()) {
        std::cout << "\n  properties";
        for (const auto& pattern : task.properties) std::cout << "  " << pattern.value;
    }
    std::cout << "\n  resource thread " << task.thread.value << ", ram ";
    if (task.ram_mib) std::cout << task.ram_mib->value << " MiB";
    else std::cout << "unlimited";
    std::cout << ", timeout " << task.timeout_seconds.value << "s\n";
}

std::string one_line(std::string_view text) {
    std::string out;
    for (char c : text) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << usage;
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "crisp " << CRISP_VERSION << '\n';
        return 0;
    }
    if (argc < 2) {
        std::cerr << "crisp: expected FILE.salt [TASK...]; see --help\n";
        return 2;
    }
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') {
            std::cerr << "crisp: unexpected option '" << one_line(argv[i]) << "'; see --help\n";
            return 2;
        }
    }
    const auto result = prepare_tasks(argv[1], {argv + 2, argv + argc});
    if (const auto* error = std::get_if<FrontError>(&result)) {
        std::cerr << "crisp: ";
        if (error->line) std::cerr << one_line(error->file.string()) << ':' << *error->line << ": ";
        std::cerr << one_line(error->message) << '\n';
        return 2;
    }
    const auto& tasks = std::get<std::vector<Task>>(result);
    crisp::check::Summary summary;
    for (const auto& task : tasks) {
        print_task(task);
        try {
            crisp::check::run(task, summary, tasks.size() > 1);
        } catch (const std::exception& error) {
            std::cerr << "crisp: " << error.what() << '\n';
            summary.error = true;
        }
    }
    std::cout << tasks.size() << (tasks.size() == 1 ? " task, " : " tasks, ")
              << summary.proved << " proved, " << summary.passed << " passed, "
              << summary.refuted << " refuted, " << summary.unknown << " unknown\n";
    return summary.exit_code();
}
