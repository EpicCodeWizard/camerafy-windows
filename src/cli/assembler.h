#pragma once
// Turns the received Annex-B byte stream into complete access units, in place in the arena.
//
// Frame-boundary detection (PRD §7), best first:
//   Length     the sideband gave this frame's byte length: close the instant the last byte lands.
//   Aud        an AUD (or the next AU's SPS/PPS/SEI) arrived. If the sender writes the AUD right
//              after the last slice, this costs nothing.
//   NextSlice  fallback: the next frame's first slice (first_mb_in_slice == 0) arrived, one
//              frame interval after the AU was actually complete.
// Every AU records how it was closed and how long it sat complete before closing.
#include <stdint.h>
#include <atomic>
#include "arena.h"
#include "frame_types.h"
#include "options.h"
#include "nal_scan.h"
#include "h264.h"

namespace cf {

struct AssemblerStats {
    std::atomic<uint64_t> aus{0};
    std::atomic<uint64_t> idrs{0};  // AUs carrying an IDR (format switches, pauses, resyncs)
    std::atomic<uint64_t> droppedBeforeIdr{0};
    std::atomic<uint64_t> droppedRingFull{0};
    std::atomic<uint64_t> closedBy[6]{};  // indexed by CloseReason
    std::atomic<uint64_t> nals{0};
    std::atomic<uint64_t> audsSeen{0};
    std::atomic<uint64_t> seiFrameIds{0};
    std::atomic<uint64_t> frameIdGaps{0};
};

class Assembler final {
public:
    void init(Arena* arena, AuRing* out, ParamRing* params, LengthRing* lengths, const Options& o, FindStartCodeFn find);

    // New bytes [prev end, end) are in the arena; recvQpc is when recv() returned them.
    void onBytes(uint64_t end, uint64_t recvQpc);
    // Flush the open AU (end of a replayed file, or before a resync).
    void flush(uint64_t qpc);
    // Discard everything up to `pos` and wait for the next IDR (arena overflow, AU drop).
    void resync(uint64_t pos);

    const AssemblerStats& stats() const { return stats_; }
    bool haveSps() const { return sps_.valid; }
    const h264::Sps& sps() const { return sps_; }
    // Set when the assembler wants the sender to produce an IDR; cleared by the sideband.
    std::atomic<bool> wantIdr{false};
    // Latest SPS snapshot for the control thread (seqlock-free: written once per SPS change).
    std::atomic<uint32_t> spsVersion{0};
    h264::Sps spsSnapshot;

private:
    struct Chunk {
        uint64_t end, qpc;
    };
    uint64_t qpcOfByte(uint64_t pos) const;
    void beginNal(uint64_t prefixPos, uint8_t type, uint64_t qpc);
    void completeNal(uint64_t endPos);
    void closeAu(uint64_t endPos, CloseReason why, uint64_t qpc);
    bool startsNewAu(uint8_t type, uint64_t nalPayloadPos, uint64_t avail);
    void cacheParam(uint8_t type, uint64_t start, uint64_t end);
    void parseSei(uint64_t start, uint64_t end);
    bool tryLengthClose(uint64_t end, uint64_t qpc);

    Arena* arena_ = nullptr;
    AuRing* out_ = nullptr;
    ParamRing* params_ = nullptr;
    LengthRing* lengths_ = nullptr;
    FindStartCodeFn find_ = nullptr;
    BoundaryMode mode_ = BoundaryMode::Auto;

    uint64_t scan_ = 0;       // next position to look for a start code
    uint64_t avail_ = 0;      // bytes [0, avail_) are in the arena
    bool inNal_ = false;
    uint64_t nalStart_ = 0;   // prefix position of the current NAL
    uint8_t nalType_ = 0;

    bool auOpen_ = false;
    uint64_t auStart_ = 0;
    bool auHasVcl_ = false;
    uint32_t auFlags_ = 0;
    uint16_t auSlices_ = 0;
    uint64_t auFirstQpc_ = 0;
    uint64_t auPhoneId_ = 0;   // frameId from the sender's SEI

    bool waitingIdr_ = true;
    uint64_t seq_ = 0;
    uint64_t lastPhoneId_ = 0;

    // Length mode
    bool haveLen_ = false;
    FrameLength curLen_{};
    uint64_t lenFramePos_ = 0;    // stream position where the next length-delimited frame starts

    // Recent recv chunks, to find when a given byte arrived.
    static constexpr uint32_t kChunkHist = 256;
    Chunk chunks_[kChunkHist] = {};
    uint32_t chunkHead_ = 0;

    // Parameter sets (raw Annex-B, start code included).
    uint8_t spsBytes_[512] = {};
    uint32_t spsLen_ = 0;
    uint8_t ppsBytes_[512] = {};
    uint32_t ppsLen_ = 0;
    h264::Sps sps_{};
    bool paramsInThisAu_ = false;

    AssemblerStats stats_;
};

}  // namespace cf
