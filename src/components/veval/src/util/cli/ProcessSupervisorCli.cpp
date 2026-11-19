#include "../process/RunWithResourceLimits.h"
#include <cmath>
#include <iostream>
#include <sstream>
#include <poll.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

// This executable supervises one command. Subreaping makes nested native
// sessions wait-owned children if their adapter crashes during cleanup.
#ifdef __linux__
static bool reapAdopted() {
    using namespace veval::verification;
    const auto end=monotonic()+2;
    while (true) {
        const auto pid=::getpid();
        std::istringstream children(read("/proc/self/task/"+std::to_string(pid)+"/children"));
        pid_t child; bool any=false;
        while (children>>child) {
            any=true; siginfo_t info{};
            if (::waitid(P_PID,child,&info,WEXITED|WNOHANG|WNOWAIT)<0) return false;
            if (info.si_pid==child) {int status;::waitpid(child,&status,0);}
            else ::kill(child,SIGKILL);
        }
        if(!any)return true;
        if(monotonic()>=end)return false;
        ::poll(nullptr,0,20);
    }
}
#endif
int main(int argc,char **argv) {
    using namespace veval::verification;
    try {
        installCancellationHandlers();Arguments a(argc,argv,{"--help"},{"--directory","--timeout","--memory","--cwd","--runtime","--manifest"});
        if(a.has("--help")){
            std::cout<<"consflow_process --directory DIR [--timeout S] [--memory GiB] -- COMMAND ...\n";
#ifdef __APPLE__
            std::cout<<"macOS supervises one session. Only --memory 0 is supported.\n";
#endif
            return 0;
        }
#ifdef __linux__
        if(::prctl(PR_SET_CHILD_SUBREAPER,1))throw std::runtime_error("cannot establish nested process ownership");
#endif
        const fs::path dir=absolute(a.required("--directory"));fs::create_directories(dir);
        const auto timeout=std::stod(a.get("--timeout","0")),memory=std::stod(a.get("--memory","0"));
        if(!std::isfinite(timeout)||timeout<0||!std::isfinite(memory)||memory<0||memory>1048576)throw std::runtime_error("invalid resource limit");
        // Zero bytes means "no limit" below, so a positive request that rounds
        // down to zero would silently lift the limit it asked for.
        if(memory>0&&static_cast<unsigned long long>(memory*1073741824.0)==0)throw std::runtime_error("memory limit below one byte");
#ifdef __APPLE__
        if (memory > 0) throw std::runtime_error("process-tree memory limits are unavailable on macOS; use --memory 0");
#endif
        J command=list();for(const auto &s:a.positional)append(command,str(s));
        auto receipt=object({{"schema",str("consflow-process/1")},{"argv",command},{"status",str("running")},{"cleanup_confirmed",boolean(false)}});
        save(dir/"command.json",receipt);
        auto done=execute(a.positional,dir/"stdout.log",a.get("--cwd",a.get("--runtime",".")),
            timeout?monotonic()+timeout:std::numeric_limits<double>::infinity(),static_cast<unsigned long long>(memory*1073741824.0));
#ifdef __linux__
        if(!reapAdopted()){done.cleanupComplete=false;done.error="nested child cleanup unconfirmed";}
#else
        put(receipt, "supervision_scope", str("session"));
#endif
        const bool success=done.exitCode==0&&done.cleanupComplete&&!done.stopped&&!done.timedOut;
        put(receipt,"status",str(success?"success":done.timedOut?"timeout":done.stopped?"cancelled":"failed"));
        put(receipt,"reason",str(done.timedOut?"outer command deadline (not a solver verdict)":done.error));
        put(receipt,"cleanup_confirmed",boolean(done.cleanupComplete));put(receipt,"wall_s",num(done.elapsed));
        put(receipt,"returncode",num(done.exitCode));put(receipt,"max_rss_kb",num(done.maxRssKb));
        save(dir/"command.json",receipt);return success?0:2;
    }catch(const std::exception &e){std::cerr<<"consflow_process: "<<e.what()<<'\n';return 2;}
}
