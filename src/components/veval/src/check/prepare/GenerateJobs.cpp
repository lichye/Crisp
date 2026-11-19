#include "../../util/task/CommandArgumentsAndFiles.h"
#include "AuditJobEnvironment.h"
#include "JobSchema.h"
#include "util/database/LoweringDatabase.h"
#include "check/prepare/ReadLocalSpec.h"
#include "util/MeasureGeneration.h"
#include "util/process/ParallelFor.h"
#include <charconv>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <set>

namespace veval::verification {
static J positive(const Arguments &a, const std::string &name, J fallback = {}) {
    if (!a.has(name)) return fallback;
    const auto s = a.get(name);
    size_t used = 0;
    const double n = std::stod(s, &used);
    if (used != s.size() || !std::isfinite(n) || n <= 0)
        throw std::runtime_error(name + " must be finite and positive");
    return real(n);
}
static J parseLadder(const std::string &encoded) {
    J ladder = parse(encoded);
    std::set<std::string> names;
    if (!ladder.is_array() || items(ladder).empty()) throw std::runtime_error("ladder must be nonempty");
    for (const auto &rung : items(ladder)) {
        if (!rung.is_array() || items(rung).size() != 4) throw std::runtime_error("ladder rung needs four fields");
        auto it = items(rung).begin();
        const auto rungName = text(*it++);
        const auto mode = text(*it++);
        const auto &depth = *it++;
        const auto &cap = *it;
        if (rungName.empty() ||
            rungName.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos ||
            !names.insert(rungName).second)
            throw std::runtime_error("invalid or repeated rung name");
        if (mode != "prove" && mode != "bmc" && mode != "live") throw std::runtime_error("invalid rung mode");
        // Bounded above too: the adapter converts it to `unsigned'.
        if (!depth.is_number()) throw std::runtime_error("depth must be a positive integer below 2^32");
        const auto depthValue = std::stod(depth.value);
        if (depthValue <= 0 || std::floor(depthValue) != depthValue || depthValue > 4294967295.0)
            throw std::runtime_error("depth must be a positive integer below 2^32");
        if (!cap.is_null()) {
            if (!cap.is_number()) throw std::runtime_error("stage cap must be null or finite and positive");
            const auto capValue = std::stod(cap.value);
            if (!std::isfinite(capValue) || capValue <= 0)
                throw std::runtime_error("stage cap must be null or finite and positive");
        }
    }
    return ladder;
}
static J engine(const Arguments &a, J &defaults) {
    const auto name = a.get("--engine", "sby");
    if (name != "sby" && name != "ebmc" && name != "fake") throw std::runtime_error("unknown engine " + name);
    const auto method = a.get("--method", name == "ebmc" ? "k-induction" : "smtbmc");
    if (!a.has("--method")) append(defaults, str("engine.method=" + method));
    const bool solverless = method == "abc pdr" || method == "aig" || method == "bdd";
    const auto solver = a.get("--solver", solverless ? "none" : name == "ebmc" ? "builtin" : "boolector");
    const J solverValue = (solver == "none" || solver == "null") ? J() : str(solver);
    if (!a.has("--solver"))
        append(defaults, str(solverless ? "engine.solver=null (" + method + " takes no solver)" : "engine.solver=" + solver));
    if (a.has("--memory") && a.has("--no-memory-limit")) throw std::runtime_error("conflicting memory options");
    const J memory = a.has("--no-memory-limit") ? J() : positive(a, "--memory", real(32));
    if (!a.has("--memory") && !a.has("--no-memory-limit")) append(defaults, str("engine.memory_gb=32.0"));
    if (!a.has("--timeout")) append(defaults, str("engine.timeout_s=null (no total limit was requested)"));
    const J ladder = parseLadder(a.get("--ladder", "[[\"prove_d5\",\"prove\",5,null]]"));
    return object({{"name", str(name)}, {"method", str(method)}, {"solver", solverValue},
                   {"ladder", ladder}, {"timeout_s", positive(a, "--timeout")}, {"memory_gb", memory}});
}
static J makeJob(const Product &product, const std::string &label, const J &entry, const J &common) {
    J job = common;
    put(job, "target", object({{"source_entry", entry["source_entry"]}, {"instance", product.head["instance"]},
        {"label", str(label)}, {"module", product.head["module"]}, {"top", product.head["wrapper"]},
        {"sentence", entry["sentence"]}, {"sentence_sha256", str(digest(text(entry["sentence"])))}}));
    put(job, "premises", jobPremises(product, label));
    put(job, "setup", object({{"clock", entry["clock"]}, {"clock_edge", entry["clock_edge"]},
        {"clock_model", entry["clock_model"]}, {"reset", product.reset}}));
    put(job, "job_id", str(identity(job)));
    validateJob(job);
    return job;
}
static int writeJobs(const Arguments &a, const fs::path &out, const J &everything,
                     const J &products, J &skipped) {
    const auto root = a.get("--whole-design"), policy = a.get("--whole-design-policy", "drop-when-below");
    if (policy != "keep" && policy != "drop-when-below") throw std::runtime_error("invalid whole-design policy");
    std::set<std::string> below;
    if (!root.empty() && policy == "drop-when-below")
        for (const auto &job : items(everything))
            if (text(job["target"]["instance"]) != root) below.insert(text(job["target"]["source_entry"]));
    veval::profile::Phase exporting("jobs.export");
    J index = list();
    std::unique_ptr<Database> store;
    if (a.has("--store")) {
        fs::create_directories(out.parent_path());
        store = std::make_unique<Database>(out, !fs::exists(out));
        store->exec("CREATE TABLE IF NOT EXISTS check_templates(job_id TEXT PRIMARY KEY,source_entry TEXT NOT NULL,"
                    "instance TEXT NOT NULL,label TEXT NOT NULL,body TEXT NOT NULL)");
        if (!store->rows("SELECT 1 FROM check_templates LIMIT 1").empty())
            throw std::runtime_error("check templates already exist; use a fresh lowering store");
        store->exec("CREATE INDEX IF NOT EXISTS check_template_source ON check_templates(source_entry,instance)");
        store->begin();
    } else fs::create_directories(out);
    for (const auto &job : items(everything)) {
        const auto &t = job["target"];
        const bool dropRoot = !root.empty() && policy == "drop-when-below" &&
            text(t["instance"]) == root && below.count(text(t["source_entry"]));
        if (dropRoot || (a.has("--instance") && text(t["instance"]) != a.get("--instance"))) {
            J skip = object({{"label", t["label"]}, {"source_entry", t["source_entry"]}, {"instance", t["instance"]}});
            if (dropRoot) {
                put(skip, "kind", str("skipped-by-policy"));
                put(skip, "policy", str(policy));
                put(skip, "reason", str("whole-design instance; monolithic proof is out of scope, and this property lands below it"));
            } else {
                put(skip, "kind", str("not-requested"));
                put(skip, "reason", str("not the requested instance"));
            }
            append(skipped, skip);
            continue;
        }
        const auto id = text(job["job_id"]);
        if (store)
            store->exec("INSERT INTO check_templates VALUES(?,?,?,?,?)",
                        {str(id), t["source_entry"], t["instance"], t["label"], str(canonical(job))});
        else save(out / (id + ".json"), job);
        auto item = object({{"job_id", str(id)}, {"job_id_short", str(id.substr(0,24))},
                            {"source_entry", t["source_entry"]}, {"instance", t["instance"]}, {"label", t["label"]}});
        if (!store) put(item, "file", str(id + ".json"));
        append(index, item);
    }
    auto catalog = object({{"schema", str("consflow-job-index/2")}, {"products", products}, {"jobs", index}, {"skipped", skipped}});
    if (store) {
        put(catalog, "schema", str("consflow-template-index/1"));
        store->meta("check.templates.index", canonical(catalog));
        store->commit();
    } else save(out / "index.json", catalog);
    std::cout << items(index).size() << " jobs, " << items(skipped).size() << " skipped: " << out << '\n';
    return 0;
}
static int jobs(const Arguments &a) {
    veval::profile::Sink profile(a.get("--generation-profile"));
    veval::profile::Phase total("jobs");
    std::size_t threads = 1;
    const auto requested = a.get("--generation-threads", a.get("-j", "1"));
    const auto parsed = std::from_chars(requested.data(), requested.data() + requested.size(), threads);
    if (parsed.ec != std::errc{} || parsed.ptr != requested.data() + requested.size() || !threads)
        throw std::runtime_error("generation threads must be a positive integer");
    if (a.has("--jobs") && a.has("--store"))
        throw std::runtime_error("choose --jobs DIR or --store DATABASE");
    const fs::path source = absolute(a.required("--out"));
    const fs::path out = absolute(a.has("--store") ? a.get("--store") : a.required("--jobs"));
    if (a.all("--design").empty()) throw std::runtime_error("at least one --design is required");
    if (!a.positional.empty()) throw std::runtime_error("unexpected positional arguments");
    J defaults = list(), engineConfig = engine(a, defaults), include = list(), defines = object();
    std::set<std::string> directories;
    for (const auto &s : a.all("--include")) {
        auto path = absolute(s);
        if (!directories.insert(path).second) throw std::runtime_error("duplicate include directory: " + path);
        append(include, str(path));
    }
    for (const auto &s : a.all("--define")) {
        const auto at = s.find('=');
        put(defines, s.substr(0, at), str(at == s.npos ? "" : s.substr(at + 1)));
    }
    std::map<std::string, std::string> digests;
    auto ref = [&](const std::string &role, const fs::path &path) {
        const auto full = absolute(path.string());
        auto [it, inserted] = digests.emplace(full, "");
        if (inserted) it->second = fileDigest(full);
        J owner;
        if (role == "include") {
            size_t i = 0;
            for (const auto &directory : items(include)) {
                if (full.rfind(text(directory) + "/", 0) == 0) { owner = num(i); break; }
                ++i;
            }
        }
        return object({{"role", str(role)}, {"path", str(full)}, {"sha256", str(it->second)}, {"include_dir_index", owner}});
    };
    J sharedInputs = list();
    for (const auto &[option, role] : std::vector<std::pair<std::string,std::string>>{
             {"--design","design"},{"--harness","harness"},{"--include-file","include"}})
        for (const auto &path : a.all(option)) append(sharedInputs, ref(role, path));
    J effective = object({{"path", J()}, {"sha256", J()}});
    if (a.has("--log")) {
        fs::path p = fs::path(absolute(a.get("--log"))) / "effective_config.json";
        if (fs::exists(p)) effective = object({{"path", str(p.string())}, {"sha256", str(fileDigest(p))}});
    }
    const J common = object({{"schema", str("consflow-job/2")},
        {"frontend", object({{"include_order", include}, {"defines", defines},
                              {"systemverilog", boolean(!a.has("--verilog"))}})},
        {"engine", engineConfig},
        {"provenance", object({{"effective_config", effective},
                               {"generator", str("consflow_jobs")}, {"defaults_applied", defaults}})}});
    J everything = list(), skipped = list(), products = list();
    std::vector<fs::path> paths;
    for (auto &e : fs::directory_iterator(source)) {
        const auto name = e.path().filename().string();
        if (name.size() >= 8 && name.substr(name.size() - 8) == "_spec.sv") paths.push_back(e.path());
    }
    std::sort(paths.begin(), paths.end());
    // No Tier-1 sentences means a valid empty Job set.
    const auto workers = util::workerCount(paths.size(), threads);
    std::vector<Files> files(workers);
    std::vector<J> jobsByProduct(paths.size(), list()), skippedByProduct(paths.size(), list());
    veval::profile::Phase construction("jobs.build");
    veval::profile::count("jobs.workers", workers);
    util::parallelFor(paths.size(), threads, [&](std::size_t i, std::size_t worker) {
        const auto& path = paths[i];
        auto& everything = jobsByProduct[i];
        auto& skipped = skippedByProduct[i];
        Product p(path, files[worker]);
        J inputs = sharedInputs;
        append(inputs, object({{"role", str("product")}, {"path", str(absolute(path.string()))},
            {"sha256", str(fileDigest(path))}, {"include_dir_index", J()}}));
        J productJob = common;
        put(productJob, "inputs", inputs);
        for (const auto &range : p.propertyRanges) {
            const auto &label = range.label;
            if (!range.assertion) {
                append(skipped, object({{"label", str(label)}, {"source_entry", p.assumes[label]["source_entry"]},
                    {"instance", p.head["instance"]}, {"kind", str("not-an-obligation")},
                    {"reason", str("premise, not an obligation: the product plants it as an assumption at this instance")}}));
                continue;
            }
            const auto &entry = p.targets[label];
            if (!truth(entry["sentence"])) {
                append(skipped, object({{"label", str(label)}, {"source_entry", entry["source_entry"]},
                    {"instance", p.head["instance"]}, {"reason", str("no SystemVerilog sentence in the product")}}));
                continue;
            }
            if (!truth(entry["source_entry"])) throw std::runtime_error("assertion lacks source entry: " + label);
            append(everything, makeJob(p, label, entry, productJob));
        }
    });
    for (std::size_t i = 0; i < paths.size(); ++i) {
        append(products, str(paths[i].string()));
        for (auto& job : jobsByProduct[i].make_array()) append(everything, std::move(job));
        for (auto& skip : skippedByProduct[i].make_array()) append(skipped, std::move(skip));
    }
    construction.stop();
    if (a.has("--log"))
        annotateEnvironment(absolute(a.get("--log")), everything, sharedInputs,
                            include, defines, !a.has("--verilog"), a.has("--inherit-process-session"));
    return writeJobs(a, out, everything, products, skipped);
}
} // namespace veval::verification
int main(int argc, char **argv) {
    using namespace veval::verification;
    try {
        installCancellationHandlers();
        Arguments a(argc, argv, {"--help", "--verilog", "--no-memory-limit", "--inherit-process-session"},
            {"--out","--log","--design","--harness","--include-file","--jobs","--engine","--solver","--method",
             "--timeout","--memory","--ladder","--include","--define","--binary","--instance","--whole-design-policy","--whole-design","--generation-profile","--store","--generation-threads","-j"});
        if (a.has("--help")) {
            std::cout << "consflow_jobs --out PRODUCTS --design FILE [--design FILE] --jobs DIR\n"
                         "  --store DATABASE  save templates in SQLite instead of per-Job files\n"
                         "  [--engine sby|ebmc] [--method METHOD] [--solver SOLVER]\n"
                         "  [--timeout SECONDS] [--memory GiB | --no-memory-limit]\n"
                         "  [--ladder JSON] [--whole-design INSTANCE --whole-design-policy keep|drop-when-below]\n"
                         "  [--generation-profile PATH]  separate Jobs/environment phase timings\n"
                         "  [-j N | --generation-threads N]  parallel Job preparation (default 1)\n";
            return 0;
        }
        return jobs(a);
    } catch (const std::exception &e) { std::cerr << "consflow_jobs: " << e.what() << '\n'; return 2; }
}
