#pragma once
// Console output (off the hot path) and the hot-path event log.
#include <windows.h>
#include <atomic>
#include <stdint.h>
#include <string>
#include "spsc_ring.h"

namespace cf {

extern bool g_verbose;
extern std::atomic<bool> g_quit;  // set by Ctrl+C

void installCtrlHandler();
void logInfo(const char* fmt, ...);
void logWarn(const char* fmt, ...);
void logError(const char* fmt, ...);
void logVerbose(const char* fmt, ...);
// A prominent multi-line banner (used for latency-costing fallbacks).
void logBanner(const char* title, const char* body);

std::string narrow(const std::wstring& w);
std::wstring widen(const std::string& s);
std::string hrString(HRESULT hr);

// Hot-path events: fixed-size records pushed into per-thread SPSC rings and drained by the
// reporter thread. No formatting, no allocation, no syscalls on the producing side.
enum class Ev : uint16_t {
    ArenaOverflow = 1,
    AuRingFull,
    PicRingFull,
    TelemetryRingFull,
    ParseError,
    DecodeError,
    MapError,
    CopyError,
    DroppedBeforeIdr,
    StreamResync,
    AuTooLarge,
    RecvError,
    SequenceChange,
};

struct HotEvent {
    uint64_t qpc;
    Ev code;
    uint16_t thread;
    uint32_t a;
    uint64_t b;
};

using EventRing = SpscRing<HotEvent, 1024>;

enum EventSource : uint16_t { kEvRecv = 0, kEvAssembler, kEvDecode, kEvMapper, kEvSideband, kEvSourceCount };
extern EventRing g_events[kEvSourceCount];

inline void hotEvent(EventSource src, Ev code, uint32_t a = 0, uint64_t b = 0) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    g_events[src].push(HotEvent{(uint64_t)t.QuadPart, code, (uint16_t)src, a, b});
}

const char* evName(Ev e);

}  // namespace cf
