// audio_engine/decoders/ogg_vorbis_decoder.cpp
//
// Vorbis decoding via stb_vorbis. The library is a single .c file, but
// stb_vorbis.c works fine when included from a C++ TU. We isolate it here
// so it never leaks into the rest of the codebase.

#include "audio_engine/decoders/ogg_vorbis_decoder.h"

#if defined(AUDIO_ENGINE_DECODERS_OGG)

// stb_vorbis is decade-old C with aggressive macros; suppress noisy
// warnings under -Wpedantic / -Wshadow etc when building under -Werror.
// Apple Clang defines __GNUC__ for compatibility but does NOT recognize
// -Wmaybe-uninitialized — keep that pragma in a GCC-only sub-block so
// Clang doesn't fail with "unknown warning option" under -Werror.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#  pragma GCC diagnostic ignored "-Wpedantic"
#  pragma GCC diagnostic ignored "-Wsign-conversion"
#  pragma GCC diagnostic ignored "-Wconversion"
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wcast-qual"
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4244)  // conversion: possible loss of data
#  pragma warning(disable : 4245)  // signed/unsigned mismatch
#  pragma warning(disable : 4267)  // size_t → smaller type
#  pragma warning(disable : 4456)  // declaration hides previous local
#  pragma warning(disable : 4457)  // declaration hides function parameter
#  pragma warning(disable : 4459)  // declaration hides global
#  pragma warning(disable : 4701)  // potentially uninitialized local
#  pragma warning(disable : 4703)  // potentially uninitialized local pointer
#  pragma warning(disable : 4996)  // CRT deprecation (sprintf, etc.)
#endif

extern "C" {
#include "stb_vorbis.c"  // NOLINT(bugprone-suspicious-include) — canonical single-TU include
}

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

namespace audio {

struct OggVorbisDecoder::State {
    stb_vorbis* vorbis = nullptr;
};

std::unique_ptr<OggVorbisDecoder> OggVorbisDecoder::CreateFromFile(const char* path) {
    if (!path) return nullptr;
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_filename(path, &err, nullptr);
    if (!v) return nullptr;
    auto d = std::unique_ptr<OggVorbisDecoder>(new OggVorbisDecoder());
    d->state_ = std::make_unique<State>();
    d->state_->vorbis = v;
    const stb_vorbis_info info = stb_vorbis_get_info(v);
    d->sampleRate_  = info.sample_rate;
    d->channels_    = static_cast<uint32_t>(info.channels);
    d->totalFrames_ = static_cast<uint64_t>(stb_vorbis_stream_length_in_samples(v));
    return d;
}

std::unique_ptr<OggVorbisDecoder> OggVorbisDecoder::CreateFromMemory(const uint8_t* data,
                                                                      uint64_t       size) {
    if (!data || size == 0) return nullptr;
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_memory(data, static_cast<int>(size), &err, nullptr);
    if (!v) return nullptr;
    auto d = std::unique_ptr<OggVorbisDecoder>(new OggVorbisDecoder());
    d->state_ = std::make_unique<State>();
    d->state_->vorbis = v;
    const stb_vorbis_info info = stb_vorbis_get_info(v);
    d->sampleRate_  = info.sample_rate;
    d->channels_    = static_cast<uint32_t>(info.channels);
    d->totalFrames_ = static_cast<uint64_t>(stb_vorbis_stream_length_in_samples(v));
    return d;
}

OggVorbisDecoder::~OggVorbisDecoder() {
    if (state_ && state_->vorbis) stb_vorbis_close(state_->vorbis);
}

uint32_t OggVorbisDecoder::DecodeFrames(float* out, uint32_t frames) noexcept {
    if (!state_ || !state_->vorbis || !out || frames == 0 || channels_ == 0) return 0;
    // stb_vorbis_get_samples_float_interleaved takes "num_floats"; channels * frames.
    const int n = stb_vorbis_get_samples_float_interleaved(
        state_->vorbis,
        static_cast<int>(channels_),
        out,
        static_cast<int>(frames * channels_));
    return n > 0 ? static_cast<uint32_t>(n) : 0;
}

bool OggVorbisDecoder::Seek(uint64_t frame) noexcept {
    if (!state_ || !state_->vorbis) return false;
    if (frame == 0) return stb_vorbis_seek_start(state_->vorbis) != 0;
    return stb_vorbis_seek(state_->vorbis,
                            static_cast<unsigned int>(frame)) != 0;
}

} // namespace audio

#else // AUDIO_ENGINE_DECODERS_OGG not defined

namespace audio {

struct OggVorbisDecoder::State {};
OggVorbisDecoder::~OggVorbisDecoder() = default;
std::unique_ptr<OggVorbisDecoder> OggVorbisDecoder::CreateFromFile(const char*)             { return nullptr; }
std::unique_ptr<OggVorbisDecoder> OggVorbisDecoder::CreateFromMemory(const uint8_t*, uint64_t) { return nullptr; }
uint32_t OggVorbisDecoder::DecodeFrames(float*, uint32_t) noexcept { return 0; }
bool     OggVorbisDecoder::Seek(uint64_t) noexcept                  { return false; }

} // namespace audio

#endif
