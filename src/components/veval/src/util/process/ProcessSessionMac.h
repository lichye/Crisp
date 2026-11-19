#pragma once

#include <cerrno>
#include <csignal>
#include <cstring>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <vector>

namespace veval::util {
namespace process_session_detail {
inline void appendError(std::string& all, const std::string& one) {
    if (one.empty()) return;
    if (!all.empty()) all += "; ";
    all += one;
}
}
namespace mac_session_detail {
inline std::string problem(const char* action, pid_t pid, int code) {
    return std::string(action) + " pid " + std::to_string(pid) + ": " + std::strerror(code);
}
inline bool owner(pid_t sid, std::string& error) {
    if (sid <= 0) { error = "invalid owned session id"; return false; }
    const auto actual = ::getsid(sid);
    if (actual == sid) return true;
    if (actual < 0 && errno == ESRCH) {
        // Darwin hides exited processes from getsid/proc_pidinfo. A wait-owned
        // unreaped leader still reserves the SID and can be checked with waitid.
        siginfo_t info{};
        if (::waitid(P_PID, sid, &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == sid)
            return true;
    }
    error = "owned session leader identity or wait ownership unavailable";
    return false;
}
inline bool inventory(pid_t sid, std::vector<pid_t>& members, std::string& error) {
    members.clear(); error.clear();
    if (!owner(sid, error)) return false;
    const int needed = ::proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (needed <= 0) { error = "cannot size macOS process inventory"; return false; }
    std::vector<pid_t> pids(static_cast<std::size_t>(needed) / sizeof(pid_t) + 128);
    int bytes = 0;
    for (;;) {
        if (pids.size() > 1000000) { error = "macOS process inventory too large"; return false; }
        bytes = ::proc_listpids(PROC_ALL_PIDS, 0, pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
        if (bytes <= 0 || bytes % sizeof(pid_t)) { error = "cannot enumerate macOS processes"; return false; }
        if (static_cast<std::size_t>(bytes) < pids.size() * sizeof(pid_t)) break;
        pids.resize(pids.size() * 2);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(bytes) / sizeof(pid_t); ++i) {
        const auto pid = pids[i]; if (pid <= 0) continue;
        const auto candidate = ::getsid(pid);
        if (candidate < 0) {
            if (errno == ESRCH) continue; // Includes exited Darwin processes.
            error = problem("identify process session", pid, errno); return false;
        }
        if (candidate == sid) members.push_back(pid);
    }
    return owner(sid, error);
}
inline bool token(pid_t pid, audit_token_t& audit, std::string& error) {
    task_name_t task = MACH_PORT_NULL;
    const auto opened = ::task_name_for_pid(mach_task_self(), pid, &task);
    if (opened != KERN_SUCCESS) {
        error = "cannot acquire macOS task identity for pid " + std::to_string(pid) + ": " + mach_error_string(opened);
        return false;
    }
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    const auto fetched = ::task_info(task, TASK_AUDIT_TOKEN, reinterpret_cast<task_info_t>(&audit), &count);
    ::mach_port_deallocate(mach_task_self(), task);
    if (fetched != KERN_SUCCESS || count != TASK_AUDIT_TOKEN_COUNT) {
        error = "cannot read macOS audit identity for pid " + std::to_string(pid); return false;
    }
    return true;
}
} // namespace mac_session_detail

// The child is held at its pre-exec barrier with SIGCONT at the default
// disposition. Sending it verifies both identity acquisition and signalling.
inline bool probePidfd(pid_t pid, pid_t expectedSid, std::string& error) {
    using namespace mac_session_detail;
    error.clear();
    if (pid != expectedSid) { error = "session leader PID/SID mismatch"; return false; }
    if (!owner(expectedSid, error)) return false;
    std::vector<pid_t> members;
    if (!inventory(expectedSid, members, error)) return false;
    bool present = false;
    for (const auto member : members) if (member == pid) present = true;
    if (!present) { error = "session leader absent from preflight inventory"; return false; }
    audit_token_t audit{};
    if (!token(pid, audit, error)) return false;
    if (::getsid(pid) != expectedSid) { error = "session leader changed during preflight"; return false; }
    const int rc = ::proc_signal_with_audittoken(&audit, SIGCONT);
    if (rc != 0) { error = problem("audit-token signal preflight", pid, rc); return false; }
    return true;
}
inline bool signalSessionExcept(pid_t sid, pid_t excluded, int signal, std::string& error) {
    using namespace mac_session_detail;
    error.clear();
    if (signal <= 0 || signal >= NSIG) { error = "invalid session signal"; return false; }
    std::vector<pid_t> members;
    if (!inventory(sid, members, error)) return false;
    for (const auto pid : members) {
        if (pid == excluded) continue;
        audit_token_t audit{}; std::string failure;
        if (!token(pid, audit, failure)) {
            if (::getsid(pid) < 0 && errno == ESRCH) continue;
            error = failure; return false;
        }
        const auto current = ::getsid(pid);
        if (current != sid) {
            if (current < 0 && errno == ESRCH) continue;
            error = "session member changed before signal: " + std::to_string(pid); return false;
        }
        const int rc = ::proc_signal_with_audittoken(&audit, signal);
        if (rc != 0) {
            if (rc == ESRCH && ::getsid(pid) < 0 && errno == ESRCH) continue;
            error = problem("audit-token session signal", pid, rc); return false;
        }
    }
    return owner(sid, error);
}
inline bool signalSession(pid_t sid, int signal, std::string& error) {
    return signalSessionExcept(sid, -1, signal, error);
}
inline bool inspectSessionExcept(pid_t sid, pid_t excluded, std::size_t& active, std::string& error) {
    std::vector<pid_t> members;
    const bool complete = mac_session_detail::inventory(sid, members, error);
    active = 0;
    for (const auto pid : members) if (pid != excluded) ++active;
    return complete;
}
inline bool probeSessionMember(pid_t pid, pid_t sid, std::string& error) {
    using namespace mac_session_detail;
    std::vector<pid_t> members;
    if (!inventory(sid, members, error)) return false;
    bool found = false;
    for (const auto member : members) if (member == pid) found = true;
    if (!found) { error = "child absent from session preflight inventory"; return false; }
    audit_token_t audit{};
    if (!token(pid, audit, error)) return false;
    if (::getsid(pid) != sid) { error = "child changed session during preflight"; return false; }
    const int rc = ::proc_signal_with_audittoken(&audit, SIGCONT);
    if (rc != 0) { error = problem("audit-token signal preflight", pid, rc); return false; }
    return true;
}
inline bool inspectSession(pid_t sid, std::size_t& active, std::string& error) {
    std::vector<pid_t> members;
    const bool complete = mac_session_detail::inventory(sid, members, error);
    active = members.size();
    return complete;
}
} // namespace veval::util
