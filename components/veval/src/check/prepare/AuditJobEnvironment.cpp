#include "AuditJobEnvironment.h"
#include "ProjectEnvironment.h"
#include "../../util/process/RunWithResourceLimits.h"
#include "util/database/LoweringDatabase.h"
#include "util/MeasureGeneration.h"
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace veval::verification {
namespace {
using TargetKey = std::pair<std::string, std::string>;
using Targets = std::map<TargetKey, J>;

std::string quote(const std::string &s) {
    if (s.find_first_of("\r\n") != s.npos)
        throw std::runtime_error("newline in Yosys identifier/path");
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out + '"';
}
std::string csv(const J &j) {
    const auto s = j.is_string() ? text(j) : j.is_null() ? "" : canonical(j);
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + '"';
}
J unavailable(const J &query, const std::string &why) {
    return object({{"property", query["property"]}, {"instance", query["instance"]},
        {"environment_missing", J()}, {"environment_status", str("unavailable")},
        {"environment_path_kind", J()}, {"environment_ports", list()}, {"environment_reason", str(why)}});
}
bool cachedTargetsCover(const fs::path &metadata, const J &analysis, const Targets &targets) {
    if (!fs::exists(metadata)) return false;
    const auto saved = parse(read(metadata));
    if (!equal(saved["analysis"], analysis)) return false;
    std::set<TargetKey> pending;
    for (const auto &[key, target] : targets) pending.insert(key);
    for (const auto &row : items(saved["rows"])) {
        const TargetKey key{text(row["property"]), text(row["instance"])};
        const auto found = targets.find(key);
        if (found != targets.end() && equal(found->second, row["target"])) pending.erase(key);
    }
    return pending.empty();
}
J queriesForRoot(const J &jobs, const std::string &instance) {
    J queries = list();
    std::set<TargetKey> seen;
    const auto prefix = instance + ".";
    for (const auto &job : items(jobs)) {
        const auto &t = job["target"];
        const auto target = text(t["instance"]);
        if (target != instance && target.rfind(prefix, 0) != 0) continue;
        if (!seen.insert({text(t["source_entry"]), target}).second) continue;
        append(queries, object({{"property", t["source_entry"]}, {"instance", t["instance"]},
            {"module", t["module"]}, {"sentence", t["sentence"]},
            {"path", str(target == instance ? "" : target.substr(prefix.size()))}}));
    }
    return queries;
}
std::string auditScript(const std::string &module, const fs::path &dir, const J &analysis) {
    std::string flags = truth(analysis["systemverilog"]) ? " -sv -defer" : " -defer";
    for (const auto &p : items(analysis["includes"])) flags += " -I" + quote(text(p));
    for (const auto &[name, value] : members(analysis["defines"]))
        flags += " -D" + quote(name + (text(value).empty() ? "" : "=" + text(value)));
    std::string script;
    for (const auto &p : items(analysis["inputs"]))
        if (text(p["role"]) == "design")
            script += "read_verilog" + flags + (truth(p["ports_only"]) ? " -lib -nowb" : "") +
                      " " + quote(text(p["path"])) + "\n";
    script += "plugin -i " + quote(helper("consflow_relations.so").string()) + "\n";
    script += "plugin -i " + quote(helper("consflow_environment.so").string()) + "\n";
    static const std::regex moduleName("[A-Za-z_][A-Za-z_0-9.$]*(\\[[0-9]+\\][A-Za-z_0-9.$]*)*");
    if (!std::regex_match(module, moduleName))
        throw std::runtime_error("unsupported exact module identifier: " + module);
    const bool projected = std::any_of(items(analysis["inputs"]).begin(), items(analysis["inputs"]).end(),
                                      [](const J &p) { return truth(p["ports_only"]); });
    script += std::string("hierarchy ") + (projected ? "-check " : "") + "-top " + module + "\nconsflow_environment_prepare " +
              quote((dir / "queries.json").string()) + "\n";
    script += "consflow_environment " + quote((dir / "queries.json").string()) + " " +
              quote((dir / "report.json").string()) + "\n";
    return script;
}
void saveEnvironment(Database &db, const fs::path &log, J &rows, const Targets &targets, const J &analysis) {
    std::map<TargetKey, J> index;
    db.exec("CREATE TABLE IF NOT EXISTS environment_dependencies(entry TEXT,instance TEXT,status TEXT,"
            "missing INTEGER,path_kind TEXT,ports TEXT,reason TEXT,PRIMARY KEY(entry,instance))");
    db.begin();
    try {
        for (const auto &row : items(rows)) {
            index[{text(row["property"]), text(row["instance"])}] = row;
            db.exec("INSERT OR REPLACE INTO environment_dependencies VALUES(?,?,?,?,?,?,?)", {
                row["property"], row["instance"], row["environment_status"],
                row["environment_missing"].is_null() ? J() : num(truth(row["environment_missing"]) ? 1 : 0),
                row["environment_path_kind"], str(canonical(row["environment_ports"])), row["environment_reason"]});
        }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
    for (auto &row : rows.make_array())
        put(row, "target", targets.at({text(row["property"]), text(row["instance"])}));
    save(log / "transport-environment.json", object({{"schema", str("consflow-environment/1")},
        {"analysis", analysis}, {"rows", rows}}));
    std::ostringstream csvOut;
    csvOut << "property,instance,environment_status,environment_missing,environment_path_kind,environment_ports,environment_reason\n";
    for (const auto &row : items(rows)) {
        bool first = true;
        for (auto key : {"property", "instance", "environment_status", "environment_missing",
                         "environment_path_kind", "environment_ports", "environment_reason"}) {
            if (!first) csvOut << ',';
            first = false;
            csvOut << csv(row[key]);
        }
        csvOut << '\n';
    }
    write(log / "transport-environment.csv", csvOut.str());
    if (fs::exists(log / "transport.json"))
        rewriteObjectRows(log / "transport.json", [&](const std::string &key, J &row) {
            if (key != "transports") return;
            const auto found = index.find({text(row["property"]), text(row["instance"])});
            if (found != index.end())
                for (const auto &[field, value] : members(found->second))
                    if (field.rfind("environment_", 0) == 0) put(row, field, value);
        });
}
}
void annotateEnvironment(const fs::path &log, const J &jobs, const J &inputs, const J &includes,
                         const J &defines, bool systemVerilog, bool inheritProcessSession) {
    veval::profile::Phase total("environment.audit");
    const auto database = log / "consflow.sqlite";
    if (!fs::exists(database)) return;
    const J analysis = object({{"inputs", inputs}, {"includes", includes}, {"defines", defines},
        {"systemverilog", boolean(systemVerilog)},
        {"audit_version", num(2)},
        {"environment_plugin", str(fileDigest(helper("consflow_environment.so")))},
        {"relations_plugin", str(fileDigest(helper("consflow_relations.so")))}});
    Targets targets;
    for (const auto &job : items(jobs)) {
        const auto &t = job["target"];
        targets[{text(t["source_entry"]), text(t["instance"])}] = object({
            {"module", t["module"]}, {"sentence", t["sentence"]}});
    }
    // A different strategy may select fewer targets. Reuse structural annotations
    // without rewriting the saved lowering data if all selected targets match.
    if (cachedTargetsCover(log / "transport-environment.json", analysis, targets)) return;
    Database db(database);
    const auto roots = db.rows("SELECT instance,module FROM instances WHERE depth=(SELECT min(depth) FROM instances) ORDER BY instance");
    const auto work = log / "environment";
    fs::create_directories(work);
    J rows = list();
    unsigned group = 0;
    for (const auto &root : roots) {
        const auto instance = text(root["instance"]);
        veval::profile::Phase rootPhase("environment.root", instance);
        const auto queries = queriesForRoot(jobs, instance);
        if (items(queries).empty()) continue;
        const auto dir = work / std::to_string(group++);
        fs::create_directories(dir);
        save(dir / "queries.json", object({{"queries", queries}}));
        try {
            auto projected = projectEnvironmentInputs(db, instance, queries, inputs, dir);
            auto prepared = analysis;
            put(prepared, "inputs", projected);
            write(dir / "audit.ys", auditScript(text(root["module"]), dir, prepared));
            std::string oss = std::getenv("OSS_CAD_BIN") ? std::getenv("OSS_CAD_BIN") : "";
            if (oss.empty() && std::getenv("HOME")) oss = std::string(std::getenv("HOME")) + "/.local/oss-cad-suite/bin";
            const auto run = [&](const fs::path &output) {
              return veval::profile::timed("environment.yosys", [&] {
                return execute({program("yosys", oss), "-Q", "-T", "-s", (dir / "audit.ys").string()},
                    output, fs::current_path(), std::numeric_limits<double>::infinity(), 0, 2, inheritProcessSession);
              });
            };
            auto done = run(dir / "yosys.log");
            if (done.exitCode != 0 && done.cleanupComplete && !equal(projected, inputs)) {
                // Unsupported source constructs keep the established audit;
                // a failed optimization must not hide environment information.
                write(dir / "audit.ys", auditScript(text(root["module"]), dir, analysis));
                veval::profile::count("environment.projection-fallback");
                done = run(dir / "yosys-full.log");
            }
            if (done.exitCode != 0 || !done.cleanupComplete)
                throw std::runtime_error("Yosys environment analysis failed; see " + (dir / "yosys.log").string());
            const auto report = parse(read(dir / "report.json"));
            if (items(report["rows"]).size() != items(queries).size())
                throw std::runtime_error("incomplete environment analysis");
            for (const auto &row : items(report["rows"])) append(rows, row);
        } catch (const Expired &) {
            throw;
        } catch (const std::exception &e) {
            std::cerr << "environment metadata unavailable: " << e.what() << '\n';
            for (const auto &query : items(queries)) append(rows, unavailable(query, e.what()));
        }
    }
    saveEnvironment(db, log, rows, targets, analysis);
}
}
