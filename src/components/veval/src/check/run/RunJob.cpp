#include "../prepare/JobSchema.h"
#include "../../util/process/RunWithResourceLimits.h"
#include "util/sby/SbyRunner.h"
#include "util/database/LoweringDatabase.h"
#include <algorithm>
#include <fcntl.h>
#include <sys/file.h>
#include <cmath>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <unistd.h>

namespace veval::verification {
static std::string utc() {
    std::time_t t=std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t,&tm);
    char s[32];
    std::strftime(s,sizeof s,"%Y-%m-%dT%H:%M:%SZ",&tm);
    return s;
}
static double numeric(const J &j,double fallback=0) {
    return j.is_number()?std::stod(j.value):fallback;
}
static const J &at(const J &a,size_t index) {
    return *(items(a).begin()+index);
}
static void need(bool ok,const std::string &why) {
    if(!ok)throw std::runtime_error(why);
}
static std::string ossBin() {
    if(auto path=std::getenv("OSS_CAD_BIN"))return path;
    const auto home=std::getenv("HOME");
    return std::string(home?home:"")+"/.local/oss-cad-suite/bin";
}
struct Decision {
    std::string outcome="completed",verdict="unknown",reason,token,method;
    J bound;
};
static J nullBound() {
    return object({{"value",J()},{"unit",J()},{"source",J()}});
}
static J bound(unsigned value,const std::string &unit,const std::string &source) {
    return object({{"value",num(value)},{"unit",str(unit)},{"source",str(source)}});
}
static J toolsFor(const J &job) {
    const auto &engine=job["engine"];
    const auto name=text(engine["name"]),method=text(engine["method"]),solver=text(engine["solver"]);
    J out=object();
    auto add=[&](const std::string &name,const std::string &exe="") {
        put(out,name,str(program(exe.empty()?name:exe,ossBin())));
    };
    if(name=="sby") {
        if(method=="smtbmc"||method=="smtbmc-multiclock") {
            static const std::set<std::string> supported{"boolector","yices","bitwuzla","z3","cvc5"};
            if(!supported.count(solver))throw Unsupported("unsupported SBY solver: "+solver);
        }else if(method!="abc pdr"||!engine["solver"].is_null())throw Unsupported("unsupported SBY method/solver");
        for(const auto &rung:items(engine["ladder"])) {
            const auto mode=text(at(rung,1));
            if((mode!="prove"&&mode!="bmc")||(method=="abc pdr"&&mode!="prove"))
                throw Unsupported("unsupported SBY ladder mode");
        }
        add("sby");
        add("yosys");
        if(method=="abc pdr")add("yosys-abc");
        else {
            add("yosys-smtbmc");
            add(solver,solver=="yices"?"yices-smt2":solver);
        }
    }else if(name=="ebmc") {
        const bool solverless=method=="bdd"||method=="aig";
        static const std::set<std::string> solvers{"builtin","boolector","z3","yices","cvc4"};
        if((method!="k-induction"&&method!="bdd"&&method!="aig"&&method!="bmc")||
           (solverless?!engine["solver"].is_null():!solvers.count(solver)))
            throw Unsupported("unsupported EBMC method/solver");
        const auto mode=(method=="bmc"||method=="aig")?"bmc":"prove";
        for(const auto &rung:items(engine["ladder"]))
            if(text(at(rung,1))!=mode)throw Unsupported("EBMC method does not match ladder mode");
        const auto binary=std::getenv("EBMC_BINARY");
        add("ebmc",binary?binary:"ebmc");
        if(!solverless&&solver!="builtin")add(solver,solver=="yices"?"yices-smt2":solver);
    }else throw Unsupported("no production driver for engine "+name);
    return out;
}
static Decision sbyDecision(const J &cell) {
    Decision d;
    d.token=text(cell["sby_verdict"]);
    d.reason=text(cell["sby_detail"]);
    d.bound=nullBound();
    if(d.token=="proved-by-induction"){
        d.verdict="proved";
        d.method="temporal-induction";
        for(const auto &stage:items(cell["stages"]))
            if(text(stage["status"])=="PASS"&&text(stage["engines"])=="abc pdr")d.method="pdr";
    }else if(d.token=="refuted")d.verdict="refuted";
    else if(d.token.rfind("bounded-clean-to-K=",0)==0) {
        d.verdict="bounded-clean";
        d.bound=bound(std::stoul(d.token.substr(19)),"sby depth (steps 0..depth-1 checked)",d.token);
    }else if(d.token=="refused-by-translator") {
        d.outcome="unsupported";
        d.verdict.clear();
    }else if(d.token=="blocked") {
        d.outcome="error";
        d.verdict.clear();
    }else if(items(cell["stages"]).empty()) {
        d.outcome="not-run";
        d.verdict.clear();
    }
    return d;
}

// Return false only when another ladder stage may still decide the check.
static bool ebmcDecision(Decision &d,const J &property,const J &job,
                         const std::set<std::string> &assumed,J &record) {
    const auto status=text(property["status"]),via=text(property["proof_via"]);
    d.token=status;
    d.method=via;
    static const std::regex proofMethod("[0-9]+-induction|BDD");
    static const std::regex boundedProof("PROVED up to bound ([0-9]+)");
    static const std::regex boundedRefutation("REFUTED up to bound [0-9]+");
    std::smatch match;
    if(status=="PROVED") {
        if(std::regex_match(via,proofMethod)) {
            d.verdict="proved";
            d.method=via=="BDD"?"bdd":"k-induction";
        }else d.reason="proof method outside audited table: "+via;
        return true;
    }
    if(std::regex_match(status,match,boundedProof)) {
        d.verdict="bounded-clean";
        d.bound=bound(std::stoul(match[1]),"ebmc timeframes",status);
        return true;
    }
    if(status=="REFUTED"||std::regex_match(status,boundedRefutation)) {
        bool all=true;
        for(const auto &premise:items(job["premises"]))
            if(truth(premise["label"])&&!assumed.count(text(premise["label"])))all=false;
        put(record,"premise_audit",object({
            {"outcome",str(all?"all-premises-hold":"premise-not-assumed")},{"checked",boolean(true)}}));
        if(all)d.verdict="refuted";
        else d.reason="EBMC did not compile all assumptions";
        return true;
    }
    if(status=="ASSUMED"||status=="FAILURE") {
        d.outcome="error";
        d.verdict.clear();
        d.reason="EBMC target status "+status;
        return true;
    }
    if(status.rfind("UNSUPPORTED",0)==0) {
        d.outcome="unsupported";
        d.verdict.clear();
        d.reason=status;
        return true;
    }
    if(status=="DISABLED") {
        d.outcome="not-run";
        d.verdict.clear();
        d.reason=status;
        return true;
    }
    return false;
}
static Decision runEbmc(const J &job,const J &tools,const fs::path &product,const fs::path &work,
                        double deadline,unsigned long long memory,double cleanup,J &record,bool inheritSession) {
    Decision d;
    d.bound=nullBound();
    const auto &engine=job["engine"];
    if(text(job["setup"]["reset"]["kind"])=="reset-contract")
        throw Unsupported("EBMC does not implement t0-only reset setup");
    if(read(product).find("// observation:")!=std::string::npos)
        throw Unsupported("EBMC does not implement internal observation binding");
    for(const auto &premise:items(job["premises"]))
        if(text(premise["kind"])=="derived")throw Unsupported("EBMC does not compile derived comment premises");
    std::vector<std::string> base{text(tools["ebmc"]),product.string()};
    for(const auto &input:items(job["inputs"]))
        if(text(input["role"])=="design"||text(input["role"])=="harness")base.push_back(text(input["path"]));
    const auto top=text(job["target"]["top"]),label=text(job["target"]["label"]),target=top+'.'+label;
    base.insert(base.end(),{"--top",top,"--property",target});
    if(truth(job["frontend"]["systemverilog"]))base.push_back("--systemverilog");
    for(const auto &p:items(job["frontend"]["include_order"]))base.insert(base.end(),{"-I",text(p)});
    for(const auto &[name,value]:members(job["frontend"]["defines"]))
        base.insert(base.end(),{"-D",name+(text(value).empty()?"":"="+text(value))});
    const auto method=text(engine["method"]),solver=text(engine["solver"]);
    static const std::regex stageName("[A-Za-z0-9_-]+");
    for(const auto &rung:items(engine["ladder"])) {
        if(cancelled()||monotonic()>=deadline) {
            d.reason=cancelled()?"cancelled":"timeout";
            break;
        }
        const auto name=text(at(rung,0)),mode=text(at(rung,1));
        const unsigned depth=numeric(at(rung,2));
        need(std::regex_match(name,stageName),"invalid stage name");
        const auto result=work/(name+".json"),log=work/(name+".log");
        auto command=base;
        if(method=="k-induction")command.insert(command.end(),{"--k-induction","--bound",std::to_string(depth)});
        else if(method=="bdd")command.push_back("--bdd");
        else {
            if(method=="aig")command.push_back("--aig");
            command.insert(command.end(),{"--bound",std::to_string(depth)});
        }
        if(!solver.empty()&&solver!="builtin")command.push_back("--"+solver);
        command.insert(command.end(),{"--json-result",result.string()});
        const auto stageDeadline=std::min(deadline,monotonic()+numeric(at(rung,3),std::numeric_limits<double>::infinity()));
        const auto done=execute(command,log,work,stageDeadline,memory,cleanup,inheritSession);
        J cmd=list();
        for(const auto &s:command)append(cmd,str(s));
        J stage=object({{"stage",str(name)},{"mode",str(mode)},{"depth",num(depth)},{"wall_s",num(done.elapsed)},{"returncode",num(done.exitCode)},
            {"command",cmd},{"stdout",str(log.string())},{"solving_s",num(done.solving)},{"cleanup_s",num(done.cleanup)},
            {"max_rss_kb",num(done.maxRssKb)},{"reaped",object({{"complete",boolean(done.cleanupComplete)}})}});
        if(!done.cleanupComplete) {
            put(stage,"status",str("CLEANUP-FAILED"));
            append(record.make_object()["stages"],stage);
            throw std::runtime_error(done.error);
        }
        if(done.timedOut||done.stopped) {
            put(stage,"status",str("TIMEOUT"));
            append(record.make_object()["stages"],stage);
            d.reason=done.stopped?"cancelled":"timeout";
            continue;
        }
        if(!fs::exists(result)) {
            put(stage,"status",str("NO-JSON"));
            append(record.make_object()["stages"],stage);
            d.outcome="error";
            d.verdict.clear();
            d.reason="EBMC did not write a result; see "+log.string();
            return d;
        }
        const auto document=parse(read(result));
        J mine;
        std::set<std::string> assumed;
        for(const auto &property:items(document["properties"])) {
            const auto id=text(property["identifier"]);
            if(id==target||(id.size()>target.size()&&id.substr(id.size()-target.size()-1)=='.'+target))mine=property;
            if(text(property["status"])=="ASSUMED")assumed.insert(id.substr(id.rfind('.')+1));
        }
        need(!mine.is_null(),"EBMC result lacks selected target");
        put(stage,"status",str(text(mine["status"])));
        put(stage,"proof_via",mine["proof_via"]);
        put(stage,"result",str(result.string()));
        append(record.make_object()["stages"],stage);
        if(ebmcDecision(d,mine,job,assumed,record))return d;
    }
    return d;
}

static J sbyRequest(const J &job,const fs::path &product,const fs::path &work,
                    unsigned long long memory,double cleanup,bool inheritSession,J &effective) {
    J designs=list(),defines=list();
    for(const auto &input:items(job["inputs"]))
        if(text(input["role"])=="design"||text(input["role"])=="harness")append(designs,input["path"]);
    for(const auto &[name,value]:members(job["frontend"]["defines"]))
        append(defines,str(name+(text(value).empty()?"":"="+text(value))));
    const auto engines=text(job["engine"]["method"])=="abc pdr"
        ?"abc pdr":"smtbmc "+text(job["engine"]["solver"]);
    put(effective,"engines_line",str(engines));
    J request=object({
        {"product",str(product.string())},{"label",job["target"]["label"]},{"module",job["target"]["module"]},
        {"instance",job["target"]["instance"]},{"work_dir",str(work.string())},{"design_files",designs},
        {"include_paths",job["frontend"]["include_order"]},{"defines",defines},
        {"systemverilog",job["frontend"]["systemverilog"]},{"oss_cad_bin",str(ossBin())},
        {"address_space_bytes",num(memory)},{"stages",job["engine"]["ladder"]},{"engines",str(engines)},
        {"cleanup_margin_s",num(cleanup)},{"job",job}});
    if(text(job["engine"]["method"])=="smtbmc-multiclock")put(request,"multiclock",boolean(true));
    if(inheritSession)put(request,"process_supervised",boolean(true));
    return request;
}

static void compactWork(const fs::path &work,const J &job,const J &record) {
    for(const auto &entry:fs::directory_iterator(work))
        if(entry.is_directory()&&(entry.path().extension()==".work"||entry.path().filename()=="sva"))
            fs::remove_all(entry.path());
    fs::remove(work/"observation-design.v");
    fs::remove(work/"observation-design.il");
    if(text(job["engine"]["name"])=="ebmc")
        for(const auto &stage:items(record["stages"]))
            if(truth(stage["result"]))fs::remove(text(stage["result"]));
    for(const auto &entry:fs::directory_iterator(work)) {
        if(!entry.is_regular_file())continue;
        const auto extension=entry.path().extension();
        if((extension!=".stdout"&&extension!=".log")||entry.file_size()<=8192)continue;
        std::ifstream in(entry.path(),std::ios::binary);
        in.seekg(-8192,std::ios::end);
        std::string tail(8192,'\0');
        in.read(tail.data(),tail.size());
        need(in.good(),"cannot compact engine log");
        write(entry.path(),"[earlier output discarded after audit]\n"+tail);
    }
}

static int run(const Arguments &args) {
    const auto start=monotonic();
    const auto began=utc();
    const fs::path root=absolute(args.required("--out"));
    fs::create_directories(root/"attempts");
    // Keep ownership until process exit, even if the parent batch disappears.
    const int lock=::open((root/"adapter.lock").c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);
    need(lock>=0&&::flock(lock,LOCK_EX|LOCK_NB)==0,"another adapter is still running for this Job");
    // Consumers sort attempt IDs; mkdtemp adds uniqueness to the timestamp.
    const auto stamp=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string pattern=(root/"attempts"/("native-"+std::to_string(stamp)+"-XXXXXX")).string();
    if(!::mkdtemp(pattern.data()))throw std::runtime_error("cannot create exclusive attempt directory");
    const fs::path dir=pattern,work=dir/"engine";
    fs::create_directory(work);
    const auto attempt=dir.filename().string();
    const fs::path requestPath=absolute(args.required("--job"));
    J job,verified,product,tools=object(),effective,record=object({{"stages",list()}});
    std::string requestHash,id,errorCode;
    Decision decision;
    decision.outcome="error";
    decision.verdict.clear();
    decision.bound=nullBound();
    double deadline=std::numeric_limits<double>::infinity(),cleanup=std::stod(args.get("--cleanup-margin","2"));
    need(std::isfinite(cleanup)&&cleanup>0,"invalid cleanup margin");
    unsigned long long memory=0;
    bool launched=false;
    try {
        const auto bytes=read(requestPath);
        requestHash=digest(bytes);
        write(dir/"request.json",bytes);
        job=parse(bytes);
        errorCode="contract-violation";
        validateJob(job);
        errorCode="identity-mismatch";
        need(identity(job)==text(job["job_id"]),"Job identity differs from content");
        id=text(job["job_id"]);
        if(job["engine"]["timeout_s"].is_number())deadline=start+numeric(job["engine"]["timeout_s"]);
        const auto memoryGb=numeric(job["engine"]["memory_gb"]);
        need(memoryGb<1048576,"memory request is out of range");
        memory=static_cast<unsigned long long>(memoryGb*1073741824.0);
        // Zero bytes means "no limit"; a positive request must not round to it.
        need(!(memoryGb>0)||memory>0,"memory request below one byte");
        if(cancelled()||monotonic()>=deadline)throw Expired("budget gone before engine launch");
        verified=list();
        fs::path productPath;
        for(const auto &input:items(job["inputs"])) {
            const auto path=text(input["path"]);
            need(fs::is_regular_file(path),"input unavailable: "+path);
            // Verify current inputs; the product is checked separately below.
            if(text(input["role"])!="product") {
                const auto before=errorCode;
                errorCode="input-mismatch";
                need(fileDigest(path)==text(input["sha256"]),"input changed after Job generation: "+path);
                errorCode=before;
            }
            append(verified,object({{"role",input["role"]},{"original_path",str(path)},
                {"execution_path",str(path)},{"snapshot_path",str(path)},{"sha256",input["sha256"]}}));
            if(text(input["role"])=="product")productPath=path;
        }
        errorCode="product-mismatch";
        Files files;
        Product parsed(productPath,files);
        product=verifyProduct(job,parsed);
        for(const auto &input:items(job["inputs"]))
            if(text(input["role"])=="product")
                need(equal(input["sha256"],product["sha256"]),"product contents changed after Job generation");
        save(dir/"product-manifest.json",product);
        errorCode="program-missing";
        tools=toolsFor(job);
        effective=job["engine"];
        put(effective,"tool_paths",tools);
        const auto path=ossBin()+":"+(std::getenv("PATH")?std::getenv("PATH"):"");
        ::setenv("PATH",path.c_str(),1);
        if(cancelled()||monotonic()>=deadline)throw Expired("budget gone before engine launch");
        errorCode="backend-error";
        launched=true;
        if(text(job["engine"]["name"])=="sby") {
            const auto request=sbyRequest(job,productPath,work,memory,cleanup,
                                          args.has("--inherit-process-session"),effective);
            save(work/"request.json",request);
            record=runSby(request,deadline);
            decision=sbyDecision(record);
        }else decision=runEbmc(job,tools,productPath,work,deadline,memory,cleanup,record,
                              args.has("--inherit-process-session"));
        if(monotonic()>=deadline&&(decision.verdict=="proved"||decision.verdict=="refuted"||
                                  decision.verdict=="bounded-clean")) {
            decision.verdict="unknown";
            decision.reason="decision unavailable before attempt deadline";
            decision.bound=nullBound();
        }
        if(decision.outcome=="completed")errorCode.clear();
    }catch(const Unsupported &e) {
        decision.outcome="unsupported";
        decision.verdict.clear();
        decision.reason=e.what();
        errorCode="unsupported";
    }catch(const Expired &e) {
        decision.outcome=launched?"completed":"not-run";
        decision.verdict=launched?"unknown":"";
        decision.reason=e.what();
        errorCode=cancelled()?(launched?"cancelled":"cancelled-before-launch"):"budget-exhausted";
    }catch(const std::exception &e) {
        // Errors caused by cancellation remain retryable.
        if(cancelled()) {
            decision.outcome=launched?"completed":"not-run";
            decision.verdict=launched?"unknown":"";
            decision.reason=e.what();
            errorCode=launched?"cancelled":"cancelled-before-launch";
        }else {
            decision.outcome="error";
            decision.verdict.clear();
            decision.reason=e.what();
        }
    }
    if(cancelled()&&decision.outcome!="error") {
        decision.outcome=launched?"completed":"not-run";
        decision.verdict=launched?"unknown":"";
        decision.reason="cancelled";
        errorCode=launched?"cancelled":"cancelled-before-launch";
        decision.bound=nullBound();
    }
    const auto elapsed=monotonic()-start;
    J commands=list(),logs=list();
    long peak=0;
    bool clean=true;
    double backend=0;
    for(const auto &stage:items(record["stages"])) {
        if(!stage["command"].is_null())append(commands,stage["command"]);
        if(truth(stage["stdout"]))append(logs,stage["stdout"]);
        peak=std::max(peak,static_cast<long>(numeric(stage["max_rss_kb"])));
        backend+=numeric(stage["wall_s"]);
        if(!stage["reaped"].is_null()&&!truth(stage["reaped"]["complete"]))clean=false;
    }
    if(!clean) {
        decision.outcome="error";
        decision.verdict.clear();
        errorCode="cleanup-failed";
    }
    const auto rawPath=work/(text(job["engine"]["name"])=="sby"?"cell.json":"ebmc.json");
    save(rawPath,record);
    auto optional=[](const std::string &s){return s.empty()?J():str(s);};
    auto budget=job.is_null()?J():job["engine"]["timeout_s"];
    const auto overrun=budget.is_null()?0:std::max(0.0,elapsed-numeric(budget));
    J result=object({{"schema",str("consflow-result/2")},
        {"identity",object({{"job_id",optional(id)},{"job_id_short",id.empty()?J():str(id.substr(0,24))},
                            {"attempt_id",str(attempt)}})},
        {"request",object({{"path",str(requestPath.string())},{"sha256",optional(requestHash)}})},
        {"verified_inputs",verified},{"target",job["target"]},{"product",product},{"premises",job["premises"]},
        {"config",object({{"requested",job["engine"]},{"effective",effective},
                          {"defaults_applied",job["provenance"]["defaults_applied"]}})},
        {"toolchain",object({{"paths",tools},{"versions",object()},{"sha256",object()},
                             {"commands",commands},{"path_prefix",str(ossBin())}})},
        {"outcome",str(decision.outcome)},{"verdict",optional(decision.verdict)},
        {"verdict_reason",decision.verdict=="unknown"?optional(decision.reason):J()},
        {"reason",object({{"code",optional(errorCode)},{"text",optional(decision.reason)}})},
        {"evidence",object({{"status_token",optional(decision.token)},{"method_token",optional(decision.method)},
            {"raw_status",optional(decision.token)},{"raw_detail",optional(decision.reason)},
            {"bound",decision.bound},{"after_deadline",budget.is_null()?J():boolean(monotonic()>=deadline)}})},
        {"timing",object({{"started_utc",str(began)},{"finished_utc",str(utc())},
            {"t0_monotonic",num(start)},{"budget_s",budget},{"elapsed_s",num(elapsed)},
            {"phases",object({{"backend",num(backend)},
                              {"preparation_and_audit",num(std::max(0.0,elapsed-backend))}})},
            {"stages",record["stages"]},
            {"cleanup",object({{"s",J()},{"status",str(clean?"all-reaped":"incomplete")},{"reaped",list()}})},
            {"overrun",object({{"seconds",overrun?num(overrun):J()},
                               {"reason",overrun?str("attempt cleanup/publication exceeded work deadline"):J()}})}})},
        {"limits",object({{"memory_bytes",object({{"requested",memory?num(memory):J()},
            {"applied",str(!items(record["stages"]).empty()
                ?(memory?"applied: RLIMIT_AS per process":"not-applied: no memory limit requested")
                :"not-applied: no engine stage was started")},{"error",J()}})},{"wall_s",budget}})},
        {"resources",object({{"peak_memory",object({{"value_kb",peak?num(peak):J()},
            {"measure",str("max-single-process")},{"source",str("wait4 ru_maxrss")},
            {"time_file",J()},{"status",str(peak?"measured":"not-measured")}})},
            {"tree_rss",object({{"status",str("not-measured")}})}})},
        {"raw",object({{"record",str(rawPath.string())},{"logs",logs}})},
        {"trace",object({{"raw",list()},{"normalized",J()},
            {"status",str(decision.verdict=="refuted"?"audited-not-retained":"not-retained")},
            {"liftable",J()},{"reason",str(decision.verdict=="refuted"
                ?"native backend premise audit completed; waveform export disabled":"no counterexample retained")}})},
        {"attempt_dir",str(dir.string())}});
    if (args.has("--keep-work") && decision.verdict == "refuted") {
        const auto &audit = record["premise_audit"];
        if (text(audit["outcome"]) == "all-premises-hold" && !items(audit["traces_audited"]).empty())
            put(result, "trace", object({{"raw", audit["traces_audited"]}, {"normalized", J()},
                {"status", str("retained")}, {"liftable", J()},
                {"reason", str("module counterexample verified against the compiled premises")}}));
    }
    validateShape(result,"result");
    save(dir/"result.json",result);
    if(args.has("--db")&&!id.empty()) {
        Database db(absolute(args.get("--db")),!fs::exists(args.get("--db")));
        db.attempt(result,dir/"result.json",args.get("--arm","local"));
    }
    if(!args.has("--keep-work"))compactWork(work,job,record);
    std::cout<<"outcome\t"<<decision.outcome<<"\nverdict\t"<<decision.verdict
             <<"\nRESULT "<<(dir/"result.json").string()<<'\n';
    return 0;
}
} // namespace veval::verification
int main(int argc,char **argv) {
    using namespace veval::verification;
    try {
        installCancellationHandlers();
        Arguments args(argc,argv,{"--help","--keep-work","--inherit-process-session"},
                       {"--job","--out","--db","--arm","--cleanup-margin"});
        if(args.has("--help")) {
            std::cout<<"consflow_engine run --job FILE --out DIR [--db DB --arm local] "
                       "[--cleanup-margin S] [--keep-work]\n";
            return 0;
        }
        if(args.positional!=std::vector<std::string>{"run"})throw std::runtime_error("expected run subcommand");
        return run(args);
    }catch(const std::exception &e) {
        std::cerr<<"consflow_engine: "<<e.what()<<'\n';
        return 2;
    }
}
