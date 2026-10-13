#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "net_guard.h"
#include "usbmux.h"
#include "console.h"
#include <stdio.h>

namespace cf::netguard {

namespace {

std::string ip4(DWORD netOrder) {
    char b[32];
    const uint8_t* p = (const uint8_t*)&netOrder;
    snprintf(b, sizeof(b), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return b;
}

uint16_t port(DWORD netOrderLow16) { return ntohs((u_short)netOrderLow16); }

const char* tcpState(DWORD s) {
    switch (s) {
    case MIB_TCP_STATE_CLOSED: return "CLOSED";
    case MIB_TCP_STATE_LISTEN: return "LISTEN";
    case MIB_TCP_STATE_SYN_SENT: return "SYN_SENT";
    case MIB_TCP_STATE_SYN_RCVD: return "SYN_RCVD";
    case MIB_TCP_STATE_ESTAB: return "ESTABLISHED";
    case MIB_TCP_STATE_FIN_WAIT1: return "FIN_WAIT1";
    case MIB_TCP_STATE_FIN_WAIT2: return "FIN_WAIT2";
    case MIB_TCP_STATE_CLOSE_WAIT: return "CLOSE_WAIT";
    case MIB_TCP_STATE_CLOSING: return "CLOSING";
    case MIB_TCP_STATE_LAST_ACK: return "LAST_ACK";
    case MIB_TCP_STATE_TIME_WAIT: return "TIME_WAIT";
    case MIB_TCP_STATE_DELETE_TCB: return "DELETE_TCB";
    default: return "?";
    }
}

template <typename Fn>
bool table(ULONG af, bool tcp, Fn&& each, std::string& err) {
    std::vector<uint8_t> buf(64 * 1024);
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD size = (DWORD)buf.size();
        DWORD r = tcp ? GetExtendedTcpTable(buf.data(), &size, FALSE, af, TCP_TABLE_OWNER_PID_ALL, 0)
                      : GetExtendedUdpTable(buf.data(), &size, FALSE, af, UDP_TABLE_OWNER_PID, 0);
        if (r == ERROR_INSUFFICIENT_BUFFER) {
            buf.resize(size + 4096);
            continue;
        }
        if (r != NO_ERROR) {
            err = "Get" + std::string(tcp ? "Tcp" : "Udp") + "Table failed: " + std::to_string(r);
            return false;
        }
        each(buf.data());
        return true;
    }
    err = "socket table kept growing";
    return false;
}

}  // namespace

bool snapshot(Snapshot& out, std::string& err) {
    out = {};
    const DWORD pid = GetCurrentProcessId();
    const DWORD loop = htonl(usbmux::kLoopbackAddr);
    char line[256];

    bool ok = table(AF_INET, true, [&](const uint8_t* p) {
        auto* t = (const MIB_TCPTABLE_OWNER_PID*)p;
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            const auto& r = t->table[i];
            if (r.dwOwningPid != pid) continue;
            snprintf(line, sizeof(line), "tcp4 %s:%u -> %s:%u %s", ip4(r.dwLocalAddr).c_str(), port(r.dwLocalPort),
                     ip4(r.dwRemoteAddr).c_str(), port(r.dwRemotePort), tcpState(r.dwState));
            out.sockets.push_back(line);
            const bool good = r.dwState != MIB_TCP_STATE_LISTEN && r.dwLocalAddr == loop && r.dwRemoteAddr == loop &&
                              port(r.dwRemotePort) == usbmux::kAmdsPort;
            if (!good) out.violations.push_back(line);
        }
    }, err);
    ok = ok && table(AF_INET6, true, [&](const uint8_t* p) {
        auto* t = (const MIB_TCP6TABLE_OWNER_PID*)p;
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            if (t->table[i].dwOwningPid != pid) continue;
            snprintf(line, sizeof(line), "tcp6 socket (state %s)", tcpState(t->table[i].dwState));
            out.sockets.push_back(line);
            out.violations.push_back(line);
        }
    }, err);
    ok = ok && table(AF_INET, false, [&](const uint8_t* p) {
        auto* t = (const MIB_UDPTABLE_OWNER_PID*)p;
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            if (t->table[i].dwOwningPid != pid) continue;
            snprintf(line, sizeof(line), "udp4 %s:%u", ip4(t->table[i].dwLocalAddr).c_str(), port(t->table[i].dwLocalPort));
            out.sockets.push_back(line);
            out.violations.push_back(line);
        }
    }, err);
    ok = ok && table(AF_INET6, false, [&](const uint8_t* p) {
        auto* t = (const MIB_UDP6TABLE_OWNER_PID*)p;
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            if (t->table[i].dwOwningPid != pid) continue;
            snprintf(line, sizeof(line), "udp6 port %u", port(t->table[i].dwLocalPort));
            out.sockets.push_back(line);
            out.violations.push_back(line);
        }
    }, err);
    return ok;
}

void Recorder::sample() {
    Snapshot s;
    std::string err;
    if (!snapshot(s, err)) {
        violations_.insert("socket table query failed: " + err);
        return;
    }
    ++samples_;
    for (auto& x : s.sockets) seen_.insert(x);
    for (auto& x : s.violations) violations_.insert(x);
}

std::vector<std::string> activeAdapters() {
    std::vector<std::string> out;
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buf(size);
    ULONG r = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                   nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    }
    if (r != NO_ERROR) return out;
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->OperStatus != IfOperStatusUp) continue;
        out.push_back(narrow(a->FriendlyName ? a->FriendlyName : L"?"));
    }
    return out;
}

}  // namespace cf::netguard
