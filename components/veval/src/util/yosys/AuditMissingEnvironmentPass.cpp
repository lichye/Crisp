// Advisory environment dependencies. This pass never changes the design.
#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/celltypes.h"
#include "libs/json11/json11.hpp"
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
USING_YOSYS_NAMESPACE
#include "YosysApiCompatibility.h"
PRIVATE_NAMESPACE_BEGIN
using Json=json11::Json;
using Bits=std::vector<RTLIL::SigBit>;
static std::string name(IdString id){return veval_yosys::unescaped(id);}
static bool is(const std::string &s,std::initializer_list<const char*> choices){for(auto x:choices)if(s==x)return true;return false;}
static bool flipflop(const std::string &t){return is(t,{"$dff","$adff","$sdff","$dffe","$adffe","$sdffe","$sdffce","$aldff","$aldffe","$dffsr","$dffsre","$ff","$_DFF_P_","$_DFF_N_"});}
struct EnvAudit {
    struct Driver {Wire *port=nullptr;Cell *cell=nullptr;IdString pin;int index=0;};
    struct Context {
        Module *module;Context *parent;Cell *instance;std::string path;
        std::map<Cell*,Context*> children;
        Context(Module *m,Context *p,Cell *i,std::string path):module(m),parent(p),instance(i),path(path){}
    };
    struct ModuleData {
        SigMap aliases;std::map<SigBit,Driver> drivers;
        ModuleData(Module *m,CellTypes &types):aliases(m){
            if(!m->processes.empty())throw std::runtime_error("environment audit needs process lowering for "+name(m->name));
            for(auto wire:m->wires())if(wire->port_input)for(int i=0;i<wire->width;++i)
                drivers[aliases(SigBit(wire,i))]={wire,nullptr,{},i};
            std::map<std::string,Cell*> ordered;for(auto cell:m->cells())ordered[name(cell->name)]=cell;
            for(auto item:ordered){auto cell=item.second;
                for(const auto &pin:cell->connections())if(types.cell_output(cell->type,pin.first)){
                    auto sig=aliases(pin.second);for(int i=0;i<GetSize(sig);++i)if(sig[i].wire)drivers[sig[i]]={nullptr,cell,pin.first,i};
                }
            }
        }
    };
    using Found=std::map<std::string,std::map<int,bool>>; // bit -> some combinational path
    using Location=std::pair<Context*,SigBit>;
    Design *design;CellTypes types;std::vector<std::unique_ptr<Context>> contexts;
    std::map<Module*,std::unique_ptr<ModuleData>> modules;
    std::map<std::string,Context*> paths;std::map<std::string,Found> cones;
    std::set<std::string> unknown;
    std::map<Context*,std::map<SigBit,std::string>> bitNames;
    std::map<std::tuple<Context*,Cell*,std::string>,Bits> portCache;
    EnvAudit(Design *d):design(d){types.setup(d);add(d->top_module(),nullptr,nullptr,"");}
    ModuleData &data(Context *c){
        auto &entry=modules[c->module];
        if(!entry)entry=std::make_unique<ModuleData>(c->module,types);
        return *entry;
    }
    SigMap &aliases(Context *c){return data(c).aliases;}
    Context *add(Module *m,Context *parent,Cell *inst,const std::string &path){
        if(!m)throw std::runtime_error("environment audit needs a top module");
        for(auto p=parent;p;p=p->parent)if(p->module==m)throw std::runtime_error("recursive module hierarchy");
        auto own=std::make_unique<Context>(m,parent,inst,path);auto c=own.get();contexts.push_back(std::move(own));paths[path]=c;
        std::map<std::string,Cell*> ordered;for(auto cell:m->cells())ordered[name(cell->name)]=cell;
        for(auto item:ordered){auto cell=item.second;
            if(auto child=design->module(cell->type))c->children[cell]=add(child,c,cell,path.empty()?item.first:path+"."+item.first);
        }
        return c;
    }
    const Bits &port(Context *c,Cell *cell,const char *p){
        auto key=std::make_tuple(c,cell,std::string(p));auto found=portCache.find(key);
        if(found!=portCache.end())return found->second;
        auto id=veval_yosys::identifier(p);Bits bits;
        if(cell->hasPort(id)){auto s=aliases(c)(cell->getPort(id));bits.assign(s.begin(),s.end());}
        return portCache.emplace(std::move(key),std::move(bits)).first->second;
    }
    bool parameter(Cell *c,const char *p){auto it=c->parameters.find(veval_yosys::identifier(p));return it!=c->parameters.end()&&it->second.as_bool();}
    using Deps=std::vector<std::pair<SigBit,bool>>;
    Deps dependencies(Context *c,Cell *cell,int index){
        const auto t=name(cell->type);const auto &a=port(c,cell,"A"),&b=port(c,cell,"B");Deps out;
        auto push=[&](const Bits &bits,bool comb=true){for(auto bit:bits)if(bit.wire)out.emplace_back(bit,comb);};
        auto all=[&](const char *p,bool comb=true){push(port(c,cell,p),comb);};
        auto at=[&](const Bits &bits,int i,bool sign=false,bool comb=true){if(i<int(bits.size()))push({bits.at(i)},comb);else if(sign&&!bits.empty())push({bits.back()},comb);};
        auto prefix=[&](const Bits &bits,int i,bool sign){for(int j=0;j<=i&&j<int(bits.size());++j)push({bits[j]});if(i>=int(bits.size())&&sign&&!bits.empty())push({bits.back()});};
        auto inputs=[&](){for(const auto &p:cell->connections())if(types.cell_input(cell->type,p.first))all(name(p.first).c_str());};
        if(is(t,{"$and","$or","$xor","$xnor","$bweqx"})){at(a,index,parameter(cell,"A_SIGNED"));at(b,index,parameter(cell,"B_SIGNED"));}
        else if(is(t,{"$not","$pos","$buf"}))at(a,index,parameter(cell,"A_SIGNED"));
        else if(is(t,{"$add","$sub","$mul","$neg"})){prefix(a,index,parameter(cell,"A_SIGNED"));prefix(b,index,parameter(cell,"B_SIGNED"));}
        else if(t=="$mux"){at(a,index);at(b,index);all("S");}
        else if(t=="$pmux"){at(a,index);if(!a.empty())for(size_t k=index;k<b.size();k+=a.size())push({b[k]});all("S");}
        else if(is(t,{"$shl","$sshl"})){prefix(a,index,parameter(cell,"A_SIGNED"));push(b);}
        else if(is(t,{"$shr","$sshr"})){for(size_t k=index;k<a.size();++k)push({a[k]});if(!a.empty())push({a.back()});push(b);}
        else if(is(t,{"$shift","$shiftx","$bmux","$demux"})){push(a);push(b);all("S");}
        else if(flipflop(t)){at(port(c,cell,"D"),index,false,false);for(auto p:{"EN","ARST","SRST","CE"})all(p,false);all("ALOAD");at(port(c,cell,"AD"),index);at(port(c,cell,"SET"),index);at(port(c,cell,"CLR"),index);}
        else if(is(t,{"$dlatch","$adlatch","$dlatchsr"})){at(port(c,cell,"D"),index);all("EN");all("ARST");}
        else if(is(t,{"$memrd","$memrd_v2"})){
            for(auto p:{"ADDR","EN","ARST","SRST"})all(p,!parameter(cell,"CLK_ENABLE"));
            auto id=veval_yosys::identifier("MEMID");
            if(cell->parameters.count(id))for(auto w:c->module->cells())if(is(name(w->type),{"$memwr","$memwr_v2"})&&w->parameters.count(id)&&w->parameters.at(id)==cell->parameters.at(id))
                for(const auto &p:w->connections())if(name(p.first)!="CLK")push(port(c,w,name(p.first).c_str()),false);
        } else if(is(t,{"$mem","$mem_v2"})){all("RD_ADDR",!parameter(cell,"RD_CLK_ENABLE"));for(auto p:{"WR_DATA","WR_ADDR","WR_EN"})all(p,false);}
        else {
            if(!is(t,{"$eq","$ne","$eqx","$nex","$lt","$le","$gt","$ge","$logic_not","$logic_and","$logic_or","$reduce_and","$reduce_or","$reduce_xor","$reduce_xnor","$reduce_bool","$div","$mod","$divfloor","$modfloor","$pow","$alu","$macc","$fa","$lcu","$lut","$sop","$tribuf","$bwmux"}))unknown.insert(t);
            inputs();
        }
        return out;
    }
    std::map<std::string,std::set<int>> reads(Context *c,const std::string &sentence){
        // A whole input port counts as read once any of its bits is named.
        // Bit/range selection still limits the backward dependency walk.
        static const std::regex literal(R"(\d*\s*'\s*[sS]?[bBoOdDhH]\s*[0-9a-fA-FxXzZ_?]+|'[01xXzZ]\b|\d+'(?=\())");
        static const std::regex identifier(R"(([A-Za-z_][A-Za-z0-9_$]*)(\s*\[\s*(\d+)\s*(?::\s*(\d+))?\s*\])?)");
        auto s=std::regex_replace(sentence,literal," 0 ");std::map<std::string,std::set<int>> result;
        for(std::sregex_iterator it(s.begin(),s.end(),identifier),end;it!=end;++it){auto m=*it;auto pos=m.position();
            if(pos>0&&(std::isalnum(s[pos-1])||s[pos-1]=='_'||s[pos-1]=='$'||s[pos-1]=='.'||s[pos-1]=='\''))continue;
            auto n=m[1].str();auto w=c->module->wire(veval_yosys::identifier(n));if(!w||!w->port_id)continue;
            auto &bits=result[n];auto next=pos+m.length();
            if(m[2].matched&&(next>=long(s.size())||s[next]!='[')){
                auto lo=std::stoi(m[3]),hi=m[4].matched?std::stoi(m[4]):lo;
                for(int j=std::min(lo,hi);j<=std::max(lo,hi);++j){int k=w->upto?w->width-1-(j-w->start_offset):j-w->start_offset;if(k>=0&&k<w->width)bits.insert(k);}
            }else for(int i=0;i<w->width;++i)bits.insert(i);
        }return result;
    }
    const Found &cone(Context *landing,Wire *wire,const std::set<int> &indices){
        std::string key=landing->path+"/"+name(wire->name);for(auto i:indices)key+=","+std::to_string(i);
        auto saved=cones.find(key);if(saved!=cones.end())return saved->second;
        Found found;std::map<Location,bool> seen;std::vector<std::tuple<Context*,SigBit,bool>> stack;
        for(auto i:indices)stack.emplace_back(landing,aliases(landing)(SigBit(wire,i)),true);
        while(!stack.empty()){
            auto [c,bit,comb]=stack.back();stack.pop_back();if(!bit.wire)continue;
            auto old=seen.find({c,bit});if(old!=seen.end()&&(old->second||!comb))continue;seen[{c,bit}]=comb;
            const auto &drivers=data(c).drivers;
            auto di=drivers.find(bit);if(di==drivers.end())continue;auto d=di->second;
            if(d.port){
                if(c==landing)found[name(d.port->name)][d.index]|=comb;
                else {const auto &sig=port(c->parent,c->instance,name(d.port->name).c_str());if(d.index<int(sig.size()))stack.emplace_back(c->parent,sig[d.index],comb);}
            }else if(c->children.count(d.cell)){
                auto child=c->children.at(d.cell);auto w=child->module->wire(d.pin);
                if(w&&d.index<w->width)stack.emplace_back(child,aliases(child)(SigBit(w,d.index)),comb);
            }else for(auto dep:dependencies(c,d.cell,d.index))stack.emplace_back(c,dep.first,comb&&dep.second);
        }return cones.emplace(key,std::move(found)).first->second;
    }
    std::string bitName(Context *c,SigBit bit){
        auto cached=bitNames.find(c);
        if(cached==bitNames.end()){
            std::map<SigBit,std::string> names;
            std::map<std::string,Wire*> wires;for(auto w:c->module->wires())wires[name(w->name)]=w;
            for(auto it:wires)for(auto b:aliases(c)(it.second))if(b.wire){
                auto old=names.find(b);const bool visible=!it.first.empty()&&it.first[0]!='$';
                if(old==names.end()||(!old->second.empty()&&old->second[0]=='$'&&visible))names[b]=it.first;
            }
            cached=bitNames.emplace(c,std::move(names)).first;
        }
        auto found=cached->second.find(bit);return found==cached->second.end()?"<undriven>":found->second;
    }
    std::map<std::string,std::set<std::string>> drivers(Context *landing,Context *dut,Wire *input,const std::map<int,bool>& bits){
        std::map<std::string,std::set<std::string>> found;std::map<Context*,Cell*> self;
        for(auto c=landing;c->parent;c=c->parent)self[c->parent]=c->instance;
        if(!landing->parent)return found;
        const auto &sig=port(landing->parent,landing->instance,name(input->name).c_str());std::vector<Location> stack;std::set<Location> seen;
        for(auto b:bits)if(b.first<int(sig.size()))stack.emplace_back(landing->parent,sig[b.first]);
        while(!stack.empty()){
            auto [c,bit]=stack.back();stack.pop_back();if(!bit.wire||!seen.insert({c,bit}).second)continue;
            const auto &drivers=data(c).drivers;
            auto it=drivers.find(bit);if(it==drivers.end())continue;auto d=it->second;
            if(d.port){if(c!=dut&&c->parent){const auto &s=port(c->parent,c->instance,name(d.port->name).c_str());if(d.index<int(s.size()))stack.emplace_back(c->parent,s[d.index]);}continue;}
            auto relative=c->path.substr(dut->path.size());if(!relative.empty()&&relative[0]=='.')relative.erase(0,1);if(!relative.empty())relative+='.';
            if(c->children.count(d.cell)){if(self[c]!=d.cell)found["siblings"].insert(relative+name(d.cell->name)+"."+name(d.pin));continue;}
            if(flipflop(name(d.cell->type))||(is(name(d.cell->type),{"$memrd","$memrd_v2"})&&parameter(d.cell,"CLK_ENABLE"))){found["registers"].insert(relative+bitName(c,bit));continue;}
            for(auto dep:dependencies(c,d.cell,d.index))if(dep.second)stack.emplace_back(c,dep.first);else{
                auto id=veval_yosys::identifier("MEMID");found["registers"].insert(relative+"memory "+(d.cell->parameters.count(id)?d.cell->parameters.at(id).decode_string():name(d.cell->name)));
            }
        }return found;
    }
    Context *locate(const Json &q,Context *&dut){
        Context *landing=nullptr;dut=contexts.front().get();
        if(q["path"].is_string()){
            auto f=paths.find(q["path"].string_value());if(f!=paths.end())landing=f->second;
        }else { // Standalone audit of frozen Jobs against a complete chip netlist.
            auto module=q["module"].string_value(),suffix=q["suffix"].string_value();
            for(const auto &c:contexts)if(name(c->module->name)==module){
                for(auto a=c.get();a;a=a->parent){auto rel=c->path.substr(a->path.size());if(!rel.empty()&&rel[0]=='.')rel.erase(0,1);
                    if(rel==suffix){if(landing)throw std::runtime_error("ambiguous landing module/path");landing=c.get();dut=a;break;}}
            }
        }
        if(!landing||name(landing->module->name)!=q["module"].string_value())throw std::runtime_error("landing module/path absent: "+q["instance"].string_value());
        return landing;
    }
    pool<Module*> requiredModules(const Json::array &queries){
        pool<Module*> needed,expanded;
        for(const auto &q:queries){
            Context *dut;auto landing=locate(q,dut);
            if(!landing->parent)continue;
            for(auto p=landing->parent;p;p=p->parent)needed.insert(p->module);
            std::vector<Context*> stack{landing};
            while(!stack.empty()){
                auto c=stack.back();stack.pop_back();
                needed.insert(c->module);
                if(!expanded.insert(c->module).second)continue;
                for(auto child:c->children)stack.push_back(child.second);
            }
        }
        return needed;
    }
    Json query(const Json &q){
        Context *dut;auto landing=locate(q,dut);
        if(!landing->parent)return Json::object{{"property",q["property"]},{"instance",q["instance"]},{"environment_missing",false},
            {"environment_status","checked"},{"environment_path_kind",""},{"environment_ports",Json::array{}}};
        auto used=reads(landing,q["sentence"].string_value());Found reached;
        for(const auto &r:used){auto w=landing->module->wire(veval_yosys::identifier(r.first));for(const auto &p:cone(landing,w,r.second))for(auto b:p.second)reached[p.first][b.first]|=b.second;}
        Json::array ports;bool comb=false;
        for(const auto &p:reached){if(used.count(p.first))continue;auto w=landing->module->wire(veval_yosys::identifier(p.first));auto sources=drivers(landing,dut,w,p.second);
            if(sources["siblings"].empty()&&sources["registers"].empty())continue;
            bool same=false;for(auto b:p.second)same|=b.second;comb|=same;Json::array siblings,registers;
            for(auto &s:sources["siblings"])siblings.push_back(s);for(auto &s:sources["registers"])registers.push_back(s);
            ports.push_back(Json::object{{"port",p.first},{"path_kind",same?"comb":"reg"},{"bits",int(p.second.size())},{"width",w->width},{"siblings",siblings},{"registers",registers}});
        }
        return Json::object{{"property",q["property"]},{"instance",q["instance"]},{"environment_missing",!ports.empty()},
            {"environment_status","checked"},{"environment_path_kind",ports.empty()?"":comb?"comb":"reg"},{"environment_ports",ports}};
    }
};
Json readQueries(std::string path){
    rewrite_filename(path);
    std::ifstream in(path);if(!in)throw std::runtime_error("cannot open environment queries");
    std::string text((std::istreambuf_iterator<char>(in)),{}),error;
    auto request=Json::parse(text,error);if(!error.empty())throw std::runtime_error(error);
    return request;
}
struct PrepareEnvironment : Pass {
    PrepareEnvironment():Pass("consflow_environment_prepare","lower processes only in the queried environment hierarchy"){}
    void execute(std::vector<std::string> args,Design *design)override{
        veval_yosys::Scope scope(design);
        if(args.size()!=2)log_cmd_error("usage: consflow_environment_prepare queries.json\n");
        try{
            const auto request=readQueries(args[1]);
            EnvAudit audit(design);
            const auto needed=audit.requiredModules(request["queries"].array_items());
            RTLIL::Selection selection(false);
            for(auto module:needed)selection.select(module);
            log("ConsFlow environment: process lowering for %zu of %zu modules.\n",needed.size(),design->modules().size());
            if(!needed.empty())Pass::call_on_selection(design,selection,"consflow_proc");
        }catch(const std::exception &e){log_cmd_error("%s\n",e.what());}
    }
} prepareEnvironment;
struct Environment : Pass {
    Environment():Pass("consflow_environment","report unread interface dependencies without changing the netlist"){}
    void execute(std::vector<std::string> args,Design *design)override{
        veval_yosys::Scope scope(design);
        if(args.size()!=3)log_cmd_error("usage: consflow_environment queries.json report.json\n");
        try{
            const auto request=readQueries(args[1]);rewrite_filename(args[2]);
            EnvAudit audit(design);Json::array rows;
            for(const auto &q:request["queries"].array_items())rows.push_back(audit.query(q));
            Json::array unknown;for(auto &t:audit.unknown)unknown.push_back(t);
            std::ofstream out(args[2]);out<<Json(Json::object{{"schema","consflow-environment/1"},{"rows",rows},{"conservative_cell_types",unknown}}).dump()<<'\n';
            if(!out)throw std::runtime_error("cannot write environment report");
        }catch(const std::exception &e){log_cmd_error("%s\n",e.what());}
    }
} environment;
PRIVATE_NAMESPACE_END
