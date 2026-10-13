#pragma once
// Diagnostics: every HRESULT is shown as hex plus its system/MF message text.
// Off the hot path only.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

namespace cf {

// Formats hr into buf as "0xXXXXXXXX (message)". Looks the message up in the system
// table first, then mfplat.dll and quartz.dll, which carry the MF and DirectShow tables.
inline const char* hrText(HRESULT hr, char* buf, size_t cap) {
    char msg[512] = {};
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)hr,
                             0, msg, sizeof(msg), nullptr);
    static const char* mods[] = {"mfplat.dll", "quartz.dll", "mf.dll"};
    for (const char* m : mods) {
        if (n) break;
        HMODULE h = GetModuleHandleA(m);
        if (!h) h = LoadLibraryExA(m, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (h) n = FormatMessageA(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS, h, (DWORD)hr, 0, msg,
                                  sizeof(msg), nullptr);
    }
    while (n && (msg[n - 1] == '\r' || msg[n - 1] == '\n' || msg[n - 1] == ' ' || msg[n - 1] == '.')) msg[--n] = 0;
    snprintf(buf, cap, "0x%08lX (%s)", (unsigned long)hr, n ? msg : "no message text");
    return buf;
}

struct HrStr {
    char s[600];
    explicit HrStr(HRESULT hr) { hrText(hr, s, sizeof(s)); }
    const char* c_str() const { return s; }
};

// OutputDebugString tracing for the DLLs, visible in DebugView / an attached debugger.
inline void dtrace(const char* tag, const char* fmt, ...) {
    char buf[1024];
    int o = snprintf(buf, sizeof(buf), "[%s %lu:%lu] ", tag, GetCurrentProcessId(), GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + o, sizeof(buf) - o - 2, fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    buf[n] = '\n';
    buf[n + 1] = 0;
    OutputDebugStringA(buf);
}

}  // namespace cf
