#pragma once
// --preview: a D3D11 window fed from the DirectShow shared-memory section, as just another
// reader. It runs on its own thread and is never on the critical path.
#include <windows.h>
#include <atomic>
#include <string>

namespace cf {

class Preview final {
public:
    ~Preview() { stop(); }
    bool start(const std::wstring& title, std::string& err);
    void stop();
    bool closedByUser() const { return closed_.load(); }

private:
    static DWORD WINAPI threadProc(void* self);
    void run();
    HANDLE thread_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<bool> closed_{false};
    std::wstring title_;
};

}  // namespace cf
