// Keep the original design text, using the native parser to identify the wrapper.
#include "../../util/cli/CheckCommands.h"
#include "../../util/task/TaskInputAndExecution.h"
#include <verilog/verilog_language.h>
#include <verilog/verilog_preprocessor.h>
#include <util/message.h>
#include <util/options.h>
#include <cctype>
#include <fstream>
#include <iostream>
#include <list>
#include <set>
#include <regex>
#include <sstream>

namespace veval::workflow {
namespace {
struct Token { std::string text; size_t begin, end; };
std::vector<Token> tokens(const std::string &text) {
    std::vector<Token> out;
    for (size_t i = 0; i < text.size();) {
        const auto start = i;
        if (std::isspace(static_cast<unsigned char>(text[i]))) { ++i; continue; }
        if (text.compare(i, 2, "//") == 0 || text[i] == '`') {
            i = text.find('\n', i); if (i == text.npos) break; continue;
        }
        if (text.compare(i, 2, "/*") == 0) {
            i = text.find("*/", i + 2); require(i != text.npos, "unterminated source comment"); i += 2; continue;
        }
        if (text[i] == '"') {
            ++i;
            while (i < text.size() && text[i] != '"') { if (text[i] == '\\') ++i; ++i; }
            require(i < text.size(), "unterminated source string"); ++i; continue;
        }
        if (text[i] == '\\') {
            ++i; while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        } else if (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_' || text[i] == '$') {
            while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_' || text[i] == '$')) ++i;
        } else ++i;
        out.push_back({text.substr(start, i - start), start, i});
    }
    return out;
}
bool formalStatement(const irept &node) {
    const auto id = node.id();
    return id == ID_verilog_assert_property || id == ID_verilog_assume_property ||
        id == ID_verilog_immediate_assert || id == ID_verilog_immediate_assume ||
        id == ID_verilog_immediate_cover || id == ID_verilog_cover_property ||
        id == ID_verilog_cover_sequence || id == ID_verilog_restrict_property ||
        id == ID_verilog_expect_property || id == ID_verilog_smv_assert ||
        id == ID_verilog_smv_assume || id == ID_verilog_assertion_item ||
        id == ID_verilog_covergroup;
}
void inventory(const irept &node, const std::string &top, J &properties) {
    const auto id = node.id();
    if (id == ID_verilog_assert_property || id == ID_verilog_assume_property) {
        const auto label = id2string(node.get(ID_base_name));
        require(!label.empty(), "S1 requires labelled source assertions and assumptions");
        append(properties, object({{"entry", str(top + "." + label)},
                                   {"kind", str(id == ID_verilog_assert_property ? "assert" : "assume")}}));
        return;
    }
    if (formalStatement(node))
        throw std::runtime_error("S1 source bridge requires labelled concurrent assert/assume properties");
    for (const auto &v : node.get_sub()) inventory(v, top, properties);
    for (const auto &v : node.get_named_sub())
        if (!v.second.get_sub().empty() || !v.second.get_named_sub().empty())
            inventory(v.second, top, properties);
}
}
bool containsFormalStatement(const irept &node) {
    if (formalStatement(node)) return true;
    for (const auto &v : node.get_sub()) if (containsFormalStatement(v)) return true;
    // Named scalar values are identifier/type metadata, not statements.
    for (const auto &v : node.get_named_sub())
        if ((!v.second.get_sub().empty() || !v.second.get_named_sub().empty()) &&
            containsFormalStatement(v.second)) return true;
    return false;
}
int prepareSources(int argc, char **argv) {
    require(argc == 4, "internal source preparation requires request and destination");
    const auto request = jsonFile(argv[2]); const fs::path out = argv[3];
    fs::create_directories(out);
    const auto top = text(request["top"]);
    require(!top.empty(), "a top module is required; specify prep -top TOP in the SBY script");
    J result = object({{"sources", list()}, {"design", list()}, {"properties", list()}, {"modules", list()}});
    std::set<std::string> seen;
    std::string formalBelowTop;
    size_t serial = 0, topCount = 0;
    // One task is one compilation unit, as it is for Yosys: a `define or
    // `undef in a file stays in effect for every later file of the task,
    // across read_verilog commands, while each command's own -D and implicit
    // FORMAL/SYNTHESIS/YOSYS macros apply to that command's files only. The
    // preprocessor's shared table carries exactly the source-text macros, so
    // it is reset once here, never between files.
    verilog_preprocessort::new_compilation_unit();
    for (const auto &unit : items(request["units"])) {
        verilog_languaget language; stream_message_handlert messages(std::cerr);
        optionst options;
        options.set_option("force-systemverilog", unit["systemverilog"].is_true());
        auto include = strings(unit["include"]), defines = strings(unit["defines"]);
        options.set_option("I", std::list<std::string>(include.begin(), include.end()));
        options.set_option("defines", std::list<std::string>(defines.begin(), defines.end()));
        language.set_language_options(options, messages);
        const auto path = text(unit["path"]);
        std::ifstream source(path); require(bool(source), "source unavailable: " + path);
        std::ostringstream preprocessed;
        require(!language.preprocess(source, path, preprocessed, messages), "preprocessing failed: " + path);
        // hw-cbmc emits `line markers for its parser but its public preprocess
        // entry does not accept those markers on a second read. They carry
        // diagnostics only; all macros have already expanded at this point.
        std::istringstream marked(preprocessed.str()); std::string row, value;
        const auto staged = fs::canonical(text(request["staged_root"]));
        while (std::getline(marked, row)) {
            if (trim(row).rfind("`line ", 0) == 0) {
                std::smatch match;
                require(std::regex_search(row, match, std::regex("`line[[:space:]]+[0-9]+[[:space:]]+\"([^\"]+)\"")), "invalid preprocessing source map");
                const auto dependency = fs::canonical(match[1].str());
                auto relative = dependency.lexically_relative(staged);
                require(!relative.empty() && *relative.begin() != "..", "include escapes frozen SBY inputs: " + dependency.string());
                value += '\n';
            } else value += row + '\n';
        }
        writeText(out / ("source-map-" + std::to_string(serial) + ".txt"), preprocessed.str());
        const auto full = out / ("source-" + std::to_string(serial) + ".sv");
        writeText(full, value);
        // parse() preprocesses again; the text has no directives left, so the
        // shared macro table is unchanged for the files that follow.
        std::istringstream parsed(value);
        require(!language.parse(parsed, full.string(), messages), "native source parse failed: " + path);
        std::set<std::string> parsedModules;
        for (const auto &item : language.get_parse_tree().items) {
            require(item.id() != ID_verilog_package,
                    "SystemVerilog packages are not supported by veval lower/check: " +
                    id2string(item.get(ID_base_name)) + " (" + path + ")");
            require(item.id() == ID_verilog_module,
                    "veval lower/check accepts module declarations only: " + path);
            auto name = id2string(to_verilog_module_source(item).base_name());
            require(seen.insert(name).second, "duplicate module: " + name);
            parsedModules.insert(name); append(result.make_object()["modules"], str(name));
            if (name == top) {
                require(unit["formal"].is_true(), "source boundary properties require read_verilog -formal");
                ++topCount; inventory(item, top, result.make_object()["properties"]);
            } else if (formalBelowTop.empty() && containsFormalStatement(item)) formalBelowTop = name;
        }
        // Token boundaries are used only after parsing, to preserve every byte of
        // every design module. This is not a source-level hierarchy inference.
        auto stream = tokens(value); std::set<std::string> lexicalModules;
        for (const auto &token : stream)
            require(token.text != "$readmemh" && token.text != "$readmemb" && token.text != "$fopen",
                    "external data initialization is outside the S1 source contract: " + token.text);
        std::string design = value, wrapper = value;
        for (size_t i = 0; i < stream.size(); ++i) {
            require(stream[i].text == "module", "S1 cannot split compilation-unit declarations or module attributes");
            auto begin = stream[i].begin;
            require(i + 1 < stream.size(), "missing module name");
            // An escaped identifier (`\\bias_child$tpu.pe11 ', as Yosys writes
            // a netlist) names the module without its backslash, as parsed.
            auto unescaped = [](const std::string &text) { return !text.empty() && text[0] == '\\' ? text.substr(1) : text; };
            auto name = unescaped(stream[++i].text);
            require(name != "automatic" && name != "static" && !name.empty(), "unsupported source module spelling: " + name);
            lexicalModules.insert(name);
            while (i < stream.size() && stream[i].text != "endmodule") ++i;
            require(i < stream.size(), "missing endmodule");
            auto end = stream[i].end;
            if (i + 1 < stream.size() && stream[i + 1].text == ":") {
                require(i + 2 < stream.size() && unescaped(stream[i + 2].text) == name, "endmodule label differs");
                i += 2; end = stream[i].end;
            }
            auto &removed = name == top ? design : wrapper;
            for (size_t p = begin; p < end; ++p) if (removed[p] != '\n') removed[p] = ' ';
        }
        require(parsedModules == lexicalModules, "source parser/token module inventory differs");
        const auto rtl = out / ("design-" + std::to_string(serial) + ".sv");
        writeText(rtl, design);
        if (parsedModules.count(top)) {
            const auto spec = out / ("wrapper-" + std::to_string(serial) + ".sv");
            writeText(spec, wrapper);
            append(result.make_object()["sources"], str(spec.string()));
        }
        if (parsedModules.size() > parsedModules.count(top)) {
            append(result.make_object()["design"], str(rtl.string()));
            append(result.make_object()["sources"], str(rtl.string()));
        }
        ++serial;
    }
    require(topCount == 1, "top module '" + top + "' was not found in the input files");
    require(formalBelowTop.empty(), "formal statements below the source wrapper are not supported: " + formalBelowTop);
    require(!items(result["properties"]).empty(), "source wrapper has no supported properties");
    std::set<std::string> properties;
    for (const auto &p : items(result["properties"]))
        require(properties.insert(text(p["entry"])).second, "S1 requires unique property labels at the source boundary");
    require(!items(result["design"]).empty(), "source wrapper has no separate design modules");
    save(out / "prepared.json", result);
    return 0;
}
}
