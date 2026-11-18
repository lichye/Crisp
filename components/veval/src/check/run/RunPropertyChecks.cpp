#include "RunPropertyChecks.h"
#include "SelectDeepestBranches.h"
#include "AncestorRecheck.h"
#include "../results/CheckReport.h"
#include "../../lowering/LoweringStore.h"
#include "util/process/RunExternalCommand.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace veval::workflow {
void completed(const J &receipt, bool includeStderr) {
    if (text(receipt["status"]) == "timeout" && text(receipt["timeout_scope"]) == "task")
        throw Expired("task deadline exhausted during " + text(receipt["stage"]));
    if (text(receipt["status"]) == "completed") return;
    auto message = text(receipt["stage"]) + " " + text(receipt["status"]) +
                   "; see " + text(receipt["directory"]);
    if (includeStderr) {
        std::ifstream log(fs::path(text(receipt["directory"])) / "stderr.log");
        char buffer[4096];
        log.read(buffer, sizeof(buffer));
        const auto detail = trim(std::string(buffer, static_cast<size_t>(log.gcount())));
        if (!detail.empty()) message += "\n" + detail;
    }
    throw std::runtime_error(message);
}
namespace {
fs::path fresh(const fs::path &parent, const std::string &stem) {
    fs::create_directories(parent);
    size_t n = 0; fs::path path;
    do { path = parent / (stem + "-" + std::to_string(n++)); } while (fs::exists(path));
    return path;
}
void retainIncomplete(const fs::path &path, const fs::path &out) {
    if (fs::exists(path)) fs::rename(path, fresh(out / "incomplete", path.filename().string()));
}
bool retryInterrupted(const J &result) {
    const auto code = text(result["reason"]["code"]);
    return code == "cancelled" || code == "cancelled-before-launch";
}
std::string resultStatus(const J &result, bool reused) {
    const auto outcome = text(result["outcome"]);
    if (outcome == "completed") return reused ? "reused" : "solved";
    if (outcome == "unsupported") return "unsupported";
    if (outcome == "not-run") return "not-run";
    return "adapter-failed";
}
J toolInputs(Process &p, const fs::path &binary) {
    J result = list(); std::set<fs::path> files;
    const auto build = binary.parent_path();
    // Executables needed for task preparation and local SBY checks.
    for (const auto &name : {"veval", "consflow_sva_monitor", "consflow_jobs", "consflow_engine", "consflow_sby",
                             "consflow_constants", "consflow_process", "consflow_yosys_identifier", "consflow_live_trace",
                             "consflow_relations.so", "consflow_observation.so", "consflow_live2safety.so", "consflow_environment.so"}) {
        require(fs::is_regular_file(build / name), "incomplete veval build: missing " + (build / name).string() +
            "; run make build with the development headers for your Yosys installation");
        files.insert(build / name);
    }
    for (const auto &name : {"sby", "yosys", "yosys-smtbmc", "python3"}) files.insert(executable(name));
    for (const auto &name : {"CONSFLOW_SVA_MONITOR", "CONSFLOW_YOSYS_IDENTIFIER", "CONSFLOW_OBSERVATION",
                            "CONSFLOW_LIVE2SAFETY", "CONSFLOW_LIVE_TRACE", "CONSFLOW_JOBS", "CONSFLOW_ENGINE"}) {
        const auto path = veval::util::command_runner::env(name);
        if (!path.empty() && fs::is_regular_file(path)) files.insert(path);
    }
    for (const auto &file : files) append(result, fileIdentity(file, p.out / "integrity"));
    return result;
}
std::map<std::string, std::string> originalEntries(Database &db) {
    std::map<std::string, std::string> original;
    for (const auto &row : db.rows("SELECT DISTINCT derived_entry,source_property FROM symeval_records "
                                  "WHERE derived_entry IS NOT NULL AND derived_entry!=''"))
        original[text(row["derived_entry"])] = text(row["source_property"]);
    return original;
}
J selectJobs(J index, const std::vector<std::string> &requested, Database &db) {
    if (requested.empty()) return index;
    J selected = list();
    const std::set<std::string> wanted(requested.begin(), requested.end());
    const auto original = originalEntries(db);
    for (const auto &row : items(index["jobs"])) {
        auto entry = text(row["source_entry"]);
        if (auto source = original.find(entry); source != original.end()) entry = source->second;
        const auto label = entry.substr(entry.rfind('.') + 1);
        if (wanted.count(label)) append(selected, row);
    }
    put(index, "jobs", selected);
    return index;
}
void checkSnapshot(Database &db, const fs::path &original) {
    db.exec("ATTACH DATABASE ? AS check_snapshot", {str(original.string())});
    try {
        for (const auto &table : {"metadata", "instances", "node_premises", "fixed_bindings", "routes", "route_hops",
            "correspondences", "discharge_records", "discharge_sites", "premise_tiers", "transport_landings",
            "arrival_signals", "symeval_records", "field_certificates", "source_edges", "source_bindings", "environment_dependencies"}) {
            const auto saved = std::string("check_snapshot.") + table, current = std::string("main.") + table;
            for (const auto &pair : {std::make_pair(saved, current), std::make_pair(current, saved)})
                require(db.rows("SELECT 1 AS changed FROM (SELECT * FROM " + pair.first + " EXCEPT SELECT * FROM " + pair.second + ") LIMIT 1").empty(),
                        "database lowering snapshot changed: " + std::string(table));
        }
        db.exec("DETACH DATABASE check_snapshot");
    } catch (...) { db.exec("DETACH DATABASE check_snapshot"); throw; }
}
std::set<std::string> selectChecks(std::vector<J> &queue, Database &db,
                                  const std::map<std::string, uintmax_t> &productSize,
                                  const std::string &strategy, const std::string &whole) {
    std::map<std::string, int> depth;
    std::map<std::string, std::string> parents;
    for (const auto &row : db.rows("SELECT instance,depth,parent FROM instances")) {
        depth[text(row["instance"])] = numeric(row["depth"]);
        parents[text(row["instance"])] = text(row["parent"]);
    }
    auto size = [&](const J &row) { return productSize.at(text(row["job_id"])); };
    std::stable_sort(queue.begin(), queue.end(), [&](const J &a, const J &b) {
        const auto da = depth.at(text(a["instance"])), db = depth.at(text(b["instance"]));
        if (da != db) return da > db;
        if (size(a) != size(b)) return size(a) < size(b);
        return text(a["job_id"]) < text(b["job_id"]);
    });
    // Select local checks from the hierarchy before looking at results.
    // auto includes Top-only properties; deepest/all-depth omit Top.
    std::set<std::string> chosen;
    const auto original = originalEntries(db);
    std::map<verification::SourceInstance, J> byLanding;
    std::set<std::pair<std::string, std::string>> landings;
    for (const auto &row : queue) {
        const auto entry = text(row["source_entry"]), instance = text(row["instance"]);
        if (strategy == "top" && instance != whole) continue;
        if (strategy != "auto" && strategy != "top" && instance == whole) continue;
        if (!landings.insert({entry, instance}).second) continue;
        if (strategy == "top" || strategy == "all-depth") {
            chosen.insert(text(row["job_id"]));
            continue;
        }
        const auto source = original.find(entry);
        auto &best = byLanding[{source == original.end() ? entry : source->second, instance}];
        if (best.is_null() || entry < text(best["source_entry"]))
            best = row;
    }
    std::set<verification::SourceInstance> candidates;
    for (const auto &[landing, row] : byLanding) candidates.insert(landing);
    for (const auto &landing : verification::deepestBranchLandings(candidates, parents))
        chosen.insert(text(byLanding.at(landing)["job_id"]));
    return chosen;
}
void importAttempts(Process &p, Journal &journal, Database &db,
                    const std::map<std::string, J> &jobById, bool reused,
                    const std::string &current = "") {
    std::map<std::string, std::pair<fs::path, J>> selected;
    for (const auto &path : filesUnder(p.out / "attempts")) {
        if (path.filename() != "result.json") continue;
        const auto result = jsonFile(path);
        const auto id = text(result["identity"]["job_id"]);
        auto retainReceipt = [&](const std::string &prefix) {
            const auto stage = prefix + text(result["identity"]["attempt_id"]);
            if (!journal.done(stage)) journal.finish(stage, filesUnder(path.parent_path()));
        };
        if (id.empty() && text(result["outcome"]) != "completed") {
            // A cancellation may precede even request.json. Retain its receipt
            // without claiming a result or preventing a retry.
            require(text(result["schema"]) == "consflow-result/2", "invalid unbound Result schema");
            const auto requestPath = path.parent_path() / "request.json";
            if (fs::exists(requestPath))
                require(jobById.count(text(jsonFile(requestPath)["job_id"])), "unbound attempt has a foreign request");
            retainReceipt("unbound-");
            continue;
        }
        require(jobById.count(id), "foreign Job result in attempts: " + path.string());
        if (!current.empty() && id != current) continue;
        if (retryInterrupted(result)) {
            retainReceipt("cancelled-");
            continue;
        }
        require(text(result["schema"]) == "consflow-result/2" && equal(result["target"], jobById.at(id)["target"]),
                "Result target differs from Job");
        const auto prior = selected.find(id);
        if (prior != selected.end() && text(prior->second.second["outcome"]) == "completed") {
            require(text(result["outcome"]) != "completed", "duplicate completed attempts for one Job");
            continue;
        }
        selected[id] = {path, result};
    }
    for (const auto &[id, attempt] : selected) {
        const auto &[path, result] = attempt;
        const auto saved = db.rows("SELECT result_path FROM problems WHERE job_id=?", {str(id)}).front();
        const bool same = text(saved["result_path"]) == path.string();
        if (!saved["result_path"].is_null() && !same)
            require(text(jsonFile(text(saved["result_path"]))["outcome"]) != "completed", "replacing a completed attempt is forbidden");
        if (!same) {
            if (journal.value["toolchain"].is_null()) put(journal.value, "toolchain", result["toolchain"]);
            journal.finish("result-" + text(result["identity"]["attempt_id"]), filesUnder(path.parent_path()));
            db.exec("UPDATE problems SET status=?,result_path=?,attempt_id=?,first_result_s=? WHERE job_id=?",
                {str(resultStatus(result, reused)),
                 str(path.string()), result["identity"]["attempt_id"],
                 text(result["outcome"]) == "completed" ? num(journal.elapsed()) : J(), str(id)});
        } else if (reused) db.exec("UPDATE problems SET status=? WHERE job_id=?", {str(resultStatus(result, true)), str(id)});
    }
}
} // namespace

int workflow(Process &p, Journal &journal) {
    const auto config = journal.value["configuration"];
    const auto binary = fs::path(text(config["binary"]["path"]));
    const auto databasePath = p.out / "check.sqlite", jobs = p.out / "jobs";
    const auto products = p.out / "transport/products", log = p.out / "transport/log";
    J inventory, index; std::unique_ptr<Database> db; J runId;
    bool snapshotReady = false, reportReady = false;
    auto publish = [&](const std::string &state) {
        // Keep earlier answers if this invocation stops before revalidating them.
        if (!reportReady && fs::exists(p.out / "check.json")) {
            auto previous = jsonFile(p.out / "check.json");
            put(previous, "status", str(state)); put(previous, "reason", journal.value["reason"]);
            put(previous, "active_s", num(journal.elapsed()));
            put(previous, "capped", boolean(state == "capped"));
            put(previous, "previous_snapshot", boolean(true));
            put(previous, "timing_incomplete", journal.value["timing_incomplete"]);
            save(p.out / "check.json", previous);
            writeText(p.out / "check.txt", "veval check: " + state + "\n" + text(journal.value["reason"]) +
                "\nPreviously published answers retained; this invocation did not revalidate them.\n" +
                (fs::exists(p.out / "check.txt") ? readText(p.out / "check.txt") : std::string()));
        } else if (db && snapshotReady) publishReport(p, journal, *db, inventory, index, state);
        else {
            save(p.out / "check.json", object({{"schema", str("veval-check-report/2")}, {"status", str(state)},
                {"reason", journal.value["reason"]}, {"active_s", num(journal.elapsed())}, {"properties", list()}}));
            writeText(p.out / "check.txt", "veval check: " + state + "\n" + text(journal.value["reason"]) + "\n");
        }
    };
    auto closeRun = [&](const std::string &state, const std::string &reason) {
        journal.status(state, reason);
        if (db && !runId.is_null()) db->exec("UPDATE check_runs SET finished_utc=?,status=?,active_s=?,reason=? WHERE run_id=?",
            {journal.value["finished_utc"], str(state), num(journal.elapsed()), str(reason), runId});
        publish(state);
    };
    try {
        Input input;
        const bool savedTask = journal.done("task");
        if (!journal.done("task")) {
            retainIncomplete(p.out / "task", p.out);
            input = prepareTask(p, text(config["source"]), text(config["task"]));
            auto identities = jsonFile(p.out / "task/input-identities.json");
            const auto tools = toolInputs(p, binary);
            for (const auto &identity : items(tools)) append(identities, identity);
            if (!input.solver.empty()) append(identities, fileIdentity(executable(input.solver == "yices" ? "yices-smt2" : input.solver), p.out / "integrity"));
            else append(identities, fileIdentity(executable("yosys-abc"), p.out / "integrity"));
            journal.freezeInputs(identities);
            journal.finish("task", filesUnder(p.out / "task"));
        } else input = restoreInput(jsonFile(p.out / "task/input.json"), p.out);
        if (input.timeout) p.deadline = std::min(p.deadline, monotonic() + input.timeout - journal.elapsed());
        if (savedTask) verifyTask(p, input, text(config["task"]));
        const auto effective = effectiveConfiguration(input, config);
        require(journal.value["effective"].is_null() || equal(journal.value["effective"], effective),
                "effective configuration changed; choose a new --out directory");
        put(journal.value, "effective", effective);
        put(journal.value, "total_budget_s", std::isfinite(p.deadline) ? num(p.deadline - monotonic() + journal.elapsed()) : J());
        journal.flush();
        if (!journal.done("source")) {
            retainIncomplete(p.out / "task/prepared", p.out);
            completed(p.run("source-preparation", {binary.string(), "__check-source",
                (p.out / "task/source-request.json").string(), (p.out / "task/prepared").string()}), true);
            journal.finish("source", filesUnder(p.out / "task/prepared"));
        }
        inventory = jsonFile(p.out / "task/prepared/prepared.json");
        const auto requested=strings(config["properties"]);
        if(!requested.empty()) {
            std::set<std::string> known;
            for(const auto &property:items(inventory["properties"])) {
                const auto entry=text(property["entry"]);
                if(text(property["kind"])=="assert")known.insert(entry.substr(entry.rfind('.')+1));
            }
            for(const auto &label:requested)require(known.count(label),"--property "+label+" names no source assertion");
        }
        require(!config["inprocess_prove"].is_true(),
                "the built-in EBMC solver has been removed; choose a new --out for an SBY check");
        if (!journal.done("transport")) {
            retainIncomplete(p.out / "transport", p.out);
            const auto key=loweringKey(input,inventory,effective);
            const auto bundle=truth(config["lowering"])?fs::path(text(config["lowering"])):
                p.out.parent_path()/".veval-lowering"/key;
            ensureLowering(p,input,inventory,effective,binary,bundle);
            restoreLowering(bundle,p.out/"transport",inventory);
            put(journal.value,"lowering_bundle",str(bundle.string()));
            put(journal.value,"lowering_key",str(key));
            // The Job preparation step adds advisory environment metadata to
            // these two records. Seal products now, metadata when Jobs finish.
            auto immutable = filesUnder(p.out / "transport");
            immutable.erase(std::remove_if(immutable.begin(),immutable.end(),[&](const fs::path &file){
                return file == log / "consflow.sqlite" || file == log / "consflow.sqlite-wal" ||
                    file == log / "consflow.sqlite-shm" || file == log / "consflow.sqlite-journal" ||
                    file == log / "transport.json";
            }),immutable.end());
            journal.finish("transport", immutable);
        }
        const auto roots = [&] { Database generated(log/"consflow.sqlite"); return generated.rows("SELECT instance FROM instances WHERE depth=(SELECT min(depth) FROM instances) ORDER BY instance"); }();
        require(roots.size() == 1, "check requires one design instance directly below the source boundary");
        auto whole = text(config["whole_design"]);
        if (whole.empty()) whole = text(roots.front()["instance"]);
        require(whole == text(roots.front()["instance"]), "whole-design differs from lowering root");
        if (!journal.done("jobs")) {
            retainIncomplete(jobs, p.out);
            std::vector<std::string> command{nativeHelper(binary, "consflow_jobs", "CONSFLOW_JOBS"),
                "--out", products.string(), "--log", log.string(), "--jobs", jobs.string(), "--engine", "sby",
                "--method", input.method, "--solver", input.solver.empty() ? "none" : input.solver,
                "--no-memory-limit", "--ladder", canonical(ladderJson(input)),
                "--whole-design", whole, "--whole-design-policy", "keep", "--binary", binary.string()};
#ifdef __APPLE__
            command.push_back("--inherit-process-session");
#endif
            for (const auto &path : strings(inventory["design"])) command.insert(command.end(), {"--design", path});
            completed(p.run("jobs", command));
            {
                Database generated(log/"consflow.sqlite",false,true);
                const auto all=jsonFile(jobs/"index.json");
                const auto selected=selectJobs(all,strings(config["properties"]),generated);
                std::set<std::string> retained;
                for(const auto &row:items(selected["jobs"]))retained.insert(text(row["file"]));
                for(const auto &row:items(all["jobs"]))if(!retained.count(text(row["file"])))fs::remove(jobs/text(row["file"]));
                save(jobs/"index.json",selected);
            }
            auto artifacts=filesUnder(jobs);
            artifacts.push_back(log/"consflow.sqlite");
            if(fs::exists(log/"transport.json"))artifacts.push_back(log/"transport.json");
            for(const auto &name:{"environment","transport-environment.json","transport-environment.csv"}){
                auto additions=filesUnder(log/name);artifacts.insert(artifacts.end(),additions.begin(),additions.end());
            }
            journal.finish("jobs", artifacts);
        }
        if (!fs::exists(databasePath)) {
            const auto snapshot = p.out / "check.sqlite.pending";
            retainIncomplete(snapshot, p.out);
            Database original(log / "consflow.sqlite"); original.exec("VACUUM INTO ?", {str(snapshot.string())});
            fs::rename(snapshot, databasePath);
        }
        db = std::make_unique<Database>(databasePath); checkSnapshot(*db, log / "consflow.sqlite"); db->checkSchema();
        snapshotReady = true;
        db->exec("UPDATE check_runs SET status='interrupted',reason='recovered by a later invocation' WHERE status='running'");
        J toolCommit;
        // Installed binaries need no source checkout. Revision is optional metadata.
        if (fs::exists(p.root / ".git")) {
            fs::create_directories(p.out / "integrity");
#ifdef __APPLE__
            const auto revision = p.run("tool-revision", {"git", "-C", p.root.string(), "rev-parse", "HEAD"});
            completed(revision);
            toolCommit = str(trim(readText(fs::path(text(revision["directory"])) / "stdout.log")));
#else
            toolCommit = str(trim(veval::util::command_runner::query(
                {"git", "-C", p.root.string(), "rev-parse", "HEAD"}, p.out / "integrity/git.log", 0, true)));
#endif
        }
        db->exec("INSERT INTO check_runs(started_utc,command,inputs_sha256,tool_commit,status,effective_config) VALUES(?,?,?,?,?,?)",
            {std::prev(items(journal.value["epochs"]).end())->operator[]("started_utc"), str(canonical(config)),
             str(hashFile(p.out / "task/input-identities.json", p.out / "integrity")),
             toolCommit, str("running"),
             str(canonical(effective))});
        runId = db->rows("SELECT last_insert_rowid() AS id").front()["id"];
        index = jsonFile(jobs / "index.json");
        std::map<std::string, J> jobById;
        std::map<std::string, uintmax_t> productSize;
        std::vector<J> queue(items(index["jobs"]).begin(), items(index["jobs"]).end());
        for (const auto &row : queue) {
            auto job = jsonFile(jobs / text(row["file"])); auto id = text(row["job_id"]);
            require(text(job["job_id"]) == id, "Job/index identity differs"); jobById[id] = job;
            for (const auto &file : items(job["inputs"]))
                if (text(file["role"]) == "product") productSize[id] += fs::file_size(text(file["path"]));
            const auto target = job["target"];
            db->exec("INSERT OR IGNORE INTO problems(job_id,source_entry,instance,module,sentence_sha256,context,engine,budget_s,memory_gb,status,first_run_id) VALUES(?,?,?,?,?,?,?,?,?,'pending',?)",
                {str(id), target["source_entry"], target["instance"], target["module"], target["sentence_sha256"],
                 str("module:" + text(target["instance"])), str(canonical(job["engine"])), J(), J(), runId});
        }
        auto chosen = selectChecks(queue, *db, productSize, text(effective["strategy"]), whole);
        const auto original = originalEntries(*db);
        auto sourceOf = [&](const J &row) {
            const auto source = text(row["source_entry"]);
            const auto mapped = original.find(source);
            return mapped == original.end() ? source : mapped->second;
        };
        std::map<std::string, std::string> parents;
        std::map<verification::SourceInstance, std::string> ancestorJobs;
        if (text(effective["strategy"]) == "auto") {
            for (const auto &row : db->rows("SELECT instance,parent FROM instances"))
                parents[text(row["instance"])] = text(row["parent"]);
            for (const auto &row : queue) {
                const verification::SourceInstance landing{sourceOf(row), text(row["instance"])};
                const auto prior = ancestorJobs.find(landing);
                if (prior == ancestorJobs.end() || text(row["source_entry"]) < text(jobById.at(prior->second)["target"]["source_entry"]))
                    ancestorJobs[landing] = text(row["job_id"]);
            }
        }
        put(journal.value, "design_under_study", str(whole));
        for (const auto &row : queue) if (!chosen.count(text(row["job_id"])))
            db->exec("UPDATE problems SET status='not-selected' WHERE job_id=? AND result_path IS NULL AND status!='adapter-failed'", {row["job_id"]});
        journal.flush();
        // Cancellation is incomplete work. Saved unsupported/error outcomes
        // are terminal; resume does not implicitly retry them.
        for (const auto &row : db->rows("SELECT job_id,result_path FROM problems WHERE result_path IS NOT NULL")) {
            if (retryInterrupted(jsonFile(text(row["result_path"]))))
                db->exec("UPDATE problems SET status='pending',result_path=NULL,attempt_id=NULL WHERE job_id=?", {row["job_id"]});
        }
        importAttempts(p, journal, *db, jobById, true);
        auto recheck = [&](const J &row) {
            if (ancestorJobs.empty()) return;
            const auto saved = db->rows("SELECT result_path FROM problems WHERE job_id=?", {row["job_id"]}).front();
            if (!truth(saved["result_path"])) return;
            const auto result = jsonFile(text(saved["result_path"]));
            const auto next = verification::ancestorRecheck({sourceOf(row), text(row["instance"])},
                text(result["outcome"]), text(result["verdict"]), ancestorJobs, parents, whole);
            if (next.empty() || !chosen.insert(next).second) return;
            db->exec("UPDATE problems SET status='pending' WHERE job_id=? AND status='not-selected'", {str(next)});
            std::cout << "recheck " << sourceOf(row) << " at " << text(jobById.at(next)["target"]["instance"])
                      << " after counterexample at " << text(row["instance"]) << std::endl;
        };
        reportReady = true; publish("running");
        for (const auto &row : queue) {
            const auto id = text(row["job_id"]);
            if (!chosen.count(id)) continue;
            const auto saved = db->rows("SELECT * FROM problems WHERE job_id=?", {str(id)}).front();
            if (!saved["result_path"].is_null() || text(saved["status"]) == "adapter-failed") {
                recheck(row);
                continue;
            }
            require(!interrupted(), "verification interrupted");
            db->exec("UPDATE problems SET status='running' WHERE job_id=?", {str(id)});
            journal.flush();
            std::cout << "check " << text(row["source_entry"]) << " at " << text(row["instance"]) << std::endl;
            std::vector<std::string> attempt{nativeHelper(binary, "consflow_engine", "CONSFLOW_ENGINE"),
                "run", "--job", (jobs / text(row["file"])).string(), "--out", (p.out / "attempts").string()};
#ifdef __APPLE__
            attempt.push_back("--inherit-process-session");
#endif
            auto receipt = p.run("attempt", attempt);
            db->exec("UPDATE problems SET command_ref=? WHERE job_id=?", {str((fs::path(text(receipt["directory"])) / "command.json").string()), str(id)});
            journal.finish("command-" + fs::path(text(receipt["directory"])).filename().string(),
                           filesUnder(text(receipt["directory"])));
            importAttempts(p, journal, *db, jobById, false, id);
            if (interrupted() || text(receipt["status"]) == "interrupted") {
                db->exec("UPDATE problems SET status='pending' WHERE job_id=? AND result_path IS NULL", {str(id)});
                throw std::runtime_error("verification interrupted");
            }
            if (text(receipt["status"]) == "timeout" && text(receipt["timeout_scope"]) == "task") {
                db->exec("UPDATE problems SET status='pending' WHERE job_id=? AND result_path IS NULL", {str(id)});
                throw Expired("task deadline exhausted during attempt");
            }
            if (db->rows("SELECT result_path FROM problems WHERE job_id=?", {str(id)}).front()["result_path"].is_null()) {
                db->exec("UPDATE problems SET status='adapter-failed' WHERE job_id=?", {str(id)});
            }
            recheck(row);
            publish("running");
        }
        journal.verify(journal.value["inputs"]);
        const auto failures = numeric(db->rows("SELECT count(*) AS count FROM problems WHERE status='adapter-failed'").front()["count"]);
        closeRun(failures ? "finished-with-errors" : "finished", failures ? "adapter failures retained; independent Jobs completed" : "");
        return failures ? 2 : 0;
    } catch (const Expired &e) {
        if (db && snapshotReady) db->exec("UPDATE problems SET status='pending' WHERE status='running' AND result_path IS NULL");
        closeRun("capped", e.what()); std::cerr << "veval check: " << e.what() << '\n'; return 2;
    } catch (const std::exception &e) {
        closeRun(interrupted() ? "interrupted" : "failed", e.what());
        std::cerr << "veval check: " << e.what() << '\n'; return interrupted() ? 130 : 2;
    }
}
}
