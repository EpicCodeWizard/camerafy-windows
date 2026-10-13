#pragma once
#include <windows.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <string>

namespace cf {

// "<phone> Webcam MediaFoundation (slow)": a Media Foundation virtual camera (Frame Server, @device_pnp_...).
// Session lifetime + CurrentUser: dies with the process, needs no admin.
class MfCamera final {
public:
    ~MfCamera() { remove(); }
    bool create(const std::wstring& friendlyName, std::string& err);
    void remove();
    bool active() const { return vcam_ != nullptr; }
    const std::wstring& symbolicLink() const { return symlink_; }

private:
    IMFVirtualCamera* vcam_ = nullptr;
    std::wstring symlink_;
};

// "<phone> Webcam": the in-process DirectShow source filter
// (@device_sw_...). Its video-input-category entry is written at runtime with the phone's
// name and removed on exit.
class DsCamera final {
public:
    ~DsCamera() { remove(); }
    bool create(const std::wstring& friendlyName, bool perUser, std::string& err);
    void remove();
    bool active() const { return registered_; }

private:
    bool ensureComServer(std::string& info);
    HMODULE dll_ = nullptr;
    bool registered_ = false;
    bool perUser_ = true;
    bool userCom64_ = false, userCom32_ = false;  // per-user COM registration we created
    std::wstring dllPath_;
};

struct ComRegistration {
    bool present = false;
    std::wstring path;
    bool fileExists = false;
};
// Reads HKLM CLSID registration in the given registry view (KEY_WOW64_64KEY / _32KEY).
ComRegistration queryComServer(const wchar_t* clsid, REGSAM view);

}  // namespace cf
