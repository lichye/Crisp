#ifdef __APPLE__
#include "ProcessSessionMac.h"
#else
// Linux owned-session cleanup across descendant process-group changes.
// The caller must keep its setsid() leader unreaped (waitid WNOWAIT) while
// scanning/signalling. No waiting or fresh cleanup/solver allowance lives here.
#ifndef VEVAL_UTIL_PROCESSSESSION_H
#define VEVAL_UTIL_PROCESSSESSION_H
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
namespace veval::util {
namespace process_session_detail {
struct Descriptor {
    int fd;
    explicit Descriptor(int value) : fd(value) {}
    ~Descriptor() { if (fd >= 0) ::close(fd); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};
struct Process { pid_t pid=0, session=0; char state=0; unsigned long long start=0; };
inline bool gone(int code) { return code==ENOENT || code==ESRCH; }
inline bool active(const Process& process) { return process.state!='Z' && process.state!='X' && process.state!='x'; }
inline std::string problem(const char* action,pid_t pid,int code) {
    return std::string(action)+" pid "+std::to_string(pid)+": "+std::strerror(code);
}
// False with an empty error means the process disappeared during the scan.
inline bool readProcess(pid_t pid,Process& process,std::string& error) {
    error.clear();
    const auto path="/proc/"+std::to_string(pid)+"/stat";
    Descriptor fd(::open(path.c_str(),O_RDONLY|O_CLOEXEC));
    if(fd.fd<0) {const auto code=errno;if(!gone(code))error=problem("read session stat",pid,code);return false;}
    std::string data;
    char buffer[4096];
    while(true) {
        auto size=::read(fd.fd,buffer,sizeof buffer);
        if(size>0) {
            if(data.size()+static_cast<std::size_t>(size)>65536) {error="oversized /proc stat for pid "+std::to_string(pid);return false;}
            data.append(buffer,static_cast<std::size_t>(size));
        } else if(size==0) break;
        else if(errno==EINTR) continue;
        else {const auto code=errno;if(!gone(code))error=problem("read session stat",pid,code);return false;}
    }
    if(data.empty()) return false; // a proc record can disappear after open
    // comm may itself contain spaces, ')' and newlines. The remaining fields
    // contain no ')' so the final delimiter, not token splitting, is exact.
    const auto left=data.find('('),right=data.rfind(')');
    if(left==std::string::npos||right==std::string::npos||right<=left) {error="malformed /proc stat for pid "+std::to_string(pid);return false;}
    std::istringstream head(data.substr(0,left)), tail(data.substr(right+1));
    long long actual=0,parent=0,group=0,session=0;
    std::string unused;
    if(!(head>>actual)||actual!=pid||!(tail>>process.state>>parent>>group>>session)||
       session<0||session>std::numeric_limits<pid_t>::max()) {error="invalid /proc identity for pid "+std::to_string(pid);return false;}
    // state/ppid/pgrp/session are fields 3..6; starttime is field 22.
    for(unsigned field=7;field<22;++field) if(!(tail>>unused)) {error="short /proc stat for pid "+std::to_string(pid);return false;}
    if(!(tail>>process.start)) {error="missing /proc start time for pid "+std::to_string(pid);return false;}
    process.pid=pid;process.session=static_cast<pid_t>(session);return true;
}
inline bool owner(pid_t sid,std::string& error) {
    if(sid<=0) {error="invalid owned session id";return false;}
    Process leader;
    if(!readProcess(sid,leader,error)) {if(error.empty())error="owned session leader is no longer retained";return false;}
    if(leader.session!=sid) {error="owned session leader identity mismatch";return false;}
    return true;
}
inline bool inventory(pid_t sid,std::vector<Process>& members,std::string& error) {
    error.clear();members.clear();
    if(!owner(sid,error)) return false;
    std::unique_ptr<DIR, int(*)(DIR*)> directory(::opendir("/proc"),::closedir);
    if(!directory) {error=std::string("scan /proc: ")+std::strerror(errno);return false;}
    bool success=true;
    const auto note=[&](const std::string& failure) {
        if(failure.empty())return;
        if(!error.empty())error+="; ";
        error+=failure;
        success=false;
    };
    while(true) {
        errno=0;
        auto entry=::readdir(directory.get());
        if(!entry) {if(errno)note(std::string("scan /proc: ")+std::strerror(errno));break;}
        char* end=nullptr;
        errno=0;auto number=std::strtol(entry->d_name,&end,10);
        if(errno||end==entry->d_name||*end||number<=0||number>std::numeric_limits<pid_t>::max()) continue;
        const auto pid=static_cast<pid_t>(number);
        // Unrelated sessions need no proc stat access (notably hidepid or
        // multi-user hosts). Only an owned-session member's unreadable record
        // compromises this cleanup inventory.
        const auto candidate=::getsid(pid);
        if(candidate<0) {
            const auto code=errno;
            if(gone(code))continue;
            Process fallback;std::string failure;
            if(readProcess(pid,fallback,failure)) {
                if(fallback.session==sid&&active(fallback))members.push_back(fallback);
                continue;
            }
            if(failure.empty())continue;
            note(problem("identify process session",pid,code)+"; "+failure);
            continue;
        }
        if(candidate!=sid)continue;
        Process process;std::string failure;
        if(!readProcess(pid,process,failure)) {
            if(!failure.empty()) {
                const auto current=::getsid(pid);
                if(current>=0&&current!=sid)continue;
                if(current<0&&gone(errno))continue;
                note(failure);
            }
            continue;
        }
        if(process.session==sid&&active(process)) members.push_back(process);
    }
    const auto closed=::closedir(directory.release());
    if(closed<0)note(std::string("close /proc scan: ")+std::strerror(errno));
    // Fail if the caller violated retained-leader ownership during enumeration.
    std::string ownerError;
    if(!owner(sid,ownerError))note(ownerError);
    return success;
}
inline int openPid(pid_t pid) {
#ifdef SYS_pidfd_open
    return static_cast<int>(::syscall(SYS_pidfd_open,pid,0));
#else
    errno=ENOSYS;return -1;
#endif
}
inline int sendPid(int fd,int signal) {
#ifdef SYS_pidfd_send_signal
    return static_cast<int>(::syscall(SYS_pidfd_send_signal,fd,signal,
                                     static_cast<siginfo_t*>(nullptr),0u));
#else
    errno=ENOSYS;return -1;
#endif
}
inline void appendError(std::string& all,const std::string& one) {
    if(one.empty())return;
    if(!all.empty())all+="; ";
    all+=one;
}
} // namespace process_session_detail

// Probe both pidfd_open and pidfd_send_signal before a session leader is
// allowed to exec an adapter that can create descendants.
inline bool probePidfd(pid_t pid,pid_t expectedSid,std::string& error) {
    using namespace process_session_detail;
    error.clear();
    Process before;
    if(!readProcess(pid,before,error)) {
        if(error.empty())error="session leader disappeared during pidfd preflight";
        return false;
    }
    if(before.session!=expectedSid) {error="session leader SID mismatch during pidfd preflight";return false;}
    Descriptor fd(openPid(pid));
    if(fd.fd<0) {const auto code=errno;error=problem("pidfd_open preflight",pid,code);return false;}
    Process after;std::string failure;
    if(!readProcess(pid,after,failure)) {error=failure.empty()?"session leader disappeared during pidfd preflight":failure;return false;}
    if(after.session!=expectedSid||after.start!=before.start) {error="session leader identity changed during pidfd preflight";return false;}
    if(sendPid(fd.fd,0)<0) {const auto code=errno;error=problem("pidfd_send_signal(0) preflight",pid,code);return false;}
    return true;
}

// True means a complete sweep succeeded, not that asynchronous signal delivery
// finished. Repeat this plus inspectSession within the caller's existing cleanup
// deadline to cover forks during a prior sweep. Gone PIDs are benign; inaccessible
// or changing identities conservatively fail. There is deliberately no kill(pid)
// fallback when stable pidfd signalling is unavailable.
inline bool signalSession(pid_t ownedSid,int signal,std::string& error) {
    using namespace process_session_detail;
    error.clear();
    if(signal<0||signal>=NSIG) {error="invalid session signal";return false;}
    std::vector<Process> members;
    const bool complete=inventory(ownedSid,members,error);
    std::string errors=error;
    if(!complete&&errors.empty())errors="session inventory was incomplete";
    for(const auto& before:members) {
        Descriptor fd(openPid(before.pid));
        if(fd.fd<0) {const auto code=errno;if(code==ESRCH)continue;appendError(errors,problem("pidfd_open session member",before.pid,code));continue;}
        Process after;std::string failure;
        if(!readProcess(before.pid,after,failure)) {if(failure.empty())continue;appendError(errors,failure);continue;}
        if(after.start!=before.start||after.session!=ownedSid) {appendError(errors,"session member identity changed while acquiring pidfd: "+std::to_string(before.pid));continue;}
        if(!active(after)) continue;
        if(sendPid(fd.fd,signal)<0) {const auto code=errno;if(code==ESRCH)continue;appendError(errors,problem("pidfd_send_signal session member",before.pid,code));continue;}
    }
    std::string ownerError;
    if(!owner(ownedSid,ownerError))appendError(errors,ownerError);
    error=std::move(errors);
    return error.empty();
}
// True means the native scan is complete and active is trustworthy for that
// snapshot. Only active==0 establishes cleanup; retained zombies are excluded.
// Session migration via a new setsid() is outside this ownership scope. Linux
// provides no atomic signal-if-SID operation, so the caller must not permit a
// descendant to deliberately migrate its SID between the final check and signal.
inline bool inspectSession(pid_t ownedSid,std::size_t& active,std::string& error) {
    active=0;
    std::vector<process_session_detail::Process> members;
    const bool complete=process_session_detail::inventory(ownedSid,members,error);
    active=members.size();return complete;
}
} // namespace veval::util
#endif

#endif // __APPLE__
