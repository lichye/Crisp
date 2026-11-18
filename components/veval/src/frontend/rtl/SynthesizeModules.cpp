#include "HwCbmcVerilogSynthesis.h"
#include "HwCbmcVerilogSynthesisClass.h"
#include "util/MeasureGeneration.h"
#include "util/process/ParallelFor.h"
#include <ebmc/ebmc_error.h>
#include <util/symbol_table.h>
#include <algorithm>
#include <functional>
#include <sstream>
#include <unordered_set>

bool verilog_synthesis_parallel(
  symbol_tablet &table, const irep_idt &root, verilog_standardt standard,
  bool ignore_initial, bool initial_zero, message_handlert &messages,
  std::size_t threads)
{
  struct Module {
    irep_idt name;
    std::vector<irep_idt> symbols;
    std::vector<std::size_t> children;
    std::size_t level = 0;
  };
  std::vector<Module> modules;
  std::map<irep_idt, std::size_t> positions;
  std::set<irep_idt> active;
  bool supported = true;
  std::function<std::size_t(irep_idt)> visit = [&](irep_idt name) -> std::size_t {
    if (active.count(name)) throw ebmc_errort{} << "cyclic module hierarchy: " << name;
    const auto known = positions.find(name);
    if (known != positions.end()) return known->second;
    const auto &symbol = table.lookup_ref(name);
    if (symbol.value.id() != ID_verilog_module) { supported = false; return 0; }
    for (const auto &port : to_module_type(symbol.type).ports())
      if (port.type().id() == ID_verilog_module_instance) supported = false;
    active.insert(name);
    Module module{name, {}, {}, 0};
    std::vector<const exprt *> pending{&symbol.value};
    std::unordered_set<const void *> visited;
    while (!pending.empty()) {
      const auto &item = *pending.back();
      pending.pop_back();
      if (!visited.insert(&item.read()).second) continue;
      if (item.id() == ID_inst) {
        for (const auto &instance : to_verilog_inst(item).instances()) {
          const auto child = visit(instance.module_identifier());
          if (!supported) return 0;
          module.children.push_back(child);
          module.level = std::max(module.level, modules[child].level + 1);
        }
      } else {
        for (auto it = item.operands().rbegin(); it != item.operands().rend(); ++it)
          pending.push_back(&*it);
      }
    }
    active.erase(name);
    const auto position = modules.size();
    positions.emplace(name, position);
    modules.push_back(std::move(module));
    return position;
  };
  visit(root);
  if (!supported) {
    veval::profile::count("frontend.synthesis.serial-interface");
    return false;
  }
  std::vector<irep_idt> global;
  std::map<std::string, std::size_t> scopes;
  for (const auto &[name, symbol] : table.symbols) {
    if (symbol.value.id() != ID_verilog_module_instance) continue;
    const auto owner = positions.find(to_verilog_module_instance(symbol.value).module_identifier());
    if (owner != positions.end()) scopes.emplace(id2string(name), owner->second);
  }
  for (const auto &[name, symbol] : table.symbols) {
    const auto module = positions.find(name);
    if (module != positions.end()) {
      modules[module->second].symbols.push_back(name);
      continue;
    }
    // The imported frontend labels most signals with module=$root. Their
    // hierarchical identifier, not that label, determines the writable scope.
    auto prefix = id2string(name);
    bool owned = false;
    while (prefix.find_last_of('.') != std::string::npos) {
      prefix.resize(prefix.find_last_of('.'));
      const auto owner = scopes.find(prefix);
      if (owner != scopes.end()) {
        modules[owner->second].symbols.push_back(name);
        owned = true;
        break;
      }
    }
    if (owned) continue;
    global.push_back(name);
    // External package functions may write their call-frame symbols. Keep
    // that language case on the established serial frontend.
    if (symbol.type.id() == ID_code) supported = false;
  }
  if (!supported) {
    veval::profile::count("frontend.synthesis.serial-package-function");
    return false;
  }
  std::sort(global.begin(), global.end());
  std::vector<std::vector<std::size_t>> levels;
  for (std::size_t i = 0; i < modules.size(); ++i) {
    auto &module = modules[i];
    if (levels.size() <= module.level) levels.resize(module.level + 1);
    levels[module.level].push_back(i);
    std::sort(module.symbols.begin(), module.symbols.end());
  }
  veval::profile::count("frontend.synthesis.modules", modules.size());
  veval::profile::count("frontend.synthesis.levels", levels.size());
  for (const auto &level : levels) {
    // Only this wave's scopes are copied, sharing their immutable expressions.
    // All lookups outside a scope use the read-only, completed dependency table.
    struct Completed { std::unique_ptr<symbol_tablet> table; std::string diagnostics; };
    std::vector<Completed> completed(level.size());
    veval::util::parallelFor(level.size(), threads, [&](std::size_t i, std::size_t) {
      const auto &module = modules[level[i]];
      auto local = std::make_unique<symbol_tablet>();
      for (const auto &name : global) local->add(table.lookup_ref(name));
      for (const auto &name : module.symbols) local->add(table.lookup_ref(name));
      // A parent's port bindings assign the child's input port symbols.
      // Include that boundary, without copying the child's internal state.
      for (const auto child : module.children)
        for (const auto &port : to_module_type(table.lookup_ref(modules[child].name).type).ports())
          local->add(table.lookup_ref(port.identifier()));
      const namespacet ns{*local, table};
      std::ostringstream diagnostic;
      stream_message_handlert handler{diagnostic};
      handler.set_verbosity(messages.get_verbosity());
      verilog_synthesist synthesis{standard, ignore_initial, initial_zero,
                                  ns, *local, module.name, handler};
      try { synthesis.synthesis(); }
      catch (const verilog_synthesist::errort &error) {
        throw ebmc_errort{} << error.what();
      }
      completed[i] = {std::move(local), diagnostic.str()};
    });
    // Publish only after all readers of this dependency snapshot have joined.
    for (auto &result : completed) {
      if (!result.diagnostics.empty()) messages.print(1, result.diagnostics);
      for (auto it = result.table->begin(); it != result.table->end(); ++it) {
        auto &symbol = it.get_writeable_symbol();
        if (std::binary_search(global.begin(), global.end(), symbol.name)) continue;
        if (auto *old = table.get_writeable(symbol.name)) *old = std::move(symbol);
        else table.insert(std::move(symbol));
      }
      result.table.reset();
    }
  }
  return true;
}
