// Preserve certified same-frame mux SCC relations without JSON/RTLIL round trips.
#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/celltypes.h"
#include "kernel/ffinit.h"
#include "libs/json11/json11.hpp"
#include <fstream>
#include <map>

USING_YOSYS_NAMESPACE
#include "YosysApiCompatibility.h"
PRIVATE_NAMESPACE_BEGIN
using Json = json11::Json;
void need(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
// write_verilog represents a $pmux as a total, side-effect-free function.
// Reading it back creates a process. proc_dlatch follows muxes outside that
// function and mistakes surrounding combinational feedback for local state.
// Recognize only the emitted mux-function shape: one complete switch, every
// arm assigning the same result, and only bound arguments/constants as data.
// Connect that function's result after proc_mux, leaving every other process
// (including genuine latches) to the unchanged upstream implementation.
struct SafeProc : Pass {
    SafeProc() : Pass("consflow_proc", "preserve complete mux functions during process lowering") {}
    void execute(std::vector<std::string> args, Design *design) override {
        veval_yosys::Scope scope(design);
        extra_args(args,1,design);
        dict<RTLIL::Process*,pool<SigBit>> combinational;
        for(auto module:design->selected_modules()) {
            SigMap aliases(module);
            for(auto entry:module->processes) {
                auto process=entry.second;
                if(process->get_bool_attribute(ID::always_latch)||process->get_bool_attribute(ID::always_ff)||
                   process->root_case.switches.size()!=1||process->syncs.size()!=1||
                   process->syncs.front()->type!=RTLIL::SyncType::STa)continue;
                auto sw=process->root_case.switches.front();
                bool valid=!sw->cases.empty(),hasDefault=false;
                pool<SigBit> inputs,result;
                dict<SigBit,SigBit> rootAssignments;
                for(const auto &action:process->syncs.front()->actions)
                    for(auto bit:aliases(action.first)) {
                        if(!bit.wire||bit.wire->name.str().find("$func$")==std::string::npos)valid=false;
                    }
                for(const auto &action:process->root_case.actions) {
                    auto lhs=aliases(action.first),rhs=aliases(action.second);
                    for(int i=0;i<GetSize(lhs);++i) {
                        if(rootAssignments.count(lhs[i]))valid=false;
                        rootAssignments[lhs[i]]=rhs[i];
                        const auto name=lhs[i].wire?lhs[i].wire->name.str():std::string();
                        if(name.find("$func$")!=std::string::npos&&name.find(".$result")==std::string::npos)
                            inputs.insert(rhs[i]);
                    }
                }
                auto input=[&](SigBit bit){return !bit.wire||inputs.count(bit);};
                for(auto bit:aliases(sw->signal))if(!input(bit))valid=false;
                bool first=true;
                for(auto arm:sw->cases) {
                    if(!arm->switches.empty())valid=false;
                    if(arm->compare.empty())hasDefault=true;
                    pool<SigBit> assigned;
                    for(const auto &action:arm->actions) {
                        for(auto bit:aliases(action.first))assigned.insert(bit);
                        for(auto bit:aliases(action.second))if(!input(bit))valid=false;
                    }
                    if(first){result=assigned;first=false;}else if(result!=assigned)valid=false;
                }
                if(!valid||!hasDefault||result.empty())continue;
                for(const auto &action:process->syncs.front()->actions) {
                    auto lhs=aliases(action.first),rhs=aliases(action.second);
                    for(int i=0;i<GetSize(lhs);++i) {
                        auto bit=rhs[i];pool<SigBit> seen;
                        while(rootAssignments.count(bit)&&seen.insert(bit).second)bit=rootAssignments.at(bit);
                        if(!bit.wire||result.count(bit))combinational[process].insert(lhs[i]);
                    }
                }
            }
        }
        for(const auto &pass:{"proc_clean","proc_rmdead","proc_prune","proc_init",
                             "proc_arst","proc_rom","proc_mux"}) Pass::call(design,pass);
        size_t discharged=0;
        for(auto module:design->selected_modules()) {
            SigMap aliases(module);
            FfInitVals initial(&aliases,module);
            for(auto process:module->processes)for(auto sync:process.second->syncs) {
                auto found=combinational.find(process.second);
                if(sync->type!=RTLIL::SyncType::STa||found==combinational.end())continue;
                RTLIL::SigSig remaining;
                for(const auto &assignment:sync->actions) {
                    auto lhs=aliases(assignment.first),rhs=aliases(assignment.second);
                    for(int i=0;i<GetSize(lhs);++i) {
                        if(found->second.count(lhs[i])) {
                            module->connect(lhs[i],rhs[i]);initial.remove_init(lhs[i]);++discharged;
                        } else {remaining.first.append(lhs[i]);remaining.second.append(rhs[i]);}
                    }
                }
                sync->actions.clear();
                if(GetSize(remaining.first))sync->actions.push_back(remaining);
            }
        }
        log("ConsFlow preserved %zu total mux-function bits; no state or assumptions added.\n",discharged);
        // The caller's ordinary prep performs expression optimization. Doing
        // it here as well is costly on wide cyclic mux equations and adds no
        // information needed to bind observation ports.
        for(const auto &pass:{"proc_dlatch","proc_dff","proc_memwr","proc_clean"}) Pass::call(design,pass);
    }
} safeProc;
struct Relations : Pass {
    Relations() : Pass("consflow_relations", "encode checked same-frame mux relations") {}
    void execute(std::vector<std::string> args, Design *design) override {
        veval_yosys::Scope scope(design);
        log_header(design, "ConsFlow same-frame mux relations.\n");
        if (args.size() != 2) log_cmd_error("usage: consflow_relations AUDIT.json\n");
        try {
            const auto marker = veval_yosys::identifier("veval_relation_component");
            for (auto module : design->modules()) for (auto cell : module->cells())
                need(!cell->attributes.count(marker), "pre-existing relation membership attribute");
            Pass::call(design, "scc -select");
            Pass::call(design, "select % t:$pmux %i");
            Pass::call(design, "splitcells");
            // splitcells replaces selected cells with newly named slices.
            // Refresh the selection so pmuxtree also sees those new cells;
            // the old selection still names the now-removed wide $pmux.
            Pass::call(design, "select -clear");
            Pass::call(design, "scc -select");
            Pass::call(design, "select % t:$pmux %i");
            Pass::call(design, "pmuxtree");
            Pass::call(design, "select -clear");
            Pass::call(design, "scc -set_attr veval_relation_component {} -select");
            Pass::call(design, "select *");
            struct Edit { Module *module; Cell *cell; };
            std::vector<Edit> edits;
            CellTypes types; types.setup(design);
            // Validate every component before changing any connection.
            for (auto module : design->modules()) {
                SigMap aliases(module); dict<SigBit, std::vector<Cell *>> drivers;
                pool<SigBit> initialized, inputs;
                for (auto wire : module->wires()) {
                    if (wire->port_input) for (auto bit : aliases(wire)) inputs.insert(bit);
                    if (wire->attributes.count(ID::init)) for (auto bit : aliases(wire)) initialized.insert(bit);
                }
                std::map<std::string, std::vector<Cell *>> groups;
                for (auto cell : module->cells()) {
                    for (const auto &port : cell->connections())
                        if (types.cell_output(cell->type, port.first))
                            for (auto bit : aliases(port.second)) drivers[bit].push_back(cell);
                    if (cell->attributes.count(marker)) groups[cell->attributes.at(marker).as_string()].push_back(cell);
                }
                for (const auto &group : groups) {
                    pool<SigBit> outputs;
                    for (auto cell : group.second) {
                        need(cell->type.in(ID($mux), ID($_MUX_)) && cell->hasPort(ID::Y), "SCC is not a supported mux relation");
                        for (auto bit : aliases(cell->getPort(ID::Y))) {
                            need(!outputs.count(bit), "SCC mux outputs share a driver bit"); outputs.insert(bit);
                        }
                    }
                    for (auto cell : group.second) {
                        const auto y = aliases(cell->getPort(ID::Y));
                        need(cell->connections().size() == 4 && cell->hasPort(ID::A) && cell->hasPort(ID::B) && cell->hasPort(ID::S), "unsupported mux ports");
                        need(GetSize(y) > 0 && GetSize(cell->getPort(ID::A)) == GetSize(y) && GetSize(cell->getPort(ID::B)) == GetSize(y) && GetSize(cell->getPort(ID::S)) == 1, "invalid mux widths");
                        if (cell->type == ID($mux)) need(cell->getParam(ID::WIDTH).as_int() == GetSize(y), "mux parameter width differs");
                        else need(GetSize(y) == 1, "invalid primitive mux width");
                        for (auto bit : aliases(cell->getPort(ID::S))) need(!outputs.count(bit), "SCC selector depends on its outputs");
                        for (auto bit : y) need(bit.wire && !inputs.count(bit) && !initialized.count(bit) && drivers[bit].size() == 1 && drivers[bit].front() == cell, "SCC output has input/init/multiple/constant driver");
                        edits.push_back({module,cell});
                    }
                }
            }
            Json::array generated;
            for (size_t i = 0; i < edits.size(); ++i) {
                auto module=edits[i].module;auto cell=edits[i].cell;
                const auto prefix="veval_relation_"+std::to_string(i);
                for (const auto &suffix:{"_rhs","_valid","_choice","_equality","_constraint"})
                    need(module->count_id(veval_yosys::identifier(prefix+suffix)) == 0, "relation helper name collision");
                const auto original=cell->getPort(ID::Y);const auto width=GetSize(original);
                auto rhs=module->addWire(veval_yosys::identifier(prefix+"_rhs"),width);
                auto valid=module->addWire(veval_yosys::identifier(prefix+"_valid"));
                cell->setPort(ID::Y,rhs);
                auto choice=module->addCell(veval_yosys::identifier(prefix+"_choice"),ID($anyseq));
                choice->setParam(ID::WIDTH,width);choice->setPort(ID::Y,original);
                auto equality=module->addEq(veval_yosys::identifier(prefix+"_equality"),original,rhs,valid);
                auto constraint=module->addAssume(veval_yosys::identifier(prefix+"_constraint"),valid,State::S1);
                const auto gadget=veval_yosys::identifier("veval_encoding_gadget"),relation=veval_yosys::identifier("veval_internal_design_relation");
                for(auto c:{choice,equality,constraint}) {c->attributes[gadget]=Const(1,32);c->attributes[relation]=Const(1,32);}
                for(auto w:{rhs,valid}) {w->attributes[gadget]=Const(1,32);w->attributes[relation]=Const(1,32);generated.emplace_back(veval_yosys::unescaped(w->name));}
            }
            Pass::call(design,"check"); Pass::call(design,"scc -expect 0");
            Json audit=Json::object{{"status",edits.empty()?"no-cycles":"checked"},
                {"internal_design_relation_count",int(edits.size())},{"generated_wire_names",generated},
                {"no_added_temporal_state_or_init",true},{"scc_zero",true},
                {"implementation","native Yosys objects; original formal cells untouched"},
                {"semantic_boundary","same-frame prepared two-state netlist relation; no event-driven four-state equivalence"}};
            std::ofstream out(args[1]);out << audit.dump() << '\n';out.close();
            need(bool(out),"cannot write relation receipt");
            log("ConsFlow encoded %zu internal design equations.\n",edits.size());
        } catch(const std::exception &e) { log_cmd_error("%s\n",e.what()); }
    }
} relations;
PRIVATE_NAMESPACE_END
