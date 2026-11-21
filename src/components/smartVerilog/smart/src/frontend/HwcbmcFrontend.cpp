#include "HwcbmcFrontend.h"

#include <ebmc/ebmc_error.h>
#include <ebmc/transition_system.h>
#include <verilog/verilog_bind.h>
#include <verilog/verilog_bits.h>
#include <verilog/verilog_ebmc_language.h>
#include <verilog/verilog_types.h>
#include <verilog/top_level_modules.h>
#include <verilog/verilog_typecheck_base.h>

#include <util/arith_tools.h>
#include <util/cmdline.h>
#include <util/invariant.h>
#include <util/message.h>
#include <util/symbol_table.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>

namespace smart {
namespace frontend {
namespace {

// hw-cbmc talks to us through a message_handlert; we only care about errors,
// and only to put them in the exception text.
class CollectingMessageHandler : public message_handlert
{
public:
    void print(unsigned level, const std::string& message) override
    {
        if(level <= 1)  // M_ERROR
        {
            if(!errors.empty())
                errors += "; ";
            errors += message;
        }
    }

    void print(unsigned level, const std::string& message,
               const source_locationt& location) override
    {
        print(level, location.is_nil() ? message : location.as_string() + ": " + message);
    }

    // Structured output is unused here; hw-cbmc only emits it under
    // --xml-ui / --json-ui, which we never set.
    void print(unsigned, const xmlt&) override {}
    void print(unsigned, const jsont&) override {}

    void flush(unsigned) override {}

    std::string errors;
};

std::string rootInstance(const std::string& top)
{
    return id2string(verilog_module_symbol(verilog_root_module_name())) + "." + top;
}

int widthOf(const typet& type, const std::string& name)
{
    const auto width = verilog_bits_opt(type);
    if(!width || *width <= 0 || *width > std::numeric_limits<int>::max())
        throw std::runtime_error("unsupported width for '" + name + "'");
    return static_cast<int>(width->to_long());
}

// Use EBMC's parsing and hierarchy elaboration before RTL lowering.
class Frontend : public verilog_ebmc_languaget
{
public:
    using verilog_ebmc_languaget::verilog_ebmc_languaget;
    using verilog_ebmc_languaget::parse;

    symbol_tablet elaborate(const parse_treest& trees, const irep_idt& top)
    {
        auto symbols = elaborate_compilation_units(trees);
        elaborate_module_hierarchy(trees, {top}, symbols);
        check_bind_directives(symbols);
        return symbols;
    }
};

bool hasAttribute(const irept& decl, const char* name)
{
    for(const auto& attribute : decl.find(ID_verilog_attributes).get_sub())
    {
        const irept& attribute_name = attribute.find(ID_name);
        if(attribute_name.get(ID_base_name) == name ||
           attribute_name.get(ID_identifier) == name)
            return true;
    }
    return false;
}

bool containsAssume(const irept& irep)
{
    if(irep.id() == ID_verilog_immediate_assume ||
       irep.id() == ID_verilog_assume_property || irep.id() == ID_sva_assume ||
       irep.id() == ID_verilog_smv_assume)
        return true;

    for(const auto& op : irep.get_sub())
        if(containsAssume(op))
            return true;

    for(const auto& named : irep.get_named_sub())
        if(containsAssume(named.second))
            return true;

    return false;
}

std::vector<FreeReg> freeRegs(const verilog_module_sourcet& module,
                             const symbol_tablet& symbols, const std::string& instance)
{
    std::vector<FreeReg> regs;
    for(const auto& item : module.module_items())
    {
        if(item.id() != ID_decl)
            continue;
        const auto& decl = static_cast<const verilog_declt&>(static_cast<const irept&>(item));
        const bool anyseq = hasAttribute(decl, "anyseq");
        if(!anyseq && !hasAttribute(decl, "anyconst"))
            continue;

        for(const auto& declarator : decl.declarators())
        {
            const auto name = id2string(declarator.base_name());
            const auto qualified = instance + "." + name;
            const auto& symbol = symbols.lookup_ref(qualified);
            regs.push_back({anyseq ? "anyseq" : "anyconst", widthOf(symbol.type, qualified), name});
        }
    }
    return regs;
}

ModuleInfo moduleInfo(const verilog_module_sourcet& module, const symbol_tablet& symbols)
{
    ModuleInfo info;
    info.top = id2string(module.base_name());
    const auto instance = rootInstance(info.top);
    const auto& symbol = symbols.lookup_ref(verilog_module_symbol(module.base_name()));
    for(const auto& port : to_module_type(symbol.type).ports())
    {
        const auto name = id2string(port.base_name());
        if(port.ref())
            throw std::runtime_error("unsupported ref port '" + name + "'");
        const auto direction = port.input() && port.output() ? "inout"
                               : port.output() ? "output" : "input";
        info.ports.push_back({direction, widthOf(port.type(), name), name});
    }

    const auto prefix = instance + ".";
    for(const auto& [id, entry] : symbols.symbols)
    {
        const auto name = id2string(id);
        if(!entry.is_macro || name.compare(0, prefix.size(), prefix) != 0 ||
           name.find('.', prefix.size()) != std::string::npos)
            continue;
        if(const auto value = numeric_cast<mp_integer>(entry.value))
        {
            if(*value < std::numeric_limits<long long>::min() ||
               *value > std::numeric_limits<long long>::max())
                throw std::runtime_error("parameter '" + name + "' exceeds SMART's integer range");
            info.params[id2string(entry.base_name)] = value->to_long();
        }
    }

    info.freeRegs = freeRegs(module, symbols, instance);
    info.hasAssume = std::any_of(module.module_items().begin(), module.module_items().end(),
                                [](const auto& item) { return containsAssume(item); });
    return info;
}

}  // namespace

bool declaresModule(const std::string& path, const std::string& name)
{
    std::ifstream in(path);
    if(!in)
        return false;

    const std::string keyword = "module";
    std::string line;
    while(std::getline(in, line))
    {
        for(std::size_t at = line.find(keyword); at != std::string::npos;
            at = line.find(keyword, at + 1))
        {
            const bool startsWord =
                at == 0 || !(std::isalnum((unsigned char)line[at - 1]) ||
                             line[at - 1] == '_');
            if(!startsWord)
                continue;

            std::size_t after = at + keyword.size();
            if(after >= line.size() || !std::isspace((unsigned char)line[after]))
                continue;
            while(after < line.size() && std::isspace((unsigned char)line[after]))
                ++after;

            const std::size_t nameStart = after;
            while(after < line.size() &&
                  (std::isalnum((unsigned char)line[after]) || line[after] == '_'))
                ++after;
            if(after - nameStart == name.size() &&
               line.compare(nameStart, after - nameStart, name) == 0)
                return true;
        }
    }
    return false;
}

namespace {

// Design files to hand to the frontend: the main one, anything the caller
// named, and — unless disabled — its sibling .sv/.v files, which is where
// multi-file designs like the i2c cores keep their submodules.
std::vector<std::string> designFiles(const std::string& path,
                                     const std::string& top,
                                     const HwcbmcOptions& options)
{
    namespace fs = std::filesystem;

    std::vector<std::string> files{path};
    // Compare by identity, not by spelling: "c17.sv" and "./c17.sv" are the
    // same file, and listing it twice makes hw-cbmc see two modules of that
    // name — it then elaborates neither, and every width goes unresolved.
    auto already_listed = [&files](const std::string& candidate) {
        std::error_code error;
        for(const auto& listed : files)
            if(fs::equivalent(listed, candidate, error))
                return true;
        return false;
    };

    for(const auto& extra : options.extraFiles)
        if(!already_listed(extra))
            files.push_back(extra);

    if(options.autoDiscoverSiblings)
    {
        const auto parent = fs::path(path).parent_path();
        std::vector<std::string> siblings;
        std::error_code error;
        for(const auto& entry :
            fs::directory_iterator(parent.empty() ? fs::path(".") : parent, error))
        {
            if(!entry.is_regular_file())
                continue;
            const auto extension = entry.path().extension().string();
            if(extension != ".sv" && extension != ".v")
                continue;
            const auto candidate = entry.path().string();
            if(already_listed(candidate))
                continue;
            // Never pull in a file that redeclares the top module — `smart`
            // writes <top>_assertion.sv next to its input, and a second run in
            // the same directory must not swallow its own previous output.
            if(!top.empty() && declaresModule(candidate, top))
                continue;
            if(top.empty()) {
                std::ifstream sibling(candidate);
                std::string header;
                if(std::getline(sibling, header) && header.rfind("// Generated by SMART ", 0) == 0)
                    continue;
            }
            siblings.push_back(candidate);
        }
        // directory_iterator order is unspecified; keep runs reproducible.
        std::sort(siblings.begin(), siblings.end());
        files.insert(files.end(), siblings.begin(), siblings.end());
    }

    return files;
}

}  // namespace

ModuleInfo parseModuleFile(const std::string& path, const std::string& top,
                           const HwcbmcOptions& options, std::vector<ElaboratedInstance>* instances,
                           std::vector<std::string>* inputs)
{
    CollectingMessageHandler messages;
    cmdlinet command;
    const char* program = "smart";
    command.parse(1, &program, "I:D:(top):(systemverilog)");
    command.set("systemverilog");
    if (!top.empty())
        command.set("top", top);
    command.args = designFiles(path, top, options);
    const auto parent = std::filesystem::path(path).parent_path();
    command.set('I', parent.empty() ? "." : parent.string());
    for(const auto& include : options.includePaths)
        command.set('I', include);
    for(const auto& define : options.defines)
        command.set('D', define);

    Frontend frontend(command, messages);
    cbmc_invariants_should_throwt invariantsThrow;
    try
    {
        const auto trees = frontend.parse();
        const auto tops = top_level_modules(trees, command);
        if (tops.size() != 1) {
            std::string message = "multiple top-level modules";
            for (const auto& name : tops)
                message += " " + id2string(name);
            throw std::runtime_error(message + "; specify top explicitly");
        }
        std::map<irep_idt, const verilog_module_sourcet*> sources;
        for(const auto& tree : trees)
        {
            for(const auto& item : tree.items)
            {
                if(item.id() == ID_verilog_module)
                {
                    const auto& module = to_verilog_module_source(item);
                    sources.emplace(module.base_name(), &module);
                }
            }
        }
        const auto* source = sources.at(tops.front());
        const auto symbols = frontend.elaborate(trees, source->base_name());
        if(instances != nullptr)
        {
            const auto prefix = rootInstance(id2string(source->base_name())) + ".";
            for(const auto& [id, symbol] : symbols.symbols)
            {
                const auto instance = id2string(id);
                if(symbol.value.id() != ID_verilog_module_instance ||
                   instance.compare(0, prefix.size(), prefix) != 0)
                    continue;

                const auto& target = symbols.lookup_ref(
                    to_verilog_module_instance(symbol.value).module_identifier());
                const auto module = sources.find(target.base_name);
                if(module == sources.end())
                    continue;  // Interfaces and checkers are not Verilog modules.
                std::vector<std::string> inputPorts;
                for (const auto& port : to_module_type(target.type).ports())
                    if (port.input() && !port.output())
                        inputPorts.push_back(id2string(port.base_name()));
                instances->push_back({instance.substr(prefix.size()), id2string(target.base_name),
                                      freeRegs(*module->second, symbols, instance), std::move(inputPorts)});
            }
            std::sort(instances->begin(), instances->end(),
                      [](const auto& a, const auto& b) { return a.path < b.path; });
        }
        if (inputs != nullptr)
            *inputs = std::move(command.args);
        return moduleInfo(*source, symbols);
    }
    catch(const ebmc_errort& e)
    {
        if(!e.what().empty())
            messages.print(1, e.what(), e.location());
        throw std::runtime_error(messages.errors.empty() ? "hw-cbmc frontend failed"
                                                         : messages.errors);
    }
    catch(const invariant_failedt& e)
    {
        throw std::runtime_error("hw-cbmc frontend invariant failed: " + std::string(e.what()));
    }
}

}  // namespace frontend
}  // namespace smart
