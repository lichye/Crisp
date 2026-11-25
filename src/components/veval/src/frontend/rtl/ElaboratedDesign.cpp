#include "ElaboratedDesign.h"
#include "HwCbmcVerilogSynthesis.h"
#include "util/MeasureGeneration.h"
#include "util/verilog/ExpressionPrinter.h"

#include <util/cmdline.h>
#include <util/config.h>
#include <util/message.h>
#include <util/ui_message.h>

#include <ebmc/ebmc_error.h>
#include <ebmc/transition_system.h>
#include <langapi/mode.h>
#include <verilog/verilog_language.h>
#include <verilog/verilog_ebmc_language.h>
#include <verilog/top_level_modules.h>
#include <verilog/verilog_bind.h>
#include <verilog/verilog_typecheck_base.h>

#include <sstream>
#include <stdexcept>

namespace veval {
namespace frontend {

namespace {
// Dropping the last owner of a deep shared expression normally recurses
// through the vendor IR destructor. Large array-update chains can overflow
// the process stack after otherwise successful generation. Move children of
// uniquely owned nodes onto an explicit work list before releasing the node.
// Shared nodes are left intact for their other owners; no COW copy is needed.
void releaseIrep(irept& root, std::vector<irept>& pending) {
    pending.push_back(std::move(root));
    while (!pending.empty()) {
        irept node = std::move(pending.back());
        pending.pop_back();
        if (node.read().ref_count != 1) continue;
        for (auto& child : node.get_sub()) pending.push_back(std::move(child));
        for (auto& entry : node.get_named_sub()) pending.push_back(std::move(entry.second));
    }
}
// The shared frontend owns parsing, binding and type checking. Veval consumes
// its typed modules with the symbolic evaluator used by property transport.
class DesignLanguage final : public verilog_ebmc_languaget {
public:
    using verilog_ebmc_languaget::verilog_ebmc_languaget;

    std::optional<transition_systemt> transition_system() override {
        auto trees = parse();
        auto symbols = elaborate_compilation_units(trees);
        const auto tops = top_level_modules(trees, cmdline);
        if (tops.size() != 1)
            throw std::runtime_error("elaborate: expected one top module, found " +
                                     std::to_string(tops.size()));
        const auto top = verilog_module_symbol(tops.front());
        const auto root = verilog_module_symbol(verilog_root_module_name());
        const auto rootStandard = trees.front().standard;
        auto topStandard = rootStandard;
        for (const auto& tree : trees) {
            std::set<std::string> modules;
            tree.modules_provided(modules);
            if (modules.count(id2string(top))) {
                topStandard = tree.standard;
                break;
            }
        }
        profile::timed("frontend.typecheck", [&] {
            elaborate_module_hierarchy(trees, tops, symbols);
            check_bind_directives(symbols);
        });

        // Parameterized instances and binds are now elaborated. The evaluator
        // needs their typed values, while parser source templates can go.
        trees.clear();
        std::vector<irep_idt> sources;
        for (const auto& entry : symbols.symbols)
            if (entry.second.type.id() == ID_module_source)
                sources.push_back(entry.first);
        for (const auto& source : sources) symbols.remove(source);

        profile::Phase synthesis("frontend.synthesis");
        const bool ignoreInitial = cmdline.isset("ignore-initial");
        const bool initialZero = cmdline.isset("initial-zero");
        verilog_synthesis(symbols, top, topStandard, ignoreInitial, initialZero, message_handler);
        create_root_module(tops, symbols);
        transition_systemt result;
        result.symbol_table = std::move(symbols);
        result.main_symbol = result.symbol_table.lookup(root);
        result.trans_expr = verilog_synthesis(result.symbol_table, root, rootStandard,
                                              ignoreInitial, initialZero, message_handler);
        return result;
    }
};
} // namespace

// The frontend reads its options straight off a cmdlinet rather than from a
// struct, so we build one. Keeping that here means the rest of the project
// never has to know hw-cbmc's option spelling.
struct ElaboratedDesign::Impl {
    cmdlinet cmdline;
    std::vector<std::string> argvStrings;
    std::vector<const char*> argv;
    std::unique_ptr<ui_message_handlert> messageHandler;
    std::optional<transition_systemt> transitionSystem;
    std::string topModule;

    ~Impl() {
        if (!transitionSystem) return;
        std::vector<irept> pending;
        pending.reserve(64);
        auto& system = *transitionSystem;
        releaseIrep(system.trans_expr, pending);
        for (auto it = system.symbol_table.begin(); it != system.symbol_table.end(); ++it) {
            auto& symbol = it.get_writeable_symbol();
            releaseIrep(symbol.value, pending);
            releaseIrep(symbol.type, pending);
            releaseIrep(symbol.location, pending);
        }
    }
};

ElaboratedDesign::ElaboratedDesign(std::unique_ptr<Impl> _impl)
    : impl(std::move(_impl)) {}

ElaboratedDesign::ElaboratedDesign(ElaboratedDesign&&) noexcept = default;
ElaboratedDesign& ElaboratedDesign::operator=(ElaboratedDesign&&) noexcept =
    default;
ElaboratedDesign::~ElaboratedDesign() = default;

// langapi keeps one global registry, and anything that prints an expression
// looks a symbol's mode up in it. Registering nothing does not merely lose
// pretty-printing: it trips an invariant and aborts the process. Only Verilog
// is registered, because that is all this project reads.
static void registerLanguages() {
    static bool done = false;
    if (done) return;
    class MonitorLanguage final : public verilog_languaget {
        bool from_expr(const exprt& expr, std::string& code, const namespacet& ns) override {
            code = describeExpression(expr, ns);
            return false;
        }
    };
    register_language([]() -> std::unique_ptr<languaget> { return std::make_unique<MonitorLanguage>(); });
    done = true;
}

ElaboratedDesign ElaboratedDesign::elaborate(const ElaborateOptions& options) {
    registerLanguages();

    if (options.top.empty())
        throw std::runtime_error("elaborate: a top module is required");
    if (options.files.empty())
        throw std::runtime_error("elaborate: no design files given");

    auto impl = std::make_unique<Impl>();
    impl->topModule = options.top;

    impl->argvStrings.push_back("veval");
    if (options.systemVerilog) impl->argvStrings.push_back("--systemverilog");
    if (options.allowConstantOobRead) impl->argvStrings.push_back("--allow-constant-oob-read");
    impl->argvStrings.push_back("--top");
    impl->argvStrings.push_back(options.top);
    for (const auto& path : options.includePaths) {
        impl->argvStrings.push_back("-I");
        impl->argvStrings.push_back(path);
    }
    for (const auto& define : options.defines) {
        impl->argvStrings.push_back("-D");
        impl->argvStrings.push_back(define);
    }
    for (const auto& file : options.files) impl->argvStrings.push_back(file);

    impl->argv.reserve(impl->argvStrings.size());
    for (const auto& s : impl->argvStrings) impl->argv.push_back(s.c_str());

    const char* spec =
        "(top):(module):I:D:(systemverilog)(reset):(ignore-initial)"
        "(initial-zero)(allow-constant-oob-read)";
    if (impl->cmdline.parse(static_cast<int>(impl->argv.size()),
                            impl->argv.data(), spec))
        throw std::runtime_error("elaborate: could not set up the frontend");
    impl->messageHandler =
        std::make_unique<ui_message_handlert>(impl->cmdline, "veval");
    impl->messageHandler->set_verbosity(options.verbosity);

    config.set(impl->cmdline);

    // The frontend signals failure by throwing a string, a char*, a bare int
    // or an ebmc_errort -- the last of which derives from nothing, so a
    // catch(std::exception) does not see it and the process terminates.
    // Translate them all so callers see one exception type.
    try {
        DesignLanguage languages(impl->cmdline, *impl->messageHandler);
        impl->transitionSystem = languages.transition_system();
    } catch (const ebmc_errort& error) {
        std::ostringstream what;
        what << error.what();
        throw std::runtime_error(
            what.str().empty() ? "module '" + options.top +
                                     "' did not elaborate"
                               : what.str());
    } catch (const std::string& error) {
        throw std::runtime_error(error);
    } catch (const char* error) {
        throw std::runtime_error(error);
    } catch (int) {
        throw std::runtime_error("module '" + options.top +
                                 "' did not elaborate");
    }

    if (!impl->transitionSystem.has_value())
        throw std::runtime_error("module '" + options.top +
                                 "' did not elaborate");

    return ElaboratedDesign(std::move(impl));
}

const transition_systemt& ElaboratedDesign::transitionSystem() const {
    return *impl->transitionSystem;
}

transition_systemt& ElaboratedDesign::transitionSystem() {
    return *impl->transitionSystem;
}

message_handlert& ElaboratedDesign::messageHandler() const {
    return *impl->messageHandler;
}

std::string ElaboratedDesign::top() const {
    return id2string(impl->transitionSystem->main_symbol->name);
}

std::string ElaboratedDesign::topInstance() const {
    return id2string(impl->transitionSystem->main_symbol->name) + '.' +
           impl->topModule;
}

std::string displayName(const std::string& text) {
    static const std::string prefixes[] = {"Verilog::$root.", "Verilog::",
                                           "VHDL::", "SMV::"};

    auto result = text;
    for (const auto& prefix : prefixes) {
        // Keep the historical pass order. A removal can expose a different
        // language prefix for the next pass, but must not rescan to its left.
        // Copy rather than erase: four linear passes, including long terms.
        // Bulk searches also avoid a compare at every byte of ordinary names.
        auto match = result.find(prefix);
        if (match == std::string::npos) continue;
        std::string next;
        next.reserve(result.size());
        std::size_t pos = 0;
        do {
            next.append(result, pos, match - pos);
            pos = match + prefix.size();
            match = result.find(prefix, pos);
        } while (match != std::string::npos);
        next.append(result, pos, std::string::npos);
        result.swap(next);
    }
    return result;
}

}  // namespace frontend
}  // namespace veval
