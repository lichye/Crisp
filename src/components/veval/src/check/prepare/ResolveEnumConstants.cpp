// Resolve compilation-unit enums with the same frontend as lowering. Only
// declarations preceding the boundary module are elaborated; no RTL is solved.
#include "JobSchema.h"
#include "check/prepare/ResolveSourcePaths.h"
#include <verilog/verilog_elaborate_compilation_unit.h>
#include <verilog/verilog_parser.h>
#include <verilog/verilog_preprocessor.h>
#include <ebmc/ebmc_error.h>
#include <util/arith_tools.h>
#include <util/bitvector_types.h>
#include <util/message.h>
#include <util/symbol_table.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

namespace veval::verification {
static void need(bool ok,const std::string &why) { if(!ok) throw std::runtime_error(why); }
static J resolve(const J &request) {
    const auto &job=request["job"], &ref=job["provenance"]["effective_config"];
    need(fileDigest(text(ref["path"]))==text(ref["sha256"]),"original frontend configuration changed");
    const auto cfg=parse(read(text(ref["path"]))),front=cfg["frontend"];
    J defines=object();std::list<std::string> includes,macros;
    for(const auto &value:items(front["defines"])) {
        const auto s=text(value);const auto at=s.find('=');macros.push_back(s);
        put(defines,s.substr(0,at),str(at==s.npos?"":s.substr(at+1)));
    }
    for(const auto &v:items(front["include_paths"]))includes.push_back(text(v));
    need(equal(defines,job["frontend"]["defines"])&&equal(front["include_paths"],job["frontend"]["include_order"])&&
         equal(front["system_verilog"],job["frontend"]["systemverilog"]),"Job frontend differs from original constant context");
    Files files;fs::path productPath;
    for(const auto &v:items(job["inputs"]))if(text(v["role"])=="product")productPath=text(v["path"]);
    Product product(productPath,files);verifyProduct(job,product);
    const auto source=SourcePaths(cfg,job,product.head).boundary.string();
    need(equal(product.head["boundary"],front["top"]),"constant boundary differs from original frontend");
    std::map<std::string,std::string> identities;
    for(const auto &v:items(front["input_identities"]))identities[absolute(text(v["path"]))]=text(v["sha256"]);
    auto check=[&](const std::string &p) {const auto path=absolute(p);need(identities.count(path)&&fileDigest(path)==identities.at(path),"original constant input changed or unregistered: "+path);};
    J wanted=list(),actual=list();bool found=false;
    for(const auto &v:items(front["file_order"])) {
        if(absolute(text(v))==source){need(!found,"repeated source boundary");found=true;}
        else append(wanted,str(absolute(text(v))));
    }
    for(const auto &v:items(job["inputs"]))if(text(v["role"])=="design"||text(v["role"])=="harness")append(actual,str(absolute(text(v["path"]))));
    need(found&&equal(wanted,actual),"constant source/Job design binding differs");
    // Includes are small source context, checked once for this helper call.
    for(const auto &[path,hash]:identities) {
        bool include=false;for(const auto &dir:includes)if(path.rfind(absolute(dir)+"/",0)==0)include=true;
        if(include)check(path);
    }
    verilog_macro_tablet unitMacros;verilog_scopest scopes;
    symbol_tablet symbols;stream_message_handlert messages(std::cerr);messages.set_verbosity(1);
    for(const auto &value:items(front["file_order"])) {
        const auto path=absolute(text(value));check(path);std::ifstream in(path);std::stringstream preprocessed;
        verilog_preprocessort preprocessor(in,preprocessed,messages,path,includes,macros,&unitMacros);
        preprocessor.preprocessor();
        verilog_parsert parser(verilog_standardt::SV2023,scopes,messages);
        parser.set_file(path);parser.in=&preprocessed;parser.grammar=verilog_parsert::LANGUAGE;verilog_scanner_init();
        need(!parser.parse(),"constant context parse failed");
        auto &tree=parser.parse_tree;
        if(path==source) {
            auto boundary=std::find_if(tree.items.begin(),tree.items.end(),[](const auto &v){return v.id()==ID_verilog_module;});
            need(boundary!=tree.items.end()&&id2string(to_verilog_module_source(*boundary).base_name())==text(front["top"]),"boundary module absent or ambiguous");
            tree.items.erase(boundary,tree.items.end());
        }
        verilog_elaborate_compilation_unit(tree,false,symbols,messages);
        if(path==source)break;
    }
    J bindings=list();std::set<std::string> seen;
    for(const auto &v:items(request["names"])) {
        const auto name=text(v);
        need(std::regex_match(name,std::regex("\\$unit::[A-Za-z_][A-Za-z_0-9]*"))&&seen.insert(name).second,"invalid constant name");
        const auto *symbol=symbols.lookup("Verilog::"+name);
        need(symbol&&symbol->is_macro&&symbol->value.is_constant(),"unknown or nonconstant compilation-unit name: "+name);
        const auto &type=symbol->value.type();auto number=numeric_cast<mp_integer>(symbol->value);
        need(number&&(type.id()==ID_unsignedbv||type.id()==ID_signedbv),"unsupported enum type: "+name);
        // Enum values carry their declaring enum identifier on the packed type.
        need(type.get(ID_C_verilog_type)==ID_verilog_enum,"not a typed enum value: "+name);
        const auto width=to_bitvector_type(type).get_width();need(width>0&&width<=64,"unsupported enum width");
        auto bits=*number;if(bits<0)bits+=mp_integer(1)<<width;
        const auto declaration=absolute(id2string(symbol->location.get_file()));check(declaration);
        append(bindings,object({{"name",v},{"literal",str(std::to_string(width)+(type.id()==ID_signedbv?"'sh":"'h")+integer2string(bits,16))},
            {"width",num(width)},{"signed",boolean(type.id()==ID_signedbv)},{"declaration",str(declaration)},
            {"line",str(id2string(symbol->location.get_line()))}}));
    }
    need(!seen.empty(),"empty constant request");
    return object({{"status",str("checked")},{"bindings",bindings},{"compiler",str("vendored hw-cbmc compilation-unit frontend")},
                   {"effective_config",ref},{"source",str(source)}});
}
}
int main(int argc,char **argv) {
    using namespace veval::verification;std::string out;
    try {
        Arguments args(argc,argv,{"--help"},{"--request","--out"});
        if(args.has("--help")){std::cout<<"consflow_constants --request FILE --out FILE\n";return 0;}
        out=args.required("--out");save(out,resolve(parse(read(args.required("--request")))));return 0;
    }catch(const ebmc_errort &e){std::cerr<<e.what()<<'\n';}
     catch(const std::exception &e){std::cerr<<e.what()<<'\n';if(!out.empty())save(out,object({{"status",str("unsupported")},{"reason",str(e.what())}}));}
     catch(...){std::cerr<<"constant frontend failed\n";}
    return 2;
}
