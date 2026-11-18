#include "JobSchema.h"
#include <algorithm>
#include <cmath>
#include <regex>
#include <set>
#include "JobSchema.inc"

namespace veval::verification {
static const J &at(const J &a,size_t index) {
    const auto &array=items(a);
    if(index>=array.size())throw std::runtime_error("array index out of bounds");
    return *(array.begin()+index);
}
static void require(bool ok,const std::string &why) {
    if(!ok)throw std::runtime_error(why);
}
static J schemaTree(const J &table) {
    J result=object({{"spec",J()},{"children",object()}});
    for(const auto &entry:items(table)) {
        const auto path=text(at(entry,0));
        size_t start=0;
        J *node=&result;
        while(true) {
            const auto end=path.find('.',start);
            const auto part=path.substr(start,end==path.npos?end:end-start);
            auto &child=node->make_object()["children"].make_object()[part];
            if(child.is_null())child=object({{"spec",J()},{"children",object()}});
            node=&child;
            if(end==path.npos)break;
            start=end+1;
        }
        put(*node,"spec",list({at(entry,1),at(entry,2),at(entry,3)}));
    }
    return result;
}
static void checkScalar(const std::string &kind,const J &value,const J &options,const std::string &path) {
    if(kind=="bool")require(value.is_boolean(),path+": expected bool");
    else if(kind=="str")require(value.is_string(),path+": expected string");
    else {
        require(value.is_number(),path+": expected number");
        const auto n=std::stod(value.value);
        require(std::isfinite(n),path+": nonfinite number");
        if(kind=="int")require(value.value.find_first_of(".eE")==std::string::npos,path+": expected integer");
        if(truth(options["positive"]))require(n>0,path+": expected positive number");
        if(!options["minimum"].is_null())require(n>=std::stod(text(options["minimum"])),path+": below minimum");
    }
    if(truth(options["nonempty"]))require(!trim(text(value)).empty(),path+": empty string");
    if(!options["enum"].is_null()) {
        const auto &allowed=items(options["enum"]);
        require(std::any_of(allowed.begin(),allowed.end(),[&](const J &v){return equal(v,value);}),
                path+": value outside enum");
    }
    if(!options["hex_len"].is_null())
        require(value.value.size()==std::stoul(text(options["hex_len"]))&&
                value.value.find_first_not_of("0123456789abcdef")==std::string::npos,
                path+": invalid hex digest");
}
static void checkValue(const J &node,const J &value,const std::string &path) {
    static const J noOptions=object();
    const auto &spec=node["spec"];
    const auto kind=spec.is_null()?"object":text(at(spec,0));
    const auto &options=spec.is_null()?noOptions:at(spec,2);
    if(value.is_null()) {
        require(!spec.is_null()&&truth(at(spec,1)),path+": must not be null");
        return;
    }
    if(kind=="object") {
        require(value.is_object(),path+": expected object");
        if(truth(options["free"])) {
            if(truth(options["value_kind"]))
                for(const auto &[key,v]:members(value))
                    if(!v.is_null())checkScalar(text(options["value_kind"]),v,noOptions,path+'.'+key);
            return;
        }
        const auto &children=members(node["children"]);
        const auto &fields=members(value);
        for(const auto &[key,v]:fields)
            require(children.find(key)!=children.end(),path+'.'+key+": field outside schema");
        for(const auto &[key,sub]:children) {
            require(fields.find(key)!=fields.end(),path+'.'+key+": required field missing");
            checkValue(sub,value[key],path+'.'+key);
        }
    } else if(kind=="array") {
        require(value.is_array(),path+": expected array");
        const auto &values=items(value);
        if(!options["min_len"].is_null())
            require(values.size()>=std::stoul(text(options["min_len"])),path+": array too short");
        if(truth(options["free_items"]))return;
        J itemSchema;
        if(!values.empty()&&options["shape"].is_null()&&!options["items"].is_null())
            itemSchema=schemaTree(options["items"]);
        size_t index=0;
        for(const auto &v:values) {
            const auto here=path+'['+std::to_string(index++)+']';
            if(!options["shape"].is_null()) {
                require(v.is_array()&&items(v).size()==items(options["shape"]).size(),here+": invalid tuple shape");
                size_t i=0;
                for(const auto &slot:items(options["shape"])) {
                    const auto &item=at(v,i++);
                    const auto field=here+'.'+text(at(slot,0));
                    if(item.is_null())require(truth(at(slot,2)),field+": must not be null");
                    else checkScalar(text(at(slot,1)),item,at(slot,3),field);
                }
            }else if(!options["items"].is_null())checkValue(itemSchema,v,here);
            else if(truth(options["item_kind"]))checkScalar(text(options["item_kind"]),v,noOptions,here);
        }
    } else checkScalar(kind,value,options,path);
}
void validateShape(const J &j,const std::string &schema) {
    static const auto tables=parse(schemaText);
    require(schema=="job"||schema=="result","unsupported schema table");
    checkValue(tables[schema],j,"");
}
void validateJob(const J &job) {
    validateShape(job,"job");
    const auto &setup=job["setup"];
    require(setup["clock"].is_null()==(text(setup["clock_edge"])=="not-provided"),"clock/edge mismatch");
    if(text(setup["reset"]["kind"])=="reset-contract")
        require(truth(setup["reset"]["text"]),"reset contract lacks text");
    std::set<std::string> dirs;
    std::vector<std::string> order;
    auto pathCheck=[](const std::string &path) {
        const fs::path file(path);
        require(file.is_absolute()&&file.lexically_normal()==file,"unnormalized absolute path: "+path);
    };
    for(const auto &p:items(job["frontend"]["include_order"])) {
        const auto path=text(p);
        pathCheck(path);
        require(dirs.insert(path).second,"duplicate include directory");
        order.push_back(path);
    }
    size_t products=0;
    for(const auto &input:items(job["inputs"])) {
        const auto path=text(input["path"]),role=text(input["role"]);
        pathCheck(path);
        if(role=="product")++products;
        if(!input["include_dir_index"].is_null()) {
            const auto i=std::stoul(text(input["include_dir_index"]));
            require(role=="include"&&i<order.size()&&path.rfind(order[i]+"/",0)==0,
                    "include file outside its search directory");
        }
    }
    require(products==1,"expected exactly one product");
    for(const auto &premise:items(job["premises"])) {
        if(text(premise["kind"])!="environment-relation")continue;
        static const std::regex reference("env:[a-f0-9]{64}");
        require(text(premise["scope"])=="compiled-with-product"&&truth(premise["text"])&&
                std::regex_match(text(premise["evidence_ref"]),reference)&&
                (text(premise["discharge_status"])=="checked-conditional"||
                 text(premise["discharge_status"])=="assumed-conditional"),"invalid environment relation");
    }
    std::set<std::string> rungs;
    for(const auto &r:items(job["engine"]["ladder"]))
        require(rungs.insert(text(at(r,0))).second,"duplicate rung");
}
J jobPremises(const Product &p,const std::string &label) {
    J out=list();
    const auto source=p.premises(label);
    for(const auto &value:items(source)) {
        J row=object();
        for(const auto &key:{"label","text","source_entry","route","arrival","clock","clock_edge",
                             "discharge_status","evidence_ref","kind","scope"})put(row,key,value[key]);
        const auto kind=text(value["kind"]);
        if(kind=="derived")put(row,"discharge_status",str("derived-from-design"));
        if(kind=="design-binding") {
            for(const auto &key:{"source_entry","route","arrival"})put(row,key,J());
            put(row,"discharge_status",str("design-fixed"));
            put(row,"evidence_ref",str("design-binding:"+digest(canonical(value["design_binding"]))));
        }
        append(out,row);
    }
    return out;
}
J verifyProduct(const J &job,const Product &p) {
    const auto &target=job["target"];
    const auto label=text(target["label"]);
    const auto &entry=p.targets[label];
    require(!entry.is_null(),"target absent from product");
    for(const auto &key:{"source_entry","sentence"})
        require(equal(target[key],entry[key]),std::string("product mismatch: target.")+key);
    for(const auto &key:{"instance","module"})
        require(equal(target[key],p.head[key]),std::string("product mismatch: target.")+key);
    require(equal(target["top"],p.head["wrapper"]),"product wrapper mismatch");
    const auto sentenceDigest=digest(text(entry["sentence"]));
    require(text(target["sentence_sha256"])==sentenceDigest,"sentence digest mismatch");
    for(const auto &key:{"clock","clock_edge","clock_model"})
        require(equal(job["setup"][key],entry[key]),std::string("product mismatch: setup.")+key);
    require(equal(job["setup"]["reset"],p.reset),"reset setup differs from product");
    const auto expected=jobPremises(p,label);
    require(equal(expected,job["premises"]),"compiled premises differ from Job");
    J verified=object({{"verified",boolean(true)},{"path",str(p.path.string())},{"sha256",str(fileDigest(p.path))},
        {"instance",p.head["instance"]},{"module",p.head["module"]},{"top",p.head["wrapper"]},{"reset",p.reset},
        {"targets",p.targets},{"assumes",p.assumes},{"compiled_assumes",p.order},{"derived_premises",p.derived},
        {"verified_target",entry},{"verified_premises",expected}});
    for(const auto &key:{"label","source_entry","clock","clock_edge","clock_model","premises"})
        put(verified,key,entry[key]);
    put(verified,"sentence_sha256",str(sentenceDigest));
    put(verified,"reset_kind",p.reset["kind"]);
    put(verified,"initial_state",p.reset["initial_state"]);
    return verified;
}
} // namespace veval::verification
