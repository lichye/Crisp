#include "../task/TaskInputAndExecution.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace veval::workflow {
namespace {
// The lines of `text' in sorted order, one per line.
std::string sortedLines(const std::string &text) {
    std::vector<std::string> lines; std::istringstream in(text);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (const auto &line : lines) out += line + "\n";
    return out;
}
std::vector<std::string> split(const std::string &line) {
    std::vector<std::string> out; std::string current; bool quote = false, token = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') { quote = !quote; token = true; }
        else if (c == '\\') {
            require(i + 1 < line.size(), "unsupported line continuation in script");
            current += line[++i]; token = true;
        } else if (!quote && c == '#') break;
        else if (!quote && std::isspace(static_cast<unsigned char>(c))) {
            if (token) { out.push_back(current); current.clear(); token = false; }
        } else { current += c; token = true; }
    }
    require(!quote, "unterminated script quote");
    if (token) out.push_back(current);
    return out;
}
std::string checked(Process &process, const std::string &stage, std::vector<std::string> argv, const fs::path &cwd) {
    auto receipt = process.run(stage, argv, cwd);
    auto directory = fs::path(text(receipt["directory"]));
    if (text(receipt["status"]) == "timeout" && text(receipt["timeout_scope"]) == "task")
        throw Expired(stage + " task deadline exhausted; see " + directory.string());
    require(text(receipt["status"]) == "completed", stage + " failed; see " + directory.string());
    return readText(directory / "stdout.log");
}
}
Input prepareTask(Process &process, const fs::path &source, const std::string &selected) {
    Input input; input.source = fs::canonical(source); input.out = process.out; input.task = selected;
    const auto cwd = source.parent_path();
    auto command = sbyCommand(); command.insert(command.end(), {"--dumpdefaults", source.string()});
    if (selected.empty()) {
        auto names = words(checked(process, "sby-defaults", command, cwd));
        require(names.size() <= 1, "several default tasks: select TASK after the .sby filename");
        if (!names.empty()) input.task = names.front();
    }
    command = sbyCommand(); command.insert(command.end(), {"--dumpcfg", source.string()});
    if (!input.task.empty()) command.push_back(input.task);
    auto expanded = checked(process, "sby-expand", command, cwd);
    writeText(process.out / "task/expanded.sby", expanded);
    std::map<std::string, std::vector<std::string>> sections;
    std::istringstream lines(expanded); std::string line, section;
    while (std::getline(lines, line)) {
        auto value = trim(line);
        if (!value.empty() && value.front() == '[' && value.back() == ']') {
            section = trim(value.substr(1, value.size() - 2));
            require(section == "options" || section == "engines" || section == "script" || section == "files" ||
                    section.rfind("file ", 0) == 0, "unsupported SBY section: " + section);
        } else sections[section].push_back(line);
    }
    // SBY has already applied task/tag prefixes; a later line overrides an
    // earlier one, as SBY's own options do.
    const std::map<std::string, std::set<std::string>> choices{
        {"multiclock", {"on", "off"}},
        {"veval_strategy", {"auto", "deepest", "all-depth", "top"}},
        {"veval_placement", {"single-path", "split", "root"}}};
    for (const auto &row : sections["options"]) {
        auto fields = split(row); if (fields.empty()) continue;
        const auto key = fields[0];
        if (key.rfind("consflow", 0) == 0) {
            const auto renamed = "veval" + key.substr(8);
            if (choices.count(renamed) || renamed == "veval_ladder")
                throw std::runtime_error(key + " is spelled " + renamed + " in a veval check task");
            throw std::runtime_error(key + " belongs to the fast_refute task driver; veval check selects landings itself");
        }
        if (key.rfind("veval_", 0) == 0) require(choices.count(key) || key == "veval_ladder", "unknown veval option: " + key);
        else require(std::set<std::string>{"mode", "depth", "timeout", "multiclock"}.count(key), "unsupported SBY option: " + key);
        if (key == "veval_ladder") {
            require(fields.size() >= 2, "veval_ladder needs at least one depth");
            input.ladder.clear();
            for (size_t i = 1; i < fields.size(); ++i) {
                size_t n = 0; unsigned long depth = 0;
                try { depth = std::stoul(fields[i], &n); } catch (const std::exception &) { n = 0; }
                require(n == fields[i].size() && depth >= 1 && depth <= 1000000, "invalid veval_ladder depth: " + fields[i]);
                require(input.ladder.empty() || depth > input.ladder.back(), "veval_ladder depths must increase: " + row);
                input.ladder.push_back(depth);
            }
            std::string value;
            for (size_t i = 1; i < fields.size(); ++i) value += (i > 1 ? " " : "") + fields[i];
            put(input.options, key, str(value)); continue;
        }
        require(fields.size() == 2, "unsupported SBY option: " + row);
        if (choices.count(key)) require(choices.at(key).count(fields[1]), "invalid " + key + ": " + fields[1]);
        put(input.options, key, str(fields[1]));
    }
    input.mode = text(input.options["mode"]);
    require(input.mode == "prove" || input.mode == "bmc", "S1 supports mode prove or bmc");
    if (truth(input.options["depth"])) {
        const auto value = text(input.options["depth"]); size_t n = 0;
        auto depth = std::stoul(value, &n);
        require(n == value.size() && depth >= 1 && depth <= 1000000, "invalid depth"); input.depth = depth;
    }
    // The ladder's last rung is the task's depth: a stated depth must agree,
    // so that the ladder and `depth' never describe two different checks.
    if (!input.ladder.empty()) {
        require(!truth(input.options["depth"]) || input.depth == input.ladder.back(),
                "depth " + text(input.options["depth"]) + " differs from the last veval_ladder depth");
        input.depth = input.ladder.back();
    }
    input.multiclock = text(input.options["multiclock"]) == "on";
    if (truth(input.options["timeout"])) {
        auto value = text(input.options["timeout"]); size_t n = 0; input.timeout = std::stod(value, &n);
        require(n == value.size() && std::isfinite(input.timeout) && input.timeout > 0, "invalid timeout");
        process.deadline = std::min(process.deadline, process.timeOrigin + input.timeout);
    }
    std::vector<std::vector<std::string>> engines;
    for (const auto &row : sections["engines"]) { auto e = split(row); if (!e.empty()) engines.push_back(e); }
    require(engines.size() == 1, "S1 requires exactly one SBY engine");
    auto engine = engines.front();
    require(engine.size() == 2, "unsupported SBY engine arguments");
    if (engine[0] == "smtbmc") {
        require(std::set<std::string>{"z3", "boolector", "cvc5", "yices"}.count(engine[1]), "unsupported SMT solver");
        // SBY's multiclock keeps every clock event (clk2fflogic); the native
        // runtime names that encoding smtbmc-multiclock.
        input.method = input.multiclock ? "smtbmc-multiclock" : "smtbmc"; input.solver = engine[1];
    } else {
        require(engine[0] == "abc" && engine[1] == "pdr" && input.mode == "prove", "unsupported engine/mode combination");
        require(!input.multiclock, "multiclock on is supported with smtbmc engines only");
        input.method = "abc pdr";
    }
    command = sbyCommand(); command.insert(command.end(), {"--dumpfiles", source.string()});
    if (!input.task.empty()) command.push_back(input.task);
    // SBY prints the --dumpfiles set in Python hash order, which changes per
    // process; kept sorted, so a resume compares the set and not the order.
    auto externalFiles = sortedLines(checked(process, "sby-files", command, cwd));
    writeText(process.out / "task/external-files.txt", externalFiles);
    J identities = list({fileIdentity(source, process.out / "task")});
    std::istringstream paths(externalFiles); std::string file;
    while (std::getline(paths, file)) {
        if (monotonic() >= process.deadline) throw Expired("task deadline during input snapshot");
        require(!interrupted(), "verification interrupted");
        file = trim(file); if (file.empty()) continue;
        const auto path = fs::absolute(cwd / file).lexically_normal();
        require(fs::exists(path), "SBY input unavailable: " + path.string());
        if (fs::is_directory(path)) {
            std::vector<std::string> names;
            for (const auto &entry : fs::recursive_directory_iterator(path)) {
                if (monotonic() >= process.deadline) throw Expired("task deadline during directory snapshot");
                require(!entry.is_symlink() || !entry.is_directory(), "nested directory symlinks are outside the S1 input contract");
                if (entry.is_regular_file()) {
                    append(identities, fileIdentity(entry.path(), process.out / "task"));
                    names.push_back(entry.path().lexically_relative(path).string());
                }
            }
            std::sort(names.begin(), names.end());
            append(identities, object({{"path", str(path.string())}, {"directory", boolean(true)},
                {"resolved_path", str(fs::canonical(path).string())}, {"files", stringArray(names)}}));
        } else append(identities, fileIdentity(path, process.out / "task"));
    }
    save(process.out / "task/input-identities.json", identities);
    // SBY owns file aliases, environment expansion and embedded file bodies.
    // It stages from a copy without the veval_* keys, which are not SBY
    // options; [files] paths resolve against the working directory, as before.
    const auto staging = process.out / "task/sby-input.sby";
    writeText(staging, withoutVevalOptions(readText(source)));
    command = sbyCommand(); command.insert(command.end(), {"--setup", "-d", (process.out / "task/sby").string(), staging.string()});
    if (!input.task.empty()) command.push_back(input.task);
    checked(process, "sby-setup", command, cwd);
    auto staged = fs::canonical(process.out / "task/sby/src");
    auto resolve = [&](const std::string &name) {
        require(!fs::path(name).is_absolute(), "script input must use a staged relative path: " + name);
        auto path = fs::canonical(staged / name);
        auto relative = path.lexically_relative(staged);
        require(!relative.empty() && *relative.begin() != "..", "script path escapes staged inputs: " + name);
        return path.string();
    };
    bool prepared = false;
    for (const auto &row : sections["script"]) {
        auto args = split(row); if (args.empty()) continue;
        if (args[0] == "prep") {
            require(!prepared && args.size() == 3 && args[1] == "-top" && !args[2].empty(),
                    "a top module is required; use exactly one prep -top TOP in the SBY script");
            input.top = args[2]; prepared = true; continue;
        }
        require(!prepared && args[0] == "read_verilog", "unsupported SBY script command: " + row);
        bool sv = false, formal = false; J includes = list(), defines = list();
        std::vector<std::string> paths;
        for (size_t i = 1; i < args.size(); ++i) {
            const auto &a = args[i];
            if (a == "-sv") sv = true;
            else if (a == "-formal") formal = true;
            else if (a.rfind("-I", 0) == 0 || a.rfind("-D", 0) == 0) {
                auto value = a.substr(2);
                if (value.empty()) { require(i + 1 < args.size(), "missing read option value"); value = args[++i]; }
                if (a[1] == 'I') append(includes, str(resolve(value))); else append(defines, str(value));
            } else {
                require(!a.empty() && a[0] != '-' && a.find_first_of(";*?[]") == a.npos, "unsupported read_verilog argument: " + a);
                paths.push_back(resolve(a));
            }
        }
        require(!paths.empty(), "read_verilog has no source files");
        std::set<std::string> macros;
        for (const auto &define : items(defines)) {
            const auto value = text(define), name = value.substr(0, value.find('='));
            require(name != "YOSYS" && name != "FORMAL" && name != "SYNTHESIS", "builtin macro override is outside the S1 contract: " + name);
            require(macros.insert(name).second, "duplicate read_verilog macro: " + name);
        }
        append(defines, str(formal ? "FORMAL=1" : "SYNTHESIS=1"));
        append(defines, str("YOSYS=1"));
        // The native frontend consumes preprocessed text with per-read flags.
        for (const auto &path : paths) append(input.units, object({{"path", str(path)},
            {"systemverilog", boolean(sv)}, {"formal", boolean(formal)}, {"include", includes}, {"defines", defines}}));
    }
    require(prepared, "a top module is required; add prep -top TOP after read_verilog in the SBY script");
    require(!items(input.units).empty(), "the SBY script requires source reads before prep -top TOP");
    save(process.out / "task/source-request.json", object({{"top", str(input.top)}, {"units", input.units},
        {"staged_root", str(staged.string())}}));
    save(process.out / "task/input.json", describeInput(input));
    return input;
}
void verifyTask(Process &process, const Input &input, const std::string &requestedTask) {
    const auto cwd = input.source.parent_path();
    if (requestedTask.empty()) {
        auto command = sbyCommand(); command.insert(command.end(), {"--dumpdefaults", input.source.string()});
        const auto names = words(checked(process, "resume-defaults", command, cwd));
        require(names.size() <= 1 && (names.empty() ? "" : names.front()) == input.task, "default task changed; choose a new --out directory");
    }
    for (const auto &field : {std::make_pair("--dumpcfg", "expanded.sby"), std::make_pair("--dumpfiles", "external-files.txt")}) {
        auto command = sbyCommand(); command.insert(command.end(), {field.first, input.source.string()});
        if (!input.task.empty()) command.push_back(input.task);
        auto actual = checked(process, "resume-task-validation", command, cwd);
        auto saved = readText(process.out / "task" / field.second);
        if (std::string(field.first) == "--dumpfiles") { actual = sortedLines(actual); saved = sortedLines(saved); }
        require(actual == saved, "expanded SBY inputs changed; choose a new --out directory");
    }
}
J describeInput(const Input &i) {
    J ladder = list(); for (const auto depth : i.ladder) append(ladder, num(depth));
    return object({{"source", str(i.source.string())}, {"task", str(i.task)}, {"mode", str(i.mode)},
        {"method", str(i.method)}, {"solver", str(i.solver)}, {"top", str(i.top)},
        {"depth", num(i.depth)}, {"timeout", num(i.timeout)}, {"multiclock", boolean(i.multiclock)},
        {"ladder", ladder}, {"units", i.units}, {"options", i.options}});
}
Input restoreInput(const J &j, const fs::path &out) {
    Input i; i.source = text(j["source"]); i.out = out; i.task = text(j["task"]);
    i.mode = text(j["mode"]); i.method = text(j["method"]); i.solver = text(j["solver"]);
    i.top = text(j["top"]); i.depth = numeric(j["depth"]); i.timeout = numeric(j["timeout"]);
    i.multiclock = j["multiclock"].is_true();
    for (const auto &depth : items(j["ladder"])) i.ladder.push_back(numeric(depth));
    i.units = j["units"]; i.options = j["options"]; return i;
}
J ladderJson(const Input &i) {
    // The single-depth rung keeps its historical name, so default Jobs (and
    // their content-derived identities) are unchanged.
    if (i.ladder.empty()) return list({list({str("check"), str(i.mode), num(i.depth), J()})});
    J ladder = list();
    for (const auto depth : i.ladder)
        append(ladder, list({str(i.mode + "_d" + std::to_string(depth)), str(i.mode), num(depth), J()}));
    return ladder;
}
J effectiveConfiguration(const Input &input, const J &configuration) {
    J effective = object(), from = object();
    auto pick = [&](const std::string &name, const std::string &fallback) {
        const auto cli = text(configuration[name]), sby = text(input.options["veval_" + name]);
        const auto origin = !cli.empty() ? "cli" : !sby.empty() ? "sby" : "default";
        const auto value = !cli.empty() ? cli : !sby.empty() ? sby : fallback;
        put(effective, name, str(value)); put(from, name, str(origin));
        return value;
    };
    pick("strategy", "auto");
    pick("placement", "single-path");
    put(effective, "mode", str(input.mode)); put(from, "mode", str("sby"));
    put(effective, "method", str(input.method));
    put(effective, "solver", input.solver.empty() ? J() : str(input.solver));
    put(from, "engine", str("sby"));
    put(effective, "multiclock", boolean(input.multiclock));
    put(from, "multiclock", str(truth(input.options["multiclock"]) ? "sby" : "default"));
    put(effective, "depth", num(input.depth));
    put(from, "depth", str(truth(input.options["depth"]) || !input.ladder.empty() ? "sby" : "default"));
    put(effective, "ladder", ladderJson(input));
    put(from, "ladder", str(input.ladder.empty() ? "depth" : "sby"));
    put(effective, "sources", from);
    return effective;
}
std::string withoutVevalOptions(const std::string &text) {
    std::istringstream in(text); std::string line, section, out;
    while (std::getline(in, line)) {
        const auto value = trim(line);
        if (!value.empty() && value.front() == '[' && value.back() == ']') section = trim(value.substr(1, value.size() - 2));
        else if (section == "options") {
            // A task/tag prefix ends at the first colon (`task: key value').
            auto option = value; const auto colon = option.find(':');
            if (colon != option.npos) option = trim(option.substr(colon + 1));
            if (option.rfind("veval_", 0) == 0) continue;
        }
        out += line + "\n";
    }
    return out;
}
}
