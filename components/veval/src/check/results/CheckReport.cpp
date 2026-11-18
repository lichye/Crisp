#include "CheckReport.h"
#include <algorithm>
#include <map>
#include <sstream>

namespace veval::workflow {
void publishReport(Process &process, Journal &journal, Database &db,
                   const J &inventory, const J &index, const std::string &runStatus) {
    // Verdicts belong to checked instances. Grouping by source is navigation
    // only; it does not infer a verdict for the original property.
    std::map<std::string, std::vector<J>> jobs, skips;
    for (const auto &row : items(index["jobs"])) jobs[text(row["source_entry"])].push_back(row);
    for (const auto &row : items(index["skipped"])) skips[text(row["source_entry"])].push_back(row);
    std::map<std::pair<std::string, std::string>, J> environments;
    for (const auto &row : db.rows("SELECT * FROM environment_dependencies"))
        environments[{text(row["entry"]), text(row["instance"])}] = row;
    const bool refusalsKnown = !db.rows("SELECT 1 FROM sqlite_master WHERE name='source_properties'").empty();
    const auto requested = strings(journal.value["configuration"]["properties"]);
    const auto &effective = journal.value["effective"];
    const auto root = text(journal.value["design_under_study"]);
    J rows = list(), counts = object(), checkCounts = object();
    double backendWall = 0, attemptWall = 0;
    std::ostringstream readable;
    readable << "veval check: " << runStatus << "\nResults apply to the checked modules.\n";
    if (!effective.is_null())
        readable << "strategy=" << text(effective["strategy"]) << "  placement="
                 << text(effective["placement"]) << "  method=" << text(effective["method"]) << '\n';
    for (const auto &property : items(inventory["properties"])) {
        const auto entry = text(property["entry"]), kind = text(property["kind"]);
        J attempts = list(), progress = list(), skipped = list(), first;
        for (const auto &skip : skips[entry]) append(skipped, skip);
        const auto problems = db.rows("SELECT job_id,instance,status,result_path,command_ref,first_result_s "
            "FROM problems WHERE source_entry=? ORDER BY instance,job_id", {str(entry)});
        size_t selected = 0, terminal = 0, belowTop = 0;
        bool atTop = false;
        for (const auto &job : jobs[entry]) {
            if (text(job["instance"]) == root) atTop = true;
            else ++belowTop;
        }
        for (const auto &problem : problems) {
            append(progress, problem);
            const auto state = text(problem["status"]);
            if (state == "not-selected") continue;
            ++selected;
            const auto time = problem["first_result_s"];
            if (time.is_number() && (first.is_null() || numeric(time) < numeric(first))) first = time;
            J attempt = object({{"job_id", problem["job_id"]}, {"instance", problem["instance"]},
                {"scope", str("module")}, {"result_ref", problem["result_path"]},
                {"command_ref", problem["command_ref"]}});
            if (!truth(problem["result_path"])) {
                if (state == "adapter-failed") { ++terminal; put(attempt, "outcome", str("error")); }
                else put(attempt, "outcome", str("not-run"));
                put(attempt, "reason", str(state));
            } else {
                ++terminal;
                const auto result = jsonFile(text(problem["result_path"]));
                for (const auto &key : {"outcome", "verdict", "reason", "verdict_reason", "premises", "timing"})
                    put(attempt, key, result[key]);
                put(attempt, "bound", result["evidence"]["bound"]);
                put(attempt, "engine", result["config"]["effective"]);
                backendWall += numeric(result["timing"]["phases"]["backend"]);
                attemptWall += numeric(result["timing"]["elapsed_s"]);
            }
            const auto env = environments.find({entry, text(problem["instance"])});
            if (env != environments.end()) {
                put(attempt, "environment_missing", env->second["missing"].is_null()
                    ? J() : boolean(numeric(env->second["missing"]) != 0));
                put(attempt, "environment_status", env->second["status"]);
                put(attempt, "environment_ports", parse(text(env->second["ports"], "[]")));
            }
            const auto verdict = text(attempt["outcome"]) == "completed"
                ? text(attempt["verdict"], "unknown") : text(attempt["outcome"]);
            put(checkCounts, verdict, num(numeric(checkCounts[verdict]) + 1));
            append(attempts, attempt);
        }
        const bool excluded = !requested.empty() && std::find(requested.begin(), requested.end(),
            entry.substr(entry.rfind('.') + 1)) == requested.end();
        // These are progress states, not source proof claims.
        const std::string status = kind == "assume" ? "assume-only" : excluded ? "not-selected"
            : index.is_null() ? "pending" : jobs[entry].empty() ? "not-generated"
            : !selected ? "not-selected" : terminal == selected ? "checked" : terminal ? "partial" : "pending";
        J row = object({{"entry", str(entry)}, {"kind", str(kind)}, {"scope", str("module")},
            {"status", str(status)}, {"attempts", attempts}, {"landings", attempts},
            {"problems", progress}, {"skipped", skipped}, {"first_result_s", first},
            {"candidates", object({{"below_top", num(belowTop)}, {"top", boolean(atTop)}, {"selected", num(selected)}})}});
        if (status == "not-generated") {
            put(row, "reason", str("no executable local check generated; see skipped reasons"));
            if (refusalsKnown) for (const auto &refusal : db.rows(
                    "SELECT reason FROM source_properties WHERE entry=? AND status='refused'", {str(entry)}))
                put(row, "reason", str("refused before lowering: " + text(refusal["reason"])));
        }
        put(counts, status, num(numeric(counts[status]) + 1));
        readable << entry << "  " << status;
        if (truth(row["reason"])) readable << "  " << text(row["reason"]);
        readable << '\n';
        for (const auto &attempt : items(attempts))
            readable << "  " << text(attempt["instance"]) << ": "
                     << (text(attempt["outcome"]) == "completed" ? text(attempt["verdict"], "unknown")
                         : text(attempt["outcome"])) << '\n';
        append(rows, row);
    }
    const auto totals = db.rows("SELECT count(*) AS total,sum(status='reused') AS reused,"
        "sum(status='unsupported') AS unsupported,sum(status='adapter-failed') AS adapter_failed,"
        "sum(status='not-selected') AS not_selected FROM problems").front();
    save(process.out / "check.json", object({{"schema", str("veval-check-report/2")},
        {"status", str(runStatus)}, {"result_scope", str("module")}, {"properties", rows},
        {"counts", counts}, {"check_counts", checkCounts}, {"problems", totals},
        {"active_s", num(journal.elapsed())}, {"reason", journal.value["reason"]},
        {"source_total", num(items(rows).size())}, {"backend_wall_s", num(backendWall)},
        {"attempt_wall_s", num(attemptWall)}, {"capped", boolean(runStatus == "capped")},
        {"timing_incomplete", journal.value["timing_incomplete"]}, {"configuration", effective}}));
    readable << "sources=" << items(rows).size() << "  jobs=" << numeric(totals["total"])
             << "  reused=" << numeric(totals["reused"]) << "  adapter-failed=" << numeric(totals["adapter_failed"]) << '\n';
    writeText(process.out / "check.txt", readable.str());
    journal.flush();
}
}
