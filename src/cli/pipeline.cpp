#include "pipeline.h"
#include "console.h"
#include "qpc.h"
#include "precise_sleep.h"
#include <ws2tcpip.h>
#include <stdio.h>

namespace cf {

namespace {
// 16 MB holds several 4K IDRs (~2-4 MB each); every page of it is resident (locked).
constexpr size_t kArenaBytes = 16u << 20;
constexpr int kRecvChunk = 256 * 1024;
constexpr int kSocketRcvBuf = 4 << 20;

FindStartCodeFn pickScanner() {
    // AVX2 is a hard requirement of the build (/arch:AVX2), so it is always available here.
    return findStartCodeAvx2;
}
}  // namespace

bool Pipeline::init(const Options& o, std::string& err) {
    opt_ = o;
    if (!arena.init(kArenaBytes, arenaInfo)) {
        err = arenaInfo;
        return false;
    }
    if (!publisher.init(!o.noDs, !o.noMf, o.readerSpinUs, publisherInfo)) {
        err = publisherInfo;
        return false;
    }
    assembler.init(&arena, &aus, &params, &lengths, o, pickScanner());
    if (!decoder.init(o, &arena, &aus, &params, &timing, &publisher, err)) return false;
    publisher.attachCuda(decoder.context(), o.hostRegister);
    telemetry.init(o, &timing, &publisher, &assembler, &decoder, &phone, &clock);
    plan = tuning::planCores(o);
    tuning::memCheckpoint("pipeline initialised");
    return true;
}

HANDLE Pipeline::spawn(LPTHREAD_START_ROUTINE fn) {
    HANDLE h = CreateThread(nullptr, 1 << 20, fn, this, 0, nullptr);
    if (h) threads_.push_back(h);
    return h;
}

bool Pipeline::startSocket(SOCKET video, std::string& err) {
    video_ = video;
    int rcv = kSocketRcvBuf;
    setsockopt(video_, SOL_SOCKET, SO_RCVBUF, (const char*)&rcv, sizeof(rcv));
    BOOL one = TRUE;
    setsockopt(video_, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    stop_ = false;
    sourceEnded_ = false;
    telemetry.start();
    if (opt_.keepalive != Keepalive::Off) spawn(keepaliveProc);
    if ((!decodeInline() && !spawn(decodeProc)) || !spawn(mapperProc) ||
        (!opt_.inlineAssembler && !spawn(assemblerProc)) || !(recvThread_ = spawn(recvProc))) {
        err = "CreateThread failed: " + std::to_string(GetLastError());
        return false;
    }
    return true;
}

bool Pipeline::reconnect(SOCKET video, std::string& err) {
    if (recvThread_) WaitForSingleObject(recvThread_, 2000);
    if (video_ != INVALID_SOCKET) closesocket(video_);
    video_ = video;
    int rcv = kSocketRcvBuf;
    setsockopt(video_, SOL_SOCKET, SO_RCVBUF, (const char*)&rcv, sizeof(rcv));
    // The new connection starts with an IDR; drop any half-received AU.
    if (opt_.inlineAssembler) {
        assembler.resync(writePos_);
    } else {
        while (!chunks.push(RecvChunk{writePos_, 1})) _mm_pause();
    }
    sourceEnded_ = false;
    recvThread_ = spawn(recvProc);
    if (!recvThread_) {
        err = "CreateThread failed: " + std::to_string(GetLastError());
        return false;
    }
    return true;
}

bool Pipeline::startReplay(std::string& err) {
    if (!loadReplay(err)) return false;
    stop_ = false;
    sourceEnded_ = false;
    telemetry.start();
    if (opt_.keepalive != Keepalive::Off) spawn(keepaliveProc);
    if ((!decodeInline() && !spawn(decodeProc)) || !spawn(mapperProc) ||
        (!opt_.inlineAssembler && !spawn(assemblerProc)) || !spawn(replayProc)) {
        err = "CreateThread failed: " + std::to_string(GetLastError());
        return false;
    }
    return true;
}

void Pipeline::stop() {
    stop_ = true;
    if (video_ != INVALID_SOCKET) {
        shutdown(video_, SD_BOTH);
        closesocket(video_);
        video_ = INVALID_SOCKET;
    }
    bool allExited = true;
    for (HANDLE h : threads_) {
        allExited &= WaitForSingleObject(h, 3000) == WAIT_OBJECT_0;
        CloseHandle(h);
    }
    threads_.clear();
    telemetry.stop();
    // A thread still inside a CUDA call (or a GPU kernel that never ended) would make
    // cuMemHostUnregister wait forever: skip CUDA teardown then, process exit releases it.
    publisher.stop(allExited && !decoder.gpuStuck());
}

DWORD WINAPI Pipeline::recvProc(void* s) { ((Pipeline*)s)->recvLoop(); return 0; }
DWORD WINAPI Pipeline::replayProc(void* s) { ((Pipeline*)s)->replayLoop(); return 0; }
DWORD WINAPI Pipeline::assemblerProc(void* s) { ((Pipeline*)s)->assemblerLoop(); return 0; }

DWORD WINAPI Pipeline::decodeProc(void* s) {
    auto* p = (Pipeline*)s;
    tuning::applyToCurrentThread(tuning::Role::Decode, p->opt_.spinStages, p->opt_, p->plan);
    p->decoder.runDecodeLoop(p->stop_);
    return 0;
}

DWORD WINAPI Pipeline::keepaliveProc(void* s) {
    auto* p = (Pipeline*)s;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    p->decoder.runKeepaliveLoop(p->stop_);
    return 0;
}

DWORD WINAPI Pipeline::mapperProc(void* s) {
    auto* p = (Pipeline*)s;
    tuning::applyToCurrentThread(tuning::Role::Mapper, p->opt_.spinStages, p->opt_, p->plan);
    p->decoder.runMapperLoop(p->stop_);
    return 0;
}

void Pipeline::assemblerLoop() {
    tuning::applyToCurrentThread(tuning::Role::Assembler, opt_.spinStages, opt_, plan);
    RecvChunk c;
    const bool lengthMode = opt_.boundary == BoundaryMode::Length;
    while (true) {
        bool got = opt_.spinStages ? chunks.popSpin(c, stop_) : chunks.popBlocking(c, stop_);
        if (!got) break;
        if (c.qpc == 0) {  // end-of-source marker
            assembler.flush(qpc());
            continue;
        }
        if (c.qpc == 1) {  // reconnect marker: discard the partial AU, wait for the IDR
            assembler.resync(c.end);
            continue;
        }
        assembler.onBytes(c.end, c.qpc);
        if (!opt_.spinStages) aus.notify();
        (void)lengthMode;
    }
}

// Pushes a received chunk to the assembler (or runs it inline). The chunk ring never drops:
// if it is momentarily full the end position is simply carried into the next push.
void Pipeline::deliver(uint64_t& w, const uint8_t*, size_t n, ChunkRing*) {
    w += n;
    const uint64_t t = qpc();
    arena.write.store(w, std::memory_order_release);
    if (opt_.inlineAssembler) {
        assembler.onBytes(w, t);
        if (decodeInline()) decoder.drainAus();  // parse + submit right here, no hand-off
        else if (!opt_.spinStages) aus.notify();
        return;
    }
    while (!chunks.push(RecvChunk{w, t})) {
        if (stop_.load(std::memory_order_relaxed)) return;
        _mm_pause();
    }
    if (!opt_.spinStages) chunks.notify();
}

void Pipeline::recvLoop() {
    // Blocking recv wakes directly on data arrival; it gets MMCSS rather than spinning.
    tuning::applyToCurrentThread(tuning::Role::Recv, false, opt_, plan);
    if (decodeInline()) g_cu.cuCtxSetCurrent(decoder.context());
    uint64_t w = writePos_;
    const size_t cap = arena.capacity();
    bool overflowLogged = false;
    while (!stop_.load(std::memory_order_relaxed)) {
        const uint64_t rel = arena.release.load(std::memory_order_acquire);
        size_t freeBytes = cap - (size_t)(w - rel);
        if (freeBytes < (size_t)kRecvChunk) {
            // The decoder is >60 MB behind. Overwriting unreleased bytes would corrupt frames in
            // flight, so wait; TCP flow control pushes back on the phone.
            if (!overflowLogged) hotEvent(kEvRecv, Ev::ArenaOverflow, (uint32_t)freeBytes, w);
            overflowLogged = true;
            Sleep(1);  // never busy-wait at elevated priority: a stuck decoder must not pin a core
            continue;
        }
        overflowLogged = false;
        const int n = recv(video_, (char*)arena.at(w), kRecvChunk, 0);
        if (n <= 0) {
            if (!stop_.load()) hotEvent(kEvRecv, Ev::RecvError, (uint32_t)WSAGetLastError());
            break;
        }
        deliver(w, nullptr, (size_t)n, &chunks);
    }
    writePos_ = w;
    sourceEnded_ = true;
}

// ---------------------------------------------------------------- replay

bool Pipeline::loadReplay(std::string& err) {
    FILE* f = _wfopen(opt_.replayPath.c_str(), L"rb");
    if (!f) {
        err = "cannot open " + narrow(opt_.replayPath);
        return false;
    }
    _fseeki64(f, 0, SEEK_END);
    const int64_t sz = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    file_.resize((size_t)sz + 64);
    fread(file_.data(), 1, (size_t)sz, f);
    fclose(f);
    file_.resize((size_t)sz);

    // Find AU starts with the same rules the assembler uses, so each delivery is one frame.
    std::vector<size_t> auStarts;
    std::vector<bool> startsWithAud;
    bool haveVcl = false, open = false;
    size_t i = 0;
    while (true) {
        const size_t s = findStartCodeScalar(file_.data(), i, file_.size());
        if (s + 4 >= file_.size()) break;
        const size_t prefix = (s > 0 && file_[s - 1] == 0) ? s - 1 : s;
        const uint8_t type = file_[s + 3] & 0x1F;
        bool newAu = !open;
        if (open && haveVcl) {
            if (type == kNalAud || type == kNalSps || type == kNalPps || type == kNalSei) newAu = true;
            if (isVcl(type)) {
                uint8_t rb[16];
                const size_t m = h264::unescape(&file_[s + 4], 12, rb, sizeof(rb));
                h264::BitReader br(rb, m);
                if (br.ue() == 0) newAu = true;
            }
        }
        if (newAu) {
            auStarts.push_back(prefix);
            startsWithAud.push_back(type == kNalAud);
            open = true;
            haveVcl = false;
        }
        if (isVcl(type)) haveVcl = true;
        if (type == kNalSps && !replaySps_.valid) {
            uint8_t rb[512];
            const size_t m = h264::unescape(&file_[s + 4], std::min<size_t>(400, file_.size() - s - 4), rb, sizeof(rb));
            h264::parseSps(rb, m, replaySps_);
        }
        i = s + 3;
    }
    if (auStarts.size() < 2 || !replaySps_.valid) {
        err = "no decodable H.264 access units / SPS in " + narrow(opt_.replayPath);
        return false;
    }
    // Delivery cut points: each frame's bytes plus, unless --replay-naive, the AUD that begins
    // the next frame. That is exactly a sender that writes an AUD right after the last slice.
    const size_t audLen = 6;
    cuts_.clear();
    cuts_.push_back(auStarts[0]);
    for (size_t k = 1; k < auStarts.size(); ++k) {
        size_t c = auStarts[k];
        if (!opt_.replayNaive && startsWithAud[k]) {
            const size_t sc = (file_[c] == 0 && file_[c + 1] == 0 && file_[c + 2] == 0) ? c + 4 : c + 3;
            c = sc + 2;  // through the AUD's primary_pic_type byte
            (void)audLen;
        }
        cuts_.push_back(c);
    }
    cuts_.push_back(file_.size());
    // When looping, the file's leading AUD terminates its last frame, exactly as the sender's
    // trailing AUD would; without this the wrap frame closes one frame late.
    firstAud_ = firstAudEnd_ = 0;
    if (!opt_.replayNaive && startsWithAud[0]) {
        const size_t c = auStarts[0];
        const size_t sc = (file_[c] == 0 && file_[c + 1] == 0 && file_[c + 2] == 0) ? c + 4 : c + 3;
        firstAud_ = c;
        firstAudEnd_ = sc + 2;
    }
    if (opt_.replayFps > 0) replayFps_ = opt_.replayFps;
    else if (replaySps_.timingInfoPresent && replaySps_.numUnitsInTick)
        replayFps_ = (double)replaySps_.timeScale / (2.0 * replaySps_.numUnitsInTick);
    else replayFps_ = 60;
    logInfo("replay: %s, %zu frames, %ux%u, %.2f fps, AUDs %s, delivery %s", narrow(opt_.replayPath).c_str(),
            auStarts.size(), replaySps_.width, replaySps_.height, replayFps_,
            startsWithAud[1] ? "present" : "absent",
            opt_.replayNaive || !startsWithAud[1] ? "AU-only (fallback boundary detection)"
                                                  : "AU + next AUD (sender-style end-of-frame marker)");
    return true;
}

void Pipeline::replayLoop() {
    tuning::applyToCurrentThread(tuning::Role::Recv, false, opt_, plan);
    if (decodeInline()) g_cu.cuCtxSetCurrent(decoder.context());
    PreciseSleeper sleeper;
    const uint64_t interval = (uint64_t)usToTicks(1e6 / replayFps_);
    uint64_t w = 0;
    uint64_t next = qpc() + interval;
    const size_t cap = arena.capacity();
    bool wrapped = false;
    // Stall simulation: frames due inside a stall window are all delivered at its end, the way
    // the phone's queued frames arrive once a stalled receiver reads again.
    const uint64_t t0 = next;
    const uint64_t stallPeriod = opt_.replayStallEveryS > 0 ? (uint64_t)usToTicks(opt_.replayStallEveryS * 1e6) : 0;
    const uint64_t stallLen = (uint64_t)usToTicks(opt_.replayStallMs * 1e3);
    do {
        for (size_t k = 0; k + 1 < cuts_.size() && !stop_.load(std::memory_order_relaxed); ++k) {
            size_t a = cuts_[k];
            const size_t b = cuts_[k + 1];
            if (k == 0 && wrapped && firstAudEnd_) a = firstAudEnd_;  // its AUD already went out
            if (b <= a) continue;
            while (cap - (size_t)(w - arena.release.load(std::memory_order_acquire)) < b - a) {
                if (stop_.load()) break;
                _mm_pause();
            }
            // Paced like a camera: sleep to just before the frame's due time, spin the rest,
            // then "receive" it.
            uint64_t due = next;
            if (stallPeriod && stallLen) {
                const uint64_t ph = (next - t0) % stallPeriod;
                if (next - t0 >= stallPeriod && ph < stallLen) due = next - ph + stallLen;
            }
            if (due > qpc() + (uint64_t)usToTicks(300)) sleeper.sleepUntil(due - (uint64_t)usToTicks(300));
            spinUntil(due);
            next += interval;
            if (!stallPeriod && qpc() > next) next = qpc() + interval;  // fell behind (debugger, etc.): do not burst
            // The kernel's recv copy, done here by memcpy, in TCP-segment-sized pieces.
            size_t off = a;
            while (off < b) {
                const size_t n = std::min<size_t>(b - off, 64 * 1024);
                memcpy(arena.at(w), &file_[off], n);
                deliver(w, nullptr, n, &chunks);
                off += n;
            }
            if (k + 2 == cuts_.size() && opt_.replayLoop && firstAudEnd_) {
                memcpy(arena.at(w), &file_[firstAud_], firstAudEnd_ - firstAud_);
                deliver(w, nullptr, firstAudEnd_ - firstAud_, &chunks);
            }
        }
        wrapped = true;
    } while (opt_.replayLoop && !stop_.load());
    if (!opt_.inlineAssembler) {
        while (!chunks.push(RecvChunk{w, 0}) && !stop_.load()) _mm_pause();
    } else {
        assembler.flush(qpc());
        if (decodeInline()) decoder.drainAus();
    }
    sourceEnded_ = true;
}

}  // namespace cf
