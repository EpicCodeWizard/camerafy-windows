#include "assembler.h"
#include "console.h"
#include "qpc.h"
#include <string.h>

namespace cf {

void Assembler::init(Arena* arena, AuRing* out, ParamRing* params, LengthRing* lengths, const Options& o,
                     FindStartCodeFn find) {
    arena_ = arena;
    out_ = out;
    params_ = params;
    lengths_ = lengths;
    mode_ = o.boundary;
    find_ = find;
}

uint64_t Assembler::qpcOfByte(uint64_t pos) const {
    // Newest chunk first; walk back while the previous chunk still covers pos.
    uint32_t i = chunkHead_ - 1;
    const uint32_t oldest = chunkHead_ > kChunkHist ? chunkHead_ - kChunkHist : 0;
    while (i > oldest && chunks_[(i - 1) % kChunkHist].end > pos) --i;
    return chunks_[i % kChunkHist].qpc;
}

void Assembler::onBytes(uint64_t end, uint64_t recvQpc) {
    chunks_[chunkHead_++ % kChunkHist] = {end, recvQpc};
    avail_ = end;
    const bool lengthMode = mode_ == BoundaryMode::Length;
    while (true) {
        if (lengthMode) tryLengthClose(end, cf::qpc());
        const uint64_t from = scan_;
        if (end < from + 3) break;
        const uint8_t* p = arena_->at(from);
        const size_t span = (size_t)(end - from);
        const size_t k = find_(p, 0, span);
        if (k >= span) {
            scan_ = end - 2;
            break;
        }
        const uint64_t s = from + k;  // 00 00 01 at s
        if (s + 3 >= end) {
            scan_ = s;  // header byte not here yet
            break;
        }
        const uint8_t type = *arena_->at(s + 3) & 0x1F;
        // A VCL NAL's first_mb_in_slice decides whether it starts a new picture; wait for a
        // few bytes of slice header. They almost always arrive in the same recv.
        if (isVcl(type) && end - (s + 4) < 8) {
            scan_ = s;
            break;
        }
        const uint64_t minPrefix = inNal_ ? nalStart_ + 4 : 0;
        const uint64_t prefix = (s >= 1 && s - 1 >= minPrefix && *arena_->at(s - 1) == 0) ? s - 1 : s;
        if (lengthMode) tryLengthClose(prefix, cf::qpc());
        if (inNal_) completeNal(prefix);
        beginNal(prefix, type, recvQpc);
        scan_ = s + 3;
    }
    if (lengthMode) tryLengthClose(end, cf::qpc());
}

bool Assembler::startsNewAu(uint8_t type, uint64_t payloadPos, uint64_t avail) {
    if (!auOpen_ || !auHasVcl_) return false;
    const bool vcl = isVcl(type);
    if (mode_ == BoundaryMode::NextFrame && !vcl) return false;
    if (!vcl) return type == kNalAud || type == kNalSei || type == kNalSps || type == kNalPps || (type >= 14 && type <= 18);
    // first_mb_in_slice == 0 marks the first slice of a new picture (no ASO in our streams).
    uint8_t raw[16], rbsp[16];
    const size_t n = (size_t)((avail - payloadPos) < 16 ? (avail - payloadPos) : 16);
    memcpy(raw, arena_->at(payloadPos), n);
    const size_t m = h264::unescape(raw, n, rbsp, sizeof(rbsp));
    h264::BitReader br(rbsp, m);
    return br.ue() == 0;
}

void Assembler::beginNal(uint64_t prefix, uint8_t type, uint64_t recvQpc) {
    stats_.nals.fetch_add(1, std::memory_order_relaxed);
    if (type == kNalAud) stats_.audsSeen.fetch_add(1, std::memory_order_relaxed);
    if (mode_ != BoundaryMode::Length) {
        // payload begins after the 3-byte start code and the 1-byte NAL header
        uint64_t sc = prefix;
        while (*arena_->at(sc) == 0) ++sc;  // skip zero_byte(s); sc now at the 0x01
        const uint64_t payload = sc + 2;
        if (startsNewAu(type, payload, avail_)) {
            CloseReason why = CloseReason::NextSlice;
            if (type == kNalAud) why = CloseReason::Aud;
            else if (!isVcl(type)) why = CloseReason::NonVcl;
            closeAu(prefix, why, cf::qpc());
        }
    }
    if (!auOpen_) {
        auOpen_ = true;
        auStart_ = prefix;
        auHasVcl_ = false;
        auFlags_ = 0;
        auSlices_ = 0;
        auPhoneId_ = 0;
        auFirstQpc_ = 0;
        (void)recvQpc;
    }
    // The sender's AUD terminates the previous frame and so opens this AU; the frame's own
    // bytes start with the next NAL. Stamp arrival from there.
    if (!auFirstQpc_ && type != kNalAud) auFirstQpc_ = qpcOfByte(prefix);
    inNal_ = true;
    nalStart_ = prefix;
    nalType_ = type;
    if (isVcl(type)) {
        auHasVcl_ = true;
        ++auSlices_;
        if (type == kNalIdr) auFlags_ |= kAuIdr;
    } else if (type == kNalSps) {
        auFlags_ |= kAuHasSps;
    } else if (type == kNalPps) {
        auFlags_ |= kAuHasPps;
    }
}

void Assembler::completeNal(uint64_t endPos) {
    if (nalType_ == kNalSps || nalType_ == kNalPps) cacheParam(nalType_, nalStart_, endPos);
    else if (nalType_ == kNalSei) parseSei(nalStart_, endPos);
    inNal_ = false;
}

// Frame ID SEI: user_data_unregistered (type 5), size 20, fixed UUID, frameId uint32 LE.
// Emulation prevention is applied by the sender (always for frameIds 1..255), so unescape.
void Assembler::parseSei(uint64_t start, uint64_t end) {
    static const uint8_t kUuid[16] = {0xC4, 0x4E, 0x5F, 0x92, 0x7B, 0x3A, 0x4D, 0x1E,
                                      0x9A, 0x6C, 0x2F, 0x81, 0xD5, 0xB7, 0xE3, 0x48};
    uint64_t h = start;
    while (h < end && *arena_->at(h) == 0) ++h;
    h += 2;  // past 0x01 and the NAL header byte
    if (h >= end) return;
    uint8_t raw[48], rbsp[48];
    const size_t n = (size_t)((end - h) < sizeof(raw) ? (end - h) : sizeof(raw));
    memcpy(raw, arena_->at(h), n);
    const size_t m = h264::unescape(raw, n, rbsp, sizeof(rbsp));
    if (m < 22 || rbsp[0] != 5 || rbsp[1] != 20 || memcmp(rbsp + 2, kUuid, 16) != 0) return;
    uint32_t id;
    memcpy(&id, rbsp + 18, 4);
    auPhoneId_ = id;
    stats_.seiFrameIds.fetch_add(1, std::memory_order_relaxed);
    // frameIds are consecutive within a connection; 1 starts a new one (or follows a wrap).
    if (lastPhoneId_ && id != 1 && id != lastPhoneId_ + 1) stats_.frameIdGaps.fetch_add(1, std::memory_order_relaxed);
    lastPhoneId_ = id;
}

void Assembler::cacheParam(uint8_t type, uint64_t start, uint64_t end) {
    while (end > start && *arena_->at(end - 1) == 0) --end;  // trailing_zero_8bits
    const uint64_t len = end - start;
    if (len < 5 || len > 512) return;
    uint8_t* dst = type == kNalSps ? spsBytes_ : ppsBytes_;
    uint32_t& dlen = type == kNalSps ? spsLen_ : ppsLen_;
    const bool same = dlen == len && !memcmp(dst, arena_->at(start), (size_t)len);
    if (same) return;
    memcpy(dst, arena_->at(start), (size_t)len);
    dlen = (uint32_t)len;
    if (type == kNalSps) {
        uint32_t h = 0;
        while (h < dlen && dst[h] == 0) ++h;
        h += 2;  // past 0x01 to the NAL header
        uint8_t rbsp[512];
        const size_t m = h264::unescape(dst + h, dlen - h, rbsp, sizeof(rbsp));
        h264::Sps s;
        if (h264::parseSps(rbsp, m, s)) {
            if (sps_.valid) auFlags_ |= kAuSeqChange;
            sps_ = s;
            spsSnapshot = s;
            spsVersion.fetch_add(1, std::memory_order_release);
        }
    }
}

void Assembler::closeAu(uint64_t endPos, CloseReason why, uint64_t qpc) {
    if (!auOpen_) return;
    auOpen_ = false;
    AuItem it{};
    it.start = auStart_;
    it.end = endPos;
    FrameTiming& t = it.t;
    t.bytes = (uint32_t)(endPos - auStart_);
    t.flags = auFlags_;
    t.slices = auSlices_;
    t.phoneFrameId = auPhoneId_;
    t.close = why;
    t.tRecvFirst = auFirstQpc_;
    t.tRecvLast = qpcOfByte(endPos - 1);
    t.tAuComplete = qpc;
    stats_.closedBy[(int)why].fetch_add(1, std::memory_order_relaxed);

    bool skip = !auHasVcl_;
    if (!skip && waitingIdr_) {
        const bool idr = (auFlags_ & kAuIdr) != 0;
        const bool params = ((auFlags_ & (kAuHasSps | kAuHasPps)) == (kAuHasSps | kAuHasPps)) || (spsLen_ && ppsLen_);
        if (idr && params) {
            waitingIdr_ = false;
        } else {
            skip = true;
            stats_.droppedBeforeIdr.fetch_add(1, std::memory_order_relaxed);
            wantIdr.store(true, std::memory_order_relaxed);
        }
    }
    if (skip) {
        // The decoder still has to release these bytes; send a skip marker in order.
        it.t.flags |= kAuSkip;
        if (!out_->push(it)) hotEvent(kEvAssembler, Ev::AuRingFull);
        return;
    }
    if ((auFlags_ & kAuIdr) && (auFlags_ & (kAuHasSps | kAuHasPps)) != (kAuHasSps | kAuHasPps)) {
        ParamBlob b;
        b.len = 0;
        if (spsLen_ + ppsLen_ <= sizeof(b.bytes)) {
            memcpy(b.bytes, spsBytes_, spsLen_);
            memcpy(b.bytes + spsLen_, ppsBytes_, ppsLen_);
            b.len = spsLen_ + ppsLen_;
            if (params_->push(b)) it.t.flags |= kAuNeedsParams;
        }
    }
    it.t.seq = ++seq_;
    stats_.aus.fetch_add(1, std::memory_order_relaxed);
    if (auFlags_ & kAuIdr) stats_.idrs.fetch_add(1, std::memory_order_relaxed);
    if (!out_->push(it)) {
        // Losing a reference frame corrupts everything until the next IDR, so ask for one.
        stats_.droppedRingFull.fetch_add(1, std::memory_order_relaxed);
        hotEvent(kEvAssembler, Ev::AuRingFull, (uint32_t)it.t.seq);
        waitingIdr_ = true;
        wantIdr.store(true, std::memory_order_relaxed);
    }
}

bool Assembler::tryLengthClose(uint64_t limit, uint64_t qpc) {
    bool closed = false;
    while (true) {
        if (!haveLen_) {
            if (!lengths_ || !lengths_->pop(curLen_)) return closed;
            haveLen_ = true;
        }
        const uint64_t endPos = lenFramePos_ + curLen_.bytes;
        if (endPos > limit || endPos > avail_) return closed;
        if (!auOpen_) {
            // Frame bytes were all non-start-code data (should not happen) or already closed.
            auOpen_ = true;
            auStart_ = lenFramePos_;
            auHasVcl_ = true;
            auFlags_ = 0;
            auSlices_ = 0;
            auFirstQpc_ = qpcOfByte(lenFramePos_);
        }
        if (auStart_ != lenFramePos_) {
            hotEvent(kEvAssembler, Ev::StreamResync, 0, auStart_);
            auStart_ = lenFramePos_;
        }
        if (inNal_) completeNal(endPos);
        closeAu(endPos, CloseReason::Length, qpc);
        // The AU's own phone frame id comes from the length record.
        lenFramePos_ = endPos;
        haveLen_ = false;
        closed = true;
    }
}

void Assembler::flush(uint64_t qpc) {
    if (!auOpen_ || !auHasVcl_) return;
    if (inNal_) completeNal(avail_);
    closeAu(avail_, CloseReason::EndOfStream, qpc);
}

void Assembler::resync(uint64_t pos) {
    auOpen_ = false;
    inNal_ = false;
    scan_ = pos;
    lenFramePos_ = pos;
    haveLen_ = false;
    waitingIdr_ = true;
    wantIdr.store(true, std::memory_order_relaxed);
    hotEvent(kEvAssembler, Ev::StreamResync, 0, pos);
}

}  // namespace cf
