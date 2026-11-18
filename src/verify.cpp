#include "verify.hpp"
#include "util/database/LoweringDatabase.h"
#include "util/task/CommandArgumentsAndFiles.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

namespace crisp {
using namespace veval::verification;

static std::vector<std::string> lines(const fs::path& file) {
    std::vector<std::string> result;
    std::istringstream input(read(file));
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        result.push_back(line);
    }
    return result;
}

static fs::path prepare(const Options& options, const fs::path& directory, std::ostream& log) {
    fs::create_directory(directory);
    auto sources = options.design;
    if (std::none_of(sources.begin(), sources.end(), [&](const auto& source) {
            return fs::equivalent(source, options.spec);
        })) sources.push_back(options.spec);
    std::set<std::string> names;
    std::ostringstream units, files;
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto& source = sources[i];
        std::string ext = source.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        const bool header = ext == ".vh" || ext == ".svh";
        const std::string name = header ? source.filename().string() : "unit" + std::to_string(i) + ".sv";
        static const std::regex safeName("[A-Za-z0-9_.+-]+");
        if (!names.insert(name).second || !std::regex_match(name, safeName))
            throw std::runtime_error("cannot stage header under its include name: " + source.string());
        fs::copy_file(source, directory / name);
        if (!header) units << ' ' << name;
        files << name << '\n';
        log << "Input " << source.string() << "  sha256 " << fileDigest(directory / name) << '\n';
    }
    const auto task = directory / "verify.sby";
    write(task, "[options]\nmode prove\ndepth " + std::to_string(options.depth) +
          "\n\n[engines]\nsmtbmc boolector\n\n[script]\nread_verilog -formal -sv" + units.str() +
          "\nprep -top " + options.top + "\n\n[files]\n" + files.str());
    return task;
}

MiningResult mine(const J& job, const fs::path& directory, const fs::path& smart,
                  const Deadline& budget, std::ostream& log) {
    if (!fs::is_regular_file(smart)) throw std::runtime_error("missing SMART. Run make smart.");
    const auto rtl = directory / "rtl", work = directory / "mining";
    fs::create_directory(rtl);
    std::vector<std::string> command{smart.string()};
    for (const auto& item : items(job["inputs"])) {
        auto role = text(item["role"]);
        if (role != "design" && role != "harness") continue;
        auto copy = rtl / ("unit" + std::to_string(command.size() - 1) + ".sv");
        fs::copy_file(text(item["path"]), copy);
        if (fileDigest(copy) != text(item["sha256"]))
            throw std::runtime_error("prepared RTL changed since veval elaboration");
        command.push_back(copy.string());
    }
    command.insert(command.end(), {"--top", text(job["target"]["module"]), "--hierarchical", "--msa",
        "--final-unbounded", "--simulator", "iverilog", "-j", "4", "--keep-work",
        "--workdir", work.string(), "--output", (directory / "mined.sv").string()});
    MiningResult result{budget.run(command, directory, directory / "miner-command"), {}};
    if (result.status != "success") {
        const auto output = directory / "miner-command/stdout.log";
        if (fs::is_regular_file(output)) log << read(output);
        return result;
    }
    result.candidates = lines(work / "invariants.txt");
    std::vector<J> checks;
    for (const auto& line : lines(work / "run-log.jsonl")) {
        auto event = parse(line);
        if (text(event["stage"]) == "check") checks.push_back(std::move(event));
    }
    if (!result.candidates.empty() && (checks.size() != 1 || text(checks[0]["mode"]) != "k-induction" ||
        !equal(checks[0]["verified"], num(result.candidates.size()))))
        throw std::runtime_error("SMART output lacks its final k-induction check record");
    return result;
}

struct CheckResult {
    std::string verdict;
    bool error = false;
};

static CheckResult checkModule(const Options& options, const Tools& tools, const Deadline& budget,
                               const fs::path& resultPath, const fs::path& scratch,
                               const std::set<std::string>& assumptions, const Miner& miner,
                               std::ostream& log) {
    const auto result = parse(read(resultPath));
    const auto job = parse(read(resultPath.parent_path() / "request.json"));
    const auto verdict = text(result["outcome"]) == "completed" ? text(result["verdict"]) : text(result["outcome"]);
    const auto& setup = job["setup"];
    std::set<std::string> compiled{text(setup["reset"]["source_entry"])};
    log << "Initial verdict " << verdict << "\nClock model " << text(setup["clock_model"])
        << ", reset " << text(setup["reset"]["kind"]) << '\n';
    for (const auto& premise : items(job["premises"])) {
        const auto source = text(premise["source_entry"]);
        compiled.insert(source);
        log << "Premise " << source << '\n';
    }
    if (!options.split) {
        for (const auto& source : assumptions) {
            if (!compiled.count(source)) {
                log << "Whole-DUT job omitted assumption " << source << '\n';
                return {"unknown"};
            }
        }
    }
    if (verdict == "refuted") {
        const auto& traces = items(result["trace"]["raw"]);
        if (traces.empty() || !fs::is_regular_file(text(*traces.begin())))
            throw std::runtime_error("module refutation lacks its audited base-case waveform");
        log << "Counterexample (VCD)\n" << read(text(*traces.begin())) << '\n';
    }
    if (!options.lemmas || verdict != "unknown") return {verdict, verdict == "error"};

    const fs::path record = text(result["raw"]["record"]);
    const auto cell = fs::is_regular_file(record) ? parse(read(record)) : J{};
    const auto& stages = items(cell["stages"]);
    const auto stage = stages.empty() ? J{} : *std::prev(stages.end());
    const auto& detail = stage["engine_detail"];
    const auto& liveness = cell["wrapper"]["liveness"];
    const auto modelDir = fs::path(text(stage["work_dir"])) / "model";
    if (text(detail["mode"]) != "prove" || !detail["basecase_completed"].is_true() ||
        text(detail["engine"]) != "smtbmc boolector" || text(setup["clock_model"]) != "single-declared-clock" ||
        truth(cell["request"]["multiclock"]) || !liveness.is_array() || !items(liveness).empty() ||
        !fs::is_regular_file(modelDir / "design.il") || !fs::is_regular_file(modelDir / "design_smt2.smt2")) {
        log << "Lemma retry requires a single-clock safety check with a completed base case\n";
        return {"unknown"};
    }
    fs::create_directory(scratch);
    const auto model = scratch / "model.il";
    fs::copy_file(modelDir / "design.il", model);
    const auto widths = modelWires(modelDir / "design_smt2.smt2", text(job["target"]["top"]));
    Solver solver{model, fileDigest(model), result["config"]["effective"]["tool_paths"], options.depth, budget};
    log << "Proof model sha256 " << solver.modelHash << '\n';
    const auto mining = miner(job, scratch, tools.smart, budget, log);
    if (mining.status != "success") {
        log << "Mining " << mining.status << '\n';
        return {"unknown", mining.status == "failed"};
    }
    std::vector<Predicate> proved;
    bool failed = false;
    for (size_t i = 0; i < mining.candidates.size(); ++i) {
        const auto& expression = mining.candidates[i];
        Predicate candidate;
        try {
            candidate = bindPredicate(expression, widths);
        } catch (const std::invalid_argument& error) {
            log << "Lemma " << expression << ": unsupported (" << error.what() << ")\n";
            continue;
        }
        auto status = solver.attempt(scratch / ("lemma-" + std::to_string(i + 1)), {candidate}, false, log);
        log << "Lemma " << expression << ": " << status << '\n';
        if (status == "proved") proved.push_back(std::move(candidate));
        if (status == "error") failed = true;
    }
    if (proved.empty()) return {"unknown", failed};
    log << "Retry with " << proved.size() << " independently proved lemma(s)\n";
    auto retry = solver.attempt(scratch / "retry", proved, true, log);
    if (retry == "refuted") {
        log << "Retry contradicts the completed base case at the same depth\n";
        return {"error", true};
    }
    return {retry, retry == "error"};
}

int verify(const Options& options, const Tools& tools, const fs::path& work,
           std::ostream& log, const Miner& miner) {
    const double start = monotonic();
    Deadline budget(options.seconds, tools.veval.parent_path() / "consflow_process");
    const auto native = work / "veval";
    log << "Top " << options.top << "  split " << (options.split ? "on" : "off")
        << "  lemmas " << (options.lemmas ? "on" : "off") << "  depth " << options.depth
        << "  budget " << options.seconds << " s\n";
    const auto task = prepare(options, work / "input", log);
    const auto status = budget.run({tools.veval.string(), "check", task.string(), "--out", native.string(),
        "--lowering", (work / "lowering").string(), "--strategy", options.split ? "auto" : "top",
        "--placement", options.split ? "split" : "root", "--keep-work"}, work, work / "veval-command");
    const auto commandLog = work / "veval-command/stdout.log";
    if (status != "success" && fs::is_regular_file(commandLog)) log << read(commandLog);
    if (!fs::is_regular_file(native / "check.json") || !fs::is_regular_file(native / "check.sqlite")) {
        log << "Verification " << (status == "timeout" ? "timed out" : "failed") << " before producing checks\n";
        return status == "timeout" ? 3 : 2;
    }
    const auto report = parse(read(native / "check.json"));
    std::set<std::string> assumptions;
    struct Coverage { unsigned selected = 0, proved = 0; };
    std::map<std::string, Coverage> properties;
    for (const auto& row : items(report["properties"])) {
        const auto name = text(row["entry"]);
        if (text(row["kind"]) == "assert") properties.try_emplace(name);
        if (text(row["kind"]) == "assume") assumptions.insert(name);
    }
    Database db(native / "check.sqlite", false, true);
    std::map<std::string, std::string> origins;
    for (const auto& row : db.rows("SELECT DISTINCT derived_entry,source_property FROM symeval_records "
                                   "WHERE derived_entry IS NOT NULL AND derived_entry != ''"))
        origins[text(row["derived_entry"])] = text(row["source_property"]);
    bool refuted = false, error = false;
    unsigned index = 0;
    for (const auto& row : db.rows("SELECT source_entry,instance,module,status,result_path FROM problems "
                                   "WHERE status != 'not-selected' ORDER BY source_entry,instance,job_id")) {
        ++index;
        const auto source = text(row["source_entry"]), instance = text(row["instance"]);
        auto& coverage = properties.at(origins.count(source) ? origins.at(source) : source);
        const auto path = text(row["result_path"]);
        log << "\nChecking " << source << " @ " << instance << '\n';
        CheckResult check{"unknown"};
        if (path.empty()) log << "Selection " << text(row["status"]) << '\n';
        else check = checkModule(options, tools, budget, path, work / ("check-" + std::to_string(index)),
                                 assumptions, miner, log);
        ++coverage.selected;
        if (check.verdict == "proved") ++coverage.proved;
        if (check.verdict == "refuted") refuted = true;
        if (check.error) error = true;
        log << source << " @ " << instance << ": " << check.verdict << '\n';
    }
    size_t covered = 0;
    for (const auto& [source, coverage] : properties) {
        if (!coverage.selected) log << source << ": no selected module check\n";
        else if (coverage.proved == coverage.selected) ++covered;
    }
    log << "\nSources with all selected module checks proved: " << covered << '/' << properties.size()
        << "\nElapsed: " << std::fixed << std::setprecision(3) << monotonic() - start << " s\n";
    if (error || (status != "timeout" && (status != "success" || text(report["status"]) != "finished"))) return 2;
    if (refuted) return 1;
    if (budget.expired() || status == "timeout") { log << "Verification timed out\n"; return 3; }
    return !properties.empty() && covered == properties.size() ? 0 : 3;
}
} // namespace crisp
