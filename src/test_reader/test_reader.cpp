// Standalone shared-memory reader: attaches like the DirectShow filter does, copies every
// frame with the same code path, and reports publish -> copied latency in microseconds.
//
//   cf_test_reader [--mf] [--frames N] [--dump file.nv12] [--spin-us N]
#include "ipc_reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>

using namespace cf;

int wmain(int argc, wchar_t** argv) {
    bool mf = false;
    int frames = 600;
    const wchar_t* dump = nullptr;
    int spinUs = -1;
    int threads = 1;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--mf")) mf = true;
        else if (!wcscmp(argv[i], L"--frames") && i + 1 < argc) frames = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--dump") && i + 1 < argc) dump = argv[++i];
        else if (!wcscmp(argv[i], L"--spin-us") && i + 1 < argc) spinUs = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--threads") && i + 1 < argc) threads = _wtoi(argv[++i]);
        else {
            fwprintf(stderr, L"usage: cf_test_reader [--mf] [--frames N] [--dump file.nv12] [--spin-us N] [--threads N]\n");
            return 2;
        }
    }
    const wchar_t* secName = mf ? ipc::kMfSectionName : ipc::kDsSectionName;
    HANDLE sec = nullptr;
    for (int i = 0; i < 100 && !sec; ++i) {
        sec = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, secName);
        if (!sec) Sleep(100);
    }
    if (!sec) {
        fwprintf(stderr, L"cannot open %s: error %lu (is camerafy.exe running?)\n", secName, GetLastError());
        return 1;
    }
    void* base = MapViewOfFile(sec, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (!base || !ipc::validRegion(base)) {
        fwprintf(stderr, L"bad region\n");
        return 1;
    }
    auto* h = (ipc::RegionHeader*)base;
    if (spinUs >= 0) h->readerSpinUs.store((uint32_t)spinUs);
    const int idx = ipc::claimReader(h, 3);
    if (idx < 0) {
        fwprintf(stderr, L"no free reader slot\n");
        return 1;
    }
    wchar_t evName[96];
    swprintf(evName, 96, mf ? ipc::kMfEventFmt : ipc::kDsEventFmt, (unsigned)idx);
    HANDLE evt = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, evName);
    if (!evt) evt = CreateEventW(nullptr, FALSE, FALSE, evName);
    ipc::raiseCameraThread();
    wprintf(L"attached to %s as reader %d (spin %u us); waiting for %d frames\n", secName, idx,
            h->readerSpinUs.load(), frames);

    std::vector<uint8_t> buf((size_t)ipc::kPayloadCapacity);
    ipc::ParallelCopier copier;
    copier.start((uint32_t)threads);
    wprintf(L"copy threads: %u\n", copier.threads());
    std::vector<double> wakeUs, copyUs;
    uint64_t last = h->publishCount.load();
    uint64_t retries = 0, lastFrameId = 0, gaps = 0;
    FILE* df = dump ? _wfopen(dump, L"wb") : nullptr;
    for (int n = 0; n < frames;) {
        auto r = ipc::waitForFrame(h, idx, last, evt, 2000, [&] { copier.prepare(); });
        if (r != ipc::WaitResult::Frame) {
            wprintf(L"timeout waiting for frame (%d received)\n", n);
            ipc::heartbeat(h, idx);
            continue;
        }
        const uint64_t woke = qpc();
        last = h->publishCount.load(std::memory_order_acquire);
        ipc::FrameInfo fi{};
        uint32_t w = 0, hh = 0;
        const bool ok = ipc::readLatest(h, base, fi, [&](const uint8_t* src, const ipc::SlotHeader& s) {
            w = s.width;
            hh = s.height;
            copier.copyNv12(src, s, buf.data(), s.width, buf.data() + (size_t)s.width * s.height, s.width, s.width, s.height);
        }, &retries);
        const uint64_t done = qpc();
        if (!ok) continue;
        ipc::recordConsumed(h, idx, fi, retries);
        if (lastFrameId && fi.frameId != lastFrameId + 1) ++gaps;
        lastFrameId = fi.frameId;
        wakeUs.push_back(ticksToUs((int64_t)(woke - fi.winPublishedQpc)));
        copyUs.push_back(ticksToUs((int64_t)(done - fi.winPublishedQpc)));
        if (df && n == frames - 1) fwrite(buf.data(), 1, (size_t)w * hh * 3 / 2, df);
        ++n;
    }
    if (df) fclose(df);
    ipc::releaseReader(h, idx);
    auto pct = [](std::vector<double> v, double p) {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : v[std::min(v.size() - 1, (size_t)(p * v.size()))];
    };
    // Skip the first 120 frames, as everywhere else.
    if (wakeUs.size() > 240) {
        wakeUs.erase(wakeUs.begin(), wakeUs.begin() + 120);
        copyUs.erase(copyUs.begin(), copyUs.begin() + 120);
    }
    wprintf(L"publish->wake   p50 %8.1f  p95 %8.1f  p99 %8.1f  max %8.1f us\n", pct(wakeUs, .5), pct(wakeUs, .95),
            pct(wakeUs, .99), pct(wakeUs, 1));
    wprintf(L"publish->copied p50 %8.1f  p95 %8.1f  p99 %8.1f  max %8.1f us\n", pct(copyUs, .5), pct(copyUs, .95),
            pct(copyUs, .99), pct(copyUs, 1));
    wprintf(L"seqlock retries %llu, frame-id gaps %llu\n", retries, gaps);
    return 0;
}
