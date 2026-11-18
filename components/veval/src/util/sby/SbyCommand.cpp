#include "SbyRunner.h"
#include <iostream>
#include <limits>
int main(int argc,char **argv) {
    using namespace veval::verification;
    try {
        installCancellationHandlers();Arguments args(argc,argv,{"--help"},{"--request"});
        if(args.has("--help")){std::cout<<"consflow_sby cell --request FILE\n";return 0;}
        if(args.positional!=std::vector<std::string>{"cell"})throw std::runtime_error("expected cell --request FILE");
        J request=parse(read(args.required("--request")));double deadline=std::numeric_limits<double>::infinity();
        // Legacy cell requests have a per-stage cap. An explicit control
        // deadline, when supplied, limits the complete attempt.
        if(truth(request["control"])) {auto control=parse(read(text(request["control"])));if(control["deadline_monotonic"].is_number())deadline=std::stod(control["deadline_monotonic"].value);}
        auto result=runSby(request,deadline);
        std::cout<<"sby_verdict\t"<<text(result["sby_verdict"])<<"\nsby_detail\t"<<text(result["sby_detail"])<<"\nrecord\t"<<(fs::path(absolute(text(request["work_dir"]))) / "cell.json").string()<<'\n';
        std::cout<<"audit\t"<<text(result["premise_audit"]["outcome"])<<'\n';
        for(const auto &stage:items(result["stages"])) {
            if(!stage["liveness_trace"].is_null())std::cout<<"liveness_audit\t"<<text(stage["liveness_trace"]["status"])<<'\n';
            for(const auto &path:items(stage["traces"]))if(text(path).find("induct")==std::string::npos){std::cout<<"trace\t"<<text(path)<<'\n';break;}
        }
        return 0;
    }catch(const std::exception &e){std::cerr<<"consflow_sby: "<<e.what()<<'\n';return 2;}
}
