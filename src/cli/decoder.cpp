#include "decoder.h"
#include "console.h"
#include "qpc.h"
#include "ipc.h"
#include "gpu_kernels.h"
#include "tuning.h"
#include <string.h>

namespace cf {

namespace {

inline bool cuOk(CUresult r, const char* what, std::string& err) {
    if (r == CUDA_SUCCESS) return true;
    err = std::string(what) + ": " + g_cu.errorText(r);
    return false;
}

}  // namespace

Decoder::~Decoder() {
    if (gpuStuck_.load()) return;  // GPU work never finished: teardown would block; exit frees it
    if (ctx_) g_cu.cuCtxPushCurrent(ctx_);
    if (parser_) g_cu.cuvidDestroyVideoParser(parser_);
    if (dec_) g_cu.cuvidDestroyDecoder(dec_);
    if (ev0_) g_cu.cuEventDestroy(ev0_);
    if (ev1_) g_cu.cuEventDestroy(ev1_);
    if (evDone_) g_cu.cuEventDestroy(evDone_);
    if (stream_) g_cu.cuStreamDestroy(stream_);
    if (staging_) g_cu.cuMemFreeHost(staging_);
    if (lock_) g_cu.cuvidCtxLockDestroy(lock_);
    if (ctx_) {
        CUcontext d;
        g_cu.cuCtxPopCurrent(&d);
        g_cu.cuCtxDestroy(ctx_);
    }
}

const char* Decoder::copyPathName() const {
    // Registration happens on the first frame; before that, report the intended path.
    const bool reg = pub_ && (pub_->hostRegisterWorks() || opt_.hostRegister);
    if (opt_.copyMode == CopyMode::Linear)
        return reg ? "one linear DMA straight into the registered IPC slot" : "linear DMA into pinned staging + memcpy";
    return reg ? "two 2D DMAs (Y, UV) straight into the registered IPC slot" : "two 2D DMAs into pinned staging + memcpy";
}

bool Decoder::init(const Options& o, Arena* arena, AuRing* aus, ParamRing* params, TimingRing* timing, Publisher* pub,
                   std::string& err) {
    opt_ = o;
    arena_ = arena;
    aus_ = aus;
    params_ = params;
    timing_ = timing;
    pub_ = pub;
    tuning::memCheckpoint("decoder init start");
    if (!g_cu.load(err)) return false;
    if (!cuOk(g_cu.cuInit(0), "cuInit", err)) return false;
    tuning::memCheckpoint("after cuInit");
    CUdevice dev = 0;
    if (!cuOk(g_cu.cuDeviceGet(&dev, (int)o.gpuOrdinal), "cuDeviceGet", err)) return false;
    // Blocking-sync scheduling: a thread waiting on the GPU sleeps in the kernel instead of
    // spinning a core. The hot waits (decode, DMA) are timed by the mapper itself (GpuWait), so
    // only --gpu-wait spin needs the spinning context.
    const unsigned ctxFlags = o.gpuWait == GpuWait::Spin ? CU_CTX_SCHED_SPIN : CU_CTX_SCHED_BLOCKING_SYNC;
    if (!cuOk(g_cu.cuCtxCreate(&ctx_, ctxFlags, dev), "cuCtxCreate", err)) return false;
    tuning::memCheckpoint("after cuCtxCreate");
    if (!cuOk(g_cu.cuvidCtxLockCreate(&lock_, ctx_), "cuvidCtxLockCreate", err)) return false;
    if (!cuOk(g_cu.cuStreamCreate(&stream_, CU_STREAM_NON_BLOCKING), "cuStreamCreate", err)) return false;
    if (!cuOk(g_cu.cuEventCreate(&ev0_, CU_EVENT_DEFAULT), "cuEventCreate", err)) return false;
    if (!cuOk(g_cu.cuEventCreate(&ev1_, CU_EVENT_DEFAULT), "cuEventCreate", err)) return false;
    if (!cuOk(g_cu.cuEventCreate(&evDone_, CU_EVENT_DISABLE_TIMING | CU_EVENT_BLOCKING_SYNC), "cuEventCreate", err))
        return false;
    // The pinned staging buffer is only needed when host registration of the shared memory
    // fails; it is allocated then (copyToRegion), not up front.

    CUVIDPARSERPARAMS pp{};
    pp.CodecType = cudaVideoCodec_H264;
    pp.ulMaxNumDecodeSurfaces = 1;  // corrected by the sequence callback's return value
    pp.ulMaxDisplayDelay = 0;       // zero display delay: mandatory
    pp.ulClockRate = 10000000;
    pp.ulErrorThreshold = 100;
    pp.pUserData = this;
    pp.pfnSequenceCallback = onSequence;
    pp.pfnDecodePicture = onDecode;
    pp.pfnDisplayPicture = onDisplay;
    if (!cuOk(g_cu.cuvidCreateVideoParser(&parser_, &pp), "cuvidCreateVideoParser", err)) return false;
    tuning::memCheckpoint("after parser create");
    CUcontext d;
    g_cu.cuCtxPopCurrent(&d);  // threads make it current themselves
    return true;
}

bool Decoder::precreate(uint32_t w, uint32_t h, std::string& err) {
    CUVIDEOFORMAT f{};
    f.codec = cudaVideoCodec_H264;
    f.chroma_format = cudaVideoChromaFormat_420;
    f.coded_width = (w + 15) & ~15u;
    f.coded_height = (h + 15) & ~15u;
    f.display_area.right = (int)w;
    f.display_area.bottom = (int)h;
    f.min_num_decode_surfaces = 4;  // the real SPS corrects this (recreating if it needs more)
    g_cu.cuCtxPushCurrent(ctx_);
    const bool ok = createDecoder(f);
    CUcontext d;
    g_cu.cuCtxPopCurrent(&d);
    if (!ok) err = "decoder pre-creation failed";
    return ok;
}

int CUDAAPI Decoder::onSequence(void* u, CUVIDEOFORMAT* f) { return ((Decoder*)u)->handleSequence(f); }
int CUDAAPI Decoder::onDecode(void* u, CUVIDPICPARAMS* p) { return ((Decoder*)u)->handleDecode(p); }
int CUDAAPI Decoder::onDisplay(void*, CUVIDPARSERDISPINFO*) { return 1; }  // display order == decode order

// The part of the phone's picture that fills the locked output: the whole display area when the
// aspect ratios match, otherwise a centred crop (even coordinates, as NV12 requires).
Decoder::Rect Decoder::cropFor(const CUVIDEOFORMAT& f) const {
    Rect r{f.display_area.left, f.display_area.top, f.display_area.right, f.display_area.bottom};
    if (!dispW_ || !dispH_) return r;
    const int64_t sw = r.r - r.l, sh = r.b - r.t;
    if (sw * dispH_ > sh * (int64_t)dispW_) {  // source wider: crop the sides
        const int w = (int)((sh * dispW_ / dispH_) & ~1LL);
        r.l += (int)(((sw - w) / 2) & ~1LL);
        r.r = r.l + w;
    } else if (sw * dispH_ < sh * (int64_t)dispW_) {  // source taller: crop top and bottom
        const int h = (int)((sw * dispH_ / dispW_) & ~1LL);
        r.t += (int)(((sh - h) / 2) & ~1LL);
        r.b = r.t + h;
    }
    return r;
}

bool Decoder::createDecoder(const CUVIDEOFORMAT& f) {
    if (dec_) {
        g_cu.cuvidDestroyDecoder(dec_);
        dec_ = nullptr;
    }
    const uint32_t nativeW = (uint32_t)(f.display_area.right - f.display_area.left);
    const uint32_t nativeH = (uint32_t)(f.display_area.bottom - f.display_area.top);
    const bool locked = dispW_ && dispH_;
    const Rect src = cropFor(f);
    CUVIDDECODECREATEINFO ci{};
    ci.CodecType = f.codec;
    ci.ChromaFormat = cudaVideoChromaFormat_420;
    ci.OutputFormat = cudaVideoSurfaceFormat_NV12;
    ci.bitDepthMinus8 = 0;
    ci.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    ci.ulNumOutputSurfaces = 2;
    ci.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    // Four spare surfaces so the parser never reuses one the mapper has not finished with.
    // Sized to the stream, not to the worst case: every surface is real memory
    // (measured: 24 surfaces at 4096x4096 cost 180 MB of RAM and 680 MB of commit).
    ci.ulNumDecodeSurfaces = f.min_num_decode_surfaces + 4 > 8 ? f.min_num_decode_surfaces + 4 : 8;
    ci.vidLock = lock_;
    ci.ulIntraDecodeOnly = 0;
    ci.ulWidth = f.coded_width;
    ci.ulHeight = f.coded_height;
    // No size headroom, for the same reason: a phone format change to a size this decoder
    // can hold is a cuvidReconfigureDecoder, a larger one recreates it (the sequence callback
    // drains the mapper first; both happen inside the phone's 0.3-1.1 s switch gap).
    ci.ulMaxWidth = f.coded_width;
    ci.ulMaxHeight = f.coded_height;
    ci.display_area.left = (short)src.l;
    ci.display_area.top = (short)src.t;
    ci.display_area.right = (short)src.r;
    ci.display_area.bottom = (short)src.b;
    // Target = what consumers get: the cropped display size the first time, the locked size after.
    ci.ulTargetWidth = locked ? dispW_ : nativeW;
    ci.ulTargetHeight = locked ? dispH_ : nativeH;
    const uint64_t t0 = qpc();
    const CUresult r = g_cu.cuvidCreateDecoder(&dec_, &ci);
    if (r != CUDA_SUCCESS) {
        logError("cuvidCreateDecoder(%lux%lu, %lu surfaces): %s", ci.ulWidth, ci.ulHeight, ci.ulNumDecodeSurfaces,
                 g_cu.errorText(r).c_str());
        dec_ = nullptr;
        return false;
    }
    codedW_ = f.coded_width;
    codedH_ = f.coded_height;
    maxW_ = ci.ulMaxWidth;
    maxH_ = ci.ulMaxHeight;
    dispW_ = ci.ulTargetWidth;
    dispH_ = ci.ulTargetHeight;
    srcRect_ = src;
    numSurfaces_ = ci.ulNumDecodeSurfaces;
    fullRange_ = f.video_signal_description.video_full_range_flag != 0;
    stats_.width = dispW_;
    stats_.height = dispH_;
    stats_.surfaceHeight = dispH_;
    stats_.nativeWidth = nativeW;
    stats_.nativeHeight = nativeH;
    stats_.decodeSurfaces = numSurfaces_;
    tuning::memCheckpoint("after cuvidCreateDecoder");
    logInfo("NVDEC decoder created: coded %ux%u, output %ux%u NV12, %u decode surfaces, max %ux%u (%.1f ms)", codedW_,
            codedH_, dispW_, dispH_, numSurfaces_, maxW_, maxH_, ticksToUs((int64_t)(qpc() - t0)) / 1000.0);
    return true;
}

int Decoder::handleSequence(CUVIDEOFORMAT* f) {
    stats_.sequenceCallbacks.fetch_add(1, std::memory_order_relaxed);
    if (f->codec != cudaVideoCodec_H264 || f->chroma_format != cudaVideoChromaFormat_420 || f->bit_depth_luma_minus8) {
        logError("unsupported stream: codec %d chroma %d bit depth %d", (int)f->codec, (int)f->chroma_format,
                 8 + f->bit_depth_luma_minus8);
        return 0;
    }
    const uint32_t dw = (uint32_t)(f->display_area.right - f->display_area.left);
    const uint32_t dh = (uint32_t)(f->display_area.bottom - f->display_area.top);
    const uint32_t need = f->min_num_decode_surfaces + 4u;
    const Rect src = cropFor(*f);
    if (dec_ && f->coded_width == codedW_ && f->coded_height == codedH_ && src == srcRect_ && need <= numSurfaces_)
        return (int)numSurfaces_;
    hotEvent(kEvDecode, Ev::SequenceChange, dw, dh);
    if (dec_) drainMapper();
    stats_.nativeWidth = dw;
    stats_.nativeHeight = dh;
    if (dec_ && need <= numSurfaces_ && f->coded_width <= maxW_ && f->coded_height <= maxH_) {
        // The output size stays locked; only the coded size and the source crop change.
        CUVIDRECONFIGUREDECODERINFO rc{};
        rc.ulWidth = f->coded_width;
        rc.ulHeight = f->coded_height;
        rc.ulTargetWidth = dispW_;
        rc.ulTargetHeight = dispH_;
        rc.ulNumDecodeSurfaces = numSurfaces_;
        rc.display_area.left = (short)src.l;
        rc.display_area.top = (short)src.t;
        rc.display_area.right = (short)src.r;
        rc.display_area.bottom = (short)src.b;
        if (g_cu.cuvidReconfigureDecoder(dec_, &rc) == CUDA_SUCCESS) {
            codedW_ = f->coded_width;
            codedH_ = f->coded_height;
            srcRect_ = src;
            return (int)numSurfaces_;
        }
    }
    if (!createDecoder(*f)) return 0;
    return (int)numSurfaces_;
}

int Decoder::handleDecode(CUVIDPICPARAMS* pp) {
    if (!dec_) return 0;
    const CUresult r = g_cu.cuvidDecodePicture(dec_, pp);
    PicItem it;
    it.picIdx = pp->CurrPicIdx;
    // Another complete AU is already waiting behind this one (a burst after a stall): this
    // picture is stale before it is decoded. It must still be decoded (it is a reference), but
    // the mapper need not publish it.
    it.pad = opt_.catchUp && !aus_->emptyApprox() ? 1u : 0u;
    it.t = cur_;
    it.t.tDecodeSubmit = qpc();
    if (r != CUDA_SUCCESS) {
        stats_.decodeErrors.fetch_add(1, std::memory_order_relaxed);
        hotEvent(kEvDecode, Ev::DecodeError, (uint32_t)r);
        return 1;
    }
    stats_.decoded.fetch_add(1, std::memory_order_relaxed);
    if (!pics_.push(it)) {
        stats_.picRingDrops.fetch_add(1, std::memory_order_relaxed);
        hotEvent(kEvDecode, Ev::PicRingFull);
    } else {
        picsPushed_.fetch_add(1, std::memory_order_relaxed);
        if (!opt_.spinStages) pics_.notify();
    }
    return 1;
}

// Decode thread, sequence change only: let the mapper finish every picture of the old format.
// The phone leaves a 0.3-1.1 s gap before a new SPS, so this normally returns at once; the
// bound keeps a stuck mapper from freezing decode.
void Decoder::drainMapper() {
    const uint64_t want = picsPushed_.load(std::memory_order_relaxed);
    const uint64_t end = qpc() + (uint64_t)usToTicks(500000);
    while (picsMapped_.load(std::memory_order_acquire) < want) {
        if (qpc() > end) {
            logWarn("format change: mapper did not drain within 500 ms; reconfiguring anyway");
            return;
        }
        Sleep(0);
    }
}

bool Decoder::decodeBuffer(const uint8_t* p, size_t n, bool eop) {
    CUVIDSOURCEDATAPACKET pkt{};
    pkt.flags = eop ? (CUVID_PKT_ENDOFPICTURE | CUVID_PKT_TIMESTAMP) : 0;
    pkt.payload = p;
    pkt.payload_size = (unsigned long)n;
    pkt.timestamp = (CUvideotimestamp)cur_.seq;
    const CUresult r = g_cu.cuvidParseVideoData(parser_, &pkt);
    if (r != CUDA_SUCCESS) {
        stats_.parseErrors.fetch_add(1, std::memory_order_relaxed);
        hotEvent(kEvDecode, Ev::ParseError, (uint32_t)r);
        return false;
    }
    return true;
}

void Decoder::processAu(AuItem& au) {
    au.t.tDecodeDequeue = qpc();
    lastAuQpc_.store(au.t.tDecodeDequeue, std::memory_order_relaxed);
    if (au.t.flags & kAuSkip) {
        arena_->release.store(au.end, std::memory_order_release);
        return;
    }
    if (au.t.flags & kAuNeedsParams) {
        ParamBlob b;
        if (params_->pop(b)) decodeBuffer(b.bytes, b.len, false);
    }
    cur_ = au.t;
    decodeBuffer(arena_->at(au.start), (size_t)(au.end - au.start), true);
    // The parser has copied the bitstream; the arena bytes may be reused.
    arena_->release.store(au.end, std::memory_order_release);
}

void Decoder::drainAus() {
    AuItem au;
    while (aus_->pop(au)) processAu(au);
}

void Decoder::runDecodeLoop(const std::atomic<bool>& stop) {
    g_cu.cuCtxSetCurrent(ctx_);
    AuItem au;
    const bool spin = opt_.spinStages;
    while (true) {
        if (spin) {
            if (!aus_->popSpin(au, stop)) break;
        } else {
            if (!aus_->popBlocking(au, stop)) break;
        }
        processAu(au);
    }
}

void Decoder::runMapperLoop(const std::atomic<bool>& stop) {
    g_cu.cuCtxSetCurrent(ctx_);
    PicItem it;
    const bool spin = opt_.spinStages;
    while (true) {
        if (spin) {
            if (!pics_.popSpin(it, stop)) break;
        } else {
            if (!pics_.popBlocking(it, stop)) break;
        }
        it.t.tMapDequeue = qpc();
        // Catch-up: after a PC/USB stall the phone's queued frames arrive
        // back to back. Publishing each one only shows the cameras a fast-forward of stale
        // frames and costs a DMA each; publish only the newest. Never skips the last one queued.
        if (opt_.catchUp && (it.pad || !pics_.emptyApprox())) {
            stats_.staleSkipped.fetch_add(1, std::memory_order_relaxed);
            picsMapped_.fetch_add(1, std::memory_order_release);
            continue;
        }
        mapAndPublish(it);
        picsMapped_.fetch_add(1, std::memory_order_release);
    }
}

void Decoder::runKeepaliveLoop(const std::atomic<bool>& stop) {
    g_cu.cuCtxSetCurrent(ctx_);
    if (opt_.keepalive == Keepalive::Kernel && keepaliveKernel(stop)) return;
    if (!stop.load()) keepaliveMemset(stop);
}

// The driver's idle heuristic drops the GPU to P8 (210 MHz, PCIe Gen1) within seconds unless
// the GPU looks busy, which triples decode and readback time (measured). A resident kernel
// is "busy" to that heuristic; a sleeping warp costs almost no power and the CPU only wakes
// to relaunch it. Two launches are kept queued so there is never a gap between them.
bool Decoder::keepaliveKernel(const std::atomic<bool>& stop) {
    CUmodule mod = nullptr;
    CUfunction fn = nullptr;
    if (g_cu.cuModuleLoadData(&mod, ptx::kIdleKernel) != CUDA_SUCCESS ||
        g_cu.cuModuleGetFunction(&fn, mod, "cf_idle") != CUDA_SUCCESS) {
        logWarn("GPU keepalive kernel unavailable (needs a Volta or newer GPU); using the memset keepalive");
        if (mod) g_cu.cuModuleUnload(mod);
        return false;
    }
    CUstream ks = nullptr;
    CUevent ev[2] = {};
    bool ok = g_cu.cuStreamCreate(&ks, CU_STREAM_NON_BLOCKING) == CUDA_SUCCESS;
    for (auto& e : ev) ok = ok && g_cu.cuEventCreate(&e, CU_EVENT_DISABLE_TIMING | CU_EVENT_BLOCKING_SYNC) == CUDA_SUCCESS;
    auto envU = [](const wchar_t* n, uint32_t d) {
        wchar_t b[32];
        const DWORD k = GetEnvironmentVariableW(n, b, 32);
        return (k && k < 32) ? (uint32_t)_wtoi(b) : d;
    };
    // Measurement knobs: kernel on-time per launch and an idle gap after it (duty cycle).
    const uint32_t onMs = envU(L"CAMERAFY_KA_ON_MS", 50), offMs = envU(L"CAMERAFY_KA_OFF_MS", 0);
    unsigned long long durNs = (unsigned long long)onMs * 1000 * 1000;
    unsigned int sleepNs = envU(L"CAMERAFY_KA_SLEEP_NS", 100000);
    void* args[] = {&durNs, &sleepNs};
    auto launch = [&](int i) {
        return g_cu.cuLaunchKernel(fn, 1, 1, 1, 32, 1, 1, 0, ks, args, nullptr) == CUDA_SUCCESS &&
               g_cu.cuEventRecord(ev[i], ks) == CUDA_SUCCESS;
    };
    ok = ok && launch(0) && (offMs || launch(1));
    if (!ok) logWarn("GPU keepalive kernel launch failed; using the memset keepalive");
    // Polled every 10 ms rather than waited on, so a stop request is never stuck behind the GPU.
    auto finished = [&](int i) {
        while (g_cu.cuEventQuery(ev[i]) == CUDA_ERROR_NOT_READY) {
            if (stop.load(std::memory_order_relaxed)) return false;
            Sleep(10);
        }
        return true;
    };
    // No video for 2 s (the phone is paused: PiP closed, camera taken by another app, or not
    // streaming yet): stop relaunching and let the GPU idle (~19 W instead of ~72 W) until the
    // next AU. The first frames after a pause decode slower while the clocks ramp back up.
    const uint64_t idleAfter = (uint64_t)usToTicks(2e6);
    auto videoIdle = [&] {
        const uint64_t last = lastAuQpc_.load(std::memory_order_relaxed);
        return !last || qpc() - last > idleAfter;
    };
    // Launch i finishes first (it was queued first); relaunch into its slot, then wait on the
    // other. With a duty cycle (offMs) only slot 0 is used.
    for (int i = 0; ok && !stop.load(std::memory_order_relaxed);) {
        if (!finished(i)) break;
        if (offMs) Sleep(offMs);
        if (videoIdle()) {
            if (!offMs && !finished(i ^ 1)) break;  // let the other queued launch end too
            while (videoIdle() && !stop.load(std::memory_order_relaxed)) Sleep(10);
            if (stop.load(std::memory_order_relaxed)) break;
            ok = launch(i) && (offMs || launch(i ^ 1));  // i is again the first to finish
            continue;
        }
        ok = launch(i);
        if (!offMs) i ^= 1;
    }
    // Clean up only if the queued launches end within 200 ms; otherwise leave them to process
    // exit (the driver tears the context down) rather than block shutdown.
    bool idle = true;
    if (ks) {
        const uint64_t end = qpc() + (uint64_t)usToTicks(200000);
        while (g_cu.cuStreamQuery(ks) == CUDA_ERROR_NOT_READY && (idle = qpc() < end)) Sleep(5);
    }
    if (idle) {
        for (auto& e : ev)
            if (e) g_cu.cuEventDestroy(e);
        if (ks) g_cu.cuStreamDestroy(ks);
        g_cu.cuModuleUnload(mod);
    } else {
        gpuStuck_.store(true);
        logWarn("GPU keepalive kernel still running at shutdown; leaving it to process exit");
    }
    return ok || stop.load();
}

void Decoder::keepaliveMemset(const std::atomic<bool>& stop) {
    // Measured load shape: 32 MB memsets back to back kept P0 with the best tail.
    auto envU = [](const wchar_t* n, uint32_t d) {
        wchar_t b[32];
        const DWORD k = GetEnvironmentVariableW(n, b, 32);
        return (k && k < 32) ? (uint32_t)_wtoi(b) : d;
    };
    const uint32_t mb = envU(L"CAMERAFY_KEEPALIVE_MB", 32);
    const uint32_t periodUs = envU(L"CAMERAFY_KEEPALIVE_US", 0);
    const uint32_t reps = envU(L"CAMERAFY_KEEPALIVE_REPS", 1);  // memsets queued per sync
    const size_t bytes = (size_t)mb << 20;
    CUstream ks = nullptr;
    CUdeviceptr dbuf = 0;
    void* hbuf = nullptr;
    if (g_cu.cuStreamCreate(&ks, CU_STREAM_NON_BLOCKING) != CUDA_SUCCESS) return;
    if (g_cu.cuMemAlloc(&dbuf, bytes) != CUDA_SUCCESS || g_cu.cuMemHostAlloc(&hbuf, 4096, 0) != CUDA_SUCCESS) {
        g_cu.cuStreamDestroy(ks);
        return;
    }
    uint8_t v = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        const uint64_t t0 = qpc();
        for (uint32_t r = 0; r < reps; ++r) g_cu.cuMemsetD8Async(dbuf, v++, bytes, ks);
        g_cu.cuMemcpyDtoHAsync(hbuf, dbuf, 4096, ks);
        g_cu.cuStreamSynchronize(ks);
        if (periodUs) spinUntil(t0 + (uint64_t)usToTicks(periodUs));
    }
    g_cu.cuStreamSynchronize(ks);
    g_cu.cuMemFree(dbuf);
    g_cu.cuMemFreeHost(hbuf);
    g_cu.cuStreamDestroy(ks);
}

// Waits until the picture's decode has finished (the map then returns at once).
void Decoder::waitDecode(PicItem& it) {
    FrameTiming& t = it.t;
    if (opt_.gpuWait == GpuWait::Block) return;  // cuvidMapVideoFrame blocks in the driver
    if (opt_.gpuWait == GpuWait::Hybrid) {
        if (const uint64_t m = decodeDur_.min()) {
            const uint64_t margin = (uint64_t)usToTicks(sleeper_.marginUs());
            if (m > margin) sleeper_.sleepUntil(t.tDecodeSubmit + m - margin);
        }
    }
    CUVIDGETDECODESTATUS st{};
    const uint64_t deadline = qpc() + (uint64_t)usToTicks(100000);
    while (g_cu.cuvidGetDecodeStatus(dec_, it.picIdx, &st) == CUDA_SUCCESS &&
           st.decodeStatus == cuvidDecodeStatus_InProgress && qpc() < deadline)
        _mm_pause();
    t.tDecodeDone = qpc();
    if (t.tDecodeSubmit && t.tDecodeDone > t.tDecodeSubmit) decodeDur_.add(t.tDecodeDone - t.tDecodeSubmit);
    if (st.decodeStatus == cuvidDecodeStatus_Error || st.decodeStatus == cuvidDecodeStatus_Error_Concealed)
        hotEvent(kEvMapper, Ev::DecodeError, (uint32_t)st.decodeStatus, t.seq);
}

// Waits for the DMA queued at `enqueuedQpc` (evDone_ recorded after it).
bool Decoder::waitDma(uint64_t enqueuedQpc) {
    CUresult r;
    if (opt_.gpuWait == GpuWait::Block) {
        r = g_cu.cuEventSynchronize(evDone_);
    } else {
        if (opt_.gpuWait == GpuWait::Hybrid) {
            if (const uint64_t m = dmaDur_.min()) {
                const uint64_t margin = (uint64_t)usToTicks(sleeper_.marginUs());
                if (m > margin) sleeper_.sleepUntil(enqueuedQpc + m - margin);
            }
        }
        while ((r = g_cu.cuEventQuery(evDone_)) == CUDA_ERROR_NOT_READY) _mm_pause();
    }
    if (r == CUDA_SUCCESS) dmaDur_.add(qpc() - enqueuedQpc);
    return r == CUDA_SUCCESS;
}

bool Decoder::copyToRegion(Region& r, CUdeviceptr src, uint32_t pitch, FrameTiming& t, bool first) {
    ipc::RegionHeader* h = r.hdr;
    const uint32_t slot = ipc::pickWriteSlot(h, r.base);
    ipc::SlotHeader* s = ipc::slotHeader(r.base, slot);
    uint8_t* payload = ipc::slotPayload(r.base, slot);
    const uint32_t w = dispW_, hh = dispH_;
    const bool linear = opt_.copyMode == CopyMode::Linear;
    const uint32_t dstStride = linear ? pitch : w;
    const uint32_t uvOffset = dstStride * hh;
    const uint32_t bytes = dstStride * hh * 3 / 2;
    if (bytes > ipc::kPayloadCapacity) return false;
    // Pin only the bytes a frame uses, on the first frame (the output size never changes
    // afterwards), rather than every slot's 32 MB capacity.
    pub_->ensureRegistered(r, bytes);
    const bool direct = r.registered.load(std::memory_order_relaxed) && bytes <= r.registeredBytes;
    if (!direct && !staging_) {
        stagingBytes_ = ipc::kPayloadCapacity;
        if (g_cu.cuMemHostAlloc((void**)&staging_, stagingBytes_, CU_MEMHOSTALLOC_PORTABLE) != CUDA_SUCCESS) {
            staging_ = nullptr;
            hotEvent(kEvMapper, Ev::CopyError, 0);
            return false;
        }
    }

    ipc::beginWrite(s);
    // With a registered view the DMA writes the slot itself; otherwise it goes to pinned
    // staging once per frame and is memcpy'd into each slot.
    uint8_t* dst = direct ? payload : staging_;
    const bool needDma = direct || stagingSeq_ != t.seq;
    CUresult res = CUDA_SUCCESS;
    if (needDma) {
        const uint64_t enq = qpc();
        if (first) g_cu.cuEventRecord(ev0_, stream_);
        if (linear) {
            res = g_cu.cuMemcpyDtoHAsync(dst, src, bytes, stream_);
        } else {
            CUDA_MEMCPY2D m{};
            m.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            m.srcDevice = src;
            m.srcPitch = pitch;
            m.dstMemoryType = CU_MEMORYTYPE_HOST;
            m.dstHost = dst;
            m.dstPitch = dstStride;
            m.WidthInBytes = w;
            m.Height = hh;
            res = g_cu.cuMemcpy2DAsync(&m, stream_);
            if (res == CUDA_SUCCESS) {
                m.srcDevice = src + (CUdeviceptr)pitch * hh;
                m.dstHost = dst + uvOffset;
                m.Height = hh / 2;
                res = g_cu.cuMemcpy2DAsync(&m, stream_);
            }
        }
        if (first) g_cu.cuEventRecord(ev1_, stream_);
        if (res == CUDA_SUCCESS) res = g_cu.cuEventRecord(evDone_, stream_);
        if (res == CUDA_SUCCESS && !waitDma(enq)) res = CUDA_ERROR_UNKNOWN;
        if (!direct && res == CUDA_SUCCESS) stagingSeq_ = t.seq;
    }
    if (res != CUDA_SUCCESS) {
        hotEvent(kEvMapper, Ev::CopyError, (uint32_t)res);
        ipc::endWrite(s);
        return false;
    }
    if (!direct) memcpy(payload, staging_, bytes);
    const uint64_t now = qpc();
    if (first) {
        t.tDmaDone = now;
        float ms = 0;
        if (g_cu.cuEventElapsedTime(&ms, ev0_, ev1_) == CUDA_SUCCESS) t.gpuDmaUs = ms * 1000.0f;
    }
    s->frameId = t.phoneFrameId ? t.phoneFrameId : t.seq;
    s->phoneCaptureUs = t.phoneCaptureUs;
    s->phoneEncodeOutUs = t.phoneEncodeOutUs;
    s->winRecvQpc = t.tRecvFirst;
    s->winAuCompleteQpc = t.tAuComplete;
    s->winDecodedQpc = t.tMapped;
    s->width = w;
    s->height = hh;
    s->strideY = dstStride;
    s->strideUV = dstStride;
    s->offsetUV = uvOffset;
    s->payloadBytes = bytes;
    s->flags = ((t.flags & kAuIdr) ? ipc::kFlagIdr : 0) | (fullRange_ ? ipc::kFlagFullRange : 0);
    const uint64_t pubQpc = qpc();
    s->winPublishedQpc = pubQpc;
    ipc::endWrite(s);
    ipc::publish(h, slot, pubQpc);
    r.signalSleepers();
    if (r.isMf) t.tPublishedMf = pubQpc;
    else t.tPublishedDs = pubQpc;
    return true;
}

void Decoder::mapAndPublish(PicItem& it) {
    FrameTiming& t = it.t;
    waitDecode(it);
    CUVIDPROCPARAMS pp{};
    pp.progressive_frame = 1;
    pp.output_stream = stream_;
    unsigned long long dptr = 0;
    unsigned int pitch = 0;
    const CUresult r = g_cu.cuvidMapVideoFrame64(dec_, it.picIdx, &dptr, &pitch, &pp);
    t.tMapped = qpc();
    if (r != CUDA_SUCCESS) {
        stats_.mapErrors.fetch_add(1, std::memory_order_relaxed);
        hotEvent(kEvMapper, Ev::MapError, (uint32_t)r, t.seq);
        return;
    }
    stats_.pitch.store(pitch, std::memory_order_relaxed);

    // DirectShow first (it is the lower-latency path), then Media Foundation. A region with
    // no live reader is skipped; with no readers at all DS is still written so the pipeline
    // keeps running (and measuring) end to end.
    Region& ds = pub_->ds();
    Region& mf = pub_->mf();
    const bool dsReady = ds.ready.load(std::memory_order_acquire);
    const bool mfReady = mf.ready.load(std::memory_order_acquire);
    const bool mfLive = mfReady && mf.anyReader.load(std::memory_order_relaxed);
    const bool dsLive = dsReady && (ds.anyReader.load(std::memory_order_relaxed) || !mfLive);
    // Wake-ahead: the DMAs are about to be queued, so readers can be awake when they land.
    if (dsLive) ds.wakeAhead();
    if (mfLive) mf.wakeAhead();
    bool first = true;
    if (dsLive) {
        copyToRegion(ds, (CUdeviceptr)dptr, pitch, t, first);
        first = false;
    }
    if (mfLive) copyToRegion(mf, (CUdeviceptr)dptr, pitch, t, first);
    g_cu.cuvidUnmapVideoFrame64(dec_, dptr);
    if (stats_.published.fetch_add(1, std::memory_order_relaxed) == 600) tuning::memCheckpoint("after 600 frames");

    if (dumped_ < opt_.dumpFrames && dsLive) {
        const ipc::SlotHeader* s = ipc::slotHeader(ds.base, ds.hdr->latest.load());
        dumpFrame(ipc::slotPayload(ds.base, ds.hdr->latest.load()), s->strideY, s->offsetUV, s->width, s->height, t.seq);
    }
    if (!timing_->push(t)) hotEvent(kEvMapper, Ev::TelemetryRingFull);
}

void Decoder::dumpFrame(const uint8_t* payload, uint32_t strideY, uint32_t offsetUV, uint32_t w, uint32_t h, uint64_t seq) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\frame_%05llu.nv12", opt_.dumpDir.c_str(), (unsigned long long)seq);
    FILE* f = _wfopen(path, L"wb");
    if (!f) return;
    for (uint32_t y = 0; y < h; ++y) fwrite(payload + (size_t)y * strideY, 1, w, f);
    for (uint32_t y = 0; y < h / 2; ++y) fwrite(payload + offsetUV + (size_t)y * strideY, 1, w, f);
    fclose(f);
    ++dumped_;
}

}  // namespace cf
