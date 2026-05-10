// audio_engine/decoders/opus_file_decoder.h
//
// IAudioDecoder over Ogg Opus files (.opus / .ogg with OpusHead) via
// libopusfile. Compiled when AUDIO_ENGINE_DECODERS_OPUS is defined;
// otherwise the constructor reports null and the factory returns
// nullptr — callers see "unsupported format" instead of a link-time
// dependency on libopusfile.
//
// Notes specific to Opus:
//   * Opus always decodes at 48 kHz regardless of the source recording
//     rate. SampleRate() therefore always reports 48000. If the engine
//     is initialized at a different sample rate, the asset registry
//     wraps this decoder in a ResamplingDecoder during registration.
//   * Channel count comes from the file (mono, stereo, surround). The
//     binary loader path will downmix multichannel streams to stereo
//     before resampling — an asset-registry concern, not this decoder.
//   * Seeking is sample-accurate against the decoded 48 kHz stream;
//     Seek(0) is the "rewind for loop" path used by streaming voices.

#ifndef AUDIO_ENGINE_DECODERS_OPUS_FILE_DECODER_H
#define AUDIO_ENGINE_DECODERS_OPUS_FILE_DECODER_H

#include "audio_engine/decoders/audio_decoder.h"

#include <cstdint>
#include <memory>

namespace audio {

class OpusFileDecoder final : public IAudioDecoder {
public:
    static std::unique_ptr<OpusFileDecoder> CreateFromFile(const char* path);
    static std::unique_ptr<OpusFileDecoder> CreateFromMemory(const uint8_t* data,
                                                               uint64_t       size);

    ~OpusFileDecoder() override;

    uint32_t SampleRate()  const noexcept override { return sampleRate_; }
    uint32_t Channels()    const noexcept override { return channels_; }
    uint64_t TotalFrames() const noexcept override { return totalFrames_; }

    uint32_t DecodeFrames(float* out, uint32_t frames) noexcept override;
    bool     Seek(uint64_t frame) noexcept override;

private:
    OpusFileDecoder() = default;

    struct State;
    std::unique_ptr<State> state_;

    uint32_t sampleRate_  = 48000;   // Opus always decodes at 48 kHz
    uint32_t channels_    = 0;
    uint64_t totalFrames_ = 0;
};

} // namespace audio

#endif // AUDIO_ENGINE_DECODERS_OPUS_FILE_DECODER_H
