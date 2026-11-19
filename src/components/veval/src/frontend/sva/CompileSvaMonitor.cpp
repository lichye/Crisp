// Compile typed SVA to deterministic safety monitors or a response liveness monitor.
// Python only supplies the standalone declarations and invokes this executable.
#include "frontend/rtl/ElaboratedDesign.h"
#include "util/verilog/ExpressionPrinter.h"
#include "frontend/sva/PropertyMonitor.h"
#include "frontend/sva/LocalVariableMonitor.h"
#include <ebmc/ebmc_error.h>
#include <ebmc/ebmc_properties.h>
#include <ebmc/transition_system.h>
#include <temporal-logic/hoa.h>
#include <temporal-logic/ltl_sva_to_string.h>
#include <temporal-logic/normalize_property.h>
#include <temporal-logic/sva_to_ltl.h>
#include <temporal-logic/temporal_logic.h>
#include <util/arith_tools.h>
#include <new>
#include <util/bitvector_types.h>
#include <util/cmdline.h>
#include <util/namespace.h>
#include <util/run.h>
#include <verilog/expr2verilog.h>
#include <verilog/sva_expr.h>
#include <verilog/verilog_language.h>
#include <util/message.h>
#include <util/options.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace {
std::string join(const std::vector<std::string>& terms, const char* op, const char* empty) {
    if (terms.empty()) return empty;
    std::string result;
    for (const auto& term : terms) {
        if (!result.empty()) result += op;
        result += "(" + term + ")";
    }
    return result;
}
bool contains(const exprt& expression, const irep_idt& id) {
    if (expression.id() == id) return true;
    for (const auto& operand : expression.operands()) if (contains(operand, id)) return true;
    return false;
}
// The frontend's synthesis currently discards the gate/clock arguments of
// sampled-value calls. Validate the native parser AST before that lossy step.
// Clock binding itself is a caller precondition: one declared posedge domain.
void validateSampledCalls(const irept& tree) {
    if (tree.id() == ID_function_call && tree.get_sub().size() == 2) {
        const auto& function = tree.get_sub()[0];
        const auto name = id2string(function.get(ID_base_name));
        const auto arguments = tree.get_sub()[1].get_sub().size();
        if (name == "$past" && (arguments == 0 || arguments > 2))
            throw std::runtime_error("gated/explicit-clock $past is unsupported; only value and optional depth are accepted");
        if ((name == "$stable" || name == "$rose" || name == "$fell" ||
             name == "$changed" || name == "$sampled") && arguments != 1)
            throw std::runtime_error("explicit-clock sampled-value function is unsupported: " + name);
    }
    for (const auto& child : tree.get_sub()) validateSampledCalls(child);
    for (const auto& item : tree.get_named_sub()) validateSampledCalls(item.second);
}
void validateParsedInterface(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read native SVA interface");
    null_message_handlert messages;
    optionst options; options.set_option("force-systemverilog", true);
    verilog_languaget language;
    language.set_language_options(options, messages);
    if (language.parse(input, path, messages))
        throw std::runtime_error("native SVA interface pre-synthesis parse failed");
    for (const auto& item : language.get_parse_tree().items) validateSampledCalls(item);
}

struct Compiler {
    const namespacet& ns;
    const std::string scope, prefix, clock;
    std::ostringstream declarations;
    std::vector<std::string> generated;
    std::map<exprt, exprt> histories;
    std::map<irep_idt, std::string> internal;
    std::map<std::string,std::string> functions;

    std::string render(exprt value) {
        auto printed=veval::frontend::prepareFunctionPrint(std::move(value),ns);
        for(const auto& [name,definition]:printed.definitions) {
            auto found=functions.emplace(name,definition);
            if(found.second) declarations<<definition;
            else if(found.first->second!=definition) throw std::runtime_error("pure function name collision");
        }
        return expr2verilog(veval::frontend::prepareTypedPrint(std::move(printed.expression)),ns);
    }

    std::string name(const std::string& suffix) {
        auto result = prefix + "_" + suffix;
        generated.push_back(result);
        return result;
    }
    exprt local(exprt value) {
        if (value.id() == ID_symbol) {
            if (value.get_bool("#veval_sva_function_local")) return value;
            auto identifier = id2string(to_symbol_expr(value).get_identifier());
            if (auto found = internal.find(to_symbol_expr(value).identifier()); found != internal.end())
                return symbol_exprt{found->second,value.type()};
            if (identifier.compare(0, scope.size(), scope) != 0)
                throw std::runtime_error("SVA atom is not in the supplied typed interface: " + identifier);
            identifier.erase(0, scope.size());
            if (!std::regex_match(identifier, std::regex("[A-Za-z_][A-Za-z_0-9$]*")))
                throw std::runtime_error("SVA atom needs prior typed observation binding: " + identifier);
            if (identifier.compare(0, prefix.size(), prefix) == 0)
                throw std::runtime_error("SVA generated name collides with interface");
            return symbol_exprt(identifier, value.type());
        }
        if (value.id() == ID_verilog_past) {
            auto existing = histories.find(value);
            if (existing != histories.end()) return existing->second;
            const auto ticks = numeric_cast<std::size_t>(value.operands().at(1));
            if (!ticks || *ticks > 1024) throw std::runtime_error("$past requires a constant depth in [0,1024]");
            auto previous = local(value.operands().at(0));
            const auto type = value.type();
            const auto width = type.id() == ID_bool ? 1u : can_cast_type<bitvector_typet>(type)
                ? to_bitvector_type(type).get_width() : 0u;
            if (!width) throw std::runtime_error("$past requires a packed typed value");
            // Match EBMC instrument_past::model_past/default_value under the
            // existing two-state semantics. Do not silently skip early launches.
            for (std::size_t tick = 0; tick < *ticks; ++tick) {
                const auto signal = name("past_" + std::to_string(generated.size()));
                const auto previousText = render(previous);
                declarations << "  reg " << (type.id() == ID_signedbv ? "signed " : "")
                    << '[' << width-1 << ":0] " << signal << "; initial " << signal << " = '0;\n"
                    << "  always @(posedge " << clock << ") " << signal << " <= "
                    << previousText << ";\n";
                previous = symbol_exprt(signal, type);
            }
            histories.emplace(value, previous);
            return previous;
        }
        for (auto& operand : value.operands()) operand = local(std::move(operand));
        return value;
    }
    std::string boolean(const exprt& value) {
        if (has_temporal_operator(value)) throw std::runtime_error("temporal expression in monitor atom");
        return render(local(value));
    }
};

std::string condition(const irept& label, const std::map<hoat::intt, std::string>& aps) {
    if (label.id() == "t") return "1'b1";
    if (label.id() == "f") return "1'b0";
    if (label.id() == "!" && label.get_sub().size() == 1)
        return "!(" + condition(label.get_sub()[0], aps) + ")";
    if (label.id() == "|" || label.id() == "&") {
        std::vector<std::string> terms;
        for (const auto& child : label.get_sub()) terms.push_back(condition(child, aps));
        return join(terms, label.id() == "|" ? " || " : " && ", label.id() == "|" ? "1'b0" : "1'b1");
    }
    return aps.at(static_cast<hoat::intt>(std::stoul(id2string(label.id()))));
}

void compile(const exprt& original, Compiler& compiler, const std::filesystem::path& out) {
    if (veval::sva::hasLocalVariables(original)) {
        if (!veval::sva::xzConstruct(original).empty())
            throw std::runtime_error("four-state-sensitive SVA with local variables");
        auto encoding = veval::sva::localViolationMonitor(original,"selected");
        const auto data = compiler.name("data"), choice = compiler.name("choice"),
            state = compiler.name("state"), next = compiler.name("next"), ok = compiler.name("ok");
        compiler.internal.emplace(encoding.monitor.stateSymbol.identifier(),state);
        const auto update = compiler.boolean(encoding.nextPayload);
        const auto failure = compiler.boolean(encoding.failure);
        const auto abort = compiler.boolean(encoding.abort);
        compiler.declarations << "  reg [" << encoding.payloadWidth-1 << ":0] " << data
            << "; initial " << data << " = '0;\n"
            << "  (* anyseq *) wire [" << encoding.choiceWidth-1 << ":0] " << choice << ";\n"
            << "  wire [" << encoding.payloadWidth+encoding.choiceWidth-1 << ":0] " << state
            << " = {" << choice << ", " << data << "};\n"
            << "  wire [" << encoding.payloadWidth-1 << ":0] " << next << " = " << update << ";\n"
            << "  always @(posedge " << compiler.clock << ") " << data << " <= (" << abort
            << ") ? '0 : " << next << ";\n"
            << "  wire " << ok << " = (" << abort << ") || !(" << failure << ");\n";
        std::ofstream(out / "monitor.v") << compiler.declarations.str();
        std::ofstream metadata(out / "monitor.tsv");
        metadata << "mode\tsafety\nexpr\t" << ok << "\nguard\t1'b1\ndelay\t0\n";
        for (const auto& signal : compiler.generated) metadata << "generated\t" << signal << '\n';
        return;
    }
    exprt body = original;
    if (body.id() == ID_sva_assume) body = body.operands().at(0);
    if (body.id() != ID_sva_always) throw std::runtime_error("expected concurrent clocked SVA property");
    body = body.operands().at(0);
    exprt abort = false_exprt{};
    if (body.id() == ID_sva_disable_iff) {
        abort = body.operands().at(0); body = body.operands().at(1);
    }
    if (contains(body, ID_sva_disable_iff) || has_temporal_operator(abort))
        throw std::runtime_error("only a single outer state-predicate disable iff is supported");
    if (!abort.is_false() && !veval::sva::abortWindowExact(body))
        throw std::runtime_error("disable iff over a body whose failure can be reported before the "
                                 "attempt's window ends is unsupported (abort window too short)");
    auto xz = veval::sva::xzConstruct(body);
    if (xz.empty()) xz = veval::sva::xzConstruct(abort);
    if (!xz.empty()) throw std::runtime_error("four-state-sensitive SVA unsupported: " + xz);
    for (const auto& id : {ID_sva_accept_on, ID_sva_reject_on, ID_sva_sync_accept_on, ID_sva_sync_reject_on})
        if (contains(body, id))
            throw std::runtime_error("accept_on/reject_on unsupported: the available lowering reads the "
                                     "abort condition at the attempt's first cycle only");
    const auto abortText = compiler.boolean(abort);
    auto normalized = normalize_property(body);
    std::string mode = "safety", expression;

    // G(trigger -> F response), including overlapping triggers. One bit is
    // exact because all pending attempts share response and abort conditions.
    exprt trigger = true_exprt{}, response;
    bool responseShape = false;
    if (normalized.id() == ID_sva_s_eventually) {
        response = normalized.operands().at(0); responseShape = true;
    } else if (normalized.id() == ID_implies && normalized.operands().at(1).id() == ID_sva_s_eventually) {
        trigger = normalized.operands().at(0); response = normalized.operands().at(1).operands().at(0); responseShape = true;
    } else if (normalized.id() == ID_or && normalized.operands().size() == 2) {
        for (unsigned i = 0; i < 2; ++i) {
            if (normalized.operands()[i].id() == ID_sva_s_eventually) {
                response = normalized.operands()[i].operands().at(0);
                trigger = not_exprt(normalized.operands()[1-i]); responseShape = true;
            }
        }
    }
    if (responseShape && !has_temporal_operator(trigger) && !has_temporal_operator(response)) {
        const auto pending = compiler.name("pending");
        const auto launch = compiler.boolean(trigger), finish = compiler.boolean(response);
        compiler.declarations << "  reg " << pending << "; initial " << pending << " = 1'b0;\n"
            << "  always @(posedge " << compiler.clock << ") " << pending << " <= !(" << abortText << ") && !("
            << finish << ") && (" << pending << " || (" << launch << "));\n";
        expression = "!(" + pending + ")";
        mode = "live";
    } else {
        // Determinize the complete negated-property NFA by keeping its whole
        // reachable state set. Unlike choosing one NFA path, this also gives
        // an exact safety assumption monitor: no violating path is hidden.
        ltl_sva_to_stringt printer;
        const auto formula = printer(not_exprt(normalize_property(sva_always_exprt(normalized))));
        std::ostringstream hoaOutput;
        if (run("ltl2tgba", {"ltl2tgba", "--sba", "--complete", "--hoaf=1.1", formula}, "", hoaOutput, "") != 0)
            throw std::runtime_error("ltl2tgba failed");
        auto hoa = hoat::from_string(hoaOutput.str());
        hoa.buechi_acceptance_cleanup();
        const auto size = hoa.max_state_number() + 1;
        if (size > 4096) throw std::runtime_error("SVA monitor exceeds 4096 states");
        std::set<unsigned> errors, starts;
        for (const auto& [state, edges] : hoa.body) {
            if (!state.is_accepting()) continue;
            bool absorbing = false;
            for (const auto& edge : edges)
                if (edge.label.id() == "t" && edge.dest_states.size() == 1 && edge.dest_states[0] == state.number)
                    absorbing = true;
            if (!absorbing) throw std::runtime_error("unbounded SVA outside response liveness fragment");
            errors.insert(state.number);
        }
        for (const auto& [key, values] : hoa.header)
            if (key == "Start:") for (const auto& value : values) {
                if (!std::regex_match(value, std::regex("[0-9]+"))) throw std::runtime_error("universal initial automaton state unsupported");
                starts.insert(std::stoul(value));
            }
        if (starts.empty()) throw std::runtime_error("SVA automaton has no initial state");
        std::map<hoat::intt, std::string> aps;
        for (const auto& [number, label] : hoa.parse_AP()) aps[number] = compiler.boolean(printer.atom(label));
        const auto states = compiler.name("states"), next = compiler.name("next");
        std::vector<std::vector<std::string>> incoming(size);
        for (const auto& [state, edges] : hoa.body) {
            if (!state.label.id().empty() && !state.label.is_nil()) throw std::runtime_error("state-labelled automaton unsupported");
            for (const auto& edge : edges) {
                if (edge.dest_states.size() != 1 || !edge.acc_sig.empty()) throw std::runtime_error("universal/edge-accepting automaton unsupported");
                incoming.at(edge.dest_states[0]).push_back(states + "[" + std::to_string(state.number) + "] && (" + condition(edge.label, aps) + ")");
            }
        }
        std::string initial(size, '0');
        for (auto start : starts) initial.at(size - 1 - start) = '1';
        const auto literal = std::to_string(size) + "'b" + initial;
        compiler.declarations << "  reg [" << size-1 << ":0] " << states << "; initial " << states << " = " << literal << ";\n"
            << "  wire [" << size-1 << ":0] " << next << ";\n";
        for (unsigned state = 0; state < size; ++state)
            compiler.declarations << "  assign " << next << '[' << state << "] = " << join(incoming[state], " || ", "1'b0") << ";\n";
        compiler.declarations << "  always @(posedge " << compiler.clock << ") " << states << " <= (" << abortText << ") ? " << literal << " : " << next << ";\n";
        std::vector<std::string> bad;
        for (auto state : errors) bad.push_back(next + '[' + std::to_string(state) + ']');
        expression = "(" + abortText + ") || !(" + join(bad, " || ", "1'b0") + ")";
        std::ofstream(out / "monitor.hoa") << hoaOutput.str();
        std::ofstream(out / "formula.txt") << formula << '\n';
    }
    const auto ok = compiler.name("ok");
    compiler.declarations << "  wire " << ok << " = " << expression << ";\n";
    std::ofstream(out / "monitor.v") << compiler.declarations.str();
    std::ofstream metadata(out / "monitor.tsv");
    metadata << "mode\t" << mode << "\nexpr\t" << ok << "\nguard\t1'b1\ndelay\t0\n";
    for (const auto& signal : compiler.generated) metadata << "generated\t" << signal << '\n';
}
}
int main(int argc, const char **argv) {
    cmdlinet args;
    if (args.parse(argc, argv, "(top):(prefix):(out):(clock):(dump)") || args.args.size() != 1) return 2;
    try {
        veval::frontend::ElaborateOptions options;
        options.files = args.args; options.top = args.get_value("top");
        validateParsedInterface(options.files.front());
        auto design = veval::frontend::ElaboratedDesign::elaborate(options);
        auto properties = ebmc_propertiest::from_transition_system(design.transitionSystem(), design.messageHandler());
        if (args.isset("dump")) {
            for (const auto& property : properties.properties)
                std::cout << "ORIGINAL\n" << property.original_expr.pretty() << "\nNORMAL\n" << property.normalized_expr.pretty() << '\n';
            return 0;
        }
        const auto prefix = args.get_value("prefix");
        if (!std::regex_match(prefix, std::regex("[A-Za-z_][A-Za-z_0-9]*")) || !args.isset("out")) return 2;
        if (properties.properties.size() != 1) throw std::runtime_error("native SVA helper expects exactly one selected property");
        const namespacet ns(design.transitionSystem().symbol_table);
        const std::string clock = args.isset("clock") ? args.get_value("clock") : "clk";
        if (!std::regex_match(clock, std::regex("[A-Za-z_][A-Za-z_0-9$]*")))
            throw std::runtime_error("invalid sampling clock identifier");
        const auto* clockSymbol = design.transitionSystem().symbol_table.lookup(design.topInstance() + '.' + clock);
        if (!clockSymbol || !clockSymbol->is_input ||
            (clockSymbol->type.id() != ID_bool &&
             (!can_cast_type<bitvector_typet>(clockSymbol->type) || to_bitvector_type(clockSymbol->type).get_width() != 1)))
            throw std::runtime_error("sampling clock must be a one-bit input port");
        Compiler compiler{ns, design.topInstance() + '.', prefix, clock, {}, {}, {}, {}, {}};
        const std::filesystem::path out(args.get_value("out"));
        std::filesystem::create_directories(out);
        const auto& selected = properties.properties.front();
        try {
            compile(selected.original_expr, compiler, out);
        } catch (const std::runtime_error& error) {
            throw std::runtime_error(id2string(selected.identifier) + ": " + error.what());
        }
        return 0;
    } catch (const ebmc_errort& e) {
        std::cerr << e.what() << '\n'; return 1;
    } catch (const ltl_sva_to_string_unsupportedt& e) {
        std::cerr << "unsupported SVA node: " << e.expr.id() << '\n'; return 1;
    } catch (const std::bad_alloc&) {
        // Distinct from 1 (refused or malformed): the runtime reads this as a
        // failed run, not as a property no encoding carries (Sby.cpp checked).
        std::cerr << "out of memory\n"; return 4;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
