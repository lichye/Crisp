#include "util/process/CurrentExecutablePath.h"
#include "LoweringStore.h"
#include "../check/run/RunPropertyChecks.h"
#include "util/task/CommandArgumentsAndFiles.h"
#include "util/process/RunWithResourceLimits.h"
#include "util/files/Sha256.h"
#include "util/process/RunExternalCommand.h"
#include <fcntl.h>
#include <cerrno>
#include <charconv>
#include <poll.h>
#include <sys/file.h>
#include <unistd.h>
#include <iostream>
#include <map>
#include <sstream>

namespace veval::workflow {
namespace {
using Paths = std::map<std::string,std::string>;
// Rewrite complete path values only. Property text and RTL bytes are opaque.
J relocate(J value, const Paths &paths) {
    if (value.is_string()) {
        const auto old = text(value);
        for (const auto &[from,to] : paths)
            if (old == from || old.rfind(from + "/",0)==0)
                return str(to + old.substr(from.size()));
    } else if (value.is_array()) {
        for (auto &item : value.make_array()) item = relocate(item,paths);
    } else if (value.is_object()) {
        for (auto &[key,item] : value.make_object()) item = relocate(item,paths);
    }
    return value;
}
struct Lock {
    int fd;
    Lock(const fs::path &p,double deadline):fd(::open(p.c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600)) {
        require(fd>=0,"cannot open lowering lock");
        try {
            while(::flock(fd,LOCK_EX|LOCK_NB)!=0) {
                require(errno==EWOULDBLOCK||errno==EAGAIN||errno==EINTR,"cannot lock lowering bundle");
                require(!interrupted(),"interrupted while waiting for lowering bundle");
                if(monotonic()>=deadline)throw Expired("task deadline exhausted while waiting for lowering bundle");
                ::poll(nullptr,0,50);
            }
        }catch(...){::close(fd);throw;}
    }
    ~Lock(){if(fd>=0)::close(fd);}
};
void environment(const fs::path &binary) {
    const auto oss=veval::util::command_runner::env("OSS_CAD_BIN",veval::util::command_runner::env("HOME")+"/.local/oss-cad-suite/bin");
    const auto path=oss+":"+veval::util::command_runner::env("PATH");
    ::setenv("PATH",path.c_str(),1);
    ::setenv("CONSFLOW_BUILD_DIR",binary.parent_path().c_str(),1);
}
J generationTools(const fs::path &binary) {
    J result=object();
    put(result,"veval",str(veval::verification::fileDigest(binary)));
    put(result,"consflow_jobs",str(veval::verification::fileDigest(nativeHelper(binary,"consflow_jobs","CONSFLOW_JOBS"))));
    for(const auto &name:{"consflow_environment.so","consflow_relations.so"})
        put(result,name,str(veval::verification::fileDigest(veval::verification::helper(name))));
    for(const auto &name:{"ltl2tgba","yosys"})put(result,name,str(veval::verification::fileDigest(executable(name))));
    return result;
}
}
std::string loweringKey(const Input &input,const J &inventory,const J &effective) {
    J sources=list();
    for(const auto &path:strings(inventory["sources"])) append(sources,str(veval::verification::fileDigest(path)));
    return veval::sha256(canonical(object({{"schema",str("consflow-lowering/1")},
        {"sources",sources},{"top",str(input.top)},{"multiclock",boolean(input.multiclock)},
        {"placement",effective["placement"]}})));
}
fs::path ensureLowering(Process &caller,const Input &input,const J &inventory,const J &effective,
                        const fs::path &binary,const fs::path &requested,
                        std::size_t generationThreads) {
    const auto bundle=fs::absolute(requested).lexically_normal();
    fs::create_directories(bundle);
    const auto key=loweringKey(input,inventory,effective);
    auto reuse=[&] {
        if(!fs::exists(bundle/"manifest.json"))return false;
        const auto manifest=jsonFile(bundle/"manifest.json");
        require(text(manifest["key"])==key,"lowering inputs changed; choose a new bundle directory");
        Database db(bundle/"lowering.sqlite",false,true);
        require(db.meta("bundle.complete")=="1" && db.meta("bundle.key")==key,"incomplete lowering bundle");
        for(const auto &file:items(manifest["inputs"]))
            require(veval::verification::fileDigest(bundle/text(file["path"]))==text(file["sha256"]),"cached lowering input changed");
        std::cout<<"lowering: reuse "<<bundle<<std::endl;return true;
    };
    // Completed bundles are immutable and can be mounted read-only. Only
    // generation needs a writer lock; recheck after waiting for another writer.
    if(reuse())return bundle;
    Lock lock(bundle/"lock",caller.deadline);
    if(reuse())return bundle;
    require(!fs::exists(bundle/".generate"),"incomplete lowering retained in "+bundle.string()+"; use a new directory");
    const auto started=monotonic();
    Process p;p.root=caller.root;p.out=bundle/".generate";p.deadline=caller.deadline;
    fs::create_directories(p.out);
    Paths paths; J shared=inventory, identities=list();
    // Preprocessed sources are self contained; keep one copy of each shared
    // input, regardless of the number of properties or generated instances.
    for(const auto &path:filesUnder(fs::path(strings(inventory["sources"]).front()).parent_path())) {
        if(path.extension()!=".sv" && path.extension()!=".v") continue;
        const auto dest=bundle/"inputs"/path.filename();
        fs::create_directories(dest.parent_path());fs::copy_file(path,dest,fs::copy_options::overwrite_existing);
        paths[path.string()]=dest.string();
        append(identities,object({{"path",str(fs::relative(dest,bundle).string())},
            {"sha256",str(veval::verification::fileDigest(dest))}}));
    }
    shared=relocate(shared,paths);
    std::vector<std::string> command{binary.string(),"transport"};
    for(const auto &source:strings(shared["sources"]))command.push_back(source);
    command.insert(command.end(),{"--top",input.top,"--out",(p.out/"products").string(),
        "--log",(p.out/"log").string(),"--log-format","sqlite",
        "--placement",text(effective["placement"])});
    if(input.multiclock)command.push_back("--multiclock");
    if(generationThreads != 1)command.insert(command.end(),{"-j",std::to_string(generationThreads)});
    std::cout<<"lowering: generate all source properties -> "<<bundle<<std::endl;
    completed(p.run("transport",command));
    const auto sourceDb=p.out/"log/consflow.sqlite";
    std::string root;
    {Database db(sourceDb);const auto roots=db.rows("SELECT instance FROM instances WHERE depth=(SELECT min(depth) FROM instances)");
        require(roots.size()==1,"lowering bundle requires one starting DUT");root=text(roots.front()["instance"]);}
    command={nativeHelper(binary,"consflow_jobs","CONSFLOW_JOBS"),"--out",(p.out/"products").string(),
        "--log",(p.out/"log").string(),"--store",sourceDb.string(),"--engine","sby","--method","smtbmc",
        "--solver","boolector","--no-memory-limit","--ladder","[[\"prove_d5\",\"prove\",5,null],[\"prove_d10\",\"prove\",10,null],[\"prove_d20\",\"prove\",20,null]]",
        "--whole-design",root,"--whole-design-policy","keep","--binary",binary.string()};
#ifdef __APPLE__
    command.push_back("--inherit-process-session");
#endif
    if(generationThreads != 1)command.insert(command.end(),{"--generation-threads",std::to_string(generationThreads)});
    for(const auto &path:strings(shared["design"]))command.insert(command.end(),{"--design",path});
    completed(p.run("jobs",command));
    J timings=list();
    for(const auto &file:filesUnder(p.out/"commands"))if(file.filename()=="command.json")append(timings,jsonFile(file));
    const auto pending=bundle/"lowering.sqlite.pending";
    {
        Database db(sourceDb);
        db.exec("CREATE TABLE bundle_files(path TEXT PRIMARY KEY,body TEXT NOT NULL,sha256 TEXT NOT NULL)");
        db.begin();
        for(const auto &dir:{"products","log"})for(const auto &file:filesUnder(p.out/dir)) {
            if(file.string().find("consflow.sqlite")!=std::string::npos)continue;
            // These are legacy duplicate exports of rows already in SQLite.
            // transport.json alone exceeds SQLite's single-value size limit
            // on large designs. No bundle consumer needs these JSON copies.
            const auto relative=fs::relative(file,p.out).generic_string();
            if(relative=="log/transport.json"||relative=="log/graph.json"||relative=="log/reach.json"||
               relative=="log/discharge.json"||relative.rfind("log/environment/",0)==0)continue;
            const auto bytes=readText(file);
            db.exec("INSERT INTO bundle_files VALUES(?,?,?)",{str(fs::relative(file,p.out).string()),str(bytes),str(veval::sha256(bytes))});
        }
        db.meta("bundle.base",bundle.string());db.meta("bundle.key",key);
        db.meta("bundle.inventory",canonical(shared));db.meta("bundle.complete","1");
        db.commit();db.exec("VACUUM INTO ?",{str(pending.string())});
    }
    fs::rename(pending,bundle/"lowering.sqlite");
    const auto manifest=object({{"schema",str("consflow-lowering/1")},{"key",str(key)},
        {"inputs",identities},{"generator",object({{"path",str(binary.string())},{"sha256",str(veval::verification::fileDigest(binary))}})},
        {"generation_tools",generationTools(binary)},
        {"generation_threads",num(generationThreads)},
        {"root",str(root)},{"top",str(input.top)},{"placement",effective["placement"]},
        {"wall_s",num(monotonic()-started)},{"stages",timings}});
    save(bundle/"manifest.json",manifest);
    fs::remove_all(p.out);
    writeText(bundle/"README.txt","Immutable lowering: inputs/ + lowering.sqlite + manifest.json.\n"
        "Run consflow_check --bundle DIR --out RUN --select deepest|top|all-depth --property LABEL\n"
#ifdef __APPLE__
        "  --method kind|pdr --timeout SECONDS --memory 0 --workers N [--keep-work]\n"
#else
        "  --method kind|pdr --timeout SECONDS --memory GiB --workers N [--keep-work]\n"
#endif
        "Jobs, local specifications and generation records are rows in SQLite.\n"
        "Only active checks materialize their inputs and .sby files. Results are local engine verdicts;\n"
        "a local counterexample is not automatically a source counterexample.\n");
    return bundle;
}
void restoreLowering(const fs::path &bundle,const fs::path &transport,const J &inventory) {
    Database src(bundle/"lowering.sqlite",false,true);
    const auto base=src.meta("bundle.base");
    Paths paths{{base+"/.generate",transport.string()}};
    const auto old=parse(src.meta("bundle.inventory"));
    for(const auto &kind:{"sources","design","wrappers"}) {
        const auto before=strings(old[kind]),after=strings(inventory[kind]);
        require(before.size()==after.size(),"cached source structure changed");
        for(size_t i=0;i<before.size();++i)paths[before[i]]=after[i];
    }
    for(const auto &row:src.rows("SELECT path FROM bundle_files")) {
        const auto relative=fs::path(text(row["path"]));
        auto body=text(src.rows("SELECT body FROM bundle_files WHERE path=?",{row["path"]}).front()["body"]);
        if(relative.extension()==".json")body=canonical(relocate(parse(body),paths))+"\n";
        if(relative.extension()==".sv") {
            // Keep the generated provenance header when relocating paths. Only
            // move the path in that comment; never rewrite property tokens.
            std::istringstream in(body);std::ostringstream out;std::string line;
            while(std::getline(in,line)) {
                if(line.rfind("// boundary:",0)==0) {
                    const auto first=line.find('('),last=line.rfind(')');
                    if(first!=line.npos&&last>first) {
                        const auto path=line.substr(first+1,last-first-1);const auto found=paths.find(path);
                        if(found!=paths.end())line.replace(first+1,last-first-1,found->second);
                    }
                }
                out<<line<<'\n';
            }
            body=out.str();
        }
        writeText(transport/relative,body);
    }
    const auto dest=transport/"log/consflow.sqlite";
    src.exec("VACUUM INTO ?",{str(dest.string())});
    Database db(dest);
    for(const auto &row:db.rows("SELECT key,value FROM metadata")) {
        auto value=text(row["value"]);J parsed;
        try {parsed=parse(value);}catch(...){parsed=str(value);}
        const auto fixed=relocate(parsed,paths);
        db.meta(text(row["key"]),parsed.is_string()?text(fixed):canonical(fixed));
    }
    db.exec("DROP TABLE bundle_files");db.exec("DROP TABLE check_templates");
    db.exec("DELETE FROM metadata WHERE key LIKE 'bundle.%' OR key='check.templates.index'");
}
int lowerMain(int argc,char **argv) {
    try {
        if(argc==3 && std::string(argv[2])=="--help") {
            std::cout<<"veval lower DESIGN.sby [TASK] --out BUNDLE [-j N] [--placement single-path|split]\n"
                "Generate all properties once; no solver is run. Existing identical bundles are reused.\n"
                "-j N or -jN sets the worker limit for module synthesis, property lowering and output preparation (default 1).\n"
                "--generation-threads N is an alias for -j N.\n"
                "The SBY script must specify an existing module with prep -top TOP. Packages are not supported.\n";
#ifdef __APPLE__
            std::cout << "macOS supervises one session per stage and applies no aggregate memory cap.\n";
#endif
            return 0;
        }
        require(argc>=3,"usage: veval lower DESIGN.sby [TASK] --out BUNDLE");
        veval::util::command_runner::installCancellationHandlers();
        const auto binary=veval::util::currentExecutable();environment(binary);
        fs::path out;std::string task,placement;
        std::size_t generationThreads=1;
        for(int i=3;i<argc;++i){const std::string a=argv[i];
            if(a=="--out"||a=="--placement"){require(i+1<argc,"missing option value");const std::string v=argv[++i];if(a=="--out")out=fs::absolute(v);else placement=v;}
            else if(a=="--generation-threads"||a.rfind("-j",0)==0){
                std::string value;
                if(a=="-j"||a=="--generation-threads"){
                    require(i+1<argc,a+" needs a value");value=argv[++i];
                }else value=a.substr(2);
                const auto parsed=std::from_chars(value.data(),value.data()+value.size(),generationThreads);
                require(parsed.ec==std::errc{} && parsed.ptr==value.data()+value.size() && generationThreads>0,
                        a+" needs a positive whole number");
            }
            else {require(task.empty() && a[0]!='-',"unexpected argument: "+a);task=a;}}
        require(!out.empty(),"--out is required");
        require(placement.empty()||placement=="single-path"||placement=="split","invalid placement");
        Process p;p.root=fs::path(VEVAL_WORKFLOW_ROOT);p.out=out.parent_path()/(".lower-prepare-"+std::to_string(::getpid()));
        fs::create_directories(p.out);
        const auto input=prepareTask(p,fs::canonical(argv[2]),task);
        completed(p.run("source-preparation",{binary.string(),"__check-source",(p.out/"task/source-request.json").string(),(p.out/"task/prepared").string()}), true);
        const auto inventory=jsonFile(p.out/"task/prepared/prepared.json");
        const auto effective=effectiveConfiguration(input,object({{"placement",str(placement)}}));
        ensureLowering(p,input,inventory,effective,binary,out,generationThreads);
        fs::remove_all(p.out);return 0;
    }catch(const std::exception &e){std::cerr<<"veval lower: "<<e.what()<<'\n';return interrupted()?130:2;}
}
}
