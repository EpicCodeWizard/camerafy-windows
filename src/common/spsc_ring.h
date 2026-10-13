#pragma once
// Fixed-capacity single-producer single-consumer ring. Drops on overflow.
// Each side caches the other's index so the common case touches only its own line.
#include <atomic>
#include <stdint.h>
#include <type_traits>
#include <intrin.h>
#include <windows.h>  // WaitOnAddress (link Synchronization.lib)

namespace cf {

inline constexpr size_t kCacheLine = 64;  // hardware_destructive_interference_size on x64

template <typename T, uint32_t CapacityPow2>
class SpscRing final {
    static_assert((CapacityPow2 & (CapacityPow2 - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>);
    static constexpr uint32_t kMask = CapacityPow2 - 1;

public:
    // Producer side.
    bool push(const T& v) {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        if (h - tailCache_ > kMask) {
            tailCache_ = tail_.load(std::memory_order_acquire);
            if (h - tailCache_ > kMask) return false;
        }
        slots_[h & kMask] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Consumer side.
    bool pop(T& out) {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == headCache_) {
            headCache_ = head_.load(std::memory_order_acquire);
            if (t == headCache_) return false;
        }
        out = slots_[t & kMask];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Consumer side: spin until an element arrives or *stop becomes true.
    bool popSpin(T& out, const std::atomic<bool>& stop) {
        while (!pop(out)) {
            if (stop.load(std::memory_order_relaxed)) return false;
            _mm_pause();
        }
        return true;
    }

    // Blocking variant (--block): the consumer parks on the head index with WaitOnAddress
    // (same-process only, which these rings are) and the producer wakes it after a push.
    // Spinning is the default; this exists so the two can be measured against each other.
    bool popBlocking(T& out, const std::atomic<bool>& stop, uint32_t spinIters = 2000) {
        for (uint32_t i = 0; i < spinIters; ++i) {
            if (pop(out)) return true;
            _mm_pause();
        }
        while (!pop(out)) {
            if (stop.load(std::memory_order_relaxed)) return false;
            uint32_t seen = headCache_;
            WaitOnAddress(&head_, &seen, sizeof(seen), 10);
        }
        return true;
    }
    void notify() { WakeByAddressSingle(&head_); }

    bool emptyApprox() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    alignas(kCacheLine) std::atomic<uint32_t> head_{0};
    uint32_t tailCache_ = 0;  // producer-owned
    alignas(kCacheLine) std::atomic<uint32_t> tail_{0};
    uint32_t headCache_ = 0;  // consumer-owned
    alignas(kCacheLine) T slots_[CapacityPow2];
};

}  // namespace cf
