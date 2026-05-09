// audio_engine/decoders/resampling_decoder.h
//
// Wraps an IAudioDecoder running at one sample rate and presents the same
// interface at a different (target) sample rate, doing linear interpolation
// on the fly. Pass-through when source and target rates match.
//
// This is intentionally minimal; linear interp is below studio-grade for
// large rate ratios but adequate for game-quality 44100<->48000 conversion
// of voice/SFX. A windowed-sinc replacement can drop in behind the same
// interface later.
//
// Channel count is preserved; downmix happens upstream.

#ifndef AUDIO_ENGINE_DECODERS_RESAMPLING_DECODER_H
#define AUDIO_ENGINE_DECODERS_RESAMPLING_DECODER_H

#include "audio_engine/decoders/audio_decoder.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace audio {

class ResamplingDecoder final : public IAudioDecoder {
public:
    ResamplingDecoder(std::unique_ptr<IAudioDecoder> source,
                       uint32_t                       targetSampleRate);

    uint32_t SampleRate()  const noexcept override { return targetSampleRate_; }
    uint32_t Channels()    const noexcept override { return channels_; }
    uint64_t TotalFrames() const noexcept override;

    uint32_t DecodeFrames(float* out, uint32_t frames) noexcept override;

    bool Seek(uint64_t frame) noexcept override;

    // For tests / diagnostics.
    bool IsPassthrough() const noexcept { return passthrough_; }

private:
    // Pull one frame of source PCM into prev_/curr_, advancing the cursor.
    // Returns false at source EOF.
    bool AdvanceSourceFrame() noexcept;

    std::unique_ptr<IAudioDecoder> source_;
    uint32_t channels_         = 0;
    uint32_t sourceSampleRate_ = 0;
    uint32_t targetSampleRate_ = 0;
    bool     passthrough_      = false;

    // Linear-interp state.
    double             phase_ = 0.0;     // [0, 1) within the current source segment
    double             ratio_ = 1.0;     // sourceRate / targetRate (frames-per-output-frame)
    std::vector<float> prev_;            // last source frame, per channel
    std::vector<float> curr_;            // next source frame, per channel
    bool               primed_ = false;  // have we pulled the first two frames?
    bool               sourceEof_ = false;

    // Single-frame scratch for AdvanceSourceFrame.
    std::vector<float> scratch_;
};

} // namespace audio

#endif // AUDIO_ENGINE_DECODERS_RESAMPLING_DECODER_H
