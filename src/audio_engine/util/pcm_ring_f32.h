// audio_engine/util/pcm_ring_f32.h
//
// Single-producer / single-consumer ring of interleaved float32 PCM frames.
// Twin of PcmRing<int16_t>; kept structurally separate so the int16 voice
// path stays untouched.
//
// One ring per active streaming-sound voice. Producer is the control
// thread (streaming pump in AudioRuntimeImpl::Update()); consumer is the
// audio render thread.

#ifndef AUDIO_ENGINE_UTIL_PCM_RING_F32_H
#define AUDIO_ENGINE_UTIL_PCM_RING_F32_H

#include <atomic>
#include <cstdint>
#include <vector>

namespace audio::util {

class PcmRingF32 {
public:
    PcmRingF32(uint32_t capacityFrames, uint32_t channels)
        : channels_(channels),
          slots_((static_cast<size_t>(capacityFrames) + 1) * channels),
          storage_(slots_) {}

    PcmRingF32(const PcmRingF32&)            = delete;
    PcmRingF32& operator=(const PcmRingF32&) = delete;

    uint32_t Push(const float* data, uint32_t frames) noexcept {
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

    uint32_t Pop(float* out, uint32_t frames) noexcept {
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

    uint32_t FreeFrames() const noexcept {
        return CapacityFrames() - AvailableFrames();
    }

    uint32_t Channels() const noexcept { return channels_; }

    void Reset() noexcept {
        head_.store(0, std::memory_order_release);
        tail_.store(0, std::memory_order_release);
    }

private:
    uint32_t channels_;
    size_t   slots_;
    std::vector<float> storage_;

    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

} // namespace audio::util

#endif // AUDIO_ENGINE_UTIL_PCM_RING_F32_H
