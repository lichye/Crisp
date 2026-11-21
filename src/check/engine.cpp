#include "engine.hpp"

#include <ebmc/bmc.h>
#include <ebmc/ebmc_error.h>
#include <ebmc/format_hooks.h>
#include <ebmc/instrument_buechi.h>
#include <ebmc/property_checker.h>
#include <langapi/mode.h>
#include <util/arith_tools.h>
#include <util/config.h>
#include <util/exception_utils.h>
#include <util/ui_message.h>
#include <verilog/verilog_bits.h>
#include <verilog/verilog_ebmc_language.h>
#include <verilog/verilog_language.h>
#include <verilog/verilog_typecheck_base.h>
#include <verilog/verilog_types.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

namespace crisp::check {

// The wildcards of properties PATTERN (spec 20): '*' matches any characters
// except '.', '**' any characters, '?' one character except '.'. All other
// characters, including '[' and ']', match literally.
bool pattern_matches(std::string_view pattern, std::string_view name) {
    enum class Kind { literal, one, star, stars };
    std::vector<std::pair<Kind, char>> tokens;
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern[i] == '*') {
            const auto run = pattern.find_first_not_of('*', i);
            const auto end = run == pattern.npos ? pattern.size() : run;
            tokens.emplace_back(end - i == 1 ? Kind::star : Kind::stars, '*');
            i = end;
        } else {
            tokens.emplace_back(pattern[i] == '?' ? Kind::one : Kind::literal, pattern[i]);
            ++i;
        }
    }
    // rest[j]: the tokens after the current one match name[j..].
    const auto size = name.size();
    std::vector<char> rest(size + 1, 0), here(size + 1);
    rest[size] = 1;
    for (auto token = tokens.rbegin(); token != tokens.rend(); ++token) {
        const auto [kind, c] = *token;
        for (std::size_t j = size + 1; j-- > 0;) {
            const bool more = j < size;
            switch (kind) {
            case Kind::literal: here[j] = more && name[j] == c && rest[j + 1]; break;
            case Kind::one: here[j] = more && name[j] != '.' && rest[j + 1]; break;
            case Kind::star: here[j] = rest[j] || (more && name[j] != '.' && here[j + 1]); break;
            case Kind::stars: here[j] = rest[j] || (more && here[j + 1]); break;
            }
        }
        std::swap(rest, here);
    }
    return rest[0];
}

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
    switch (task.mode.value) {
    case Mode::automatic:
    case Mode::bdd: args.push_back("--bdd"); break;
    case Mode::pdr: args.push_back("--new-ic3"); break;
    case Mode::kind:
    case Mode::bmc:
        if (task.depth->value > std::numeric_limits<unsigned>::max())
            throw std::runtime_error("depth exceeds the maximum EBMC can represent");
        args.insert(args.end(), {"--bound", std::to_string(task.depth->value)});
        if (task.mode.value == Mode::kind) args.push_back("--k-induction");
        if (task.level.value == front::Level::bit) args.push_back("--aig");
        // Word-level SMT, which keeps arrays such as memories unflattened.
        if (task.level.value == front::Level::word) args.push_back("--yices");
        break;
    }
    for (const auto& file : task.files) args.push_back(file.value.string());
    std::vector<const char*> argv;
    for (const auto& arg : args) argv.push_back(arg.c_str());
    cmdlinet cmd;
    if (cmd.parse(argv.size(), argv.data(),
                  "I:D:(systemverilog)(top):(bdd)(new-ic3)(bound):(k-induction)(aig)(yices)(undriven):") ||
        config.set(cmd))
        throw std::runtime_error("EBMC rejected the task options");
    // In this fork --aig takes precedence over --k-induction and dispatches
    // to bounded checking. It cannot implement the requested bit-level induction.
    if (cmd.isset("aig") && cmd.isset("k-induction"))
        throw std::runtime_error("EBMC does not support level bit for mode kind");
    return cmd;
}

// Disable the assertions that no PATTERN selects, as EBMC's --property does.
// Assumptions stay in effect. Every PATTERN must select an assertion.
void select_properties(const front::Task& task, ebmc_propertiest& properties) {
    std::vector<bool> used(task.properties.size(), false);
    for (auto& p : properties.properties) {
        if (p.is_assumption()) continue;
        const auto name = id2string(p.name);
        bool selected = false;
        for (std::size_t i = 0; i < task.properties.size(); ++i) {
            if (pattern_matches(task.properties[i].value, name)) selected = used[i] = true;
        }
        if (!selected) p.disable();
    }
    for (std::size_t i = 0; i < task.properties.size(); ++i) {
        if (used[i]) continue;
        const auto& pattern = task.properties[i];
        const auto& source = pattern.source.value();
        throw std::runtime_error("crisp: " + source.file.string() + ":" + std::to_string(source.line) +
                                 ": [options] properties: pattern '" + pattern.value +
                                 "' matches no assertion");
    }
}

bool refuted(const Property& p) {
    return p.is_refuted() || p.status == Property::statust::REFUTED_WITH_BOUND;
}

[[noreturn]] void reset_error(const front::Setting<front::Reset>& reset, const std::string& detail) {
    const auto& source = reset.source.value();
    throw std::runtime_error("crisp: " + source.file.string() + ":" + std::to_string(source.line) +
                             ": reset '" + reset.value.signal + "' " + detail);
}

class Verilog : public verilog_ebmc_languaget {
public:
    using verilog_ebmc_languaget::verilog_ebmc_languaget;
    void apply_reset(const front::Task& task, transition_systemt& system) {
        const auto& reset = *task.reset;
        const auto& name = reset.value.signal;
        const auto prefix = id2string(system.main_symbol->name) + ".";
        std::string top = task.top ? task.top->value : "";
        if (!task.top) {
            // Match the existing reset lookup among EBMC's default top-level modules.
            for (const auto& entry : system.symbol_table.symbols) {
                const auto& symbol = entry.second;
                if (symbol.type.id() != ID_verilog_module_instance ||
                    id2string(symbol.name) != prefix + id2string(symbol.base_name) ||
                    !system.symbol_table.lookup(id2string(symbol.name) + "." + name))
                    continue;
                if (!top.empty()) reset_error(reset, "is ambiguous among top-level modules; specify top");
                top = id2string(symbol.base_name);
            }
            if (top.empty()) reset_error(reset, "does not exist in any top-level module");
        }

        const auto& module = system.symbol_table.lookup_ref(verilog_module_symbol(top));
        const auto& ports = to_module_type(module.type).ports();
        const auto port = std::find_if(ports.begin(), ports.end(), [&](const auto& item) {
            return item.base_name() == name;
        });
        if (port == ports.end()) {
            const auto* symbol = system.symbol_table.lookup(prefix + top + "." + name);
            if (!symbol) reset_error(reset, "does not exist in " + top);
            const char* kind = symbol->is_state_var || symbol->is_lvalue ? "a register" :
                               symbol->is_macro ? "a constant" : "an internal signal";
            reset_error(reset, std::string("is ") + kind + ", not an input of " + top);
        }
        if (port->direction() != ID_input)
            reset_error(reset, "is an " + id2string(port->direction()) + " port, not an input of " + top);

        const auto width = verilog_bits_opt(port->type());
        if (!width)
            reset_error(reset, "is an input of " + top + " with no fixed bit width; expected a 1-bit input");
        if (*width != 1)
            reset_error(reset, "is a " + integer2string(*width) + "-bit input of " + top +
                               "; expected a 1-bit input");

        // Use EBMC's --reset implementation after validating the elaborated port.
        create_reset_logic(reset_expression(reset.value, top), system);
    }
};

struct UndrivenError {};

// The pinned IC3 engine returns a verdict without a witness. Reconstruct a
// reachable counterexample for that exact assertion under the same deadline.
trans_tracet reconstruct(const cmdlinet& cmd, const transition_systemt& system,
                         ebmc_propertiest target, const Property& property, ui_message_handlert& messages) {
    target.properties.remove_if([&](const auto& p) {
        return !p.is_assumption() && p.identifier != property.identifier;
    });
    for (auto& p : target.properties)
        if (!p.is_assumption()) p.unknown(); // IC3 has already marked this goal refuted.
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

void solve(const front::Task& task, const fs::path& scratch, Reporter reporter, EngineRun operation,
           const std::set<std::string>& skip) {
    auto cmd = command_line(task);
    register_language(new_verilog_language);
    format_hooks();
    ui_message_handlert messages(cmd, "Crisp");
    messages.set_verbosity(messaget::M_WARNING);
    Verilog language(cmd, messages);
    auto system = language.transition_system([&](const auto& undriven) {
        for (const auto& net : undriven)
            reporter.send(0, "undriven", "undriven " + net.name + " at " +
                          id2string(net.location.get_file()) + ':' + id2string(net.location.get_line()));
        if (!undriven.empty() && task.undriven.value == Undriven::error) throw UndrivenError{};
    });
    if (!system) throw std::runtime_error("EBMC produced no transition system");
    if (task.reset) language.apply_reset(task, *system);
    if (operation == EngineRun::validate_reset) return;
    auto properties = ebmc_propertiest::from_command_line(cmd, *system, messages);
    properties.properties.remove_if([](const auto& p) { return p.is_exists_path(); });
    if (!task.properties.empty()) select_properties(task, properties);
    for (auto& p : properties.properties)
        if (!p.is_assumption() && skip.count(id2string(p.name))) p.disable();

    std::map<irep_idt, std::size_t> ids;
    for (const auto& p : properties.properties) {
        if (p.is_assumption() || p.is_disabled()) continue;
        const auto index = ids.size();
        ids.emplace(p.identifier, index);
        reporter.send(index, "pending", id2string(p.name));
    }
    if (ids.empty()) return;

    const bool pdr = task.mode.value == Mode::pdr;
    auto check_batch = [&](transition_systemt& model, ebmc_propertiest& batch) {
        if (pdr || task.mode.value == Mode::kind)
            instrument_buechi(model, batch, pdr, messages);
        auto checked = property_checker(cmd, model, batch, messages);
        if (checked.status != property_checker_resultt::statust::VERIFICATION_RESULT)
            throw std::runtime_error("EBMC did not return verification results");
        const namespacet ns(model.symbol_table);
        messaget message(messages);
        for (auto& p : checked.properties) {
            if (p.is_assumption()) {
                if (p.is_unsupported() || p.is_failure())
                    throw std::runtime_error(id2string(p.name) + ": " + p.status_as_string());
                continue;
            }
            if (p.is_disabled()) continue;
            const auto index = ids.at(p.identifier);
            std::string verdict, reason;
            if (p.is_failure() || p.is_unsupported()) {
                verdict = "error";
                reason = p.location.as_string() + ": " + p.status_as_string();
            } else if (refuted(p)) {
                if (!p.witness_trace) {
                    auto target = pdr ? batch : ebmc_propertiest::from_command_line(cmd, model, messages);
                    p.witness_trace = reconstruct(cmd, model, std::move(target), p, messages);
                }
                write_wave(*p.witness_trace, scratch / (std::to_string(index) + ".vcd"), message, ns);
                verdict = "refuted";
            } else if (task.mode.value == Mode::bmc && (p.is_proved_with_bound() || p.is_proved())) {
                verdict = "passed";
                reason = "bmc depth " + std::to_string(task.depth->value);
            } else if (p.is_proved()) {
                verdict = "proved";
            } else {
                verdict = "unknown";
                if (p.is_proved_with_bound())
                    reason = "no counterexample through depth " + std::to_string(p.bound);
                else if (task.mode.value == Mode::kind && p.is_inconclusive())
                    reason = "k-induction inconclusive at depth " + std::to_string(task.depth->value);
                else reason = "EBMC: " + p.status_as_string();
            }
            reporter.send(index, verdict.c_str(), reason);
        }
    };
    if (!pdr || ids.size() == 1) {
        check_batch(*system, properties);
        return;
    }
    // Publish each PDR verdict before checking the next property in case the task times out.
    ebmc_propertiest assumptions;
    for (const auto& p : properties.properties)
        if (p.is_assumption()) assumptions.properties.push_back(p);
    for (const auto& p : properties.properties) {
        if (p.is_assumption() || p.is_disabled()) continue;
        auto single = assumptions;
        single.properties.push_back(p);
        // The monitor and the checker can both change the model. Start each PDR
        // query from the elaborated design with its original reset and assumptions.
        auto model = *system;
        model.main_symbol = &model.symbol_table.lookup_ref(system->main_symbol->name);
        check_batch(model, single);
    }
}
} // namespace

bool run_engine(const front::Task& task, const std::filesystem::path& scratch, Reporter reporter,
                EngineRun operation, const std::set<std::string>& skip) {
    try {
        if (operation == EngineRun::lower) lower_for_veval(task, scratch, reporter);
        else solve(task, scratch, reporter, operation, skip);
        return true;
    } catch (const UndrivenError&) {
        return false;
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
