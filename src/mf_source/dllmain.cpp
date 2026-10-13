// COM server for the MF virtual camera source.
// Registered under HKLM (both Frame Server services must load it) with ThreadingModel=Both.
#include "mf_source.h"
#include "ids.h"
#include <new>
#include <stdio.h>

namespace cfmf {
std::atomic<long> g_objects{0};
HMODULE g_module = nullptr;
}  // namespace cfmf

using namespace cfmf;

namespace {

std::atomic<long> g_locks{0};

class ClassFactory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }   // static instance
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        // MF may be started already by the host; MFStartup is refcounted.
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
        HRESULT hr = Activator::create(riid, ppv);
        MFTRACE("ClassFactory::CreateInstance -> 0x%08lX", hr);
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) g_locks.fetch_add(1);
        else g_locks.fetch_sub(1);
        return S_OK;
    }
};

ClassFactory g_factory;

LSTATUS setString(HKEY root, const wchar_t* sub, const wchar_t* name, const wchar_t* value) {
    HKEY k = nullptr;
    LSTATUS s = RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
    if (s != ERROR_SUCCESS) return s;
    s = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return s;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid != cf::ids::kMfSourceClsid) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() {
    return (g_objects.load() == 0 && g_locks.load() == 0) ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    wchar_t key[128];
    swprintf(key, 128, L"Software\\Classes\\CLSID\\%s", cf::ids::kMfSourceClsidStr);
    LSTATUS s = setString(HKEY_LOCAL_MACHINE, key, nullptr, L"Camerafy virtual camera source");
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);
    wcscat_s(key, L"\\InprocServer32");
    s = setString(HKEY_LOCAL_MACHINE, key, nullptr, path);
    if (s == ERROR_SUCCESS) s = setString(HKEY_LOCAL_MACHINE, key, L"ThreadingModel", L"Both");
    return HRESULT_FROM_WIN32(s);
}

STDAPI DllUnregisterServer() {
    wchar_t key[128];
    swprintf(key, 128, L"Software\\Classes\\CLSID\\%s", cf::ids::kMfSourceClsidStr);
    LSTATUS s = RegDeleteTreeW(HKEY_LOCAL_MACHINE, key);
    return (s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(s);
}
