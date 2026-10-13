// --self-test: the verification suite (PRD §15). Prints a pass/fail/skip table.
// Unit tests always run. Integration tests run when their prerequisites exist (registered
// DLLs, test data, AMDS, a phone) and otherwise SKIP with the reason; nothing is faked.
#include <winsock2.h>
#include "options.h"
#include "console.h"
#include "plist.h"
#include "usbmux.h"
#include "nal_scan.h"
#include "h264.h"
#include "ipc_reader.h"
#include "spsc_ring.h"
#include "arena.h"
#include "assembler.h"
#include "sideband.h"
#include "clock_sync.h"
#include "pipeline.h"
#include "vcam.h"
#include "ids.h"
#include "net_guard.h"
#include "qpc.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <random>
#include <thread>
#include <vector>
#include <string>
#include <memory>

#pragma comment(lib, "mfreadwrite.lib")

namespace cf {

namespace {

enum class R { Pass, Fail, Skip };
struct Result {
    std::string name;
    R r;
    std::string detail;
};
std::vector<Result> g_results;

void record(const std::string& name, R r, const std::string& detail = "") {
    g_results.push_back({name, r, detail});
    const char* tag = r == R::Pass ? "\x1b[32mPASS\x1b[0m" : r == R::Fail ? "\x1b[31mFAIL\x1b[0m" : "\x1b[33mSKIP\x1b[0m";
    logInfo("  [%s] %s%s%s", tag, name.c_str(), detail.empty() ? "" : "  -- ", detail.c_str());
}
#define CHECK(cond, msg) do { if (!(cond)) { record(name, R::Fail, msg); return; } } while (0)

std::wstring exeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L'\\'));
}

// Walks up from the exe to find testdata/ (the build tree lives under the repo).
std::wstring findTestData(const wchar_t* file) {
    std::wstring dir = exeDir();
    for (int i = 0; i < 5; ++i) {
        std::wstring p = dir + L"\\testdata\\" + file;
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
        const size_t k = dir.find_last_of(L'\\');
        if (k == std::wstring::npos) break;
        dir = dir.substr(0, k);
    }
    return {};
}

std::vector<uint8_t> readFile(const std::wstring& path, size_t maxBytes = SIZE_MAX) {
    std::vector<uint8_t> v;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return v;
    _fseeki64(f, 0, SEEK_END);
    const int64_t n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    v.resize(std::min<size_t>((size_t)(n < 0 ? 0 : n), maxBytes));
    fread(v.data(), 1, v.size(), f);
    fclose(f);
    return v;
}

// ================================================================ unit tests

void testPlistAndHeader() {
    const std::string name = "usbmux plist round-trip, header encoding, PortNumber byte-swap";
    plist::Writer w;
    w.kv("MessageType", "Connect").kv("DeviceID", (int64_t)7).kv("PortNumber", (int64_t)usbmux::wirePort(22));
    const uint8_t blob[] = {0, 1, 2, 250, 251};
    w.key("Data").data(blob, sizeof(blob)).key("Flag").boolean(true).kv("Esc", "a<b&c>\"'");
    const std::string xml = w.finish();
    plist::Node n;
    CHECK(plist::parse(xml.data(), xml.size(), n), "parse failed");
    int64_t port = 0, id = 0, flag = 0;
    CHECK(n.getInt("PortNumber", port) && port == 5632, "PortNumber 22 must go on the wire as 5632");
    CHECK(usbmux::wirePort(22) == 5632 && usbmux::wirePort(7001) == 0x591B, "wirePort byte swap");
    CHECK(n.getInt("DeviceID", id) && id == 7, "DeviceID");
    CHECK(n.getInt("Flag", flag) && flag == 1, "bool");
    const plist::Node* d = n.get("Data");
    CHECK(d && d->data.size() == 5 && d->data[3] == 250, "data base64 round trip");
    CHECK(n.getString("Esc") && *n.getString("Esc") == "a<b&c>\"'", "entity escaping");
    uint8_t hdr[16];
    usbmux::encodeHeader(hdr, 100, 9);
    const uint8_t want[16] = {116, 0, 0, 0, 1, 0, 0, 0, 8, 0, 0, 0, 9, 0, 0, 0};
    CHECK(!memcmp(hdr, want, 16), "header must be LE {16+len, 1, 8, tag}");
    const std::string req = usbmux::buildRequest("Connect", {{"DeviceID", 3}, {"PortNumber", usbmux::wirePort(7002)}});
    plist::Node r;
    CHECK(plist::parse(req.data(), req.size(), r), "request parse");
    CHECK(r.getString("ProgName") && r.getString("ClientVersionString") && r.getString("MessageType"),
          "every request carries MessageType, ClientVersionString, ProgName");
    // A real AMDS ListDevices reply shape, including an array and nested dicts.
    const char* reply =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><!DOCTYPE plist><plist version=\"1.0\"><dict><key>DeviceList</key><array>"
        "<dict><key>DeviceID</key><integer>4</integer><key>MessageType</key><string>Attached</string><key>Properties</key>"
        "<dict><key>ConnectionType</key><string>USB</string><key>SerialNumber</key><string>00008110-AAA</string>"
        "<key>ProductID</key><integer>4776</integer></dict></dict>"
        "<dict><key>DeviceID</key><integer>9</integer><key>Properties</key><dict><key>ConnectionType</key>"
        "<string>Network</string><key>SerialNumber</key><string>00008110-BBB</string></dict></dict>"
        "</array></dict></plist>";
    plist::Node lr;
    CHECK(plist::parse(reply, strlen(reply), lr), "ListDevices reply parse");
    const plist::Node* list = lr.get("DeviceList");
    CHECK(list && list->items.size() == 2, "DeviceList array");
    record(name, R::Pass);
}

void testUsbOnly() {
    const std::string name = "USB-only device selection (Wi-Fi-attached devices refused)";
    usbmux::Device usb{4, "AAA", "USB"}, wifi{9, "BBB", "Network"};
    usbmux::Device out;
    std::string err;
    CHECK(!usbmux::chooseDevice({wifi}, "", out, err), "a lone network device must be refused");
    CHECK(err.find("network") != std::string::npos, "error must say why: " + err);
    CHECK(!usbmux::chooseDevice({wifi, usb}, "BBB", out, err), "explicit --udid of a network device must be refused");
    CHECK(usbmux::chooseDevice({wifi, usb}, "", out, err) && out.deviceId == 4, "USB device chosen, Wi-Fi ignored");
    CHECK(!usbmux::chooseDevice({usb, usbmux::Device{5, "CCC", "USB"}}, "", out, err), "two USB devices need --udid");
    CHECK(usbmux::chooseDevice({usb, usbmux::Device{5, "CCC", "USB"}}, "CCC", out, err) && out.deviceId == 5, "--udid picks");
    record(name, R::Pass);
}

void testScanner() {
    const std::string name = "start-code scan: AVX2 == scalar (random, adversarial, every split offset)";
    std::mt19937 rng(1234);
    auto allCodes = [](FindStartCodeFn f, const uint8_t* b, size_t n) {
        std::vector<size_t> v;
        size_t i = 0;
        while (true) {
            const size_t k = f(b, i, n);
            if (k >= n) break;
            v.push_back(k);
            i = k + 1;
        }
        return v;
    };
    // Random data biased toward zeros so start codes and near-misses are common.
    for (int t = 0; t < 400; ++t) {
        std::vector<uint8_t> b(1 + rng() % 3000);
        for (auto& x : b) { const uint32_t r = rng() % 8; x = r < 4 ? 0 : r < 6 ? 1 : (uint8_t)rng(); }
        CHECK(allCodes(findStartCodeAvx2, b.data(), b.size()) == allCodes(findStartCodeScalar, b.data(), b.size()),
              "mismatch on random buffer " + std::to_string(t));
    }
    // Adversarial: one start code at every offset of a zero-free buffer, around every
    // 30-byte SIMD step and 32-byte load boundary.
    for (size_t off = 0; off < 200; ++off) {
        std::vector<uint8_t> b(260, 0xAA);
        b[off] = 0; b[off + 1] = 0; b[off + 2] = 1;
        auto a = allCodes(findStartCodeAvx2, b.data(), b.size());
        CHECK(a.size() == 1 && a[0] == off, "missed start code at offset " + std::to_string(off));
        // near miss: 00 00 02 must never match
        b[off + 2] = 2;
        CHECK(allCodes(findStartCodeAvx2, b.data(), b.size()).empty(), "false positive at " + std::to_string(off));
    }
    // Incremental scanning exactly as the assembler does it, splitting the stream at every
    // possible recv boundary: must find the same codes as one pass over the whole buffer.
    std::vector<uint8_t> s(512, 0x55);
    for (size_t p : {0, 7, 29, 30, 31, 32, 33, 61, 62, 63, 64, 100, 254, 255, 256, 400, 509}) {
        s[p] = 0; s[p + 1] = 0; s[p + 2] = 1;
    }
    const auto ref = allCodes(findStartCodeScalar, s.data(), s.size());
    for (size_t split = 1; split < s.size(); ++split) {
        std::vector<size_t> got;
        size_t scan = 0;
        for (size_t end : {split, s.size()}) {
            while (true) {
                if (end < scan + 3) break;
                const size_t k = findStartCodeAvx2(s.data(), scan, end);
                if (k >= end) { scan = end - 2; break; }
                got.push_back(k);
                scan = k + 3;
            }
        }
        CHECK(got == ref, "straddling split at " + std::to_string(split));
    }
    record(name, R::Pass);
}

void testEmulationPrevention() {
    const std::string name = "emulation prevention removal (incl. the frameId-1 SEI example)";
    const uint8_t a[] = {0, 0, 3, 1, 0, 0, 3, 0, 0, 0, 3, 3};
    uint8_t o[16];
    const size_t n = h264::unescape(a, sizeof(a), o, sizeof(o));
    const uint8_t wantA[] = {0, 0, 1, 0, 0, 0, 0, 0, 3};
    CHECK(n == sizeof(wantA) && !memcmp(o, wantA, n), "generic unescape");
    // Wire format example: SEI carrying frameId 1, bytes after the NAL header.
    const uint8_t sei[] = {0x05, 0x14, 0xC4, 0x4E, 0x5F, 0x92, 0x7B, 0x3A, 0x4D, 0x1E, 0x9A, 0x6C,
                           0x2F, 0x81, 0xD5, 0xB7, 0xE3, 0x48, 0x01, 0x00, 0x00, 0x03, 0x00, 0x80};
    uint8_t r[32];
    const size_t m = h264::unescape(sei, sizeof(sei), r, sizeof(r));
    uint32_t id;
    memcpy(&id, r + 18, 4);
    CHECK(m == 23 && id == 1 && r[22] == 0x80, "frameId 1 SEI must unescape to id 1");
    record(name, R::Pass);
}

void testSps() {
    const std::string name = "SPS/PPS parsing against known-good bitstreams (x264 1080p High, 720p Main)";
    struct Case { const wchar_t* file; uint32_t w, h; uint8_t profile; };
    const Case cases[] = {{L"1080p60_4slice_aud.h264", 1920, 1080, 100}, {L"720p30_noaud.h264", 1280, 720, 77}};
    int ran = 0;
    for (auto& c : cases) {
        const std::wstring p = findTestData(c.file);
        if (p.empty()) continue;
        auto b = readFile(p, 4096);
        bool foundSps = false, foundPps = false;
        size_t i = 0;
        while (true) {
            const size_t s = findStartCodeScalar(b.data(), i, b.size());
            if (s + 4 >= b.size()) break;
            const uint8_t t = b[s + 3] & 0x1F;
            const size_t e = findStartCodeScalar(b.data(), s + 3, b.size());
            uint8_t rb[600];
            const size_t m = h264::unescape(&b[s + 4], std::min<size_t>(e - s - 4, 500), rb, sizeof(rb));
            if (t == kNalSps && !foundSps) {
                h264::Sps sps;
                CHECK(h264::parseSps(rb, m, sps), "SPS parse failed");
                CHECK(sps.width == c.w && sps.height == c.h,
                      "SPS size " + std::to_string(sps.width) + "x" + std::to_string(sps.height));
                CHECK(sps.profileIdc == c.profile, "profile_idc " + std::to_string(sps.profileIdc));
                CHECK(sps.frameMbsOnly && sps.chromaFormatIdc == 1, "progressive 4:2:0");
                foundSps = true;
            }
            if (t == kNalPps && !foundPps) {
                h264::Pps pps;
                CHECK(h264::parsePps(rb, m, pps) && pps.spsId == 0, "PPS parse");
                foundPps = true;
            }
            i = s + 3;
        }
        CHECK(foundSps && foundPps, "no SPS/PPS in the first 4 KB");
        ++ran;
    }
    if (!ran) record(name, R::Skip, "testdata/*.h264 not found");
    else record(name, R::Pass, std::to_string(ran) + " streams");
}

void testSeqlock() {
    const std::string name = "seqlock + triple buffering under concurrent read/write stress";
    std::vector<uint8_t> mem((size_t)ipc::kRegionHeaderBytes + ipc::kSlotCount * ipc::kSlotStride);
    void* base = mem.data();
    ipc::initRegion(base);
    auto* h = (ipc::RegionHeader*)base;
    constexpr uint32_t kBytes = 256 * 1024;  // big enough to make tearing likely if the protocol were wrong
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> torn{0}, reads{0}, retriesTotal{0};
    std::thread writer([&] {
        for (uint64_t f = 1; !stop.load(); ++f) {
            const uint32_t slot = ipc::pickWriteSlot(h, base);
            auto* s = ipc::slotHeader(base, slot);
            ipc::beginWrite(s);
            memset(ipc::slotPayload(base, slot), (int)(f & 0xFF), kBytes);
            s->frameId = f;
            s->width = 512; s->height = 341; s->strideY = 512; s->strideUV = 512; s->offsetUV = 512 * 342;
            ipc::endWrite(s);
            ipc::publish(h, slot, qpc());
        }
    });
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            std::vector<uint8_t> buf(kBytes);
            uint64_t retries = 0;
            while (!stop.load()) {
                ipc::FrameInfo fi{};
                if (!ipc::readLatest(h, base, fi, [&](const uint8_t* p, const ipc::SlotHeader&) { memcpy(buf.data(), p, kBytes); },
                                     &retries))
                    continue;
                reads.fetch_add(1);
                const uint8_t want = (uint8_t)(fi.frameId & 0xFF);
                for (uint32_t i = 0; i < kBytes; i += 4096)
                    if (buf[i] != want || buf[kBytes - 1] != want) { torn.fetch_add(1); break; }
            }
            retriesTotal.fetch_add(retries);
        });
    }
    Sleep(1500);
    stop = true;
    writer.join();
    for (auto& t : readers) t.join();
    CHECK(reads.load() > 1000, "too few reads: " + std::to_string(reads.load()));
    CHECK(torn.load() == 0, std::to_string(torn.load()) + " torn frames accepted");
    record(name, R::Pass, std::to_string(reads.load()) + " consistent reads, " + std::to_string(retriesTotal.load()) +
                              " detected-and-retried, 0 torn");
}

void testSpsc() {
    const std::string name = "SPSC ring producer/consumer stress (ordering, no loss, no duplication)";
    // MSVC has no ThreadSanitizer; the equivalent check is a long ordered stress run with
    // the consumer verifying every value.
    auto ring = std::make_unique<SpscRing<uint64_t, 256>>();
    constexpr uint64_t kN = 20'000'000;
    std::atomic<bool> stop{false};
    uint64_t bad = 0, got = 0;
    std::thread prod([&] {
        for (uint64_t i = 1; i <= kN;)
            if (ring->push(i)) ++i; else _mm_pause();
    });
    uint64_t expect = 1, v;
    while (got < kN) {
        if (!ring->pop(v)) { _mm_pause(); continue; }
        if (v != expect) ++bad;
        expect = v + 1;
        ++got;
    }
    prod.join();
    CHECK(bad == 0, std::to_string(bad) + " out-of-order values");
    record(name, R::Pass, std::to_string(kN) + " values");
}

// Builds Annex-B NALs with 4-byte start codes.
void nal(std::vector<uint8_t>& out, std::initializer_list<uint8_t> bytes) {
    out.insert(out.end(), {0, 0, 0, 1});
    out.insert(out.end(), bytes);
}

void testAssembler() {
    const std::string name = "AU assembly closes on the AUD with zero frame delay (wire format example)";
    // Synthetic SPS/PPS: the assembler only needs their NAL types to accept the IDR.
    std::vector<uint8_t> sps = {0x67, 0x42, 0xC0, 0x28, 0xDA, 0x01, 0xE0, 0x08, 0x9F, 0x96, 0x10, 0x00, 0x00, 0x03,
                                0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0xC8, 0xF1, 0x83, 0x2A};
    std::vector<uint8_t> stream;
    // AU 1: SEI(1) SPS PPS IDR AUD
    nal(stream, {0x06, 0x05, 0x14, 0xC4, 0x4E, 0x5F, 0x92, 0x7B, 0x3A, 0x4D, 0x1E, 0x9A, 0x6C, 0x2F, 0x81, 0xD5, 0xB7,
                 0xE3, 0x48, 0x01, 0x00, 0x00, 0x03, 0x00, 0x80});
    stream.insert(stream.end(), {0, 0, 0, 1});
    stream.insert(stream.end(), sps.begin(), sps.end());
    nal(stream, {0x68, 0xCE, 0x3C, 0x80});
    nal(stream, {0x65, 0x88, 0x84, 0x00, 0x33, 0xFF, 0x10, 0x20, 0x30, 0x40, 0x50});
    nal(stream, {0x09, 0x30});
    const size_t au1End = stream.size() - 6;
    // AU 2: exactly the wire format example (frameId 2, two slices, AUD)
    const uint8_t ex[] = {0x00, 0x00, 0x00, 0x01, 0x06, 0x05, 0x14, 0xC4, 0x4E, 0x5F, 0x92, 0x7B, 0x3A, 0x4D,
                          0x1E, 0x9A, 0x6C, 0x2F, 0x81, 0xD5, 0xB7, 0xE3, 0x48, 0x02, 0x00, 0x00, 0x03, 0x00,
                          0x80, 0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x02, 0x1C, 0x0F, 0x00, 0x00, 0x00, 0x01,
                          0x41, 0x01, 0xE7, 0x40, 0x21, 0x00, 0x00, 0x00, 0x01, 0x09, 0x30};
    stream.insert(stream.end(), ex, ex + sizeof(ex));
    const size_t au2End = stream.size() - 6;

    // Feed the whole thing byte by byte (worst-case recv splitting) and in one chunk.
    for (int mode = 0; mode < 2; ++mode) {
        Arena arena;
        std::string info;
        CHECK(arena.init(1 << 20, info), info);
        auto aus = std::make_unique<AuRing>();
        auto params = std::make_unique<ParamRing>();
        auto lengths = std::make_unique<LengthRing>();
        Options o;
        Assembler a;
        a.init(&arena, aus.get(), params.get(), lengths.get(), o, findStartCodeAvx2);
        memcpy(arena.at(0), stream.data(), stream.size());
        std::vector<AuItem> got;
        size_t closedAtByte[2] = {0, 0};
        const size_t step = mode == 0 ? 1 : stream.size();
        for (size_t end = step; end <= stream.size(); end += step) {
            a.onBytes(end, qpc());
            AuItem it;
            while (aus->pop(it)) {
                if (got.size() < 2) closedAtByte[got.size()] = end;
                got.push_back(it);
            }
        }
        CHECK(got.size() == 2, "expected 2 AUs, got " + std::to_string(got.size()) + (mode ? " (one chunk)" : " (byte by byte)"));
        CHECK(got[0].start == 0 && got[0].end == au1End, "AU1 bounds");
        CHECK((got[0].t.flags & kAuIdr) && got[0].t.phoneFrameId == 1 && got[0].t.close == CloseReason::Aud, "AU1 flags/frameId");
        CHECK(got[1].start == au1End && got[1].end == au2End, "AU2 bounds (starts at AU1's AUD)");
        CHECK(got[1].t.phoneFrameId == 2 && got[1].t.slices == 2 && got[1].t.close == CloseReason::Aud, "AU2 frameId/slices");
        if (mode == 0) {
            // Closed when the AUD's NAL header byte arrived: no wait for the next start code.
            CHECK(closedAtByte[0] == au1End + 5, "AU1 closed at byte " + std::to_string(closedAtByte[0]) + ", AUD header is at " +
                                                     std::to_string(au1End + 5));
            CHECK(closedAtByte[1] == au2End + 5, "AU2 closed late");
        }
    }
    record(name, R::Pass, "closed at the AUD header byte, frameIds 1 and 2 from the SEI");
}

void testSidebandCodec() {
    const std::string name = "sideband codec vs the wire format worked examples (FrameTiming, ClockPing, RequestIDR, Hello)";
    const uint8_t ft[80] = {0x4C, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
                            0x35, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF2, 0x05, 0x2A, 0x01, 0x00, 0x00, 0x00,
                            0xFC, 0x45, 0x06, 0x2A, 0x01, 0x00, 0x00, 0x00, 0x1A, 0x46, 0x06, 0x2A, 0x01, 0x00, 0x00, 0x00,
                            0x42, 0x50, 0x06, 0x2A, 0x01, 0x00, 0x00, 0x00, 0x42, 0x50, 0x06, 0x2A, 0x01, 0x00, 0x00, 0x00,
                            0x60, 0x50, 0x06, 0x2A, 0x01, 0x00, 0x00, 0x00, 0x7E, 0x50, 0x06, 0x2A, 0x01, 0x00, 0x00, 0x00};
    PhoneTiming pt;
    CHECK(wire::frameTiming(ft, sizeof(ft), pt), "FrameTiming decode");
    CHECK(pt.frameId == 2 && !pt.isIdr && pt.sliceCount == 2 && pt.frameBytes == 53, "FrameTiming header fields");
    CHECK(pt.capturePtsUs == 5000000000ull && pt.captureCallbackUs == 5000021500ull && pt.encodeSubmitUs == 5000021530ull &&
              pt.encodeOutputUs == 5000024130ull && pt.encodeCompleteUs == 5000024130ull &&
              pt.firstByteSendUs == 5000024160ull && pt.lastByteSendUs == 5000024190ull,
          "FrameTiming timestamps");
    uint8_t ping[24];
    wire::clockPing(ping, 9, 1000);
    const uint8_t wantPing[24] = {0x14, 0, 0, 0, 3, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0xE8, 0x03, 0, 0, 0, 0, 0, 0};
    CHECK(!memcmp(ping, wantPing, 24), "ClockPing bytes");
    uint8_t idr[16];
    wire::requestIdr(idr, 1, 3);
    const uint8_t wantIdr[16] = {0x0C, 0, 0, 0, 5, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0};
    CHECK(!memcmp(idr, wantIdr, 16), "RequestIDR bytes");
    // Hello built from the 4.2 layout: version 1, H.264, CBP, level 42, 1920x1080@60/1, name "iPhone"
    uint8_t h[50] = {};
    const uint32_t len = 46;
    memcpy(h, &len, 4);
    h[4] = 1;
    h[8] = 1; h[10] = 1; h[12] = 1; h[14] = 42;
    const uint32_t w = 1920, hh = 1080, fn = 60, fd = 1, spf = 4, nl = 6;
    memcpy(h + 16, &w, 4); memcpy(h + 20, &hh, 4); memcpy(h + 24, &fn, 4); memcpy(h + 28, &fd, 4);
    memcpy(h + 32, &spf, 4); memcpy(h + 40, &nl, 4); memcpy(h + 44, "iPhone", 6);
    DeviceHello dh;
    CHECK(wire::hello(h, 50, dh), "Hello decode");
    CHECK(dh.deviceName == "iPhone" && dh.width == 1920 && dh.height == 1080 && dh.fpsNum == 60 && dh.level == 42 &&
              dh.profile == "Constrained Baseline",
          "Hello fields");
    h[40] = 200;  // name length overrunning the message must be rejected
    CHECK(!wire::hello(h, 50, dh), "overrunning deviceNameLen accepted");
    record(name, R::Pass);
}

void testClockSync() {
    const std::string name = "NTP clock correlation (offset/RTT, min-RTT filter)";
    // phone = windows + 1,000,000 us; 600 us each way; 50 us phone turnaround.
    const ClockSample s = ntpSample(100, 1000700, 1000750, 1350);
    CHECK(s.offsetUs == 1000000 && s.rttUs == 1200, "offset " + std::to_string(s.offsetUs) + " rtt " + std::to_string(s.rttUs));
    // Asymmetric delay biases the offset by (d1-d2)/2, bounded by rtt/2.
    const ClockSample a = ntpSample(0, 1000900, 1000900, 1200);
    CHECK(a.offsetUs == 1000300 && llabs(a.offsetUs - 1000000) <= a.rttUs / 2, "asymmetric bound");
    MinRttFilter f;
    f.add({5, 3000});
    f.add({7, 900});
    f.add({9, 1500});
    ClockSample b;
    CHECK(f.best(b) && b.offsetUs == 7, "min-RTT sample wins");
    record(name, R::Pass);
}

// ================================================================ integration

struct ReplayRun {
    std::unique_ptr<Pipeline> p;
    bool ok = false;
    std::string err;
};

ReplayRun startReplay(const Options& base, const std::wstring& file, bool ds, bool mf, const std::wstring& dumpDir = L"",
                      uint32_t dumpN = 0) {
    ReplayRun r;
    Options o = base;
    o.replayPath = file;
    o.replayLoop = true;
    o.noDs = !ds;
    o.noMf = !mf;
    o.dumpDir = dumpDir;
    o.dumpFrames = dumpN;
    r.p = std::make_unique<Pipeline>();
    if (!r.p->init(o, r.err)) return r;
    if (!r.p->startReplay(r.err)) return r;
    r.p->publisher.setFormat(r.p->replaySps().width, r.p->replaySps().height, (uint32_t)r.p->replayFps(), 1);
    r.ok = true;
    return r;
}

void testDecodeVsReference(const Options& o) {
    const std::string name = "canned H.264 through the full NVDEC path == reference decode (bit-exact)";
    const std::wstring file = findTestData(L"1080p60_4slice_aud.h264");
    const std::wstring ref = findTestData(L"1080p60_ref.nv12");
    if (file.empty() || ref.empty()) {
        record(name, R::Skip, "testdata/1080p60_4slice_aud.h264 and 1080p60_ref.nv12 needed (see README)");
        return;
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"camerafy_selftest";
    CreateDirectoryW(dir.c_str(), nullptr);
    constexpr uint32_t kN = 40;
    auto run = startReplay(o, file, true, false, dir, kN);
    CHECK(run.ok, run.err);
    const uint64_t t0 = qpc();
    while (run.p->decoder.stats().published.load() < kN + 5 && ticksToUs((int64_t)(qpc() - t0)) < 10e6) Sleep(20);
    run.p->stop();
    const size_t frameBytes = 1920 * 1080 * 3 / 2;
    FILE* rf = _wfopen(ref.c_str(), L"rb");
    CHECK(rf, "cannot open reference");
    std::vector<uint8_t> a(frameBytes), b;
    uint32_t compared = 0, mismatched = 0;
    for (uint32_t k = 1; k <= kN; ++k) {
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%s\\frame_%05u.nv12", dir.c_str(), k);
        b = readFile(path);
        DeleteFileW(path);
        if (b.size() != frameBytes) continue;
        _fseeki64(rf, (int64_t)(k - 1) * (int64_t)frameBytes, SEEK_SET);
        fread(a.data(), 1, frameBytes, rf);
        ++compared;
        if (memcmp(a.data(), b.data(), frameBytes) != 0) ++mismatched;
    }
    fclose(rf);
    RemoveDirectoryW(dir.c_str());
    CHECK(compared >= kN - 1, "only " + std::to_string(compared) + " frames dumped");
    CHECK(mismatched == 0, std::to_string(mismatched) + " of " + std::to_string(compared) + " frames differ from libavcodec");
    record(name, R::Pass, std::to_string(compared) + " frames identical to ffmpeg/libavcodec's decode");
}

void testShmRoundTrip(const Options& o) {
    const std::string name = "shared memory round trip: CLI -> independent reader process (cf_test_reader)";
    const std::wstring file = findTestData(L"1080p60_4slice_aud.h264");
    if (file.empty()) { record(name, R::Skip, "testdata missing"); return; }
    const std::wstring reader = exeDir() + L"\\cf_test_reader.exe";
    if (GetFileAttributesW(reader.c_str()) == INVALID_FILE_ATTRIBUTES) { record(name, R::Skip, "cf_test_reader.exe not built"); return; }
    auto run = startReplay(o, file, true, false);
    CHECK(run.ok, run.err);
    std::wstring cmd = L"\"" + reader + L"\" --frames 360";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    std::string out;
    if (ok) {
        char buf[4096];
        DWORD n;
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
        WaitForSingleObject(pi.hProcess, 20000);
    }
    DWORD code = 1;
    if (ok) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(rd);
    if (ok) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    run.p->stop();
    CHECK(ok && code == 0, "cf_test_reader failed: " + out);
    std::string summary;
    for (const char* key : {"publish->wake", "publish->copied", "seqlock retries"}) {
        const size_t i = out.find(key);
        if (i != std::string::npos) summary += out.substr(i, out.find('\n', i) - i) + "; ";
    }
    record(name, R::Pass, summary);
}

// The MF media source driven in-process, without the Frame Server and without registration:
// load camerafy_mf.dll, get its IMFActivate from DllGetClassObject, activate the source and
// pull samples with an IMFSourceReader. Exercises activation, the section/format handshake,
// Start and the event queue, RequestSample/token handling, the allocator path and the copy.
// The test hook moves the section names to Local\ (creating Global\ needs the Frame Server).
void testMfSourceInProcess(const Options& o) {
    const std::string name = "MF media source in-process (no Frame Server): activate, start, deliver NV12 samples";
    const std::wstring file = findTestData(L"1080p60_4slice_aud.h264");
    if (file.empty()) { record(name, R::Skip, "testdata missing"); return; }
    const std::wstring dllPath = exeDir() + L"\\camerafy_mf.dll";
    HMODULE dll = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) { record(name, R::Skip, "camerafy_mf.dll not next to camerafy.exe"); return; }
    SetEnvironmentVariableW(L"CAMERAFY_MF_LOCAL_TEST", L"1");
    auto run = startReplay(o, file, false, true);
    if (!run.ok) {
        SetEnvironmentVariableW(L"CAMERAFY_MF_LOCAL_TEST", nullptr);
        record(name, R::Fail, run.err);
        return;
    }
    using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto getClass = (GetClassObjectFn)GetProcAddress(dll, "DllGetClassObject");
    IClassFactory* cf = nullptr;
    IMFActivate* act = nullptr;
    IMFMediaSource* src = nullptr;
    IMFSourceReader* rd = nullptr;
    HRESULT hr = getClass ? getClass(ids::kMfSourceClsid, IID_PPV_ARGS(&cf)) : E_NOINTERFACE;
    if (SUCCEEDED(hr)) hr = cf->CreateInstance(nullptr, IID_PPV_ARGS(&act));
    const uint64_t tAct = qpc();
    if (SUCCEEDED(hr)) hr = act->ActivateObject(IID_PPV_ARGS(&src));
    const double actMs = ticksToUs((int64_t)(qpc() - tAct)) / 1000.0;
    // The Frame Server releases the activator right after activation; so do we.
    if (act) { act->Release(); act = nullptr; }
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(src, nullptr, &rd);
    UINT32 w = 0, h = 0;
    if (SUCCEEDED(hr)) {
        IMFMediaType* mt = nullptr;
        if (SUCCEEDED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &mt))) {
            MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &w, &h);
            mt->Release();
        }
    }
    uint32_t frames = 0;
    size_t bytes = 0;
    uint64_t gaps = 0;
    LONGLONG lastTs = -1;
    const uint64_t t0 = qpc();
    while (SUCCEEDED(hr) && frames < 120 && ticksToUs((int64_t)(qpc() - t0)) < 10e6) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* s = nullptr;
        hr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s);
        if (s) {
            DWORD len = 0;
            s->GetTotalLength(&len);
            bytes = len;
            if (lastTs >= 0 && ts <= lastTs) ++gaps;
            lastTs = ts;
            ++frames;
            s->Release();
        }
    }
    const double secs = ticksToUs((int64_t)(qpc() - t0)) / 1e6;
    if (rd) rd->Release();
    if (src) { src->Shutdown(); src->Release(); }
    if (cf) cf->Release();
    // Latency of the MF path's own delivery (published -> sample queued) from the consume log.
    double cons50 = NAN;
    run.p->stop();
    for (auto& s : run.p->telemetry.summaries())
        if (s.name == "published -> MF delivered") cons50 = s.p50;
    SetEnvironmentVariableW(L"CAMERAFY_MF_LOCAL_TEST", nullptr);
    CHECK(SUCCEEDED(hr), "MF call chain: " + hrString(hr));
    CHECK(w == 1920 && h == 1080, "source advertised " + std::to_string(w) + "x" + std::to_string(h) +
                                      " (the CLI's format did not reach the source)");
    CHECK(frames >= 120, "only " + std::to_string(frames) + " samples");
    CHECK(bytes >= 1920 * 1080 * 3 / 2, "sample size " + std::to_string(bytes));
    CHECK(gaps == 0, std::to_string(gaps) + " non-increasing sample times");
    char d[256];
    snprintf(d, sizeof(d), "%u samples %ux%u in %.2f s (%.1f fps), activation %.1f ms, published->sample queued p50 %.0f us",
             frames, w, h, secs, frames / secs, actMs, cons50);
    record(name, R::Pass, d);
}

void testMfCamera(const Options& o) {
    const std::string name = "MF virtual camera registers, enumerates as a video capture source, delivers NV12 frames";
    const ComRegistration reg = queryComServer(ids::kMfSourceClsidStr, KEY_WOW64_64KEY);
    if (!reg.present) { record(name, R::Skip, "camerafy_mf.dll not registered (run register.cmd elevated)"); return; }
    // The Frame Server loads the registered copy. If it is from an older build its shared-memory
    // names differ (ipc.h version), no frame ever arrives, and the synchronous ReadSample below
    // would block forever.
    {
        auto bytes = [](const std::wstring& p) {
            std::string out;
            if (FILE* f = _wfopen(p.c_str(), L"rb")) {
                char b[65536];
                size_t n;
                while ((n = fread(b, 1, sizeof(b), f)) > 0) out.append(b, n);
                fclose(f);
            }
            return out;
        };
        const std::string installed = bytes(reg.path), built = bytes(exeDir() + L"\\camerafy_mf.dll");
        if (!built.empty() && installed != built) {
            record(name, R::Fail, "the registered " + narrow(reg.path) +
                                      " differs from this build's camerafy_mf.dll: re-run register.cmd elevated");
            return;
        }
    }
    const std::wstring file = findTestData(L"1080p60_4slice_aud.h264");
    if (file.empty()) { record(name, R::Skip, "testdata missing"); return; }
    auto run = startReplay(o, file, false, true);
    CHECK(run.ok, run.err);
    MfCamera cam;
    std::string err;
    const std::wstring friendly = std::wstring(L"camerafy-selftest") + ids::kMfSuffix;
    if (!cam.create(friendly, err)) { run.p->stop(); record(name, R::Fail, err); return; }
    // Enumerate like a Media Foundation app.
    IMFAttributes* attrs = nullptr;
    MFCreateAttributes(&attrs, 1);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(attrs, &devs, &count);
    IMFActivate* mine = nullptr;
    std::wstring foundLink;
    for (UINT32 i = 0; i < count; ++i) {
        WCHAR* fn = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &fn, &len))) {
            if (std::wstring(fn).find(L"camerafy-selftest") != std::wstring::npos && !mine) {
                mine = devs[i];
                mine->AddRef();
                WCHAR* link = nullptr;
                if (SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len))) {
                    foundLink = link;
                    CoTaskMemFree(link);
                }
            }
            CoTaskMemFree(fn);
        }
        devs[i]->Release();
    }
    CoTaskMemFree(devs);
    attrs->Release();
    if (!mine) {
        cam.remove();
        run.p->stop();
        record(name, R::Fail, "MFEnumDeviceSources (" + hrString(hr) + ") did not list the camera");
        return;
    }
    IMFMediaSource* src = nullptr;
    hr = mine->ActivateObject(IID_PPV_ARGS(&src));
    IMFSourceReader* rd = nullptr;
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(src, nullptr, &rd);
    uint32_t frames = 0;
    size_t sampleBytes = 0;
    const uint64_t t0 = qpc();
    while (SUCCEEDED(hr) && frames < 60 && ticksToUs((int64_t)(qpc() - t0)) < 10e6) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* s = nullptr;
        hr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s);
        if (s) {
            DWORD len = 0;
            s->GetTotalLength(&len);
            sampleBytes = len;
            ++frames;
            s->Release();
        }
    }
    const double secs = ticksToUs((int64_t)(qpc() - t0)) / 1e6;
    if (rd) rd->Release();
    if (src) { src->Shutdown(); src->Release(); }
    mine->Release();
    cam.remove();
    run.p->stop();
    CHECK(SUCCEEDED(hr), "ReadSample: " + hrString(hr));
    CHECK(frames >= 60, "only " + std::to_string(frames) + " frames");
    CHECK(sampleBytes >= 1920 * 1080 * 3 / 2, "sample size " + std::to_string(sampleBytes));
    const bool pnp = foundLink.find(L"\\\\?\\") == 0 || foundLink.find(L"SWD#") != std::wstring::npos;
    record(name, R::Pass, std::to_string(frames) + " frames in " + std::to_string(secs).substr(0, 4) + " s; symbolic link " +
                              narrow(foundLink.substr(0, 60)) + (pnp ? " (PnP device interface)" : ""));
}

void testDsFilter(const Options& o) {
    const std::string name64 = "DirectShow filter (x64) registers and delivers frames to a DirectShow graph";
    const std::string name32 = "DirectShow filter (x86) registers and delivers frames to a 32-bit DirectShow graph";
    const ComRegistration r64 = queryComServer(ids::kDsFilterClsidStr, KEY_WOW64_64KEY);
    const ComRegistration r32 = queryComServer(ids::kDsFilterClsidStr, KEY_WOW64_32KEY);
    const std::wstring file = findTestData(L"1080p60_4slice_aud.h264");
    if (file.empty()) { record(name64, R::Skip, "testdata missing"); return; }
    auto run = startReplay(o, file, true, false);
    if (!run.ok) { record(name64, R::Fail, run.err); return; }
    DsCamera cam;
    std::string err;
    const std::wstring friendly = std::wstring(L"camerafy-selftest") + ids::kDsSuffix;
    if (!cam.create(friendly, true, err)) { run.p->stop(); record(name64, R::Fail, err); return; }
    auto probe = [&](const std::wstring& exe, const std::string& name) {
        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) { record(name, R::Skip, narrow(exe) + " not built"); return; }
        std::wstring cmd = L"\"" + exe + L"\" --capture \"" + friendly + L"\" --seconds 3";
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        CreatePipe(&rd, &wr, &sa, 0);
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = wr;
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
        CloseHandle(wr);
        std::string out;
        char buf[4096];
        DWORD n;
        if (ok) while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
        DWORD code = 1;
        if (ok) { WaitForSingleObject(pi.hProcess, 20000); GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
        CloseHandle(rd);
        const size_t i = out.find("PASS");
        const size_t f = out.find("FAIL");
        std::string line = i != std::string::npos ? out.substr(i, out.find('\n', i) - i)
                         : f != std::string::npos ? out.substr(f, out.find('\n', f) - f) : out.substr(0, 200);
        const bool sw = out.find("@device_sw_{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\" + narrow(ids::kDsFilterClsidStr)) != std::string::npos;
        if (ok && code == 0) record(name, R::Pass, line + (sw ? "; enumerates as @device_sw_" : ""));
        else record(name, R::Fail, line);
    };
    probe(exeDir() + L"\\ds_probe.exe", name64);
    // Without register.cmd the CLI registers both bitnesses per user, so always probe.
    probe(exeDir() + L"\\..\\..\\x86\\Release\\ds_probe.exe", name32);
    (void)r64;
    (void)r32;
    cam.remove();
    run.p->stop();
}

void testDevice(const Options& o) {
    const std::string name = "usbmux connect to a real device (USB) and Hello on the sideband";
    std::vector<usbmux::Device> devs;
    std::string err;
    if (!usbmux::listDevices(devs, err)) { record(name, R::Skip, "AMDS not reachable: " + err); return; }
    usbmux::Device d;
    if (!usbmux::chooseDevice(devs, narrow(o.udid), d, err)) { record(name, R::Skip, err); return; }
    SOCKET s = usbmux::connectDevice(d.deviceId, defaultPorts().sideband, err);
    if (s == INVALID_SOCKET) { record(name, R::Fail, err); return; }
    Sideband sb;
    DeviceHello h;
    const bool ok = sb.handshake(s, h, err);
    netguard::Snapshot snap;
    netguard::snapshot(snap, err);
    sb.stop();
    CHECK(ok, err);
    CHECK(snap.violations.empty(), "non-loopback socket held");
    record(name, R::Pass, "\"" + h.deviceName + "\" " + std::to_string(h.width) + "x" + std::to_string(h.height));
}

void testNoNetworkSockets() {
    const std::string name = "socket self-check: process holds only 127.0.0.1 -> 127.0.0.1:27015 sockets";
    netguard::Snapshot s;
    std::string err;
    CHECK(netguard::snapshot(s, err), err);
    CHECK(s.violations.empty(), s.violations.empty() ? "" : s.violations[0]);
    record(name, R::Pass, std::to_string(s.sockets.size()) + " socket(s) held");
}

}  // namespace

int runSelfTest(const Options& o) {
    logInfo("camerafy self-test\n\nUnit:");
    testPlistAndHeader();
    testUsbOnly();
    testScanner();
    testEmulationPrevention();
    testSps();
    testSeqlock();
    testSpsc();
    testAssembler();
    testSidebandCodec();
    testClockSync();
    logInfo("\nIntegration:");
    testDecodeVsReference(o);
    testShmRoundTrip(o);
    testMfSourceInProcess(o);
    testMfCamera(o);
    testDsFilter(o);
    testDevice(o);
    testNoNetworkSockets();
    logInfo("\nEnd-to-end with the phone (PRD §15, run manually; see README):");
    for (const char* e : {"ffmpeg -list_devices true -f dshow -i dummy lists both devices (@device_pnp_ / @device_sw_)",
                          "Windows Camera app shows live video from the MF camera",
                          "ffplay -f dshow -i video=\"<name> Webcam\" shows live video",
                          "Chrome getUserMedia lists and displays both", "OBS lists and displays both",
                          "network-disabled run (camerafy --verify-no-network with every adapter disabled)",
                          "latency with per-stage breakdown (camerafy --stats)", "sustained 10-minute run"})
        record(e, R::Skip, "manual");

    int pass = 0, fail = 0, skip = 0;
    for (auto& r : g_results) (r.r == R::Pass ? pass : r.r == R::Fail ? fail : skip)++;
    logInfo("\n%d passed, %d failed, %d skipped", pass, fail, skip);
    return fail ? 1 : 0;
}

}  // namespace cf
