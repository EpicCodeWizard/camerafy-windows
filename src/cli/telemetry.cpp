#include "telemetry.h"
#include "console.h"
#include "qpc.h"
#include <algorithm>
#include <math.h>

namespace cf {

namespace {

enum Stage {
    sXfer,          // recv: AU first byte -> last byte (USB transfer of the AU)
    sBoundary,      // AU last byte -> AU closed (the frame-boundary cost)
    sToDecode,      // AU closed -> decode thread dequeued
    sDecodeSubmit,  // dequeued -> cuvidDecodePicture returned
    sToMapper,      // submitted -> mapper dequeued
    sDecodeWait,    // mapper dequeued -> decode status complete
    sMap,           // -> cuvidMapVideoFrame64 returned
    sDmaCpu,        // mapped -> first DMA complete (CPU view)
    sDmaGpu,        // GPU-timed DMA (cuEventElapsedTime)
    sPubDs,         // mapped -> published to DirectShow section
    sPubMf,         // mapped -> published to MF section
    sOursLast,      // last byte received -> first publish (our share, excluding USB transfer)
    sOursFirst,     // first byte received -> first publish
    sConsDs,        // published -> DirectShow filter copied it into a sample
    sConsMf,        // published -> MF source queued the sample to the Frame Server
    sPhoneIsp,      // phone: camera PTS -> capture delegate (sensor/ISP; outside the app)
    sPhoneApp,      // phone: capture delegate -> VTCompressionSessionEncodeFrame
    sPhoneEncode,   // phone: encode submit -> last slice out of the encoder
    sPhoneSend,     // phone: last slice out -> writev with the AUD returned
    sUsbAbs,        // phone writev returned -> Windows recv returned the AUD (clock-corrected)
    sGlassPub,      // phone camera PTS -> first publish (clock-corrected)
    sCount
};

const char* kNames[sCount] = {
    "recv first->last byte", "boundary wait", "AU -> decode thread", "parse+decode submit", "submit -> mapper",
    "decode completion", "map", "DMA (cpu)", "DMA (gpu events)", "map -> published DS", "map -> published MF",
    "OURS last byte->publish", "OURS first byte->publish", "published -> DS consumed", "published -> MF delivered",
    "phone PTS->delegate (ISP)", "phone delegate->enc submit", "phone encode", "phone enc out->sent",
    "USB+AMDS (abs, +-clk)", "GLASS->publish (abs, +-clk)",
};

double us(uint64_t a, uint64_t b) { return (a && b) ? ticksToUs((int64_t)(b - a)) : NAN; }

void pct(std::vector<double> v, double& mean, double& p50, double& p95, double& p99, double& p999, double& mx) {
    if (v.empty()) { mean = p50 = p95 = p99 = p999 = mx = NAN; return; }
    std::sort(v.begin(), v.end());
    double s = 0;
    for (double x : v) s += x;
    mean = s / (double)v.size();
    auto q = [&](double p) { return v[std::min(v.size() - 1, (size_t)ceil(p * (double)v.size()) - (p > 0 ? 1 : 0))]; };
    p50 = q(0.5); p95 = q(0.95); p99 = q(0.99); p999 = q(0.999); mx = v.back();
}

}  // namespace

void Telemetry::init(const Options& o, TimingRing* timing, Publisher* pub, const Assembler* asmb, const Decoder* dec,
                     PhoneTimingRing* phone, const ClockEstimate* clock) {
    opt_ = o;
    timing_ = timing;
    pub_ = pub;
    asm_ = asmb;
    dec_ = dec;
    phone_ = phone;
    clock_ = clock;
    names_.assign(kNames, kNames + sCount);
    win_.assign(sCount, Window{});
    for (auto& w : win_) w.all.reserve(1 << 16);
    if (!o.csvPath.empty()) {
        csv_ = _wfopen(o.csvPath.c_str(), L"w");
        if (!csv_) logWarn("cannot open CSV %s", narrow(o.csvPath).c_str());
        else
            fprintf(csv_, "seq,phone_frame_id,bytes,slices,idr,close_reason,"
                          "t_recv_first_qpc,recv_xfer_us,boundary_wait_us,to_decode_us,decode_submit_us,to_mapper_us,"
                          "decode_wait_us,map_us,dma_cpu_us,dma_gpu_us,pub_ds_us,pub_mf_us,ours_last_byte_us,"
                          "ours_first_byte_us,phone_isp_us,phone_app_us,phone_encode_us,phone_send_us,usb_amds_abs_us,glass_to_publish_abs_us\n");
    }
}

void Telemetry::start() {
    stop_ = false;
    thread_ = CreateThread(nullptr, 0, threadProc, this, 0, nullptr);
}

void Telemetry::stop() {
    if (stop_.exchange(true)) return;
    if (thread_) {
        WaitForSingleObject(thread_, 3000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    drain();
    if (csv_) {
        fclose(csv_);
        csv_ = nullptr;
    }
}

DWORD WINAPI Telemetry::threadProc(void* self) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    ((Telemetry*)self)->loop();
    return 0;
}

void Telemetry::loop() {
    const uint64_t start = qpc();
    uint64_t nextReport = start + (uint64_t)usToTicks(1e6);
    while (!stop_.load()) {
        Sleep(100);  // ≤10 Hz sampling
        logSps();
        drain();
        const uint64_t now = qpc();
        if (now >= nextReport) {
            report(ticksToUs((int64_t)(now - start)) / 1e6);
            nextReport += (uint64_t)usToTicks(1e6);
            if (nextReport < now) nextReport = now + (uint64_t)usToTicks(1e6);
        }
    }
}

// The stream's real profile, from the SPS itself. The iOS low-latency rate control is documented
// for High profiles only, so whether a requested Constrained Baseline is honoured shows up here.
void Telemetry::logSps() {
    if (!asm_) return;
    const uint32_t v = asm_->spsVersion.load(std::memory_order_acquire);
    if (v == seenSpsVersion_) return;
    seenSpsVersion_ = v;
    const h264::Sps s = asm_->spsSnapshot;
    const bool cs1 = (s.constraintFlags & 0x40) != 0;
    const char* name = s.profileIdc == 66 ? (cs1 ? "Constrained Baseline" : "Baseline")
                     : s.profileIdc == 77 ? "Main"
                     : s.profileIdc == 100 ? "High"
                     : "other";
    logInfo("stream SPS: profile_idc %u (%s), constraint flags 0x%02X, level %u.%u, %ux%u, %u ref frame(s), POC type %u",
            s.profileIdc, name, s.constraintFlags, s.levelIdc / 10, s.levelIdc % 10, s.width, s.height, s.numRefFrames,
            s.pocType);
}

void Telemetry::account(const FrameTiming& t, bool havePhone, const PhoneTiming& ph) {
    ++postWarmup_;
    if (havePhone) ++phoneJoined_;
    else if (phone_ && t.phoneFrameId) ++phoneMissing_;
    const int64_t offUs = clock_ ? clock_->offsetUs.load() : 0;
    const bool haveClock = clock_ && clock_->rttUs.load() >= 0;
    const uint64_t pub = t.tPublishedDs && t.tPublishedMf ? std::min(t.tPublishedDs, t.tPublishedMf)
                                                          : (t.tPublishedDs ? t.tPublishedDs : t.tPublishedMf);
    double v[sCount];
    for (double& x : v) x = NAN;
    v[sXfer] = us(t.tRecvFirst, t.tRecvLast);
    v[sBoundary] = us(t.tRecvLast, t.tAuComplete);
    v[sToDecode] = us(t.tAuComplete, t.tDecodeDequeue);
    v[sDecodeSubmit] = us(t.tDecodeDequeue, t.tDecodeSubmit);
    v[sToMapper] = us(t.tDecodeSubmit, t.tMapDequeue);
    v[sDecodeWait] = us(t.tMapDequeue, t.tDecodeDone);
    v[sMap] = us(t.tDecodeDone ? t.tDecodeDone : t.tMapDequeue, t.tMapped);
    v[sDmaCpu] = us(t.tMapped, t.tDmaDone);
    v[sDmaGpu] = t.gpuDmaUs > 0 ? t.gpuDmaUs : NAN;
    v[sPubDs] = us(t.tMapped, t.tPublishedDs);
    v[sPubMf] = us(t.tMapped, t.tPublishedMf);
    v[sOursLast] = us(t.tRecvLast, pub);
    v[sOursFirst] = us(t.tRecvFirst, pub);
    auto d = [](uint64_t a, uint64_t b) { return (a && b) ? (double)(int64_t)(b - a) : NAN; };
    if (havePhone) {
        v[sPhoneIsp] = d(ph.capturePtsUs, ph.captureCallbackUs);
        v[sPhoneApp] = d(ph.captureCallbackUs, ph.encodeSubmitUs);
        v[sPhoneEncode] = d(ph.encodeSubmitUs, ph.encodeCompleteUs);
        v[sPhoneSend] = d(ph.encodeCompleteUs, ph.lastByteSendUs);
        if (haveClock) {
            // phone us -> Windows us: subtract the offset (phone - windows)
            if (ph.lastByteSendUs) v[sUsbAbs] = ticksToUs((int64_t)t.tRecvLast) - ((double)ph.lastByteSendUs - (double)offUs);
            if (ph.capturePtsUs && pub) v[sGlassPub] = ticksToUs((int64_t)pub) - ((double)ph.capturePtsUs - (double)offUs);
        }
    }
    for (int s = 0; s < sCount; ++s)
        if (!isnan(v[s])) win_[s].add(v[s]);
    if (csv_) {
        fprintf(csv_, "%llu,%llu,%u,%u,%u,%u,%llu", (unsigned long long)t.seq, (unsigned long long)t.phoneFrameId,
                t.bytes, t.slices, (t.flags & kAuIdr) ? 1 : 0, (unsigned)t.close, (unsigned long long)t.tRecvFirst);
        const int cols[] = {sXfer,     sBoundary,    sToDecode,  sDecodeSubmit, sToMapper, sDecodeWait, sMap,
                            sDmaCpu,   sDmaGpu,      sPubDs,     sPubMf,        sOursLast, sOursFirst,  sPhoneIsp,
                            sPhoneApp, sPhoneEncode, sPhoneSend, sUsbAbs,       sGlassPub};
        for (int c : cols) isnan(v[c]) ? (void)fputs(",", csv_) : (void)fprintf(csv_, ",%.1f", v[c]);
        fputc('\n', csv_);
    }
}

void Telemetry::drainConsumers(Region& r, int stage, uint32_t* lastHead) {
    if (!r.hdr) return;
    for (uint32_t i = 0; i < ipc::kMaxReaders; ++i) {
        auto& log = r.hdr->consume[i];
        const uint32_t head = log.head.load(std::memory_order_acquire);
        uint32_t from = lastHead[i];
        if (head - from > ipc::kConsumeLogSize) from = head - ipc::kConsumeLogSize;
        for (uint32_t k = from; k != head; ++k) {
            const auto e = log.e[k & (ipc::kConsumeLogSize - 1)];
            if (postWarmup_ && e.publishedQpc && e.consumedQpc >= e.publishedQpc)
                win_[stage].add(ticksToUs((int64_t)(e.consumedQpc - e.publishedQpc)));
        }
        lastHead[i] = head;
    }
}

void Telemetry::drain() {
    AcquireSRWLockExclusive(&lock_);
    PhoneTiming pt;
    while (phone_ && phone_->pop(pt)) phoneHist_[pt.frameId % kPhoneHist] = pt;

    FrameTiming t;
    while (timing_->pop(t)) {
        const uint64_t n = frames_.fetch_add(1) + 1;
        if (n <= kWarmupFrames) continue;
        pending_.push_back(t);
    }
    // Join each frame with its FrameTiming; give up on the phone side after 500 ms.
    const uint64_t now = qpc();
    const uint64_t maxWait = (uint64_t)usToTicks(500000);
    size_t keep = 0;
    for (size_t i = 0; i < pending_.size(); ++i) {
        const FrameTiming& f = pending_[i];
        const PhoneTiming& ph = phoneHist_[f.phoneFrameId % kPhoneHist];
        const bool have = f.phoneFrameId && ph.frameId == f.phoneFrameId;
        if (have || !phone_ || !f.phoneFrameId || stop_.load() || now - f.tAuComplete > maxWait) {
            account(f, have, ph);
        } else {
            pending_[keep++] = f;
        }
    }
    pending_.resize(keep);
    drainConsumers(pub_->ds(), sConsDs, dsHead_);
    drainConsumers(pub_->mf(), sConsMf, mfHead_);

    // Hot-path events: printed here, never formatted on the producing thread.
    for (auto& ring : g_events) {
        HotEvent e;
        while (ring.pop(e)) logVerbose("[event] %s a=%u b=%llu", evName(e.code), e.a, (unsigned long long)e.b);
    }
    ReleaseSRWLockExclusive(&lock_);
}

void Telemetry::report(double elapsed) {
    AcquireSRWLockExclusive(&lock_);
    const uint64_t n = frames_.load();
    const double fps = (double)(n - lastSecondFrames_);
    lastSecondFrames_ = n;
    if (n <= kWarmupFrames) {
        logInfo("[%5.0fs] warming up: %llu/%u frames (statistics start after %u)", elapsed, (unsigned long long)n,
                kWarmupFrames, kWarmupFrames);
        ReleaseSRWLockExclusive(&lock_);
        return;
    }
    // Live line from the most recent 600 samples of each stage.
    auto recent = [&](int s, double& p50, double& p99) {
        const auto& a = win_[s].all;
        if (a.empty()) { p50 = p99 = NAN; return; }
        std::vector<double> v(a.end() - (ptrdiff_t)std::min<size_t>(a.size(), 600), a.end());
        double mean, p95, p999, mx;
        pct(v, mean, p50, p95, p99, p999, mx);
    };
    double ol50, ol99, bw50, bw99, dec50, dec99, dma50, dma99, cd50, cd99, cm50, cm99, gp50, gp99;
    recent(sOursLast, ol50, ol99);
    recent(sBoundary, bw50, bw99);
    recent(sDecodeWait, dec50, dec99);
    recent(sDmaCpu, dma50, dma99);
    recent(sConsDs, cd50, cd99);
    recent(sConsMf, cm50, cm99);
    recent(sGlassPub, gp50, gp99);
    const auto& as = asm_->stats();
    const auto& ds = dec_->stats();
    char glass[96] = "";
    if (!isnan(gp50))
        snprintf(glass, sizeof(glass), " | glass->pub %.1f ms (+-%.1f)", gp50 / 1000, clock_->rttUs.load() / 2000.0);
    logInfo("[%5.0fs] %4.0f fps | OURS last-byte->publish p50 %6.0f p99 %6.0f us | boundary %5.0f | decode %5.0f | "
            "DMA %5.0f | DS consumed +%.0f | MF delivered +%.0f%s | drops pre-IDR %llu ring %llu | stale skipped %llu",
            elapsed, fps, ol50, ol99, bw50, dec50, dma50, cd50, cm50, glass,
            (unsigned long long)as.droppedBeforeIdr.load(), (unsigned long long)(as.droppedRingFull.load() + ds.picRingDrops.load()),
            (unsigned long long)ds.staleSkipped.load());

    // Regression alarm: p95 more than 200 µs above the baseline (first 600 post-warmup samples).
    for (int s = 0; s < sCount; ++s) {
        auto& w = win_[s];
        if (w.all.size() < 600) continue;
        double mean, p50, p95, p99, p999, mx;
        if (w.baselineP95 < 0) {
            pct(std::vector<double>(w.all.begin(), w.all.begin() + 600), mean, p50, p95, p99, p999, mx);
            w.baselineP95 = p95;
            continue;
        }
        pct(std::vector<double>(w.all.end() - 600, w.all.end()), mean, p50, p95, p99, p999, mx);
        if (p95 > w.baselineP95 + 200.0) {
            if (!w.alarmed)
                logWarn("REGRESSION: stage '%s' p95 %.0f us is %.0f us above its baseline %.0f us", kNames[s], p95,
                        p95 - w.baselineP95, w.baselineP95);
            w.alarmed = true;
        } else {
            w.alarmed = false;
        }
    }

    // A frame of latency from the fallback boundary rule is a defect, not a design: say so.
    const uint64_t closedNext = as.closedBy[(int)CloseReason::NextSlice].load();
    const uint64_t closedAll = as.aus.load();
    if (!fallbackWarned_ && closedAll > 300 && closedNext * 2 > closedAll) {
        fallbackWarned_ = true;
        char body[1024];
        snprintf(body, sizeof(body),
                 "%llu of %llu access units were closed only when the NEXT frame's first slice arrived.\n"
                 "Measured cost (boundary wait p50): %.2f ms per frame, added to every frame.\n"
                 "The sender must end each frame with an AUD (00 00 00 01 09 F0) written immediately after the\n"
                 "last slice, or send the frame's byte length on the sideband.",
                 (unsigned long long)closedNext, (unsigned long long)closedAll, bw50 / 1000.0);
        logBanner("FRAME-BOUNDARY FALLBACK IN USE", body);
    }
    ReleaseSRWLockExclusive(&lock_);
}

double Telemetry::boundaryWaitP50() {
    AcquireSRWLockShared(&lock_);
    double mean, p50, p95, p99, p999, mx;
    pct(win_[sBoundary].all, mean, p50, p95, p99, p999, mx);
    ReleaseSRWLockShared(&lock_);
    return p50;
}

std::vector<StageSummary> Telemetry::summaries() {
    std::vector<StageSummary> out;
    AcquireSRWLockShared(&lock_);
    for (int s = 0; s < sCount; ++s) {
        StageSummary x{kNames[s]};
        pct(win_[s].all, x.mean, x.p50, x.p95, x.p99, x.p999, x.max);
        x.count = win_[s].all.size();
        x.baselineP95 = win_[s].baselineP95;
        out.push_back(x);
    }
    ReleaseSRWLockShared(&lock_);
    return out;
}

void Telemetry::printFullTable() {
    auto rows = summaries();
    logInfo("\nPer-stage latency, microseconds (first %u frames discarded; %llu frames counted)", kWarmupFrames,
            (unsigned long long)postWarmup_);
    logInfo("%-28s %8s %8s %8s %8s %8s %8s %7s", "stage", "mean", "p50", "p95", "p99", "p99.9", "max", "n");
    for (auto& r : rows) {
        if (!r.count) continue;
        logInfo("%-28s %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f %7zu", r.name.c_str(), r.mean, r.p50, r.p95, r.p99, r.p999,
                r.max, r.count);
    }
    const auto& as = asm_->stats();
    logInfo("AU closed by: AUD %llu, next-AU non-VCL %llu, next frame's slice (fallback) %llu, length %llu, EOS %llu",
            (unsigned long long)as.closedBy[(int)CloseReason::Aud].load(),
            (unsigned long long)as.closedBy[(int)CloseReason::NonVcl].load(),
            (unsigned long long)as.closedBy[(int)CloseReason::NextSlice].load(),
            (unsigned long long)as.closedBy[(int)CloseReason::Length].load(),
            (unsigned long long)as.closedBy[(int)CloseReason::EndOfStream].load());
    logInfo("Our share is the OURS rows. Phone sensor/ISP (~16-35 ms) and the consuming app's own pipeline "
            "(~16-50 ms) are outside this receiver and dominate any glass-to-glass figure.");
    if (clock_ && clock_->rttUs.load() >= 0)
        logInfo("Absolute (abs) rows depend on clock correlation: offset error is bounded by RTT/2 = +-%.2f ms.",
                clock_->rttUs.load() / 2000.0);
}

}  // namespace cf
