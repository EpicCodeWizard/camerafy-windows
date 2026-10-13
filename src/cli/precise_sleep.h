#pragma once
// Sleeping until a QPC deadline with a high-resolution waitable timer (Windows 10 1803+), so a
// thread waiting for a GPU result it can predict (decode ~1.4 ms, DMA ~0.3 ms) is off the CPU
// for most of the wait and polls only for the last stretch.
//
// The timer is not precise: in testing it wakes 300-550 us late (p50) and up to ~1 ms
// late (p99) whatever the thread priority or timeBeginPeriod. So the
// sleeper measures its own lateness and margin() tells the caller how early to aim.
#include <windows.h>
#include <algorithm>
#include "qpc.h"

namespace cf {

class PreciseSleeper {
public:
    PreciseSleeper() {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    ~PreciseSleeper() {
        if (timer_) CloseHandle(timer_);
    }
    PreciseSleeper(const PreciseSleeper&) = delete;
    PreciseSleeper& operator=(const PreciseSleeper&) = delete;

    bool highResolution() const { return timer_ != nullptr; }

    // Blocks until about `deadlineQpc`. Returns at once if the deadline is under `minUs` away:
    // not worth a syscall.
    void sleepUntil(uint64_t deadlineQpc, double minUs = 60.0) {
        const uint64_t now = qpc();
        if (!timer_ || deadlineQpc <= now) return;
        const double us = ticksToUs((int64_t)(deadlineQpc - now));
        if (us < minUs) return;
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(us * 10.0);  // relative, 100 ns units
        if (!SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) return;
        WaitForSingleObject(timer_, INFINITE);
        const int64_t late = (int64_t)(qpc() - deadlineQpc);
        late_[n_++ & 63] = late > 0 ? (float)ticksToUs(late) : 0.0f;
        dirty_ = true;
    }

    // How long before a deadline to stop sleeping and start polling, in us: the 90th
    // percentile of recent wake lateness plus 50 us (500 us until there is data).
    double marginUs() {
        if (n_ < 16) return 500.0;
        if (dirty_) {
            const uint32_t k = n_ < 64 ? n_ : 64;
            float tmp[64];
            std::copy(late_, late_ + k, tmp);
            std::nth_element(tmp, tmp + (k * 9) / 10, tmp + k);
            margin_ = std::clamp((double)tmp[(k * 9) / 10] + 50.0, 100.0, 2000.0);
            dirty_ = false;
        }
        return margin_;
    }

private:
    HANDLE timer_ = nullptr;
    float late_[64] = {};
    uint32_t n_ = 0;
    bool dirty_ = false;
    double margin_ = 500.0;
};

}  // namespace cf
