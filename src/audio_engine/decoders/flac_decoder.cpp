// audio_engine/decoders/flac_decoder.cpp

#include "audio_engine/decoders/flac_decoder.h"

#if defined(AUDIO_ENGINE_DECODERS_FLAC)

#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#  pragma GCC diagnostic ignored "-Wpedantic"
#  pragma GCC diagnostic ignored "-Wsign-conversion"
#  pragma GCC diagnostic ignored "-Wconversion"
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4244)  // conversion: possible loss of data
#  pragma warning(disable : 4245)  // signed/unsigned mismatch
#  pragma warning(disable : 4267)  // size_t → smaller type
#  pragma warning(disable : 4456)  // declaration hides previous local
#  pragma warning(disable : 4701)  // potentially uninitialized local
#  pragma warning(disable : 4703)  // potentially uninitialized local pointer
#  pragma warning(disable : 4996)  // CRT deprecation
#endif

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

namespace audio {

struct FlacDecoder::State {
    drflac* flac = nullptr;
};

std::unique_ptr<FlacDecoder> FlacDecoder::CreateFromFile(const char* path) {
    if (!path) return nullptr;
    drflac* f = drflac_open_file(path, nullptr);
    if (!f) return nullptr;
    auto d = std::unique_ptr<FlacDecoder>(new FlacDecoder());
    d->state_ = std::make_unique<State>();
    d->state_->flac = f;
    d->sampleRate_  = f->sampleRate;
    d->channels_    = f->channels;
    d->totalFrames_ = f->totalPCMFrameCount;
    return d;
}

std::unique_ptr<FlacDecoder> FlacDecoder::CreateFromMemory(const uint8_t* data,
                                                            uint64_t       size) {
    if (!data || size == 0) return nullptr;
    drflac* f = drflac_open_memory(data, static_cast<size_t>(size), nullptr);
    if (!f) return nullptr;
    auto d = std::unique_ptr<FlacDecoder>(new FlacDecoder());
    d->state_ = std::make_unique<State>();
    d->state_->flac = f;
    d->sampleRate_  = f->sampleRate;
    d->channels_    = f->channels;
    d->totalFrames_ = f->totalPCMFrameCount;
    return d;
}

FlacDecoder::~FlacDecoder() {
    if (state_ && state_->flac) drflac_close(state_->flac);
}

uint32_t FlacDecoder::DecodeFrames(float* out, uint32_t frames) noexcept {
    if (!state_ || !state_->flac || !out || frames == 0) return 0;
    return static_cast<uint32_t>(
        drflac_read_pcm_frames_f32(state_->flac, frames, out));
}

bool FlacDecoder::Seek(uint64_t frame) noexcept {
    if (!state_ || !state_->flac) return false;
    return drflac_seek_to_pcm_frame(state_->flac, frame) != 0;
}

} // namespace audio

#else // AUDIO_ENGINE_DECODERS_FLAC not defined

namespace audio {

struct FlacDecoder::State {};
FlacDecoder::~FlacDecoder() = default;
std::unique_ptr<FlacDecoder> FlacDecoder::CreateFromFile(const char*)             { return nullptr; }
std::unique_ptr<FlacDecoder> FlacDecoder::CreateFromMemory(const uint8_t*, uint64_t) { return nullptr; }
uint32_t FlacDecoder::DecodeFrames(float*, uint32_t) noexcept { return 0; }
bool     FlacDecoder::Seek(uint64_t) noexcept                  { return false; }

} // namespace audio

#endif
