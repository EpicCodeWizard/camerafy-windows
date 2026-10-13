#include "tuning.h"
#include "console.h"
#include <avrt.h>
#include <powrprof.h>
#include <psapi.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <winsvc.h>
#include <stdio.h>
#include <map>
#include <algorithm>

namespace cf::tuning {

const char* roleName(Role r) {
    switch (r) {
    case Role::Recv: return "recv";
    case Role::Assembler: return "assembler";
    case Role::Decode: return "decode";
    case Role::Mapper: return "mapper";
    case Role::Sideband: return "sideband";
    default: return "?";
    }
}

namespace {

struct Lp {
    ULONG id;
    BYTE group, lp, core, llc, eff;
    bool parked, allocatedElsewhere;
};

std::vector<Lp> cpuSets() {
    std::vector<Lp> out;
    ULONG len = 0;
    GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
    std::vector<uint8_t> buf(len);
    if (!GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf.data(), len, &len, GetCurrentProcess(), 0)) return out;
    for (size_t o = 0; o < len;) {
        auto* e = (SYSTEM_CPU_SET_INFORMATION*)(buf.data() + o);
        if (e->Type == CpuSetInformation) {
            const auto& c = e->CpuSet;
            out.push_back({c.Id, (BYTE)c.Group, c.LogicalProcessorIndex, c.CoreIndex, c.LastLevelCacheIndex,
                           c.EfficiencyClass, c.Parked != 0, c.AllocatedToTargetProcess == 0 && c.Allocated != 0});
        }
        o += e->Size;
    }
    return out;
}

}  // namespace

CorePlan planCores(const Options& o) {
    CorePlan plan;
    if (!o.pinThreads) {
        plan.report = "thread pinning disabled (--no-pin)";
        return plan;
    }
    auto lps = cpuSets();
    if (lps.empty()) {
        plan.report = "GetSystemCpuSetInformation failed; threads unpinned";
        return plan;
    }
    BYTE maxEff = 0;
    for (auto& l : lps) maxEff = std::max(maxEff, l.eff);
    // physical cores (group, core) of the top efficiency class, by LLC
    std::map<BYTE, std::vector<std::pair<int, ULONG>>> byLlc;  // llc -> [(core key, first lp's cpu set id)]
    BYTE core0 = 0xFF;
    for (auto& l : lps)
        if (l.group == 0 && l.lp == 0) core0 = l.core;
    std::map<int, bool> seen;
    for (auto& l : lps) {
        if (l.eff != maxEff || l.parked) continue;
        const int key = l.group * 256 + l.core;
        if (seen[key]) continue;
        seen[key] = true;
        byLlc[l.llc].push_back({key, l.id});
    }
    // Biggest LLC domain; on a single-CCD part this is all of them.
    auto best = byLlc.begin();
    for (auto it = byLlc.begin(); it != byLlc.end(); ++it)
        if (it->second.size() > best->second.size()) best = it;
    auto cores = best->second;
    const size_t roles = (size_t)Role::Count;
    if (cores.size() > roles) {
        cores.erase(std::remove_if(cores.begin(), cores.end(), [&](auto& c) { return (c.first & 0xFF) == core0; }),
                    cores.end());
    }
    // Hot stages get dedicated cores first; the sideband thread is least critical.
    const Role order[] = {Role::Recv, Role::Decode, Role::Mapper, Role::Assembler, Role::Sideband};
    char line[512];
    int n = snprintf(line, sizeof(line), "%zu P-core(s) of efficiency class %u in LLC %u:", cores.size(), maxEff,
                     best->first);
    for (size_t i = 0; i < roles && !cores.empty(); ++i) {
        const auto& c = cores[i % cores.size()];
        plan.cpuSet[(int)order[i]] = c.second;
        n += snprintf(line + n, sizeof(line) - n, " %s->core %d", roleName(order[i]), c.first & 0xFF);
    }
    if (cores.size() < roles) snprintf(line + n, sizeof(line) - n, " (fewer cores than threads: some share)");
    plan.report = line;
    return plan;
}

void applyToCurrentThread(Role role, bool spins, const Options& o, const CorePlan& plan) {
    if (ULONG id = plan.cpuSet[(int)role]) SetThreadSelectedCpuSets(GetCurrentThread(), &id, 1);
    if (o.mmcss && !spins) {
        DWORD idx = 0;
        HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
        if (h) {
            AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL);
            return;
        }
        logWarn("MMCSS registration failed for %s thread (error %lu); using plain priority", roleName(role), GetLastError());
    }
    SetThreadPriority(GetCurrentThread(), spins ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_TIME_CRITICAL);
}

bool haveLockMemoryPrivilege() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, SE_LOCK_MEMORY_NAME, &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok);
    return ok;
}

std::string setupProcess(const Options& o) {
    std::string r;
    const DWORD cls = o.realtimeClass ? REALTIME_PRIORITY_CLASS : HIGH_PRIORITY_CLASS;
    if (SetPriorityClass(GetCurrentProcess(), cls)) {
        // Without SeIncreaseBasePriorityPrivilege, REALTIME silently becomes HIGH.
        r += GetPriorityClass(GetCurrentProcess()) == REALTIME_PRIORITY_CLASS ? "REALTIME priority class" : "HIGH priority class";
        if (o.realtimeClass && GetPriorityClass(GetCurrentProcess()) != REALTIME_PRIORITY_CLASS)
            r += " (REALTIME needs an elevated prompt)";
    }
    // GPU scheduling: our copies and the keepalive ahead of other processes' GPU work.
    const int gpu = setGpuPriority(GetCurrentProcess(), o.realtimeClass);
    r += gpu == 5 ? ", GPU priority REALTIME" : gpu == 4 ? ", GPU priority HIGH" : ", GPU priority unchanged";
    // Opt out of EcoQoS / power throttling for this process.
    PROCESS_POWER_THROTTLING_STATE pt{};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    pt.StateMask = 0;
    if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt))) r += ", power throttling off";
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    r += ", system/display kept awake";
    // Just enough minimum working set for VirtualLock of the 16 MB arena; both limits soft.
    // CUDA pins the frame pages itself (driver MDLs), outside this quota.
    if (SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)48 << 20, (SIZE_T)256 << 20,
                                   QUOTA_LIMITS_HARDWS_MIN_DISABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE))
        r += ", working set min 48 MB (soft)";
    else
        r += ", working-set raise failed (" + std::to_string(GetLastError()) + ")";
    r += haveLockMemoryPrivilege() ? ", SeLockMemoryPrivilege available" : ", no SeLockMemoryPrivilege (large pages unavailable)";
    return r;
}

std::string powerAdvisory() {
    GUID* active = nullptr;
    std::string r;
    if (PowerGetActiveScheme(nullptr, &active) == ERROR_SUCCESS && active) {
        static const GUID kHigh = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
        static const GUID kUltimate = {0xe9a42b02, 0xd5df, 0x448d, {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};
        wchar_t name[256] = {};
        DWORD sz = sizeof(name);
        PowerReadFriendlyName(nullptr, active, nullptr, nullptr, (UCHAR*)name, &sz);
        const bool good = IsEqualGUID(*active, kHigh) || IsEqualGUID(*active, kUltimate);
        r = "power plan: " + narrow(name) +
            (good ? "" : " (camerafy switches to High performance while it runs unless --no-power-boost)");
        LocalFree(active);
    }
    r += "\n  GPU clocks: held up by camerafy's own keepalive kernel (no driver setting needed)";
    return r;
}

void memCheckpoint(const char* label) {
    static const bool on = GetEnvironmentVariableW(L"CAMERAFY_MEMTRACE", nullptr, 0) > 0;
    if (!on) return;
    struct {  // PROCESS_MEMORY_COUNTERS_EX2 (Windows 10 1809+), hidden by the SDK target version
        DWORD cb, PageFaultCount;
        SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
            QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage,
            PrivateWorkingSetSize;
        ULONG64 SharedCommitUsage;
    } m{};
    m.cb = sizeof(m);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&m, sizeof(m))) return;
    logInfo("[mem] %-34s private WS %7.1f MB  WS %7.1f MB  commit %7.1f MB", label, m.PrivateWorkingSetSize / 1048576.0,
            m.WorkingSetSize / 1048576.0, m.PrivateUsage / 1048576.0);
}

namespace {
DWORD amdsListenerPid() {
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<uint8_t> buf(size + 4096);
    size = (ULONG)buf.size();
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return 0;
    const auto* t = (const MIB_TCPTABLE_OWNER_PID*)buf.data();
    for (DWORD i = 0; i < t->dwNumEntries; ++i) {
        const auto& r = t->table[i];
        if (ntohs((u_short)r.dwLocalPort) == 27015 && r.dwLocalAddr == htonl(INADDR_LOOPBACK)) return r.dwOwningPid;
    }
    return 0;
}

bool setThrottling(HANDLE h, bool off) {
    PROCESS_POWER_THROTTLING_STATE pt{};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    // off: explicitly opt out of EcoQoS and timer coalescing; !off: hand control back to Windows.
    pt.ControlMask = off ? (PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION) : 0;
    pt.StateMask = 0;
    return SetProcessInformation(h, ProcessPowerThrottling, &pt, sizeof(pt)) != 0;
}
}  // namespace

// D3DKMT_SCHEDULINGPRIORITYCLASS: IDLE 0, BELOW_NORMAL 1, NORMAL 2, ABOVE_NORMAL 3, HIGH 4,
// REALTIME 5 (REALTIME needs SeIncreaseBasePriorityPrivilege, i.e. elevation).
typedef LONG(APIENTRY* PFN_SetGpuPri)(HANDLE, int);
typedef LONG(APIENTRY* PFN_GetGpuPri)(HANDLE, int*);

namespace {
PFN_SetGpuPri gpuSet() {
    static PFN_SetGpuPri f = (PFN_SetGpuPri)GetProcAddress(LoadLibraryW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass");
    return f;
}
PFN_GetGpuPri gpuGet() {
    static PFN_GetGpuPri f = (PFN_GetGpuPri)GetProcAddress(LoadLibraryW(L"gdi32.dll"), "D3DKMTGetProcessSchedulingPriorityClass");
    return f;
}
const char* gpuClassName(int c) {
    static const char* n[] = {"IDLE", "BELOW_NORMAL", "NORMAL", "ABOVE_NORMAL", "HIGH", "REALTIME"};
    return c >= 0 && c <= 5 ? n[c] : "unchanged";
}

DWORD servicePid(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 0;
    DWORD pid = 0;
    if (SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS_PROCESS st{};
        DWORD need = 0;
        if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (BYTE*)&st, sizeof(st), &need) &&
            st.dwCurrentState == SERVICE_RUNNING)
            pid = st.dwProcessId;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return pid;
}
}  // namespace

int setGpuPriority(HANDLE process, bool realtime) {
    PFN_SetGpuPri f = gpuSet();
    if (!f) return -1;
    if (realtime && f(process, 5) >= 0) return 5;
    if (f(process, 4) >= 0) return 4;
    return -1;
}

std::string Boosts::boost(Target& t, DWORD pid, const char* label) {
    if (!pid) {
        if (t.h) {
            restore(t);
            return std::string(label) + ": process gone; boost dropped";
        }
        return "";
    }
    if (pid == t.pid && (t.h || t.denied)) return "";  // already boosted, or already reported
    restore(t);
    t.h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_SET_LIMITED_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION |
                          PROCESS_QUERY_INFORMATION,
                      FALSE, pid);
    if (!t.h) t.h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!t.h) {
        t.pid = pid;  // report once per process instance
        t.denied = true;
        return std::string(label) + " (pid " + std::to_string(pid) + "): cannot open (error " +
               std::to_string(GetLastError()) + "; run camerafy elevated to boost it)";
    }
    t.pid = pid;
    t.oldClass = GetPriorityClass(t.h);
    if (PFN_GetGpuPri g = gpuGet()) {
        int c = -1;
        if (g(t.h, &c) >= 0) t.oldGpu = c;
    }
    // A run that was killed before it could restore leaves REALTIME behind: never "restore" to it.
    if (t.oldClass == REALTIME_PRIORITY_CLASS) t.oldClass = NORMAL_PRIORITY_CLASS;
    if (t.oldGpu == 5) t.oldGpu = 2;
    // HIGH by default. REALTIME froze the whole PC: AMDS is busy for every USB packet while
    // the phone streams, and at REALTIME it starved input, Explorer and DWM.
    if (realtime) SetPriorityClass(t.h, REALTIME_PRIORITY_CLASS);  // silently HIGH without the privilege
    if (GetPriorityClass(t.h) != REALTIME_PRIORITY_CLASS) SetPriorityClass(t.h, HIGH_PRIORITY_CLASS);
    const DWORD cls = GetPriorityClass(t.h);
    const bool eco = setThrottling(t.h, true);
    const int gpu = setGpuPriority(t.h, realtime);
    char b[256];
    snprintf(b, sizeof(b), "%s (pid %lu): priority %s, efficiency mode %s, GPU priority %s", label, pid,
             cls == REALTIME_PRIORITY_CLASS ? "REALTIME" : cls == HIGH_PRIORITY_CLASS ? "HIGH" : "unchanged (denied)",
             eco ? "off" : "unchanged (denied)", gpuClassName(gpu));
    return b;
}

void Boosts::restore(Target& t) {
    if (t.h) {
        if (t.oldClass) SetPriorityClass(t.h, t.oldClass);
        setThrottling(t.h, false);
        if (t.oldGpu >= 0 && gpuSet()) gpuSet()(t.h, t.oldGpu);
        CloseHandle(t.h);
    }
    t = Target{};
}

std::vector<std::string> Boosts::refresh(bool amds, bool frameServer) {
    std::vector<std::string> out;
    std::string r;
    if (amds && !(r = boost(amds_, amdsListenerPid(), "Apple Mobile Device Service")).empty()) out.push_back(r);
    if (frameServer && !(r = boost(fs_, servicePid(L"FrameServer"), "Windows Camera Frame Server")).empty())
        out.push_back(r);
    return out;
}

void Boosts::restoreAll() {
    restore(amds_);
    restore(fs_);
}

// The "power mode" slider (Windows 10/11, also on Modern Standby laptops that only expose
// Balanced) is an overlay scheme set through these powrprof exports.
typedef DWORD(WINAPI* PFN_GetOverlay)(GUID*);
typedef DWORD(WINAPI* PFN_SetOverlay)(const GUID*);

std::string PowerPlanBoost::apply() {
    static const GUID kHigh = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
    static const GUID kUltimate = {0xe9a42b02, 0xd5df, 0x448d, {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};
    static const GUID kBestPerf = {0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}};
    std::string r;
    GUID* active = nullptr;
    if (PowerGetActiveScheme(nullptr, &active) == ERROR_SUCCESS && active) {
        oldScheme_ = *active;
        LocalFree(active);
        if (!IsEqualGUID(oldScheme_, kUltimate) && !IsEqualGUID(oldScheme_, kHigh)) {
            for (const GUID* g : {&kUltimate, &kHigh}) {
                if (PowerSetActiveScheme(nullptr, g) == ERROR_SUCCESS) {
                    changedScheme_ = true;
                    r = IsEqualGUID(*g, kUltimate) ? "power plan: Ultimate Performance" : "power plan: High performance";
                    break;
                }
            }
        } else {
            r = "power plan: already High/Ultimate";
        }
    }
    HMODULE pp = LoadLibraryW(L"powrprof.dll");
    auto getO = (PFN_GetOverlay)GetProcAddress(pp, "PowerGetEffectiveOverlayScheme");
    auto setO = (PFN_SetOverlay)GetProcAddress(pp, "PowerSetActiveOverlayScheme");
    if (getO && setO && getO(&oldOverlay_) == ERROR_SUCCESS && !IsEqualGUID(oldOverlay_, kBestPerf) &&
        setO(&kBestPerf) == ERROR_SUCCESS) {
        changedOverlay_ = true;
        r += r.empty() ? "power mode: Best performance" : ", power mode: Best performance";
    }
    if (r.empty()) r = "power plan: unchanged (no High/Ultimate plan, power mode overlay unavailable)";
    return r + " (restored on exit)";
}

void PowerPlanBoost::restore() {
    if (changedOverlay_) {
        auto setO = (PFN_SetOverlay)GetProcAddress(LoadLibraryW(L"powrprof.dll"), "PowerSetActiveOverlayScheme");
        if (setO) setO(&oldOverlay_);
        changedOverlay_ = false;
    }
    if (changedScheme_) {
        PowerSetActiveScheme(nullptr, &oldScheme_);
        changedScheme_ = false;
    }
}

}  // namespace cf::tuning
