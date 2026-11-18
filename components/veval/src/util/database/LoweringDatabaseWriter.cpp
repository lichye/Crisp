#include "LoweringDatabaseWriter.h"
#include "LoweringDatabase.h"
#include "frontend/rtl/ElaboratedDesign.h"
#include "util/process/ParallelBatches.h"
#include "util/MeasureGeneration.h"
#include <sstream>
#include <unistd.h>
#include <util/format_expr.h>
#include <util/json_irep.h>

namespace veval::result_validation {
void checkDatabaseDestination(const fs::path &log) {
    const auto path = log / "consflow.sqlite";
    if (!fs::exists(path))
        return;
    Database db(path);
    // Old result databases may contain a source ledger. Never overwrite one.
    const bool legacyResults = !db.rows("SELECT 1 FROM sqlite_master WHERE name='qualification_runs'").empty()
        && !db.rows("SELECT 1 FROM qualification_runs LIMIT 1").empty();
    if (legacyResults || !db.rows("SELECT 1 FROM attempts LIMIT 1").empty())
        throw std::runtime_error(
            "lowering database contains verification results; choose a fresh log directory: " +
            path.string());
}
void writeDatabase(const fs::path &log, const lowering::DesignGraph &graph,
                   const std::vector<lowering::PropertyLanding> &transports,
                   const lowering::AssumptionDependencyReport &discharge,
                   const std::vector<lowering::InstanceEdge> &edges,
                   const sva::MonitorSet &sources, std::size_t workers) {
    auto final = fs::absolute(log / "consflow.sqlite");
    checkDatabaseDestination(log);
    auto pending = final.string() + ".pending." + std::to_string(getpid());
    {
        Database db(pending, true);
        // This unpublished file has no readers. Avoid writing the whole export
        // to WAL and copying it back during commit; retain transaction rollback.
        db.exec("PRAGMA journal_mode=DELETE");
        db.exec("PRAGMA cache_size=-65536");
        // SQLite caps helper threads at its own build limit; include the
        // writer itself in the caller's -j budget.
        db.exec("PRAGMA threads=" + std::to_string(workers > 1 ? workers - 1 : 0));
        db.begin();
        try {
            const auto indexes = db.deferIndexes();
            db.meta("log", fs::absolute(log).string());
            db.meta("origin", "native-producer");
            db.meta("graph_schema", "consflow-graph/1");
            db.meta("untouched_records", std::to_string(graph.untouchedRecords));
            Files files;
            J config = files.json(log / "effective_config.json"), front = object();
            for (auto key : {"top", "file_order", "include_paths", "defines", "system_verilog"})
                put(front, key, config["frontend"][key]);
            db.meta("source_frontend", canonical(front));
            // Every source entry this run selected, including those refused
            // before transport: routes exist only for transported entries.
            auto kind = [](bool assumption, bool reset) { return reset ? "reset-contract" : assumption ? "assume" : "assert"; };
            for (auto &m : sources.monitors)
                db.execValues("INSERT OR IGNORE INTO source_properties VALUES(?,?,?,NULL)",
                        {m.property, kind(m.isAssumption, m.isResetContract), "transported"});
            for (auto &u : sources.unsupported)
                db.execValues("INSERT OR IGNORE INTO source_properties VALUES(?,?,?,?)",
                        {u.property, kind(u.isAssumption, u.isResetContract), "refused", u.reason});
            {
                profile::Phase phase("logs.database.nodes");
                util::parallelBatches(graph.nodes.size(), 64, workers,
                    [&](std::size_t begin, std::size_t end) {
                        std::vector<std::vector<std::string>> encoded(end - begin);
                        for (auto i = begin; i < end; ++i)
                            for (const auto &binding : graph.nodes[i].fixedBindings)
                                encoded[i - begin].push_back(canonical(json_irept(false).convert_from_irep(binding)));
                        return encoded;
                    }, [&](std::size_t begin, auto encoded) {
                        for (std::size_t j = 0; j < encoded.size(); ++j) {
                            const auto &n = graph.nodes[begin + j];
                            db.execValues("INSERT INTO instances VALUES(?,?,?,?,?,?)",
                                {n.instance, n.module, std::int64_t(n.depth), n.parent, n.scope, n.environmentProfile});
                            std::int64_t ordinal = 0;
                            for (const auto &p : n.premises)
                                db.execValues("INSERT INTO node_premises VALUES(?,?,?,?,?)",
                                    {n.instance, ordinal++, p.assume, p.arrival, p.grade});
                            ordinal = 0;
                            for (const auto &binding : encoded[j])
                                db.execValues("INSERT INTO fixed_bindings VALUES(?,?,?)", {n.instance, ordinal++, binding});
                        }
                    });
            }
            {
                profile::Phase phase("logs.database.routes");
                std::int64_t row = 0;
                for (auto &r : graph.records) {
                    ++row;
                    db.execValues("INSERT INTO routes VALUES(?,?,?,?,?)", {row, r.entry, r.instance, r.result, r.stopReason});
                    std::int64_t ordinal = 0;
                    for (auto &h : r.route)
                        db.execValues("INSERT INTO route_hops VALUES(?,?,?,?,?,?,?,?)",
                            {row, ordinal++, h.edge, h.kind, h.relation, std::int64_t(h.composite), std::int64_t(h.span), h.detail});
                }
            }
            {
                profile::Phase phase("logs.database.arrivals");
                for (auto &t : transports) {
                    if (!db.execValues("INSERT OR IGNORE INTO transport_landings VALUES(?,?)", {t.property, t.instance})) continue;
                    std::int64_t ordinal = 0;
                    for (const auto &fate : t.fates)
                        if (fate.reached)
                            db.execValues("INSERT INTO arrival_signals VALUES(?,?,?,?)", {t.property, t.instance, ordinal++, fate.parentSignal});
                }
            }
            db.meta("transport_rows", std::to_string(transports.size()));
            for (auto &item : discharge.records) {
                auto &d = item.second;
                db.execValues("INSERT OR REPLACE INTO discharge_records VALUES(?,?,?,?)",
                    {d.property, d.instance, d.classification, std::int64_t(d.grantedOverDriven)});
                db.execValues("DELETE FROM discharge_sites WHERE entry=? AND consumer=?", {d.property, d.instance});
                std::int64_t ordinal = 0;
                for (auto &s : d.discharge)
                    db.execValues("INSERT INTO discharge_sites VALUES(?,?,?,?,?,?)",
                        {d.property, d.instance, ordinal++, s.instance, s.status, s.property});
            }
            for (auto &item : discharge.premiseTier) {
                auto pos = item.first.find('\t');
                const std::string_view key(item.first);
                db.execValues("INSERT OR REPLACE INTO premise_tiers VALUES(?,?,?)",
                    {pos == std::string::npos ? std::string_view{} : key.substr(pos + 1), key.substr(0, pos), item.second});
            }
            {
                profile::Phase phase("logs.database.bindings");
                // The general formatter may construct IR temporaries. Run it
                // before workers borrow the immutable identifier table.
                const auto uncommon = [&] {
                    std::unordered_map<const lowering::PortBinding *, std::string> values;
                    for (const auto &e : edges) for (const auto &b : e.bindings)
                        if (b.kind != lowering::BindingKind::Computed && b.parentExpr.id() != ID_symbol) {
                            std::ostringstream text; text << format(b.parentExpr);
                            values.emplace(&b, frontend::displayName(text.str()));
                        }
                    return values;
                }();
                using BindingText = std::pair<std::string, std::string>;
                util::parallelBatches(edges.size(), 64, workers,
                    [&](std::size_t begin, std::size_t end) {
                        std::vector<std::vector<BindingText>> encoded(end - begin);
                        for (auto i = begin; i < end; ++i) {
                            auto &values = encoded[i - begin]; values.reserve(edges[i].bindings.size());
                            for (const auto &b : edges[i].bindings) {
                                std::string parent;
                                if (b.kind != lowering::BindingKind::Computed)
                                    parent = b.parentExpr.id() == ID_symbol
                                        ? frontend::displayName(id2string(to_symbol_expr(b.parentExpr).get_identifier())) : uncommon.at(&b);
                                values.emplace_back(frontend::displayName(id2string(b.port.get_identifier())), std::move(parent));
                            }
                        }
                        return encoded;
                    }, [&](std::size_t begin, auto encoded) {
                        for (std::size_t j = 0; j < encoded.size(); ++j) {
                            const auto &e = edges[begin + j];
                            db.execValues("INSERT OR REPLACE INTO source_edges VALUES(?,?)", {e.instance, e.module});
                            db.execValues("DELETE FROM source_bindings WHERE instance=?", {e.instance});
                            for (std::size_t k = 0; k < e.bindings.size(); ++k) {
                                const auto &b = e.bindings[k]; const auto &s = encoded[j][k];
                                db.execValues("INSERT INTO source_bindings VALUES(?,?,?,?,?,?)",
                                    {e.instance, std::int64_t(k), s.first, s.second, b.isOutput ? "out" : "in", lowering::toString(b.kind)});
                            }
                        }
                    });
            }
            db.meta("source_bindings_complete", "1");
            db.meta("complete", "1");
            profile::timed("logs.database.indexes", [&] {
                profile::count("logs.database.sort-workers", std::stoull(text(db.rows("PRAGMA threads")[0]["threads"])));
                for (const auto &definition : indexes) db.exec(definition);
            });
            profile::timed("logs.database.commit", [&] { db.commit(); });
        } catch (...) {
            db.rollback();
            throw;
        }
    }
    fs::rename(pending, final);
}
} // namespace veval::result_validation
