// The Global\ frame section. The media source (in the Frame Server, LocalService, session 0)
// creates it, because the user's CLI lacks SeCreateGlobalPrivilege. The CLI opens it,
// retrying until it exists, then writes the stream format and signals kMfFormatReadyName.
#include "mf_source.h"
#include <sddl.h>
#include <stdio.h>

namespace cfmf {

namespace {

// SYSTEM, LocalService, Administrators: full. Interactive users (the CLI's user): full
// section access, since the CLI writes frames. App containers get read access so packaged
// consumers that load this DLL can inspect the header without failing.
constexpr wchar_t kSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GA;;;IU)(A;;GR;;;AC)";

bool makeSa(SECURITY_ATTRIBUTES& sa) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &sd, nullptr)) return false;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    return true;
}

}  // namespace

SharedFrames::~SharedFrames() { close(); }

HRESULT SharedFrames::open(bool* created) {
    if (base_) {
        if (created) *created = false;
        return S_OK;
    }
    using namespace cf::ipc;
    SECURITY_ATTRIBUTES sa{};
    const bool haveSa = makeSa(sa);
    bool isNew = false;
    section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, haveSa ? &sa : nullptr, PAGE_READWRITE | SEC_COMMIT,
                                  (DWORD)(kRegionBytes >> 32), (DWORD)kRegionBytes, mfSectionName());
    DWORD err = GetLastError();
    if (section_) {
        isNew = err != ERROR_ALREADY_EXISTS;
    } else {
        // Not in the Frame Server (no SeCreateGlobalPrivilege): open an existing one.
        MFTRACE("CreateFileMapping(Global) failed %lu; trying open", err);
        section_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mfSectionName());
        if (!section_) {
            err = GetLastError();
            if (haveSa) LocalFree(sa.lpSecurityDescriptor);
            MFTRACE("OpenFileMapping(Global) failed %lu", err);
            return HRESULT_FROM_WIN32(err);
        }
    }
    base_ = MapViewOfFile(section_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (!base_) {
        err = GetLastError();
        if (haveSa) LocalFree(sa.lpSecurityDescriptor);
        close();
        return HRESULT_FROM_WIN32(err);
    }
    if (isNew) {
        initRegion(base_);
        // Header only: payload pages are committed as frames arrive, and only the bytes a
        // frame uses (1080p: ~3 MB of each 32 MB slot) ever become resident.
        pretouch(base_, kRegionHeaderBytes);
    } else if (!validRegion(base_)) {
        MFTRACE("existing Global section has a foreign layout");
        if (haveSa) LocalFree(sa.lpSecurityDescriptor);
        close();
        return E_UNEXPECTED;
    }
    hdr_ = (RegionHeader*)base_;

    fmtEvent_ = CreateEventW(haveSa ? &sa : nullptr, TRUE, FALSE, mfFormatReadyName());
    if (!fmtEvent_) fmtEvent_ = OpenEventW(SYNCHRONIZE, FALSE, mfFormatReadyName());

    reader_ = claimReader(hdr_, 2);
    if (reader_ >= 0) {
        wchar_t name[96];
        swprintf(name, 96, mfEventFmt(), (unsigned)reader_);
        event_ = CreateEventW(haveSa ? &sa : nullptr, FALSE, FALSE, name);
        if (!event_) event_ = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
    }
    if (haveSa) LocalFree(sa.lpSecurityDescriptor);
    MFTRACE("Global section %s, reader %d, event %p", isNew ? "created" : "opened", reader_, event_);
    if (created) *created = isNew;
    return S_OK;
}

bool SharedFrames::waitForFormat(DWORD timeoutMs, StreamFormat& out) {
    if (!hdr_) return false;
    const ULONGLONG end = GetTickCount64() + timeoutMs;
    while (true) {
        if (hdr_->formatSeq.load(std::memory_order_acquire) && hdr_->width && hdr_->height) {
            out.width = hdr_->width;
            out.height = hdr_->height;
            out.fpsNum = hdr_->fpsNum ? hdr_->fpsNum : 30;
            out.fpsDen = hdr_->fpsDen ? hdr_->fpsDen : 1;
            return true;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= end) return false;
        if (fmtEvent_) WaitForSingleObject(fmtEvent_, (DWORD)(end - now < 50 ? end - now : 50));
        else Sleep(5);
    }
}

void SharedFrames::close() {
    if (hdr_ && reader_ >= 0) cf::ipc::releaseReader(hdr_, reader_);
    reader_ = -1;
    hdr_ = nullptr;
    if (event_) CloseHandle(event_);
    event_ = nullptr;
    if (fmtEvent_) CloseHandle(fmtEvent_);
    fmtEvent_ = nullptr;
    if (base_) UnmapViewOfFile(base_);
    base_ = nullptr;
    if (section_) CloseHandle(section_);
    section_ = nullptr;
}

}  // namespace cfmf
