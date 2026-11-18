#include "CheckMemoryAdmission.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unistd.h>

namespace veval::verification {
static double positive(const std::string &s,bool zero=false) {
    size_t used;double value=std::stod(s,&used);
    if(used!=s.size()||!std::isfinite(value)||(zero?value<0:value<=0))throw std::runtime_error("invalid pool budget");
    return value;
}
Pool::Pool(const Arguments &a) {
    const auto requested=positive(a.get("--workers","1"));
    if(requested!=std::floor(requested)||requested>256)throw std::runtime_error("workers must be in [1,256]");
    workers=requested;
#ifdef __APPLE__
    if (a.has("--pool-memory-gb") || a.has("--admission-gb") || a.has("--reserve-gb"))
        throw std::runtime_error("aggregate memory admission is unavailable on macOS");
#else
    if (workers==1) return;
    const auto maximum=positive(a.required("--pool-memory-gb"))*1073741824.0;
    estimate=positive(a.required("--admission-gb"))*1073741824.0;
    reserve=positive(a.get("--reserve-gb","30"),true)*1073741824.0;
    std::istringstream in(read("/proc/self/cgroup"));std::string path,line;
    while(std::getline(in,line))if(line.rfind("0::",0)==0)path=line.substr(3);
    fs::path dir=fs::path("/sys/fs/cgroup")/fs::path(path).relative_path();
    if(!fs::exists(dir/"memory.max"))dir="/sys/fs/cgroup";
    while(dir.string().rfind("/sys/fs/cgroup",0)==0) {
        if(fs::exists(dir/"memory.max")) {
            const auto s=trim(read(dir/"memory.max"));
            if(s!="max") {const auto value=std::stoull(s);if(!limit||value<limit){limit=value;group=dir;}}
        }
        if(dir=="/sys/fs/cgroup")break;dir=dir.parent_path();
    }
    if(!limit||limit>maximum||estimate>limit)throw std::runtime_error("parallel run requires an existing aggregate cgroup memory limit at or below --pool-memory-gb");
    // An estimate that fills the whole limit admits nobody once anything is
    // charged, and the batch then waited forever with no message.
    if(estimate>=double(limit))throw std::runtime_error("--admission-gb must be below the aggregate memory limit, or no second worker is ever admitted");
#endif
}
void Pool::sample() {
    if(limit)peak=std::max(peak,double(std::stoull(trim(read(group/"memory.current")))));
}
#ifndef __APPLE__
// Resident bytes of each worker's process tree, read from /proc. A solver
// started under setsid is still a descendant of its adapter.
static std::vector<double> treeResident(const std::vector<int> &roots) {
    std::map<int,std::vector<int>> children;std::map<int,double> resident;
    const double page=double(::sysconf(_SC_PAGESIZE));
    for(const auto &entry:fs::directory_iterator("/proc")) {
        const auto name=entry.path().filename().string();
        if(name.empty()||name.find_first_not_of("0123456789")!=std::string::npos)continue;
        std::ifstream stat(entry.path()/"stat"),statm(entry.path()/"statm");std::string line;
        if(!std::getline(stat,line))continue;   // the process exited meanwhile
        const auto close=line.rfind(')');if(close==std::string::npos)continue;
        std::istringstream rest(line.substr(close+1));std::string state;int parent=0;
        if(!(rest>>state>>parent))continue;
        const int pid=std::stoi(name);children[parent].push_back(pid);
        unsigned long long size=0,pages=0;if(statm>>size>>pages)resident[pid]=double(pages)*page;
    }
    std::vector<double> result;
    for(const int root:roots) {
        double total=0;std::vector<int> stack{root};std::set<int> seen;
        while(!stack.empty()){const int pid=stack.back();stack.pop_back();if(!seen.insert(pid).second)continue;
            total+=resident.count(pid)?resident[pid]:0;for(const int c:children[pid])stack.push_back(c);}
        result.push_back(total);
    }
    return result;
}
#endif
bool Pool::admit(const std::vector<int> &running) {
    const auto active=running.size();
    if(active>=workers)return false;
#ifdef __APPLE__
    return true;
#else
    if(workers==1)return true;
    const auto used=std::stoull(trim(read(group/"memory.current")));peak=std::max(peak,double(used));
    std::istringstream in(read("/proc/meminfo"));std::string key,unit;double value=0,available=0;
    for(std::string line;std::getline(in,line);){std::istringstream row(line);if(row>>key>>value>>unit&&key=="MemAvailable:"){available=value*1024;break;}}
    // What is in use, plus what each running worker may still claim before
    // it reaches its estimate. Charging max(used, estimate*active) instead
    // let one worker far above its estimate hide the estimates of workers
    // that had not allocated yet, and admitted a worker the pool could not
    // hold (srcProof/Runtime/Pool.v, admit_overrun_counterexample). A tree's
    // resident size never exceeds what the group charges for it, so the
    // growth still to come is if anything overstated.
    double growth=0;for(const auto bytes:treeResident(running))growth+=std::max(0.0,estimate-bytes);
    const auto charge=double(used)+growth;
    const auto needed=charge-used+estimate;
    return charge+estimate<=limit&&available-reserve>=needed;
#endif
}
}
