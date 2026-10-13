#pragma once
// Annex-B start-code scanning over a contiguous buffer.
// find*() return the offset of the first "00 00 01" prefix at or after `from`
// whose three bytes all lie before `end`, or `end` if there is none.
// Callers re-scan from max(from, end - 2) after new bytes arrive, so a prefix that
// straddles a recv boundary is found without copying anything.
#include <stdint.h>
#include <stddef.h>
#include <immintrin.h>

namespace cf {

inline size_t findStartCodeScalar(const uint8_t* buf, size_t from, size_t end) {
    if (end < 3) return end;
    for (size_t i = from; i + 2 < end; ++i) {
        if (buf[i + 2] > 1) { i += 2; continue; }  // neither of the next two positions can start here
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) return i;
    }
    return end;
}

// 32-byte loads stepping by 30: bit k of the candidate mask means bytes k,k+1 are zero
// and byte k+2 is one, valid for k in [0,29]. Fully vectorized; no scalar re-check.
inline size_t findStartCodeAvx2(const uint8_t* buf, size_t from, size_t end) {
    size_t i = from;
    const __m256i zero = _mm256_setzero_si256();
    const __m256i one = _mm256_set1_epi8(1);
    while (i + 32 <= end) {
        const __m256i v = _mm256_loadu_si256((const __m256i*)(buf + i));
        const uint32_t z = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, zero));
        if (z) {
            const uint32_t o = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, one));
            const uint32_t cand = z & (z >> 1) & (o >> 2) & 0x3FFFFFFFu;
            if (cand) return i + _tzcnt_u32(cand);
        }
        i += 30;
    }
    return findStartCodeScalar(buf, i, end);
}

// Chosen once at startup (see --bench); the hot path calls through this.
using FindStartCodeFn = size_t (*)(const uint8_t*, size_t, size_t);

// H.264 NAL unit types we care about.
enum : uint8_t {
    kNalSlice = 1,
    kNalIdr = 5,
    kNalSei = 6,
    kNalSps = 7,
    kNalPps = 8,
    kNalAud = 9,
    kNalEndSeq = 10,
    kNalEndStream = 11,
    kNalFiller = 12,
};

inline bool isVcl(uint8_t t) { return t >= 1 && t <= 5; }

}  // namespace cf
