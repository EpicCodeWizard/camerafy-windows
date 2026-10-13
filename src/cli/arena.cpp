#include "arena.h"
#include <memoryapi.h>

namespace cf {

Arena::~Arena() {
    if (view2_) UnmapViewOfFile2(GetCurrentProcess(), view2_, 0);
    if (base_) UnmapViewOfFile2(GetCurrentProcess(), base_, 0);
    if (section_) CloseHandle(section_);
}

bool Arena::init(size_t cap, std::string& info) {
    cap_ = cap;
    mask_ = cap - 1;
    section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, (DWORD)((uint64_t)cap >> 32),
                                  (DWORD)cap, nullptr);
    if (!section_) {
        info = "CreateFileMapping(arena) failed: " + std::to_string(GetLastError());
        return false;
    }
    // Reserve 2x as one placeholder, split it, and map the section into both halves.
    uint8_t* ph = (uint8_t*)VirtualAlloc2(nullptr, nullptr, 2 * cap, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                          PAGE_NOACCESS, nullptr, 0);
    if (!ph) {
        info = "VirtualAlloc2(placeholder) failed: " + std::to_string(GetLastError());
        return false;
    }
    if (!VirtualFree(ph, cap, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        info = "placeholder split failed: " + std::to_string(GetLastError());
        VirtualFree(ph, 0, MEM_RELEASE);
        return false;
    }
    void* v1 = MapViewOfFile3(section_, nullptr, ph, 0, cap, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    void* v2 = MapViewOfFile3(section_, nullptr, ph + cap, 0, cap, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    if (!v1 || !v2) {
        info = "MapViewOfFile3(arena) failed: " + std::to_string(GetLastError());
        if (v1) UnmapViewOfFile(v1); else VirtualFree(ph, 0, MEM_RELEASE);
        if (v2) UnmapViewOfFile(v2); else VirtualFree(ph + cap, 0, MEM_RELEASE);
        return false;
    }
    base_ = (uint8_t*)v1;
    view2_ = v2;
    // Pre-touch through the first view (the second shares the same pages) and lock.
    for (size_t o = 0; o < cap; o += 4096) base_[o] = 0;
    const bool locked = VirtualLock(base_, cap) != 0;
    // Sanity: the mirror really mirrors.
    base_[0] = 0xA5;
    const bool mirrored = ((volatile uint8_t*)base_)[cap] == 0xA5;
    base_[0] = 0;
    if (!mirrored) {
        info = "arena mirror check failed";
        return false;
    }
    info = std::to_string(cap >> 20) + " MB mirrored ring, " + (locked ? "VirtualLocked" : "not locked") +
           ", 4 KB pages";
    return true;
}

}  // namespace cf
