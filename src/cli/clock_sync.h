#pragma once
// NTP-style clock correlation between the phone and Windows over the sideband.
//   t1 = Windows send (µs), t2 = phone receive, t3 = phone send, t4 = Windows receive.
//   offset (phone - windows) = ((t2 - t1) + (t3 - t4)) / 2,  rtt = (t4 - t1) - (t3 - t2)
// The minimum-RTT sample in a sliding window is the best estimate; its error is bounded
// by rtt/2, which is what gets reported next to any absolute figure.
#include <stdint.h>

namespace cf {

struct ClockSample {
    int64_t offsetUs;
    int64_t rttUs;
};

inline ClockSample ntpSample(int64_t t1, int64_t t2, int64_t t3, int64_t t4) {
    return {((t2 - t1) + (t3 - t4)) / 2, (t4 - t1) - (t3 - t2)};
}

class MinRttFilter {
public:
    static constexpr int kWindow = 32;  // a few seconds at the sideband's ping rate
    void add(const ClockSample& s) {
        buf_[n_++ % kWindow] = s;
    }
    bool best(ClockSample& out) const {
        const int count = n_ < kWindow ? n_ : kWindow;
        if (!count) return false;
        out = buf_[0];
        for (int i = 1; i < count; ++i)
            if (buf_[i].rttUs < out.rttUs) out = buf_[i];
        return true;
    }
    int count() const { return n_; }

private:
    ClockSample buf_[kWindow] = {};
    int n_ = 0;
};

}  // namespace cf
