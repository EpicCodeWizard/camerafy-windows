#include "mf_source.h"
#include "ipc_reader.h"
#include <avrt.h>
#include <new>

namespace cfmf {

namespace {

HRESULT makeType(const StreamFormat& f, bool yuy2, IMFMediaType** out) {
    ComPtr<IMFMediaType> t;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;
    // The Frame Server rejects incomplete types without saying which field is missing,
    // so every one of these is set.
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, yuy2 ? MFVideoFormat_YUY2 : MFVideoFormat_NV12);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, f.width, f.height);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, f.fpsNum, f.fpsDen);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    t->SetUINT32(MF_MT_DEFAULT_STRIDE, yuy2 ? f.width * 2 : f.width);
    t->SetUINT32(MF_MT_SAMPLE_SIZE, yuy2 ? f.width * f.height * 2 : f.width * f.height * 3 / 2);
    t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    *out = t.Detach();
    return S_OK;
}

HRESULT setStreamAttrs(IMFAttributes* a) {
    HRESULT hr = a->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    if (SUCCEEDED(hr)) hr = a->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    if (SUCCEEDED(hr)) hr = a->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    if (SUCCEEDED(hr)) hr = a->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
    return hr;
}

}  // namespace

HRESULT MediaStream::initialize(MediaSource* source, const StreamFormat& fmt) {
    source_ = source;
    fmt_ = fmt;
    durationHns_ = 10000000ull * fmt.fpsDen / fmt.fpsNum;
    HRESULT hr = MFCreateEventQueue(&queue_);
    if (FAILED(hr)) return hr;
    hr = MFCreateAttributes(&attrs_, 8);
    if (FAILED(hr)) return hr;
    hr = setStreamAttrs(attrs_.Get());
    if (FAILED(hr)) return hr;

    // NV12 first (native, preferred). YUY2 too: without it the Frame Server synthesizes YUY2
    // for clients that want it (the DirectShow bridge even lists it first) and converts every
    // frame in an extra CPU pass; offered here, the conversion is fused into the copy this
    // source makes anyway (ParallelCopier::nv12ToYuy2).
    ComPtr<IMFMediaType> type, yuy2;
    hr = makeType(fmt, false, &type);
    if (SUCCEEDED(hr)) hr = makeType(fmt, true, &yuy2);
    if (FAILED(hr)) return hr;
    IMFMediaType* types[] = {type.Get(), yuy2.Get()};
    hr = MFCreateStreamDescriptor(0, 2, types, &sd_);
    if (FAILED(hr)) return hr;
    ComPtr<IMFMediaTypeHandler> handler;
    hr = sd_->GetMediaTypeHandler(&handler);
    if (SUCCEEDED(hr)) hr = handler->SetCurrentMediaType(type.Get());
    if (SUCCEEDED(hr)) hr = setStreamAttrs(sd_.Get());
    requestEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return hr;
}

MediaStream::~MediaStream() {
    shutdown();
    if (requestEvent_) CloseHandle(requestEvent_);
    g_objects.fetch_sub(1);
}

STDMETHODIMP MediaStream::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaStream) ||
        riid == __uuidof(IMFMediaStream2)) {
        *ppv = static_cast<IMFMediaStream2*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) MediaStream::AddRef() { return ++ref_; }
STDMETHODIMP_(ULONG) MediaStream::Release() {
    const ULONG r = --ref_;
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* cb, IUnknown* st) {
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(cb, st);
}
STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** ev) {
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(r, ev);
}
STDMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** ev) {
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->GetEvent(flags, ev);
}
STDMETHODIMP MediaStream::QueueEvent(MediaEventType met, REFGUID ext, HRESULT hr, const PROPVARIANT* v) {
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(met, ext, hr, v);
}

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** s) {
    if (!s) return E_POINTER;
    if (shutdown_ || !source_) return MF_E_SHUTDOWN;
    return source_->QueryInterface(IID_PPV_ARGS(s));
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** sd) {
    if (!sd) return E_POINTER;
    if (shutdown_) return MF_E_SHUTDOWN;
    return sd_.CopyTo(sd);
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* token) {
    if (shutdown_) return MF_E_SHUTDOWN;
    if (!active_) return MF_E_MEDIA_SOURCE_WRONGSTATE;
    AcquireSRWLockExclusive(&lock_);
    HRESULT hr = S_OK;
    if (tokHead_ - tokTail_ >= kMaxTokens - 1) {  // one slot of headroom for a re-queued token
        hr = MF_E_SAMPLEALLOCATOR_EMPTY;
    } else {
        if (token) token->AddRef();
        tokens_[tokHead_++ % kMaxTokens] = token;
        pending_.fetch_add(1, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&lock_);
    SetEvent(requestEvent_);
    return hr;
}

STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE s) {
    MFTRACE("SetStreamState(%d)", (int)s);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (s == state_) return S_OK;
    switch (s) {
    case MF_STREAM_STATE_RUNNING: {
        ComPtr<IMFMediaTypeHandler> h;
        ComPtr<IMFMediaType> t;
        HRESULT hr = sd_->GetMediaTypeHandler(&h);
        if (SUCCEEDED(hr)) hr = h->GetCurrentMediaType(&t);
        if (SUCCEEDED(hr)) hr = start(t.Get());
        return hr;
    }
    case MF_STREAM_STATE_STOPPED:
        return stop();
    case MF_STREAM_STATE_PAUSED:
        state_ = s;
        return S_OK;
    default:
        return E_INVALIDARG;
    }
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* s) {
    if (!s) return E_POINTER;
    if (shutdown_) return MF_E_SHUTDOWN;
    *s = state_;
    return S_OK;
}

HRESULT MediaStream::setAllocator(IUnknown* allocator) {
    AcquireSRWLockExclusive(&lock_);
    allocator_.Reset();
    HRESULT hr = S_OK;
    if (allocator) hr = allocator->QueryInterface(IID_PPV_ARGS(&allocator_));
    ReleaseSRWLockExclusive(&lock_);
    return hr;
}

// Heavy work happens here, not at construction.
HRESULT MediaStream::start(IMFMediaType* type) {
    MFTRACE("MediaStream::start");
    if (shutdown_) return MF_E_SHUTDOWN;
    HRESULT hr = S_OK;
    if (!allocator_) {
        hr = MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator_));
        MFTRACE("own allocator: 0x%08lX", hr);
        if (FAILED(hr)) return hr;
    }
    // Enough samples that a downstream holding a few never starves us; extra samples
    // cost memory only, and the newest frame is always the one written.
    GUID sub = GUID_NULL;
    yuy2_ = type && SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_YUY2;
    MFTRACE("output %s", yuy2_ ? "YUY2" : "NV12");
    hr = allocator_->InitializeSampleAllocator(8, type);
    if (FAILED(hr)) {
        char e[600];
        MFTRACE("InitializeSampleAllocator failed %s", cf::hrText(hr, e, sizeof(e)));
        return hr;
    }
    shm_.open(nullptr);

    if (!thread_) {
        copier_.start(0);
        threadStop_ = false;
        firstSample_ = true;
        thread_ = CreateThread(nullptr, 0, deliveryThreadProc, this, 0, nullptr);
        if (!thread_) return HRESULT_FROM_WIN32(GetLastError());
    }
    active_ = true;
    state_ = MF_STREAM_STATE_RUNNING;
    PROPVARIANT t;
    PropVariantInit(&t);
    t.vt = VT_I8;
    t.hVal.QuadPart = MFGetSystemTime();
    return queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, &t);
}

HRESULT MediaStream::stop() {
    MFTRACE("MediaStream::stop");
    active_ = false;
    threadStop_ = true;
    if (requestEvent_) SetEvent(requestEvent_);
    if (thread_) {
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    copier_.stop();
    // start() initializes it again; a live allocator must not be re-initialized.
    if (allocator_ && state_ != MF_STREAM_STATE_STOPPED) allocator_->UninitializeSampleAllocator();
    AcquireSRWLockExclusive(&lock_);
    while (tokTail_ != tokHead_) {
        if (IUnknown* t = tokens_[tokTail_ % kMaxTokens]) t->Release();
        ++tokTail_;
    }
    pending_ = 0;
    ReleaseSRWLockExclusive(&lock_);
    if (state_ == MF_STREAM_STATE_STOPPED) return S_OK;
    state_ = MF_STREAM_STATE_STOPPED;
    if (shutdown_) return S_OK;
    PROPVARIANT t;
    PropVariantInit(&t);
    t.vt = VT_I8;
    t.hVal.QuadPart = MFGetSystemTime();
    return queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, &t);
}

HRESULT MediaStream::shutdown() {
    if (shutdown_.exchange(true)) return S_OK;
    MFTRACE("MediaStream::shutdown");
    stop();
    if (queue_) queue_->Shutdown();
    if (allocator_) allocator_->UninitializeSampleAllocator();
    allocator_.Reset();
    shm_.close();
    source_ = nullptr;
    return S_OK;
}

DWORD WINAPI MediaStream::deliveryThreadProc(void* self) {
    // This thread sleeps between frames and spins only briefly after the writer's wake-ahead
    // signal (readerSpinUs), so MMCSS critical priority is safe and puts it in the real-time band.
    cf::ipc::raiseCameraThread();
    static_cast<MediaStream*>(self)->deliveryLoop();
    return 0;
}

// Delivers the newest published frame for each outstanding request, as soon as both exist.
// No local clock: the phone's cadence drives delivery.
void MediaStream::deliveryLoop() {
    uint64_t lastOpenTry = 0;
    uint64_t lastDeliverQpc = cf::qpc();
    while (!threadStop_.load(std::memory_order_acquire)) {
        if (pending_.load(std::memory_order_acquire) == 0) {
            WaitForSingleObject(requestEvent_, 50);
            continue;
        }
        cf::ipc::RegionHeader* h = shm_.header();
        if (!h) {
            const uint64_t now = cf::qpc();
            if (now - lastOpenTry > (uint64_t)cf::usToTicks(250000)) {
                lastOpenTry = now;
                shm_.open(nullptr);
            }
            Sleep(10);
            continue;
        }
        const uint64_t pc = h->publishCount.load(std::memory_order_acquire);
        if (pc == lastDeliveredPublish_) {
            auto r = cf::ipc::waitForFrame(h, shm_.readerIndex(), lastDeliveredPublish_, shm_.event(), 50,
                                            [&] { copier_.prepare(); });
            if (r != cf::ipc::WaitResult::Frame) {
                // No signal for a while (CLI stalled or phone gone): keep the stream alive with
                // the last frame so consumers do not declare the device dead.
                if (cf::ticksToUs((int64_t)(cf::qpc() - lastDeliverQpc)) > 500000.0 &&
                    h->latest.load(std::memory_order_acquire) < cf::ipc::kSlotCount) {
                    lastDeliveredPublish_ = pc - 1;
                }
                cf::ipc::heartbeat(h, shm_.readerIndex());
                continue;
            }
            continue;
        }
        IUnknown* token = nullptr;
        AcquireSRWLockExclusive(&lock_);
        if (tokTail_ != tokHead_) {
            token = tokens_[tokTail_ % kMaxTokens];
            ++tokTail_;
            pending_.fetch_sub(1, std::memory_order_release);
        }
        ReleaseSRWLockExclusive(&lock_);
        lastDeliveredPublish_ = pc;
        const HRESULT hr = deliverOne(token);
        if (hr == S_OK) {
            if (token) token->Release();
            lastDeliverQpc = cf::qpc();
        } else if (hr != MF_E_SHUTDOWN) {
            // Every RequestSample must be answered by exactly one sample, or the Frame Server
            // waits forever. If no sample buffer was free (all downstream) or the read lost the
            // race with the writer, give the token back and retry with the newest frame.
            AcquireSRWLockExclusive(&lock_);
            --tokTail_;
            tokens_[tokTail_ % kMaxTokens] = token;
            pending_.fetch_add(1, std::memory_order_release);
            ReleaseSRWLockExclusive(&lock_);
            lastDeliveredPublish_ = pc - 1;
            SwitchToThread();
        } else if (token) {
            token->Release();
        }
    }
}

HRESULT MediaStream::deliverOne(IUnknown* token) {
    if (!allocator_ || shutdown_) return MF_E_SHUTDOWN;
    ComPtr<IMFSample> sample;
    HRESULT hr = allocator_->AllocateSample(&sample);
    if (FAILED(hr)) return hr;  // every sample is downstream: drop this frame
    ComPtr<IMFMediaBuffer> buf;
    hr = sample->GetBufferByIndex(0, &buf);
    if (FAILED(hr)) return hr;

    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    BYTE* start = nullptr;
    DWORD len = 0;
    ComPtr<IMF2DBuffer2> b2;
    bool locked2d = false;
    if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2DSize(MF2DBuffer_LockFlags_Write, &scan0, &pitch, &start, &len))) {
        locked2d = true;
    } else {
        DWORD maxLen = 0;
        hr = buf->Lock(&scan0, &maxLen, nullptr);
        if (FAILED(hr)) return hr;
        pitch = (LONG)(yuy2_ ? fmt_.width * 2 : fmt_.width);
        len = maxLen;
    }
    const uint32_t w = fmt_.width, hgt = fmt_.height;
    const uint32_t stride = (uint32_t)(pitch < 0 ? -pitch : pitch);
    cf::ipc::FrameInfo fi{};
    cf::ipc::RegionHeader* h = shm_.header();
    const bool ok = h && cf::ipc::readLatest(h, shm_.base(), fi,
        [&](const uint8_t* src, const cf::ipc::SlotHeader& s) {
            if (yuy2_) copier_.nv12ToYuy2(src, s, scan0, stride, w, hgt);
            else copier_.copyNv12(src, s, scan0, stride, scan0 + (size_t)stride * hgt, stride, w, hgt);
        },
        &retries_);
    // Lock2DSize does not update the current length; without this every sample reads as empty.
    if (locked2d) {
        b2->Unlock2D();
        buf->SetCurrentLength(yuy2_ ? stride * hgt : stride * hgt * 3 / 2);
    } else {
        buf->SetCurrentLength(yuy2_ ? w * 2 * hgt : w * hgt * 3 / 2);
        buf->Unlock();
    }
    if (!ok) return S_FALSE;

    sample->SetSampleTime(MFGetSystemTime());
    sample->SetSampleDuration(durationHns_);
    sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    if (firstSample_) {
        sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
        firstSample_ = false;
    }
    if (token) sample->SetUnknown(MFSampleExtension_Token, token);
    hr = queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    if (SUCCEEDED(hr)) cf::ipc::recordConsumed(h, shm_.readerIndex(), fi, retries_);
    return hr;
}

}  // namespace cfmf
