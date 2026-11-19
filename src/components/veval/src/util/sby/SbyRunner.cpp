#include "util/process/CurrentExecutablePath.h"
#include "util/process/ProcessSession.h"
#include "SbyRunner.h"
#include "../process/RunWithResourceLimits.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <regex>
#include <sstream>

namespace veval::verification {
static double numeric(const J &j,double fallback=0) {return j.is_number()?std::stod(j.value):fallback;}
static void checkpoint(double deadline) {
    if(cancelled())throw Expired("cancelled");
    if(monotonic()>=deadline)throw Expired("attempt budget exhausted");
}
static std::string quote(const std::string &s) {
    if(s.find_first_of("\r\n")!=s.npos)throw Unsupported("newline in Yosys path");
    std::string out="\"";for(char c:s){if(c=='\\'||c=='\"')out+='\\';out+=c;}return out+'"';
}
static std::string shellQuote(const std::string &s) {
    std::string out="'";
    for(char c:s)out+=c=='\''?"'\\''":std::string(1,c);
    return out+"'";
}
static fs::path inductionOutputFilter(const fs::path &dir,const std::string &smtbmc) {
    // Suppress induction diagnostic dumps, keeping solver options and base-case traces.
    const auto path=dir/"smtbmc-output.sh";
    std::string pythonEnvironment;
    for (const auto name : {"PYTHONHOME", "PYTHONEXECUTABLE"}) {
        if (const auto value = std::getenv(name))
            pythonEnvironment += std::string("export ") + name + "=" + shellQuote(value) + "\n";
        else pythonEnvironment += std::string("unset ") + name + "\n";
    }
    write(path,"#!/usr/bin/env bash\nset -euo pipefail\n" + pythonEnvironment +
        "induction=false\nfor arg in \"$@\"; do [[ $arg != -i ]] || induction=true; done\n"
        "args=()\nwhile (($#)); do\n"
        "  if $induction; then\n"
        "    case $1 in\n"
        "      --dump-vcd|--dump-yw|--dump-vlogtb|--dump-smtc) shift 2; continue;;\n"
        "    esac\n  fi\n  args+=(\"$1\"); shift\ndone\n"
        // Prevent SBY from treating an induction diagnostic as a missing base-case trace.
        "if $induction; then\n"
        "  "+shellQuote(smtbmc)+" \"${args[@]}\" | sed 's/^\\(## [0-9: ]*\\)Assert failed in /\\1Induction diagnostic: Assert failed in /'\n"
        "else\n  exec "+shellQuote(smtbmc)+" \"${args[@]}\"\nfi\n");
    fs::permissions(path,fs::perms::owner_exec,fs::perm_options::add);
    return path;
}
static J reference(const fs::path &p) {return object({{"path",str(absolute(p.string()))},{"sha256",str(fileDigest(p))}});}
template <typename Visitor>
static void readFields(const fs::path &path, const Visitor &visit, const char *malformed = nullptr) {
    std::istringstream input(read(path));
    for (std::string line; std::getline(input, line);) {
        const auto tab = line.find('\t');
        if (tab == line.npos) {
            if (malformed) throw Unsupported(malformed);
            continue;
        }
        visit(line.substr(0, tab), line.substr(tab + 1));
    }
}
static std::string readDesign(const J &request,bool defer=false) {
    std::string flags=truth(request["systemverilog"])?" -sv":"";
    if(defer)flags+=" -defer";
    for(const auto &p:items(request["include_paths"]))flags+=" -I"+quote(text(p));
    for(const auto &d:items(request["defines"]))flags+=" -D"+quote(text(d));
    std::string out;for(const auto &f:items(request["design_files"]))out+="read_verilog"+flags+" "+quote(text(f))+"\n";
    return out;
}
static fs::path plugin(const std::string &name,const std::string &env) {
    // A missing plugin is an installation error, not an unsupported property.
    if (const auto override = std::getenv(env.c_str())) {
        if (fs::is_regular_file(override)) return fs::absolute(override);
        throw std::runtime_error("plugin unavailable: " + std::string(override));
    }
    const auto build = std::getenv("CONSFLOW_BUILD_DIR");
    const auto path = (build ? fs::path(build) : veval::util::currentExecutable().parent_path()) / name;
    if (!fs::is_regular_file(path)) throw std::runtime_error("plugin unavailable: " + path.string());
    return path;
}
// CompileSvaMonitor reports allocation failure with this exit status.
constexpr int kHelperOutOfMemory=4;
static std::string helperDiagnostic(const fs::path &log) {
    // Helper scratch is discarded after publication. Retain a bounded error
    // excerpt in the Result instead of leaving only a path to a deleted log.
    std::ifstream input(log,std::ios::binary|std::ios::ate);
    const auto end=input.tellg();
    if(end<=0)return "";
    const auto count=std::min<std::streamoff>(end,4096);
    input.seekg(end-count);
    std::string value(static_cast<std::size_t>(count),'\0');
    input.read(value.data(),count);value.resize(static_cast<std::size_t>(input.gcount()));
    if(end>count) {
        const auto newline=value.find('\n');
        if(newline!=value.npos && newline+1<value.size())value.erase(0,newline+1);
        // The excerpt can begin in the middle of a UTF-8 character.
        while(!value.empty() && (static_cast<unsigned char>(value.front())&0xc0)==0x80)
            value.erase(0,1);
    }
    value=trim(value);
    return value.empty()?"":"; diagnostic: "+value;
}
static void checked(const Execution &done,const fs::path &log,const std::string &phase) {
    if(!done.cleanupComplete)throw std::runtime_error(phase+": cleanup unconfirmed: "+done.error);
    if(done.timedOut||done.stopped)throw Expired(phase+": stopped before completion");
    // A helper killed by a signal or out of memory did not refuse the
    // property; it failed, and the row stays undecided.
    if(done.exitCode>=128||done.exitCode==kHelperOutOfMemory)
        throw std::runtime_error(phase+" did not complete (exit "+std::to_string(done.exitCode)+")"+helperDiagnostic(log)+"; see "+log.string());
    if(done.exitCode!=0)throw Unsupported(phase+" failed"+helperDiagnostic(log)+"; see "+log.string());
}
static Encoding nativeEncode(const SbyProduct &p,const std::string &label,const std::string &body,
                             size_t count,const fs::path &dir,double deadline,unsigned long long memory,bool shared) {
    const auto clock=samplingClock(p,body);
    fs::create_directories(dir);std::string source="module consflow_sva_input(\n";bool first=true;
    // A tied wire is a name the sentence may read like a port.
    auto names=p.ports;names.insert(p.tiedWires.begin(),p.tiedWires.end());
    for(const auto &[name,width]:names) {
        if(!first)source+=",\n";first=false;
        std::string range=width>1?"["+std::to_string(width-1)+":0]":"";
        std::smatch m;
        const std::regex declaration("(?:^|\\n)\\s*(?:wire|input|output|inout)\\s*"
            "(?:(?:logic|wire|reg)\\s+)?(?:signed\\s+)?(\\[[^\\]]+\\])?\\s*" + name + "\\s*[,;]");
        if (std::regex_search(p.prelude, m, declaration) && m[1].matched) range = m[1];
        source+="input "+std::string(p.signedPorts.count(name)?"signed ":"")+range+" "+name;
    }
    std::string declarations;
    for (const auto& block : items(p.blocks)) if (text(block["label"])==label)
        declarations=text(block["local_declarations"]);
    source+="\n);\n";
    for (const auto& [name,definition]:p.functions)
        if (body.find(name)!=body.npos || declarations.find(name)!=declarations.npos)
            source+=definition+"\n";
    if (declarations.empty()) source+=" p: assert property ("+body+");\n";
    else {
        const auto eventEnd=balancedEnd(body,body.find('('));
        source+=" property __cf_selected;\n "+declarations+"\n "+body.substr(eventEnd+1)+";\n endproperty\n";
        source+=" p: assert property ("+body.substr(0,eventEnd+1)+" __cf_selected);\n";
    }
    source+="endmodule\n";
    write(dir/"input.sv",source);
    auto log=dir/"compiler.log";
    auto done = execute({helper("consflow_sva_monitor", "CONSFLOW_SVA_MONITOR").string(),
                         (dir / "input.sv").string(), "--top", "consflow_sva_input", "--clock", clock,
                         "--prefix", "veval_sva_" + std::to_string(count), "--out", dir.string()},
                        log, dir, deadline, memory, 2, shared);
    checked(done,log,"native SVA compilation of "+label);
    Encoding code;
    code.declarations.push_back(read(dir / "monitor.v"));
    readFields(dir / "monitor.tsv", [&](const auto &key, const auto &value) {
        if (key == "generated") code.generated.insert(value);
        else if (key == "guard") code.guard = value;
        else if (key == "expr") code.expr = value;
        else if (key == "delay") code.delay = std::stoul(value);
        else if (key == "mode") code.mode = value;
    }, "bad native SVA receipt");
    if(code.guard.empty()||code.expr.empty())throw Unsupported("incomplete native SVA receipt");
    return code;
}
static bool nameBoundary(char c){return !std::isalnum(static_cast<unsigned char>(c))&&c!='_'&&c!='$'&&c!='.';}
static void replaceReference(std::string &body,const std::string &ref,const std::string &alias) {
    size_t pos = 0;
    while ((pos = body.find(ref, pos)) != body.npos) {
        const auto end = pos + ref.size();
        if ((!pos || nameBoundary(body[pos - 1])) && (end == body.size() || nameBoundary(body[end]))) {
            body.replace(pos, ref.size(), alias);
            pos += alias.size();
        } else pos = end;
    }
}
static J constants(SbyProduct &p,const J &request,const fs::path &dir,double deadline,unsigned long long memory,bool shared) {
    std::set<std::string> names;static const std::regex reference(R"(\$unit::[A-Za-z_][A-Za-z_0-9]*)");
    for(const auto &block:items(p.blocks))if(text(block["kind"])=="assume"||block["label"]==request["label"]) {
        auto body=text(block["body"]);
        for(std::sregex_iterator i(body.begin(),body.end(),reference),e;i!=e;++i)names.insert(i->str());
    }
    if(names.empty())return {};
    if(request["job"].is_null())throw Unsupported("compilation-unit constants require original Job frontend context");
    J wanted=list();for(const auto &name:names)append(wanted,str(name));
    const auto input=dir/"constants-request.json",output=dir/"constants.json",log=dir/"constants.log";
    save(input,object({{"job",request["job"]},{"names",wanted}}));
    checked(execute({helper("consflow_constants", "CONSFLOW_CONSTANTS").string(),
                     "--request", input.string(), "--out", output.string()},
                    log, dir, deadline, memory, 2, shared), log, "compilation-unit constants");
    J audit=parse(read(output));if(text(audit["status"])!="checked")throw Unsupported("constant context not resolved");
    J bodies=list();
    for(auto &block:p.blocks.make_array())if(text(block["kind"])=="assume"||block["label"]==request["label"]) {
        auto body=text(block["body"]),original=body;
        for(const auto &b:items(audit["bindings"]))replaceReference(body,text(b["name"]),"("+text(b["literal"])+")");
        put(block, "body", str(body));
        append(bodies, object({{"kind", block["kind"]}, {"label", block["label"]},
                              {"original_body", str(original)}, {"translated_body", str(body)}}));
    }
    put(audit,"bodies",bodies);return audit;
}
static J observations(SbyProduct &p,J &request,const fs::path &dir,double deadline,unsigned long long memory,bool shared) {
    if(items(p.observations).empty())return {};
    auto module=text(request["module"]);
    auto identifierLog=dir/"identifier.log";
    checked(execute({helper("consflow_yosys_identifier", "CONSFLOW_YOSYS_IDENTIFIER").string(), "module", module},
                    identifierLog, dir, deadline, memory, 2, shared), identifierLog, "Yosys identifier");
    module=trim(read(identifierLog));
    J bodies=list();for(const auto &b:items(p.blocks))if(text(b["kind"])=="assume"||b["label"]==request["label"])append(bodies,b);
    auto selection=dir/"observation-request.json",bindings=dir/"observation-bindings.json",design=dir/"observation-design.il";
    save(selection,object({{"module",str(module)},{"instance",str(p.instance)},{"observations",p.observations},{"bodies",bodies}}));
    auto script=dir/"observation.ys",log=dir/"observation.stdout";
    write(script,readDesign(request,true)+"hierarchy -check -top "+module+
          "\nplugin -i "+quote(plugin("consflow_relations.so","CONSFLOW_RELATIONS").string())+
          "\nconsflow_proc\nplugin -i "+quote(plugin("consflow_observation.so","CONSFLOW_OBSERVATION").string())+
          "\nconsflow_observe "+quote(selection.string())+" "+quote(bindings.string())+"\nwrite_rtlil "+quote(design.string())+"\n");
    const auto done=execute({program("yosys",text(request["oss_cad_bin"])),"-Q","-T","-s",script.string()},log,dir,deadline,memory,2,shared);
    checked(done,log,"observation compilation");checkpoint(deadline);
    J routing=parse(read(bindings));std::vector<std::string> declarations,connections;size_t index=0;
    for(auto &item:routing.make_object()["mapping"].make_array()) {
        const auto &actual = item["actual"];
        const auto width = static_cast<int>(numeric(actual["width"]));
        const auto lo = static_cast<int>(numeric(actual["offset"])), hi = lo + width - 1;
        if(width<=0)throw Unsupported("invalid observation width");
        auto alias="veval_observe_"+std::to_string(index++);
        if(p.ports.count(alias)||std::regex_search(p.prelude,std::regex("\\b"+alias+"\\b")))throw Unsupported("observation alias collision");
        int left=truth(actual["increasing"])?lo:hi,right=truth(actual["increasing"])?hi:lo;
        auto range=width>1||lo?" ["+std::to_string(left)+":"+std::to_string(right)+"]":"";
        declarations.push_back("  wire"+std::string(truth(actual["signed"])?" signed":"")+range+" "+alias+";");
        connections.push_back("."+text(item["output"])+"("+alias+")");p.ports[alias]=width;
        if(truth(actual["signed"]))p.signedPorts.insert(alias);
        for(auto &block:p.blocks.make_array()){auto body=text(block["body"]);replaceReference(body,text(item["reference"]),alias);put(block,"body",str(body));}
        put(item,"alias",str(alias));
    }
    // The generated product has exactly one named DUT instance; its module
    // was checked against the product by the caller before this routing step.
    std::smatch match;std::regex instance(R"(\bu_dut\s*\()");
    if(!std::regex_search(p.prelude,match,instance))throw Unsupported("observation DUT instance absent");
    auto open=match.position()+match.length()-1;auto close=balancedEnd(p.prelude,open);
    if(!connections.empty()) {
        std::string extra=",\n    ";for(size_t i=0;i<connections.size();++i){if(i)extra+=",\n    ";extra+=connections[i];}
        p.prelude.insert(close,extra);
        auto moduleName=p.prelude.find("module "+p.module);
        if(moduleName==p.prelude.npos)throw Unsupported("wrapper module absent");
        auto moduleStart=p.prelude.find(';',balancedEnd(p.prelude,p.prelude.find('(',moduleName)));
        if(moduleStart==p.prelude.npos)throw Unsupported("wrapper header incomplete");
        std::string wires="\n";for(const auto &s:declarations)wires+=s+"\n";p.prelude.insert(moduleStart+1,wires);
    }
    put(routing,"design",str(design.string()));put(routing,"format",str("rtlil"));
    put(routing,"preprocess",object({{"wall_s",num(done.elapsed)},{"exit_code",num(done.exitCode)},
        {"address_space_bytes",memory?num(memory):J()}}));
    fs::remove(bindings);return routing;
}
static J detail(const std::string &output,const std::string &mode,unsigned depth,const std::string &engines) {
    static const std::regex basecasePass("returned pass for basecase|Status returned by engine for basecase: pass");
    static const std::vector<std::pair<std::string, std::regex>> phases{
        {"basecase", std::regex("engine_0 \\(([^)]*)\\) returned (\\S+) for basecase")},
        {"induction", std::regex("engine_0 \\(([^)]*)\\) returned (\\S+) for induction")}};
    J d=object({{"mode",str(mode)},{"depth",num(depth)},{"engine",str(engines)},
               {"induction_closed",boolean(output.find("Temporal induction successful")!=output.npos)},
               {"basecase_completed",boolean(std::regex_search(output,basecasePass))}});
    for(const auto &[phase, pattern] : phases) {
        std::smatch m;
        if(std::regex_search(output,m,pattern)) {put(d,phase,str(m[2]));put(d,"engine",str(m[1]));}
    }
    if(truth(d["induction_closed"])) {
        static const std::regex re("Trying induction in step ([0-9]+)");unsigned best=depth,last=depth;bool found=false;
        for(std::sregex_iterator i(output.begin(),output.end(),re),e;i!=e;++i){last=std::stoul((*i)[1]);best=std::min(best,last);found=true;}
        if(found){put(d,"induction_k",num(best));put(d,"induction_last_step",num(last));}
    }
    static const std::vector<std::pair<std::string, std::regex>> patterns{
        {"reached_bound", std::regex("Reached bound ([0-9]+)")},
        {"last_checked_step", std::regex("Checking assertions in step ([0-9]+)")}};
    for(const auto &[key,pattern]:patterns)
        for(std::sregex_iterator i(output.begin(),output.end(),pattern),e;i!=e;++i)
            put(d,key,num(std::stoul((*i)[1])));
    return d;
}
static J auditPremises(const J &stage, const J &witnesses, double deadline) {
    J traced = list(), skipped = list(), violations = list();
    for (const auto &path : items(stage["traces"])) {
        const bool induction = fs::path(text(path)).filename().string().find("induct") != std::string::npos;
        append(induction ? skipped : traced, path);
    }
    J audit = object({{"outcome", str("all-premises-hold")}, {"checked", num(0)},
        {"violations", violations}, {"traces_audited", traced}, {"traces_skipped", skipped}});
    const auto base = text(stage["engine_detail"]["basecase"]);
    if (!base.empty() && base != "FAIL") {
        put(audit, "outcome", str("not-a-basecase-failure"));
        return audit;
    }
    if (items(traced).empty()) {
        put(audit, "outcome", str("no-trace"));
        return audit;
    }
    std::vector<std::pair<std::string, J>> witnessSignals;
    for (const auto &witness : items(witnesses))
        witnessSignals.emplace_back("veval_prem_" + text(witness["index"]), witness["label"]);
    size_t checkedCount = 0;
    for (const auto &path : items(traced)) {
        // A missing reported trace is an error, not an unsupported encoding.
        std::ifstream in(text(path));
        if (!in) throw std::runtime_error("trace unreadable: " + text(path));
        std::map<std::string, std::string> ids, values;
        std::set<std::string> declared;
        bool frame = false;
        size_t step = 0;
        auto checkFrame = [&] {
            if (!frame) return;
            for (const auto &[name, label] : witnessSignals) {
                if (!declared.count(name)) {
                    put(audit, "outcome", str("witnesses-absent-from-trace"));
                    continue;
                }
                if (values[name] != "1")
                    append(violations, object({{"trace", path}, {"premise", label},
                        {"frame", num(step)}, {"value", str(values[name])}}));
            }
            ++step;
        };
        for (std::string line; std::getline(in, line);) {
            checkpoint(deadline);
            line = trim(line);
            if (line.empty()) continue;
            if (line.rfind("$var", 0) == 0) {
                const auto parts = words(line);
                if (parts.size() >= 6 && (parts[1] == "wire" || parts[1] == "reg") &&
                    parts[2] == "1" && parts[4].rfind("veval_prem_", 0) == 0) {
                    ids[parts[3]] = parts[4];
                    declared.insert(parts[4]);
                    values[parts[4]] = "x";
                }
            } else if (line[0] == '#') {
                checkFrame();
                frame = true;
            } else if (frame && (line[0] == 'b' || line[0] == 'B')) {
                const auto parts = words(line);
                if (parts.size() == 2 && ids.count(parts[1])) {
                    const auto value = parts[0].substr(1);
                    const auto at = value.find_first_not_of('0');
                    values[ids[parts[1]]] = at == value.npos ? "0" : value.substr(at);
                }
            } else if (frame && std::string("01xz").find(line[0]) != std::string::npos &&
                       ids.count(line.substr(1))) {
                values[ids[line.substr(1)]] = line.substr(0, 1);
            }
        }
        checkFrame();
        checkedCount += declared.size();
        if (!frame && !items(witnesses).empty()) put(audit, "outcome", str("no-trace"));
    }
    if (!items(violations).empty()) put(audit, "outcome", str("premise-false-on-trace"));
    put(audit, "checked", num(checkedCount));
    put(audit, "violations", violations);
    return audit;
}
static J livenessReduction(const fs::path &path) {
    J reduction = object({{"required", boolean(true)}, {"status", str("missing")}, {"generated", list()}});
    if (fs::exists(path)) {
        readFields(path, [&](const auto &key, const auto &value) {
            if (key == "generated" || key == "state_cell") append(reduction.make_object()[key], str(value));
            else put(reduction, key, key == "state_bits" ? num(std::stod(value)) : str(value));
        });
        put(reduction, "status", str("compiled"));
        put(reduction, "audit_ref", reference(path));
    }
    if (text(reduction["schema"]) != "consflow-live2safety/1" || numeric(reduction["state_bits"]) <= 0)
        put(reduction, "status", str("invalid"));
    return reduction;
}
static bool auditLiveness(J &stage, const std::string &module, const fs::path &dir,
                          double deadline, unsigned long long memory, bool shared) {
    const auto &reduction = stage["liveness_reduction"];
    if (text(reduction["status"]) != "compiled") return false;
    for (const auto &trace : items(stage["traces"])) {
        if (fs::path(text(trace)).filename().string().find("induct") != std::string::npos) continue;
        const auto log = dir / "live-trace.log";
        const auto done = execute({helper("consflow_live_trace", "CONSFLOW_LIVE_TRACE").string(),
                                   text(trace), module, text(reduction["audit_ref"]["path"])},
                                  log, dir, deadline, memory, 2, shared);
        if (done.exitCode != 0 || !done.cleanupComplete || done.timedOut || done.stopped) continue;
        J lasso = object();
        readFields(log, [&](const auto &key, const auto &value) {
            const bool numericField = key == "loop_start" || key == "loop_end" || key == "state_bits";
            put(lasso, key, numericField ? num(std::stod(value)) : str(value));
        });
        put(lasso, "trace", trace);
        put(lasso, "audit_ref", reduction["audit_ref"]);
        put(stage, "liveness_trace", lasso);
        if (text(lasso["status"]) == "checked" && text(lasso["kind"]) == "lasso" &&
            numeric(lasso["loop_start"]) >= 0 && numeric(lasso["loop_end"]) > numeric(lasso["loop_start"]) &&
            equal(lasso["state_bits"], reduction["state_bits"])) return true;
    }
    return false;
}
J runSby(J request, double deadline) {
    const fs::path dir = absolute(text(request["work_dir"]));
    fs::create_directories(dir);
    J record = object({{"schema_version", num(1)}, {"request", request}, {"stages", list()}});
    auto finish = [&](const std::string &verdict, const std::string &why) {
        put(record, "sby_verdict", str(verdict));
        put(record, "sby_detail", str(why));
        save(dir / "cell.json", record);
        return record;
    };
    try {
        checkpoint(deadline);
        SbyProduct p(text(request["product"]));
        put(record, "product", object({{"path", str(p.path.string())},
            {"sha256", str(fileDigest(p.path))}, {"module", str(p.module)},
            {"instance", str(p.instance)}, {"premise_lines", p.premises}}));
        const auto memory = static_cast<unsigned long long>(numeric(request["address_space_bytes"]));
#ifdef __APPLE__
        const bool shared = truth(request["process_supervised"]);
        if (shared) {
            if (::getsid(0) != ::getpid())
                throw std::runtime_error("supervised SBY adapter must be a session leader on macOS");
            std::size_t otherMembers = 0;
            std::string sessionError;
            if (!veval::util::inspectSessionExcept(::getpid(), ::getpid(), otherMembers, sessionError) || otherMembers)
                throw std::runtime_error("supervised SBY adapter session is not exclusively owned: " + sessionError);
        }
#else
        const bool shared = false;
#endif
        const auto constantContext = constants(p, request, dir, deadline, memory, shared);
        if (!constantContext.is_null()) put(record, "source_constants", constantContext);
        const auto observed = observations(p, request, dir, deadline, memory, shared);
        if (!observed.is_null()) put(record, "observations", observed);
        const bool multiclock = truth(request["multiclock"]);
        const auto wrapper = buildWrapper(p, {text(request["label"])},
            [&](const auto &label, const auto &body, size_t count) {
                return nativeEncode(p, label, body, count, dir / "sva" / label, deadline, memory, shared);
            }, multiclock);
        if (items(wrapper["asserts"]).size() != 1 ||
            text(*items(wrapper["asserts"]).begin()) != text(request["label"]))
            return finish("refused-by-translator", "target not encodable: " + canonical(wrapper["dropped_asserts"]));

        write(dir / "top.v", text(wrapper["text"]));
        J evidence = object();
        for (const auto &[key, value] : members(wrapper))
            if (key != "text" && key != "reset") put(evidence, key, value);
        put(evidence, "path", str((dir / "top.v").string()));
        put(evidence, "sha256", str(fileDigest(dir / "top.v")));
        put(evidence, "assume_count", num(items(wrapper["assumes"]).size()));
        put(evidence, "reset_contract", wrapper["reset"]);
        put(evidence, "product_premise_lines", p.premises);
        put(record, "wrapper", evidence);
        if (truth(request["prepare_only"])) return finish("prepared", "wrapper written; no engine was run");

        const auto oss = text(request["oss_cad_bin"]);
        const auto engines = text(request["engines"], "smtbmc boolector");
        const auto path = oss + ":" + (std::getenv("PATH") ? std::getenv("PATH") : "");
        if (!oss.empty()) ::setenv("PATH", path.c_str(), 1);
        const auto sby = program("sby", oss);
        auto design = readDesign(request, true);
        if (!observed.is_null()) {
            // RTLIL omits packages. Retain those declarations, but discard the deferred modules.
            static const std::regex qualified(R"([A-Za-z_][A-Za-z_0-9]*::[A-Za-z_])");
            design = std::regex_search(text(wrapper["text"]), qualified) ? design + "delete\n" : "";
            design += "read_rtlil " + quote(text(observed["design"])) + "\n";
        }
        put(record, "read_design", str(design));
        fs::path smtbmcFilter;
        if (engines.rfind("smtbmc ", 0) == 0) {
            smtbmcFilter = inductionOutputFilter(dir, program("yosys-smtbmc", oss));
            put(record, "induction_trace_policy",
                str("omit diagnostic dumps; base-case trace and premise audit unchanged"));
        }
        const bool live = !items(wrapper["liveness"]).empty();
        for (const auto &rung : items(request["stages"])) {
            checkpoint(deadline);
            if (items(rung).size() != 3 && items(rung).size() != 4)
                throw Unsupported("stage needs three or four fields");
            auto i = items(rung).begin();
            const auto name = text(*i++), mode = text(*i++);
            const unsigned depth = static_cast<unsigned>(numeric(*i++));
            const double cap = i == items(rung).end()
                ? numeric(request["wall_limit_s"], std::numeric_limits<double>::infinity())
                : numeric(*i, std::numeric_limits<double>::infinity());
            static const std::regex stageName("[A-Za-z0-9_-]+");
            if (!std::regex_match(name, stageName)) throw Unsupported("invalid stage name");
            const double stageDeadline = std::min(deadline, monotonic() + cap);
            const auto remaining = stageDeadline - monotonic();
            const std::string timeout = std::isfinite(remaining)
                ? "timeout " + std::to_string(static_cast<unsigned long long>(std::ceil(remaining))) + "\n" : "";
            const auto config = dir / (name + ".sby"), work = dir / (name + ".work");
            const auto log = dir / (name + ".stdout");
            const auto relation = plugin("consflow_relations.so", "CONSFLOW_RELATIONS");
            const std::string process = "plugin -i " + quote(relation.string()) +
                "\nhierarchy -check -top " + p.module + "\nconsflow_proc\n";
            // Expose ROM constants to induction, retaining shared unspecified bits and writable RAMs.
            std::string passes = "consflow_relations veval_relation-audit.json\nmemory_map -rom-only -keepdc\nopt_clean\n";
            put(record, "readonly_memory_policy",
                str("map ROMs; keep shared unspecified bits; leave writable memories unchanged"));
            if (live)
                passes += "plugin -i " + quote(plugin("consflow_live2safety.so", "CONSFLOW_LIVE2SAFETY").string()) +
                    "\nchformal -lower\nasync2sync\ndffunmap\nmemory_map\nconsflow_live2safety clk veval_live-audit.tsv\n";
            // Relative paths avoid SBY splitting an absolute path containing spaces.
            write(config, "[options]\nmode " + mode + "\ndepth " + std::to_string(depth) + "\n" +
                (multiclock ? "multiclock on\n" : "") + timeout + "\n[engines]\n" + engines +
                "\n\n[script]\n" + design + "read_verilog -sv -formal -noautowire top.v\n" + process +
                "prep -top " + p.module + " -flatten\n" + passes + "\n[files]\ntop.v\n");
            // Pin the Yosys used for observation routing and its plugin ABI.
            std::vector<std::string> command{sby, "--yosys", shellQuote(program("yosys", oss))};
            if (!smtbmcFilter.empty())
                command.insert(command.end(), {"--smtbmc", shellQuote(smtbmcFilter.string())});
            command.insert(command.end(), {"-f", "-d", work.filename().string(), config.filename().string()});
            J cmd = list();
            for (const auto &arg : command) append(cmd, str(arg));
            const auto done = execute(command, log, dir, stageDeadline, memory,
                                      numeric(request["cleanup_margin_s"], 2), shared);

            std::string status = "ERROR", raw;
            if (fs::is_regular_file(work / "status")) {
                raw = trim(read(work / "status"));
                const auto tokens = words(raw);
                if (!tokens.empty()) status = tokens[0];
            }
            if (done.timedOut) status = "TIMEOUT";
            const auto output = read(log);
            J traces = list();
            std::vector<fs::path> ordered;
            if (fs::is_directory(work))
                for (const auto &entry : fs::recursive_directory_iterator(work))
                    if (entry.is_regular_file() && entry.path().extension() == ".vcd")
                        ordered.push_back(entry.path());
            std::sort(ordered.begin(), ordered.end());
            for (const auto &trace : ordered) append(traces, str(trace.string()));
            J stage = object({{"stage", str(name)}, {"mode", str(mode)}, {"depth", num(depth)},
                {"status", str(status)}, {"sby_status_raw", raw.empty() ? J() : str(raw)},
                {"returncode", num(done.exitCode)}, {"external_timeout", boolean(done.timedOut)},
                {"wall_s", num(done.elapsed)}, {"solving_s", num(done.solving)}, {"cleanup_s", num(done.cleanup)},
                {"remaining_at_start_s", finiteOrNull(remaining)}, {"wall_limit_s", finiteOrNull(remaining)},
                {"engine_detail", detail(output, mode, depth, engines)}, {"engines", str(engines)},
                {"config", str(config.string())}, {"work_dir", str(work.string())}, {"stdout", str(log.string())},
                {"command", cmd}, {"traces", traces}, {"max_rss_kb", num(done.maxRssKb)},
                {"reaped", object({{"complete", boolean(done.cleanupComplete)}, {"survivors", list()},
                    {"errors", done.error.empty() ? list() : list({str(done.error)})}})}});
            const auto relationAudit = work / "src/veval_relation-audit.json";
            if (fs::exists(relationAudit)) put(stage, "internal_design_relations", parse(read(relationAudit)));
            if (live) put(stage, "liveness_reduction", livenessReduction(work / "src/veval_live-audit.tsv"));
            if (!done.cleanupComplete) {
                append(record.make_object()["stages"], stage);
                return finish("blocked", "process cleanup unconfirmed: " + done.error);
            }
            if (status == "FAIL") {
                const auto audit = auditPremises(stage, wrapper["premise_witnesses"], deadline);
                put(record, "premise_audit", audit);
                const bool liveChecked = !live || auditLiveness(stage, p.module, dir, deadline, memory, shared);
                append(record.make_object()["stages"], stage);
                if (text(audit["outcome"]) == "all-premises-hold" && liveChecked)
                    return finish("refuted", "counterexample honours every compiled premise");
                return finish("refuted-unaudited", "counterexample confirmation incomplete");
            }
            append(record.make_object()["stages"], stage);
            save(dir / "cell.json", record);
            if (status == "PASS") {
                const auto &receipt = stage["engine_detail"];
                const bool proved = engines == "abc pdr" ||
                    (truth(receipt["induction_closed"]) && truth(receipt["basecase_completed"]));
                const bool reductionComplete = !live || text(stage["liveness_reduction"]["status"]) == "compiled";
                if (mode == "prove" && proved && reductionComplete) {
                    std::string how = engines + " complete proof receipt; ";
                    if (engines == "abc pdr")
                        how += engines + ", unbounded, at depth " + std::to_string(depth);
                    else
                        how += "temporal induction at depth " + std::to_string(depth) +
                            (receipt["induction_k"].is_null() ? std::string() :
                             ", k=" + std::to_string(static_cast<unsigned long>(numeric(receipt["induction_k"]))));
                    return finish("proved-by-induction", how);
                }
                if (mode == "bmc" && !live)
                    return finish("bounded-clean-to-K=" + std::to_string(depth), "BMC exhausted the requested bound");
                return finish("unknown-after-escalation", "PASS lacks the complete proof receipt for the requested method");
            }
            // Cancellation may precede status publication; it must remain retryable even with ERROR.
            if (done.stopped || cancelled()) return finish("unknown-after-escalation", "cancelled");
            if (status == "ERROR") return finish("blocked", "SBY or Yosys reported an error; see " + log.string());
        }
        return finish("unknown-after-escalation", "no complete proof or audited refutation within the requested ladder");
    } catch (const Expired &error) {
        return finish("adapter-timeout", error.what());
    } catch (const Unsupported &error) {
        return finish("refused-by-translator", error.what());
    }
}
} // namespace veval::verification
