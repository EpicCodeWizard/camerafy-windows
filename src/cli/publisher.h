#pragma once
// Writer side of the two shared-memory sections.
//   DS: Local\ section created here at startup.
//   MF: Global\ section created by the media source inside the Frame Server; a background
//       thread opens it as soon as it exists (before or after the camera starts) and
//       writes the stream format into it.
// The mapper thread DMAs decoded surfaces directly into these sections when CUDA accepts
// them as registered host memory.
#include <windows.h>
#include <atomic>
#include <string>
#include "ipc.h"
#include "nvdec_api.h"

namespace cf {

struct Region {
    const char* label = "";
    bool isMf = false;
    HANDLE section = nullptr;
    void* base = nullptr;
    ipc::RegionHeader* hdr = nullptr;
    std::atomic<bool> ready{false};      // mapper may write
    std::atomic<bool> registered{false}; // cuMemHostRegister succeeded on every slot's payload
    bool registerTried = false;          // mapper thread only
    size_t registeredBytes = 0;          // per slot, page-rounded
    std::atomic<HANDLE> events[ipc::kMaxReaders] = {};
    std::atomic<bool> anyReader{false};  // a reader heartbeat within the last 2 s

    // Wake-ahead: called when the frame's DMA is queued, so readers (and their copy helpers)
    // are running when it lands instead of spinning between frames (ipc_reader.h).
    void wakeAhead() const {
        const ipc::RegionHeader* h = hdr;
        for (uint32_t i = 0; i < ipc::kMaxReaders; ++i) {
            if (h->readers[i].inUse.load(std::memory_order_relaxed)) {
                if (HANDLE e = events[i].load(std::memory_order_relaxed)) SetEvent(e);
            }
        }
    }
    // After the publish: only readers still blocked (they missed the wake-ahead, e.g. it came
    // while they were busy). Waking an awake reader would leave a stale signal behind.
    void signalSleepers() const {
        const ipc::RegionHeader* h = hdr;
        for (uint32_t i = 0; i < ipc::kMaxReaders; ++i) {
            if (h->readers[i].inUse.load(std::memory_order_relaxed) &&
                h->readers[i].sleeping.load(std::memory_order_seq_cst)) {
                if (HANDLE e = events[i].load(std::memory_order_relaxed)) SetEvent(e);
            }
        }
    }
};

class Publisher final {
public:
    ~Publisher();
    // Creates the DS section (unless disabled) and starts the management thread.
    bool init(bool ds, bool mf, uint32_t readerSpinUs, std::string& info);
    // Registers both views with CUDA (context must be current on the calling thread).
    void attachCuda(CUcontext ctx, bool tryRegister);
    // Mapper thread, CUDA context current: pins the first `bytes` of each slot's payload
    // (once per region; the output size never changes after the first frame).
    void ensureRegistered(Region& r, size_t bytes);
    void setFormat(uint32_t w, uint32_t h, uint32_t fpsNum, uint32_t fpsDen);
    void stop(bool releaseCuda = true);

    Region& ds() { return ds_; }
    Region& mf() { return mf_; }
    bool hostRegisterWorks() const { return hostRegisterWorks_.load(); }
    const std::string& registerReport() const { return registerReport_; }

private:
    static DWORD WINAPI mgmtThreadProc(void* self);
    void mgmtLoop();
    bool openMf();
    void writeFormat(Region& r);

    Region ds_, mf_;
    bool wantDs_ = false, wantMf_ = false;
    uint32_t readerSpinUs_ = 0;
    HANDLE thread_ = nullptr;
    std::atomic<bool> stop_{false};
    CUcontext ctx_ = nullptr;
    bool tryRegister_ = true;
    std::atomic<bool> hostRegisterWorks_{false};
    std::string registerReport_;
    SRWLOCK fmtLock_ = SRWLOCK_INIT;
    uint32_t w_ = 0, h_ = 0, fpsNum_ = 0, fpsDen_ = 0;
    std::atomic<bool> haveFormat_{false};
};

}  // namespace cf
