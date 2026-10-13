#pragma once
// Reader-side helpers shared by the DirectShow filter and the MF media source.
#include "ipc.h"
#include "qpc.h"
#include <immintrin.h>
#pragma comment(lib, "synchronization.lib")  // WaitOnAddress
#include <avrt.h>
#pragma comment(lib, "avrt.lib")

namespace cf::ipc {
// Every thread that moves camera frames (the DirectShow streaming thread, the MF delivery
// thread, copy helpers) runs in MMCSS "Pro Audio" at critical priority: the real-time band,
// above every normal-priority thread of any process, as the built-in webcam's Frame Server
// path is. These threads sleep between frames and use a fraction of their MMCSS budget, so
// MMCSS never demotes them. Falls back to TIME_CRITICAL if MMCSS is unavailable.
inline void raiseCameraThread() {
    DWORD task = 0;
    if (HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task)) {
        AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL);
        return;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}
}  // namespace cf::ipc

namespace cf::ipc {

enum class WaitResult { Frame, Timeout, Stopped };

// Waits for publishCount to move past `lastSeen` without burning a core between frames.
//
// The writer signals each reader's auto-reset event twice per frame:
//   1. wake-ahead, when it queues the frame's DMA (a few hundred us before the publish), so the
//      reader is already running when the frame lands; it then spins for at most
//      readerSpinUs. `onWake` runs at that point (it wakes the copy helpers the same way).
//   2. after the publish, only if this reader is marked sleeping (the wake-ahead was missed).
// Between frames the reader is blocked in the kernel. Cost: ~0.3 ms of spinning per frame
// instead of a whole core. The phone's cadence drives pacing; there is no local frame clock.
template <class OnWake>
inline WaitResult waitForFrame(RegionHeader* h, int readerIdx, uint64_t lastSeen, HANDLE evt, DWORD timeoutMs,
                               OnWake&& onWake, const volatile bool* stop = nullptr) {
    auto published = [&] { return h->publishCount.load(std::memory_order_seq_cst) != lastSeen; };
    if (published()) return WaitResult::Frame;
    std::atomic<uint32_t>* sleeping = readerIdx >= 0 ? &h->readers[readerIdx].sleeping : nullptr;
    const uint32_t spinUs = h->readerSpinUs.load(std::memory_order_relaxed);
    const uint64_t endQpc = cf::qpc() + (uint64_t)cf::usToTicks(timeoutMs * 1000.0);
    while (true) {
        if (stop && *stop) return WaitResult::Stopped;
        const uint64_t now = cf::qpc();
        if (now >= endQpc) return WaitResult::Timeout;
        const DWORD ms = (DWORD)((endQpc - now) * 1000 / cf::qpcFreq()) + 1;
        // Dekker pairing with the writer: mark sleeping, then re-check; the writer publishes,
        // then checks the mark. One of the two always sees the other.
        if (sleeping) sleeping->store(1, std::memory_order_seq_cst);
        if (published()) {
            if (sleeping) sleeping->store(0, std::memory_order_relaxed);
            return WaitResult::Frame;
        }
        const DWORD w = WaitForSingleObject(evt, ms < 50 ? ms : 50);
        if (sleeping) sleeping->store(0, std::memory_order_relaxed);
        if (published()) return WaitResult::Frame;
        if (w != WAIT_OBJECT_0 || !spinUs) continue;
        // Wake-ahead: the frame's DMA is in flight.
        onWake();
        const uint64_t deadline = cf::qpc() + (uint64_t)cf::usToTicks(spinUs);
        uint32_t n = 0;
        while (true) {
            if (published()) return WaitResult::Frame;
            if ((++n & 255) == 0) {
                if (stop && *stop) return WaitResult::Stopped;
                if (cf::qpc() > deadline) break;
            }
            _mm_pause();
        }
    }
}

inline WaitResult waitForFrame(RegionHeader* h, int readerIdx, uint64_t lastSeen, HANDLE evt, DWORD timeoutMs) {
    return waitForFrame(h, readerIdx, lastSeen, evt, timeoutMs, [] {});
}

inline void heartbeat(RegionHeader* h, int readerIdx) {
    if (readerIdx >= 0) h->readers[readerIdx].heartbeatQpc.store(cf::qpc(), std::memory_order_relaxed);
}

inline void recordConsumed(RegionHeader* h, int readerIdx, const FrameInfo& f, uint64_t retries) {
    if (readerIdx < 0) return;
    auto& r = h->readers[readerIdx];
    const uint64_t now = cf::qpc();
    r.lastFrameId.store(f.frameId, std::memory_order_relaxed);
    r.lastPublishedQpc.store(f.winPublishedQpc, std::memory_order_relaxed);
    r.lastConsumedQpc.store(now, std::memory_order_relaxed);
    r.heartbeatQpc.store(now, std::memory_order_relaxed);
    r.retries.store(retries, std::memory_order_relaxed);
    r.consumedCount.fetch_add(1, std::memory_order_release);
    auto& log = h->consume[readerIdx];
    const uint32_t i = log.head.load(std::memory_order_relaxed);
    log.e[i & (kConsumeLogSize - 1)] = {f.winPublishedQpc, now};
    log.head.store(i + 1, std::memory_order_release);
}

// Pitch-linear NV12 slot -> NV12 with the caller's stride, for luma rows [r0, r1) (even
// bounds) and the matching chroma rows. Rows past the slot's height are filled black when the
// destination is taller (format mismatch during a change).
inline void copyNv12Rows(const uint8_t* src, const SlotHeader& s, uint8_t* dstY, uint32_t dstStrideY, uint8_t* dstUV,
                         uint32_t dstStrideUV, uint32_t dstW, uint32_t dstH, uint32_t r0, uint32_t r1) {
    const uint32_t w = s.width < dstW ? s.width : dstW;
    const uint32_t hgt = s.height < dstH ? s.height : dstH;
    const uint8_t* sy = src;
    const uint8_t* suv = src + s.offsetUV;
    const bool same = s.strideY == dstStrideY && s.strideUV == dstStrideUV && w == dstW && hgt == dstH;
    const uint32_t yEnd = r1 < hgt ? r1 : hgt;
    if (same) {
        if (yEnd > r0) memcpy(dstY + (size_t)r0 * dstStrideY, sy + (size_t)r0 * s.strideY, (size_t)dstStrideY * (yEnd - r0));
    } else {
        for (uint32_t y = r0; y < yEnd; ++y) memcpy(dstY + (size_t)y * dstStrideY, sy + (size_t)y * s.strideY, w);
    }
    for (uint32_t y = (yEnd > r0 ? yEnd : r0); y < r1; ++y) memset(dstY + (size_t)y * dstStrideY, 16, dstW);
    const uint32_t c0 = r0 / 2, c1 = r1 / 2, cEnd = (yEnd / 2) > c0 ? yEnd / 2 : c0;
    if (same) {
        if (cEnd > c0) memcpy(dstUV + (size_t)c0 * dstStrideUV, suv + (size_t)c0 * s.strideUV, (size_t)dstStrideUV * (cEnd - c0));
    } else {
        for (uint32_t y = c0; y < cEnd; ++y) memcpy(dstUV + (size_t)y * dstStrideUV, suv + (size_t)y * s.strideUV, w);
    }
    for (uint32_t y = cEnd; y < c1; ++y) memset(dstUV + (size_t)y * dstStrideUV, 128, dstW);
}

inline void copyNv12(const uint8_t* src, const SlotHeader& s, uint8_t* dstY, uint32_t dstStrideY, uint8_t* dstUV,
                     uint32_t dstStrideUV, uint32_t dstW, uint32_t dstH) {
    copyNv12Rows(src, s, dstY, dstStrideY, dstUV, dstStrideUV, dstW, dstH, 0, dstH);
}

// NV12 -> YUY2 (packed Y0 U Y1 V), AVX2, chroma line-doubled, rows [r0, r1). dst stride in bytes.
inline void nv12ToYuy2Rows(const uint8_t* src, const SlotHeader& s, uint8_t* dst, uint32_t dstStride, uint32_t dstW,
                           uint32_t dstH, uint32_t r0, uint32_t r1) {
    const uint32_t w = (s.width < dstW ? s.width : dstW) & ~1u;
    const uint32_t hgt = s.height < dstH ? s.height : dstH;
    const uint8_t* suv = src + s.offsetUV;
    for (uint32_t y = r0; y < r1; ++y) {
        uint8_t* d = dst + (size_t)y * dstStride;
        uint32_t x = 0;
        if (y < hgt) {
            const uint8_t* yr = src + (size_t)y * s.strideY;
            const uint8_t* uvr = suv + (size_t)(y / 2) * s.strideUV;
            for (; x + 32 <= w; x += 32) {
                const __m256i yy = _mm256_loadu_si256((const __m256i*)(yr + x));
                const __m256i uv = _mm256_loadu_si256((const __m256i*)(uvr + x));
                // unpack within 128-bit lanes, then fix lane order with permute
                const __m256i lo = _mm256_unpacklo_epi8(yy, uv);
                const __m256i hi = _mm256_unpackhi_epi8(yy, uv);
                _mm256_storeu_si256((__m256i*)(d + x * 2), _mm256_permute2x128_si256(lo, hi, 0x20));
                _mm256_storeu_si256((__m256i*)(d + x * 2 + 32), _mm256_permute2x128_si256(lo, hi, 0x31));
            }
            for (; x < w; x += 2) {
                d[x * 2 + 0] = yr[x];
                d[x * 2 + 1] = uvr[x];
                d[x * 2 + 2] = yr[x + 1];
                d[x * 2 + 3] = uvr[x + 1];
            }
        }
        for (; x < dstW; x += 2) {
            d[x * 2 + 0] = 16; d[x * 2 + 1] = 128; d[x * 2 + 2] = 16; d[x * 2 + 3] = 128;
        }
    }
}

inline void nv12ToYuy2(const uint8_t* src, const SlotHeader& s, uint8_t* dst, uint32_t dstStride, uint32_t dstW,
                       uint32_t dstH) {
    nv12ToYuy2Rows(src, s, dst, dstStride, dstW, dstH, 0, dstH);
}

// Splits a frame copy across the caller and N-1 helper threads. The source was just written by
// a DMA, so it is cold in DRAM; one core cannot saturate the memory system, several can
// (measured: 230 us on 1 thread vs ~150 us on 4 for 1080p NV12).
//
// Helpers sleep between frames (WaitOnAddress). prepare() wakes them when the reader gets the
// writer's wake-ahead signal, so they are running by the time the frame lands; after a job, or
// a spin window with no job, they go back to sleep. Work is claimed in 64-row chunks, so a
// helper that wakes late only means the caller copies more rows itself: never a stall.
// CAMERAFY_COPY_THREADS overrides the count (1 disables).
class ParallelCopier {
public:
    ~ParallelCopier() { stop(); }

    // threads == 0: automatic, min(6, logical processors / 3). 6 measured best.
    void start(uint32_t threads) {
        stop();
        if (threads == 0) {
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            threads = si.dwNumberOfProcessors / 3;
            if (threads > 6) threads = 6;
        }
        wchar_t b[16];
        const DWORD n = GetEnvironmentVariableW(L"CAMERAFY_COPY_THREADS", b, 16);
        if (n && n < 16) threads = (uint32_t)_wtoi(b);
        if (threads < 1) threads = 1;
        if (threads > kMax) threads = kMax;
        n_ = threads;
        quit_.store(false);
        for (uint32_t i = 1; i < n_; ++i) {
            th_[i] = CreateThread(nullptr, 0, helperProc, this, 0, nullptr);
            if (!th_[i]) { n_ = i; break; }
        }
    }

    void stop() {
        quit_.store(true);
        wake();
        for (uint32_t i = 1; i < kMax; ++i) {
            if (th_[i]) {
                WaitForSingleObject(th_[i], 1000);
                CloseHandle(th_[i]);
                th_[i] = nullptr;
            }
        }
        n_ = 1;
    }

    uint32_t threads() const { return n_; }

    // A frame is about to land: get the helpers running.
    void prepare() {
        if (n_ > 1) wake();
    }

    void copyNv12(const uint8_t* src, const SlotHeader& s, uint8_t* dstY, uint32_t dstStrideY, uint8_t* dstUV,
                  uint32_t dstStrideUV, uint32_t dstW, uint32_t dstH) {
        Job j{false, src, &s, dstY, dstStrideY, dstUV, dstStrideUV, dstW, dstH};
        run(j);
    }
    void nv12ToYuy2(const uint8_t* src, const SlotHeader& s, uint8_t* dst, uint32_t dstStride, uint32_t dstW,
                    uint32_t dstH) {
        Job j{true, src, &s, dst, dstStride, nullptr, 0, dstW, dstH};
        run(j);
    }

private:
    static constexpr uint32_t kMax = 8;
    static constexpr uint32_t kChunkRows = 64;     // even, so chroma rows split cleanly
    static constexpr uint32_t kHelperSpinUs = 3000; // awake window after a wake with no job yet
    struct Job {
        bool yuy2;
        const uint8_t* src;
        const SlotHeader* s;
        uint8_t* d0;
        uint32_t stride0;
        uint8_t* d1;
        uint32_t stride1;
        uint32_t w, h;
    };

    void rows(const Job& j, uint32_t r0, uint32_t r1) const {
        if (j.yuy2) nv12ToYuy2Rows(j.src, *j.s, j.d0, j.stride0, j.w, j.h, r0, r1);
        else copyNv12Rows(j.src, *j.s, j.d0, j.stride0, j.d1, j.stride1, j.w, j.h, r0, r1);
    }

    // Claims and copies chunks of job generation g until none are left. claim_ packs
    // (generation << 32 | next chunk); a claimed chunk is always finished before run() can
    // return, so job_ stays valid for whoever claimed it.
    void work(uint32_t g) {
        while (true) {
            uint64_t s = claim_.load(std::memory_order_acquire);
            if ((uint32_t)(s >> 32) != g || (uint32_t)s >= total_) return;
            if (!claim_.compare_exchange_weak(s, s + 1, std::memory_order_acq_rel)) continue;
            const uint32_t r0 = (uint32_t)s * kChunkRows;
            const uint32_t r1 = r0 + kChunkRows < job_.h ? r0 + kChunkRows : job_.h;
            rows(job_, r0, r1);
            done_.fetch_add(1, std::memory_order_release);
        }
    }

    void run(const Job& j) {
        if (n_ == 1) {
            rows(j, 0, j.h);
            return;
        }
        job_ = j;
        total_ = (j.h + kChunkRows - 1) / kChunkRows;
        done_.store(0, std::memory_order_relaxed);
        const uint32_t g = ++gen_;
        claim_.store((uint64_t)g << 32, std::memory_order_release);
        wake();
        work(g);
        while (done_.load(std::memory_order_acquire) < total_) _mm_pause();
    }

    void wake() {
        wakeSeq_.fetch_add(1, std::memory_order_release);
        WakeByAddressAll(&wakeSeq_);
    }

    static DWORD WINAPI helperProc(void* p) {
        raiseCameraThread();
        static_cast<ParallelCopier*>(p)->helper();
        return 0;
    }

    void helper() {
        uint32_t lastG = (uint32_t)(claim_.load(std::memory_order_acquire) >> 32);
        while (!quit_.load(std::memory_order_relaxed)) {
            // Awake: wait briefly for the job this wake announced.
            const uint64_t deadline = cf::qpc() + (uint64_t)cf::usToTicks(kHelperSpinUs);
            uint32_t n = 0;
            while (!quit_.load(std::memory_order_relaxed)) {
                const uint32_t g = (uint32_t)(claim_.load(std::memory_order_acquire) >> 32);
                if (g != lastG) {
                    work(g);
                    lastG = g;
                    break;  // the next frame is ~16 ms away: sleep
                }
                if ((++n & 255) == 0 && cf::qpc() > deadline) break;
                _mm_pause();
            }
            // Asleep until prepare()/run()/stop().
            const uint64_t w = wakeSeq_.load(std::memory_order_acquire);
            if ((uint32_t)(claim_.load(std::memory_order_acquire) >> 32) != lastG) continue;
            if (quit_.load(std::memory_order_relaxed)) break;
            uint64_t expect = w;
            WaitOnAddress(&wakeSeq_, &expect, sizeof(expect), INFINITE);
        }
    }

    uint32_t n_ = 1;
    Job job_{};
    uint32_t total_ = 0;
    uint32_t gen_ = 0;
    alignas(64) std::atomic<uint64_t> claim_{0};
    alignas(64) std::atomic<uint32_t> done_{0};
    alignas(64) std::atomic<uint64_t> wakeSeq_{0};
    std::atomic<bool> quit_{false};
    HANDLE th_[kMax] = {};
};

// Black NV12 frame, for "no signal" output before the first publish.
inline void fillBlackNv12(uint8_t* y, uint32_t strideY, uint8_t* uv, uint32_t strideUV, uint32_t w, uint32_t h) {
    for (uint32_t r = 0; r < h; ++r) memset(y + (size_t)r * strideY, 16, w);
    for (uint32_t r = 0; r < h / 2; ++r) memset(uv + (size_t)r * strideUV, 128, w);
}

}  // namespace cf::ipc
