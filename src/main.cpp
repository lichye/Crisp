#include "verify.hpp"
#include "util/database/LoweringDatabase.h"
#include "util/process/RunExternalCommand.h"
#include "util/task/CommandArgumentsAndFiles.h"
#include <charconv>
#include <iostream>
#include <regex>

using namespace crisp;
using namespace veval::verification;

static bool enabled(const Arguments& args, const std::string& flag) {
    const auto value = args.get(flag, "on");
    if (value == "on") return true;
    if (value == "off") return false;
    throw std::runtime_error(flag + " expects on or off");
}

static long long positive(const Arguments& args, const std::string& flag, const std::string& fallback) {
    const auto value = args.get(flag, fallback);
    long long number = 0;
    const auto end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, number);
    if (parsed.ec != std::errc{} || parsed.ptr != end || number <= 0)
        throw std::runtime_error(flag + " expects a positive integer");
    return number;
}

static Options parseOptions(int argc, char** argv) {
    Arguments args(argc, argv, {}, {"--top","--spec","--split","--lemmas","--budget","--depth"});
    Options options;
    options.top = args.required("--top");
    if (!std::regex_match(options.top, std::regex("[A-Za-z_][A-Za-z_0-9$]*")))
        throw std::runtime_error("--top needs a simple Verilog module identifier");
    options.spec = args.required("--spec");
    if (args.positional.empty()) throw std::runtime_error("supply at least one RTL input");
    for (const auto& input : args.positional) {
        if (!fs::is_regular_file(input)) throw std::runtime_error("missing RTL input: " + input);
        options.design.emplace_back(input);
    }
    if (!fs::is_regular_file(options.spec)) throw std::runtime_error("missing specification: " + options.spec.string());
    options.split = enabled(args,"--split");
    options.lemmas = enabled(args,"--lemmas");
    const auto depth = positive(args,"--depth","5");
    if (depth > 1000000) throw std::runtime_error("--depth exceeds 1000000");
    options.depth = static_cast<int>(depth);
    options.seconds = positive(args,"--budget","60");
    return options;
}

static void showLowering(const fs::path& directory) {
    Database db(directory / "lowering.sqlite", false, true);
    for (const auto& row : db.rows("SELECT source_entry,instance,body FROM check_templates "
                                   "ORDER BY source_entry,instance,job_id")) {
        const auto job = parse(text(row["body"]));
        std::cout << text(row["source_entry"]) << " @ " << text(row["instance"]) << ": "
                  << text(job["target"]["sentence"]) << '\n';
        for (const auto& premise : items(job["premises"]))
            std::cout << "Premise " << text(premise["source_entry"]) << '\n';
    }
}

static void showChecks(const fs::path& directory) {
    const auto report = parse(read(directory / "check.json"));
    for (const auto& prop : items(report["properties"])) {
        for (const auto& problem : items(prop["problems"])) {
            const auto path = text(problem["result_path"]);
            const auto result = path.empty() ? J{} : parse(read(path));
            std::cout << text(prop["entry"]) << " @ " << text(problem["instance"]) << ": "
                      << text(result["verdict"], text(problem["status"])) << '\n';
            if (text(result["verdict"]) == "refuted") {
                const auto& traces = items(result["trace"]["raw"]);
                if (!traces.empty()) std::cout << "Counterexample (VCD)\n" << read(text(*traces.begin())) << '\n';
            }
        }
    }
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "show-lowering") { showLowering(argv[2]); return 0; }
        if (argc == 3 && std::string(argv[1]) == "show-checks") { showChecks(argv[2]); return 0; }
        if (argc < 4 || std::string(argv[1]) != "verify") throw std::runtime_error("run ./crisp --help");
        const fs::path root = argv[2], scratch = argv[3];
        const Tools tools{root / "components/veval/build/veval", root / "components/smartVerilog/build/smart"};
        veval::util::command_runner::installCancellationHandlers();
        auto options = parseOptions(argc - 3, argv + 3);
        return verify(options, tools, scratch, std::cout);
    } catch (const std::exception& error) {
        std::cerr << "crisp: " << error.what() << '\n';
        return 2;
    }
}
