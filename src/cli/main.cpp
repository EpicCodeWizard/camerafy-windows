// camerafy.exe: iPhone over USB -> NVDEC -> two virtual cameras.
#include <winsock2.h>
#include "options.h"
#include "console.h"
#include "env_check.h"
#include "usbmux.h"
#include "lockdown.h"
#include "net_guard.h"
#include "pipeline.h"
#include "sideband.h"
#include "vcam.h"
#include "preview.h"
#include "tuning.h"
#include "ids.h"
#include "qpc.h"
#include <memory>
#include <objbase.h>

namespace cf {
int runSelfTest(const Options& o);
int runBench(const Options& o);
}  // namespace cf

using namespace cf;

namespace {

bool isGenericName(const std::string& n) {
    return n.empty() || n == "iPhone" || n == "iPad" || n == "iPod touch" || n == "iPod";
}

// USB only (PRD 2): anything else is refused, even if it is the only device and would work.
bool selectDevice(const Options& o, usbmux::Device& out, std::string& err) {
    std::vector<usbmux::Device> devs;
    if (!usbmux::listDevices(devs, err)) return false;
    std::vector<std::string> ignored;
    const bool ok = usbmux::chooseDevice(devs, narrow(o.udid), out, err, &ignored);
    for (auto& i : ignored) logWarn("%s", i.c_str());
    if (!ok && devs.empty()) {
        const std::string hint = appleUsbDriverHint();
        if (!hint.empty()) err += ". Cause: " + hint;
    }
    return ok;
}

struct Connection {
    SOCKET video = INVALID_SOCKET;
    DeviceHello hello;
};

bool connectPhone(const usbmux::Device& dev, Sideband& sb, Connection& c, std::string& err) {
    const SidebandPorts ports = defaultPorts();
    SOCKET side = usbmux::connectDevice(dev.deviceId, ports.sideband, err);
    if (side == INVALID_SOCKET) return false;
    if (!sb.handshake(side, c.hello, err)) return false;
    c.video = usbmux::connectDevice(dev.deviceId, ports.video, err);
    return c.video != INVALID_SOCKET;
}

std::wstring resolveName(const Options& o, const DeviceHello& h, const usbmux::Device& dev) {
    if (!o.nameOverride.empty()) return o.nameOverride;
    std::string name = h.deviceName;
    if (isGenericName(name)) {
        // iOS 16+ returns the model name from UIDevice.name without a paid entitlement.
        // lockdownd still reports the user-assigned name to the host.
        std::string ld, err;
        if (lockdown::queryDeviceName(dev.deviceId, ld, err) && !isGenericName(ld)) {
            logWarn("the phone reported the generic name \"%s\" (iOS hides the user-assigned name from apps without "
                    "an entitlement); using lockdownd's DeviceName \"%s\" instead",
                    name.c_str(), ld.c_str());
            name = ld;
        } else if (name.empty()) {
            logWarn("no device name from the sideband%s; falling back to the UDID", err.empty() ? "" : (" or lockdownd (" + err + ")").c_str());
            name = dev.udid;
        }
    }
    return widen(name);
}

int listDevicesCmd(const Options&) {
    std::vector<usbmux::Device> devs;
    std::string err;
    if (!usbmux::listDevices(devs, err)) {
        logError("%s", err.c_str());
        return 1;
    }
    if (devs.empty()) {
        logInfo("no devices attached");
        const std::string hint = appleUsbDriverHint();
        if (!hint.empty()) logError("%s", hint.c_str());
    }
    for (auto& d : devs) {
        std::string name, lerr;
        if (d.isUsb()) lockdown::queryDeviceName(d.deviceId, name, lerr);
        logInfo("%-42s DeviceID %-4u %-8s %s%s", d.udid.c_str(), d.deviceId, d.connectionType.c_str(),
                name.empty() ? "" : ("\"" + name + "\"").c_str(), d.isUsb() ? "" : "  (refused: not USB)");
    }
    return 0;
}

int runCamera(const Options& o) {
    const bool replay = !o.replayPath.empty();
    EnvResult env = checkEnvironment(o, !replay);
    if (!env.ok) {
        logError("environment check failed; see above");
        return 1;
    }
    logInfo("Process: %s", tuning::setupProcess(o).c_str());
    // Everything in the camera path at the top of the scheduler; all restored by
    // the destructors on every exit path.
    tuning::PowerPlanBoost power;
    if (o.powerPlanBoost) logInfo("%s", power.apply().c_str());
    tuning::Boosts boosts;
    boosts.realtime = o.realtimeClass;
    for (auto& line : boosts.refresh(!replay && o.amdsBoost, o.frameServerBoost)) logInfo("%s", line.c_str());
    uint64_t nextBoostCheck = 0;

    auto p = std::make_unique<Pipeline>();
    std::string err;
    if (!p->init(o, err)) {
        logError("pipeline init: %s", err.c_str());
        return 1;
    }
    logInfo("Arena: %s", p->arenaInfo.c_str());
    if (!o.noDs) logInfo("IPC: %s", p->publisherInfo.c_str());
    logInfo("Readback: %s (--copy %s)", p->decoder.copyPathName(), o.copyMode == CopyMode::Linear ? "linear" : "2d");
    logInfo("Threads: %s; hand-offs %s; MMCSS %s", p->plan.report.c_str(), o.spinStages ? "spin" : "block",
            o.mmcss ? "on (blocking threads)" : "off");

    usbmux::Device dev;
    auto sb = std::make_unique<Sideband>();
    Connection conn;
    std::wstring name;
    uint32_t w = 0, h = 0, fpsNum = 60, fpsDen = 1;
    if (replay) {
        name = o.nameOverride;
    } else {
        if (!selectDevice(o, dev, err)) {
            logError("%s", err.c_str());
            return 1;
        }
        logInfo("Device: %s (DeviceID %u, %s)", dev.udid.c_str(), dev.deviceId, dev.connectionType.c_str());
        // The app may not be open yet (or iOS has not rebuilt its listeners): wait for it rather
        // than exit, retrying every second like a reconnect (FINAL-REPORT 4).
        bool waitingLogged = false;
        while (!connectPhone(dev, *sb, conn, err)) {
            if (err.find("ConnRefused") == std::string::npos || g_quit.load()) {
                logError("%s", err.c_str());
                return 1;
            }
            if (!waitingLogged) {
                logInfo("waiting for the camerafy app on the phone (open it; it can then go to the background in "
                        "PiP). Ctrl+C to quit.");
                waitingLogged = true;
            }
            Sleep(1000);
            if (g_quit.load()) return 1;
            sb = std::make_unique<Sideband>();
            usbmux::Device d2;
            Options pin = o;
            pin.udid = widen(dev.udid);
            if (selectDevice(pin, d2, err)) dev = d2;
        }
        const DeviceHello& hh = conn.hello;
        logInfo("Hello: protocol %u, \"%s\", %s %s level %u.%u, %ux%u @ %u/%u, ~%u slices/frame, slices %s",
                hh.protocolVersion, hh.deviceName.c_str(), hh.codec.c_str(), hh.profile.c_str(), hh.level / 10,
                hh.level % 10, hh.width, hh.height, hh.fpsNum, hh.fpsDen, hh.slicesPerFrame,
                hh.slicesProgressive ? "PROGRESSIVE (multiple encoder callbacks per frame)" : "not progressive (one callback per frame)");
        name = resolveName(o, hh, dev);
        w = hh.width;
        h = hh.height;
        fpsNum = hh.fpsNum ? hh.fpsNum : 60;
        fpsDen = hh.fpsDen ? hh.fpsDen : 1;
        if (w && h && !p->decoder.precreate(w, h, err)) logWarn("%s", err.c_str());
    }
    std::string sErr;
    if (replay) {
        if (!p->startReplay(sErr)) {
            logError("%s", sErr.c_str());
            return 1;
        }
        w = p->replaySps().width;
        h = p->replaySps().height;
        fpsNum = (uint32_t)(p->replayFps() * 1000 + 0.5);
        fpsDen = 1000;
        if (fpsNum % 1000 == 0) { fpsNum /= 1000; fpsDen = 1; }
    } else {
        if (!p->startSocket(conn.video, sErr)) {
            logError("%s", sErr.c_str());
            return 1;
        }
        sb->start(&p->phone, &p->lengths, &p->clock, &p->assembler.wantIdr);
    }
    p->publisher.setFormat(w, h, fpsNum, fpsDen);

    // PRD §2: every socket we hold must be loopback to 127.0.0.1:27015. Abort otherwise.
    netguard::Snapshot snap;
    if (!netguard::snapshot(snap, err)) logWarn("socket self-check failed to run: %s", err.c_str());
    else if (!snap.violations.empty()) {
        logError("USB-only constraint violated; aborting. Offending sockets:");
        for (auto& v : snap.violations) logError("  %s", v.c_str());
        p->stop();
        return 3;
    } else {
        logInfo("Network self-check: %zu socket(s), all 127.0.0.1 -> 127.0.0.1:27015", snap.sockets.size());
    }

    MfCamera mf;
    DsCamera ds;
    if (!o.noDs) {
        const std::wstring dsName = name + ids::kDsSuffix;
        if (ds.create(dsName, o.dsPerUser, err)) logInfo("DirectShow camera: \"%s\"", narrow(dsName).c_str());
        else logError("DirectShow camera: %s", err.c_str());
    }
    if (!o.noMf) {
        const std::wstring mfName = name + ids::kMfSuffix;
        if (mf.create(mfName, err)) logInfo("Media Foundation camera: \"%s\"  %s", narrow(mfName).c_str(), narrow(mf.symbolicLink()).c_str());
        else logError("Media Foundation camera: %s", err.c_str());
    }
    Preview preview;
    if (o.preview) preview.start(name + L" (camerafy preview)", err);

    // Hot-unplug: AMDS pushes Attached/Detached on a dedicated Listen socket (loopback too).
    std::atomic<bool> detached{false};
    usbmux::Listener listener;
    if (!replay) {
        const uint32_t myId = dev.deviceId;
        std::string lerr;
        if (!listener.start([&detached, myId](bool attached, const usbmux::Device& d) {
                if (!attached && d.deviceId == myId) detached.store(true);
            }, lerr))
            logWarn("usbmux Listen failed (%s); unplug is detected by the stream ending instead", lerr.c_str());
    }

    netguard::Recorder rec;
    const uint64_t t0 = qpc();
    uint32_t lastW = w, lastH = h;
    bool reconnecting = false;
    uint64_t nextReconnect = 0;
    int reconnectState = 0;
    uint64_t lostQpc = 0;
    uint64_t lastAuCount = 0, lastAuChange = qpc(), pausedSince = 0;
    uint64_t seenFormatChanges = 0, formatChangeQpc = 0, idrsAtFormatChange = 0;
    bool idrRequested = false;
    while (!g_quit.load()) {
        Sleep(100);
        if (o.verifyNoNetwork) {
            rec.sample();
            if (ticksToUs((int64_t)(qpc() - t0)) > 10e6) break;
        }
        if (preview.closedByUser()) break;
        if (o.durationS > 0 && ticksToUs((int64_t)(qpc() - t0)) > o.durationS * 1e6) break;
        // Phone format change: the in-band SPS is authoritative. The cameras
        // keep the size they advertised (open DirectShow graphs and Frame Server clients cannot
        // renegotiate, and recreating the MF camera kills every app using it); NVDEC scales the
        // new format into that size. Restart camerafy to advertise the new native size.
        const uint32_t nw = p->decoder.stats().nativeWidth.load(), nh = p->decoder.stats().nativeHeight.load();
        if (nw && nh && (nw != lastW || nh != lastH)) {
            const uint32_t ow = p->decoder.stats().width.load(), oh = p->decoder.stats().height.load();
            DeviceHello f;
            if (sb->latestFormat(f) && f.fpsNum) {
                fpsNum = f.fpsNum;
                fpsDen = f.fpsDen;
                p->publisher.setFormat(ow, oh, fpsNum, fpsDen);
            }
            if (nw == ow && nh == oh)
                logInfo("phone format is %ux%u again: native, no scaling", nw, nh);
            else
                logWarn("phone format changed to %ux%u; the cameras stay %ux%u (scaled on the GPU, centre crop if the "
                        "aspect ratio differs). Restart camerafy to switch the cameras to %ux%u.",
                        nw, nh, ow, oh, nw, nh);
            lastW = nw;
            lastH = nh;
        }
        // The Frame Server starts only when an app opens a camera, and either process can
        // restart: re-check every 2 s and boost new instances.
        if (qpc() >= nextBoostCheck) {
            nextBoostCheck = qpc() + (uint64_t)usToTicks(2e6);
            for (auto& line : boosts.refresh(!replay && o.amdsBoost, o.frameServerBoost)) logInfo("%s", line.c_str());
        }
        if (replay) {
            if (p->sourceEnded() && !o.replayLoop) break;
            continue;
        }
        // Pauses: the video socket stays connected but silent while the phone's
        // PiP window is closed, another app has the camera, or iOS throttles. Not an error; the
        // cameras hold the last frame and the next AU is an IDR in the same format. A format
        // switch also leaves a gap (up to ~1.2 s to 4K), so stay quiet within 3 s of one.
        {
            const uint64_t now = qpc();
            const uint64_t aus = p->assembler.stats().aus.load(std::memory_order_relaxed);
            const uint64_t idrs = p->assembler.stats().idrs.load(std::memory_order_relaxed);
            const uint64_t fcs = sb->formatChanges.load(std::memory_order_relaxed);
            if (fcs < seenFormatChanges) seenFormatChanges = fcs;  // new sideband after a reconnect
            if (fcs > seenFormatChanges) {
                seenFormatChanges = fcs;
                formatChangeQpc = now;
                idrsAtFormatChange = idrs;
                idrRequested = false;
            }
            // No IDR 3 s after a FormatChange: ask once; never during the gap.
            if (formatChangeQpc && !idrRequested && idrs == idrsAtFormatChange &&
                ticksToUs((int64_t)(now - formatChangeQpc)) > 3e6) {
                idrRequested = true;
                logWarn("no IDR within 3 s of the phone's format change; requesting one");
                sb->requestIdr(1);
            }
            if (aus != lastAuCount) {
                if (pausedSince)
                    logInfo("phone resumed after %.1f s", ticksToUs((int64_t)(now - pausedSince)) / 1e6);
                pausedSince = 0;
                lastAuCount = aus;
                lastAuChange = now;
            } else if (!pausedSince && aus && !reconnecting && ticksToUs((int64_t)(now - lastAuChange)) > 1.5e6 &&
                       (!formatChangeQpc || ticksToUs((int64_t)(now - formatChangeQpc)) > 3e6)) {
                pausedSince = lastAuChange;
                logWarn("camera paused on the phone (PiP closed, another app using the camera, or iOS throttling); "
                        "the cameras hold the last frame until it resumes");
            }
        }
        // Phone unplugged / app backgrounded: keep the cameras up and reconnect when it is back.
        if (detached.exchange(false) && !reconnecting) logWarn("AMDS reports the phone was unplugged");
        if (p->sourceEnded() || !sb->connected()) {
            if (!reconnecting) {
                logWarn("lost the phone's stream; cameras stay up, reconnecting when it returns");
                reconnecting = true;
                reconnectState = 0;
                lostQpc = qpc();
            }
            if (qpc() >= nextReconnect) {
                nextReconnect = qpc() + (uint64_t)usToTicks(1e6);
                usbmux::Device d2;
                Options pin = o;
                pin.udid = widen(dev.udid);  // the same phone, even if others are attached now
                // Say once per state what the reconnect is waiting for, so a silent 0 fps is
                // never a mystery: 1 = phone not on USB, 2 = on USB but the app is not listening,
                // 3 = other error.
                auto state = [&](int st, const char* msg) {
                    if (reconnectState == st) return;
                    reconnectState = st;
                    logWarn("%s (%.0f s since the stream was lost)", msg, ticksToUs((int64_t)(qpc() - lostQpc)) / 1e6);
                };
                if (!selectDevice(pin, d2, err)) {
                    state(1, "the phone is not visible to Apple Mobile Device Service (unplugged, rebooting, or "
                             "locked after a restart: unlock it; if it stays missing, replug the cable)");
                } else {
                    sb->stop();
                    sb = std::make_unique<Sideband>();
                    Connection c2;
                    if (connectPhone(d2, *sb, c2, err) && p->reconnect(c2.video, err)) {
                        sb->start(&p->phone, &p->lengths, &p->clock, &p->assembler.wantIdr);
                        dev = d2;
                        reconnecting = false;
                        logInfo("phone reconnected after %.1f s", ticksToUs((int64_t)(qpc() - lostQpc)) / 1e6);

                    } else if (err.find("ConnRefused") != std::string::npos) {
                        state(2, "the phone is back on USB but the camerafy app is not listening: unlock the phone "
                                 "and open the app (after an iPhone restart iOS does not relaunch it)");
                    } else {
                        state(3, ("reconnect failed: " + err).c_str());
                    }
                }
            }
        }
    }

    logInfo("shutting down");
    listener.stop();
    ds.remove();
    mf.remove();
    preview.stop();
    sb->stop();
    p->stop();
    if (o.verifyNoNetwork) {
        logInfo("\n--verify-no-network: %u samples over ~10 s. Every socket this process held:", rec.samples());
        for (auto& s : rec.seen()) logInfo("  %s", s.c_str());
        if (rec.violations().empty()) {
            logInfo("PASS: no socket other than 127.0.0.1 -> 127.0.0.1:27015 (AMDS loopback) was ever held; "
                    "no listeners, no UDP, no IPv6.");
        } else {
            logError("FAIL: violations:");
            for (auto& v : rec.violations()) logError("  %s", v.c_str());
        }
        const auto ad = netguard::activeAdapters();
        logInfo("Network adapters up during the run: %zu%s", ad.size(), ad.empty() ? " (all disabled: the network-disabled test condition)" : "");
        for (auto& a : ad) logInfo("  %s", a.c_str());
    }
    if (o.stats || o.verifyNoNetwork) p->telemetry.printFullTable();
    if (!replay) {
        logInfo("Sideband: %llu FrameTiming, %llu pings (min RTT %.2f ms -> USB+AMDS one-way ~%.2f ms), %llu IDR requests",
                (unsigned long long)sb->frameTimings.load(), (unsigned long long)sb->ping.count.load(),
                sb->ping.minRttUs.load() / 1000.0, sb->ping.minRttUs.load() / 2000.0,
                (unsigned long long)sb->idrRequests.load());
    }
    return o.verifyNoNetwork && !rec.violations().empty() ? 4 : 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options o;
    if (!parseOptions(argc, argv, o)) return 2;
    g_verbose = o.verbose;
    installCtrlHandler();
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // One instance at a time: two writers would share the same shared-memory sections.
    HANDLE instance = nullptr;
    if (!o.listDevices) {
        instance = CreateMutexW(nullptr, TRUE, L"Local\\Camerafy.Instance");
        if (instance && GetLastError() == ERROR_ALREADY_EXISTS) {
            logError("another camerafy.exe (camera, --self-test or --bench) is already running in this session");
            return 5;
        }
    }
    int rc;
    if (o.selfTest) rc = runSelfTest(o);
    else if (o.listDevices) rc = listDevicesCmd(o);
    else if (o.bench) rc = runBench(o);
    else rc = runCamera(o);
    CoUninitialize();
    WSACleanup();
    if (instance) CloseHandle(instance);
    return rc;
}
