#include "vcam.h"
#include "ids.h"
#include "console.h"

namespace cf {

namespace {
using RegisterFn = HRESULT(__stdcall*)(const wchar_t*, BOOL);
using UnregisterFn = HRESULT(__stdcall*)(BOOL);

std::wstring exeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L'\\'));
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring classKey() { return std::wstring(L"Software\\Classes\\CLSID\\") + ids::kDsFilterClsidStr; }

bool writeUserComServer(const std::wstring& dll, REGSAM view) {
    HKEY k = nullptr;
    const std::wstring key = classKey() + L"\\InprocServer32";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE | view, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    const wchar_t tm[] = L"Both";
    bool ok = RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE*)dll.c_str(), (DWORD)((dll.size() + 1) * 2)) == ERROR_SUCCESS &&
              RegSetValueExW(k, L"ThreadingModel", 0, REG_SZ, (const BYTE*)tm, sizeof(tm)) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

}  // namespace

// The DirectShow filter loads in-process in the consumer's own session, so a per-user COM
// registration (HKCU\Software\Classes) is enough: with no machine-wide registration the
// DirectShow camera needs no admin at all. register.cmd's HKLM registration takes precedence
// when present. (The MF source cannot do this: the Frame Server runs as LocalService.)
bool DsCamera::ensureComServer(std::string& info) {
    const ComRegistration m64 = queryComServer(ids::kDsFilterClsidStr, KEY_WOW64_64KEY);
    const ComRegistration m32 = queryComServer(ids::kDsFilterClsidStr, KEY_WOW64_32KEY);
    if (m64.present && m64.fileExists) {
        dllPath_ = m64.path;
    } else {
        const std::wstring p64 = exeDir() + L"\\camerafy_ds.dll";
        if (!exists(p64) || !writeUserComServer(p64, KEY_WOW64_64KEY)) {
            info = "camerafy_ds.dll not found next to camerafy.exe and not registered";
            return false;
        }
        userCom64_ = true;
        dllPath_ = p64;
        info = "registered the DirectShow filter for this user (no admin)";
    }
    if (!(m32.present && m32.fileExists)) {
        for (const std::wstring& c : {exeDir() + L"\\x86\\camerafy_ds.dll", exeDir() + L"\\..\\..\\x86\\Release\\camerafy_ds.dll"}) {
            wchar_t full[MAX_PATH];
            if (exists(c) && GetFullPathNameW(c.c_str(), MAX_PATH, full, nullptr) && writeUserComServer(full, KEY_WOW64_32KEY)) {
                userCom32_ = true;
                info += std::string(info.empty() ? "" : "; ") + "32-bit filter registered for this user";
                break;
            }
        }
        if (!userCom32_) logWarn("no 32-bit camerafy_ds.dll found: 32-bit DirectShow apps will not see the camera");
    }
    return true;
}

bool DsCamera::create(const std::wstring& name, bool perUser, std::string& err) {
    perUser_ = perUser;
    std::string info;
    if (!ensureComServer(info)) {
        err = info;
        return false;
    }
    if (!info.empty()) logInfo("DirectShow: %s", info.c_str());
    // Load the same DLL consumers will load, to write the device entry.
    dll_ = LoadLibraryExW(dllPath_.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll_) {
        err = "cannot load " + narrow(dllPath_) + ": error " + std::to_string(GetLastError());
        return false;
    }
    auto reg = (RegisterFn)GetProcAddress(dll_, "CfRegisterDevice");
    if (!reg) {
        err = "camerafy_ds.dll lacks CfRegisterDevice (stale build?)";
        return false;
    }
    const HRESULT hr = reg(name.c_str(), perUser ? TRUE : FALSE);
    if (FAILED(hr)) {
        err = "CfRegisterDevice: " + hrString(hr) + (perUser ? "" : " (machine-wide registration needs admin)");
        return false;
    }
    registered_ = true;
    return true;
}

void DsCamera::remove() {
    if (registered_ && dll_) {
        if (auto unreg = (UnregisterFn)GetProcAddress(dll_, "CfUnregisterDevice")) {
            const HRESULT hr = unreg(perUser_ ? TRUE : FALSE);
            if (FAILED(hr)) logWarn("CfUnregisterDevice: %s", hrString(hr).c_str());
        }
    }
    registered_ = false;
    if (dll_) FreeLibrary(dll_);
    dll_ = nullptr;
    if (userCom64_) RegDeleteTreeW(HKEY_CURRENT_USER, classKey().c_str());
    if (userCom32_) {
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID", 0, KEY_ALL_ACCESS | KEY_WOW64_32KEY, &k) == ERROR_SUCCESS) {
            RegDeleteTreeW(k, ids::kDsFilterClsidStr);
            RegCloseKey(k);
        }
    }
    userCom64_ = userCom32_ = false;
}

}  // namespace cf
