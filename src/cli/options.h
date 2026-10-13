#pragma once
// Every configuration decision resolved at startup into one flat struct of scalars.
// The hot path reads these; it never parses or branches on option strings.
#include <stdint.h>
#include <string>

namespace cf {

enum class BoundaryMode : uint8_t {
    Auto = 0,     // whatever the stream supports, best first (sideband length > AUD > next frame)
    Aud,          // AUD (NAL 9) or any next-AU NAL closes the current AU
    Length,       // sideband gives the frame's byte length
    NextFrame,    // forced fallback: close only on the next frame's first slice
};

enum class GpuWait : uint8_t {
    Hybrid = 0,  // sleep (hi-res timer) to just before the predicted finish, then poll
    Spin,        // poll from the start (a core while the GPU works)
    Block,       // let the driver block (blocking-sync context)
};

enum class Keepalive : uint8_t {
    Kernel = 0,  // one sleeping warp resident on the GPU (relaunched every 50 ms)
    Memset,      // the old continuous 32 MB memsets (a core + ~100 W)
    Off,
};

enum class CopyMode : uint8_t {
    Linear = 0,   // one cuMemcpyDtoHAsync of the whole pitch-linear surface
    TwoPlane2D,   // two cuMemcpy2DAsync (luma, chroma)
};

struct Options {
    // modes
    bool listDevices = false;
    bool verifyNoNetwork = false;
    bool selfTest = false;
    bool bench = false;
    bool benchMicroOnly = false;
    bool preview = false;
    bool stats = false;
    bool verbose = false;
    bool noMf = false;
    bool noDs = false;

    std::wstring udid;
    std::wstring csvPath;
    std::wstring replayPath;     // dev: feed a canned Annex-B file instead of the phone
    std::wstring nameOverride;   // dev: with --replay, the "phone" name
    double replayFps = 0;        // 0 = from SPS timing info, else 60
    bool replayLoop = true;
    double replayStallEveryS = 0;  // --replay-stall-every: simulate a receiver/USB stall this often
    double replayStallMs = 0;      // ... holding frames this long, then delivering them back to back
    bool replayNaive = false;    // deliver AUs without the trailing next-AU AUD (shows fallback cost)
    std::wstring dumpDir;        // dev: dump decoded frames (first N) as .nv12
    uint32_t dumpFrames = 0;
    double durationS = 0;        // stop after this many seconds (0 = until Ctrl+C)

    // pipeline tuning, all measurable via --bench
    BoundaryMode boundary = BoundaryMode::Auto;
    CopyMode copyMode = CopyMode::Linear;
    // Thread layout. Default: recv, assemble and decode-submit all run on the recv thread
    // (blocked in recv between frames), and the mapper sleeps through the GPU work. Nothing
    // spins between frames, which keeps the CPU budget low.
    bool inlineAssembler = true;     // run the assembler on the recv thread
    bool inlineDecode = true;        // and the parser + decode submit too (needs inlineAssembler)
    bool spinStages = false;         // --spin: hand-offs spin rather than block (a core each)
    GpuWait gpuWait = GpuWait::Hybrid;
    Keepalive keepalive = Keepalive::Kernel;
    bool mmcss = true;
    bool pinThreads = true;
    bool realtimeClass = false;      // --realtime-unsafe: REALTIME class + GPU priority for us, AMDS, Frame Server
    bool hostRegister = true;        // cuMemHostRegister the shared-memory views
    bool amdsBoost = true;           // AppleMobileDeviceProcess: REALTIME/HIGH, no EcoQoS, GPU priority, while we run
    bool frameServerBoost = true;    // the Frame Server process hosting the MF camera, likewise (needs elevation)
    bool catchUp = true;             // after a stall, publish only the newest of a burst of queued frames
    bool powerPlanBoost = true;      // High/Ultimate plan + best-performance power mode while we run
    uint32_t readerSpinUs = 3000;    // readers' spin window after the wake-ahead signal
    uint32_t gpuOrdinal = 0;
    bool dsPerUser = true;           // register the DirectShow device per-user (no admin)
    uint32_t dsBuffers = 1;
};

// Returns false and prints usage on error.
bool parseOptions(int argc, wchar_t** argv, Options& o);
void printUsage();

}  // namespace cf
