// --bench: measure, don't reason (PRD §17). Micro-benchmarks for the primitives, then the
// whole pipeline under each plausible alternative, replaying a canned stream. Each variant
// runs in two interleaved rounds so thermal/clock drift cannot favour whichever runs first.
#include "options.h"
#include "console.h"
#include "pipeline.h"
#include "nal_scan.h"
#include "ipc_reader.h"
#include "precise_sleep.h"
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#include "qpc.h"
#include <thread>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <math.h>

namespace cf {

namespace {

std::wstring exeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L'\\'));
}

std::wstring findTestData(const wchar_t* file) {
    std::wstring dir = exeDir();
    for (int i = 0; i < 5; ++i) {
        std::wstring p = dir + L"\\testdata\\" + file;
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
        const size_t k = dir.find_last_of(L'\\');
        if (k == std::wstring::npos) break;
        dir = dir.substr(0, k);
    }
    return {};
}

double median(std::vector<double> v) {
    if (v.empty()) return NAN;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
double pctl(std::vector<double> v, double p) {
    if (v.empty()) return NAN;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(p * (double)v.size()))];
}

void microBenchmarks(const std::wstring& file) {
    logInfo("Micro-benchmarks:");
    // QPC cost
    {
        const int n = 1000000;
        const uint64_t t0 = qpc();
        volatile uint64_t sink = 0;
        for (int i = 0; i < n; ++i) sink += qpc();
        logInfo("  QueryPerformanceCounter: %.1f ns per call (frequency %llu Hz)", ticksToUs((int64_t)(qpc() - t0)) * 1000.0 / n,
                (unsigned long long)qpcFreq());
    }
    // Start-code scanners over the canned stream
    if (!file.empty()) {
        FILE* f = _wfopen(file.c_str(), L"rb");
        std::vector<uint8_t> b(16u << 20);
        const size_t n = f ? fread(b.data(), 1, b.size(), f) : 0;
        if (f) fclose(f);
        for (auto [name, fn] : {std::pair<const char*, FindStartCodeFn>{"scalar", findStartCodeScalar},
                                std::pair<const char*, FindStartCodeFn>{"AVX2", findStartCodeAvx2}}) {
            std::vector<double> runs;
            size_t codes = 0;
            for (int r = 0; r < 5; ++r) {
                const uint64_t t0 = qpc();
                size_t i = 0;
                codes = 0;
                while (true) {
                    const size_t k = fn(b.data(), i, n);
                    if (k >= n) break;
                    ++codes;
                    i = k + 3;
                }
                runs.push_back(ticksToUs((int64_t)(qpc() - t0)));
            }
            const double us = median(runs);
            logInfo("  start-code scan %-6s: %6.2f GB/s (%zu MB, %zu start codes, %.0f us; a 1 MB IDR scans in %.1f us)",
                    name, (double)n / us / 1000.0, n >> 20, codes, us, us * 1048576.0 / (double)n);
        }
    }
    // memcpy of one 1080p NV12 frame (what each consumer does once)
    {
        const size_t sz = 1920 * 1080 * 3 / 2;
        std::vector<uint8_t> a(sz, 1), c(sz, 2);
        std::vector<double> runs;
        for (int r = 0; r < 200; ++r) {
            const uint64_t t0 = qpc();
            memcpy(c.data(), a.data(), sz);
            runs.push_back(ticksToUs((int64_t)(qpc() - t0)));
            a[r] = (uint8_t)r;
        }
        logInfo("  memcpy 1080p NV12 (3.1 MB): p50 %.0f us, p95 %.0f us", median(runs), pctl(runs, 0.95));
    }
    // The consumer's copy out of shared memory: the source was just written by a DMA, so it
    // is cold in DRAM. Compare memcpy, non-temporal AVX2 stores, and 2/4-thread splits.
    {
        const size_t sz = 1920 * 1080 * 3 / 2;
        std::vector<uint8_t> src(sz + 64, 3), dst(sz + 64, 4), evict(64u << 20, 5);
        uint8_t* s = (uint8_t*)(((uintptr_t)src.data() + 63) & ~(uintptr_t)63);
        uint8_t* d = (uint8_t*)(((uintptr_t)dst.data() + 63) & ~(uintptr_t)63);
        auto flush = [&] {
            for (size_t i = 0; i < evict.size(); i += 64) evict[i]++;
            for (size_t i = 0; i < sz; i += 64) _mm_clflushopt(s + i);
            _mm_sfence();
        };
        auto ntCopy = [](uint8_t* dd, const uint8_t* ss, size_t n) {
            size_t i = 0;
            for (; i + 128 <= n; i += 128) {
                _mm_prefetch((const char*)ss + i + 1024, _MM_HINT_NTA);
                const __m256i a = _mm256_load_si256((const __m256i*)(ss + i));
                const __m256i b = _mm256_load_si256((const __m256i*)(ss + i + 32));
                const __m256i c = _mm256_load_si256((const __m256i*)(ss + i + 64));
                const __m256i e = _mm256_load_si256((const __m256i*)(ss + i + 96));
                _mm256_stream_si256((__m256i*)(dd + i), a);
                _mm256_stream_si256((__m256i*)(dd + i + 32), b);
                _mm256_stream_si256((__m256i*)(dd + i + 64), c);
                _mm256_stream_si256((__m256i*)(dd + i + 96), e);
            }
            _mm_sfence();
            memcpy(dd + i, ss + i, n - i);
        };
        // Spinning helper threads, as a consumer would keep them.
        auto parallel = [&](int threads, bool nt) {
            std::atomic<uint64_t> go{0};
            std::atomic<int> done{0};
            std::atomic<bool> stop{false};
            std::vector<std::thread> pool;
            const size_t chunk = ((sz / threads) + 127) & ~(size_t)127;
            for (int t = 1; t < threads; ++t)
                pool.emplace_back([&, t] {
                    uint64_t seen = 0;
                    while (!stop.load()) {
                        const uint64_t g = go.load(std::memory_order_acquire);
                        if (g == seen) { _mm_pause(); continue; }
                        seen = g;
                        const size_t off = chunk * t, n = off < sz ? std::min(chunk, sz - off) : 0;
                        if (nt) ntCopy(d + off, s + off, n); else memcpy(d + off, s + off, n);
                        done.fetch_add(1, std::memory_order_release);
                    }
                });
            std::vector<double> runs;
            for (int r = 0; r < 60; ++r) {
                flush();
                done = 0;
                const uint64_t t0 = qpc();
                go.fetch_add(1, std::memory_order_release);
                if (nt) ntCopy(d, s, std::min(chunk, sz)); else memcpy(d, s, std::min(chunk, sz));
                while (done.load(std::memory_order_acquire) < threads - 1) _mm_pause();
                runs.push_back(ticksToUs((int64_t)(qpc() - t0)));
            }
            stop = true;
            for (auto& th : pool) th.join();
            return std::pair<double, double>(median(runs), pctl(runs, 0.95));
        };
        for (int th : {1, 2, 4})
            for (bool nt : {false, true}) {
                auto [p50, p95] = parallel(th, nt);
                logInfo("  cold-source frame copy, %d thread(s), %-9s: p50 %4.0f us, p95 %4.0f us", th, nt ? "NT stores" : "memcpy",
                        p50, p95);
            }
    }
    // Cross-thread wake: spin vs auto-reset event vs WaitOnAddress
    auto wakeBench = [](const char* name, int mode) {
        std::atomic<uint64_t> stamp{0};
        std::atomic<uint32_t> word{0};
        std::atomic<bool> stop{false};
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::vector<double> lat;
        lat.reserve(2000);
        std::thread waiter([&] {
            uint32_t seen = 0;
            while (!stop.load()) {
                if (mode == 0) {
                    while (word.load(std::memory_order_acquire) == seen && !stop.load()) _mm_pause();
                } else if (mode == 1) {
                    WaitForSingleObject(ev, 100);
                } else {
                    uint32_t s = seen;
                    while (word.load(std::memory_order_acquire) == seen && !stop.load()) WaitOnAddress(&word, &s, 4, 100);
                }
                const uint64_t now = qpc();
                const uint32_t w = word.load(std::memory_order_acquire);
                if (w != seen) {
                    seen = w;
                    lat.push_back(ticksToUs((int64_t)(now - stamp.load())));
                }
            }
        });
        Sleep(50);
        for (int i = 0; i < 1000; ++i) {
            stamp.store(qpc());
            word.fetch_add(1, std::memory_order_release);
            if (mode == 1) SetEvent(ev);
            if (mode == 2) WakeByAddressSingle(&word);
            spinUntil(qpc() + (uint64_t)usToTicks(1000));
        }
        stop = true;
        word.fetch_add(1);
        SetEvent(ev);
        WakeByAddressSingle(&word);
        waiter.join();
        CloseHandle(ev);
        logInfo("  wake %-14s: p50 %6.2f us, p99 %7.2f us", name, median(lat), pctl(lat, 0.99));
    };
    wakeBench("spin", 0);
    wakeBench("SetEvent/Wait", 1);
    wakeBench("WaitOnAddress", 2);

    // High-resolution waitable timer: how late does a sleep to a deadline wake up?
    for (int mode = 0; mode < 3; ++mode) {
        // 0: as is; 1: + TIME_CRITICAL thread; 2: + timeBeginPeriod(1)
        if (mode == 1) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        if (mode == 2) timeBeginPeriod(1);
        logInfo("  -- timer mode: %s", mode == 0 ? "normal thread" : mode == 1 ? "time-critical thread" : "time-critical + timeBeginPeriod(1)");
        PreciseSleeper sl;
        for (double target : {250.0, 1000.0}) {
            std::vector<double> late;
            for (int i = 0; i < 200; ++i) {
                const uint64_t d = qpc() + (uint64_t)usToTicks(target);
                sl.sleepUntil(d, 0.0);
                late.push_back(ticksToUs((int64_t)(qpc() - d)));
            }
            logInfo("  hi-res timer sleep %4.0f us%s: wakes late by p50 %6.1f us, p99 %6.1f us, max %6.1f us", target,
                    sl.highResolution() ? "" : " (NO hi-res timer)", median(late), pctl(late, 0.99),
                    *std::max_element(late.begin(), late.end()));
        }
        if (mode == 2) timeEndPeriod(1);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
}

struct Variant {
    const char* label;
    const char* flag;
    void (*apply)(Options&);
};

struct Measured {
    std::vector<double> ours50, ours95, ours99, dec50, dma50, cons50;
};

bool runVariant(const Options& base, const std::wstring& file, const Variant& v, double seconds, Measured& m) {
    Options o = base;
    o.replayPath = file;
    o.replayLoop = true;
    o.noMf = true;
    o.noDs = false;
    v.apply(o);
    auto p = std::make_unique<Pipeline>();
    std::string err;
    if (!p->init(o, err) || !p->startReplay(err)) {
        logError("variant %s: %s", v.label, err.c_str());
        return false;
    }
    p->publisher.setFormat(p->replaySps().width, p->replaySps().height, (uint32_t)p->replayFps(), 1);
    // An in-process consumer, so the publish -> consumed stage is exercised too.
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        HANDLE sec = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, ipc::kDsSectionName);
        if (!sec) return;
        void* b = MapViewOfFile(sec, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        auto* h = (ipc::RegionHeader*)b;
        const int idx = ipc::claimReader(h, 3);
        wchar_t en[96];
        swprintf(en, 96, ipc::kDsEventFmt, (unsigned)idx);
        HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, en);
        std::vector<uint8_t> buf(ipc::kPayloadCapacity);
        uint64_t last = h->publishCount.load(), retries = 0;
        while (!stop.load()) {
            if (ipc::waitForFrame(h, idx, last, ev, 50) != ipc::WaitResult::Frame) continue;
            last = h->publishCount.load();
            ipc::FrameInfo fi{};
            if (ipc::readLatest(h, b, fi, [&](const uint8_t* src, const ipc::SlotHeader& s) {
                    ipc::copyNv12(src, s, buf.data(), s.width, buf.data() + (size_t)s.width * s.height, s.width, s.width, s.height);
                }, &retries))
                ipc::recordConsumed(h, idx, fi, retries);
        }
        ipc::releaseReader(h, idx);
        if (ev) CloseHandle(ev);
        UnmapViewOfFile(b);
        CloseHandle(sec);
    });
    Sleep((DWORD)(seconds * 1000));
    stop = true;
    reader.join();
    p->stop();
    for (auto& s : p->telemetry.summaries()) {
        if (s.name == "OURS last byte->publish") {
            m.ours50.push_back(s.p50);
            m.ours95.push_back(s.p95);
            m.ours99.push_back(s.p99);
        } else if (s.name == "decode completion") {
            m.dec50.push_back(s.p50);
        } else if (s.name == "DMA (cpu)") {
            m.dma50.push_back(s.p50);
        } else if (s.name == "published -> DS consumed") {
            m.cons50.push_back(s.p50);
        }
    }
    return true;
}

}  // namespace

int runBench(const Options& base) {
    const std::wstring file = base.replayPath.empty() ? findTestData(L"1080p60_4slice_aud.h264") : base.replayPath;
    microBenchmarks(file);
    if (base.benchMicroOnly) return 0;
    if (file.empty()) {
        logError("pipeline variants need a canned stream: pass --replay <file.h264> or generate testdata (README)");
        return 1;
    }
    static const Variant variants[] = {
        {"default", "", [](Options&) {}},
        {"pinned staging + memcpy", "--no-host-register", [](Options& o) { o.hostRegister = false; }},
        {"two 2D DMAs", "--copy 2d", [](Options& o) { o.copyMode = CopyMode::TwoPlane2D; }},
        {"spinning hand-offs", "--spin", [](Options& o) { o.spinStages = true; o.inlineDecode = false; }},
        {"GPU wait: spin", "--gpu-wait spin", [](Options& o) { o.gpuWait = GpuWait::Spin; }},
        {"GPU wait: block", "--gpu-wait block", [](Options& o) { o.gpuWait = GpuWait::Block; }},
        {"decode on its own thread", "--decode-thread", [](Options& o) { o.inlineDecode = false; }},
        {"no core pinning", "--no-pin", [](Options& o) { o.pinThreads = false; }},
        {"no MMCSS", "--no-mmcss", [](Options& o) { o.mmcss = false; }},
        {"memset keepalive", "--keepalive memset", [](Options& o) { o.keepalive = Keepalive::Memset; }},
        {"no GPU keepalive", "--keepalive off", [](Options& o) { o.keepalive = Keepalive::Off; }},
    };
    const int nv = (int)(sizeof(variants) / sizeof(variants[0]));
    const double seconds = 8.0;
    std::vector<Measured> m(nv);
    logInfo("\nPipeline variants: %d x 2 rounds x %.0f s each, replaying %s (first 120 frames of each run discarded)", nv,
            seconds, narrow(file).c_str());
    for (int round = 0; round < 2; ++round) {
        for (int k = 0; k < nv; ++k) {
            const int i = round == 0 ? k : nv - 1 - k;  // reverse order in round 2
            logInfo("  round %d: %s %s", round + 1, variants[i].label, variants[i].flag);
            runVariant(base, file, variants[i], seconds, m[i]);
            if (g_quit.load()) return 1;
        }
    }
    logInfo("\n%-26s %-22s %9s %9s %9s %9s %9s %9s", "variant", "flag", "OURS p50", "p95", "p99", "decode", "DMA",
            "consumed");
    int best = 0;
    for (int i = 0; i < nv; ++i) {
        logInfo("%-26s %-22s %9.0f %9.0f %9.0f %9.0f %9.0f %9.1f", variants[i].label, variants[i].flag, median(m[i].ours50),
                median(m[i].ours95), median(m[i].ours99), median(m[i].dec50), median(m[i].dma50), median(m[i].cons50));
        if (median(m[i].ours50) < median(m[best].ours50)) best = i;
    }
    logInfo("(microseconds; OURS = last byte received -> published to shared memory; each cell is the median of 2 runs)");
    logInfo("\nFastest by OURS p50: %s %s. Differences under ~100 us are within run-to-run noise.",
            variants[best].label, variants[best].flag);
    return 0;
}

}  // namespace cf
