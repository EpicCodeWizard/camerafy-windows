#pragma once
// Hand-written PTX (no CUDA toolkit needed: the driver JIT-compiles it once at startup).

namespace cf::ptx {

// cf_idle(u64 durationNs, u32 sleepNs): one warp that sleeps in `nanosleep` steps until
// %globaltimer passes start + durationNs. Resident on the GPU, it makes the driver's idle
// heuristic see a busy GPU, so clocks and the PCIe link stay up, while it uses one SM
// sub-partition at near-zero power and no CPU at all (it is relaunched every 50 ms, and a
// bounded kernel can always be preempted, so it can never trip the TDR watchdog).
// nanosleep needs sm_70 (Volta) or newer; older GPUs fall back to the memset keepalive.
inline constexpr char kIdleKernel[] = R"PTX(
.version 7.0
.target sm_70
.address_size 64

.visible .entry cf_idle(
    .param .u64 cf_idle_duration_ns,
    .param .u32 cf_idle_sleep_ns
)
{
    .reg .pred %p<3>;
    .reg .b32 %r<4>;
    .reg .b64 %rd<5>;

    ld.param.u64 %rd1, [cf_idle_duration_ns];
    ld.param.u32 %r1, [cf_idle_sleep_ns];
    mov.u64 %rd2, %globaltimer;
    add.u64 %rd3, %rd2, %rd1;
    // Hard cap on iterations (nanosleep may return early, never much later): the kernel ends
    // even if %globaltimer stalls, so shutdown can never wait on it forever.
    mov.u32 %r2, 0;
    mov.u32 %r3, 200000;
$L_loop:
    nanosleep.u32 %r1;
    add.u32 %r2, %r2, 1;
    setp.ge.u32 %p2, %r2, %r3;
    @%p2 bra $L_done;
    mov.u64 %rd4, %globaltimer;
    setp.lt.u64 %p1, %rd4, %rd3;
    @%p1 bra $L_loop;
$L_done:
    ret;
}
)PTX";

}  // namespace cf::ptx
