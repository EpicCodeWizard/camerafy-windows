#pragma once
// Byte arena for the received elementary stream: one section mapped twice back-to-back
// (a "magic ring"), so any span of up to `capacity` bytes starting anywhere is contiguous.
// recv() writes straight into it, the assembler scans it in place, and NVDEC's parser
// reads access units from it. Payload bytes are never copied between stages.
#include <windows.h>
#include <atomic>
#include <stdint.h>
#include <string>
#include "spsc_ring.h"

namespace cf {

class Arena final {
public:
    ~Arena();
    bool init(size_t capacityPow2, std::string& info);

    uint8_t* at(uint64_t pos) const { return base_ + (pos & mask_); }
    size_t capacity() const { return cap_; }

    // Producer (recv) position: bytes [0, write) have been received.
    alignas(kCacheLine) std::atomic<uint64_t> write{0};
    // Consumer (decode) position: bytes [0, release) may be overwritten.
    alignas(kCacheLine) std::atomic<uint64_t> release{0};

private:
    HANDLE section_ = nullptr;
    uint8_t* base_ = nullptr;
    void* view2_ = nullptr;
    size_t cap_ = 0;
    uint64_t mask_ = 0;
};

}  // namespace cf
