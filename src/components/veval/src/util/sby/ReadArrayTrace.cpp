// Native, typed static-array observation planning and read-only scalar routing.
#include "../database/Json.h"
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
using namespace veval::result_validation;
namespace {
void need(bool ok, const std::string& why) { if (!ok) throw std::runtime_error(why); }
const J& at(const J& j, const std::string& key) { const auto& m=members(j); auto it=m.find(key); need(it!=m.end(), "missing field "+key); return it->second; }
bool has(const J& j, const std::string& key) { const auto& m=members(j); return m.find(key)!=m.end(); }
long long integer(const J& j) {
    const auto s = text(j);
    need(std::regex_match(s, std::regex("-?[0-9]+")), "non-integral array metadata");
    return std::stoll(s);
}
std::string read(const fs::path& p) { std::ifstream f(p); need(bool(f), "cannot read " + p.string()); return {std::istreambuf_iterator<char>(f), {}}; }
void write(const fs::path& p, const std::string& s) { std::ofstream f(p); need(bool(f), "cannot write " + p.string()); f << s; }
bool plain(const std::string& s) { return std::regex_match(s, std::regex("[A-Za-z_][A-Za-z_0-9$]*")); }
bool moduleToken(const std::string& s) { return std::regex_match(s, std::regex("[A-Za-z_][A-Za-z_0-9.$]*(\\[[0-9]+\\][A-Za-z_0-9.$]*)*")); }
bool path(const std::string& s) { return std::regex_match(s, std::regex("([A-Za-z_][A-Za-z_0-9$]*(\\[[0-9]+\\])?\\.)*[A-Za-z_][A-Za-z_0-9$]*")); }
bool boundary(char c) { return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '.'); }
std::string replace(std::string s, const std::string& from, const std::string& to, bool& seen) {
    size_t pos=0;
    while ((pos=s.find(from,pos)) != std::string::npos) {
        auto end=pos+from.size();
        if ((pos==0 || boundary(s[pos-1])) && (end==s.size() || boundary(s[end]))) {
            s.replace(pos,from.size(),to); seen=true; pos+=to.size();
        } else pos=end;
    }
    return s;
}
struct Leaf { std::string module, base; };
void routes(const J& modules, const std::string& module, const std::string& remaining, std::vector<Leaf>& found, std::set<std::string> active) {
    need(active.insert(module).second, "cyclic observation hierarchy");
    const auto& data=at(modules,module);
    if (has(at(data,"netnames"),remaining) ||
        (has(data,"memories") && has(at(data,"memories"),remaining))) found.push_back({module,remaining});
    for (const auto& [key,cell] : members(at(data,"cells"))) {
        const auto child=text(at(cell,"type")), prefix=key+".";
        if (remaining.compare(0,prefix.size(),prefix)==0 && has(modules,child))
            routes(modules,child,remaining.substr(prefix.size()),found,active);
    }
}
std::vector<J> selected(const J& request) {
    std::vector<J> result;
    std::set<std::string> names;
    std::map<std::string,std::set<std::string>> arrays;
    for (const auto& obs : items(at(request,"observations"))) {
        if (text(at(obs,"schema")) != "consflow-observation/2") continue;
        const auto base=text(at(obs,"base_relative")), instance=text(at(request,"instance"));
        need(path(base), "unsupported array base path");
        need(text(at(obs,"module"))==text(at(request,"module")) && text(at(obs,"instance"))==instance,
             "array observation module/instance identity mismatch");
        need(text(at(obs,"symbol"))=="Verilog::$root."+instance+"."+base, "array source symbol identity mismatch");
        const auto& a=at(obs,"array");
        const auto size=integer(at(a,"size")), offset=integer(at(a,"offset")), j=integer(at(a,"internal_index"));
        need(size>0 && j>=0 && j<size && size<1000000 && offset>-1000000 && offset<1000000, "array index/range unsupported");
        const auto i=truth(at(a,"increasing")) ? offset+j : offset+size-1-j;
        need(i==integer(at(a,"declared_index")), "array declared/internal index disagreement");
        const auto relative=base+"["+std::to_string(i)+"]", reference="u_dut."+relative;
        need(text(at(obs,"relative"))==relative && text(at(obs,"reference"))==reference, "array reference identity mismatch");
        arrays["u_dut."+base].insert(reference);
        bool used=false;
        for (const auto& body : items(at(request,"bodies"))) { bool present=false; replace(text(at(body,"body")),reference,reference,present); used |= present; }
        if (used) { need(names.insert(reference).second, "duplicate array observation mapping"); result.push_back(obs); }
    }
    // A selected body must identify an element by its canonical typed record.
    // Dynamic/unknown array indices never become basename approximations.
    for (const auto& [base, references] : arrays) {
        for (const auto& body : items(at(request,"bodies"))) {
            const auto source=text(at(body,"body")); size_t pos=0;
            while ((pos=source.find(base,pos))!=std::string::npos) {
                const auto end=pos+base.size();
                if ((pos && !boundary(source[pos-1])) ||
                    (end<source.size() && !boundary(source[end]))) {pos=end;continue;}
                need(end<source.size() && source[end]=='[',"array read has no unique element index");
                const auto close=source.find(']',end+1);
                need(close!=std::string::npos && references.count(source.substr(pos,close-pos+1)),
                     "dynamic/unknown array read has no unique typed element mapping");
                pos=close+1;
            }
        }
    }
    return result;
}
Leaf leaf(const J& modules,const std::string& root,const std::string& base) {
    std::vector<Leaf> found; routes(modules,root,base,found,{});
    need(found.size()==1,"array base has no unique actual compiled hierarchy binding"); return found[0];
}
}
int main(int argc,char** argv) {
    try {
        std::map<std::string,std::string> options;
        for(int i=1;i<argc;i+=2) { need(i+1<argc,"missing option value"); options[argv[i]]=argv[i+1]; }
        const auto action=options.at("--action"); need(action=="plan" || action=="bind","unknown array action");
        const auto request=parse(read(options.at("--input"))), before=parse(read(options.at("--before")));
        const auto root=text(at(request,"module")); need(moduleToken(root),"unsupported exact array root module");
        const auto& modules=at(before,"modules"); need(has(modules,root),"array root module absent");
        const auto observations=selected(request);
        const fs::path out=options.at("--out-dir"); fs::create_directories(out);
        J selection=list(); for(const auto& obs:observations) append(selection,obs);
        if(action=="plan") {
            std::set<std::pair<std::string,std::string>> memories;
            J planned=list();
            for(const auto& obs:observations) {
                std::vector<Leaf> matches;
                routes(modules,root,text(at(obs,"base_relative")),matches,{});
                if(matches.empty()) {
                    const auto exact=leaf(modules,root,text(at(obs,"relative")));
                    const auto bracket=exact.base.rfind('[');
                    need(bracket!=std::string::npos,"compiled element has no index suffix");
                    matches.push_back({exact.module,exact.base.substr(0,bracket)});
                }
                need(matches.size()==1,"array base has multiple compiled bindings");
                const auto target=matches[0];
                const auto& data=at(modules,target.module);
                const auto element=target.base+"["+text(at(at(obs,"array"),"declared_index"))+"]";
                bool memory=has(data,"memories") && has(at(data,"memories"),target.base);
                need(memory || has(at(data,"netnames"),element),"compiled array is neither memory nor exact element netname");
                if(memory) {
                    const auto& mem=at(at(data,"memories"),target.base);
                    const auto& array=at(obs,"array");
                    need(integer(at(mem,"width"))==integer(at(obs,"width")) &&
                         integer(at(mem,"size"))==integer(at(array,"size")) &&
                         integer(at(mem,"start_offset"))==integer(at(array,"offset")),
                         "compiled memory layout differs from typed array");
                    need(moduleToken(target.module)&&plain(target.base),"unsupported exact memory-map selection");
                    memories.emplace(target.module,target.base);
                }
                append(planned,object({{"original",obs},{"module",str(target.module)},{"memory",str(target.base)},{"requires_memory_map",boolean(memory)}}));
            }
            std::string script;
            for(const auto& [module,memory]:memories)
                script+="select -clear\nselect -module "+module+"\nselect m:"+memory+"\nselect -assert-count 1 m:"+memory+"\nmemory_map\nselect -clear\n";
            save(out/"plan.json",object({{"schema",str("consflow-array-plan/1")},{"selected",selection},{"conversions",planned},{"memory_map_script",str(script)}}));
            write(out/"memory-map.ys",script); return 0;
        }
        J expected=before, derived=list(), replacements=list(), bodies=at(request,"bodies");
        std::map<std::string,std::vector<std::string>> additions;
        size_t count=0;
        for(const auto& obs:observations) {
            const auto base=text(at(obs,"base_relative"));
            // After memory_map the base memory is absent: resolve its exact element.
            const auto elementPath=text(at(obs,"relative"));
            auto target=leaf(modules,root,elementPath);
            const auto& data=at(modules,target.module);
            const auto& wire=at(at(data,"netnames"),target.base);
            need(integer(at(obs,"width"))==static_cast<long long>(items(at(wire,"bits")).size()),"compiled element width differs");
            auto value=[&](const std::string& key,long long fallback) { auto it=members(wire).find(key);return it==members(wire).end()?fallback:integer(it->second); };
            need(value("offset",0)==integer(at(obs,"offset")) && (value("signed",0)!=0)==truth(at(obs,"signed")) && (value("upto",0)!=0)==truth(at(obs,"increasing")),"compiled element type/range differs");
            for(const auto& bit:items(at(wire,"bits"))) need(bit.is_number() || text(bit)=="0" || text(bit)=="1","unknown original array element bit");
            const auto alias="veval_array_observe_"+std::to_string(count++);
            need(!has(at(data,"netnames"),alias)&&!has(at(data,"ports"),alias)&&!has(at(data,"cells"),alias),"array scalar alias collision");
            J newWire=wire; put(newWire,"hide_name",num(0)); put(newWire,"attributes",object({{"veval_array_observation_alias",str("00000000000000000000000000000001")}}));
            expected.make_object()["modules"].make_object()[target.module].make_object()["netnames"].make_object()[alias]=newWire;
            std::string decl="  attribute \\veval_array_observation_alias 1\n  wire width "+std::to_string(items(at(wire,"bits")).size());
            if(value("offset",0)) decl+=" offset "+std::to_string(value("offset",0));
            if(value("upto",0)) decl+=" upto";
            if(value("signed",0)) decl+=" signed";
            decl+=" \\"+alias+"\n  connect \\"+alias+" \\"+target.base+"\n";
            additions[target.module].push_back(decl);
            const auto cut=elementPath.size()-target.base.size();
            const auto relative=elementPath.substr(0,cut)+alias, reference="u_dut."+relative;
            J mapped=obs; put(mapped,"schema",str("consflow-observation/1"));
            put(mapped,"relative",str(relative)); put(mapped,"reference",str(reference));
            put(mapped,"symbol",str("Verilog::$root."+text(at(request,"instance"))+"."+relative));
            append(derived,mapped);
            for(auto& body: bodies.make_array()) { bool seen=false; body.make_object()["body"]=str(replace(text(at(body,"body")),text(at(obs,"reference")),reference,seen)); }
            append(replacements,object({{"from",at(obs,"reference")},{"to",str(reference)},{"original",obs},{"compiled_module",str(target.module)},{"compiled_leaf",str(target.base)},{"alias",str(alias)},{"original_bits",at(wire,"bits")}}));
        }
        // Route only enumerated read-only wires. Original cells/state stay byte-identical.
        std::istringstream input(read(options.at("--rtlil"))); std::ostringstream routed;
        std::string line,module; std::set<std::string> emitted;
        while(std::getline(input,line)) {
            if(line.rfind("module ",0)==0) { module=line.substr(7); if(!module.empty()&&module[0]=='\\')module.erase(0,1); }
            if(line=="end" && !module.empty()) {
                auto it=additions.find(module); if(it!=additions.end()) { for(const auto& decl:it->second)routed<<decl; emitted.insert(module); }
                module.clear();
            }
            routed<<line<<'\n';
        }
        need(emitted.size()==additions.size(),"array alias RTLIL coverage differs");
        write(out/"routed.il",routed.str()); save(out/"expected.json",expected);
        save(out/"binding.json",object({{"schema",str("consflow-array-binding/1")},{"observations",derived},{"bodies",bodies},{"replacements",replacements}}));
        return 0;
    } catch(const std::exception& e) {std::cerr<<"array observation refused: "<<e.what()<<'\n';return 1;}
}
