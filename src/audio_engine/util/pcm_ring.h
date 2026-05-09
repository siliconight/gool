// audio_engine/util/pcm_ring.h
//
// Single-producer / single-consumer ring of int16 PCM samples. One per
// VoiceSource; producer is the control thread (running the codec), consumer
// is the audio render thread.
//
// Capacity is fixed at construction. Frames are interleaved across channels.
// Push and Pop operate in units of frames.

#ifndef AUDIO_ENGINE_UTIL_PCM_RING_H
#define AUDIO_ENGINE_UTIL_PCM_RING_H

#include <atomic>
#include <cstdint>
#include <vector>

namespace audio::util {

class PcmRing {
public:
    PcmRing(uint32_t capacityFrames, uint32_t channels)
        : channels_(channels),
          slots_((static_cast<size_t>(capacityFrames) + 1) * channels),
          storage_(slots_) {}

    PcmRing(const PcmRing&)            = delete;
    PcmRing& operator=(const PcmRing&) = delete;

    // Returns number of frames actually pushed (may be < frames if near full).
    uint32_t Push(const int16_t* data, uint32_t frames) noexcept {
        uint32_t pushed = 0;
        size_t   head   = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);

        for (uint32_t f = 0; f < frames; ++f) {
            const size_t next = (head + channels_) % slots_;
            if (next == tail) break;
            for (uint32_t c = 0; c < channels_; ++c) {
                storage_[head + c] = data[f * channels_ + c];
            }
            head = next;
            ++pushed;
        }
        head_.store(head, std::memory_order_release);
        return pushed;
    }

    // Returns number of frames actually popped.
    uint32_t Pop(int16_t* out, uint32_t frames) noexcept {
        uint32_t popped = 0;
        size_t   tail   = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);

        for (uint32_t f = 0; f < frames; ++f) {
            if (tail == head) break;
            for (uint32_t c = 0; c < channels_; ++c) {
                out[f * channels_ + c] = storage_[tail + c];
            }
            tail = (tail + channels_) % slots_;
            ++popped;
        }
        tail_.store(tail, std::memory_order_release);
        return popped;
    }

    uint32_t AvailableFrames() const noexcept {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        const size_t samples = (h + slots_ - t) % slots_;
        return static_cast<uint32_t>(samples / channels_);
    }

    uint32_t CapacityFrames() const noexcept {
        return static_cast<uint32_t>((slots_ - channels_) / channels_);
    }

    uint32_t Channels() const noexcept { return channels_; }

    // Reset state. Caller must guarantee neither producer nor consumer is
    // actively using the ring.
    void Reset() noexcept {
        head_.store(0, std::memory_order_release);
        tail_.store(0, std::memory_order_release);
    }

private:
    uint32_t channels_;
    size_t   slots_;
    std::vector<int16_t> storage_;

    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

} // namespace audio::util

#endif // AUDIO_ENGINE_UTIL_PCM_RING_H
