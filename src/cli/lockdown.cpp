#include "lockdown.h"
#include "usbmux.h"
#include "plist.h"

namespace cf::lockdown {

namespace {

// lockdownd frames each plist with a 4-byte big-endian length.
bool sendMsg(SOCKET s, const std::string& xml) {
    const uint32_t n = (uint32_t)xml.size();
    const uint8_t len[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    return usbmux::sendAll(s, len, 4) && usbmux::sendAll(s, xml.data(), xml.size());
}

bool recvMsg(SOCKET s, plist::Node& root, std::string& err) {
    uint8_t len[4];
    if (!usbmux::recvAll(s, len, 4)) { err = "lockdownd closed the connection"; return false; }
    const uint32_t n = (uint32_t)len[0] << 24 | (uint32_t)len[1] << 16 | (uint32_t)len[2] << 8 | len[3];
    if (n == 0 || n > (1u << 20)) { err = "bad lockdownd frame length"; return false; }
    std::string body(n, '\0');
    if (!usbmux::recvAll(s, body.data(), n)) { err = "short lockdownd frame"; return false; }
    if (!plist::parse(body.data(), n, root)) { err = "unparseable lockdownd reply"; return false; }
    return true;
}

}  // namespace

bool queryDeviceName(uint32_t deviceId, std::string& name, std::string& err) {
    SOCKET s = usbmux::connectDevice(deviceId, kPort, err);
    if (s == INVALID_SOCKET) return false;
    bool ok = false;
    plist::Node r;
    plist::Writer q;
    q.kv("Label", "camerafy").kv("Request", "QueryType");
    if (sendMsg(s, q.finish()) && recvMsg(s, r, err)) {
        plist::Writer g;
        g.kv("Label", "camerafy").kv("Key", "DeviceName").kv("Request", "GetValue");
        plist::Node v;
        if (sendMsg(s, g.finish()) && recvMsg(s, v, err)) {
            if (const std::string* nm = v.getString("Value")) {
                name = *nm;
                ok = !name.empty();
            } else if (const std::string* e = v.getString("Error")) {
                err = "lockdownd GetValue(DeviceName): " + *e;
            }
        }
    }
    closesocket(s);
    return ok;
}

}  // namespace cf::lockdown
