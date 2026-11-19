// A finite-state recurrence reduction for one GF predicate. A counterexample
// includes an actual nonempty cycle of every design and premise-monitor state.
#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/ff.h"
#include "kernel/celltypes.h"
#include <fstream>
#include <algorithm>
USING_YOSYS_NAMESPACE
#include "YosysApiCompatibility.h"
PRIVATE_NAMESPACE_BEGIN
struct LiveToSafety : public Pass {
    LiveToSafety() : Pass("consflow_live2safety", "reduce one response liveness monitor to recurrence safety") {}
    void help() override {
        log("    consflow_live2safety CLOCK AUDIT.tsv\n");
        log("Requires one flattened module, synchronous state, and one $live cell.\n");
    }
    void execute(std::vector<std::string> args, RTLIL::Design *design) override {
        veval_yosys::Scope scope(design);
        if (args.size() != 3) log_cmd_error("Expected CLOCK and AUDIT.tsv.\n");
        auto module = design->top_module();
        if (!module || design->modules().size() != 1 || !module->processes.empty() || !module->memories.empty())
            log_cmd_error("Liveness reduction requires a fully flattened, process-free, mapped design.\n");
        auto clockWire = module->wire(veval_yosys::identifier(args[1]));
        if (!clockWire || clockWire->width != 1) log_cmd_error("Missing one-bit sampling clock.\n");
        SigMap sigmap(module);
        CellTypes types;
        types.setup();
        const SigSpec clock = sigmap(clockWire);
        std::vector<Cell*> cells;
        for (auto cell : module->cells()) cells.push_back(cell);
        std::sort(cells.begin(), cells.end(), [](Cell* a, Cell* b) { return a->name.str() < b->name.str(); });
        SigSpec state;
        pool<SigBit> seen;
        std::vector<Cell*> live;
        std::vector<std::string> stateCells;
        for (auto cell : cells) {
            if (!types.cell_known(cell->type) || cell->type.in(ID($allconst), ID($allseq), ID($future_ff)))
                log_cmd_error("Unknown or universally quantified cell in liveness reduction: %s.\n", cell->name.str().c_str());
            if (cell->type == ID($live)) { live.push_back(cell); continue; }
            // $check carries the same flavours (live, fair, assert) after an
            // unlowered chformal; refused with the others rather than ignored.
            if (cell->type == ID($fair) || cell->type == ID($assert) || cell->type == ID($check) || cell->type.in(ID($mem), ID($mem_v2), ID($memrd), ID($memwr), ID($meminit)))
                log_cmd_error("Untranslated fairness/assertion/memory cell %s.\n", cell->name.str().c_str());
            SigSpec q;
            if (RTLIL::builtin_ff_cell_types().count(cell->type) || cell->type == ID($anyinit)) {
                FfData ff(nullptr, cell);
                if (ff.has_aload || ff.has_arst || ff.has_sr ||
                    (!ff.has_clk && !ff.has_gclk) ||
                    (ff.has_clk && (!ff.pol_clk || sigmap(ff.sig_clk) != clock)))
                    log_cmd_error("Liveness state is not in the declared posedge sampling domain: %s.\n", cell->name.str().c_str());
                q = sigmap(ff.sig_q);
            } else if (cell->type == ID($anyconst)) {
                q = sigmap(cell->getPort(ID::Y));
            } else if (design->module(cell->type) || cell->type.str().compare(0,4,"$mem") == 0) {
                log_cmd_error("Unflattened or unmapped state %s.\n", cell->name.str().c_str());
            }
            if (!q.empty()) {
                stateCells.push_back(cell->name.str());
                for (auto bit : q) if (bit.wire && !seen.count(bit)) { seen.insert(bit); state.append(bit); }
            }
        }
        if (live.size() != 1 || state.empty()) log_cmd_error("Expected one liveness goal and nonempty monitored state.\n");
        // $live(A, EN) is read as GF(A || !EN) below; that is meant for the
        // one producer, which ties EN to 1. Any other enable is refused.
        if (sigmap(live[0]->getPort(ID::EN)) != SigSpec(State::S1))
            log_cmd_error("Liveness goal %s has a non-constant enable.\n", live[0]->name.str().c_str());
        auto wire = [&](const char* name, int width = 1) {
            IdString id = veval_yosys::identifier(name);
            if (module->wire(id)) log_cmd_error("Reserved liveness signal collision: %s.\n", name);
            auto w = module->addWire(id, width);
            w->attributes[ID::keep] = Const(1);
            return w;
        };
        // $initstate is implicit state even when the design has no explicit
        // start register. Its initial phase must not be repeated as a cycle.
        auto phase = wire("veval_live_phase");
        phase->attributes[ID::init] = Const(0, 1);
        module->addDff(NEW_ID, clock, State::S1, phase);
        state.append(phase);
        stateCells.push_back("explicit initial-state phase");
        auto current = wire("veval_live_current", GetSize(state));
        auto saved = wire("veval_live_saved", GetSize(state));
        auto armed = wire("veval_live_armed");
        auto save = wire("veval_live_save");
        auto ok = wire("veval_live_ok");
        auto loop = wire("veval_live_loop");
        auto noten = module->Not(NEW_ID, live[0]->getPort(ID::EN));
        module->addOr(NEW_ID, live[0]->getPort(ID::A), noten, ok);
        module->connect(current, state);
        auto choice = module->addCell(NEW_ID, ID($anyseq));
        choice->setParam(ID::WIDTH, 1); choice->setPort(ID::Y, save);
        auto bad = module->Not(NEW_ID, ok);
        auto capture = module->And(NEW_ID, module->And(NEW_ID, save, module->Not(NEW_ID, armed)), bad);
        auto nextSaved = module->Mux(NEW_ID, saved, current, capture);
        auto nextArmed = module->And(NEW_ID, bad, module->Or(NEW_ID, armed, save));
        module->addDff(NEW_ID, clock, nextSaved, saved);
        module->addDff(NEW_ID, clock, nextArmed, armed);
        armed->attributes[ID::init] = Const(0, 1);
        auto equal = module->Eq(NEW_ID, current, saved);
        module->addAnd(NEW_ID, module->And(NEW_ID, armed, bad), equal, loop);
        module->addAssert(NEW_ID, module->Not(NEW_ID, loop), State::S1);
        module->remove(live[0]);
        std::ofstream audit(args[2]);
        if (!audit) log_cmd_error("Cannot write liveness reduction audit.\n");
        audit << "schema\tconsflow-live2safety/1\nstate_bits\t" << GetSize(state)
              << "\nclock\t" << args[1] << "\ncycle\tnonempty; full synchronous state; no GF predicate on cycle\n";
        for (const auto& cell : stateCells) audit << "state_cell\t" << cell << '\n';
        for (const auto& signal : {"veval_live_current", "veval_live_saved", "veval_live_armed", "veval_live_save", "veval_live_ok", "veval_live_loop", "veval_live_phase"})
            audit << "generated\t" << signal << '\n';
        log("Reduced response liveness using %d full-state snapshot bits.\n", GetSize(state));
    }
} LiveToSafety;
PRIVATE_NAMESPACE_END
