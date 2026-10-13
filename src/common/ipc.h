#pragma once
// Shared-memory frame exchange between camerafy.exe and the two camera DLLs.
//
// One layout, two sections:
//   Local\Camerafy.DS.v4   created by the CLI, read in-process by the DirectShow filter.
//   Global\Camerafy.MF.v4  created by the MF media source inside the Frame Server
//                          (session 0, LocalService); opened by the CLI.
//
// Each slot's payload is laid out exactly like the NVDEC output surface (pitch-linear
// NV12, UV plane at pitch*surfaceHeight), so one linear DMA fills it. Readers convert
// to their own stride while copying into their sample buffers, which they must do anyway.
#include <windows.h>
#include <atomic>
#include <stdint.h>
#include <string.h>
#include <intrin.h>

namespace cf::ipc {

inline constexpr uint32_t kMagic = 0x43414D46;  // 'CAMF'
inline constexpr uint32_t kVersion = 4;
inline constexpr uint32_t kPage = 4096;
inline constexpr uint32_t kSlotCount = 4;
inline constexpr uint32_t kMaxReaders = 8;
// Enough for NVDEC's H.264 maximum, 4096x4096 at a 4096 pitch (24 MB), e.g. the iPhone's
// 4032x3024 photo-size format (18.6 MB). v2 held 16 MB and silently dropped such frames.
inline constexpr uint32_t kPayloadCapacity = 32u << 20;
inline constexpr uint32_t kSlotHeaderBytes = kPage;
inline constexpr uint32_t kSlotStride = kSlotHeaderBytes + kPayloadCapacity;
inline constexpr uint32_t kRegionHeaderBytes = 4 * kPage;  // header + per-reader consume logs
inline constexpr uint32_t kConsumeLogSize = 64;           // power of two
inline constexpr uint64_t kRegionBytes = (uint64_t)kRegionHeaderBytes + (uint64_t)kSlotCount * kSlotStride;

inline constexpr wchar_t kDsSectionName[] = L"Local\\Camerafy.DS.v4";
inline constexpr wchar_t kDsEventFmt[] = L"Local\\Camerafy.DS.v4.evt.%u";
inline constexpr wchar_t kMfSectionName[] = L"Global\\Camerafy.MF.v4";
inline constexpr wchar_t kMfEventFmt[] = L"Global\\Camerafy.MF.v4.evt.%u";
// Signalled by the CLI after it has written the format into a freshly created MF section.
inline constexpr wchar_t kMfFormatReadyName[] = L"Global\\Camerafy.MF.v4.fmt";

// Test hook: with CAMERAFY_MF_LOCAL_TEST set, the MF names move to Local\ so the media source
// can be driven in-process by --self-test without the Frame Server (and without the
// SeCreateGlobalPrivilege that creating Global\ objects needs). Never set in normal use.
inline bool mfLocalTest() {
    wchar_t b[4];
    return GetEnvironmentVariableW(L"CAMERAFY_MF_LOCAL_TEST", b, 4) > 0;
}
inline const wchar_t* mfSectionName() { return mfLocalTest() ? L"Local\\Camerafy.MFT.v4" : kMfSectionName; }
inline const wchar_t* mfEventFmt() { return mfLocalTest() ? L"Local\\Camerafy.MFT.v4.evt.%u" : kMfEventFmt; }
inline const wchar_t* mfFormatReadyName() { return mfLocalTest() ? L"Local\\Camerafy.MFT.v4.fmt" : kMfFormatReadyName; }

enum SlotFlags : uint32_t {
    kFlagIdr = 1u << 0,
    kFlagFullRange = 1u << 1,
};

struct alignas(64) SlotHeader {
    std::atomic<uint64_t> seq;  // odd = write in progress
    uint64_t frameId;
    uint64_t phoneCaptureUs;
    uint64_t phoneEncodeOutUs;
    uint64_t winRecvQpc;
    uint64_t winAuCompleteQpc;
    uint64_t winDecodedQpc;
    uint64_t winPublishedQpc;
    uint32_t width, height;     // display size
    uint32_t strideY, strideUV; // bytes per row in this slot
    uint32_t offsetUV;          // from payload start
    uint32_t payloadBytes;
    uint32_t flags;
    uint32_t pad0;
    alignas(64) std::atomic<uint32_t> readers;  // readers currently copying this slot
};
static_assert(sizeof(SlotHeader) <= kSlotHeaderBytes);

struct alignas(64) ReaderSlot {
    std::atomic<uint32_t> inUse;  // 0 free, 1 claimed
    uint32_t pid;
    uint32_t kind;                // 1 = DirectShow, 2 = Media Foundation
    // 1 while the reader is blocked on its event. The writer sets the event after a publish
    // only then; a reader that is awake (spinning after the wake-ahead signal) would otherwise
    // find a stale signal at its next wait and spin a whole window for nothing.
    std::atomic<uint32_t> sleeping;
    std::atomic<uint64_t> heartbeatQpc;
    std::atomic<uint64_t> lastFrameId;
    std::atomic<uint64_t> lastPublishedQpc;  // winPublishedQpc of the frame last consumed
    std::atomic<uint64_t> lastConsumedQpc;   // when that frame was handed downstream
    std::atomic<uint64_t> consumedCount;
    std::atomic<uint64_t> retries;           // seqlock retries
};
static_assert(sizeof(ReaderSlot) == 64);

struct alignas(64) RegionHeader {
    uint32_t magic, version;
    uint32_t slotCount, slotStride, slotHeaderBytes, payloadCapacity;
    uint32_t creatorPid;
    uint32_t pad0;

    // Current stream format, written by the CLI. formatSeq bumps on every change.
    alignas(64) std::atomic<uint32_t> formatSeq;
    uint32_t width, height, fpsNum, fpsDen;
    uint32_t pad1[3];

    alignas(64) std::atomic<uint32_t> latest;     // newest complete slot
    std::atomic<uint32_t> writerAlive;
    std::atomic<uint64_t> publishCount;           // readers spin on this
    std::atomic<uint64_t> writerHeartbeatQpc;
    std::atomic<uint64_t> lastPublishQpc;
    std::atomic<uint64_t> frameIntervalQpc;       // smoothed, for reader spin prediction
    std::atomic<uint32_t> readerSpinUs;           // bounded spin after a wake-ahead signal

    alignas(64) ReaderSlot readers[kMaxReaders];

    // Per-reader log of (published, consumed) QPC pairs, so the CLI's 10 Hz reporter sees
    // every frame's consumption latency rather than a sample.
    struct ConsumeEntry {
        uint64_t publishedQpc;
        uint64_t consumedQpc;
    };
    struct alignas(64) ConsumeLog {
        std::atomic<uint32_t> head;
        uint32_t pad[15];
        ConsumeEntry e[kConsumeLogSize];
    };
    ConsumeLog consume[kMaxReaders];
};
static_assert(sizeof(RegionHeader) <= kRegionHeaderBytes);

inline SlotHeader* slotHeader(void* base, uint32_t i) {
    return (SlotHeader*)((uint8_t*)base + kRegionHeaderBytes + (uint64_t)i * kSlotStride);
}
inline uint8_t* slotPayload(void* base, uint32_t i) {
    return (uint8_t*)base + kRegionHeaderBytes + (uint64_t)i * kSlotStride + kSlotHeaderBytes;
}

inline void initRegion(void* base) {
    memset(base, 0, kRegionHeaderBytes);
    auto* h = (RegionHeader*)base;
    h->magic = kMagic;
    h->version = kVersion;
    h->slotCount = kSlotCount;
    h->slotStride = kSlotStride;
    h->slotHeaderBytes = kSlotHeaderBytes;
    h->payloadCapacity = kPayloadCapacity;
    h->creatorPid = GetCurrentProcessId();
    h->latest.store(UINT32_MAX, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kSlotCount; ++i) memset(slotHeader(base, i), 0, sizeof(SlotHeader));
    std::atomic_thread_fence(std::memory_order_release);
}

inline bool validRegion(const void* base) {
    auto* h = (const RegionHeader*)base;
    return h->magic == kMagic && h->version == kVersion && h->slotCount == kSlotCount &&
           h->slotStride == kSlotStride;
}

// Pre-touch every page so the first frames never take a soft fault.
inline void pretouch(void* base, uint64_t bytes) {
    volatile uint8_t* p = (volatile uint8_t*)base;
    for (uint64_t o = 0; o < bytes; o += kPage) p[o] = p[o];
}

// ---------------------------------------------------------------- writer side

// Picks a slot that is neither `latest` nor being read. Never blocks.
inline uint32_t pickWriteSlot(RegionHeader* h, void* base) {
    const uint32_t latest = h->latest.load(std::memory_order_relaxed);
    const uint32_t start = (latest == UINT32_MAX) ? 0 : latest + 1;
    for (uint32_t k = 0; k < kSlotCount; ++k) {
        const uint32_t c = (start + k) % kSlotCount;
        if (c == latest) continue;
        if (slotHeader(base, c)->readers.load(std::memory_order_acquire) == 0) return c;
    }
    // Every other slot is mid-read (only possible with >= 3 stalled readers); overwrite the
    // oldest anyway. The seqlock makes those readers retry.
    return start % kSlotCount;
}

inline void beginWrite(SlotHeader* s) {
    const uint64_t q = s->seq.load(std::memory_order_relaxed);
    s->seq.store(q | 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);  // odd seq visible before payload writes
}

inline void endWrite(SlotHeader* s) {
    const uint64_t q = s->seq.load(std::memory_order_relaxed);
    s->seq.store((q | 1) + 1, std::memory_order_release);
}

inline void publish(RegionHeader* h, uint32_t slot, uint64_t nowQpc) {
    h->latest.store(slot, std::memory_order_release);
    const uint64_t last = h->lastPublishQpc.load(std::memory_order_relaxed);
    if (last) {
        const uint64_t prev = h->frameIntervalQpc.load(std::memory_order_relaxed);
        const uint64_t d = nowQpc - last;
        h->frameIntervalQpc.store(prev ? (prev * 7 + d) / 8 : d, std::memory_order_relaxed);
    }
    h->lastPublishQpc.store(nowQpc, std::memory_order_relaxed);
    h->writerHeartbeatQpc.store(nowQpc, std::memory_order_relaxed);
    // seq_cst: pairs with the readers' "sleeping" mark (see Region::signalSleepers).
    h->publishCount.fetch_add(1, std::memory_order_seq_cst);
}

// ---------------------------------------------------------------- reader side

inline int claimReader(RegionHeader* h, uint32_t kind) {
    for (uint32_t i = 0; i < kMaxReaders; ++i) {
        uint32_t expected = 0;
        auto& r = h->readers[i];
        if (r.inUse.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
            r.pid = GetCurrentProcessId();
            r.kind = kind;
            r.sleeping.store(0, std::memory_order_relaxed);
            r.lastFrameId.store(0, std::memory_order_relaxed);
            r.consumedCount.store(0, std::memory_order_relaxed);
            r.retries.store(0, std::memory_order_relaxed);
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            r.heartbeatQpc.store((uint64_t)t.QuadPart, std::memory_order_release);
            return (int)i;
        }
    }
    return -1;
}

// Frees reader slots whose owner process has exited (crashed consumers never release).
inline void reapDeadReaders(RegionHeader* h) {
    for (uint32_t i = 0; i < kMaxReaders; ++i) {
        auto& r = h->readers[i];
        if (r.inUse.load(std::memory_order_acquire) == 0 || r.pid == 0) continue;
        HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, r.pid);
        bool dead = (p == nullptr && GetLastError() == ERROR_INVALID_PARAMETER);
        if (p) {
            dead = WaitForSingleObject(p, 0) == WAIT_OBJECT_0;
            CloseHandle(p);
        }
        if (dead) r.inUse.store(0, std::memory_order_release);
    }
}

inline void releaseReader(RegionHeader* h, int idx) {
    if (idx >= 0) h->readers[idx].inUse.store(0, std::memory_order_release);
}

struct FrameInfo {
    uint64_t frameId;
    uint64_t phoneCaptureUs;
    uint64_t winRecvQpc, winAuCompleteQpc, winDecodedQpc, winPublishedQpc;
    uint32_t width, height, flags;
};

// Seqlock read of the newest slot. `copy(payload, slotHeader)` performs the actual copy
// into the caller's buffer and must tolerate torn data (it is discarded on retry).
// Returns false if no frame has ever been published or the retry budget is exhausted.
template <typename CopyFn>
inline bool readLatest(RegionHeader* h, void* base, FrameInfo& info, CopyFn&& copy, uint64_t* retries = nullptr) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t idx = h->latest.load(std::memory_order_acquire);
        if (idx >= kSlotCount) return false;
        SlotHeader* s = slotHeader(base, idx);
        s->readers.fetch_add(1, std::memory_order_acq_rel);
        const uint64_t q1 = s->seq.load(std::memory_order_acquire);
        if (q1 & 1) {
            s->readers.fetch_sub(1, std::memory_order_release);
            if (retries) ++*retries;
            _mm_pause();
            continue;
        }
        info.frameId = s->frameId;
        info.phoneCaptureUs = s->phoneCaptureUs;
        info.winRecvQpc = s->winRecvQpc;
        info.winAuCompleteQpc = s->winAuCompleteQpc;
        info.winDecodedQpc = s->winDecodedQpc;
        info.winPublishedQpc = s->winPublishedQpc;
        info.width = s->width;
        info.height = s->height;
        info.flags = s->flags;
        copy(slotPayload(base, idx), *s);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t q2 = s->seq.load(std::memory_order_relaxed);
        s->readers.fetch_sub(1, std::memory_order_release);
        if (q1 == q2) return true;
        if (retries) ++*retries;
    }
    return false;
}

}  // namespace cf::ipc
