#pragma once
#include <winsock2.h>
#include <atomic>
#include <string>
#include <vector>
#include "options.h"
#include "arena.h"
#include "assembler.h"
#include "decoder.h"
#include "publisher.h"
#include "telemetry.h"
#include "tuning.h"

namespace cf {

// Owns every hot-path object. Allocated once at startup; nothing here allocates afterwards.
class Pipeline final {
public:
    bool init(const Options& o, std::string& err);
    // Source: a usbmux tunnel to the phone's video port...
    bool startSocket(SOCKET video, std::string& err);
    // ...or a canned Annex-B file paced at the stream's frame rate (development/measurement).
    bool startReplay(std::string& err);
    // After the phone's stream ended: resync on a new video socket, keeping decoder/cameras.
    bool reconnect(SOCKET video, std::string& err);
    void stop();

    bool sourceEnded() const { return sourceEnded_.load(); }
    double replayFps() const { return replayFps_; }
    const h264::Sps& replaySps() const { return replaySps_; }

    Arena arena;
    AuRing aus;
    ParamRing params;
    LengthRing lengths;
    ChunkRing chunks;
    TimingRing timing;
    PhoneTimingRing phone;
    ClockEstimate clock;
    Assembler assembler;
    Decoder decoder;
    Publisher publisher;
    Telemetry telemetry;
    tuning::CorePlan plan;
    std::string arenaInfo, publisherInfo;

private:
    bool decodeInline() const { return opt_.inlineAssembler && opt_.inlineDecode && !opt_.spinStages; }
    static DWORD WINAPI recvProc(void* self);
    static DWORD WINAPI replayProc(void* self);
    static DWORD WINAPI assemblerProc(void* self);
    static DWORD WINAPI decodeProc(void* self);
    static DWORD WINAPI mapperProc(void* self);
    static DWORD WINAPI keepaliveProc(void* self);
    void recvLoop();
    void replayLoop();
    void assemblerLoop();
    bool loadReplay(std::string& err);
    void deliver(uint64_t& w, const uint8_t* p, size_t n, ChunkRing* pendingTarget);
    HANDLE spawn(LPTHREAD_START_ROUTINE fn);

    Options opt_{};
    SOCKET video_ = INVALID_SOCKET;
    HANDLE recvThread_ = nullptr;
    uint64_t writePos_ = 0;  // arena write position, owned by whichever recv thread is live
    std::atomic<bool> stop_{false};
    std::atomic<bool> sourceEnded_{false};
    std::vector<HANDLE> threads_;

    // Replay
    std::vector<uint8_t> file_;
    std::vector<size_t> cuts_;  // delivery boundaries
    size_t firstAud_ = 0, firstAudEnd_ = 0;  // the file's leading AUD, carried across a loop wrap
    double replayFps_ = 60;
    h264::Sps replaySps_{};
};

}  // namespace cf
