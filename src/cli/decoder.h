#pragma once
// NVDEC via the CUDA Driver API and the CUVID parser, plus the mapper stage.
//
// decode thread:  one complete AU per cuvidParseVideoData call with CUVID_PKT_ENDOFPICTURE,
//                 ulMaxDisplayDelay = 0. Callbacks fire synchronously inside that call; the
//                 decode callback submits the picture and hands CurrPicIdx to the mapper
//                 (the SDK's force_zero_latency pattern; the stream has no B-frames).
// mapper thread:  cuvidMapVideoFrame64 (blocks until decode completes, so it must not run on
//                 the decode thread) -> DMA straight into the shared-memory slot -> publish.
#include <atomic>
#include <string>
#include <stdio.h>
#include "nvdec_api.h"
#include "frame_types.h"
#include "options.h"
#include "arena.h"
#include "publisher.h"
#include "precise_sleep.h"

namespace cf {

struct DecoderStats {
    std::atomic<uint64_t> decoded{0};
    std::atomic<uint64_t> published{0};
    std::atomic<uint64_t> parseErrors{0};
    std::atomic<uint64_t> decodeErrors{0};
    std::atomic<uint64_t> mapErrors{0};
    std::atomic<uint64_t> picRingDrops{0};
    std::atomic<uint64_t> staleSkipped{0};  // decoded but not published: a newer frame was already queued
    std::atomic<uint64_t> sequenceCallbacks{0};
    std::atomic<uint32_t> width{0}, height{0}, surfaceHeight{0}, pitch{0};  // what the cameras get
    std::atomic<uint32_t> nativeWidth{0}, nativeHeight{0};                  // what the phone sends
    std::atomic<uint32_t> decodeSurfaces{0};
};

class Decoder final {
public:
    ~Decoder();
    bool init(const Options& o, Arena* arena, AuRing* aus, ParamRing* params, TimingRing* timing, Publisher* pub,
              std::string& err);
    // Optional: create the hardware decoder before the first IDR (size known from the sideband).
    bool precreate(uint32_t width, uint32_t height, std::string& err);

    void runDecodeLoop(const std::atomic<bool>& stop);
    // Inline decode (default): the recv thread calls this after the assembler has queued AUs;
    // it parses and submits every queued AU without a thread hand-off.
    void drainAus();
    void runMapperLoop(const std::atomic<bool>& stop);
    // Keeps the GPU out of its idle P-states (and the PCIe link at full speed) between frames.
    // Default: a sleeping one-warp kernel kept resident on the GPU (gpu_kernels.h); the thread
    // only relaunches it every 50 ms. Fallback: the old continuous memsets.
    void runKeepaliveLoop(const std::atomic<bool>& stop);
    // Decode thread only: feed one buffer synchronously (used by the self-test/bench).
    bool decodeBuffer(const uint8_t* p, size_t n, bool endOfPicture);

    CUcontext context() const { return ctx_; }
    const DecoderStats& stats() const { return stats_; }
    // True if GPU work did not finish at shutdown: skip CUDA teardown, let process exit do it.
    bool gpuStuck() const { return gpuStuck_.load(); }
    const char* copyPathName() const;

private:
    static int CUDAAPI onSequence(void* user, CUVIDEOFORMAT* fmt);
    static int CUDAAPI onDecode(void* user, CUVIDPICPARAMS* pp);
    static int CUDAAPI onDisplay(void* user, CUVIDPARSERDISPINFO* di);
    int handleSequence(CUVIDEOFORMAT* fmt);
    int handleDecode(CUVIDPICPARAMS* pp);
    bool createDecoder(const CUVIDEOFORMAT& fmt);
    void processAu(AuItem& au);
    bool keepaliveKernel(const std::atomic<bool>& stop);
    void keepaliveMemset(const std::atomic<bool>& stop);
    // Mapper waits: decode completion and DMA completion (GpuWait).
    void waitDecode(PicItem& item);
    bool waitDma(uint64_t enqueuedQpc);
    void mapAndPublish(PicItem& item);
    bool copyToRegion(Region& r, CUdeviceptr src, uint32_t pitch, FrameTiming& t, bool first);
    void dumpFrame(const uint8_t* payload, uint32_t strideY, uint32_t offsetUV, uint32_t w, uint32_t h, uint64_t seq);

    Options opt_{};
    Arena* arena_ = nullptr;
    AuRing* aus_ = nullptr;
    ParamRing* params_ = nullptr;
    TimingRing* timing_ = nullptr;
    Publisher* pub_ = nullptr;

    CUcontext ctx_ = nullptr;
    CUvideoctxlock lock_ = nullptr;
    CUvideoparser parser_ = nullptr;
    CUvideodecoder dec_ = nullptr;
    CUstream stream_ = nullptr;
    CUevent ev0_ = nullptr, ev1_ = nullptr;
    CUevent evDone_ = nullptr;  // recorded after every DMA; polled or waited on

    // Recent minimum of a duration (QPC ticks): the hybrid wait sleeps until start + min -
    // margin, then polls, so it is off the CPU for most of each GPU wait.
    struct RecentMin {
        uint64_t v[32] = {};
        uint32_t n = 0;
        void add(uint64_t d) { v[n++ & 31] = d; }
        uint64_t min() const {
            const uint32_t k = n < 32 ? n : 32;
            uint64_t m = 0;
            for (uint32_t i = 0; i < k; ++i)
                if (!m || v[i] < m) m = v[i];
            return k >= 4 ? m : 0;  // no prediction until a few samples exist
        }
    };
    RecentMin decodeDur_, dmaDur_;
    PreciseSleeper sleeper_;   // mapper thread only

    // Decoder geometry (written in the sequence callback on the decode thread, read by the
    // mapper only after a picture from that sequence has been handed over).
    uint32_t codedW_ = 0, codedH_ = 0, maxW_ = 0, maxH_ = 0;
    // dispW_/dispH_ is the output (target) size. It is fixed by the first format and never
    // changes afterwards: the cameras have already advertised it, and neither DirectShow graphs
    // nor Frame Server clients renegotiate mid-stream. A later phone format is scaled into it by
    // NVDEC's own scaler (centre crop to fill), so a format switch never stalls a consumer.
    uint32_t dispW_ = 0, dispH_ = 0;
    struct Rect {
        int l = 0, t = 0, r = 0, b = 0;
        bool operator==(const Rect&) const = default;
    };
    Rect srcRect_{};  // source crop currently configured
    Rect cropFor(const CUVIDEOFORMAT& f) const;
    uint32_t numSurfaces_ = 0;
    bool fullRange_ = false;

    // Current AU's timing record, completed by the decode callback.
    FrameTiming cur_{};
    PicRing pics_;
    // Pictures handed to / finished by the mapper. A sequence change waits for them to match so
    // no surface of the old format is mapped after the reconfigure (format switches only).
    std::atomic<uint64_t> picsPushed_{0}, picsMapped_{0};
    void drainMapper();

    // Staging fallback when cuMemHostRegister rejects the file-mapping views.
    uint8_t* staging_ = nullptr;
    size_t stagingBytes_ = 0;
    uint64_t stagingSeq_ = UINT64_MAX;  // AU whose pixels staging_ currently holds

    uint32_t dumped_ = 0;
    std::atomic<bool> gpuStuck_{false};
    std::atomic<uint64_t> lastAuQpc_{0};  // when the decoder last got an AU (keepalive idling)
    DecoderStats stats_;
};

}  // namespace cf
