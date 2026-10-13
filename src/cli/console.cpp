#include "console.h"
#include "diag.h"
#include <stdio.h>
#include <stdarg.h>

namespace cf {

bool g_verbose = false;
std::atomic<bool> g_quit{false};
EventRing g_events[kEvSourceCount];

static SRWLOCK g_outLock = SRWLOCK_INIT;

static BOOL WINAPI ctrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_quit.exchange(true)) return FALSE;  // second Ctrl+C: default handling (terminate)
        return TRUE;
    }
    return FALSE;
}

void installCtrlHandler() {
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    SetConsoleOutputCP(CP_UTF8);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode)) SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

static void vlog(FILE* f, const char* prefix, const char* fmt, va_list ap) {
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    AcquireSRWLockExclusive(&g_outLock);
    fprintf(f, "%s%s\n", prefix, buf);
    fflush(f);
    ReleaseSRWLockExclusive(&g_outLock);
}

void logInfo(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlog(stdout, "", fmt, ap); va_end(ap); }
void logWarn(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlog(stdout, "\x1b[33mwarning:\x1b[0m ", fmt, ap); va_end(ap); }
void logError(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlog(stderr, "\x1b[31merror:\x1b[0m ", fmt, ap); va_end(ap); }
void logVerbose(const char* fmt, ...) {
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vlog(stdout, "  ", fmt, ap);
    va_end(ap);
}

void logBanner(const char* title, const char* body) {
    AcquireSRWLockExclusive(&g_outLock);
    fprintf(stdout,
            "\n\x1b[41;97m !!! %s !!! \x1b[0m\n%s\n"
            "\x1b[41;97m %*s \x1b[0m\n\n",
            title, body, (int)strlen(title) + 8, "");
    fflush(stdout);
    ReleaseSRWLockExclusive(&g_outLock);
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string hrString(HRESULT hr) { return HrStr(hr).c_str(); }

const char* evName(Ev e) {
    switch (e) {
    case Ev::ArenaOverflow: return "arena overflow (decoder fell >64 MB behind; resynced)";
    case Ev::AuRingFull: return "AU ring full (frame dropped)";
    case Ev::PicRingFull: return "picture ring full (frame dropped)";
    case Ev::TelemetryRingFull: return "telemetry ring full";
    case Ev::ParseError: return "cuvidParseVideoData failed";
    case Ev::DecodeError: return "cuvidDecodePicture failed";
    case Ev::MapError: return "cuvidMapVideoFrame64 failed";
    case Ev::CopyError: return "readback copy failed";
    case Ev::DroppedBeforeIdr: return "AU dropped while waiting for IDR";
    case Ev::StreamResync: return "stream resync";
    case Ev::AuTooLarge: return "AU larger than the arena";
    case Ev::RecvError: return "recv error";
    case Ev::SequenceChange: return "sequence change";
    }
    return "?";
}

}  // namespace cf
