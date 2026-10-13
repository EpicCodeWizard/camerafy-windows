#include "env_check.h"
#include "console.h"
#include "usbmux.h"
#include "nvdec_api.h"
#include "vcam.h"
#include "ids.h"
#include "tuning.h"
#include <setupapi.h>
#include <cwctype>

namespace cf {

uint32_t windowsBuild() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v{sizeof(v)};
    if (fn && fn(&v) == 0) return v.dwBuildNumber;
    return 0;
}

std::string appleUsbDriverHint() {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return {};
    std::string hint;
    SP_DEVINFO_DATA d{sizeof(d)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        wchar_t id[512] = {};
        if (!SetupDiGetDeviceInstanceIdW(set, &d, id, 512, nullptr)) continue;
        std::wstring s = id;
        for (auto& c : s) c = (wchar_t)towupper(c);
        if (s.find(L"VID_05AC") == std::wstring::npos || s.find(L"&MI_") != std::wstring::npos) continue;
        wchar_t svc[128] = {};
        SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_SERVICE, nullptr, (BYTE*)svc, sizeof(svc), nullptr);
        const std::wstring service = svc;
        if (!_wcsicmp(svc, L"VBoxUSB") || !_wcsicmp(svc, L"usbipd")) {
            hint = "the iPhone (" + narrow(id) + ") is bound to usbipd's capture driver (" + narrow(service) +
                   "), so Windows' Apple driver and Apple Mobile Device Service cannot see it. A forced bind "
                   "(\"Shared (forced)\" in `usbipd list`) keeps it captured even after `usbipd detach`. Fix, in "
                   "an elevated prompt: `usbipd unbind --busid <BUSID>` (BUSID from `usbipd list`).";
            break;
        }
        if (service.empty())
            hint = "an Apple USB device (" + narrow(id) + ") is present but has no driver. Install the Apple "
                   "Devices app from the Microsoft Store, then unplug and replug the iPhone.";
    }
    SetupDiDestroyDeviceInfoList(set);
    return hint;
}

EnvResult checkEnvironment(const Options& o, bool needAmds) {
    EnvResult r;
    logInfo("Environment:");
    r.osBuild = windowsBuild();
    if (r.osBuild >= 22000) {
        logInfo("  [ok]   Windows build %u (>= 22000: MFCreateVirtualCamera available)", r.osBuild);
    } else {
        logError("Windows build %u: MFCreateVirtualCamera needs Windows 11 (build 22000+); it returns E_NOINTERFACE "
                 "here. The Media Foundation camera is impossible on this OS.",
                 r.osBuild);
        r.ok = false;
    }
    HMODULE sg = LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const bool vcamApi = sg && GetProcAddress(sg, "MFCreateVirtualCamera");
    logInfo("  [%s]   mfsensorgroup!MFCreateVirtualCamera %s", vcamApi ? "ok" : "FAIL", vcamApi ? "present" : "missing");
    if (!vcamApi) r.ok = false;

    r.amds = usbmux::probeAmds();
    if (r.amds) {
        logInfo("  [ok]   Apple Mobile Device Service listening on 127.0.0.1:27015");
    } else if (needAmds) {
        logError("Apple Mobile Device Service is not reachable on 127.0.0.1:27015. Install the Apple Devices app from "
                 "the Microsoft Store (or iTunes), which bundles it, then connect and unlock the iPhone. "
                 "(The Store version starts the service on demand when a device is attached.)");
        r.ok = false;
    } else {
        logInfo("  [--]   Apple Mobile Device Service not reachable (not needed for this mode)");
    }

    std::string err;
    r.nvdec = probeNvdec((int)o.gpuOrdinal, r.gpuReport, err);
    if (r.nvdec) logInfo("  [ok]   NVDEC: %s", r.gpuReport.c_str());
    else {
        logError("NVDEC: %s", err.c_str());
        r.ok = false;
    }

    const ComRegistration mf = queryComServer(ids::kMfSourceClsidStr, KEY_WOW64_64KEY);
    const ComRegistration ds64 = queryComServer(ids::kDsFilterClsidStr, KEY_WOW64_64KEY);
    const ComRegistration ds32 = queryComServer(ids::kDsFilterClsidStr, KEY_WOW64_32KEY);
    r.mfRegistered = mf.present && mf.fileExists;
    r.dsRegistered64 = ds64.present && ds64.fileExists;
    r.dsRegistered32 = ds32.present && ds32.fileExists;
    auto show = [](const char* what, const ComRegistration& c) {
        if (c.present && c.fileExists) logInfo("  [ok]   %s registered: %s", what, narrow(c.path).c_str());
        else if (c.present) logWarn("%s registered at %s, but that file does not exist", what, narrow(c.path).c_str());
        else logWarn("%s is not registered (run register.cmd elevated)", what);
    };
    if (!o.noMf) show("camerafy_mf.dll (x64)", mf);
    if (!o.noDs) {
        // DirectShow consumers load the filter in their own session, so without register.cmd
        // the CLI registers it per user at runtime (no admin) and removes it on exit.
        auto showDs = [&](const char* what, const ComRegistration& c) {
            if (c.present && c.fileExists) logInfo("  [ok]   %s registered machine-wide: %s", what, narrow(c.path).c_str());
            else logInfo("  [ok]   %s: not registered machine-wide; registered per user at runtime (no admin)", what);
        };
        showDs("camerafy_ds.dll (x64)", ds64);
        showDs("camerafy_ds.dll (x86)", ds32);
    }
    logInfo("  %s", tuning::powerAdvisory().c_str());
    return r;
}

}  // namespace cf
