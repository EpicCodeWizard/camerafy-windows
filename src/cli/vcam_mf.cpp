#include "vcam.h"
#include "ids.h"
#include "console.h"
#include <mfapi.h>

namespace cf {

ComRegistration queryComServer(const wchar_t* clsid, REGSAM view) {
    ComRegistration r;
    wchar_t key[160];
    swprintf(key, 160, L"Software\\Classes\\CLSID\\%s\\InprocServer32", clsid);
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return r;
    wchar_t path[MAX_PATH] = {};
    DWORD sz = sizeof(path), type = 0;
    if (RegQueryValueExW(k, nullptr, nullptr, &type, (BYTE*)path, &sz) == ERROR_SUCCESS) {
        r.present = true;
        r.path = path;
        r.fileExists = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    }
    RegCloseKey(k);
    return r;
}

bool MfCamera::create(const std::wstring& name, std::string& err) {
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        err = "MFStartup: " + hrString(hr);
        return false;
    }
    const ComRegistration reg = queryComServer(ids::kMfSourceClsidStr, KEY_WOW64_64KEY);
    if (!reg.present || !reg.fileExists) {
        err = reg.present ? "camerafy_mf.dll registered at " + narrow(reg.path) + " but the file is missing"
                          : "camerafy_mf.dll is not registered: run register.cmd (elevated) once";
        return false;
    }
    BOOL supported = FALSE;
    hr = MFIsVirtualCameraTypeSupported(MFVirtualCameraType_SoftwareCameraSource, &supported);
    if (FAILED(hr) || !supported) {
        err = "MFIsVirtualCameraTypeSupported: " + (FAILED(hr) ? hrString(hr) : std::string("not supported")) +
              " (needs Windows 11 build 22000+)";
        return false;
    }
    hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
                               MFVirtualCameraAccess_CurrentUser, name.c_str(), ids::kMfSourceClsidStr, nullptr, 0,
                               &vcam_);
    if (FAILED(hr)) {
        err = "MFCreateVirtualCamera: " + hrString(hr);
        vcam_ = nullptr;
        return false;
    }
    hr = vcam_->Start(nullptr);
    if (FAILED(hr)) {
        err = "IMFVirtualCamera::Start: " + hrString(hr);
        if (hr == (HRESULT)0xC00D3E85) err += " (MF_E_SHUTDOWN: the source was shut down when its activator was released)";
        vcam_->Shutdown();
        vcam_->Release();
        vcam_ = nullptr;
        return false;
    }
    // There is no SetSymbolicLink; the link comes back as an attribute after Start.
    LPWSTR link = nullptr;
    UINT32 len = 0;
    if (SUCCEEDED(vcam_->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len)) && link) {
        symlink_ = link;
        CoTaskMemFree(link);
    }
    return true;
}

void MfCamera::remove() {
    if (!vcam_) return;
    HRESULT hr = vcam_->Remove();
    if (FAILED(hr)) logVerbose("IMFVirtualCamera::Remove: %s", hrString(hr).c_str());
    vcam_->Shutdown();
    vcam_->Release();
    vcam_ = nullptr;
    symlink_.clear();
}

}  // namespace cf
