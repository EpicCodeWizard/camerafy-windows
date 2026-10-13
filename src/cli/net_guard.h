#pragma once
// Enforces the USB-only constraint: every socket this process holds must be a TCP
// connection between 127.0.0.1 and 127.0.0.1:27015. No listeners, no UDP, no IPv6.
#include <string>
#include <vector>
#include <set>

namespace cf::netguard {

struct Snapshot {
    std::vector<std::string> sockets;     // human-readable description of each socket we hold
    std::vector<std::string> violations;  // subset that breaks the constraint
};

bool snapshot(Snapshot& out, std::string& err);

// Accumulates unique sockets over a run (for --verify-no-network).
class Recorder {
public:
    void sample();
    const std::set<std::string>& seen() const { return seen_; }
    const std::set<std::string>& violations() const { return violations_; }
    unsigned samples() const { return samples_; }

private:
    std::set<std::string> seen_, violations_;
    unsigned samples_ = 0;
};

// Lists every enabled network adapter that is up (for the network-disabled test report).
std::vector<std::string> activeAdapters();

}  // namespace cf::netguard
