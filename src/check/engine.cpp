#include "engine.hpp"

#include <ebmc/bmc.h>
#include <ebmc/ebmc_error.h>
#include <ebmc/format_hooks.h>
#include <ebmc/property_checker.h>
#include <langapi/mode.h>
#include <util/config.h>
#include <util/exception_utils.h>
#include <util/ui_message.h>
#include <verilog/verilog_language.h>
#include <verilog/verilog_ebmc_language.h>

#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>

namespace crisp::check {
namespace {
namespace fs = std::filesystem;
using front::Mode;
using front::Undriven;
using Property = ebmc_propertiest::propertyt;

std::string reset_expression(const front::Reset& reset, const std::string& top) {
    return (reset.polarity == front::Polarity::low ? "!" : "") + top + "." + reset.signal;
}

cmdlinet command_line(const front::Task& task) {
    std::vector<std::string> args{"crisp", "--systemverilog", "--undriven",
                                  task.undriven.value == Undriven::constant ? "const" : "seq"};
    for (const auto& dir : task.includes) args.insert(args.end(), {"-I", dir.value.string()});
    for (const auto& item : task.defines) {
        const auto& define = item.value;
        args.insert(args.end(), {"-D", define.name + (define.value ? "=" + *define.value : "")});
    }
    if (task.top) args.insert(args.end(), {"--top", task.top->value});
    if (task.reset && task.top)
        args.insert(args.end(), {"--reset", reset_expression(task.reset->value, task.top->value)});
    switch (task.mode.value) {
    case Mode::automatic:
    case Mode::bdd: args.push_back("--bdd"); break;
    case Mode::pdr: args.push_back("--ic3"); break;
    case Mode::kind:
    case Mode::bmc:
        if (task.depth->value > std::numeric_limits<unsigned>::max())
            throw std::runtime_error("depth exceeds the maximum EBMC can represent");
        args.insert(args.end(), {"--bound", std::to_string(task.depth->value)});
        if (task.mode.value == Mode::kind) args.push_back("--k-induction");
        if (task.level.value == front::Level::bit) args.push_back("--aig");
        break;
    }
    for (const auto& file : task.files) args.push_back(file.value.string());
    std::vector<const char*> argv;
    for (const auto& arg : args) argv.push_back(arg.c_str());
    cmdlinet cmd;
    if (cmd.parse(argv.size(), argv.data(),
                  "I:D:(systemverilog)(top):(reset):(bdd)(ic3)(bound):(k-induction)(aig)(undriven):(undriven-report)") ||
        config.set(cmd))
        throw std::runtime_error("EBMC rejected the task options");
    // In this fork --aig takes precedence over --k-induction and dispatches
    // to bounded checking. It cannot implement the requested bit-level induction.
    if (cmd.isset("aig") && cmd.isset("k-induction"))
        throw std::runtime_error("EBMC does not support level bit for mode kind");
    return cmd;
}

bool refuted(const Property& p) {
    return p.is_refuted() || p.status == Property::statust::REFUTED_WITH_BOUND;
}

// EBMC resolves --reset from $root. When top was omitted, let EBMC
// elaborate its default hierarchy first, then use the same reset handler.
class Verilog : public verilog_ebmc_languaget {
public:
    using verilog_ebmc_languaget::verilog_ebmc_languaget;
    void reset_in_default_top(const front::Reset& reset, transition_systemt& system) {
        const auto prefix = id2string(system.main_symbol->name) + ".";
        std::string top;
        for (const auto& entry : system.symbol_table.symbols) {
            const auto& symbol = entry.second;
            if (symbol.type.id() != ID_verilog_module_instance ||
                id2string(symbol.name) != prefix + id2string(symbol.base_name) ||
                !system.symbol_table.lookup(id2string(symbol.name) + "." + reset.signal))
                continue;
            if (!top.empty())
                throw std::runtime_error("ambiguous reset signal " + reset.signal + "; specify top");
            top = id2string(symbol.base_name);
        }
        if (top.empty()) throw std::runtime_error("unknown reset signal " + reset.signal + " in top-level modules");
        create_reset_logic(reset_expression(reset, top), system);
    }
};

class CoutCapture {
public:
    explicit CoutCapture(std::ostream& target) : original(std::cout.rdbuf(target.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(original); }

    CoutCapture(const CoutCapture&) = delete;
    CoutCapture& operator=(const CoutCapture&) = delete;

private:
    std::streambuf* original;
};

void report_undriven(const cmdlinet& cmd, ui_message_handlert& messages, Undriven policy, Reporter reporter) {
    auto report_cmd = cmd;
    report_cmd.set("undriven-report");
    std::ostringstream output;
    {
        // EBMC exposes the analysis through its report-only frontend path.
        CoutCapture capture(output);
        Verilog(report_cmd, messages).transition_system();
    }
    std::istringstream records(output.str());
    std::string errors, line;
    constexpr std::string_view prefix = "UNDRIVEN\t";
    while (std::getline(records, line)) {
        if (line.compare(0, prefix.size(), prefix) != 0) continue;
        const auto end = line.find('\t', prefix.size());
        if (end == std::string::npos) throw std::runtime_error("malformed EBMC undriven report");
        const auto detail = "undriven " + line.substr(prefix.size(), end - prefix.size()) +
                            " at " + line.substr(end + 1);
        if (policy == Undriven::error) {
            if (!errors.empty()) errors += '\n';
            errors += "crisp: error: " + detail;
        } else {
            reporter.send(0, "undriven", detail);
        }
    }
    if (!errors.empty()) throw std::runtime_error(errors);
}

// The pinned IC3 engine returns a verdict without a witness. Reconstruct a
// reachable counterexample for that exact assertion under the same deadline.
trans_tracet reconstruct(const cmdlinet& cmd, const transition_systemt& system,
                         const Property& property, ui_message_handlert& messages) {
    auto target = ebmc_propertiest::from_command_line(cmd, system, messages);
    target.properties.remove_if([&](const auto& p) {
        return !p.is_assumption() && p.identifier != property.identifier;
    });
    const auto factory = ebmc_solver_factory(cmd);
    for (std::size_t bound = 1; bound < std::numeric_limits<unsigned>::max(); bound *= 2) {
        auto result = bmc(bound, false, false, false, system, target, factory, messages);
        if (result.status != property_checker_resultt::statust::VERIFICATION_RESULT) break;
        bool reached_bound = false;
        for (auto& p : result.properties) {
            if (p.identifier != property.identifier) continue;
            if (refuted(p) && p.witness_trace) return std::move(*p.witness_trace);
            reached_bound = p.is_proved_with_bound();
        }
        if (!reached_bound) break;
    }
    throw std::runtime_error("EBMC could not reconstruct the counterexample for " + id2string(property.name));
}

void write_wave(const trans_tracet& trace, const fs::path& file, messaget& message, const namespacet& ns) {
    if (trace.states.empty()) throw std::runtime_error("EBMC returned an empty counterexample");
    std::ofstream wave(file);
    wave.exceptions(std::ios::badbit | std::ios::failbit);
    if (!trace.states.front().assignments.empty()) {
        show_trans_trace_vcd(trace, message, ns, wave);
    } else {
        // A constant assertion can fail in a module without any signals.
        // The fork's VCD writer requires a signal to identify the top scope.
        wave << "$timescale 1ns $end\n$scope module crisp $end\n"
                "$var wire 1 ! property_failed $end\n$upscope $end\n$enddefinitions $end\n";
        for (std::size_t t = 0; t < trace.states.size(); ++t) {
            wave << '#' << t << '\n' << (trace.states[t].property_failed ? '1' : '0') << "!\n";
            if (trace.states[t].property_failed) break;
        }
    }
    wave.close();
}

void solve(const front::Task& task, const fs::path& scratch, Reporter reporter) {
    auto cmd = command_line(task);
    register_language(new_verilog_language);
    format_hooks();
    ui_message_handlert messages(cmd, "Crisp");
    messages.set_verbosity(messaget::M_WARNING);
    report_undriven(cmd, messages, task.undriven.value, reporter);
    Verilog language(cmd, messages);
    auto system = language.transition_system();
    if (!system) throw std::runtime_error("EBMC produced no transition system");
    if (task.reset && !task.top) language.reset_in_default_top(task.reset->value, *system);
    auto properties = ebmc_propertiest::from_command_line(cmd, *system, messages);
    properties.properties.remove_if([](const auto& p) { return p.is_exists_path(); });

    std::map<irep_idt, std::size_t> ids;
    for (const auto& p : properties.properties) {
        if (p.is_assumption()) continue;
        const auto index = ids.size();
        ids.emplace(p.identifier, index);
        reporter.send(index, "pending", id2string(p.name));
    }
    if (ids.empty()) return;

    auto check_batch = [&](ebmc_propertiest& batch) {
        auto checked = property_checker(cmd, *system, batch, messages);
        if (checked.status != property_checker_resultt::statust::VERIFICATION_RESULT)
            throw std::runtime_error("EBMC did not return verification results");
        const namespacet ns(system->symbol_table);
        messaget message(messages);
        for (auto& p : checked.properties) {
            if (p.is_assumption()) {
                if (p.is_unsupported() || p.is_failure())
                    throw std::runtime_error(id2string(p.name) + ": " + p.status_as_string());
                continue;
            }
            const auto index = ids.at(p.identifier);
            std::string verdict, reason;
            if (p.is_failure() || p.is_unsupported()) {
                verdict = "error";
                reason = p.location.as_string() + ": " + p.status_as_string();
            } else if (refuted(p)) {
                if (!p.witness_trace) p.witness_trace = reconstruct(cmd, *system, p, messages);
                write_wave(*p.witness_trace, scratch / (std::to_string(index) + ".vcd"), message, ns);
                verdict = "refuted";
            } else if (p.is_proved() && task.mode.value != Mode::bmc) {
                verdict = "proved";
            } else {
                verdict = "unknown";
                if (task.mode.value == Mode::bmc && (p.is_proved_with_bound() || p.is_proved()))
                    reason = "no counterexample through depth " + std::to_string(task.depth->value);
                else if (task.mode.value == Mode::kind && p.is_inconclusive())
                    reason = "k-induction inconclusive at depth " + std::to_string(task.depth->value);
                else reason = "EBMC: " + p.status_as_string();
            }
            reporter.send(index, verdict.c_str(), reason);
        }
    };
    if (task.mode.value != Mode::pdr || ids.size() == 1) {
        check_batch(properties);
        return;
    }
    // The pinned IC3 implementation checks only one output per invocation.
    ebmc_propertiest assumptions;
    for (const auto& p : properties.properties)
        if (p.is_assumption()) assumptions.properties.push_back(p);
    for (const auto& p : properties.properties) {
        if (p.is_assumption()) continue;
        auto single = assumptions;
        single.properties.push_back(p);
        check_batch(single);
    }
}
} // namespace

void run_engine(const front::Task& task, const std::filesystem::path& scratch, Reporter reporter) {
    try {
        solve(task, scratch, reporter);
    } catch (const ebmc_errort& error) {
        const auto where = error.location().as_string();
        throw std::runtime_error((where.empty() ? "" : where + ": ") + error.what());
    } catch (const cprover_exception_baset& error) {
        throw std::runtime_error(error.what());
    } catch (const invariant_failedt& error) {
        throw std::runtime_error(error.what());
    } catch (const std::string& error) {
        throw std::runtime_error(error);
    } catch (const char* error) {
        throw std::runtime_error(error);
    }
}

} // namespace crisp::check
