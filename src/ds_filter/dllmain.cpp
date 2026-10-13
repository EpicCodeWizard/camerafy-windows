// COM plumbing and registration for camerafy_ds.dll.
//
// DllRegisterServer (elevated, register.cmd) registers only the COM class.
// The video-input-category entry that makes the camera visible carries the phone's name,
// which only the CLI knows. The CLI calls CfRegisterDevice / CfUnregisterDevice exported
// from this DLL at runtime, per-user (no admin), and removes the entry on exit.
// `regsvr32 /i:machine camerafy_ds.dll` adds a persistent machine-wide entry with a generic name.
#include "ds_filter.h"
#include "ids.h"
#include "diag.h"
#include <olectl.h>

using namespace cf;

static const AMOVIESETUP_MEDIATYPE kPinTypes[] = {
    {&MEDIATYPE_Video, &MEDIASUBTYPE_NV12},
    {&MEDIATYPE_Video, &MEDIASUBTYPE_YUY2},
};

CFactoryTemplate g_Templates[] = {
    {L"Camerafy DirectShow source", &ids::kDsFilterClsid, CamerafySource::CreateInstance, nullptr, nullptr},
};
int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);

extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);

BOOL APIENTRY DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    return DllEntryPoint(h, reason, reserved);
}

namespace {

constexpr wchar_t kInstanceKeyFmt[] = L"Software\\Classes\\CLSID\\%s\\Instance\\%s";

HRESULT registerCategoryEntry(const wchar_t* friendlyName) {
    IFilterMapper2* fm = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, (void**)&fm);
    if (FAILED(hr)) return hr;
    REGPINTYPES types[] = {{&MEDIATYPE_Video, &MEDIASUBTYPE_NV12}, {&MEDIATYPE_Video, &MEDIASUBTYPE_YUY2}};
    REGFILTERPINS2 pin{};
    pin.dwFlags = REG_PINFLAG_B_OUTPUT;
    pin.cInstances = 1;
    pin.nMediaTypes = 2;
    pin.lpMediaType = types;
    pin.clsPinCategory = &PIN_CATEGORY_CAPTURE;
    REGFILTER2 rf2{};
    rf2.dwVersion = 2;
    rf2.dwMerit = MERIT_DO_NOT_USE;
    rf2.cPins2 = 1;
    rf2.rgPins2 = &pin;
    hr = fm->RegisterFilter(ids::kDsFilterClsid, friendlyName, nullptr, &CLSID_VideoInputDeviceCategory,
                            ids::kDsFilterClsidStr, &rf2);
    fm->Release();
    return hr;
}

HRESULT unregisterCategoryEntry() {
    IFilterMapper2* fm = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, (void**)&fm);
    if (FAILED(hr)) return hr;
    hr = fm->UnregisterFilter(&CLSID_VideoInputDeviceCategory, ids::kDsFilterClsidStr, ids::kDsFilterClsid);
    fm->Release();
    return hr;
}

// Copies every value of src into dst (same-depth instance key; values only).
LSTATUS copyValues(HKEY src, HKEY dst) {
    DWORD idx = 0;
    wchar_t name[256];
    BYTE data[4096];
    while (true) {
        DWORD nameLen = 256, dataLen = sizeof(data), type = 0;
        LSTATUS s = RegEnumValueW(src, idx++, name, &nameLen, nullptr, &type, data, &dataLen);
        if (s == ERROR_NO_MORE_ITEMS) return ERROR_SUCCESS;
        if (s != ERROR_SUCCESS) return s;
        s = RegSetValueExW(dst, name, 0, type, data, dataLen);
        if (s != ERROR_SUCCESS) return s;
    }
}

}  // namespace

STDAPI DllRegisterServer() {
    // Registers InprocServer32 (ThreadingModel=Both) only; see the header comment.
    return AMovieDllRegisterServer2(TRUE);
}

STDAPI DllUnregisterServer() {
    // Best effort: also drop any machine-wide category entry left by /i:machine.
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        unregisterCategoryEntry();
        CoUninitialize();
    }
    return AMovieDllRegisterServer2(FALSE);
}

STDAPI DllInstall(BOOL install, LPCWSTR cmdLine) {
    if (!cmdLine || _wcsicmp(cmdLine, L"machine") != 0) return E_INVALIDARG;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = SUCCEEDED(hr);
    hr = install ? registerCategoryEntry(ids::kDsDefaultFriendlyName) : unregisterCategoryEntry();
    if (uninit) CoUninitialize();
    return hr;
}

// Per-user registration of the video-input-category entry with the phone's name.
// HKCR is redirected to HKCU\Software\Classes for the duration of the IFilterMapper2 call,
// so the mapper serializes FilterData for us exactly as it would machine-wide. The entry is
// then mirrored into the 32-bit registry view so 32-bit consumers see it too.
extern "C" HRESULT __stdcall CfRegisterDevice(const wchar_t* friendlyName, BOOL perUser) {
    if (!friendlyName || !*friendlyName) return E_INVALIDARG;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = SUCCEEDED(hrInit);
    HRESULT hr = S_OK;
    if (!perUser) {
        hr = registerCategoryEntry(friendlyName);
    } else {
        HKEY classes = nullptr;
        LSTATUS s = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, nullptr, 0,
                                    KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &classes, nullptr);
        if (s != ERROR_SUCCESS) {
            hr = HRESULT_FROM_WIN32(s);
        } else {
            s = RegOverridePredefKey(HKEY_CLASSES_ROOT, classes);
            if (s != ERROR_SUCCESS) {
                hr = HRESULT_FROM_WIN32(s);
            } else {
                hr = registerCategoryEntry(friendlyName);
                RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
            }
            RegCloseKey(classes);
        }
        if (SUCCEEDED(hr)) {
            wchar_t path[256];
            swprintf(path, 256, kInstanceKeyFmt, ids::kVideoInputCategoryStr, ids::kDsFilterClsidStr);
            HKEY src = nullptr, dst = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ | KEY_WOW64_64KEY, &src) == ERROR_SUCCESS) {
                if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_ALL_ACCESS | KEY_WOW64_32KEY, nullptr,
                                    &dst, nullptr) == ERROR_SUCCESS) {
                    copyValues(src, dst);
                    RegCloseKey(dst);
                }
                RegCloseKey(src);
            } else {
                hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);  // the override did not take effect
            }
        }
    }
    if (uninit) CoUninitialize();
    return hr;
}

extern "C" HRESULT __stdcall CfUnregisterDevice(BOOL perUser) {
    if (!perUser) {
        HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        HRESULT hr = unregisterCategoryEntry();
        if (SUCCEEDED(hrInit)) CoUninitialize();
        return hr;
    }
    wchar_t path[256];
    swprintf(path, 256, kInstanceKeyFmt, ids::kVideoInputCategoryStr, ids::kDsFilterClsidStr);
    LSTATUS a = RegDeleteKeyExW(HKEY_CURRENT_USER, path, KEY_WOW64_64KEY, 0);
    LSTATUS b = RegDeleteKeyExW(HKEY_CURRENT_USER, path, KEY_WOW64_32KEY, 0);
    if (a != ERROR_SUCCESS && a != ERROR_FILE_NOT_FOUND) return HRESULT_FROM_WIN32(a);
    if (b != ERROR_SUCCESS && b != ERROR_FILE_NOT_FOUND) return HRESULT_FROM_WIN32(b);
    return S_OK;
}
