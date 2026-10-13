// Sideband (port 7002), implementing the camerafy sideband protocol (version 1).
// All messages: uint32 msgLen (bytes after this field) | uint16 msgType | uint16 reserved | payload,
// little-endian, total size <= 4096.
#include "sideband.h"
#include "console.h"
#include "clock_sync.h"
#include "qpc.h"
#include <string.h>

namespace cf {

namespace {

constexpr uint16_t kMsgHello = 1, kMsgFrameTiming = 2, kMsgClockPing = 3, kMsgClockPong = 4, kMsgRequestIdr = 5,
                   kMsgFormatChange = 6;
constexpr size_t kHelloFixed = 44, kFrameTimingSize = 80, kPingSize = 24, kPongSize = 40, kIdrSize = 16,
                 kFormatChangeSize = 40, kMaxMsg = 4096;

inline uint16_t get16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
inline uint32_t get32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline uint64_t get64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }
inline void put16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
inline void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
inline void put64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); }

inline uint64_t nowUs() { return (uint64_t)ticksToUs((int64_t)qpc()); }

bool recvExact(SOCKET s, uint8_t* p, size_t n) {
    while (n) {
        const int r = recv(s, (char*)p, (int)n, 0);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

// Reads one framed message into buf (header included). Returns total size, 0 on EOF/error,
// SIZE_MAX on an impossible length (protocol error).
size_t readMessage(SOCKET s, uint8_t* buf, uint16_t& type) {
    if (!recvExact(s, buf, 8)) return 0;
    const uint32_t msgLen = get32(buf);
    if (msgLen < 4 || msgLen > kMaxMsg - 4) return SIZE_MAX;
    type = get16(buf + 4);
    if (!recvExact(s, buf + 8, msgLen - 4)) return 0;
    return (size_t)msgLen + 4;
}

const char* profileName(uint16_t p) {
    switch (p) {
    case 1: return "Constrained Baseline";
    case 2: return "Constrained High";
    case 3: return "Main";
    case 4: return "High";
    default: return "unknown";
    }
}

void parseStreamInfo(const uint8_t* m, DeviceHello& h) {
    h.protocolVersion = get16(m + 8);
    h.codec = get16(m + 10) == 1 ? "h264" : "unknown(" + std::to_string(get16(m + 10)) + ")";
    h.profile = profileName(get16(m + 12));
    h.level = get16(m + 14);
    h.width = get32(m + 16);
    h.height = get32(m + 20);
    h.fpsNum = get32(m + 24);
    h.fpsDen = get32(m + 28) ? get32(m + 28) : 1;
    h.slicesPerFrame = get32(m + 32);
    h.slicesProgressive = m[36] != 0;
}

}  // namespace

SidebandPorts defaultPorts() { return {7001, 7002}; }

namespace wire {

size_t clockPing(uint8_t m[24], uint32_t seq, uint64_t t1) {
    memset(m, 0, kPingSize);
    put32(m, (uint32_t)kPingSize - 4);
    put16(m + 4, kMsgClockPing);
    put32(m + 8, seq);
    put64(m + 16, t1);
    return kPingSize;
}

size_t requestIdr(uint8_t m[16], uint32_t seq, uint32_t reason) {
    memset(m, 0, kIdrSize);
    put32(m, (uint32_t)kIdrSize - 4);
    put16(m + 4, kMsgRequestIdr);
    put32(m + 8, seq);
    put32(m + 12, reason);
    return kIdrSize;
}

bool frameTiming(const uint8_t* buf, size_t n, PhoneTiming& pt) {
    if (n < kFrameTimingSize || get16(buf + 4) != kMsgFrameTiming) return false;
    pt = {};
    pt.frameId = get32(buf + 8);
    pt.isIdr = buf[12];
    pt.sliceCount = buf[13];
    pt.frameBytes = get32(buf + 16);
    pt.capturePtsUs = get64(buf + 24);
    pt.captureCallbackUs = get64(buf + 32);
    pt.encodeSubmitUs = get64(buf + 40);
    pt.encodeOutputUs = get64(buf + 48);
    pt.encodeCompleteUs = get64(buf + 56);
    pt.firstByteSendUs = get64(buf + 64);
    pt.lastByteSendUs = get64(buf + 72);
    return true;
}

bool hello(const uint8_t* buf, size_t n, DeviceHello& h) {
    if (n < kHelloFixed || get16(buf + 4) != kMsgHello) return false;
    h = {};
    parseStreamInfo(buf, h);
    const uint32_t nameLen = get32(buf + 40);
    if (kHelloFixed + nameLen > n) return false;
    h.deviceName.assign((const char*)buf + kHelloFixed, nameLen);
    h.audAfterLastSlice = true;  // always true in version 1
    h.valid = true;
    return true;
}

}  // namespace wire

Sideband::~Sideband() { stop(); }

bool Sideband::handshake(SOCKET s, DeviceHello& hello, std::string& err, DWORD timeoutMs) {
    s_ = s;
    hello = {};
    BOOL one = TRUE;
    setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    DWORD to = timeoutMs;
    setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    uint8_t buf[kMaxMsg];
    uint16_t type = 0;
    const size_t n = readMessage(s_, buf, type);
    to = 0;
    setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    if (n == 0) {
        err = "sideband closed or timed out before Hello (WSA " + std::to_string(WSAGetLastError()) + ")";
        return false;
    }
    if (n == SIZE_MAX) {
        err = "sideband protocol error: impossible msgLen in first message";
        return false;
    }
    if (type != kMsgHello || n < kHelloFixed) {
        err = "sideband: first message is type " + std::to_string(type) + " (" + std::to_string(n) +
              " bytes), expected Hello";
        return false;
    }
    if (!wire::hello(buf, n, hello)) {
        err = "sideband: malformed Hello (deviceNameLen " + std::to_string(get32(buf + 40)) + " in a " +
              std::to_string(n) + "-byte message)";
        return false;
    }
    if (hello.protocolVersion != 1)
        logWarn("sideband protocol version %u (this receiver implements version 1)", hello.protocolVersion);
    return true;
}

void Sideband::start(PhoneTimingRing* phone, LengthRing* lengths, ClockEstimate* clock, std::atomic<bool>* wantIdr) {
    phone_ = phone;
    lengths_ = lengths;
    clock_ = clock;
    wantIdr_ = wantIdr;
    stop_ = false;
    dead_ = false;
    recvThread_ = CreateThread(nullptr, 0, recvThreadProc, this, 0, nullptr);
    pingThread_ = CreateThread(nullptr, 0, pingThreadProc, this, 0, nullptr);
}

bool Sideband::sendRaw(const uint8_t* p, size_t n) {
    AcquireSRWLockExclusive(&sendLock_);
    bool ok = true;
    while (n && ok) {
        const int r = send(s_, (const char*)p, (int)n, 0);
        if (r <= 0) ok = false;
        else { p += r; n -= (size_t)r; }
    }
    ReleaseSRWLockExclusive(&sendLock_);
    if (!ok) dead_ = true;
    return ok;
}

bool Sideband::requestIdr(uint32_t reason) {
    if (!connected()) return false;
    uint8_t m[kIdrSize];
    wire::requestIdr(m, ++idrSeq_, reason);
    idrRequests.fetch_add(1, std::memory_order_relaxed);
    return sendRaw(m, sizeof(m));
}

bool Sideband::sendPing() {
    uint8_t m[kPingSize];
    wire::clockPing(m, ++pingSeq_, nowUs());  // t1, stamped as late as possible
    return sendRaw(m, sizeof(m));
}

DWORD WINAPI Sideband::recvThreadProc(void* self) {
    ((Sideband*)self)->recvLoop();
    return 0;
}

DWORD WINAPI Sideband::pingThreadProc(void* self) {
    ((Sideband*)self)->pingLoop();
    return 0;
}

void Sideband::recvLoop() {
    // The pong's t4 must be stamped promptly, so this thread runs at high priority.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    uint8_t buf[kMaxMsg];
    MinRttFilter filt;
    while (!stop_.load()) {
        uint16_t type = 0;
        const size_t n = readMessage(s_, buf, type);
        const uint64_t t4 = nowUs();
        if (n == 0 || n == SIZE_MAX) {
            if (n == SIZE_MAX) logError("sideband protocol error (impossible msgLen); closing the sideband");
            dead_ = true;
            break;
        }
        switch (type) {
        case kMsgFrameTiming:
            if (phone_) {
                PhoneTiming pt;
                if (wire::frameTiming(buf, n, pt)) {
                    frameTimings.fetch_add(1, std::memory_order_relaxed);
                    phone_->push(pt);
                }
            }
            break;
        case kMsgClockPong:
            if (n >= kPongSize) {
                const int64_t t1 = (int64_t)get64(buf + 16), t2 = (int64_t)get64(buf + 24), t3 = (int64_t)get64(buf + 32);
                const ClockSample cs = ntpSample(t1, t2, t3, (int64_t)t4);
                if (cs.rttUs >= 0) {
                    filt.add(cs);
                    ClockSample best;
                    if (filt.best(best) && clock_) {
                        clock_->offsetUs.store(best.offsetUs);
                        clock_->rttUs.store(best.rttUs);
                        clock_->samples.fetch_add(1);
                    }
                    ping.count.fetch_add(1);
                    ping.lastRttUs.store(cs.rttUs);
                    const int64_t mn = ping.minRttUs.load();
                    if (mn < 0 || cs.rttUs < mn) ping.minRttUs.store(cs.rttUs);
                    ping.sumRttUs.fetch_add((uint64_t)cs.rttUs);
                }
            }
            break;
        case kMsgFormatChange:
            if (n >= kFormatChangeSize) {
                DeviceHello h;
                parseStreamInfo(buf, h);
                AcquireSRWLockExclusive(&fmtLock_);
                lastFormat_ = h;
                ReleaseSRWLockExclusive(&fmtLock_);
                formatChanges.fetch_add(1, std::memory_order_release);
            }
            break;
        case kMsgHello:
            break;  // only valid as the first message; ignore repeats
        default:
            break;  // unknown types are skipped by length (forward compatibility)
        }
    }
}

// Clock sync: a burst of pings every 2 s (the min-RTT sample of each window wins), plus IDR
// requests when the assembler/decoder ask for one, rate limited to one per 250 ms.
void Sideband::pingLoop() {
    uint64_t nextBurst = 0;
    uint64_t lastIdr = 0;
    while (!stop_.load() && !dead_.load()) {
        const uint64_t now = qpc();
        if (now >= nextBurst) {
            for (int i = 0; i < 8 && !stop_.load(); ++i) {
                if (!sendPing()) break;
                Sleep(15);
            }
            nextBurst = qpc() + (uint64_t)usToTicks(2e6);
        }
        if (wantIdr_ && wantIdr_->load(std::memory_order_relaxed) && now - lastIdr > (uint64_t)usToTicks(250000)) {
            wantIdr_->store(false, std::memory_order_relaxed);
            requestIdr(1);  // decode error / missing reference
            lastIdr = now;
        }
        Sleep(10);
    }
}

bool Sideband::latestFormat(DeviceHello& out) {
    if (!formatChanges.load(std::memory_order_acquire)) return false;
    AcquireSRWLockShared(&fmtLock_);
    out = lastFormat_;
    ReleaseSRWLockShared(&fmtLock_);
    return true;
}

void Sideband::stop() {
    stop_ = true;
    if (s_ != INVALID_SOCKET) {
        shutdown(s_, SD_BOTH);
        closesocket(s_);
        s_ = INVALID_SOCKET;
    }
    for (HANDLE* t : {&recvThread_, &pingThread_}) {
        if (*t) {
            WaitForSingleObject(*t, 2000);
            CloseHandle(*t);
            *t = nullptr;
        }
    }
}

}  // namespace cf
