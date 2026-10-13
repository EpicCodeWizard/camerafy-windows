// cf_contention: does the camera stay as fast as a normal webcam while the PC is busy?
//
// Attaches to camerafy's DirectShow shared memory as a consumer (the same wait and copy code the
// DirectShow filter runs inside apps), then runs timed load phases while it measures every frame:
//
//   idle    nothing (reference)
//   cpu     a busy thread on every logical processor, normal priority (a game, a compile)
//   mem     DRAM bandwidth: big memcpy loops on half the logical processors
//   gpu     CUDA compute saturating every SM (short kernels, like a game or an ML job)
//   gpumem  VRAM bandwidth: back-to-back 512 MB device memsets/copies
//   pcie    PCIe bandwidth both ways (64 MB pinned copies), which competes with camerafy's readback
//   all     everything at once
//
// Per phase it prints, from the frames' own QPC stamps (same clock, no correction needed):
//   pipeline   camerafy: AU complete -> published
//   consumer   published -> this process has copied the frame
//   total      AU complete -> copied (what an app would see)
//   interval   time between consecutive copied frames (p99 shows stutter), and dropped frame ids
//
//   cf_contention [--seconds N] [--phases idle,cpu,mem,gpu,gpumem,pcie,all,idle] [--threads N]
//                 [--high]   load threads at HIGH priority class instead of normal
//
// Run camerafy.exe first (with the phone streaming, or --replay).
#include "ipc_reader.h"
#include "nvdec_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>

using namespace cf;

namespace {

enum Load : uint32_t { kCpu = 1, kMem = 2, kGpu = 4, kGpuMem = 8, kPcie = 16 };

struct Phase {
    std::string name;
    uint32_t loads;
};

std::atomic<uint32_t> g_loads{0};
std::atomic<bool> g_quit{false};
std::atomic<uint64_t> g_cpuWork{0}, g_memBytes{0}, g_gpuKernels{0}, g_gpuMemBytes{0}, g_pcieBytes{0};

// ---------------------------------------------------------------- CPU / memory load

void cpuLoad(uint32_t) {
    double x = 1.0001;
    uint64_t n = 0;
    while (!g_quit.load(std::memory_order_relaxed)) {
        if (!(g_loads.load(std::memory_order_relaxed) & kCpu)) {
            Sleep(5);
            continue;
        }
        for (int i = 0; i < 100000; ++i) x = sqrt(x * 1.0000001 + 0.5);
        n += 100000;
        if ((n & 0xFFFFF) < 100000) g_cpuWork.fetch_add(1, std::memory_order_relaxed);
    }
    if (x == 0) printf("%f", x);  // keep the loop alive
}

void memLoad(uint32_t) {
    const size_t bytes = 128u << 20;
    std::vector<uint8_t> a(bytes, 1), b(bytes, 2);
    while (!g_quit.load(std::memory_order_relaxed)) {
        if (!(g_loads.load(std::memory_order_relaxed) & kMem)) {
            Sleep(5);
            continue;
        }
        memcpy(b.data(), a.data(), bytes);
        memcpy(a.data(), b.data(), bytes);
        g_memBytes.fetch_add(2 * bytes, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------- GPU load (CUDA, own context)

// Each thread runs FMAs until %globaltimer passes start + ns, then stores (so nothing is
// optimized away). Kernels last ~8 ms: real compute load, always preemptible, never a TDR.
constexpr char kBurnPtx[] = R"PTX(
.version 7.0
.target sm_52
.address_size 64
.visible .entry cf_burn(.param .u64 out, .param .u64 ns)
{
    .reg .pred %p<2>;
    .reg .f32 %f<4>;
    .reg .b32 %r<5>;
    .reg .b64 %rd<8>;
    ld.param.u64 %rd1, [out];
    ld.param.u64 %rd2, [ns];
    mov.u64 %rd3, %globaltimer;
    add.u64 %rd4, %rd3, %rd2;
    mov.u32 %r1, %tid.x;
    cvt.rn.f32.u32 %f1, %r1;
    mov.f32 %f2, 0f3F800001;
    mov.f32 %f3, 0f3F7FFFFE;
$L_loop:
    fma.rn.f32 %f1, %f1, %f2, %f3;
    fma.rn.f32 %f1, %f1, %f3, %f2;
    fma.rn.f32 %f1, %f1, %f2, %f3;
    fma.rn.f32 %f1, %f1, %f3, %f2;
    fma.rn.f32 %f1, %f1, %f2, %f3;
    fma.rn.f32 %f1, %f1, %f3, %f2;
    fma.rn.f32 %f1, %f1, %f2, %f3;
    fma.rn.f32 %f1, %f1, %f3, %f2;
    fma.rn.f32 %f1, %f1, %f2, %f3;
    fma.rn.f32 %f1, %f1, %f3, %f2;
    fma.rn.f32 %f1, %f1, %f2, %f3;
    fma.rn.f32 %f1, %f1, %f3, %f2;
    fma.rn.f32 %f1, %f1, %f2, %f3;
    fma.rn.f32 %f1, %f1, %f3, %f2;
    fma.rn.f32 %f1, %f1, %f2, %f3;
    fma.rn.f32 %f1, %f1, %f3, %f2;
    mov.u64 %rd5, %globaltimer;
    setp.lt.u64 %p1, %rd5, %rd4;
    @%p1 bra $L_loop;
    mov.u32 %r2, %ctaid.x;
    mov.u32 %r3, %ntid.x;
    mad.lo.u32 %r4, %r2, %r3, %r1;
    mul.wide.u32 %rd6, %r4, 4;
    add.u64 %rd7, %rd1, %rd6;
    st.global.f32 [%rd7], %f1;
    ret;
}
)PTX";

tcuMemcpyHtoDAsync_v2* p_cuMemcpyHtoDAsync = nullptr;
tcuMemcpyDtoDAsync_v2* p_cuMemcpyDtoDAsync = nullptr;

bool gpuInit(CUcontext& ctx, std::string& err) {
    if (!g_cu.load(err)) return false;
    if (g_cu.cuInit(0) != CUDA_SUCCESS) {
        err = "cuInit failed";
        return false;
    }
    p_cuMemcpyHtoDAsync = (tcuMemcpyHtoDAsync_v2*)GetProcAddress(g_cu.cuda, "cuMemcpyHtoDAsync_v2");
    p_cuMemcpyDtoDAsync = (tcuMemcpyDtoDAsync_v2*)GetProcAddress(g_cu.cuda, "cuMemcpyDtoDAsync_v2");
    CUdevice dev = 0;
    if (g_cu.cuDeviceGet(&dev, 0) != CUDA_SUCCESS || g_cu.cuCtxCreate(&ctx, CU_CTX_SCHED_BLOCKING_SYNC, dev) != CUDA_SUCCESS) {
        err = "no CUDA context";
        return false;
    }
    CUcontext d;
    g_cu.cuCtxPopCurrent(&d);
    return true;
}

void gpuComputeLoad(CUcontext ctx) {
    g_cu.cuCtxSetCurrent(ctx);
    CUdevice dev = 0;
    g_cu.cuDeviceGet(&dev, 0);
    int sms = 0;
    g_cu.cuDeviceGetAttribute(&sms, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, dev);
    if (sms <= 0) sms = 40;
    CUmodule mod = nullptr;
    CUfunction fn = nullptr;
    if (g_cu.cuModuleLoadData(&mod, kBurnPtx) != CUDA_SUCCESS || g_cu.cuModuleGetFunction(&fn, mod, "cf_burn") != CUDA_SUCCESS) {
        fprintf(stderr, "GPU compute load: PTX JIT failed\n");
        return;
    }
    const unsigned blocks = (unsigned)sms * 8, threads = 256;
    CUdeviceptr out = 0;
    g_cu.cuMemAlloc(&out, (size_t)blocks * threads * 4);
    CUstream s = nullptr;
    g_cu.cuStreamCreate(&s, CU_STREAM_NON_BLOCKING);
    unsigned long long ns = 8ull * 1000 * 1000;
    void* args[] = {&out, &ns};
    while (!g_quit.load(std::memory_order_relaxed)) {
        if (!(g_loads.load(std::memory_order_relaxed) & kGpu)) {
            g_cu.cuStreamSynchronize(s);
            Sleep(5);
            continue;
        }
        g_cu.cuLaunchKernel(fn, blocks, 1, 1, threads, 1, 1, 0, s, args, nullptr);
        g_cu.cuLaunchKernel(fn, blocks, 1, 1, threads, 1, 1, 0, s, args, nullptr);
        g_cu.cuStreamSynchronize(s);
        g_gpuKernels.fetch_add(2, std::memory_order_relaxed);
    }
    g_cu.cuStreamSynchronize(s);
    g_cu.cuMemFree(out);
    g_cu.cuStreamDestroy(s);
    g_cu.cuModuleUnload(mod);
}

void gpuMemLoad(CUcontext ctx) {
    g_cu.cuCtxSetCurrent(ctx);
    const size_t bytes = 512u << 20;
    CUdeviceptr a = 0, b = 0;
    if (g_cu.cuMemAlloc(&a, bytes) != CUDA_SUCCESS || g_cu.cuMemAlloc(&b, bytes) != CUDA_SUCCESS) {
        fprintf(stderr, "GPU memory load: cannot allocate 1 GB of VRAM\n");
        return;
    }
    CUstream s = nullptr;
    g_cu.cuStreamCreate(&s, CU_STREAM_NON_BLOCKING);
    uint8_t v = 0;
    while (!g_quit.load(std::memory_order_relaxed)) {
        if (!(g_loads.load(std::memory_order_relaxed) & kGpuMem)) {
            g_cu.cuStreamSynchronize(s);
            Sleep(5);
            continue;
        }
        g_cu.cuMemsetD8Async(a, v++, bytes, s);
        if (p_cuMemcpyDtoDAsync) p_cuMemcpyDtoDAsync(b, a, bytes, s);
        g_cu.cuStreamSynchronize(s);
        g_gpuMemBytes.fetch_add(p_cuMemcpyDtoDAsync ? 3 * bytes : bytes, std::memory_order_relaxed);
    }
    g_cu.cuStreamSynchronize(s);
    g_cu.cuMemFree(a);
    g_cu.cuMemFree(b);
    g_cu.cuStreamDestroy(s);
}

void pcieLoad(CUcontext ctx) {
    g_cu.cuCtxSetCurrent(ctx);
    const size_t bytes = 64u << 20;
    CUdeviceptr d1 = 0, d2 = 0;
    void *h1 = nullptr, *h2 = nullptr;
    if (g_cu.cuMemAlloc(&d1, bytes) != CUDA_SUCCESS || g_cu.cuMemAlloc(&d2, bytes) != CUDA_SUCCESS ||
        g_cu.cuMemHostAlloc(&h1, bytes, 0) != CUDA_SUCCESS || g_cu.cuMemHostAlloc(&h2, bytes, 0) != CUDA_SUCCESS) {
        fprintf(stderr, "PCIe load: allocation failed\n");
        return;
    }
    CUstream up = nullptr, down = nullptr;
    g_cu.cuStreamCreate(&up, CU_STREAM_NON_BLOCKING);
    g_cu.cuStreamCreate(&down, CU_STREAM_NON_BLOCKING);
    while (!g_quit.load(std::memory_order_relaxed)) {
        if (!(g_loads.load(std::memory_order_relaxed) & kPcie)) {
            g_cu.cuStreamSynchronize(up);
            g_cu.cuStreamSynchronize(down);
            Sleep(5);
            continue;
        }
        g_cu.cuMemcpyDtoHAsync(h1, d1, bytes, down);
        if (p_cuMemcpyHtoDAsync) p_cuMemcpyHtoDAsync(d2, h2, bytes, up);
        g_cu.cuStreamSynchronize(down);
        g_cu.cuStreamSynchronize(up);
        g_pcieBytes.fetch_add(p_cuMemcpyHtoDAsync ? 2 * bytes : bytes, std::memory_order_relaxed);
    }
    g_cu.cuStreamSynchronize(up);
    g_cu.cuStreamSynchronize(down);
    g_cu.cuMemFree(d1);
    g_cu.cuMemFree(d2);
    g_cu.cuMemFreeHost(h1);
    g_cu.cuMemFreeHost(h2);
    g_cu.cuStreamDestroy(up);
    g_cu.cuStreamDestroy(down);
}

// ---------------------------------------------------------------- measurement

struct Stats {
    std::vector<double> pipeline, consumer, total, interval;
    uint64_t frames = 0, gaps = 0;
    void clear() { *this = Stats{}; }
};

double pct(std::vector<double> v, double p) {
    if (v.empty()) return NAN;
    const size_t k = std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

BOOL WINAPI onCtrl(DWORD) {
    g_quit = true;
    return TRUE;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    double seconds = 15;
    std::wstring phaseList = L"idle,cpu,mem,gpu,gpumem,pcie,all,idle";
    uint32_t threads = 0;
    bool high = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--seconds") && i + 1 < argc) seconds = _wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"--phases") && i + 1 < argc) phaseList = argv[++i];
        else if (!wcscmp(argv[i], L"--threads") && i + 1 < argc) threads = (uint32_t)_wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--high")) high = true;
        else {
            fwprintf(stderr, L"usage: cf_contention [--seconds N] [--phases idle,cpu,mem,gpu,gpumem,pcie,all,idle] "
                             L"[--threads N] [--high]\n");
            return 2;
        }
    }
    std::vector<Phase> phases;
    for (size_t pos = 0; pos <= phaseList.size();) {
        size_t c = phaseList.find(L',', pos);
        if (c == std::wstring::npos) c = phaseList.size();
        std::wstring n = phaseList.substr(pos, c - pos);
        pos = c + 1;
        if (n.empty()) continue;
        uint32_t l = n == L"idle" ? 0 : n == L"cpu" ? kCpu : n == L"mem" ? kMem : n == L"gpu" ? kGpu
                   : n == L"gpumem" ? kGpuMem : n == L"pcie" ? kPcie : n == L"all" ? (kCpu | kMem | kGpu | kGpuMem | kPcie)
                   : 0xFFFFFFFFu;
        if (l == 0xFFFFFFFFu) {
            fwprintf(stderr, L"unknown phase %s\n", n.c_str());
            return 2;
        }
        std::string name;
        for (wchar_t ch : n) name.push_back((char)ch);  // phase names are ASCII
        phases.push_back({name, l});
    }
    SetConsoleCtrlHandler(onCtrl, TRUE);

    // Attach like the DirectShow filter does.
    HANDLE sec = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, ipc::kDsSectionName);
    if (!sec) {
        fwprintf(stderr, L"cannot open %s (is camerafy.exe running?)\n", ipc::kDsSectionName);
        return 1;
    }
    void* base = MapViewOfFile(sec, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (!base || !ipc::validRegion(base)) {
        fwprintf(stderr, L"shared memory has a different layout (camerafy.exe from another build?)\n");
        return 1;
    }
    auto* h = (ipc::RegionHeader*)base;
    const int idx = ipc::claimReader(h, 3);
    if (idx < 0) {
        fwprintf(stderr, L"no free reader slot\n");
        return 1;
    }
    wchar_t evName[96];
    swprintf(evName, 96, ipc::kDsEventFmt, (unsigned)idx);
    HANDLE evt = OpenEventW(SYNCHRONIZE, FALSE, evName);
    if (!evt) {
        fwprintf(stderr, L"cannot open the reader event\n");
        return 1;
    }
    // A camera consumer in an app: real-time band, exactly like the DirectShow filter's streaming thread.
    ipc::raiseCameraThread();
    ipc::ParallelCopier copier;
    copier.start(0);
    std::vector<uint8_t> buf((size_t)ipc::kPayloadCapacity);

    // Load threads.
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uint32_t ncpu = threads ? threads : si.dwNumberOfProcessors;
    if (high) SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    std::vector<std::thread> pool;
    for (uint32_t i = 0; i < ncpu; ++i) pool.emplace_back(cpuLoad, i);
    for (uint32_t i = 0; i < std::max(1u, ncpu / 2); ++i) pool.emplace_back(memLoad, i);
    CUcontext ctx = nullptr;
    std::string err;
    const bool gpuOk = gpuInit(ctx, err);
    if (gpuOk) {
        pool.emplace_back(gpuComputeLoad, ctx);
        pool.emplace_back(gpuMemLoad, ctx);
        pool.emplace_back(pcieLoad, ctx);
    } else {
        fprintf(stderr, "GPU loads disabled: %s\n", err.c_str());
    }
    // The load threads run at normal priority (inside a HIGH class only with --high), like the
    // apps that would compete with a webcam; this thread (the consumer) stays above them.
    for (auto& t : pool) SetThreadPriority(t.native_handle(), THREAD_PRIORITY_NORMAL);

    printf("cf_contention: %u CPU load threads, %u memory threads, GPU loads %s, %.0f s per phase, consumer reader %d\n",
           ncpu, std::max(1u, ncpu / 2), gpuOk ? "on" : "off", seconds, idx);
    printf("%-7s %5s %5s | %-21s | %-21s | %-21s | %-17s | load achieved\n", "phase", "fps", "drops",
           "pipeline p50/p95/p99", "consumer p50/p95/p99", "total p50/p95/p99", "interval p50/p99");

    uint64_t last = h->publishCount.load(std::memory_order_acquire);
    uint64_t lastFrameId = 0, lastCopiedQpc = 0, retries = 0;
    for (const Phase& ph : phases) {
        if (g_quit) break;
        g_loads.store(ph.loads);
        Stats st;
        const uint64_t cpu0 = g_cpuWork, mem0 = g_memBytes, gk0 = g_gpuKernels, gm0 = g_gpuMemBytes, pc0 = g_pcieBytes;
        // The first 1.5 s let the load ramp up (clocks, queues); frames are consumed but not
        // counted, as an app keeps reading throughout.
        const uint64_t rampEnd = qpc() + (uint64_t)usToTicks(1.5e6);
        const uint64_t t0 = rampEnd, end = t0 + (uint64_t)usToTicks(seconds * 1e6);
        lastCopiedQpc = 0;
        while (qpc() < end && !g_quit) {
            const auto r = ipc::waitForFrame(h, idx, last, evt, 200, [&] { copier.prepare(); });
            if (r != ipc::WaitResult::Frame) {
                ipc::heartbeat(h, idx);
                continue;
            }
            last = h->publishCount.load(std::memory_order_acquire);
            ipc::FrameInfo fi{};
            const bool ok = ipc::readLatest(h, base, fi,
                [&](const uint8_t* src, const ipc::SlotHeader& s) {
                    copier.copyNv12(src, s, buf.data(), s.width, buf.data() + (size_t)s.width * s.height, s.width,
                                    s.width, s.height);
                },
                &retries);
            if (!ok) continue;
            const uint64_t now = qpc();
            const uint64_t auQpc = fi.winAuCompleteQpc, pubQpc = fi.winPublishedQpc;
            ipc::recordConsumed(h, idx, fi, retries);
            if (now < rampEnd) {
                lastFrameId = fi.frameId;
                continue;
            }
            ++st.frames;
            if (lastFrameId && fi.frameId > lastFrameId + 1) st.gaps += fi.frameId - lastFrameId - 1;
            lastFrameId = fi.frameId;
            if (auQpc && pubQpc >= auQpc) st.pipeline.push_back(ticksToUs((int64_t)(pubQpc - auQpc)));
            if (pubQpc && now >= pubQpc) st.consumer.push_back(ticksToUs((int64_t)(now - pubQpc)));
            if (auQpc && now >= auQpc) st.total.push_back(ticksToUs((int64_t)(now - auQpc)));
            if (lastCopiedQpc) st.interval.push_back(ticksToUs((int64_t)(now - lastCopiedQpc)) / 1000.0);
            lastCopiedQpc = now;
        }
        const double secs = ticksToUs((int64_t)(qpc() - t0)) / 1e6;
        const double loadSecs = secs + 1.5;  // load counters run from the phase start, ramp included
        char load[160] = "";
        int n = 0;
        if (ph.loads & kCpu) n += snprintf(load + n, sizeof(load) - n, "cpu %.0f Mops/s ", (g_cpuWork - cpu0) * 1.048576 / loadSecs);
        if (ph.loads & kMem) n += snprintf(load + n, sizeof(load) - n, "dram %.1f GB/s ", (g_memBytes - mem0) / loadSecs / 1e9);
        if (ph.loads & kGpu) n += snprintf(load + n, sizeof(load) - n, "gpu %.0f kernels/s ", (g_gpuKernels - gk0) / loadSecs);
        if (ph.loads & kGpuMem) n += snprintf(load + n, sizeof(load) - n, "vram %.0f GB/s ", (g_gpuMemBytes - gm0) / loadSecs / 1e9);
        if (ph.loads & kPcie) n += snprintf(load + n, sizeof(load) - n, "pcie %.1f GB/s ", (g_pcieBytes - pc0) / loadSecs / 1e9);
        if (!ph.loads) snprintf(load, sizeof(load), "-");
        printf("%-7s %5.1f %5llu | %6.0f %6.0f %7.0f | %6.0f %6.0f %7.0f | %6.0f %6.0f %7.0f | %6.1f %8.1f ms | %s\n",
               ph.name.c_str(), st.frames / secs, (unsigned long long)st.gaps, pct(st.pipeline, 0.5), pct(st.pipeline, 0.95),
               pct(st.pipeline, 0.99), pct(st.consumer, 0.5), pct(st.consumer, 0.95), pct(st.consumer, 0.99),
               pct(st.total, 0.5), pct(st.total, 0.95), pct(st.total, 0.99), pct(st.interval, 0.5), pct(st.interval, 0.99),
               load);
        fflush(stdout);
    }
    printf("(latencies in microseconds; pipeline = camerafy, consumer = this app's wake + copy; total = what an app sees.\n"
           " The phone, USB and the app's own rendering are outside these numbers.)\n");

    g_loads.store(0);
    g_quit = true;
    for (auto& t : pool) t.join();
    ipc::releaseReader(h, idx);
    if (ctx) g_cu.cuCtxDestroy(ctx);
    return 0;
}
