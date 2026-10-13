#pragma once
#include <streams.h>
#include <initguid.h>
#include <dvdmedia.h>
#include <ks.h>
#include <ksmedia.h>
#include "ipc_reader.h"

class CamerafyPin;

class CamerafySource final : public CSource, public IAMFilterMiscFlags {
public:
    static CUnknown* WINAPI CreateInstance(LPUNKNOWN unk, HRESULT* phr);

    DECLARE_IUNKNOWN
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv) override;

    // IAMFilterMiscFlags
    STDMETHODIMP_(ULONG) GetMiscFlags() override { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }

    // Live source: no data while paused.
    STDMETHODIMP GetState(DWORD ms, FILTER_STATE* state) override;

    // Exposed so the pin can stamp samples against the graph clock.
    HRESULT CurrentStreamTime(REFERENCE_TIME* rt) { return StreamTime(*(CRefTime*)rt); }
    bool HasClock() const { return m_pClock != nullptr; }

private:
    CamerafySource(LPUNKNOWN unk, HRESULT* phr);
    ~CamerafySource() override;
    CamerafyPin* pin_ = nullptr;
};

class CamerafyPin final : public CSourceStream, public IAMStreamConfig, public IKsPropertySet, public IAMPushSource {
public:
    CamerafyPin(HRESULT* phr, CamerafySource* filter);
    ~CamerafyPin() override;

    DECLARE_IUNKNOWN
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv) override;

    // CSourceStream / CBasePin
    HRESULT GetMediaType(int pos, CMediaType* mt) override;
    HRESULT CheckMediaType(const CMediaType* mt) override;
    HRESULT SetMediaType(const CMediaType* mt) override;
    HRESULT DecideBufferSize(IMemAllocator* alloc, ALLOCATOR_PROPERTIES* req) override;
    HRESULT FillBuffer(IMediaSample* sample) override;
    HRESULT OnThreadCreate() override;
    HRESULT OnThreadDestroy() override;
    HRESULT OnThreadStartPlay() override;
    STDMETHODIMP Notify(IBaseFilter*, Quality) override { return E_NOTIMPL; }

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* mt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** mt) override;
    STDMETHODIMP GetNumberOfCapabilities(int* count, int* size) override;
    STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** mt, BYTE* caps) override;

    // IKsPropertySet
    STDMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP Get(REFGUID set, DWORD id, LPVOID instData, DWORD instLen, LPVOID data, DWORD dataLen,
                     DWORD* returned) override;
    STDMETHODIMP QuerySupported(REFGUID set, DWORD id, DWORD* support) override;

    // IAMLatency / IAMPushSource: we are a live push source with zero added latency.
    STDMETHODIMP GetLatency(REFERENCE_TIME* lat) override { if (!lat) return E_POINTER; *lat = 0; return S_OK; }
    STDMETHODIMP GetPushSourceFlags(ULONG* flags) override { if (!flags) return E_POINTER; *flags = 0; return S_OK; }
    STDMETHODIMP SetPushSourceFlags(ULONG) override { return E_NOTIMPL; }
    STDMETHODIMP SetStreamOffset(REFERENCE_TIME) override { return E_NOTIMPL; }
    STDMETHODIMP GetStreamOffset(REFERENCE_TIME* o) override { if (!o) return E_POINTER; *o = 0; return S_OK; }
    STDMETHODIMP GetMaxStreamOffset(REFERENCE_TIME* o) override { if (!o) return E_POINTER; *o = 0; return S_OK; }
    STDMETHODIMP SetMaxStreamOffset(REFERENCE_TIME) override { return E_NOTIMPL; }

private:
    struct Format {
        uint32_t width, height, fpsNum, fpsDen;
    };
    void currentSourceFormat(Format& f);
    HRESULT buildType(int index, const Format& f, CMediaType* mt);
    bool attach();
    void detach();
    void stampAndFinish(IMediaSample* sample);

    CamerafySource* filter_;
    CCritSec formatLock_;
    CMediaType forced_;  // set via IAMStreamConfig::SetFormat
    bool hasForced_ = false;

    // Shared memory, owned by the streaming thread.
    HANDLE section_ = nullptr;
    void* base_ = nullptr;
    cf::ipc::RegionHeader* hdr_ = nullptr;
    HANDLE event_ = nullptr;
    int readerIdx_ = -1;
    uint64_t lastSeen_ = 0;
    uint64_t retries_ = 0;
    uint64_t lastAttachTry_ = 0;
    cf::ipc::ParallelCopier copier_;
    bool haveFrame_ = false;

    // Negotiated output, cached at SetMediaType so FillBuffer branches on scalars only.
    bool yuy2_ = false;
    uint32_t outW_ = 0, outH_ = 0, outStride_ = 0;
    REFERENCE_TIME frameDur_ = 333333;
    REFERENCE_TIME lastStart_ = 0;
    REFERENCE_TIME synthetic_ = 0;
    bool first_ = true;
};
