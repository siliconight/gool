// audio_engine/util/spsc_ring.h
//
// Single-producer / single-consumer ring buffer. Wait-free push and pop, no
// locks, no allocation after construction. Used for cross-thread event
// passing on every audio path that crosses a thread boundary.
//
// Capacity is fixed at construction. Storage is N+1 slots so head==tail
// distinguishes empty without a separate flag.
//
// T should be trivially copyable for the audio control path; non-trivial T
// is allowed but its copy/move must not allocate or block.

#ifndef AUDIO_ENGINE_UTIL_SPSC_RING_H
#define AUDIO_ENGINE_UTIL_SPSC_RING_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <utility>

namespace audio::util {

template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacity)
        : storage_(capacity + 1),
          slots_(capacity + 1) {}

    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    bool Push(const T& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % slots_;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;   // full
        }
        storage_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool Push(T&& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % slots_;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        storage_[head] = std::move(item);
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool Pop(T& out) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;   // empty
        }
        out = std::move(storage_[tail]);
        tail_.store((tail + 1) % slots_, std::memory_order_release);
        return true;
    }

    bool Empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    size_t Capacity() const noexcept { return slots_ - 1; }

    // Approximate size (changes during concurrent access).
    size_t SizeApprox() const noexcept {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        return (h + slots_ - t) % slots_;
    }

private:
    std::vector<T> storage_;
    size_t         slots_;

    // Cache-line padding to avoid false sharing between producer and consumer.
    // MSVC C4324 warns "structure was padded due to alignment specifier" —
    // which is exactly what alignas(64) is for. Suppressed here so /WX builds
    // don't trip on intentional padding.
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4324)
#endif
    alignas(64) std::atomic<size_t> head_{0};   // producer-owned
    alignas(64) std::atomic<size_t> tail_{0};   // consumer-owned
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif
};

} // namespace audio::util

#endif // AUDIO_ENGINE_UTIL_SPSC_RING_H
