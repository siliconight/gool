// audio_engine/decoders/resampling_decoder.cpp

#include "audio_engine/decoders/resampling_decoder.h"

#include <algorithm>
#include <cstring>

namespace audio {

ResamplingDecoder::ResamplingDecoder(std::unique_ptr<IAudioDecoder> source,
                                       uint32_t                       targetSampleRate)
    : source_(std::move(source)),
      targetSampleRate_(targetSampleRate)
{
    if (source_) {
        channels_         = source_->Channels();
        sourceSampleRate_ = source_->SampleRate();
    }
    passthrough_ = (sourceSampleRate_ == targetSampleRate_);
    if (sourceSampleRate_ > 0 && targetSampleRate_ > 0) {
        ratio_ = static_cast<double>(sourceSampleRate_)
               / static_cast<double>(targetSampleRate_);
    }
    prev_.assign(channels_, 0.0f);
    curr_.assign(channels_, 0.0f);
    scratch_.assign(channels_, 0.0f);
}

uint64_t ResamplingDecoder::TotalFrames() const noexcept {
    if (!source_) return 0;
    const uint64_t srcFrames = source_->TotalFrames();
    if (srcFrames == 0)            return 0;     // unknown
    if (sourceSampleRate_ == 0)    return 0;
    if (passthrough_)              return srcFrames;
    // Target frames ≈ srcFrames * targetRate / sourceRate.
    return static_cast<uint64_t>(
        (static_cast<double>(srcFrames) * targetSampleRate_) / sourceSampleRate_);
}

bool ResamplingDecoder::AdvanceSourceFrame() noexcept {
    if (!source_ || sourceEof_) return false;
    const uint32_t got = source_->DecodeFrames(scratch_.data(), 1);
    if (got == 0) { sourceEof_ = true; return false; }
    return true;
}

uint32_t ResamplingDecoder::DecodeFrames(float* out, uint32_t frames) noexcept {
    if (!source_ || channels_ == 0 || frames == 0) return 0;

    if (passthrough_) {
        return source_->DecodeFrames(out, frames);
    }

    // Prime the interpolation window: load the first two source frames.
    if (!primed_) {
        if (!AdvanceSourceFrame()) return 0;
        for (uint32_t c = 0; c < channels_; ++c) prev_[c] = scratch_[c];
        if (AdvanceSourceFrame()) {
            for (uint32_t c = 0; c < channels_; ++c) curr_[c] = scratch_[c];
        } else {
            // Single source frame total; emit it once and stop.
            for (uint32_t c = 0; c < channels_; ++c) curr_[c] = prev_[c];
        }
        primed_ = true;
        phase_  = 0.0;
    }

    uint32_t produced = 0;
    while (produced < frames) {
        // Walk forward whole source segments while phase >= 1.
        while (phase_ >= 1.0) {
            for (uint32_t c = 0; c < channels_; ++c) prev_[c] = curr_[c];
            if (!AdvanceSourceFrame()) {
                // Drained: hold the last sample to fill the tail and stop.
                if (produced == 0) return 0;
                return produced;
            }
            for (uint32_t c = 0; c < channels_; ++c) curr_[c] = scratch_[c];
            phase_ -= 1.0;
        }

        const float t = static_cast<float>(phase_);
        for (uint32_t c = 0; c < channels_; ++c) {
            out[produced * channels_ + c] = prev_[c] + (curr_[c] - prev_[c]) * t;
        }
        ++produced;
        phase_ += ratio_;
    }
    return produced;
}

bool ResamplingDecoder::Seek(uint64_t frame) noexcept {
    if (!source_) return false;
    // Convert target-frame to source-frame.
    const uint64_t srcFrame = passthrough_
        ? frame
        : static_cast<uint64_t>(static_cast<double>(frame) * ratio_);
    if (!source_->Seek(srcFrame)) return false;
    primed_    = false;
    sourceEof_ = false;
    phase_     = 0.0;
    return true;
}

} // namespace audio
