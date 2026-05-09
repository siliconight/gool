// audio_engine/decoders/wav_decoder.h
//
// IAudioDecoder over WAV files via dr_wav. Compiled when
// AUDIO_ENGINE_DECODERS_WAV is defined; otherwise the factory returns
// Unsupported.

#ifndef AUDIO_ENGINE_DECODERS_WAV_DECODER_H
#define AUDIO_ENGINE_DECODERS_WAV_DECODER_H

#include "audio_engine/decoders/audio_decoder.h"

#include <cstdint>
#include <memory>

namespace audio {

class WavDecoder final : public IAudioDecoder {
public:
    static std::unique_ptr<WavDecoder> CreateFromFile(const char* path);
    static std::unique_ptr<WavDecoder> CreateFromMemory(const uint8_t* data,
                                                          uint64_t       size);

    ~WavDecoder() override;

    uint32_t SampleRate()  const noexcept override { return sampleRate_; }
    uint32_t Channels()    const noexcept override { return channels_; }
    uint64_t TotalFrames() const noexcept override { return totalFrames_; }

    uint32_t DecodeFrames(float* out, uint32_t frames) noexcept override;
    bool     Seek(uint64_t frame) noexcept override;

private:
    WavDecoder() = default;

    struct State;
    std::unique_ptr<State> state_;

    uint32_t sampleRate_  = 0;
    uint32_t channels_    = 0;
    uint64_t totalFrames_ = 0;
};

} // namespace audio

#endif // AUDIO_ENGINE_DECODERS_WAV_DECODER_H
