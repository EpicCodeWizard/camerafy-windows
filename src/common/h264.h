#pragma once
// Minimal H.264 header parsing. Only the ~64 bytes we read ourselves are unescaped;
// slice data is never touched.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "nal_scan.h"

namespace cf::h264 {

// Removes emulation-prevention bytes (00 00 03 -> 00 00) from src into dst.
// Returns the number of bytes written; stops at dstCap.
inline size_t unescape(const uint8_t* src, size_t n, uint8_t* dst, size_t dstCap) {
    size_t o = 0;
    int zeros = 0;
    for (size_t i = 0; i < n && o < dstCap; ++i) {
        const uint8_t b = src[i];
        if (zeros >= 2 && b == 3) {
            zeros = 0;
            continue;
        }
        dst[o++] = b;
        zeros = (b == 0) ? zeros + 1 : 0;
    }
    return o;
}

class BitReader final {
public:
    BitReader(const uint8_t* p, size_t n) : p_(p), bits_(n * 8) {}
    bool overrun() const { return pos_ > bits_; }
    size_t pos() const { return pos_; }

    uint32_t u(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | bit();
        return v;
    }
    uint32_t bit() {
        if (pos_ >= bits_) { ++pos_; return 0; }
        const uint32_t b = (p_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1;
        ++pos_;
        return b;
    }
    uint32_t ue() {
        int lz = 0;
        while (bit() == 0) {
            if (++lz > 31 || overrun()) return 0;
        }
        if (lz == 0) return 0;
        return ((1u << lz) - 1) + u(lz);
    }
    int32_t se() {
        const uint32_t k = ue();
        return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
    }

private:
    const uint8_t* p_;
    size_t bits_;
    size_t pos_ = 0;
};

struct Sps {
    bool valid = false;
    uint8_t profileIdc = 0, constraintFlags = 0, levelIdc = 0;
    uint32_t spsId = 0;
    uint32_t chromaFormatIdc = 1;
    uint32_t bitDepthLuma = 8, bitDepthChroma = 8;
    uint32_t log2MaxFrameNum = 4;
    uint32_t pocType = 0;
    uint32_t log2MaxPocLsb = 4;
    bool deltaPicOrderAlwaysZero = false;
    uint32_t numRefFrames = 0;
    bool frameMbsOnly = true;
    uint32_t widthMbs = 0, heightMapUnits = 0;
    uint32_t cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    uint32_t width = 0, height = 0;  // after cropping
    bool vuiPresent = false;
    bool timingInfoPresent = false;
    uint32_t numUnitsInTick = 0, timeScale = 0;
    bool fullRange = false;
};

struct Pps {
    bool valid = false;
    uint32_t ppsId = 0, spsId = 0;
    bool bottomFieldPicOrderInFramePresent = false;
};

struct SliceHeader {
    bool valid = false;
    uint32_t firstMb = 0;
    uint32_t sliceType = 0;
    uint32_t ppsId = 0;
    uint32_t frameNum = 0;
    bool fieldPic = false, bottomField = false;
    uint32_t idrPicId = 0;
    uint32_t pocLsb = 0;
};

inline void skipScalingList(BitReader& br, int size) {
    int last = 8, next = 8;
    for (int j = 0; j < size; ++j) {
        if (next != 0) next = (last + br.se() + 256) % 256;
        last = (next == 0) ? last : next;
    }
}

// rbsp: unescaped bytes *after* the one-byte NAL header.
inline bool parseSps(const uint8_t* rbsp, size_t n, Sps& s) {
    BitReader br(rbsp, n);
    s = Sps{};
    s.profileIdc = (uint8_t)br.u(8);
    s.constraintFlags = (uint8_t)br.u(8);
    s.levelIdc = (uint8_t)br.u(8);
    s.spsId = br.ue();
    const uint8_t p = s.profileIdc;
    if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 ||
        p == 128 || p == 138 || p == 139 || p == 134 || p == 135) {
        s.chromaFormatIdc = br.ue();
        if (s.chromaFormatIdc == 3) br.u(1);
        s.bitDepthLuma = br.ue() + 8;
        s.bitDepthChroma = br.ue() + 8;
        br.u(1);  // qpprime_y_zero_transform_bypass_flag
        if (br.u(1)) {
            const int lists = (s.chromaFormatIdc != 3) ? 8 : 12;
            for (int i = 0; i < lists; ++i)
                if (br.u(1)) skipScalingList(br, i < 6 ? 16 : 64);
        }
    }
    s.log2MaxFrameNum = br.ue() + 4;
    s.pocType = br.ue();
    if (s.pocType == 0) {
        s.log2MaxPocLsb = br.ue() + 4;
    } else if (s.pocType == 1) {
        s.deltaPicOrderAlwaysZero = br.u(1);
        br.se();
        br.se();
        const uint32_t cycle = br.ue();
        if (cycle > 255) return false;
        for (uint32_t i = 0; i < cycle; ++i) br.se();
    }
    s.numRefFrames = br.ue();
    br.u(1);  // gaps_in_frame_num_value_allowed_flag
    s.widthMbs = br.ue() + 1;
    s.heightMapUnits = br.ue() + 1;
    s.frameMbsOnly = br.u(1);
    if (!s.frameMbsOnly) br.u(1);  // mb_adaptive_frame_field_flag
    br.u(1);                       // direct_8x8_inference_flag
    if (br.u(1)) {
        s.cropLeft = br.ue();
        s.cropRight = br.ue();
        s.cropTop = br.ue();
        s.cropBottom = br.ue();
    }
    s.vuiPresent = br.u(1);
    if (s.vuiPresent) {
        if (br.u(1)) {  // aspect_ratio_info_present_flag
            if (br.u(8) == 255) br.u(32);
        }
        if (br.u(1)) br.u(1);  // overscan
        if (br.u(1)) {         // video_signal_type_present_flag
            br.u(3);
            s.fullRange = br.u(1);
            if (br.u(1)) br.u(24);
        }
        if (br.u(1)) { br.ue(); br.ue(); }  // chroma_loc_info
        s.timingInfoPresent = br.u(1);
        if (s.timingInfoPresent) {
            s.numUnitsInTick = br.u(32);
            s.timeScale = br.u(32);
        }
    }
    const uint32_t frameHeightMbs = (2 - (s.frameMbsOnly ? 1 : 0)) * s.heightMapUnits;
    uint32_t cropUnitX = 1, cropUnitY = 2 - (s.frameMbsOnly ? 1 : 0);
    if (s.chromaFormatIdc == 1) { cropUnitX = 2; cropUnitY *= 2; }
    else if (s.chromaFormatIdc == 2) { cropUnitX = 2; }
    s.width = s.widthMbs * 16 - cropUnitX * (s.cropLeft + s.cropRight);
    s.height = frameHeightMbs * 16 - cropUnitY * (s.cropTop + s.cropBottom);
    s.valid = !br.overrun() && s.width > 0 && s.height > 0;
    return s.valid;
}

inline bool parsePps(const uint8_t* rbsp, size_t n, Pps& p) {
    BitReader br(rbsp, n);
    p = Pps{};
    p.ppsId = br.ue();
    p.spsId = br.ue();
    br.u(1);  // entropy_coding_mode_flag
    p.bottomFieldPicOrderInFramePresent = br.u(1);
    p.valid = !br.overrun() && p.ppsId < 256 && p.spsId < 32;
    return p.valid;
}

// nalType is needed for idr_pic_id. Parses only up to pic_order_cnt_lsb.
inline bool parseSliceHeader(const uint8_t* rbsp, size_t n, uint8_t nalType, const Sps& sps, SliceHeader& h) {
    BitReader br(rbsp, n);
    h = SliceHeader{};
    h.firstMb = br.ue();
    h.sliceType = br.ue();
    h.ppsId = br.ue();
    if (!sps.valid) { h.valid = !br.overrun(); return h.valid; }
    h.frameNum = br.u((int)sps.log2MaxFrameNum);
    if (!sps.frameMbsOnly) {
        h.fieldPic = br.u(1);
        if (h.fieldPic) h.bottomField = br.u(1);
    }
    if (nalType == cf::kNalIdr) h.idrPicId = br.ue();
    if (sps.pocType == 0) h.pocLsb = br.u((int)sps.log2MaxPocLsb);
    h.valid = !br.overrun();
    return h.valid;
}

}  // namespace cf::h264
