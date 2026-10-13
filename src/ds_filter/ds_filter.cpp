// DirectShow source filter: "<phone> Webcam" (the fast camera).
// Runs in-process in each consumer, so a frame crosses one process boundary.
#include "ds_filter.h"
#include "ids.h"
#include "ipc_reader.h"
#include "diag.h"
#include <stdio.h>

using namespace cf;

namespace {

constexpr uint32_t kDefaultW = 1920, kDefaultH = 1080, kDefaultFpsNum = 30, kDefaultFpsDen = 1;

DWORD envDword(const wchar_t* name, DWORD def) {
    wchar_t buf[32];
    DWORD n = GetEnvironmentVariableW(name, buf, 32);
    if (!n || n >= 32) return def;
    return (DWORD)_wtoi(buf);
}

// Reads the stream format the CLI has published, without claiming a reader slot.
bool peekShmFormat(uint32_t& w, uint32_t& h, uint32_t& fn, uint32_t& fd) {
    HANDLE s = OpenFileMappingW(FILE_MAP_READ, FALSE, ipc::kDsSectionName);
    if (!s) return false;
    bool ok = false;
    if (void* v = MapViewOfFile(s, FILE_MAP_READ, 0, 0, ipc::kRegionHeaderBytes)) {
        auto* hdr = (const ipc::RegionHeader*)v;
        if (ipc::validRegion(v) && hdr->formatSeq.load(std::memory_order_acquire) && hdr->width && hdr->height) {
            w = hdr->width;
            h = hdr->height;
            fn = hdr->fpsNum ? hdr->fpsNum : kDefaultFpsNum;
            fd = hdr->fpsDen ? hdr->fpsDen : kDefaultFpsDen;
            ok = true;
        }
        UnmapViewOfFile(v);
    }
    CloseHandle(s);
    return ok;
}

}  // namespace

// ================================================================ filter

CUnknown* WINAPI CamerafySource::CreateInstance(LPUNKNOWN unk, HRESULT* phr) {
    auto* f = new CamerafySource(unk, phr);
    if (!f && phr) *phr = E_OUTOFMEMORY;
    return f;
}

CamerafySource::CamerafySource(LPUNKNOWN unk, HRESULT* phr)
    : CSource(NAME("Camerafy DirectShow source"), unk, ids::kDsFilterClsid) {
    pin_ = new CamerafyPin(phr, this);
    if (!pin_ && phr) *phr = E_OUTOFMEMORY;
    dtrace("cf-ds", "filter created");
}

CamerafySource::~CamerafySource() { dtrace("cf-ds", "filter destroyed"); }

STDMETHODIMP CamerafySource::NonDelegatingQueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IAMFilterMiscFlags) return GetInterface((IAMFilterMiscFlags*)this, ppv);
    return CSource::NonDelegatingQueryInterface(riid, ppv);
}

STDMETHODIMP CamerafySource::GetState(DWORD ms, FILTER_STATE* state) {
    HRESULT hr = CSource::GetState(ms, state);
    if (SUCCEEDED(hr) && *state == State_Paused) return VFW_S_CANT_CUE;
    return hr;
}

// ================================================================ pin

CamerafyPin::CamerafyPin(HRESULT* phr, CamerafySource* filter)
    : CSourceStream(NAME("Camerafy capture pin"), phr, filter, L"Capture"), filter_(filter) {}

CamerafyPin::~CamerafyPin() { detach(); }

STDMETHODIMP CamerafyPin::NonDelegatingQueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IAMStreamConfig) return GetInterface((IAMStreamConfig*)this, ppv);
    if (riid == IID_IKsPropertySet) return GetInterface((IKsPropertySet*)this, ppv);
    if (riid == IID_IAMPushSource) return GetInterface((IAMPushSource*)this, ppv);
    if (riid == IID_IAMLatency) return GetInterface((IAMLatency*)(IAMPushSource*)this, ppv);
    return CSourceStream::NonDelegatingQueryInterface(riid, ppv);
}

void CamerafyPin::currentSourceFormat(Format& f) {
    if (!peekShmFormat(f.width, f.height, f.fpsNum, f.fpsDen)) {
        f = {kDefaultW, kDefaultH, kDefaultFpsNum, kDefaultFpsDen};
    }
}

// index 0 = NV12, 1 = YUY2 (for older apps)
HRESULT CamerafyPin::buildType(int index, const Format& f, CMediaType* mt) {
    if (index < 0 || index > 1) return VFW_S_NO_MORE_ITEMS;
    auto* vih = (VIDEOINFOHEADER*)mt->AllocFormatBuffer(sizeof(VIDEOINFOHEADER));
    if (!vih) return E_OUTOFMEMORY;
    ZeroMemory(vih, sizeof(VIDEOINFOHEADER));
    const bool yuy2 = index == 1;
    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = (LONG)f.width;
    vih->bmiHeader.biHeight = (LONG)f.height;
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biBitCount = yuy2 ? 16 : 12;
    vih->bmiHeader.biCompression = yuy2 ? MAKEFOURCC('Y', 'U', 'Y', '2') : MAKEFOURCC('N', 'V', '1', '2');
    vih->bmiHeader.biSizeImage = yuy2 ? f.width * f.height * 2 : f.width * f.height * 3 / 2;
    vih->AvgTimePerFrame = (REFERENCE_TIME)(10000000ull * f.fpsDen / f.fpsNum);
    vih->dwBitRate = (DWORD)((uint64_t)vih->bmiHeader.biSizeImage * 8 * f.fpsNum / f.fpsDen);
    SetRectEmpty(&vih->rcSource);
    SetRectEmpty(&vih->rcTarget);
    mt->SetType(&MEDIATYPE_Video);
    mt->SetSubtype(yuy2 ? &MEDIASUBTYPE_YUY2 : &MEDIASUBTYPE_NV12);
    mt->SetFormatType(&FORMAT_VideoInfo);
    mt->SetTemporalCompression(FALSE);
    mt->SetSampleSize(vih->bmiHeader.biSizeImage);
    return S_OK;
}

HRESULT CamerafyPin::GetMediaType(int pos, CMediaType* mt) {
    CAutoLock lock(&formatLock_);
    if (hasForced_) {
        if (pos != 0) return VFW_S_NO_MORE_ITEMS;
        *mt = forced_;
        return S_OK;
    }
    Format f;
    currentSourceFormat(f);
    return buildType(pos, f, mt);
}

HRESULT CamerafyPin::CheckMediaType(const CMediaType* mt) {
    if (!mt || *mt->Type() != MEDIATYPE_Video || *mt->FormatType() != FORMAT_VideoInfo || !mt->Format() ||
        mt->FormatLength() < sizeof(VIDEOINFOHEADER))
        return E_INVALIDARG;
    const GUID& sub = *mt->Subtype();
    if (sub != MEDIASUBTYPE_NV12 && sub != MEDIASUBTYPE_YUY2) return E_INVALIDARG;
    auto* vih = (const VIDEOINFOHEADER*)mt->Format();
    Format f;
    currentSourceFormat(f);
    if (vih->bmiHeader.biWidth != (LONG)f.width || labs(vih->bmiHeader.biHeight) != (LONG)f.height)
        return E_INVALIDARG;
    return S_OK;
}

HRESULT CamerafyPin::SetMediaType(const CMediaType* mt) {
    HRESULT hr = CSourceStream::SetMediaType(mt);
    if (FAILED(hr)) return hr;
    auto* vih = (const VIDEOINFOHEADER*)mt->Format();
    yuy2_ = *mt->Subtype() == MEDIASUBTYPE_YUY2;
    outW_ = (uint32_t)vih->bmiHeader.biWidth;
    outH_ = (uint32_t)labs(vih->bmiHeader.biHeight);
    outStride_ = yuy2_ ? outW_ * 2 : outW_;
    frameDur_ = vih->AvgTimePerFrame ? vih->AvgTimePerFrame : 333333;
    dtrace("cf-ds", "media type %s %ux%u", yuy2_ ? "YUY2" : "NV12", outW_, outH_);
    return S_OK;
}

HRESULT CamerafyPin::DecideBufferSize(IMemAllocator* alloc, ALLOCATOR_PROPERTIES* req) {
    auto* vih = (const VIDEOINFOHEADER*)m_mt.Format();
    // One buffer: downstream cannot queue frames behind the newest one.
    req->cBuffers = (long)envDword(L"CAMERAFY_DS_BUFFERS", 1);
    if (req->cBuffers < 1) req->cBuffers = 1;
    req->cbBuffer = (long)vih->bmiHeader.biSizeImage;
    if (req->cbAlign < 1) req->cbAlign = 1;
    ALLOCATOR_PROPERTIES actual{};
    HRESULT hr = alloc->SetProperties(req, &actual);
    if (FAILED(hr)) return hr;
    if (actual.cbBuffer < req->cbBuffer) return E_FAIL;
    dtrace("cf-ds", "allocator: %ld buffers of %ld bytes", actual.cBuffers, actual.cbBuffer);
    return S_OK;
}

bool CamerafyPin::attach() {
    if (hdr_) return true;
    const uint64_t now = cf::qpc();
    if (lastAttachTry_ && now - lastAttachTry_ < (uint64_t)cf::usToTicks(250000)) return false;
    lastAttachTry_ = now;
    section_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, ipc::kDsSectionName);
    if (!section_) return false;
    base_ = MapViewOfFile(section_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (!base_ || !ipc::validRegion(base_)) {
        detach();
        return false;
    }
    hdr_ = (ipc::RegionHeader*)base_;
    readerIdx_ = ipc::claimReader(hdr_, 1);
    if (readerIdx_ < 0) {
        dtrace("cf-ds", "all %u reader slots busy", ipc::kMaxReaders);
        detach();
        return false;
    }
    wchar_t name[96];
    swprintf(name, 96, ipc::kDsEventFmt, (unsigned)readerIdx_);
    event_ = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
    if (!event_) event_ = CreateEventW(nullptr, FALSE, FALSE, name);
    lastSeen_ = hdr_->publishCount.load(std::memory_order_acquire);
    // A frame published before we attached is still the newest one; take it immediately.
    if (hdr_->latest.load(std::memory_order_acquire) < ipc::kSlotCount && lastSeen_) --lastSeen_;
    dtrace("cf-ds", "attached as reader %d", readerIdx_);
    return true;
}

void CamerafyPin::detach() {
    if (hdr_ && readerIdx_ >= 0) ipc::releaseReader(hdr_, readerIdx_);
    readerIdx_ = -1;
    hdr_ = nullptr;
    if (event_) CloseHandle(event_);
    event_ = nullptr;
    if (base_) UnmapViewOfFile(base_);
    base_ = nullptr;
    if (section_) CloseHandle(section_);
    section_ = nullptr;
}

HRESULT CamerafyPin::OnThreadCreate() {
    // The worker thread is ours for the life of the stream; give it the scheduler's attention.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    copier_.start(0);
    first_ = true;
    lastStart_ = 0;
    synthetic_ = 0;
    attach();
    return S_OK;
}

HRESULT CamerafyPin::OnThreadDestroy() {
    copier_.stop();
    detach();
    return S_OK;
}

HRESULT CamerafyPin::OnThreadStartPlay() {
    first_ = true;
    return S_OK;
}

void CamerafyPin::stampAndFinish(IMediaSample* sample) {
    REFERENCE_TIME start = 0;
    if (!(filter_->HasClock() && SUCCEEDED(filter_->CurrentStreamTime(&start)))) start = synthetic_;
    if (start <= lastStart_ && !first_) start = lastStart_ + 1;
    REFERENCE_TIME stop = start + frameDur_;
    lastStart_ = start;
    synthetic_ = stop;
    sample->SetTime(&start, &stop);
    sample->SetMediaTime(nullptr, nullptr);
    sample->SetSyncPoint(TRUE);
    sample->SetDiscontinuity(first_ ? TRUE : FALSE);
    sample->SetPreroll(FALSE);
    first_ = false;
}

HRESULT CamerafyPin::FillBuffer(IMediaSample* sample) {
    // DirectShow's streaming thread: real-time band on its first frame (ipc_reader.h).
    static thread_local bool raised = false;
    if (!raised) {
        ipc::raiseCameraThread();
        raised = true;
    }
    BYTE* p = nullptr;
    if (FAILED(sample->GetPointer(&p)) || !p) return E_POINTER;
    const long cb = sample->GetSize();
    const long need = (long)(yuy2_ ? outStride_ * outH_ : outW_ * outH_ * 3 / 2);
    if (cb < need) return E_FAIL;
    sample->SetActualDataLength(need);

    if (!attach()) {
        // No CLI running: emit black at the negotiated rate so apps keep the device open.
        if (!haveFrame_) {
            if (yuy2_) {
                for (uint32_t y = 0; y < outH_; ++y)
                    for (uint32_t x = 0; x < outW_; x += 2) {
                        BYTE* d = p + (size_t)y * outStride_ + x * 2;
                        d[0] = 16; d[1] = 128; d[2] = 16; d[3] = 128;
                    }
            } else {
                ipc::fillBlackNv12(p, outW_, p + (size_t)outW_ * outH_, outW_, outW_, outH_);
            }
        }
        Sleep((DWORD)(frameDur_ / 10000));
        stampAndFinish(sample);
        return S_OK;
    }

    // Wait for the phone's next frame. The poll timeout only exists so a stop request
    // can interrupt us; it never paces delivery. While the phone is paused (PiP closed, camera
    // taken by another app) the latest frame is repeated every 500 ms, as the
    // MF source does, so apps that watch for a stalled camera keep the device open.
    const uint64_t waitStart = cf::qpc();
    bool repeat = false;
    while (true) {
        const auto r = ipc::waitForFrame(hdr_, readerIdx_, lastSeen_, event_, 50, [&] { copier_.prepare(); });
        if (r == ipc::WaitResult::Frame) break;
        if (haveFrame_ && cf::ticksToUs((int64_t)(cf::qpc() - waitStart)) > 500000.0) {
            repeat = true;
            break;
        }
        if (CheckRequest(nullptr)) {
            // Stop/pause pending: hand back whatever is in the buffer so the thread can exit.
            stampAndFinish(sample);
            return S_OK;
        }
        ipc::heartbeat(hdr_, readerIdx_);
    }
    lastSeen_ = hdr_->publishCount.load(std::memory_order_acquire);

    ipc::FrameInfo fi{};
    const bool yuy2 = yuy2_;
    const uint32_t w = outW_, h = outH_, stride = outStride_;
    const bool ok = ipc::readLatest(hdr_, base_, fi,
        [&](const uint8_t* src, const ipc::SlotHeader& s) {
            if (yuy2) copier_.nv12ToYuy2(src, s, p, stride, w, h);
            else copier_.copyNv12(src, s, p, w, p + (size_t)w * h, w, w, h);
        },
        &retries_);
    if (ok) {
        haveFrame_ = true;
        if (!repeat) ipc::recordConsumed(hdr_, readerIdx_, fi, retries_);  // repeats are not latency samples
    }
    stampAndFinish(sample);
    return S_OK;
}

// ---------------------------------------------------------------- IAMStreamConfig

STDMETHODIMP CamerafyPin::SetFormat(AM_MEDIA_TYPE* pmt) {
    if (!pmt) {
        CAutoLock lock(&formatLock_);
        hasForced_ = false;
        return S_OK;
    }
    CMediaType mt(*pmt);
    HRESULT hr = CheckMediaType(&mt);
    if (FAILED(hr)) return hr;
    {
        CAutoLock lock(&formatLock_);
        forced_ = mt;
        hasForced_ = true;
    }
    if (IsConnected()) {
        if (IFilterGraph* g = filter_->GetFilterGraph()) return g->Reconnect(this);
    }
    return S_OK;
}

STDMETHODIMP CamerafyPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    CMediaType mt;
    if (IsConnected()) {
        mt = m_mt;
    } else {
        HRESULT hr = GetMediaType(0, &mt);
        if (FAILED(hr)) return hr;
    }
    *ppmt = CreateMediaType(&mt);
    return *ppmt ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CamerafyPin::GetNumberOfCapabilities(int* count, int* size) {
    if (!count || !size) return E_POINTER;
    *count = 2;
    *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP CamerafyPin::GetStreamCaps(int index, AM_MEDIA_TYPE** ppmt, BYTE* capsBytes) {
    if (!ppmt || !capsBytes) return E_POINTER;
    Format f;
    currentSourceFormat(f);
    CMediaType mt;
    HRESULT hr = buildType(index, f, &mt);
    if (hr != S_OK) return hr == VFW_S_NO_MORE_ITEMS ? S_FALSE : hr;
    *ppmt = CreateMediaType(&mt);
    if (!*ppmt) return E_OUTOFMEMORY;
    auto* caps = (VIDEO_STREAM_CONFIG_CAPS*)capsBytes;
    ZeroMemory(caps, sizeof(*caps));
    const LONGLONG dur = (LONGLONG)(10000000ull * f.fpsDen / f.fpsNum);
    caps->guid = FORMAT_VideoInfo;
    caps->VideoStandard = AnalogVideo_None;
    caps->InputSize = {(LONG)f.width, (LONG)f.height};
    caps->MinCroppingSize = caps->MaxCroppingSize = caps->InputSize;
    caps->CropGranularityX = caps->CropGranularityY = 1;
    caps->CropAlignX = caps->CropAlignY = 1;
    caps->MinOutputSize = caps->MaxOutputSize = caps->InputSize;
    caps->OutputGranularityX = caps->OutputGranularityY = 1;
    caps->MinFrameInterval = caps->MaxFrameInterval = dur;
    const LONG bits = (LONG)(mt.GetSampleSize() * 8 * f.fpsNum / f.fpsDen);
    caps->MinBitsPerSecond = caps->MaxBitsPerSecond = bits;
    return S_OK;
}

// ---------------------------------------------------------------- IKsPropertySet

STDMETHODIMP CamerafyPin::Get(REFGUID set, DWORD id, LPVOID, DWORD, LPVOID data, DWORD dataLen, DWORD* returned) {
    if (set != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (!data && !returned) return E_POINTER;
    if (returned) *returned = sizeof(GUID);
    if (!data) return S_OK;
    if (dataLen < sizeof(GUID)) return E_UNEXPECTED;
    *(GUID*)data = PIN_CATEGORY_CAPTURE;
    return S_OK;
}

STDMETHODIMP CamerafyPin::QuerySupported(REFGUID set, DWORD id, DWORD* support) {
    if (set != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (support) *support = KSPROPERTY_SUPPORT_GET;
    return S_OK;
}
