// Validate the concrete lasso produced by the full-state recurrence reduction.
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
struct Signal { std::string name; unsigned width; };
using Values = std::map<std::string, std::string>;
void require(bool yes, const std::string& why) { if (!yes) throw std::runtime_error(why); }
bool binary(const std::string& text) { return !text.empty() && text.find_first_not_of("01") == std::string::npos; }
int main(int argc, char** argv) {
    try {
        require(argc == 4, "usage: consflow_live_trace TRACE.vcd TOP REDUCTION.tsv");
        unsigned width=0; std::string line;
        std::ifstream audit(argv[3]); require(bool(audit), "liveness reduction audit missing");
        bool schema=false;
        while (std::getline(audit,line)) {
            if (line=="schema\tconsflow-live2safety/1") schema=true;
            if (line.rfind("state_bits\t",0)==0) width=std::stoul(line.substr(11));
        }
        require(schema && width>0, "invalid liveness reduction schema/state width");
        std::ifstream in(argv[1]); require(bool(in), "VCD unavailable");
        std::map<std::string,Signal> signals;
        std::vector<std::string> scope;
        std::vector<Values> steps;
        Values values;
        std::string counter; bool frame=false, sampled=false;
        auto flush=[&] {
            if (sampled) { steps.push_back(values); sampled=false; }
        };
        while (std::getline(in,line)) {
            std::istringstream tokens(line); std::string word;
            tokens >> word; if(word.empty()) continue;
            if(word=="$scope") { std::string type,name; tokens>>type>>name; scope.push_back(name); }
            else if(word=="$upscope") { require(!scope.empty(),"malformed VCD scope"); scope.pop_back(); }
            else if(word=="$var") {
                std::string type,id,name; unsigned n; tokens>>type>>n>>id>>name;
                if(name=="smt_step" && scope.empty()) {counter=id;signals[id]={name,n};}
                if(scope.size()==1 && scope[0]==argv[2] &&
                   (name.rfind("veval_live_",0)==0 || name=="clk")) {
                    require(std::none_of(signals.begin(),signals.end(),[&](const auto& p){return p.second.name==name;}),"ambiguous liveness VCD signal");
                    signals[id]={name,n};
                }
            } else if(word[0]=='#') { flush(); frame=true; }
            else if(frame) {
                std::string id,bits;
                if(word[0]=='b' || word[0]=='B') { bits=word.substr(1); tokens>>id; }
                else if(std::string("01xzXZ").find(word[0])!=std::string::npos) {bits=word.substr(0,1);id=word.substr(1);}
                auto found=signals.find(id); if(found==signals.end()) continue;
                if(bits.size()<found->second.width) bits=std::string(found->second.width-bits.size(),bits[0]=='1'?'0':bits[0])+bits;
                require(bits.size()==found->second.width,"liveness VCD width mismatch");
                values[found->second.name]=bits;
                if(id==counter) sampled=true;
            }
        }
        flush(); require(!counter.empty() && steps.size()>=2,"no nonempty sampled trace");
        const std::vector<std::string> scalar={"veval_live_armed","veval_live_save","veval_live_ok","veval_live_loop","clk"};
        for(size_t i=0;i<steps.size();++i) {
            for(const auto& name:scalar) require(steps[i][name].size()==1 && binary(steps[i][name]),"missing/unknown liveness control "+name);
            require(steps[i]["clk"]=="1", "trace does not use declared posedge samples");
            require(binary(steps[i]["smt_step"]) && std::stoul(steps[i]["smt_step"],nullptr,2)==i,"trace step sequence mismatch");
        }
        for(size_t end=1;end<steps.size();++end) {
            if(steps[end]["veval_live_loop"]!="1") continue;
            const auto final=steps[end]["veval_live_current"];
            require(final.size()==width && binary(final) && steps[end]["veval_live_saved"]==final,"loop full-state equality missing");
            require(steps[end]["veval_live_armed"]=="1" && steps[end]["veval_live_ok"]=="0","invalid loop endpoint");
            for(size_t start=0;start<end;++start) {
                if(steps[start]["veval_live_armed"]!="0" || steps[start]["veval_live_save"]!="1" || steps[start]["veval_live_current"]!=final) continue;
                bool valid=true;
                for(size_t i=start;i<end;++i) {
                    valid &= steps[i]["veval_live_ok"]=="0";
                    if(i>start) valid &= steps[i]["veval_live_armed"]=="1" && steps[i]["veval_live_saved"]==final;
                }
                if(valid) {
                    std::cout<<"status\tchecked\nkind\tlasso\nloop_start\t"<<start<<"\nloop_end\t"<<end<<"\nstate_bits\t"<<width<<'\n';
                    return 0;
                }
            }
            throw std::runtime_error("loop has no matching capture and uninterrupted bad cycle");
        }
        throw std::runtime_error("no recurrence assertion violation in trace");
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
