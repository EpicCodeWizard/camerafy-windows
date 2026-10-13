#include "usbmux.h"
#include "console.h"
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <string.h>

namespace cf::usbmux {

namespace {
constexpr char kProgName[] = "camerafy";
constexpr char kClientVersion[] = "camerafy-1.0";
std::atomic<uint32_t> g_tag{1};
}  // namespace

const char* resultName(int64_t n) {
    switch (n) {
    case kResultOk: return "OK";
    case kResultBadCommand: return "BadCommand";
    case kResultBadDevice: return "BadDevice (device not attached or DeviceID stale)";
    case kResultConnRefused: return "ConnRefused (nothing listening on that port on the phone: is the camerafy app running in the foreground?)";
    case kResultBadVersion: return "BadVersion (usbmux protocol version rejected)";
    default: return "unknown result";
    }
}

void encodeHeader(uint8_t out[16], uint32_t payloadLen, uint32_t tag) {
    const Header h{16 + payloadLen, 1, 8, tag};
    memcpy(out, &h, 16);  // x64 is little-endian, as the protocol requires
}

std::string buildRequest(const char* messageType, const std::vector<std::pair<const char*, int64_t>>& ints) {
    plist::Writer w;
    w.kv("MessageType", messageType).kv("ClientVersionString", kClientVersion).kv("ProgName", kProgName);
    w.kv("kLibUSBMuxVersion", (int64_t)3);
    for (auto& [k, v] : ints) w.kv(k, v);
    return w.finish();
}

bool sendAll(SOCKET s, const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n) {
        const int r = send(s, c, (int)n, 0);
        if (r <= 0) return false;
        c += r;
        n -= (size_t)r;
    }
    return true;
}

bool recvAll(SOCKET s, void* p, size_t n) {
    char* c = (char*)p;
    while (n) {
        const int r = recv(s, c, (int)n, 0);
        if (r <= 0) return false;
        c += r;
        n -= (size_t)r;
    }
    return true;
}

bool sendPlist(SOCKET s, const std::string& xml, uint32_t tag) {
    uint8_t hdr[16];
    encodeHeader(hdr, (uint32_t)xml.size(), tag);
    return sendAll(s, hdr, 16) && sendAll(s, xml.data(), xml.size());
}

bool recvPlist(SOCKET s, plist::Node& root, uint32_t* tag, std::string& err) {
    Header h{};
    if (!recvAll(s, &h, 16)) {
        err = "connection closed by AMDS (WSA error " + std::to_string(WSAGetLastError()) + ")";
        return false;
    }
    if (h.length < 16 || h.length > (16u << 20)) {
        err = "bad usbmux header length " + std::to_string(h.length);
        return false;
    }
    if (h.version != 1 || h.message != 8) {
        err = "unexpected usbmux framing: version " + std::to_string(h.version) + ", message " + std::to_string(h.message);
        return false;
    }
    std::string body(h.length - 16, '\0');
    if (!recvAll(s, body.data(), body.size())) {
        err = "short usbmux payload";
        return false;
    }
    if (tag) *tag = h.tag;
    if (!plist::parse(body.data(), body.size(), root)) {
        err = "unparseable plist from AMDS: " + body.substr(0, 200);
        return false;
    }
    return true;
}

SOCKET openAmdsSocket(std::string& err) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        err = "socket() failed: " + std::to_string(WSAGetLastError());
        return INVALID_SOCKET;
    }
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kAmdsPort);
    a.sin_addr.s_addr = htonl(kLoopbackAddr);
    if (connect(s, (const sockaddr*)&a, sizeof(a)) != 0) {
        const int e = WSAGetLastError();
        closesocket(s);
        err = (e == WSAECONNREFUSED)
                  ? "Apple Mobile Device Service is not listening on 127.0.0.1:27015. Install the Apple Devices app "
                    "from the Microsoft Store (or iTunes), connect the iPhone by cable and unlock it."
                  : "connect(127.0.0.1:27015) failed: WSA error " + std::to_string(e);
        return INVALID_SOCKET;
    }
    return s;
}

bool probeAmds() {
    std::string err;
    SOCKET s = openAmdsSocket(err);
    if (s == INVALID_SOCKET) return false;
    closesocket(s);
    return true;
}

static bool parseDevice(const plist::Node& entry, Device& d) {
    int64_t id = 0;
    if (!entry.getInt("DeviceID", id)) return false;
    d.deviceId = (uint32_t)id;
    const plist::Node* props = entry.get("Properties");
    if (props) {
        if (auto* s = props->getString("SerialNumber")) d.udid = *s;
        if (auto* s = props->getString("ConnectionType")) d.connectionType = *s;
        int64_t v = 0;
        if (props->getInt("ProductID", v)) d.productId = (uint32_t)v;
        if (props->getInt("LocationID", v)) d.locationId = (uint32_t)v;
        if (d.deviceId == 0 && props->getInt("DeviceID", v)) d.deviceId = (uint32_t)v;
    }
    return true;
}

bool listDevices(std::vector<Device>& out, std::string& err) {
    out.clear();
    SOCKET s = openAmdsSocket(err);
    if (s == INVALID_SOCKET) return false;
    const uint32_t tag = g_tag++;
    bool ok = sendPlist(s, buildRequest("ListDevices"), tag);
    plist::Node root;
    if (ok) ok = recvPlist(s, root, nullptr, err);
    closesocket(s);
    if (!ok) return false;
    const plist::Node* list = root.get("DeviceList");
    if (!list || list->type != plist::Node::Type::Array) {
        int64_t n = 0;
        if (root.getInt("Number", n)) err = std::string("ListDevices failed: ") + resultName(n);
        else err = "ListDevices reply has no DeviceList";
        return false;
    }
    for (const auto& e : list->items) {
        Device d;
        if (parseDevice(e, d)) out.push_back(d);
    }
    return true;
}

bool chooseDevice(const std::vector<Device>& devs, const std::string& udid, Device& out, std::string& err,
                  std::vector<std::string>* ignored) {
    std::vector<Device> usb;
    for (auto& d : devs) {
        if (d.isUsb()) {
            usb.push_back(d);
        } else if (!udid.empty() && d.udid == udid) {
            err = "device " + d.udid + " is attached via '" + d.connectionType +
                  "', not USB. camerafy only uses the cable: connect it with a USB cable.";
            return false;
        } else if (ignored) {
            ignored->push_back(d.connectionType + "-attached device " + d.udid + " ignored (USB only, by design)");
        }
    }
    if (!udid.empty()) {
        for (auto& d : usb)
            if (d.udid == udid) { out = d; return true; }
        err = "no USB-attached device with UDID " + udid;
        return false;
    }
    if (usb.empty()) {
        err = devs.empty() ? "no iPhone attached (connect it by USB cable, unlock it, and tap Trust)"
                           : "no USB-attached iPhone; the only devices visible are network-attached, and those are refused";
        return false;
    }
    if (usb.size() > 1) {
        err = "more than one USB iPhone attached; pick one with --udid:";
        for (auto& d : usb) err += "\n    " + d.udid;
        return false;
    }
    out = usb[0];
    return true;
}

SOCKET connectDevice(uint32_t deviceId, uint16_t port, std::string& err, int64_t* result) {
    SOCKET s = openAmdsSocket(err);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    const uint32_t tag = g_tag++;
    const std::string req = buildRequest("Connect", {{"DeviceID", (int64_t)deviceId}, {"PortNumber", (int64_t)wirePort(port)}});
    plist::Node root;
    if (!sendPlist(s, req, tag) || !recvPlist(s, root, nullptr, err)) {
        if (err.empty()) err = "Connect: send failed";
        closesocket(s);
        return INVALID_SOCKET;
    }
    int64_t n = -1;
    if (!root.getInt("Number", n)) {
        err = "Connect reply has no Result Number";
        closesocket(s);
        return INVALID_SOCKET;
    }
    if (result) *result = n;
    if (n != kResultOk) {
        err = "usbmux Connect to device port " + std::to_string(port) + " failed: " + std::to_string(n) + " " + resultName(n);
        closesocket(s);
        return INVALID_SOCKET;
    }
    // From here on the socket is a transparent byte pipe to the phone's listener.
    return s;
}

bool Listener::start(Callback cb, std::string& err) {
    s_ = openAmdsSocket(err);
    if (s_ == INVALID_SOCKET) return false;
    plist::Node root;
    if (!sendPlist(s_, buildRequest("Listen"), g_tag++) || !recvPlist(s_, root, nullptr, err)) {
        closesocket(s_);
        s_ = INVALID_SOCKET;
        return false;
    }
    int64_t n = -1;
    if (!root.getInt("Number", n) || n != kResultOk) {
        err = std::string("Listen rejected: ") + resultName(n);
        closesocket(s_);
        s_ = INVALID_SOCKET;
        return false;
    }
    cb_ = std::move(cb);
    stop_ = false;
    thread_ = CreateThread(nullptr, 0, threadProc, this, 0, nullptr);
    return thread_ != nullptr;
}

void Listener::stop() {
    stop_ = true;
    if (s_ != INVALID_SOCKET) {
        shutdown(s_, SD_BOTH);
        closesocket(s_);
        s_ = INVALID_SOCKET;
    }
    if (thread_) {
        WaitForSingleObject(thread_, 2000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
}

DWORD WINAPI Listener::threadProc(void* self) {
    auto* l = (Listener*)self;
    while (!l->stop_) {
        plist::Node msg;
        std::string err;
        if (!recvPlist(l->s_, msg, nullptr, err)) break;
        const std::string* type = msg.getString("MessageType");
        if (!type) continue;
        if (*type == "Attached") {
            Device d;
            if (parseDevice(msg, d) && l->cb_) l->cb_(true, d);
        } else if (*type == "Detached") {
            Device d;
            int64_t id = 0;
            if (msg.getInt("DeviceID", id)) d.deviceId = (uint32_t)id;
            if (l->cb_) l->cb_(false, d);
        }
    }
    return 0;
}

}  // namespace cf::usbmux
