#include "mf_source.h"
#include <new>

namespace cfmf {

HRESULT MediaSource::create(MediaSource** out) {
    auto* s = new (std::nothrow) MediaSource();
    if (!s) return E_OUTOFMEMORY;
    g_objects.fetch_add(1);
    HRESULT hr = s->initialize();
    if (FAILED(hr)) {
        s->Release();
        return hr;
    }
    *out = s;
    return S_OK;
}

// Light work only: sensor-group enumeration creates sources that are never started.
// The one exception is the short wait for the CLI's format, which the stream descriptor
// needs; the CLI writes it within milliseconds of the section appearing.
HRESULT MediaSource::initialize() {
    MFTRACE("MediaSource::initialize");
    HRESULT hr = MFCreateEventQueue(&queue_);
    if (FAILED(hr)) return hr;
    hr = MFCreateAttributes(&attrs_, 4);
    if (FAILED(hr)) return hr;
    attrs_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);

    stream_ = new (std::nothrow) MediaStream();
    if (!stream_) return E_OUTOFMEMORY;
    g_objects.fetch_add(1);

    StreamFormat fmt;
    bool created = false;
    if (SUCCEEDED(stream_->shm_.open(&created))) {
        if (!stream_->shm_.waitForFormat(3000, fmt)) MFTRACE("no format from CLI within 3 s; using %ux%u", fmt.width, fmt.height);
    }
    MFTRACE("format %ux%u @ %u/%u", fmt.width, fmt.height, fmt.fpsNum, fmt.fpsDen);
    hr = stream_->initialize(this, fmt);
    if (FAILED(hr)) return hr;

    IMFStreamDescriptor* sds[] = {stream_->sd_.Get()};
    hr = MFCreatePresentationDescriptor(1, sds, &pd_);
    if (FAILED(hr)) return hr;
    return pd_->SelectStream(0);
}

MediaSource::~MediaSource() {
    MFTRACE("MediaSource::~MediaSource");
    if (stream_) {
        stream_->shutdown();
        stream_->Release();
    }
    g_objects.fetch_sub(1);
}

STDMETHODIMP MediaSource::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaSource) ||
        riid == __uuidof(IMFMediaSourceEx))
        *ppv = static_cast<IMFMediaSourceEx*>(this);
    else if (riid == __uuidof(IKsControl))
        *ppv = static_cast<IKsControl*>(this);
    else if (riid == __uuidof(IMFGetService))
        *ppv = static_cast<IMFGetService*>(this);
    else if (riid == __uuidof(IMFSampleAllocatorControl))
        *ppv = static_cast<IMFSampleAllocatorControl*>(this);
    else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) MediaSource::AddRef() { return ++ref_; }
STDMETHODIMP_(ULONG) MediaSource::Release() {
    const ULONG r = --ref_;
    if (r == 0) delete this;
    return r;
}

// ---------------------------------------------------------------- IMFMediaEventGenerator

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) {
    if (FAILED(checkShutdown())) return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(cb, state);
}
STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** ev) {
    if (FAILED(checkShutdown())) return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(r, ev);
}
STDMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** ev) {
    if (FAILED(checkShutdown())) return MF_E_SHUTDOWN;
    return queue_->GetEvent(flags, ev);
}
STDMETHODIMP MediaSource::QueueEvent(MediaEventType met, REFGUID ext, HRESULT hr, const PROPVARIANT* v) {
    if (FAILED(checkShutdown())) return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(met, ext, hr, v);
}

// ---------------------------------------------------------------- IMFMediaSource

STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** pd) {
    MFTRACE("CreatePresentationDescriptor");
    if (!pd) return E_POINTER;
    if (FAILED(checkShutdown())) return MF_E_SHUTDOWN;
    return pd_->Clone(pd);
}

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* c) {
    if (!c) return E_POINTER;
    if (FAILED(checkShutdown())) return MF_E_SHUTDOWN;
    *c = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP MediaSource::Pause() {
    MFTRACE("Pause (unsupported)");
    if (FAILED(checkShutdown())) return MF_E_SHUTDOWN;
    return MF_E_INVALID_STATE_TRANSITION;
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT* startPos) {
    MFTRACE("Start");
    if (!pd) return E_INVALIDARG;
    if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
    if (FAILED(checkShutdown())) return MF_E_SHUTDOWN;
    AcquireSRWLockExclusive(&lock_);

    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> sd;
    HRESULT hr = pd->GetStreamDescriptorByIndex(0, &selected, &sd);
    ComPtr<IMFMediaType> type;
    if (SUCCEEDED(hr)) {
        ComPtr<IMFMediaTypeHandler> handler;
        hr = sd->GetMediaTypeHandler(&handler);
        if (SUCCEEDED(hr)) hr = handler->GetCurrentMediaType(&type);
    }
    PROPVARIANT t;
    PropVariantInit(&t);
    t.vt = VT_I8;
    t.hVal.QuadPart = MFGetSystemTime();
    if (SUCCEEDED(hr)) {
        const bool wasActive = stream_->active();
        if (selected) {
            hr = queue_->QueueEventParamUnk(wasActive ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK,
                                            static_cast<IMFMediaStream*>(stream_));
            if (SUCCEEDED(hr)) hr = stream_->start(type.Get());
        } else if (wasActive) {
            stream_->stop();
        }
    }
    if (SUCCEEDED(hr)) {
        started_ = true;
        hr = queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &t);
    }
    ReleaseSRWLockExclusive(&lock_);
    char e[600];
    MFTRACE("Start -> %s", cf::hrText(hr, e, sizeof(e)));
    return hr;
}

STDMETHODIMP MediaSource::Stop() {
    MFTRACE("Stop");
    if (FAILED(checkShutdown())) return MF_E_SHUTDOWN;
    AcquireSRWLockExclusive(&lock_);
    stream_->stop();
    started_ = false;
    PROPVARIANT t;
    PropVariantInit(&t);
    t.vt = VT_I8;
    t.hVal.QuadPart = MFGetSystemTime();
    HRESULT hr = queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &t);
    ReleaseSRWLockExclusive(&lock_);
    return hr;
}

STDMETHODIMP MediaSource::Shutdown() {
    MFTRACE("Shutdown");
    if (shutdown_.exchange(true)) return MF_E_SHUTDOWN;
    AcquireSRWLockExclusive(&lock_);
    if (stream_) stream_->shutdown();
    if (queue_) queue_->Shutdown();
    ReleaseSRWLockExclusive(&lock_);
    return S_OK;
}

// ---------------------------------------------------------------- IMFMediaSourceEx

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** a) {
    MFTRACE("GetSourceAttributes");
    if (!a) return E_POINTER;
    if (FAILED(checkShutdown())) return MF_E_SHUTDOWN;
    return attrs_.CopyTo(a);
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD id, IMFAttributes** a) {
    MFTRACE("GetStreamAttributes(%lu)", id);
    if (!a) return E_POINTER;
    if (FAILED(checkShutdown())) return MF_E_SHUTDOWN;
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    *a = stream_->attributes();
    (*a)->AddRef();
    return S_OK;
}

// We produce system-memory NV12: every consumer path (DirectShow bridge, CPU readers)
// needs system memory, so a D3D manager would only add an upload and a readback.
STDMETHODIMP MediaSource::SetD3DManager(IUnknown* m) {
    MFTRACE("SetD3DManager(%p) ignored", m);
    if (FAILED(checkShutdown())) return MF_E_SHUTDOWN;
    return S_OK;
}

// ---------------------------------------------------------------- IKsControl

STDMETHODIMP MediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG* ret) {
    if (ret) *ret = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* ret) {
    if (ret) *ret = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* ret) {
    if (ret) *ret = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// ---------------------------------------------------------------- IMFGetService

STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* obj) {
    if (obj) *obj = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

// ---------------------------------------------------------------- IMFSampleAllocatorControl

STDMETHODIMP MediaSource::SetDefaultAllocator(DWORD id, IUnknown* allocator) {
    MFTRACE("SetDefaultAllocator(%lu, %p)", id, allocator);
    if (FAILED(checkShutdown())) return MF_E_SHUTDOWN;
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    return stream_->setAllocator(allocator);
}

STDMETHODIMP MediaSource::GetAllocatorUsage(DWORD id, DWORD* inputId, MFSampleAllocatorUsage* usage) {
    MFTRACE("GetAllocatorUsage(%lu)", id);
    if (!inputId || !usage) return E_POINTER;
    if (FAILED(checkShutdown())) return MF_E_SHUTDOWN;
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    *inputId = id;
    *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}

}  // namespace cfmf
