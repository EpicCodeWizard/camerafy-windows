#pragma once
// usbmux client over Apple Mobile Device Service's loopback socket.
// 127.0.0.1:27015 is a compile-time constant: no host configuration, no DNS, ever.
#include <winsock2.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include "plist.h"

namespace cf::usbmux {

inline constexpr uint32_t kLoopbackAddr = 0x7F000001;  // 127.0.0.1, host order
inline constexpr uint16_t kAmdsPort = 27015;

struct Header {
    uint32_t length;   // 16 + payload bytes
    uint32_t version;  // 1 = plist protocol
    uint32_t message;  // 8 = PLIST payload
    uint32_t tag;      // echoed in the reply
};
static_assert(sizeof(Header) == 16);

enum : int64_t { kResultOk = 0, kResultBadCommand = 1, kResultBadDevice = 2, kResultConnRefused = 3,
                 kResultBadVersion = 6 };

const char* resultName(int64_t n);

// usbmux takes the port in network byte order inside a little-endian plist integer:
// port 22 goes on the wire as 5632.
constexpr uint16_t wirePort(uint16_t port) { return (uint16_t)((port >> 8) | (port << 8)); }

void encodeHeader(uint8_t out[16], uint32_t payloadLen, uint32_t tag);
std::string buildRequest(const char* messageType, const std::vector<std::pair<const char*, int64_t>>& ints = {});

struct Device {
    uint32_t deviceId = 0;           // what Connect takes
    std::string udid;                // Properties.SerialNumber
    std::string connectionType;      // "USB" or "Network"
    uint32_t productId = 0;
    uint32_t locationId = 0;
    bool isUsb() const { return connectionType == "USB"; }
};

// Opens a TCP connection to 127.0.0.1:27015. This is the only place in the program that
// creates a socket or calls connect().
SOCKET openAmdsSocket(std::string& err);

bool probeAmds();  // true if AMDS accepts a connection

// One request/response on a fresh socket.
bool listDevices(std::vector<Device>& out, std::string& err);

// Picks the device to use: USB-attached only (PRD 2). A network-attached device is refused
// with an error even if it is the only one; more than one USB device needs `udid`.
bool chooseDevice(const std::vector<Device>& all, const std::string& udid, Device& out, std::string& err,
                  std::vector<std::string>* ignored = nullptr);

// Opens a new socket and turns it into a byte pipe to `port` on the device.
// On failure returns INVALID_SOCKET with err set (and *result set when usbmux replied).
SOCKET connectDevice(uint32_t deviceId, uint16_t port, std::string& err, int64_t* result = nullptr);

// Attached/Detached notifications on a dedicated Listen socket.
class Listener {
public:
    using Callback = std::function<void(bool attached, const Device& d)>;
    bool start(Callback cb, std::string& err);
    void stop();
    ~Listener() { stop(); }

private:
    static DWORD WINAPI threadProc(void* self);
    SOCKET s_ = INVALID_SOCKET;
    HANDLE thread_ = nullptr;
    Callback cb_;
    std::atomic<bool> stop_{false};
};

// Low-level helpers shared with lockdown.
bool sendAll(SOCKET s, const void* p, size_t n);
bool recvAll(SOCKET s, void* p, size_t n);
bool sendPlist(SOCKET s, const std::string& xml, uint32_t tag);
bool recvPlist(SOCKET s, plist::Node& root, uint32_t* tag, std::string& err);

}  // namespace cf::usbmux
