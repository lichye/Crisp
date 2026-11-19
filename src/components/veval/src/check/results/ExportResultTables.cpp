#include "../../util/task/CommandArgumentsAndFiles.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <regex>
#include <set>
#include <sstream>

int main(int argc,char **argv) {
    using namespace veval::verification;
    try {
        Arguments a(argc,argv,{"--help"},{"--spec","--top","--log","--name"});
        if(a.has("--help")){std::cout<<"consflow_table --spec FILE --top MODULE --log DIR [--name NAME]\n";return 0;}
        const auto spec=absolute(a.required("--spec")),log=absolute(a.required("--log")),top=a.required("--top");
        std::map<std::string,std::string> kinds;std::string kind;
        std::istringstream input(read(spec));std::smatch m;
        const std::regex category(R"(^\s*//\s*semantic_class:\s*(\S+))"),label(R"(^\s*([A-Za-z_]\w*)\s*:\s*(?:assert|assume)\s+property)");
        for(std::string line;std::getline(input,line);) {
            if(std::regex_search(line,m,category))kind=m[1];
            else if(std::regex_search(line,m,label)) {
                const std::string name=m[1];if(name.rfind("reset_contract",0)==0)continue;
                if(kind.empty())throw std::runtime_error("missing semantic_class for "+name);
                kinds[name]=kind;
            }
        }
        std::map<std::string,std::array<unsigned,4>> totals;
        for(const auto &[label,category]:kinds)++totals[category][0];
        unsigned overlap=0;Budget budget(std::numeric_limits<double>::infinity());
        streamObject(fs::path(log)/"transport.json",budget,[](const auto &,J&&){},[&](const std::string &key,J&&row) {
            if(key!="transports")return;const auto name=text(row["property"]);auto at=name.rfind('.');auto found=kinds.find(name.substr(at==name.npos?0:at+1));
            if(found==kinds.end())return;++overlap;auto &count=totals[found->second];const auto result=text(row["result"]);
            if(result=="full"&&truth(row["round_trip"]))++count[1];
            else if(result=="partial"&&truth(row["round_trip"]))++count[2];
            else if(result=="blocked")for(const auto &signal:items(row["signals"]))if(truth(signal["reached"])||truth(signal["primary_reason_code"])){++count[3];break;}
        });
        if(!overlap)throw std::runtime_error("no overlap between source entries and transport log");
        std::ostringstream out;out<<"# "<<a.get("--name",top)<<" (top = "<<top<<")\n\n| kind | intents | Tier-1 exact | Tier-1 partial | Tier-2 notices |\n|---|---:|---:|---:|---:|\n";
        std::array<unsigned,4> sum{};std::vector<std::string> order;
        for(const auto &[category,count]:totals)order.push_back(category);
        std::sort(order.begin(),order.end(),[&](const auto &x,const auto &y){return totals[x][0]!=totals[y][0]?totals[x][0]>totals[y][0]:x<y;});
        for(const auto &category:order) {out<<"| "<<category;for(unsigned i=0;i<4;++i){out<<" | "<<totals[category][i];sum[i]+=totals[category][i];}out<<" |\n";}
        out<<"| **total**";for(auto n:sum)out<<" | "<<n;out<<" |\n\nIntents count source entries; Tier-1/2 columns count landings (entry × instance).\n";
        write(fs::path(log)/"table.md",out.str());std::cout<<out.str();return 0;
    }catch(const std::exception &e){std::cerr<<"consflow_table: "<<e.what()<<'\n';return 2;}
}
