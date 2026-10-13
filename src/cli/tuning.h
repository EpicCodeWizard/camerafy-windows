#pragma once
// Windows scheduling/memory/power tuning (PRD §12). Each knob is switchable so --bench can
// measure it; nothing here is system-wide.
#include <windows.h>
#include <string>
#include <vector>
#include "options.h"

namespace cf::tuning {

enum class Role { Recv = 0, Assembler, Decode, Mapper, Sideband, Count };
const char* roleName(Role r);

struct CorePlan {
    ULONG cpuSet[(int)Role::Count] = {};  // 0 = unpinned
    std::string report;
};

// Picks distinct physical P-cores sharing one last-level cache, avoiding the core that
// owns logical processor 0 when there are enough cores.
CorePlan planCores(const Options& o);

// Applies pinning and scheduling class to the calling thread. Blocking threads get MMCSS
// "Pro Audio" at critical priority; spinning threads get plain high priority, because MMCSS
// demotes threads that use up their period.
void applyToCurrentThread(Role role, bool spins, const Options& o, const CorePlan& plan);

// Priority class, power throttling opt-out, execution state, working-set size.
std::string setupProcess(const Options& o);

// Advice only; changes nothing system-wide.
std::string powerAdvisory();

// With CAMERAFY_MEMTRACE set: logs private working set / commit at a startup checkpoint.
void memCheckpoint(const char* label);

bool haveLockMemoryPrivilege();

// Every process in the camera path runs at the top of the scheduler while camerafy runs
// (latency first, ahead of everything else):
//   - Apple Mobile Device Service (AppleMobileDeviceProcess): relays every byte from USB.
//   - the Windows Camera Frame Server (svchost -k Camera -s FrameServer): hosts the MF camera.
// Each gets the HIGH priority class, power throttling (EcoQoS) off and GPU priority HIGH
// (REALTIME for both only with --realtime-unsafe: it froze the whole PC while streaming). refresh() re-applies when a process
// restarts; restoreAll() (also the destructor) puts the previous values back.
class Boosts {
public:
    ~Boosts() { restoreAll(); }
    // Returns one log line per process whose boost changed.
    std::vector<std::string> refresh(bool amds, bool frameServer);
    void restoreAll();
    bool realtime = false;  // --realtime-unsafe: REALTIME class + GPU priority (froze the PC)

private:
    struct Target {
        DWORD pid = 0;
        HANDLE h = nullptr;
        DWORD oldClass = 0;
        int oldGpu = -1;
        bool denied = false;
    };
    std::string boost(Target& t, DWORD pid, const char* label);
    static void restore(Target& t);
    Target amds_, fs_;
};

// High performance / Ultimate power plan (or the "best performance" power mode overlay) while
// camerafy runs; the previous plan and overlay come back in restore().
class PowerPlanBoost {
public:
    ~PowerPlanBoost() { restore(); }
    std::string apply();
    void restore();

private:
    bool changedScheme_ = false, changedOverlay_ = false;
    GUID oldScheme_{}, oldOverlay_{};
};

// GPU scheduling priority class for a process (D3DKMT, gdi32). Returns the class set, or -1.
int setGpuPriority(HANDLE process, bool realtime);

}  // namespace cf::tuning
