// Check saved lowering products without running generation again.
#include "util/process/CurrentExecutablePath.h"
#include "check/prepare/JobSchema.h"
#include "check/run/SelectDeepestBranches.h"
#include "../util/process/RunWithResourceLimits.h"
#include "util/cli/CheckCommands.h"
#include "util/task/TaskInputAndExecution.h"
#include "util/task/RunProgressLog.h"
#include "util/database/LoweringDatabase.h"
#include "util/process/RunExternalCommand.h"
#include <algorithm>
#include <cmath>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <set>
#include <sstream>
#include <sys/file.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>

namespace veval::verification {
static void need(bool b,const std::string &s){if(!b)throw std::runtime_error(s);}
static double positive(const std::string &s) {
    size_t n=0;const auto v=std::stod(s,&n);
    need(n==s.size()&&std::isfinite(v)&&v>0,"expected positive finite number: "+s);return v;
}
static std::string csv(const J &j) {
    std::string out="\"",s=j.is_null()?"":j.is_string()?text(j):canonical(j);
    for(char c:s){if(c=='"')out+='"';out+=c;}return out+'"';
}
static void report(Database &db,const fs::path &out) {
    const std::vector<std::string> columns{"source_entry","instance","label","selection","method","status","outcome","verdict",
        "raw_status","reason","wall_s","engine_s","peak_bytes","timeout_s","memory_bytes","bundle_key","template_id","job_id"};
    std::ostringstream s;for(size_t i=0;i<columns.size();++i)s<<(i?",":"")<<columns[i];s<<'\n';
    for(const auto &row:db.rows("SELECT * FROM checks ORDER BY source_entry,instance")) {
        for(size_t i=0;i<columns.size();++i)s<<(i?",":"")<<csv(row[columns[i]]);s<<'\n';
    }
    write(out/"results.csv",s.str());
}
static J ladder(const Arguments &a) {
    if(a.get("--method","kind")=="pdr")return list({list({str("pdr"),str("prove"),num(1),J()})});
    std::istringstream in(a.get("--depths","5,10,20"));std::string item;J result=list();std::set<unsigned> seen;
    while (std::getline(in,item,',')) {
        const auto value=positive(item);
        need(value<=1000000&&std::floor(value)==value,"invalid induction depth");
        const unsigned k=value;
        need(seen.insert(k).second,"duplicate induction depth");
        append(result,list({str("prove_d"+item),str("prove"),num(k),J()}));
    }
    need(!items(result).empty(),"empty induction ladder");
    return result;
}
static std::vector<J> selectTemplates(Database &bundle, const std::string &top,
                                     const std::string &selection, const std::vector<std::string> &requested) {
    const auto rows=bundle.rows(
        "SELECT t.job_id AS template_id,t.source_entry,t.instance,t.label,i.depth "
        "FROM check_templates t JOIN instances i ON i.instance=t.instance "
        "ORDER BY i.depth DESC,t.instance,t.source_entry,t.job_id");
    std::map<std::string,std::string> original;
    for (const auto &row:bundle.rows("SELECT DISTINCT derived_entry,source_property FROM symeval_records "
                                    "WHERE derived_entry IS NOT NULL AND derived_entry!=''"))
        original[text(row["derived_entry"])]=text(row["source_property"]);
    const std::set<std::string> wanted(requested.begin(),requested.end());
    std::set<std::string> found;
    std::set<SourceInstance> chosen;
    std::vector<J> queue;
    for (const auto &row:rows) {
        auto source=text(row["source_entry"]);
        const auto mapped=original.find(source);
        if (mapped!=original.end()) source=mapped->second;
        const auto label=source.substr(source.rfind('.')+1);
        if (wanted.count(source)) found.insert(source);
        if (wanted.count(label)) found.insert(label);
        if (!wanted.empty()&&!wanted.count(source)&&!wanted.count(label)) continue;
        const bool atTop=text(row["instance"])==top;
        if (selection=="top"&&!atTop) continue;
        if (selection!="top"&&selection!="all"&&atTop) continue;
        if (selection=="deepest"&&!chosen.insert({source,text(row["instance"])}).second) continue;
        queue.push_back(row);
    }
    if (selection=="deepest") {
        std::map<std::string,std::string> parents;
        for (const auto &row:bundle.rows("SELECT instance,parent FROM instances"))
            parents[text(row["instance"])]=text(row["parent"]);
        const auto leaves=deepestBranchLandings(chosen,parents);
        queue.erase(std::remove_if(queue.begin(),queue.end(),[&](const J &row) {
            auto source=text(row["source_entry"]);
            const auto mapped=original.find(source);
            if (mapped!=original.end()) source=mapped->second;
            return !leaves.count({source,text(row["instance"])});
        }),queue.end());
    }
    for (const auto &property:requested)
        need(found.count(property),"property has no encodable Job: "+property);
    return queue;
}

static J runtimeFingerprint(const std::string &method, const std::string &configuredSolver) {
    J runtime=object();
    for (const auto &name:{"consflow_engine","consflow_sby","consflow_constants","consflow_sva_monitor",
                          "consflow_yosys_identifier","consflow_relations.so","consflow_observation.so",
                          "consflow_live2safety.so","consflow_live_trace"})
        put(runtime,name,str(fileDigest(helper(name))));
    for (const auto &name:{"CONSFLOW_SVA_MONITOR","CONSFLOW_CONSTANTS","CONSFLOW_YOSYS_IDENTIFIER",
                          "CONSFLOW_OBSERVATION","CONSFLOW_LIVE2SAFETY","CONSFLOW_LIVE_TRACE","CONSFLOW_RELATIONS"})
        if (const auto value=std::getenv(name)) put(runtime,name,str(fileDigest(value)));
    const auto oss=std::getenv("OSS_CAD_BIN")?std::getenv("OSS_CAD_BIN"):
        (std::string(std::getenv("HOME")?std::getenv("HOME"):"")+"/.local/oss-cad-suite/bin");
    for (const auto &name:{"sby","yosys","yosys-smtbmc"}) put(runtime,name,str(fileDigest(program(name,oss))));
    const auto solver=method=="pdr"?"yosys-abc":configuredSolver=="yices"?"yices-smt2":configuredSolver;
    put(runtime,solver,str(fileDigest(program(solver,oss))));
    return runtime;
}

static J materialize(Database &bundle,J job,const fs::path &root,const fs::path &work,const J &engine) {
    const auto original=bundle.meta("bundle.base");
    for(auto &input:job.make_object()["inputs"].make_array()) {
        const auto path=text(input["path"]);
        if(text(input["role"])=="product") {
            const auto rel=fs::path(path).lexically_relative(fs::path(original)/".generate");
            need(!rel.empty()&&*rel.begin()!="..","product is outside lowering bundle");
            const auto rows=bundle.rows("SELECT body FROM bundle_files WHERE path=?",{str(rel.string())});
            need(rows.size()==1,"missing local specification in lowering store");
            const auto dest=work/"product.sv";write(dest,text(rows.front()["body"]));
            need(fileDigest(dest)==text(input["sha256"]),"stored specification hash mismatch");
            put(input,"path",str(dest.string()));
        }else {
            const auto rel=fs::path(path).lexically_relative(original);
            need(!rel.empty()&&*rel.begin()!="..","shared input is outside lowering bundle");
            put(input,"path",str((root/rel).string()));
        }
    }
    auto &provenance=job.make_object()["provenance"];
    // The archived generating configuration is not an engine input.
    put(provenance,"effective_config",object({{"path",J()},{"sha256",J()}}));
    put(provenance,"defaults_applied",list());
    put(job,"engine",engine);put(job,"job_id",str(identity(job)));validateJob(job);
    return job;
}
static J readCheckResult(const fs::path &work, const J &job, const J &receipt) {
    J result,raw;
    for(const auto &file:veval::workflow::filesUnder(work/"engine/attempts"))if(file.filename()=="result.json") {
        need(result.is_null(),"multiple Results for one check");result=parse(read(file));
        need(text(result["identity"]["job_id"])==text(job["job_id"]),"Result does not match requested Job");
        validateShape(result,"result");
        const auto rawPath=text(result["raw"]["record"]);if(fs::is_regular_file(rawPath))raw=parse(read(rawPath));
    }
    auto status=text(receipt["status"]);std::string outcome,verdict,reason,token;
    if(!result.is_null()) {
        outcome=text(result["outcome"]);verdict=text(result["verdict"]);reason=text(result["reason"]["text"]);
        token=text(result["evidence"]["raw_status"]);
        std::string stages;
        for(const auto &stage:items(raw["stages"])) {
            if(!stages.empty())stages+="; ";
            stages+=text(stage["stage"])+"="+(truth(stage["sby_status_raw"])?text(stage["sby_status_raw"]):text(stage["status"]));
        }
        if(!stages.empty())token=stages;
    }else {
        const bool memoryLimit = text(receipt["result"]) == "oom-kill" || text(receipt["result"]) == "memory-limit";
        outcome=(status=="timeout"||memoryLimit)?"completed":"error";
        verdict=outcome=="completed"?"unknown":"";
        reason=status=="timeout"?"check process-tree deadline":memoryLimit?"check process-tree memory limit":"adapter did not publish a Result";
    }
    J peak;const auto peakText=text(receipt["memory_peak_bytes"]);
    if(!peakText.empty()&&peakText.find_first_not_of("0123456789")==std::string::npos)peak=num(std::stoull(peakText));
    return object({{"outcome",str(outcome)},{"verdict",str(verdict)},{"reason",str(reason)},
        {"raw_status",str(token)},{"job_id",job["job_id"]},{"wall_s",receipt["wall_s"]},
        {"engine_s",result["timing"]["elapsed_s"]},{"peak_bytes",peak},{"job",job},{"result",result},{"raw",raw},{"receipt",receipt}});
}
static void runCheck(const fs::path &root, const fs::path &work, const J &row, const J &engine,
                     uint64_t memory, double timeout, pid_t owner) {
    fs::create_directories(work);
    Database bundle(root/"lowering.sqlite",false,true);
    const auto stored=bundle.rows("SELECT body FROM check_templates WHERE job_id=?",{row["template_id"]});
    need(stored.size()==1,"check template missing");
    const auto job=materialize(bundle,parse(text(stored.front()["body"])),root,work,engine);
    save(work/"job.json",job);
    veval::workflow::Process p;
    p.root=work;
    p.out=work/"supervisor";
    p.memoryBytes=memory;
    // Start this check's budget after queue waiting; allow five seconds for cleanup.
    p.deadline=monotonic()+timeout+5;
    p.parentToMonitor=owner;
    std::vector<std::string> command{helper("consflow_engine").string(), "run",
        "--job", (work/"job.json").string(), "--out", (work/"engine").string()};
#ifdef __APPLE__
    command.push_back("--inherit-process-session");
#endif
    const auto receipt=p.run("check",command);
    save(work/"done.json",readCheckResult(work,job,receipt));
}
// Persist before deleting scratch; completed rows are sufficient for resume.
static bool recordCheck(Database &db, const J &row, const fs::path &work, bool keepWork) {
    if (!fs::is_regular_file(work/"done.json")) {
        const auto reason=fs::exists(work/"error.txt")?read(work/"error.txt"):"worker interrupted before publication";
        db.exec("UPDATE checks SET status='failed',outcome='error',reason=? WHERE template_id=?",
                {str(reason),row["template_id"]});
        return false;
    }
    const auto result=parse(read(work/"done.json"));
    const auto verdict=text(result["verdict"]);
    const bool cancelled=text(result["receipt"]["status"])=="interrupted"&&verdict!="proved"&&verdict!="refuted";
    db.exec("UPDATE checks SET status=?,outcome=?,verdict=?,raw_status=?,reason=?,wall_s=?,engine_s=?,"
            "peak_bytes=?,job_id=?,evidence=? WHERE template_id=?",
            {str(cancelled?"cancelled":"complete"),result["outcome"],result["verdict"],result["raw_status"],
             result["reason"],result["wall_s"],result["engine_s"],result["peak_bytes"],result["job_id"],
             str(canonical(result)),row["template_id"]});
    if (!keepWork) fs::remove_all(work);
    return text(result["outcome"])!="error";
}
static int run(const Arguments &a) {
    const fs::path root=fs::canonical(a.required("--bundle"));
    Database bundle(root/"lowering.sqlite",false,true);
    need(bundle.meta("bundle.complete")=="1","incomplete lowering bundle");
    const auto manifest=parse(read(root/"manifest.json"));const auto key=bundle.meta("bundle.key");
    need(text(manifest["key"])==key,"bundle identity mismatch");
    // One input hash pass for the batch; the adapter also checks its own Job.
    for(const auto &file:items(manifest["inputs"]))need(fileDigest(root/text(file["path"]))==text(file["sha256"]),"lowering shared input changed");
    const auto selection=a.get("--select","deepest"),method=a.get("--method","kind");
    need(selection=="deepest"||selection=="top"||selection=="all-depth"||selection=="all","invalid selection");
    need(method=="kind"||method=="pdr","method must be kind or pdr");
    const auto timeout = positive(a.get("--timeout", "3600"));
    const auto workersValue = positive(a.get("--workers", "1"));
    need(workersValue <= 256 && std::floor(workersValue) == workersValue,
        "workers must be an integer in [1,256]");
    const unsigned workers = workersValue;
#ifdef __APPLE__
    need(!a.has("--memory") || a.get("--memory") == "0", "process-tree memory limits are unavailable on macOS; use --memory 0");
    const uint64_t memory = 0;
    const auto configuredSolver = a.get("--solver", "z3");
#else
    const auto gib = positive(a.get("--memory", "32"));
    need(gib <= 1048576 && gib * 1073741824.0 >= 1, "invalid memory limit");
    const uint64_t memory = gib * 1073741824.0;
    const auto configuredSolver = a.get("--solver", "boolector");
#endif
    const auto memoryRecord = memory ? num(memory) : J();
    const auto engine=object({{"name",str("sby")},{"method",str(method=="pdr"?"abc pdr":"smtbmc")},
        {"solver",method=="pdr"?J():str(configuredSolver)},{"ladder",ladder(a)},
        {"timeout_s",real(timeout)},{"memory_gb",J()}}); // outer tree monitor, not per-process RLIMIT_AS
    const auto requested=a.all("--property");
    auto queue=selectTemplates(bundle,text(manifest["root"]),selection,requested);
    if (a.has("--list")) {
        for (const auto &row:queue) std::cout<<text(row["source_entry"])<<'\t'<<text(row["instance"])<<'\n';
        return 0;
    }
    need(!queue.empty(),"no encodable checks in this selection; inspect --list or try --select top");
    const fs::path out=absolute(a.required("--out"));fs::create_directories(out);
    int lock=::open((out/"lock").c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);
    need(lock>=0&&::flock(lock,LOCK_EX|LOCK_NB)==0,"another check owns output directory");
    struct Guard{int fd;~Guard(){::close(fd);}}guard{lock};
    Database db(out/"results.sqlite",!fs::exists(out/"results.sqlite"));
    const auto self=veval::util::currentExecutable();
    const auto runtime=runtimeFingerprint(method,configuredSolver);
    const auto config=canonical(object({{"bundle_key",str(key)},{"selection",str(selection)},{"engine",engine},
        {"memory_bytes",memoryRecord},{"properties",veval::workflow::stringArray(requested)},
        {"consumer_sha256",str(fileDigest(self))},{"runtime",runtime}}));
    need(db.meta("checks.config").empty()||db.meta("checks.config")==config,"check configuration changed; use a new output directory");
    db.meta("checks.config",config);
    db.exec("CREATE TABLE IF NOT EXISTS checks(template_id TEXT PRIMARY KEY,source_entry TEXT,instance TEXT,"
            "label TEXT,selection TEXT,method TEXT,status TEXT,outcome TEXT,verdict TEXT,raw_status TEXT,reason TEXT,"
            "wall_s REAL,engine_s REAL,peak_bytes INTEGER,timeout_s REAL,memory_bytes INTEGER,bundle_key TEXT,"
            "job_id TEXT,evidence TEXT)");
    db.begin();
    for (const auto &row:queue)
        db.exec("INSERT OR IGNORE INTO checks(template_id,source_entry,instance,label,selection,method,status,"
                "timeout_s,memory_bytes,bundle_key) VALUES(?,?,?,?,?,?,'pending',?,?,?)",
                {row["template_id"],row["source_entry"],row["instance"],row["label"],str(selection),str(method),
                 real(timeout),memoryRecord,str(key)});
    db.commit();
    queue=db.rows("SELECT * FROM checks WHERE status!='complete' ORDER BY source_entry,instance");
    struct Worker{J row;fs::path work;unsigned slot;};std::map<pid_t,Worker> active;
    size_t next=0;bool failed=false;
    while(next<queue.size()||!active.empty()) {
        if(!veval::workflow::interrupted()&&!fs::exists(out/"PAUSE")&&next<queue.size()&&active.size()<workers) {
            unsigned slot=0;while(std::any_of(active.begin(),active.end(),[&](const auto &p){return p.second.slot==slot;}))++slot;
            const auto row=queue[next++];const auto work=out/"work"/(a.has("--keep-work")?text(row["template_id"]):"slot-"+std::to_string(slot));
            // Rebuild interrupted scratch; finished outcomes stay in SQLite.
            fs::remove_all(work);fs::create_directories(work);
            std::cout<<selection<<' '<<text(row["source_entry"])<<" @ "<<text(row["instance"])<<std::endl;
            const auto owner=::getpid(),pid=::fork();need(pid>=0,"cannot fork check worker");
            if(pid==0) {
#ifdef __linux__
                ::prctl(PR_SET_PDEATHSIG,SIGTERM);
#endif
                if (::getppid() != owner) ::_exit(125);
                try {runCheck(root,work,row,engine,memory,timeout,owner);::_exit(0);}
                catch(const std::exception &e){write(work/"error.txt",e.what());::_exit(2);}
            }
            db.exec("UPDATE checks SET status='running' WHERE template_id=?",{row["template_id"]});active.emplace(pid,Worker{row,work,slot});
        }
        for(auto it=active.begin();it!=active.end();) {
            if(veval::workflow::interrupted())::kill(it->first,SIGTERM);
            int status;const auto pid=::waitpid(it->first,&status,WNOHANG);
            if(pid==0||(pid<0&&errno==EINTR)){++it;continue;}
            auto &w=it->second;
            if (!recordCheck(db,w.row,w.work,a.has("--keep-work"))) failed=true;
            report(db,out);it=active.erase(it);
        }
        if(active.empty()&&(veval::workflow::interrupted()||fs::exists(out/"PAUSE")))break;
        ::poll(nullptr,0,50);
    }
    failed=failed||!db.rows("SELECT 1 FROM checks WHERE outcome='error' LIMIT 1").empty();
    report(db,out);return veval::workflow::interrupted()?130:failed?2:next<queue.size()?75:0;
}
}
int main(int argc,char **argv) {
    using namespace veval::verification;
    try {
        if(argc>1&&std::string(argv[1])=="__check-exec")return veval::workflow::executeOwned(argc,argv);
        veval::util::command_runner::installCancellationHandlers();
        Arguments a(argc,argv,{"--help","--keep-work","--list"},{"--bundle","--out","--select","--property","--method","--solver","--depths","--timeout","--memory","--workers"});
        if(a.has("--help")){std::cout<<"consflow_check --bundle DIR --out RUN [--property LABEL ...] [--select deepest|top|all-depth|all]\n"
            "  [--method kind|pdr --solver SOLVER --depths 5,10,20]\n"
            "  [--timeout 3600 --memory GiB --workers 1] [--keep-work] [--list]\n"
#ifdef __APPLE__
            "macOS defaults to Z3 and accepts only --memory 0 (no aggregate memory cap).\n"
#else
            "Linux defaults to Boolector and a sampled 32 GiB tree RSS+swap limit per check.\n"
#endif
            "Default cleanup leaves SQLite/CSV outcomes. --keep-work keeps SBY files and logs.\n"
            "Local refutations are not source conclusions. Touch RUN/PAUSE to stop admission.\n";return 0;}
        need(a.positional.empty(),"unexpected positional arguments");return run(a);
    }catch(const std::exception &e){std::cerr<<"consflow_check: "<<e.what()<<'\n';return 2;}
}
