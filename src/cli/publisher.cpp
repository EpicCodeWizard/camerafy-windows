#include "publisher.h"
#include "console.h"
#include "qpc.h"
#include <stdio.h>

namespace cf {

Publisher::~Publisher() { stop(); }

bool Publisher::init(bool ds, bool mf, uint32_t readerSpinUs, std::string& info) {
    wantDs_ = ds;
    wantMf_ = mf;
    readerSpinUs_ = readerSpinUs;
    ds_.label = "DirectShow";
    mf_.label = "MediaFoundation";
    mf_.isMf = true;
    if (ds) {
        HANDLE s = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                      (DWORD)(ipc::kRegionBytes >> 32), (DWORD)ipc::kRegionBytes, ipc::kDsSectionName);
        const DWORD err = GetLastError();
        if (!s) {
            info = "CreateFileMapping(" + narrow(ipc::kDsSectionName) + ") failed: " + std::to_string(err);
            return false;
        }
        void* v = MapViewOfFile(s, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!v) {
            info = "MapViewOfFile(DS) failed: " + std::to_string(GetLastError());
            CloseHandle(s);
            return false;
        }
        if (err == ERROR_ALREADY_EXISTS && ipc::validRegion(v)) {
            // A DirectShow consumer kept the section alive across a CLI restart; keep its reader
            // claims and just reset the slots.
            auto* h = (ipc::RegionHeader*)v;
            h->latest.store(UINT32_MAX);
            h->creatorPid = GetCurrentProcessId();
        } else {
            ipc::initRegion(v);
        }
        ipc::pretouch(v, ipc::kRegionHeaderBytes);  // payload pages: pinned on first use
        ds_.section = s;
        ds_.base = v;
        ds_.hdr = (ipc::RegionHeader*)v;
        ds_.hdr->readerSpinUs.store(readerSpinUs);
        ds_.hdr->writerAlive.store(1);
        for (uint32_t i = 0; i < ipc::kMaxReaders; ++i) {
            wchar_t name[96];
            swprintf(name, 96, ipc::kDsEventFmt, i);
            ds_.events[i] = CreateEventW(nullptr, FALSE, FALSE, name);
        }
        info = "DS section " + narrow(ipc::kDsSectionName) + " " + std::to_string(ipc::kRegionBytes >> 20) +
               " MB reserved (only the bytes a frame uses are ever resident)";
    }
    stop_ = false;
    thread_ = CreateThread(nullptr, 0, mgmtThreadProc, this, 0, nullptr);
    return true;
}

void Publisher::attachCuda(CUcontext ctx, bool tryRegister) {
    ctx_ = ctx;
    tryRegister_ = tryRegister;
    if (ds_.base) ds_.ready.store(true, std::memory_order_release);
}

// The PRD flags this as unverified: does cuMemHostRegister accept a pagefile-backed
// file-mapping view? Try it, report, and fall back to a pinned staging buffer if not.
// Only the bytes a frame uses are pinned (1080p NV12: ~3.3 MB per slot), not the 32 MB slot
// capacity: pinned pages are resident RAM for the whole run.
void Publisher::ensureRegistered(Region& r, size_t bytes) {
    if (r.registerTried || !r.base) return;
    r.registerTried = true;
    if (!tryRegister_) {
        registerReport_ += std::string(r.label) + ": host registration disabled (--no-host-register); staging path. ";
        return;
    }
    const size_t len = (bytes + ipc::kPage - 1) & ~(size_t)(ipc::kPage - 1);
    const uint64_t t0 = qpc();
    CUresult res = CUDA_SUCCESS;
    uint32_t done = 0;
    for (; done < ipc::kSlotCount && res == CUDA_SUCCESS; ++done)
        res = g_cu.cuMemHostRegister(ipc::slotPayload(r.base, done), len, CU_MEMHOSTREGISTER_PORTABLE);
    if (res != CUDA_SUCCESS) {
        for (uint32_t i = 0; i + 1 < done; ++i) g_cu.cuMemHostUnregister(ipc::slotPayload(r.base, i));
    }
    const double ms = ticksToUs((int64_t)(qpc() - t0)) / 1000.0;
    char b[512];
    if (res == CUDA_SUCCESS) {
        r.registeredBytes = len;
        r.registered.store(true);
        hostRegisterWorks_.store(true);
        snprintf(b, sizeof(b), "%s: cuMemHostRegister on the file-mapping view OK (%u slots x %.1f MB, %.1f ms): DMA "
                 "lands directly in the IPC slot. ", r.label, ipc::kSlotCount, len / 1048576.0, ms);
    } else {
        snprintf(b, sizeof(b), "%s: cuMemHostRegister on the file-mapping view FAILED: %s; using pinned staging + memcpy. ",
                 r.label, g_cu.errorText(res).c_str());
    }
    registerReport_ += b;
    logInfo("  %s", b);
}

void Publisher::writeFormat(Region& r) {
    if (!r.hdr || !haveFormat_.load()) return;
    AcquireSRWLockShared(&fmtLock_);
    r.hdr->width = w_;
    r.hdr->height = h_;
    r.hdr->fpsNum = fpsNum_;
    r.hdr->fpsDen = fpsDen_;
    ReleaseSRWLockShared(&fmtLock_);
    r.hdr->formatSeq.fetch_add(1, std::memory_order_release);
}

void Publisher::setFormat(uint32_t w, uint32_t h, uint32_t fpsNum, uint32_t fpsDen) {
    AcquireSRWLockExclusive(&fmtLock_);
    w_ = w;
    h_ = h;
    fpsNum_ = fpsNum;
    fpsDen_ = fpsDen;
    ReleaseSRWLockExclusive(&fmtLock_);
    haveFormat_.store(true);
    writeFormat(ds_);
    if (mf_.hdr) writeFormat(mf_);
}

bool Publisher::openMf() {
    HANDLE s = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, ipc::mfSectionName());
    if (!s) return false;
    void* v = MapViewOfFile(s, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (!v || !ipc::validRegion(v)) {
        if (v) UnmapViewOfFile(v);
        CloseHandle(s);
        logWarn("Global MF section exists but has an unexpected layout (stale camerafy_mf.dll in the Frame Server?)");
        return false;
    }
    ipc::pretouch(v, ipc::kRegionHeaderBytes);  // payload pages: pinned on first use
    mf_.section = s;
    mf_.base = v;
    mf_.hdr = (ipc::RegionHeader*)v;
    mf_.hdr->readerSpinUs.store(readerSpinUs_);
    mf_.hdr->writerAlive.store(1);
    writeFormat(mf_);
    if (HANDLE fmt = OpenEventW(EVENT_MODIFY_STATE, FALSE, ipc::mfFormatReadyName())) {
        SetEvent(fmt);
        CloseHandle(fmt);
    }

    mf_.ready.store(true, std::memory_order_release);
    logInfo("Opened the media source's %s (created by pid %u)", narrow(ipc::mfSectionName()).c_str(), mf_.hdr->creatorPid);
    return true;
}

DWORD WINAPI Publisher::mgmtThreadProc(void* self) {
    ((Publisher*)self)->mgmtLoop();
    return 0;
}

// Off the hot path, ~1 kHz while waiting for the MF section (so the source's format wait
// is short), 10 Hz otherwise: reap dead readers, open new readers' events, heartbeat.
void Publisher::mgmtLoop() {
    while (!stop_.load()) {
        if (wantMf_ && !mf_.base && haveFormat_.load()) openMf();
        for (Region* r : {&ds_, &mf_}) {
            if (!r->hdr) continue;
            ipc::reapDeadReaders(r->hdr);
            const uint64_t now = qpc();
            bool any = false;
            for (uint32_t i = 0; i < ipc::kMaxReaders; ++i) {
                auto& rd = r->hdr->readers[i];
                if (!rd.inUse.load(std::memory_order_acquire)) continue;
                if (now - rd.heartbeatQpc.load(std::memory_order_relaxed) < (uint64_t)usToTicks(2e6)) any = true;
                if (!r->events[i].load()) {
                    wchar_t name[96];
                    swprintf(name, 96, r->isMf ? ipc::mfEventFmt() : ipc::kDsEventFmt, i);
                    if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, name)) r->events[i].store(e);
                }
            }
            r->anyReader.store(any, std::memory_order_relaxed);
            r->hdr->writerHeartbeatQpc.store(now, std::memory_order_relaxed);
        }
        Sleep((wantMf_ && !mf_.base) ? 1 : 100);
    }
}

void Publisher::stop(bool releaseCuda) {
    if (stop_.exchange(true)) return;
    if (thread_) {
        WaitForSingleObject(thread_, 2000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    for (Region* r : {&ds_, &mf_}) {
        r->ready.store(false);
        if (r->hdr) r->hdr->writerAlive.store(0);
        if (r->registered.load() && ctx_ && releaseCuda) {
            g_cu.cuCtxPushCurrent(ctx_);
            for (uint32_t i = 0; i < ipc::kSlotCount; ++i) g_cu.cuMemHostUnregister(ipc::slotPayload(r->base, i));
            CUcontext d;
            g_cu.cuCtxPopCurrent(&d);
        }
        for (auto& e : r->events)
            if (HANDLE h = e.exchange(nullptr)) CloseHandle(h);
        if (r->base) UnmapViewOfFile(r->base);
        if (r->section) CloseHandle(r->section);
        r->base = nullptr;
        r->hdr = nullptr;
        r->section = nullptr;
    }
}

}  // namespace cf
