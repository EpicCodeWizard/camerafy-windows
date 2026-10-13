#pragma once
// Records handed between pipeline stages. All trivially copyable and fixed size; each stage
// adds its own timestamps and passes the record on by value.
#include <stdint.h>
#include "spsc_ring.h"

namespace cf {

enum AuFlags : uint32_t {
    kAuIdr = 1u << 0,
    kAuHasSps = 1u << 1,
    kAuHasPps = 1u << 2,
    kAuNeedsParams = 1u << 3,     // decoder must feed the cached SPS/PPS first
    kAuSeqChange = 1u << 4,       // SPS differs from the previous one
    kAuSkip = 1u << 31,           // not decodable (pre-IDR); decoder only releases the bytes
};

enum class CloseReason : uint8_t {
    None = 0,
    Aud,          // an access unit delimiter arrived (sender emits it right after the last slice)
    NonVcl,       // SPS/PPS/SEI of the next AU arrived
    NextSlice,    // fallback: the next frame's first slice arrived (one frame interval late)
    Length,       // sideband told us the frame's byte length
    EndOfStream,
};

// QPC ticks unless noted. 0 means the stage was not reached.
struct FrameTiming {
    uint64_t seq;             // local AU sequence number
    uint64_t phoneFrameId;    // sender's frame id (0 if unknown)
    uint64_t phoneCaptureUs;  // sender clock, µs
    uint64_t phoneEncodeOutUs;
    uint32_t bytes;
    uint32_t flags;           // AuFlags
    uint16_t slices;
    CloseReason close;
    uint8_t pad0;
    uint32_t pad1;
    uint64_t tRecvFirst;      // recv() returned the AU's first byte
    uint64_t tRecvLast;       // recv() returned the AU's last byte
    uint64_t tAuComplete;     // assembler closed the AU
    uint64_t tDecodeDequeue;  // decode thread picked it up
    uint64_t tDecodeSubmit;   // cuvidDecodePicture returned (inside the parse call)
    uint64_t tParseReturn;    // cuvidParseVideoData returned
    uint64_t tMapDequeue;     // mapper picked it up
    uint64_t tDecodeDone;     // cuvidGetDecodeStatus reported success (if polled)
    uint64_t tMapped;         // cuvidMapVideoFrame64 returned
    uint64_t tDmaDone;        // readback into the shared slot complete
    uint64_t tPublishedDs;
    uint64_t tPublishedMf;
    float gpuDmaUs;           // cuEventElapsedTime around the readback
    float pad2;
};

struct AuItem {
    uint64_t start, end;  // arena positions
    FrameTiming t;
};

struct PicItem {
    int32_t picIdx;
    uint32_t pad;
    FrameTiming t;
};

struct ParamBlob {
    uint32_t len;
    uint8_t bytes[1020];  // Annex-B SPS + PPS with start codes
};

using AuRing = SpscRing<AuItem, 256>;
using PicRing = SpscRing<PicItem, 64>;
using ParamRing = SpscRing<ParamBlob, 16>;
using TimingRing = SpscRing<FrameTiming, 4096>;

// recv -> assembler: (arena end position, QPC when recv returned)
struct RecvChunk {
    uint64_t end;
    uint64_t qpc;
};
using ChunkRing = SpscRing<RecvChunk, 4096>;

// sideband -> assembler (Length mode): byte length of each frame in stream order.
struct FrameLength {
    uint64_t phoneFrameId;
    uint64_t bytes;
};
using LengthRing = SpscRing<FrameLength, 1024>;

}  // namespace cf
