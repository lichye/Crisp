#include "verify.hpp"
#include "util/process/RunExternalCommand.h"
#include "util/task/CommandArgumentsAndFiles.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace crisp {
using namespace veval::verification;

Deadline::Deadline(double seconds, fs::path program) : end(monotonic() + seconds), supervisor(std::move(program)) {}
bool Deadline::expired() const { return monotonic() >= end; }

std::string Deadline::run(const std::vector<std::string>& command, const fs::path& cwd,
                          const fs::path& logs) const {
    const double remaining = end - monotonic();
    if (remaining <= 0) return "timeout";
    std::ostringstream timeout;
    timeout << std::setprecision(17) << remaining;
    fs::create_directories(logs);
    std::vector<std::string> supervised{supervisor.string(), "--directory", logs.string(), "--cwd",
                                        cwd.string(), "--timeout", timeout.str(), "--"};
    supervised.insert(supervised.end(), command.begin(), command.end());
    veval::util::command_runner::command(supervised, logs / "supervisor.log", 0, false, true);
    const auto receipt = parse(read(logs / "command.json"));
    if (!receipt["cleanup_confirmed"].is_true())
        throw std::runtime_error("could not confirm that the component stopped");
    return expired() ? "timeout" : text(receipt["status"]);
}

Widths modelWires(const fs::path& model, const std::string& top) {
    std::ifstream stream(model);
    if (!stream) throw std::runtime_error("cannot read elaborated SMT model");
    const std::string modulePrefix = "; yosys-smt2-module ", wirePrefix = "; yosys-smt2-wire ";
    std::string line, module;
    Widths widths;
    while (std::getline(stream, line)) {
        if (line.compare(0, modulePrefix.size(), modulePrefix) == 0) {
            module = trim(line.substr(modulePrefix.size()));
        } else if (module == top && line.compare(0, wirePrefix.size(), wirePrefix) == 0) {
            auto field = trim(line.substr(wirePrefix.size()));
            auto space = field.find_last_of(" \t");
            if (space == std::string::npos) throw std::runtime_error("malformed SMT wire declaration");
            widths[trim(field.substr(0, space))] = std::stoul(field.substr(space + 1));
        }
    }
    if (stream.bad()) throw std::runtime_error("error reading elaborated SMT model");
    return widths;
}

// SMART emits fully parenthesized Boolean expressions. Every signal must bind
// to a single bit of the frozen proof model before it can enter an SMT command.
class BooleanParser {
public:
    BooleanParser(const std::string& input, const Widths& widths) : widths(widths) {
        static const std::regex token(R"(\s*(\|->|&&|\|\||==|!=|[()!~^&|]|1'[bB][01]|[01]|[A-Za-z_][A-Za-z0-9_$]*(\.[A-Za-z_][A-Za-z0-9_$]*)*))");
        const auto cleaned = trim(input);
        auto pos = cleaned.cbegin();
        while (pos != cleaned.cend()) {
            std::smatch match;
            if (!std::regex_search(pos, cleaned.cend(), match, token, std::regex_constants::match_continuous))
                throw std::invalid_argument("outside the single-frame Boolean grammar");
            tokens.push_back(match[1].str());
            pos = match[0].second;
        }
    }
    Predicate parsePredicate(const std::string& expression) {
        auto formula = term(0);
        if (index != tokens.size() || wires.empty())
            throw std::invalid_argument("expected one Boolean predicate over DUT signals");
        return {expression, std::move(formula), std::move(wires)};
    }
private:
    const Widths& widths;
    std::vector<std::string> tokens;
    size_t index = 0;
    std::set<std::string> wires;

    std::string take() {
        if (index == tokens.size()) throw std::invalid_argument("incomplete SMART expression");
        return tokens[index++];
    }
    std::string term(unsigned depth) {
        if (depth > 512) throw std::invalid_argument("SMART expression nesting exceeds 512");
        auto value = take();
        if (value != "(") {
            const auto wire = "u_dut." + value;
            const auto found = widths.find(wire);
            if (found == widths.end()) {
                if (value == "0" || value == "1'b0" || value == "1'B0" || value == "false") return "false";
                if (value == "1" || value == "1'b1" || value == "1'B1" || value == "true") return "true";
            }
            if (found == widths.end() || found->second != 1)
                throw std::invalid_argument(wire + " is absent or is not a one-bit wire in the proof model");
            wires.insert(wire);
            return "[" + wire + "]";
        }
        if (index < tokens.size() && (tokens[index] == "!" || tokens[index] == "~")) {
            ++index;
            value = "(not " + term(depth + 1) + ")";
        } else {
            value = term(depth + 1);
            auto op = take();
            if (op == ")") return value;
            static const std::map<std::string, std::string> operators{
                {"&&","and"}, {"&","and"}, {"||","or"}, {"|","or"},
                {"^","xor"}, {"|->","=>"}, {"==","="}, {"!=","distinct"}};
            auto found = operators.find(op);
            if (found == operators.end()) throw std::invalid_argument("unsupported operator " + op);
            value = "(" + found->second + " " + value + " " + term(depth + 1) + ")";
        }
        if (take() != ")") throw std::invalid_argument("unbalanced SMART expression");
        return value;
    }
};

Predicate bindPredicate(const std::string& expression, const Widths& widths) {
    return BooleanParser(expression, widths).parsePredicate(expression);
}

static std::string shellQuote(const std::string& value) {
    std::string result = "'";
    for (char c : value) result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
}

static std::string optionalText(const fs::path& file) {
    return fs::is_regular_file(file) ? read(file) : "";
}

std::string Solver::attempt(const fs::path& directory, const std::vector<Predicate>& predicates,
                            bool retry, std::ostream& log) const {
    if (predicates.empty() || (!retry && predicates.size() != 1))
        throw std::logic_error("each lemma needs its own independent assertion check");
    fs::create_directory(directory);
    std::set<std::string> wires;
    std::ostringstream rules, task;
    rules << "always\n";
    for (const auto& predicate : predicates) {
        rules << (retry ? "assume " : "assert ") << predicate.formula << '\n';
        wires.insert(predicate.wires.begin(), predicate.wires.end());
    }
    task << "[options]\nmode prove\ndepth " << depth << "\nsmtc rules.smtc\n\n[engines]\n"
         << "smtbmc boolector\n\n[script]\nread_rtlil model.il\nsetattr -set keep 1";
    for (const auto& wire : wires) task << " w:" << wire;
    if (!retry) task << "\nchformal -assert -remove";
    task << "\n\n[files]\nmodel.il ../model.il\nrules.smtc\n";
    write(directory / "rules.smtc", rules.str());
    write(directory / "proof.sby", task.str());
    if (fileDigest(model) != modelHash) throw std::runtime_error("original module model changed");
    const auto rulesHash = fileDigest(directory / "rules.smtc");
    const auto bin = fs::path(text(tools["boolector"])).parent_path();
    const auto* path = std::getenv("PATH");
    const auto status = budget.run({"env", "PATH=" + bin.string() + ":" + (path ? path : ""),
        text(tools["sby"]), "--yosys", shellQuote(text(tools["yosys"])),
        "--smtbmc", shellQuote(text(tools["yosys-smtbmc"])), "-d", "sby", "proof.sby"},
        directory, directory / "command");
    const auto run = directory / "sby";
    std::string raw;
    std::istringstream(optionalText(run / "status")) >> raw;
    if (raw == "PASS" || raw == "FAIL" || raw == "UNKNOWN") {
        if (fileDigest(run / "src/model.il") != modelHash || fileDigest(run / "src/rules.smtc") != rulesHash)
            throw std::runtime_error("solver loaded different model or constraint bytes");
    }
    const auto base = optionalText(run / "engine_0/logfile_basecase.txt");
    const auto induction = optionalText(run / "engine_0/logfile_induction.txt");
    if (status == "success" && raw == "PASS" && base.find("Status: passed") != std::string::npos &&
        induction.find("Temporal induction successful") != std::string::npos &&
        induction.find("Status: passed") != std::string::npos) return "proved";
    if (status == "failed" && raw == "FAIL" && base.find("Status: failed") != std::string::npos) {
        const auto wave = run / "engine_0/trace.vcd";
        if (!fs::is_regular_file(wave)) return "unknown";
        log << "Counterexample (VCD)\n" << read(wave) << '\n';
        return "refuted";
    }
    if (status == "timeout" || raw == "UNKNOWN" || raw == "TIMEOUT") return "unknown";
    log << "Solver error\n" << optionalText(directory / "command/stdout.log");
    return "error";
}
} // namespace crisp
