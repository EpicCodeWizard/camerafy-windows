#pragma once
// Media Foundation virtual camera source, loaded by the Frame Server (svchost -k Camera,
// LocalService, session 0) and by the Frame Server Monitor / consumer processes.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfobjects.h>
#include <mfvirtualcamera.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <atomic>
#include "ipc_reader.h"
#include "diag.h"

#define MFTRACE(...) cf::dtrace("cf-mf", __VA_ARGS__)

namespace cfmf {

using Microsoft::WRL::ComPtr;

extern std::atomic<long> g_objects;
extern HMODULE g_module;

struct StreamFormat {
    uint32_t width = 1920, height = 1080, fpsNum = 30, fpsDen = 1;
};

// The Global\ section and its events. Created here (the Frame Server holds
// SeCreateGlobalPrivilege) with a DACL that lets the interactive user's CLI open it.
class SharedFrames final {
public:
    ~SharedFrames();
    HRESULT open(bool* created);
    // Waits (bounded) for the CLI to publish the stream format into the section.
    bool waitForFormat(DWORD timeoutMs, StreamFormat& out);
    void close();

    cf::ipc::RegionHeader* header() const { return hdr_; }
    void* base() const { return base_; }
    HANDLE event() const { return event_; }
    int readerIndex() const { return reader_; }

private:
    HANDLE section_ = nullptr;
    HANDLE fmtEvent_ = nullptr;
    HANDLE event_ = nullptr;
    void* base_ = nullptr;
    cf::ipc::RegionHeader* hdr_ = nullptr;
    int reader_ = -1;
};

class MediaSource;

class MediaStream final : public IMFMediaStream2 {
public:
    HRESULT initialize(MediaSource* source, const StreamFormat& fmt);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID ext, HRESULT hr, const PROPVARIANT* v) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** sd) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

    // Called by the source.
    HRESULT start(IMFMediaType* type);
    HRESULT stop();
    HRESULT shutdown();
    HRESULT setAllocator(IUnknown* allocator);
    IMFAttributes* attributes() const { return attrs_.Get(); }
    bool active() const { return active_.load(std::memory_order_acquire); }

private:
    MediaStream() = default;
    ~MediaStream();
    friend class MediaSource;

    static DWORD WINAPI deliveryThreadProc(void* self);
    void deliveryLoop();
    HRESULT deliverOne(IUnknown* token);

    std::atomic<ULONG> ref_{1};
    SRWLOCK lock_ = SRWLOCK_INIT;
    MediaSource* source_ = nullptr;  // weak; the source owns us
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFStreamDescriptor> sd_;
    ComPtr<IMFAttributes> attrs_;
    ComPtr<IMFVideoSampleAllocatorEx> allocator_;
    StreamFormat fmt_;
    uint64_t durationHns_ = 333333;

    std::atomic<bool> active_{false};
    std::atomic<bool> shutdown_{false};
    std::atomic<bool> threadStop_{false};
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    HANDLE thread_ = nullptr;
    HANDLE requestEvent_ = nullptr;

    // Outstanding RequestSample tokens. A plain fixed ring guarded by lock_; RequestSample
    // and the delivery thread are the only users.
    static constexpr uint32_t kMaxTokens = 64;
    IUnknown* tokens_[kMaxTokens] = {};
    uint32_t tokHead_ = 0, tokTail_ = 0;
    std::atomic<uint32_t> pending_{0};

    SharedFrames shm_;
    cf::ipc::ParallelCopier copier_;
    uint64_t lastDeliveredPublish_ = 0;
    uint64_t retries_ = 0;
    bool firstSample_ = true;
    bool yuy2_ = false;  // current output type (set in start)
};

class MediaSource final : public IMFMediaSourceEx, public IKsControl, public IMFGetService, public IMFSampleAllocatorControl {
public:
    static HRESULT create(MediaSource** out);
    HRESULT initialize();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID ext, HRESULT hr, const PROPVARIANT* v) override;

    // IMFMediaSource
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pd) override;
    STDMETHODIMP GetCharacteristics(DWORD* c) override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;
    STDMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT* startPos) override;
    STDMETHODIMP Stop() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** attrs) override;
    STDMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attrs) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;

    // IKsControl: must exist as a routing target; we support no property sets.
    STDMETHODIMP KsProperty(PKSPROPERTY p, ULONG pl, LPVOID d, ULONG dl, ULONG* ret) override;
    STDMETHODIMP KsMethod(PKSMETHOD m, ULONG ml, LPVOID d, ULONG dl, ULONG* ret) override;
    STDMETHODIMP KsEvent(PKSEVENT e, ULONG el, LPVOID d, ULONG dl, ULONG* ret) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* obj) override;

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD streamId, IUnknown* allocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD streamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) override;

    HRESULT checkShutdown() const { return shutdown_.load(std::memory_order_acquire) ? MF_E_SHUTDOWN : S_OK; }

private:
    MediaSource() = default;
    ~MediaSource();

    std::atomic<ULONG> ref_{1};
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::atomic<bool> shutdown_{false};
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFAttributes> attrs_;
    ComPtr<IMFPresentationDescriptor> pd_;
    MediaStream* stream_ = nullptr;  // owned (one ref)
    bool started_ = false;
};

// IMFActivate handed to the Frame Server. It creates the source on ActivateObject and must
// NOT shut it down when released: the Frame Server releases the activator right after
// activation and keeps using the source (doing so yields MF_E_SHUTDOWN on Start).
class Activator final : public IMFActivate {
public:
    static HRESULT create(REFIID riid, void** ppv);

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

    // IMFAttributes, forwarded to an inner store.
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return a_->GetItem(k, v); }
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return a_->GetItemType(k, t); }
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return a_->CompareItem(k, v, r); }
    STDMETHODIMP Compare(IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* r) override { return a_->Compare(o, t, r); }
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return a_->GetUINT32(k, v); }
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return a_->GetUINT64(k, v); }
    STDMETHODIMP GetDouble(REFGUID k, double* v) override { return a_->GetDouble(k, v); }
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return a_->GetGUID(k, v); }
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* n) override { return a_->GetStringLength(k, n); }
    STDMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 n, UINT32* l) override { return a_->GetString(k, s, n, l); }
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* l) override { return a_->GetAllocatedString(k, s, l); }
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* n) override { return a_->GetBlobSize(k, n); }
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* l) override { return a_->GetBlob(k, b, n, l); }
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) override { return a_->GetAllocatedBlob(k, b, n); }
    STDMETHODIMP GetUnknown(REFGUID k, REFIID r, LPVOID* p) override { return a_->GetUnknown(k, r, p); }
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return a_->SetItem(k, v); }
    STDMETHODIMP DeleteItem(REFGUID k) override { return a_->DeleteItem(k); }
    STDMETHODIMP DeleteAllItems() override { return a_->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return a_->SetUINT32(k, v); }
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return a_->SetUINT64(k, v); }
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return a_->SetDouble(k, v); }
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return a_->SetGUID(k, v); }
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return a_->SetString(k, v); }
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return a_->SetBlob(k, b, n); }
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* p) override { return a_->SetUnknown(k, p); }
    STDMETHODIMP LockStore() override { return a_->LockStore(); }
    STDMETHODIMP UnlockStore() override { return a_->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* n) override { return a_->GetCount(n); }
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return a_->GetItemByIndex(i, k, v); }
    STDMETHODIMP CopyAllItems(IMFAttributes* d) override { return a_->CopyAllItems(d); }

private:
    Activator() = default;
    ~Activator();
    std::atomic<ULONG> ref_{1};
    ComPtr<IMFAttributes> a_;
    SRWLOCK lock_ = SRWLOCK_INIT;
    ComPtr<IMFMediaSource> source_;
};

}  // namespace cfmf
