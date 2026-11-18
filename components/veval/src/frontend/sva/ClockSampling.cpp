#include "ClockSampling.h"

#include "frontend/rtl/ElaboratedDesign.h"

#include <ebmc/transition_system.h>
#include <util/std_expr.h>
#include <util/symbol_table.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <regex>
#include <set>

namespace veval {
namespace sva {

namespace {

// The text with comments and strings blanked, so that neither a decoy
// `@(negedge x)' in a comment nor one in a string is read as code.
std::string maskComments(const std::string& file) {
    std::string masked = file;
    enum { code, lineComment, blockComment, stringLiteral } state = code;
    for (std::size_t i = 0; i < file.size(); ++i) {
        if (state == code && i + 1 < file.size() && file[i] == '/' && file[i + 1] == '/') {
            state = lineComment; masked[i] = masked[i + 1] = ' '; ++i;
        } else if (state == code && i + 1 < file.size() && file[i] == '/' && file[i + 1] == '*') {
            state = blockComment; masked[i] = masked[i + 1] = ' '; ++i;
        } else if (state == blockComment && i + 1 < file.size() && file[i] == '*' && file[i + 1] == '/') {
            masked[i] = masked[i + 1] = ' '; ++i; state = code;
        } else if (state == lineComment && file[i] == '\n') state = code;
        else if (state == code && file[i] == '"') state = stringLiteral;
        else if (state == stringLiteral && file[i] == '"' && (i == 0 || file[i - 1] != '\\')) state = code;
        else if (state == lineComment || state == blockComment || state == stringLiteral) {
            if (file[i] != '\n') masked[i] = ' ';
        }
    }
    return masked;
}

std::string lastComponent(const std::string& name) {
    const auto at = name.rfind('.');
    return at == std::string::npos ? name : name.substr(at + 1);
}

// Every match of these declaration patterns starts with a known literal.
// Locate it with the string search, then apply the unchanged regex at that
// position. Keeping the preceding character visible preserves word-boundary
// semantics; the regex still decides whether each candidate is a match.
template <typename Match>
void literalMatches(const std::string& text, const std::string& literal,
                    const std::regex& expression, Match match) {
    std::size_t from = 0;
    while (from < text.size()) {
        const auto at = text.find(literal, from);
        if (at == std::string::npos) break;
        auto flags = std::regex_constants::match_continuous;
        if (at != 0) flags |= std::regex_constants::match_prev_avail;
        std::smatch found;
        if (std::regex_search(text.cbegin() + at, text.cend(), found,
                              expression, flags)) {
            match(at, found);
            from = at + found.length();
        } else {
            from = at + 1;
        }
    }
}

}  // namespace

struct SamplingCheck::Impl {
    const transition_systemt& system;
    std::map<std::string, std::string> masked;     // file -> masked text
    bool clocksBuilt = false;
    std::map<irep_idt, irep_idt> parent;           // union-find over nets
    std::set<irep_idt> flopClockRoots;

    explicit Impl(const transition_systemt& s) : system(s) {}
    bool statement(const irep_idt& property, const exprt& original,
                   std::string& body, std::string& shownName);

    const std::string& text(const std::string& file) {
        auto found = masked.find(file);
        if (found != masked.end()) return found->second;
        std::ifstream in(file);
        std::string content((std::istreambuf_iterator<char>(in)), {});
        return masked.emplace(file, maskComments(content)).first->second;
    }

    irep_idt root(irep_idt a) {
        while (true) {
            auto found = parent.find(a);
            if (found == parent.end() || found->second == a) return a;
            a = found->second;
        }
    }
    void join(const irep_idt& a, const irep_idt& b) {
        auto ra = root(a), rb = root(b);
        if (ra != rb) parent[std::max(ra, rb)] = std::min(ra, rb);
    }

    // Every net that clocks a flop, as alias classes: the event controls of
    // the always blocks, read from each module's source and placed at each
    // instance of that module, joined through whole same-width ties of the
    // invariant (the port bindings).
    void buildClocks() {
        if (clocksBuilt) return;
        clocksBuilt = true;

        std::function<void(const exprt&)> ties = [&](const exprt& e) {
            if (e.id() == ID_and) {
                for (const auto& op : e.operands()) ties(op);
                return;
            }
            if (e.id() != ID_equal || e.operands().size() != 2) return;
            auto bare = [](const exprt& x) -> const exprt& {
                const exprt* p = &x;
                while (p->id() == ID_typecast && p->operands().size() == 1 &&
                       p->type() == p->operands()[0].type())
                    p = &p->operands()[0];
                return *p;
            };
            const auto& l = bare(e.operands()[0]);
            const auto& r = bare(e.operands()[1]);
            if (l.id() == ID_symbol && r.id() == ID_symbol && l.type() == r.type())
                join(to_symbol_expr(l).get_identifier(), to_symbol_expr(r).get_identifier());
        };
        ties(system.trans_expr.invar());

        // Module name -> clock identifiers its always blocks are written on.
        std::map<std::string, std::set<std::string>> moduleClocks;
        std::set<std::string> files;
        for (const auto& [name, symbol] : system.symbol_table.symbols)
            if (symbol.type.id() == ID_module && !symbol.location.get_file().empty())
                files.insert(id2string(symbol.location.get_file()));
        const std::regex moduleHead(R"(\bmodule\s+(\\?[A-Za-z_$][\w$]*))");
        const std::regex event(R"(\balways(?:_ff|_latch)?\s*@\s*\(\s*(?:posedge|negedge)\s+([A-Za-z_][\w$]*))");
        for (const auto& file : files) {
            const auto& masked = text(file);
            literalMatches(masked, "module", moduleHead,
                           [&](std::size_t begin, const std::smatch& module) {
                const auto close = masked.find("endmodule", begin);
                const auto body = masked.substr(begin, close == std::string::npos ? std::string::npos : close - begin);
                literalMatches(body, "always", event,
                               [&](std::size_t, const std::smatch& clock) {
                    moduleClocks[module[1].str()].insert(clock[1].str());
                });
            });
        }

        // Each instance (and the top) of each module.
        auto place = [&](const std::string& path, const std::string& module) {
            auto clocks = moduleClocks.find(module);
            if (clocks == moduleClocks.end()) return;
            for (const auto& clock : clocks->second) {
                const irep_idt id = "Verilog::$root." + path + "." + clock;
                if (system.symbol_table.lookup(id) != nullptr) flopClockRoots.insert(id);
            }
        };
        for (const auto& [name, symbol] : system.symbol_table.symbols) {
            if (symbol.type.id() != ID_verilog_module_instance) continue;
            const auto* module = system.symbol_table.lookup(symbol.value.get(ID_module));
            if (module == nullptr) continue;
            place(frontend::displayName(id2string(name)), id2string(module->pretty_name));
        }
        if (system.main_symbol != nullptr)
            place(frontend::displayName(id2string(system.main_symbol->base_name)),
                  id2string(system.main_symbol->pretty_name));

        std::set<irep_idt> roots;
        for (const auto& id : flopClockRoots) roots.insert(root(id));
        flopClockRoots = std::move(roots);
    }
};

SamplingCheck::SamplingCheck(const transition_systemt& system)
    : impl(std::make_unique<Impl>(system)) {}
SamplingCheck::~SamplingCheck() = default;

// The labelled source statement whose span covers the original expression,
// as its text between the outer parentheses; false when it cannot be
// located uniquely.
bool SamplingCheck::Impl::statement(const irep_idt& propertyId, const exprt& original,
                                    std::string& body, std::string& shownName) {
    const auto* property = system.symbol_table.lookup(propertyId);
    if (property == nullptr) return false;
    const auto file = id2string(property->location.get_file());
    if (file.empty()) return false;
    const auto& masked = text(file);
    if (masked.empty()) return false;

    shownName = frontend::displayName(id2string(propertyId));
    const auto label = lastComponent(shownName);
    if (!std::regex_match(label, std::regex("[A-Za-z_][A-Za-z_0-9]*"))) return false;

    // The lines the original expression comes from: the source statement is
    // the one labelled `label' whose span covers them.
    std::size_t first = 0, last = 0;
    std::function<void(const exprt&)> lines = [&](const exprt& e) {
        const auto& location = e.source_location();
        if (id2string(location.get_file()) == file && !location.get_line().empty()) {
            try {
                const auto line = static_cast<std::size_t>(std::stoull(id2string(location.get_line())));
                if (!first || line < first) first = line;
                last = std::max(last, line);
            } catch (const std::exception&) {
            }
        }
        for (const auto& op : e.operands()) lines(op);
    };
    lines(original);
    if (!first) return false;

    const std::regex declaration("\\b" + label + "\\s*:\\s*(assert|assume|cover)\\s+property\\s*\\(");
    bool located = false;
    for (std::sregex_iterator it(masked.begin(), masked.end(), declaration), end; it != end; ++it) {
        const auto begin = static_cast<std::size_t>(it->position());
        const auto open = begin + it->length() - 1;
        std::size_t close = open + 1, depth = 1;
        for (; close < masked.size() && depth; ++close) {
            if (masked[close] == '(') ++depth;
            if (masked[close] == ')') --depth;
        }
        if (depth) continue;
        const auto firstLine = 1 + static_cast<std::size_t>(std::count(masked.begin(), masked.begin() + begin, '\n'));
        const auto lastLine = 1 + static_cast<std::size_t>(std::count(masked.begin(), masked.begin() + close, '\n'));
        if (first < firstLine || last > lastLine) continue;
        if (located) return false;  // ambiguous: say nothing rather than guess
        located = true;
        body = masked.substr(open + 1, close - open - 2);
    }
    return located;
}

std::string SamplingCheck::refusal(const irep_idt& propertyId, const exprt& original) {
    std::string body, shownName;
    if (!impl->statement(propertyId, original, body, shownName)) return {};

    const auto events = std::count(body.begin(), body.end(), '@');
    if (body.find('`') != std::string::npos && events == 0)
        return "sampling event written as a macro is not checked; refused";
    if (events > 1)
        return "nested or multiple sampling events are unsupported: the frontend keeps none of them";
    if (events == 0) return {};  // default clocking: the frontend's own model
    std::smatch m;
    if (!std::regex_search(body, m, std::regex(R"(^\s*@\s*\(\s*posedge\s+([A-Za-z_][A-Za-z_0-9]*)\s*\))")))
        return "the sampling event is not `@(posedge <net>)'; the frontend drops "
               "event controls and would check it as a posedge property";

    // `@(posedge X)': X must be (tied to) a net that clocks a flop of the
    // design, or the property is sampled on edges the model does not have.
    impl->buildClocks();
    if (impl->flopClockRoots.empty()) return {};
    const auto scope = shownName.substr(0, shownName.rfind('.'));
    const irep_idt clock = "Verilog::$root." + scope + "." + m[1].str();
    if (impl->system.symbol_table.lookup(clock) == nullptr) return {};
    if (impl->flopClockRoots.count(impl->root(clock))) return {};
    return "sampled on `" + m[1].str() + "', which clocks no flop of the design; the "
           "frontend steps every flop on one global clock, so this would be checked "
           "on different edges than written";
}

std::string SamplingCheck::clock(const irep_idt& propertyId, const exprt& original) {
    std::string body, shownName;
    if (!impl->statement(propertyId, original, body, shownName)) return {};
    if (std::count(body.begin(), body.end(), '@') != 1) return {};
    std::smatch m;
    if (!std::regex_search(body, m, std::regex(R"(^\s*@\s*\(\s*posedge\s+([A-Za-z_][A-Za-z_0-9]*)\s*\))")))
        return {};
    const auto net = shownName.substr(0, shownName.rfind('.')) + "." + m[1].str();
    if (impl->system.symbol_table.lookup("Verilog::$root." + net) == nullptr) return {};
    return net;
}

}  // namespace sva
}  // namespace veval
