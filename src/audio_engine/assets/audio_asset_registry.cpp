// audio_engine/assets/audio_asset_registry.cpp

#include "audio_engine/assets/audio_asset_registry.h"

#include "audio_engine/decoders/decoder_factory.h"
#include "audio_engine/decoders/memory_pcm_decoder.h"
#include "audio_engine/decoders/resampling_decoder.h"

#include <algorithm>
#include <vector>

namespace audio {

namespace {

// Like MemoryPcmDecoder but owns its buffer. Used for the streaming-from-
// memory path so the registry can hand ownership of decoded PCM into the
// StreamingAsset and not require the caller to keep the buffer alive.
class OwningMemoryPcmDecoder final : public IAudioDecoder {
public:
    OwningMemoryPcmDecoder(std::vector<float>&& samples,
                             uint32_t             sampleRate,
                             uint32_t             channels) noexcept
        : samples_(std::move(samples)),
          frames_(channels == 0 ? 0 : samples_.size() / channels),
          sampleRate_(sampleRate),
          channels_(channels),
          cursor_(0) {}

    uint32_t SampleRate()  const noexcept override { return sampleRate_; }
    uint32_t Channels()    const noexcept override { return channels_; }
    uint64_t TotalFrames() const noexcept override { return frames_; }

    uint32_t DecodeFrames(float* out, uint32_t frames) noexcept override {
        if (channels_ == 0 || cursor_ >= frames_) return 0;
        const uint64_t remaining = frames_ - cursor_;
        const uint32_t take      = static_cast<uint32_t>(
            (remaining < frames) ? remaining : frames);
        const uint32_t total     = take * channels_;
        const float*   src       = samples_.data() + cursor_ * channels_;
        for (uint32_t i = 0; i < total; ++i) out[i] = src[i];
        cursor_ += take;
        return take;
    }

    bool Seek(uint64_t frame) noexcept override {
        if (frame > frames_) return false;
        cursor_ = frame;
        return true;
    }

private:
    std::vector<float> samples_;
    uint64_t           frames_;
    uint32_t           sampleRate_;
    uint32_t           channels_;
    uint64_t           cursor_;
};

// Decode an entire stream into an interleaved float vector at the source's
// channel count. Returns false on decode error.
bool DecodeFully(IAudioDecoder& dec, std::vector<float>& out) {
    const uint32_t channels = dec.Channels();
    if (channels == 0) return false;
    constexpr uint32_t kChunk = 4096;
    std::vector<float> buf(static_cast<size_t>(kChunk) * channels);
    out.clear();
    const uint64_t total = dec.TotalFrames();
    if (total > 0) out.reserve(static_cast<size_t>(total) * channels);
    while (true) {
        const uint32_t got = dec.DecodeFrames(buf.data(), kChunk);
        if (got == 0) break;
        out.insert(out.end(),
                    buf.begin(),
                    buf.begin() + static_cast<size_t>(got) * channels);
        if (got < kChunk) break;
    }
    // v0.66.0: zero-frame decode is a failure. Previously returned
    // true with an empty out vector — the registry would then store
    // an empty PcmAsset, the voice would tick producing silence,
    // and no diagnostic would fire. (Multi-hour debug session in
    // the sandbox v0.64.x work uncovered this — voices were
    // "playing" sounds whose decoders produced no samples.) Now
    // propagates as DecodeError to RegisterDecodedFromMemory's
    // caller, which surfaces a push_error at the GDScript binding
    // boundary. Catches corrupted files, format-detector
    // mismatches, and decoder library failures that produce no
    // output but no exception.
    return !out.empty();
}

// Downmix interleaved 'channels'-channel data to 'maxChannels'.
void DownmixInPlace(std::vector<float>& samples,
                     uint32_t&            channels,
                     uint32_t             maxChannels) {
    if (channels == 0 || channels <= maxChannels) return;
    const uint64_t frames = samples.size() / channels;
    std::vector<float> out(static_cast<size_t>(frames) * maxChannels);
    if (maxChannels == 2) {
        // Multichannel→stereo: take ch0/ch1 directly. Center/LFE/surrounds
        // dropped. Adequate for game use; revisit with ITU-R BS.775
        // coefficients if needed.
        for (uint64_t f = 0; f < frames; ++f) {
            out[f * 2 + 0] = samples[f * channels + 0];
            out[f * 2 + 1] = samples[f * channels + 1];
        }
    } else {
        // Multichannel→mono: equal-weight average.
        const float inv = 1.0f / static_cast<float>(channels);
        for (uint64_t f = 0; f < frames; ++f) {
            float acc = 0.0f;
            for (uint32_t c = 0; c < channels; ++c) acc += samples[f * channels + c];
            out[f] = acc * inv;
        }
    }
    samples  = std::move(out);
    channels = maxChannels;
}

std::unique_ptr<IAudioDecoder> MaybeResample(std::unique_ptr<IAudioDecoder> dec,
                                                uint32_t target) {
    if (!dec) return dec;
    if (dec->SampleRate() == target) return dec;
    return std::make_unique<ResamplingDecoder>(std::move(dec), target);
}

} // namespace

AudioAssetRegistry::AudioAssetRegistry(uint32_t maxRegisteredSounds)
    : capacity_(maxRegisteredSounds) {
    defs_.reserve(maxRegisteredSounds);
    pcms_.reserve(maxRegisteredSounds);
    streams_.reserve(maxRegisteredSounds);
}

AudioResult AudioAssetRegistry::RegisterDefinition(const SoundDefinition& def) {
    if (def.soundId == kInvalidSoundId) return AudioResult::InvalidArgument;
    if (defs_.size() >= capacity_ && !defs_.contains(def.soundId)) {
        return AudioResult::BudgetExceeded;
    }
    defs_[def.soundId] = def;
    return AudioResult::Success;
}

AudioResult AudioAssetRegistry::RegisterPcm(AudioSoundId id,
                                              std::span<const float> samples,
                                              uint32_t sampleRate,
                                              uint32_t channels) {
    if (id == kInvalidSoundId)         return AudioResult::InvalidArgument;
    if (channels == 0 || channels > 2) return AudioResult::InvalidArgument;
    if (samples.empty())               return AudioResult::InvalidArgument;
    if (samples.size() % channels)     return AudioResult::InvalidArgument;

    if (pcms_.size() >= capacity_ && !pcms_.contains(id)) {
        return AudioResult::BudgetExceeded;
    }

    PcmAsset asset;
    asset.sampleRate = sampleRate;
    asset.channels   = channels;
    asset.frames     = static_cast<uint32_t>(samples.size() / channels);
    asset.samples.assign(samples.begin(), samples.end());

    pcms_[id] = std::move(asset);
    return AudioResult::Success;
}

AudioResult AudioAssetRegistry::RegisterDecodedFromFile(AudioSoundId id,
                                                           const char*  path,
                                                           uint32_t     engineSampleRate,
                                                           uint32_t     maxChannels) {
    if (id == kInvalidSoundId || !path)                   return AudioResult::InvalidArgument;
    if (maxChannels == 0 || maxChannels > 2)              return AudioResult::InvalidArgument;
    if (pcms_.size() >= capacity_ && !pcms_.contains(id)) return AudioResult::BudgetExceeded;

    auto raw = DecoderFactory::CreateForFile(path);
    if (!raw) return AudioResult::IoError;

    auto dec = MaybeResample(std::move(raw), engineSampleRate);
    if (!dec) return AudioResult::DecodeError;

    PcmAsset asset;
    asset.sampleRate = dec->SampleRate();
    asset.channels   = dec->Channels();
    if (!DecodeFully(*dec, asset.samples)) return AudioResult::DecodeError;
    DownmixInPlace(asset.samples, asset.channels, maxChannels);
    asset.frames = static_cast<uint32_t>(
        asset.channels > 0 ? asset.samples.size() / asset.channels : 0);

    pcms_[id] = std::move(asset);
    return AudioResult::Success;
}

AudioResult AudioAssetRegistry::RegisterDecodedFromMemory(AudioSoundId             id,
                                                            std::span<const uint8_t> bytes,
                                                            AudioFileFormat          formatHint,
                                                            uint32_t                 engineSampleRate,
                                                            uint32_t                 maxChannels) {
    if (id == kInvalidSoundId || bytes.empty())           return AudioResult::InvalidArgument;
    if (maxChannels == 0 || maxChannels > 2)              return AudioResult::InvalidArgument;
    if (pcms_.size() >= capacity_ && !pcms_.contains(id)) return AudioResult::BudgetExceeded;

    auto raw = DecoderFactory::CreateForMemory(bytes.data(), bytes.size(), formatHint);
    if (!raw) return AudioResult::DecodeError;

    auto dec = MaybeResample(std::move(raw), engineSampleRate);
    if (!dec) return AudioResult::DecodeError;

    PcmAsset asset;
    asset.sampleRate = dec->SampleRate();
    asset.channels   = dec->Channels();
    if (!DecodeFully(*dec, asset.samples)) return AudioResult::DecodeError;
    DownmixInPlace(asset.samples, asset.channels, maxChannels);
    asset.frames = static_cast<uint32_t>(
        asset.channels > 0 ? asset.samples.size() / asset.channels : 0);

    pcms_[id] = std::move(asset);
    return AudioResult::Success;
}

AudioResult AudioAssetRegistry::RegisterStreamingFromFile(AudioSoundId id,
                                                            const char*  path,
                                                            uint32_t     engineSampleRate,
                                                            uint32_t     maxChannels,
                                                            uint32_t     ringFrames) {
    if (id == kInvalidSoundId || !path)                         return AudioResult::InvalidArgument;
    if (maxChannels == 0 || maxChannels > 2)                    return AudioResult::InvalidArgument;
    if (ringFrames == 0)                                         return AudioResult::InvalidArgument;
    if (streams_.size() >= capacity_ && !streams_.contains(id)) return AudioResult::BudgetExceeded;

    auto raw = DecoderFactory::CreateForFile(path);
    if (!raw) return AudioResult::IoError;

    auto dec = MaybeResample(std::move(raw), engineSampleRate);
    if (!dec) return AudioResult::DecodeError;

    const uint32_t srcChannels = dec->Channels();
    const uint32_t outChannels = std::min(srcChannels, maxChannels);
    if (outChannels == 0 || outChannels > 2) return AudioResult::DecodeError;

    StreamingAsset asset;
    asset.decoder     = std::move(dec);
    asset.channels    = outChannels;
    asset.sampleRate  = engineSampleRate;
    asset.totalFrames = asset.decoder->TotalFrames();
    asset.ring        = std::make_unique<util::PcmRingF32>(ringFrames, outChannels);
    asset.state       = StreamingAsset::State::Idle;

    streams_[id] = std::move(asset);
    return AudioResult::Success;
}

AudioResult AudioAssetRegistry::RegisterStreamingFromMemory(AudioSoundId         id,
                                                              std::vector<float>&& samples,
                                                              uint32_t             srcSampleRate,
                                                              uint32_t             srcChannels,
                                                              uint32_t             engineSampleRate,
                                                              uint32_t             maxChannels,
                                                              uint32_t             ringFrames) {
    if (id == kInvalidSoundId)                                  return AudioResult::InvalidArgument;
    if (srcChannels == 0 || srcChannels > 2)                    return AudioResult::InvalidArgument;
    if (maxChannels == 0 || maxChannels > 2)                    return AudioResult::InvalidArgument;
    if (srcSampleRate == 0 || engineSampleRate == 0)            return AudioResult::InvalidArgument;
    if (ringFrames == 0)                                         return AudioResult::InvalidArgument;
    if (samples.empty() || samples.size() % srcChannels != 0)   return AudioResult::InvalidArgument;
    if (streams_.size() >= capacity_ && !streams_.contains(id)) return AudioResult::BudgetExceeded;

    auto raw = MemoryPcmDecoder::CreateOwning(std::move(samples), srcSampleRate, srcChannels);
    auto dec = MaybeResample(std::move(raw), engineSampleRate);
    if (!dec) return AudioResult::DecodeError;

    const uint32_t outChannels = std::min(dec->Channels(), maxChannels);
    if (outChannels == 0 || outChannels > 2) return AudioResult::DecodeError;

    StreamingAsset asset;
    asset.decoder     = std::move(dec);
    asset.channels    = outChannels;
    asset.sampleRate  = engineSampleRate;
    asset.totalFrames = asset.decoder->TotalFrames();
    asset.ring        = std::make_unique<util::PcmRingF32>(ringFrames, outChannels);
    asset.state       = StreamingAsset::State::Idle;

    streams_[id] = std::move(asset);
    return AudioResult::Success;
}

const SoundDefinition* AudioAssetRegistry::GetDefinition(AudioSoundId id) const noexcept {
    auto it = defs_.find(id);
    return it == defs_.end() ? nullptr : &it->second;
}

const PcmAsset* AudioAssetRegistry::GetAsset(AudioSoundId id) const noexcept {
    auto it = pcms_.find(id);
    return it == pcms_.end() ? nullptr : &it->second;
}

StreamingAsset* AudioAssetRegistry::GetStreamingAsset(AudioSoundId id) noexcept {
    auto it = streams_.find(id);
    return it == streams_.end() ? nullptr : &it->second;
}

const StreamingAsset* AudioAssetRegistry::GetStreamingAsset(AudioSoundId id) const noexcept {
    auto it = streams_.find(id);
    return it == streams_.end() ? nullptr : &it->second;
}

uint32_t AudioAssetRegistry::Count() const noexcept {
    return static_cast<uint32_t>(pcms_.size() + streams_.size());
}

} // namespace audio
