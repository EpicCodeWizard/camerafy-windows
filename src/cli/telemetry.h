#pragma once
// Reporter thread: drains per-frame timing records, consumer logs and hot-path events;
// computes rolling percentiles; prints a line per second; exports CSV; raises the
// regression alarm. Runs entirely off the hot path (≤10 Hz sampling).
#include <atomic>
#include <string>
#include <vector>
#include <stdio.h>
#include "frame_types.h"
#include "publisher.h"
#include "assembler.h"
#include "decoder.h"
#include "options.h"

namespace cf {

// Phone-side timestamps for one frame: the sideband's FrameTiming,
// phone mach_continuous_time in µs.
struct PhoneTiming {
    uint64_t frameId;
    uint64_t capturePtsUs;
    uint64_t captureCallbackUs;
    uint64_t encodeSubmitUs;
    uint64_t encodeOutputUs;
    uint64_t encodeCompleteUs;
    uint64_t firstByteSendUs;
    uint64_t lastByteSendUs;
    uint32_t frameBytes;
    uint8_t isIdr;
    uint8_t sliceCount;
    uint16_t pad;
};
using PhoneTimingRing = SpscRing<PhoneTiming, 1024>;

// Clock correlation published by the sideband thread: phoneUs - windowsUs.
struct ClockEstimate {
    std::atomic<int64_t> offsetUs{0};
    std::atomic<int64_t> rttUs{-1};  // -1 = no estimate yet
    std::atomic<uint32_t> samples{0};
};

struct StageSummary {
    std::string name;
    double mean, p50, p95, p99, p999, max;
    size_t count;
    double baselineP95;
};

class Telemetry final {
public:
    static constexpr uint32_t kWarmupFrames = 120;

    void init(const Options& o, TimingRing* timing, Publisher* pub, const Assembler* asmb, const Decoder* dec,
              PhoneTimingRing* phone, const ClockEstimate* clock);
    void start();
    void stop();
    // Full table (for --stats and the end of --self-test runs).
    void printFullTable();
    std::vector<StageSummary> summaries();
    uint64_t framesSeen() const { return frames_.load(); }
    // Fallback cost (µs, p50 of "boundary wait").
    double boundaryWaitP50();

private:
    static DWORD WINAPI threadProc(void* self);
    void loop();
    void drain();
    void report(double elapsedS);
    void drainConsumers(Region& r, int stage, uint32_t* lastHead);
    void account(const FrameTiming& t, bool havePhone, const PhoneTiming& ph);

    struct Window {
        std::vector<double> all;  // every post-warmup sample (for --stats)
        double baselineP95 = -1;
        bool alarmed = false;
        void add(double v) { all.push_back(v); }
    };

    Options opt_{};
    TimingRing* timing_ = nullptr;
    Publisher* pub_ = nullptr;
    const Assembler* asm_ = nullptr;
    const Decoder* dec_ = nullptr;
    PhoneTimingRing* phone_ = nullptr;
    const ClockEstimate* clock_ = nullptr;

    uint32_t seenSpsVersion_ = 0;
    void logSps();

    HANDLE thread_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> frames_{0};
    uint64_t postWarmup_ = 0;
    uint64_t lastSecondFrames_ = 0;
    FILE* csv_ = nullptr;
    bool fallbackWarned_ = false;
    uint32_t dsHead_[8] = {}, mfHead_[8] = {};

    static constexpr int kPhoneHist = 4096;
    PhoneTiming phoneHist_[kPhoneHist] = {};
    // Frame records waiting (up to 500 ms) for their FrameTiming, which travels on another
    // TCP connection and may arrive after the frame itself.
    std::vector<FrameTiming> pending_;
    uint64_t phoneJoined_ = 0, phoneMissing_ = 0;

    std::vector<std::string> names_;
    std::vector<Window> win_;
    SRWLOCK lock_ = SRWLOCK_INIT;
};

}  // namespace cf
