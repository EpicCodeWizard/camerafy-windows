#pragma once
// QueryPerformanceCounter helpers. Raw ticks everywhere; convert only when reporting.
#include <windows.h>
#include <stdint.h>
#include <intrin.h>

namespace cf {

inline uint64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

// Initialized once at module load (dynamic init of an inline variable), so reads are a plain
// load: a function-local static would cost a thread-safe-init guard check on every call,
// which the disassembly spot-check found in the mapper's per-frame path.
inline const uint64_t g_qpcFreq = [] {
    LARGE_INTEGER x;
    QueryPerformanceFrequency(&x);
    return (uint64_t)x.QuadPart;
}();

inline uint64_t qpcFreq() { return g_qpcFreq; }

inline double ticksToUs(int64_t ticks) { return (double)ticks * 1e6 / (double)qpcFreq(); }
inline int64_t usToTicks(double us) { return (int64_t)(us * (double)qpcFreq() / 1e6); }

// Spin until qpc() >= deadline. Used for bounded spin windows only.
inline void spinUntil(uint64_t deadline) {
    while (qpc() < deadline) _mm_pause();
}

}  // namespace cf
