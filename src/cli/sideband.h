#pragma once
// Sideband channel to the iOS app (camerafy sideband protocol, version 1):
// Hello (device name, stream format), per-frame FrameTiming, ClockPing/ClockPong,
// RequestIDR, FormatChange. Nothing here is needed to decode; it adds names, telemetry,
// clock correlation and IDR requests.
#include <winsock2.h>
#include <stdint.h>
#include <string>
#include <atomic>
#include "telemetry.h"
#include "frame_types.h"

namespace cf {

// Ports the iOS app listens on (phone-side 127.0.0.1, reached through usbmux Connect).
struct SidebandPorts {
    uint16_t video;
    uint16_t sideband;
};
SidebandPorts defaultPorts();

struct DeviceHello {
    bool valid = false;
    uint32_t protocolVersion = 0;
    std::string deviceName;  // UIDevice.current.name, verbatim
    std::string codec;       // "h264"
    std::string profile;
    uint32_t level = 0;      // level_idc (x10); 0 before the first IDR
    uint32_t width = 0, height = 0, fpsNum = 0, fpsDen = 1;
    uint32_t slicesPerFrame = 0;
    bool slicesProgressive = false;  // measured on the phone
    bool audAfterLastSlice = false;  // always true in version 1
};

// Pure encoders/decoders for the sideband messages (unit-tested against worked examples).
namespace wire {
size_t clockPing(uint8_t out[24], uint32_t seq, uint64_t t1);
size_t requestIdr(uint8_t out[16], uint32_t seq, uint32_t reason);
bool frameTiming(const uint8_t* msg, size_t n, PhoneTiming& out);
bool hello(const uint8_t* msg, size_t n, DeviceHello& out);
}  // namespace wire

class Sideband final {
public:
    ~Sideband();
    // Takes ownership of a socket already connected through usbmux; reads Hello.
    bool handshake(SOCKET s, DeviceHello& hello, std::string& err, DWORD timeoutMs = 5000);
    void start(PhoneTimingRing* phone, LengthRing* lengths, ClockEstimate* clock, std::atomic<bool>* wantIdr);
    // reason: 0 unspecified, 1 decode error, 2 client joined, 3 manual/debug
    bool requestIdr(uint32_t reason);
    void stop();
    bool connected() const { return s_ != INVALID_SOCKET && !dead_.load(); }
    // The most recent FormatChange, if any arrived.
    bool latestFormat(DeviceHello& out);

    struct PingStats {
        std::atomic<uint64_t> count{0};
        std::atomic<int64_t> minRttUs{-1};
        std::atomic<int64_t> lastRttUs{-1};
        std::atomic<uint64_t> sumRttUs{0};
    } ping;
    std::atomic<uint64_t> frameTimings{0};
    std::atomic<uint64_t> idrRequests{0};
    std::atomic<uint64_t> formatChanges{0};

private:
    static DWORD WINAPI recvThreadProc(void* self);
    static DWORD WINAPI pingThreadProc(void* self);
    void recvLoop();
    void pingLoop();
    bool sendRaw(const uint8_t* p, size_t n);
    bool sendPing();

    SOCKET s_ = INVALID_SOCKET;
    HANDLE recvThread_ = nullptr, pingThread_ = nullptr;
    std::atomic<bool> stop_{false}, dead_{false};
    PhoneTimingRing* phone_ = nullptr;
    LengthRing* lengths_ = nullptr;
    ClockEstimate* clock_ = nullptr;
    std::atomic<bool>* wantIdr_ = nullptr;
    SRWLOCK sendLock_ = SRWLOCK_INIT;
    SRWLOCK fmtLock_ = SRWLOCK_INIT;
    DeviceHello lastFormat_;
    uint32_t pingSeq_ = 0, idrSeq_ = 0;
};

}  // namespace cf
