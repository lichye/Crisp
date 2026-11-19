// A resumable experiment queue. Solvers own their declared Job budgets;
// the scheduler adds no trial/restart policy or whole-campaign timeout.
#include "../prepare/JobSchema.h"
#include "../../util/process/RunWithResourceLimits.h"
#include "CheckMemoryAdmission.h"
#include "SelectDeepestBranches.h"
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <csignal>
#include "util/database/LoweringDatabase.h"
#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <set>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#include <poll.h>

namespace veval::verification {
static void need(bool b,const std::string &s){if(!b)throw std::runtime_error(s);}
static double number(const J &j){return j.is_number()?std::stod(j.value):0;}
#ifdef __APPLE__
static pid_t batchOwner = 0;
static bool batchOwnerExited() { return ::getppid() != batchOwner; }
#endif

static J fingerprint(Database &db) {
    std::set<fs::path> paths;
    const auto build=helper("consflow_engine").parent_path();
    for(const auto &name:{"veval","consflow_engine","consflow_sby","consflow_constants","consflow_sva_monitor",
                         "consflow_yosys_identifier","consflow_live_trace","consflow_observation.so",
                         "consflow_relations.so","consflow_live2safety.so","consflow_batch"})paths.insert(build/name);
    for(const auto &env:{"CONSFLOW_SVA_MONITOR","CONSFLOW_CONSTANTS","CONSFLOW_YOSYS_IDENTIFIER","CONSFLOW_OBSERVATION",
                        "CONSFLOW_LIVE2SAFETY","CONSFLOW_LIVE_TRACE","CONSFLOW_RELATIONS"})
        if(auto p=std::getenv(env))paths.insert(fs::path(p));
    // Use the same tool lookup as RunJob, including the default OSS CAD directory.
    const auto home=std::getenv("HOME");
    const std::string ossDefault=std::string(home?home:"")+"/.local/oss-cad-suite/bin";
    const char *oss=std::getenv("OSS_CAD_BIN")?std::getenv("OSS_CAD_BIN"):ossDefault.c_str();
    for(const auto &row:db.rows("SELECT DISTINCT job_path FROM runtime_queue")) {
        const auto engine=parse(read(text(row["job_path"])))["engine"];
        if(text(engine["name"])=="sby") {
            for(const auto &p:{"sby","yosys","yosys-smtbmc"})paths.insert(program(p,oss));
            const auto solver=text(engine["solver"]);
            paths.insert(program(text(engine["method"])=="abc pdr"?"yosys-abc":solver=="yices"?"yices-smt2":solver,oss));
        }else if(auto p=std::getenv("EBMC_BINARY"))paths.insert(p);
        else paths.insert(program("ebmc",oss));
    }
    J result=object();
    for(const auto &p:paths)put(result,absolute(p.string()),fs::is_regular_file(p)?str(fileDigest(p)):J());
    return result;
}
static std::string csv(const J &j) {
    auto s=j.is_null()?"":j.is_string()?text(j):canonical(j);std::string out="\"";
    for(char c:s){if(c=='"')out+='"';out+=c;}return out+'"';
}
static void report(Database &db,const fs::path &out) {
    std::ostringstream table;table<<"arm,entry,instance,job_id,status,outcome,verdict,elapsed_s,peak_memory_kb,result_path\n";
    const auto rows=db.rows("SELECT * FROM runtime_queue ORDER BY arm,entry,depth DESC,instance");
    std::map<std::pair<std::string,std::string>,unsigned> counts;
    for(const auto &row:rows) {
        bool first=true;for(const auto &key:{"arm","entry","instance","job_id","status","outcome","verdict","elapsed_s","peak_memory_kb","result_path"}) {
            if(!first)table<<',';first=false;table<<csv(row[key]);
        }
        table<<'\n';auto bucket=text(row["status"]);
        if(bucket=="complete")bucket=text(row["outcome"])=="completed"?text(row["verdict"]):text(row["outcome"]);
        ++counts[{text(row["arm"]),bucket}];
    }
    write(out/"attempts.csv",table.str());std::ostringstream summary;
    summary<<"Raw engine answers; local refutations are not source conclusions.\n";
    for(const auto &[key,n]:counts)summary<<key.first<<'\t'<<key.second<<'\t'<<n<<'\n';
    write(out/"summary.txt",summary.str());std::cout<<summary.str();
}
static std::vector<J> jobs(const fs::path &dir) {
    const auto index=parse(read(dir/"index.json"));need(text(index["schema"])=="consflow-job-index/2","unknown Job index");
    std::vector<J> out;for(const auto &row:items(index["jobs"])) {
        J job=parse(read(dir/text(row["file"])));validateJob(job);
        need(identity(job)==text(job["job_id"])&&equal(job["job_id"],row["job_id"]),"Job/index identity differs");
        out.push_back(std::move(job));
    }
    return out;
}
static bool ablationPair(const J &full,const J &isolated) {
    for (const auto &key : {"target", "setup", "frontend"})
        if (!equal(full[key], isolated[key])) return false;
    J fullPremises = list(), isolatedPremises = list(), fullInputs = list(), isolatedInputs = list();
    unsigned removed = 0;
    for (const auto &premise : items(full["premises"])) {
        if (text(premise["kind"]) == "transported-assume") ++removed;
        else append(fullPremises, premise);
    }
    for (const auto &premise : items(isolated["premises"])) append(isolatedPremises, premise);
    for (const auto &input : items(full["inputs"]))
        if (text(input["role"]) != "product") append(fullInputs, input);
    for (const auto &input : items(isolated["inputs"]))
        if (text(input["role"]) != "product") append(isolatedInputs, input);
    return removed && equal(fullPremises, isolatedPremises) && equal(fullInputs, isolatedInputs);
}
static void overrideResources(J &job, const Arguments &a) {
    static const std::vector<std::pair<std::string, std::string>> fields{
        {"--timeout", "timeout_s"}, {"--memory", "memory_gb"}};
    for (const auto &[option, field] : fields) {
        if (!a.has(option)) continue;
        size_t used = 0;
        const auto value = a.get(option);
        const auto n = std::stod(value, &used);
#ifdef __APPLE__
        if (option == "--memory") {
            need(used == value.size() && n == 0, "process-tree memory limits are unavailable on macOS; use --memory 0");
            put(job.make_object()["engine"], field, J());
        } else
#endif
        {
            need(used == value.size() && std::isfinite(n) && n > 0,
                 "positive finite resource limit required: " + option + " " + value);
            put(job.make_object()["engine"], field, real(n));
        }
        // The overridden default must no longer appear as an applied default.
        J kept = list();
        const auto prefix = "engine." + field + "=";
        for (const auto &note : items(job["provenance"]["defaults_applied"]))
            if (text(note).rfind(prefix, 0) != 0) append(kept, note);
        if (job["provenance"].is_object()) put(job.make_object()["provenance"], "defaults_applied", kept);
    }
}
static void prepare(Database &db,const Arguments &a,const fs::path &out) {
    const auto arm=a.get("--arm","local"),selection=a.get("--select","deepest");
    need(selection=="deepest"||selection=="root"||selection=="all-depth"||selection=="all","unknown selection");
    need(arm!="ablation"||a.has("--isolation-jobs"),"ablation needs independently generated isolation Jobs");
    std::map<std::string,int> depths;
    std::map<std::string,std::string> parents;
    Database lowering(absolute(a.required("--database")));
    for(const auto &row:lowering.rows("SELECT instance,depth,parent FROM instances")) {
        depths[text(row["instance"])]=number(row["depth"]);
        parents[text(row["instance"])]=text(row["parent"]);
    }
    need(!depths.empty(),"lowering database has no instances");
    int root=std::min_element(depths.begin(),depths.end(),[](const auto &x,const auto &y){return x.second<y.second;})->second;
    // A symeval-derived entry is its source property lowered at one instance;
    // Deepest counts landings per original source, not per derived entry.
    std::map<std::string,std::string> original;
    for(const auto &row:lowering.rows("SELECT DISTINCT derived_entry,source_property FROM symeval_records "
                                    "WHERE derived_entry IS NOT NULL AND derived_entry!=''"))
        original[text(row["derived_entry"])]=text(row["source_property"]);
    auto sourceOf=[&](const std::string &entry){auto it=original.find(entry);return it==original.end()?entry:it->second;};
    auto source=jobs(absolute(a.required("--jobs")));
    std::sort(source.begin(),source.end(),[&](const J &x,const J &y) {
        auto a=text(x["target"]["instance"]),b=text(y["target"]["instance"]);
        need(depths.count(a)&&depths.count(b),"Job instance absent from lowering");
        if(depths[a]!=depths[b])return depths[a]>depths[b];
        if(a!=b)return a<b;
        return text(x["target"]["source_entry"])<text(y["target"]["source_entry"]);
    });
    std::map<std::string,J> isolated;
    if(a.has("--isolation-jobs"))for(const auto &job:jobs(absolute(a.get("--isolation-jobs"))))isolated[canonical(job["target"])]=job;
    std::set<SourceInstance> leaves,selected;
    if(selection=="deepest") {
        for(const auto &job:source) {
            const auto &target=job["target"];
            const auto instance=text(target["instance"]);
            if(depths.at(instance)!=root) leaves.insert({sourceOf(text(target["source_entry"])),instance});
        }
        leaves=deepestBranchLandings(leaves,parents);
    }
    for(auto job:source) {
        const auto target=job["target"];const auto entry=text(target["source_entry"]),instance=text(target["instance"]);
        const auto depth=depths.at(instance);
        // Deepest/All depth omit Top; same-depth instances remain distinct landings.
        if(selection=="root"?depth!=root:selection!="all"&&depth==root)continue;
        const SourceInstance key{selection=="deepest"?sourceOf(entry):entry,instance};
        if(selection=="deepest"&&!leaves.count(key))continue;
        if(!selected.insert(key).second)continue;
        std::string skip;
        if(arm=="ablation") {
            auto it=isolated.find(canonical(target));
            if(it==isolated.end()||!ablationPair(job,it->second))skip="no matched nonempty-Am ablation pair";
            else job=it->second;
        }
        overrideResources(job, a);
        put(job,"job_id",str(identity(job)));validateJob(job);auto id=text(job["job_id"]);
        auto path=out/"jobs"/(id+".json");if(!fs::exists(path))save(path,job);else need(equal(parse(read(path)),job),"saved Job changed");
        const auto prior=db.rows("SELECT job_id FROM runtime_queue WHERE arm=? AND entry=? AND instance=?",{str(arm),str(entry),str(instance)});
        if(!prior.empty())need(text(prior.front()["job_id"])==id,"resumed task configuration changed; use a new output directory");
        db.exec("INSERT OR IGNORE INTO runtime_queue(arm,entry,instance,depth,job_id,job_path,status,reason) VALUES(?,?,?,?,?,?,?,?)",
                {str(arm),str(entry),str(instance),num(depth),str(id),str(path.string()),str(skip.empty()?"pending":"ineligible"),str(skip)});
    }
}
static pid_t startWorker(const fs::path &binary, const J &row, const fs::path &directory,
                         const fs::path &out, int lock) {
    const auto owner = ::getpid();
    const auto pid = ::fork();
    need(pid >= 0, "fork failed");
    if (pid == 0) {
#ifdef __APPLE__
        ::close(lock); // The watchdog owns only its Job lock.
        batchOwner = owner;
        const auto receipt = directory / "supervision.json";
        try {
            save(receipt, object({{"cleanup_confirmed", boolean(false)}}));
            const auto result = execute({binary.string(), "run", "--job", text(row["job_path"]),
                "--out", directory.string(), "--inherit-process-session"},
                directory/"launch.log", out, std::numeric_limits<double>::infinity(),
                0, 5, false, {}, batchOwnerExited);
            save(receipt, object({{"cleanup_confirmed", boolean(result.cleanupComplete)}}));
            ::_exit(result.cleanupComplete ? 0 : 2);
        } catch (...) { ::_exit(2); }
#else
        // Parent death requests normal cancellation so the adapter can reap its solver session.
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (::getppid() != owner) ::_exit(125);
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);
        const int fd = ::open((directory / "launch.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || ::dup2(fd, 1) < 0 || ::dup2(fd, 2) < 0) ::_exit(126);
        ::close(fd);
        ::execl(binary.c_str(), binary.c_str(), "run", "--job", text(row["job_path"]).c_str(),
                "--out", directory.c_str(), nullptr);
        ::_exit(127);
#endif
    }
    return pid;
}
static fs::path terminalResult(const fs::path &directory, const std::string &id) {
    fs::path saved;
    if (!fs::exists(directory / "attempts")) return saved;
    for (const auto &entry : fs::directory_iterator(directory / "attempts")) {
        const auto path = entry.path() / "result.json";
        if (!fs::exists(path)) continue;
        const auto result = parse(read(path));
        if (text(result["identity"]["job_id"]) != id ||
            text(result["reason"]["code"]).rfind("cancelled", 0) == 0) continue;
        need(saved.empty(), "multiple terminal attempts for one task");
        saved = path;
    }
    return saved;
}
static int run(const Arguments &a) {
    need(a.positional.size()==1,"expected prepare, run, or report");const auto action=a.positional.front();
    const fs::path out=absolute(a.required("--out"));fs::create_directories(out);
    int lock=::open((out/"lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);need(lock>=0&&::flock(lock,LOCK_EX|LOCK_NB)==0,"batch already running");
    struct Lock{int fd;~Lock(){::close(fd);}} guard{lock};
    Database db(out/"results.sqlite",!fs::exists(out/"results.sqlite"));
    db.exec("CREATE TABLE IF NOT EXISTS runtime_queue(arm TEXT,entry TEXT,instance TEXT,depth INTEGER,"
            "job_id TEXT,job_path TEXT,status TEXT,reason TEXT,outcome TEXT,verdict TEXT,elapsed_s REAL,"
            "peak_memory_kb INTEGER,result_path TEXT,PRIMARY KEY(arm,entry,instance))");
    if (action == "prepare") prepare(db, a, out);
    if (action == "prepare" || action == "report") {
        report(db, out);
        return 0;
    }
    need(action=="run","unknown batch command");
    auto binary=helper("consflow_engine");const auto runtime=canonical(fingerprint(db)),saved=db.meta("runtime.files");
    need(saved.empty()||saved==runtime,"runtime files changed; start a separate measurement directory");db.meta("runtime.files",runtime);
    const auto arm=a.get("--arm","local");
    Pool pool(a);
#ifdef __APPLE__
    for (const auto &row : db.rows("SELECT job_path FROM runtime_queue WHERE arm=? AND status IN ('pending','running','cancelled')", {str(arm)}))
        need(parse(read(text(row["job_path"])))["engine"]["memory_gb"].is_null(),
             "macOS batch Jobs need no memory limit; prepare with --memory 0");
#endif

    db.exec("CREATE TABLE IF NOT EXISTS runtime_epochs(id INTEGER PRIMARY KEY,arm TEXT,started REAL,"
            "finished REAL,workers INTEGER,pool_bytes INTEGER,peak_bytes INTEGER,status TEXT)");
    db.exec("INSERT INTO runtime_epochs(arm,started,workers,pool_bytes,status) VALUES(?,?,?,?, 'running')",
            {str(arm),num(monotonic()),num(pool.workers),pool.limit?num(pool.limit):J()});
    const auto epoch=db.rows("SELECT last_insert_rowid() AS id").front()["id"];
    struct Worker {J row;fs::path directory;bool cancelled=false;};
    std::map<pid_t,Worker> active;
    auto queue=db.rows("SELECT * FROM runtime_queue WHERE arm=? AND status IN ('pending','running','cancelled') ORDER BY depth DESC,entry,instance",{str(arm)});
    size_t next=0;bool failed=false;std::string error;
    auto adopt=[&](const J &row,const fs::path &resultPath) {
        const auto id=text(row["job_id"]);const auto r=parse(read(resultPath));
        validateShape(r,"result");need(text(r["identity"]["job_id"])==id,"wrong result identity");
        const bool stopped=text(r["reason"]["code"]).rfind("cancelled",0)==0;
        db.begin();db.attempt(r,resultPath,arm);
        db.exec("UPDATE runtime_queue SET status=?,outcome=?,verdict=?,elapsed_s=?,peak_memory_kb=?,result_path=?,reason=? WHERE arm=? AND job_id=?",
            {str(stopped?"cancelled":"complete"),r["outcome"],r["verdict"],r["timing"]["elapsed_s"],
             r["resources"]["peak_memory"]["value_kb"],str(resultPath.string()),r["reason"]["text"],str(arm),str(id)});
        db.commit();
    };
    auto failure=[&](const J &row,const std::string &why) {
        db.rollback();failed=true;error=why;
        db.exec("UPDATE runtime_queue SET status='failed',reason=? WHERE arm=? AND job_id=?",{str(why),str(arm),row["job_id"]});
    };
    // As subreaper, reclaim orphaned descendants before retrying their Jobs.
#ifdef __linux__
    ::prctl(PR_SET_CHILD_SUBREAPER,1);
    auto reapOrphans=[&] {
        const auto self=std::to_string(::getpid());
        for(const auto &entry:fs::directory_iterator("/proc")) {
            const auto name=entry.path().filename().string();
            if(name.empty()||name.find_first_not_of("0123456789")!=std::string::npos)continue;
            std::ifstream stat(entry.path()/"stat");std::string line;if(!std::getline(stat,line))continue;
            const auto close=line.rfind(')');if(close==std::string::npos)continue;
            std::istringstream rest(line.substr(close+1));std::string state,parent;if(!(rest>>state>>parent))continue;
            const int pid=std::stoi(name);
            if(parent!=self||active.count(pid))continue;
            ::kill(-pid,SIGKILL);::kill(pid,SIGKILL);
            int status=0;::waitpid(pid,&status,0);
        }
    };
#endif
    while(next<queue.size()||!active.empty()) {
#ifdef __linux__
        reapOrphans();
#endif
        const bool paused=fs::exists(out/"PAUSE");
        if (cancelled()) for (auto &[pid, worker] : active) {
            if (worker.cancelled) continue;
            ::kill(pid, SIGTERM);
            worker.cancelled = true;
        }
        bool admit = false;
        try {
            pool.sample();
            if (!cancelled() && !failed && !paused && next < queue.size()) {
                std::vector<int> pids;
                for (const auto &entry : active) pids.push_back(entry.first);
                admit = pool.admit(pids);
            }
        } catch (const std::exception &exception) {
            failed = true;
            error = exception.what();
        }
        if(admit) {
            const auto row=queue[next++];const auto id=text(row["job_id"]);
            const auto directory=out/"attempts"/arm/id.substr(0,24);fs::create_directories(directory);
            try {
#ifdef __APPLE__
                // A resumed batch must wait for a previous watchdog to
                // finish cleanup before adopting its Result or retrying.
                const int supervision = ::open((directory/"supervision.lock").c_str(),
                    O_RDWR | O_CREAT | O_CLOEXEC, 0600);
                need(supervision >= 0, "cannot open worker supervision lock");
                struct SupervisionLock {
                    int fd;
                    ~SupervisionLock() { ::close(fd); }
                } supervisionLock{supervision};
                if (::flock(supervision, LOCK_EX | LOCK_NB) != 0) {
                    need(errno == EWOULDBLOCK || errno == EAGAIN, "cannot lock worker supervision");
                    queue.push_back(row);
                    continue;
                }
#endif
#ifdef __APPLE__
                const auto priorReceipt = directory / "supervision.json";
                need(!fs::exists(priorReceipt) || parse(read(priorReceipt))["cleanup_confirmed"].is_true(),
                     "previous worker cleanup could not be confirmed");
#endif
                const auto savedResult = terminalResult(directory, id);
                if (!savedResult.empty()) {
                    adopt(row, savedResult);
                    continue;
                }
                // An earlier adapter for this Job still holds its lock (orphaned by a
                // killed batch): wait for it rather than run the Job twice.
                {
                    const int probe=::open((directory/"adapter.lock").c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);
                    const bool busy=probe>=0&&::flock(probe,LOCK_EX|LOCK_NB)!=0;
                    if(probe>=0)::close(probe);
                    if(busy){queue.push_back(row);continue;}
                }
                db.exec("UPDATE runtime_queue SET status='running' WHERE arm=? AND job_id=?",{str(arm),str(id)});
                std::cout<<arm<<' '<<text(row["entry"])<<" @ "<<text(row["instance"])<<std::endl;
                const auto pid = startWorker(binary, row, directory, out, lock);
                active.emplace(pid,Worker{row,directory});
            }catch(const std::exception &e){failure(row,e.what());}
        }
        for(auto i=active.begin();i!=active.end();) {
            int status=0;const auto w=::waitpid(i->first,&status,WNOHANG);
            if(w==0||(w<0&&errno==EINTR)){++i;continue;}
            try {
                need(w==i->first,"wait failed");
#ifdef __APPLE__
                need(WIFEXITED(status) && WEXITSTATUS(status) == 0, "adapter cleanup could not be confirmed");
#endif
                const auto log = i->second.directory/"launch.log";
                const auto tokens = fs::exists(log) ? words(read(log)) : std::vector<std::string>{};
                if(i->second.cancelled &&
                   (tokens.empty() || !fs::is_regular_file(tokens.back()))) {
                    // Cancellation can precede exec or interrupt the shared
                    // parser before a Result can be published. The native
                    // adapter cleans its solver before reading that output.
                    db.exec("UPDATE runtime_queue SET status='cancelled',reason='cancelled before Result publication' WHERE arm=? AND job_id=?",
                            {str(arm),i->second.row["job_id"]});
                    i=active.erase(i);continue;
                }
                need(!tokens.empty()&&fs::is_regular_file(tokens.back()),"adapter emitted no Result; see "+i->second.directory.string());
                adopt(i->second.row,tokens.back());
            }catch(const std::exception &e){failure(i->second.row,e.what());}
            i=active.erase(i);
        }
        if(active.empty()&&(failed||paused||cancelled()))break;
        ::poll(nullptr,0,pool.workers>1?100:20);
    }
    db.exec("UPDATE runtime_epochs SET finished=?,peak_bytes=?,status=? WHERE id=?",
            {num(monotonic()),pool.limit?num(pool.peak):J(),
             str(failed?"failed":cancelled()?"cancelled":next==queue.size()?"finished":"paused"),epoch});
    if(!error.empty())std::cerr<<error<<'\n';
    report(db,out);return failed?2:cancelled()?130:0;
}
}
int main(int argc,char **argv) {
    using namespace veval::verification;
    try {
        installCancellationHandlers();
        Arguments a(argc,argv,{"--help"},{"--out","--jobs","--database","--select","--arm","--isolation-jobs",
            "--timeout","--memory","--workers","--pool-memory-gb","--admission-gb","--reserve-gb"});
        if(a.has("--help")) {
            std::cout << "consflow_batch prepare --out DIR --jobs DIR --database LOWERING.sqlite "
                         "[--select deepest|root|all-depth|all] [--arm NAME] [--isolation-jobs DIR] "
                         "[--timeout S --memory GiB]\n"
                         "  deepest: the deepest below-top landing on every generated branch; "
                         "all-depth: every below-top landing; root: the design-under-study root; all: every landing\n"
                         "consflow_batch run|report --out DIR [--arm NAME] "
                         "[--workers N --pool-memory-gb N --admission-gb N --reserve-gb N]\n"
                         "Touch DIR/PAUSE to stop admission after the current attempt. Remove it to resume.\n";
#ifdef __APPLE__
            std::cout << "macOS supervises one session per Job. Prepare with --memory 0 and use --workers to limit concurrency.\n";
#endif
            return 0;
        }
        return run(a);
    }catch(const std::exception &e){std::cerr<<"consflow_batch: "<<e.what()<<'\n';return 2;}
}
