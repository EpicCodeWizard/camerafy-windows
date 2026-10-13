#include "options.h"
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>

namespace cf {

void printUsage() {
    fwprintf(stderr,
             L"camerafy.exe [options]\n"
             L"\n"
             L"  --list-devices            Enumerate USB-attached iPhones and exit\n"
             L"  --udid <udid>             Select a specific device\n"
             L"  --cams both|ds|mf         Which virtual cameras to create (default both). A camera no\n"
             L"                            app has open costs nothing per frame either way\n"
             L"  --no-mf / --no-ds         Same as --cams ds / --cams mf\n"
             L"  --stats                   Full percentile table on exit\n"
             L"  --csv <path>              Per-frame timing export\n"
             L"  --verify-no-network       Prove the USB-only constraint and exit\n"
             L"  --self-test               Run the full verification suite\n"
             L"  --bench                   Startup micro-benchmarks, choose fastest paths\n"
             L"  --preview                 Open a D3D11 preview window (off the hot path)\n"
             L"  --verbose                 Full HRESULT/CUresult decoding\n"
             L"\n"
             L"Development / measurement:\n"
             L"  --replay <file.h264>      Feed a canned Annex-B stream instead of the phone\n"
             L"  --name <name>             Device name to use with --replay (default iPhone-Sim)\n"
             L"  --replay-fps <fps>        Pacing for --replay (default: SPS timing, else 60)\n"
             L"  --replay-once             Stop at end of file instead of looping\n"
             L"  --duration <s>            Stop after s seconds (scripted runs)\n"
             L"  --replay-naive            Do not append the next AUD to each AU (shows fallback cost)\n"
             L"  --replay-stall-every <s> --replay-stall-ms <ms>\n"
             L"                            Hold frames for <ms> every <s> seconds, then deliver them back to back\n"
             L"                            (a phone draining its socket backlog after a PC/USB stall)\n"
             L"  --dump <dir> <n>          Write the first n decoded frames as .nv12\n"
             L"  --boundary auto|aud|length|next-frame\n"
             L"  --copy linear|2d          NVDEC surface readback strategy\n"
             L"  --separate-assembler      Assemble AUs on their own thread (default: on the recv thread)\n"
             L"  --decode-thread           Parse/submit on their own thread (default: on the recv thread)\n"
             L"  --spin                    Spinning hand-offs (old default; costs a core per stage)\n"
             L"  --gpu-wait hybrid|spin|block  How the mapper waits for decode and DMA (default hybrid:\n"
             L"                            sleep to just before the predicted finish, then poll)\n"
             L"  --keepalive kernel|memset|off  How the GPU is kept out of idle clocks (default kernel:\n"
             L"                            one sleeping warp resident on the GPU: no CPU, little power)\n"
             L"  --no-mmcss  --no-pin  --no-host-register\n"
             L"  --no-amds-boost           Leave Apple Mobile Device Service at normal priority / efficiency mode\n"
             L"  --no-fs-boost             Leave the Camera Frame Server process at its normal priority\n"
             L"  --no-power-boost          Keep the current power plan / power mode\n"
             L"  --no-catchup              Publish every decoded frame, even ones already superseded by a newer\n"
             L"                            one (default: after a PC or USB stall, skip the stale burst)\n"
             L"  --realtime-unsafe         REALTIME priority class + GPU priority for camerafy, AMDS and the Frame\n"
             L"                            Server (elevated). Can freeze the whole PC while the phone streams\n"
             L"  --no-boost                All of the above: no priority changes outside default HIGH\n"
             L"  --reader-spin-us <us>     Consumer spin window after the wake-ahead signal (default 3000)\n"
             L"  --gpu <ordinal>\n"
             L"  --ds-machine              Register the DirectShow device machine-wide (admin)\n"
             L"  --ds-buffers <n>          DirectShow allocator buffers (default 1)\n");
}

bool parseOptions(int argc, wchar_t** argv, Options& o) {
    auto need = [&](int& i) -> const wchar_t* {
        if (i + 1 >= argc) {
            fwprintf(stderr, L"%s needs a value\n", argv[i]);
            return nullptr;
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        const wchar_t* v = nullptr;
        if (!wcscmp(a, L"--list-devices")) o.listDevices = true;
        else if (!wcscmp(a, L"--udid")) { if (!(v = need(i))) return false; o.udid = v; }
        else if (!wcscmp(a, L"--cams")) {
            if (!(v = need(i))) return false;
            if (!wcscmp(v, L"both")) { o.noDs = false; o.noMf = false; }
            else if (!wcscmp(v, L"ds")) { o.noDs = false; o.noMf = true; }
            else if (!wcscmp(v, L"mf")) { o.noDs = true; o.noMf = false; }
            else { fwprintf(stderr, L"bad --cams %s (both, ds or mf)\n", v); return false; }
        }
        else if (!wcscmp(a, L"--no-mf")) o.noMf = true;
        else if (!wcscmp(a, L"--no-ds")) o.noDs = true;
        else if (!wcscmp(a, L"--stats")) o.stats = true;
        else if (!wcscmp(a, L"--csv")) { if (!(v = need(i))) return false; o.csvPath = v; }
        else if (!wcscmp(a, L"--verify-no-network")) o.verifyNoNetwork = true;
        else if (!wcscmp(a, L"--self-test")) o.selfTest = true;
        else if (!wcscmp(a, L"--bench")) o.bench = true;
        else if (!wcscmp(a, L"--bench-micro")) { o.bench = true; o.benchMicroOnly = true; }
        else if (!wcscmp(a, L"--preview")) o.preview = true;
        else if (!wcscmp(a, L"--verbose")) o.verbose = true;
        else if (!wcscmp(a, L"--replay")) { if (!(v = need(i))) return false; o.replayPath = v; }
        else if (!wcscmp(a, L"--name")) { if (!(v = need(i))) return false; o.nameOverride = v; }
        else if (!wcscmp(a, L"--replay-fps")) { if (!(v = need(i))) return false; o.replayFps = _wtof(v); }
        else if (!wcscmp(a, L"--replay-once")) o.replayLoop = false;
        else if (!wcscmp(a, L"--duration")) { if (!(v = need(i))) return false; o.durationS = _wtof(v); }
        else if (!wcscmp(a, L"--replay-naive")) o.replayNaive = true;
        else if (!wcscmp(a, L"--replay-stall-every")) { if (!(v = need(i))) return false; o.replayStallEveryS = _wtof(v); }
        else if (!wcscmp(a, L"--replay-stall-ms")) { if (!(v = need(i))) return false; o.replayStallMs = _wtof(v); }
        else if (!wcscmp(a, L"--dump")) {
            if (!(v = need(i))) return false;
            o.dumpDir = v;
            if (!(v = need(i))) return false;
            o.dumpFrames = (uint32_t)_wtoi(v);
        }
        else if (!wcscmp(a, L"--boundary")) {
            if (!(v = need(i))) return false;
            if (!wcscmp(v, L"auto")) o.boundary = BoundaryMode::Auto;
            else if (!wcscmp(v, L"aud")) o.boundary = BoundaryMode::Aud;
            else if (!wcscmp(v, L"length")) o.boundary = BoundaryMode::Length;
            else if (!wcscmp(v, L"next-frame")) o.boundary = BoundaryMode::NextFrame;
            else { fwprintf(stderr, L"bad --boundary %s\n", v); return false; }
        }
        else if (!wcscmp(a, L"--copy")) {
            if (!(v = need(i))) return false;
            if (!wcscmp(v, L"linear")) o.copyMode = CopyMode::Linear;
            else if (!wcscmp(v, L"2d")) o.copyMode = CopyMode::TwoPlane2D;
            else { fwprintf(stderr, L"bad --copy %s\n", v); return false; }
        }
        else if (!wcscmp(a, L"--inline-assembler")) o.inlineAssembler = true;
        else if (!wcscmp(a, L"--separate-assembler")) { o.inlineAssembler = false; o.inlineDecode = false; }
        else if (!wcscmp(a, L"--decode-thread")) o.inlineDecode = false;
        else if (!wcscmp(a, L"--block")) o.spinStages = false;
        else if (!wcscmp(a, L"--spin")) { o.spinStages = true; o.inlineDecode = false; }
        else if (!wcscmp(a, L"--gpu-wait")) {
            if (!(v = need(i))) return false;
            if (!wcscmp(v, L"hybrid")) o.gpuWait = GpuWait::Hybrid;
            else if (!wcscmp(v, L"spin")) o.gpuWait = GpuWait::Spin;
            else if (!wcscmp(v, L"block")) o.gpuWait = GpuWait::Block;
            else { fwprintf(stderr, L"bad --gpu-wait %s\n", v); return false; }
        }
        else if (!wcscmp(a, L"--keepalive")) {
            if (!(v = need(i))) return false;
            if (!wcscmp(v, L"kernel")) o.keepalive = Keepalive::Kernel;
            else if (!wcscmp(v, L"memset")) o.keepalive = Keepalive::Memset;
            else if (!wcscmp(v, L"off")) o.keepalive = Keepalive::Off;
            else { fwprintf(stderr, L"bad --keepalive %s\n", v); return false; }
        }
        else if (!wcscmp(a, L"--no-mmcss")) o.mmcss = false;
        else if (!wcscmp(a, L"--no-amds-boost")) o.amdsBoost = false;
        else if (!wcscmp(a, L"--no-realtime")) o.realtimeClass = false;
        else if (!wcscmp(a, L"--no-fs-boost")) o.frameServerBoost = false;
        else if (!wcscmp(a, L"--no-power-boost")) o.powerPlanBoost = false;
        else if (!wcscmp(a, L"--no-catchup")) o.catchUp = false;
        else if (!wcscmp(a, L"--no-boost")) { o.amdsBoost = o.frameServerBoost = o.powerPlanBoost = false; o.realtimeClass = false; }
        else if (!wcscmp(a, L"--no-pin")) o.pinThreads = false;
        else if (!wcscmp(a, L"--realtime-unsafe")) o.realtimeClass = true;
        else if (!wcscmp(a, L"--realtime")) {}  // was REALTIME class; now only --realtime-unsafe (REALTIME froze the PC)
        else if (!wcscmp(a, L"--no-host-register")) o.hostRegister = false;
        else if (!wcscmp(a, L"--no-poll-status")) o.gpuWait = GpuWait::Block;
        else if (!wcscmp(a, L"--no-gpu-keepalive")) o.keepalive = Keepalive::Off;
        else if (!wcscmp(a, L"--reader-spin-us")) { if (!(v = need(i))) return false; o.readerSpinUs = (uint32_t)_wtoi(v); }
        else if (!wcscmp(a, L"--gpu")) { if (!(v = need(i))) return false; o.gpuOrdinal = (uint32_t)_wtoi(v); }
        else if (!wcscmp(a, L"--ds-machine")) o.dsPerUser = false;
        else if (!wcscmp(a, L"--ds-buffers")) { if (!(v = need(i))) return false; o.dsBuffers = (uint32_t)_wtoi(v); }
        else if (!wcscmp(a, L"--help") || !wcscmp(a, L"-h") || !wcscmp(a, L"/?")) { printUsage(); return false; }
        else {
            fwprintf(stderr, L"unknown option %s\n\n", a);
            printUsage();
            return false;
        }
    }
    if (!o.replayPath.empty() && o.nameOverride.empty()) o.nameOverride = L"iPhone-Sim";
    return true;
}

}  // namespace cf
