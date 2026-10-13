#include "mf_source.h"
#include <new>

namespace cfmf {

HRESULT Activator::create(REFIID riid, void** ppv) {
    auto* a = new (std::nothrow) Activator();
    if (!a) return E_OUTOFMEMORY;
    g_objects.fetch_add(1);
    HRESULT hr = MFCreateAttributes(&a->a_, 4);
    if (SUCCEEDED(hr)) hr = a->QueryInterface(riid, ppv);
    a->Release();
    return hr;
}

Activator::~Activator() {
    MFTRACE("Activator released (source kept alive: %p)", source_.Get());
    g_objects.fetch_sub(1);
}

STDMETHODIMP Activator::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IMFAttributes) || riid == __uuidof(IMFActivate)) {
        *ppv = static_cast<IMFActivate*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) Activator::AddRef() { return ++ref_; }
STDMETHODIMP_(ULONG) Activator::Release() {
    const ULONG r = --ref_;
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP Activator::ActivateObject(REFIID riid, void** ppv) {
    MFTRACE("ActivateObject");
    if (!ppv) return E_POINTER;
    AcquireSRWLockExclusive(&lock_);
    HRESULT hr = S_OK;
    if (!source_) {
        MediaSource* s = nullptr;
        hr = MediaSource::create(&s);
        if (SUCCEEDED(hr)) source_.Attach(static_cast<IMFMediaSourceEx*>(s));
    }
    if (SUCCEEDED(hr)) hr = source_->QueryInterface(riid, ppv);
    ReleaseSRWLockExclusive(&lock_);
    char e[600];
    MFTRACE("ActivateObject -> %s", cf::hrText(hr, e, sizeof(e)));
    return hr;
}

// Deliberately does not shut the source down; see the class comment.
STDMETHODIMP Activator::ShutdownObject() {
    MFTRACE("ShutdownObject (source left running for the Frame Server)");
    return S_OK;
}

STDMETHODIMP Activator::DetachObject() {
    MFTRACE("DetachObject");
    AcquireSRWLockExclusive(&lock_);
    source_.Reset();
    ReleaseSRWLockExclusive(&lock_);
    return S_OK;
}

}  // namespace cfmf
