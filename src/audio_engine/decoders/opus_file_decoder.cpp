// audio_engine/decoders/opus_file_decoder.cpp
//
// Decoder backed by libopusfile (the .opus container reader on top of
// libopus). Conditionally compiled — when AUDIO_ENGINE_DECODERS_OPUS
// is undefined this TU expands to the stub at the bottom and the
// factory's CreateFor* returns nullptr.
//
// libopusfile is the C library used by every major Opus tooling
// (Firefox, Audacity, etc). It handles Ogg page reassembly, header
// parsing, seeking, and float decode. We treat it as a black box
// behind IAudioDecoder.

#include "audio_engine/decoders/opus_file_decoder.h"

#if defined(AUDIO_ENGINE_DECODERS_OPUS)

#include <opus/opusfile.h>

#include <cstring>

namespace audio {

struct OpusFileDecoder::State {
    OggOpusFile* file = nullptr;
};

namespace {

uint64_t SafePcmTotal(OggOpusFile* of) noexcept {
    // op_pcm_total returns a signed total or a negative error; treat
    // unknown / unseekable streams as 0 (the IAudioDecoder contract
    // permits 0 = "unknown size", same convention as Vorbis).
    if (!of) return 0;
    const ogg_int64_t n = op_pcm_total(of, -1);
    return n > 0 ? static_cast<uint64_t>(n) : 0;
}

} // namespace

std::unique_ptr<OpusFileDecoder> OpusFileDecoder::CreateFromFile(const char* path) {
    if (!path) return nullptr;
    int err = 0;
    OggOpusFile* f = op_open_file(path, &err);
    if (!f) return nullptr;

    auto d = std::unique_ptr<OpusFileDecoder>(new OpusFileDecoder());
    d->state_         = std::make_unique<State>();
    d->state_->file   = f;
    // Opus always decodes at 48 kHz; channels come from the link-0
    // header. Multilink files (concatenated streams) are rare enough
    // that we don't try to be clever — we report the first link's
    // channel count and let libopusfile handle link transitions
    // internally during decode.
    const int ch = op_channel_count(f, -1);
    d->channels_      = (ch > 0) ? static_cast<uint32_t>(ch) : 0;
    d->sampleRate_    = 48000;
    d->totalFrames_   = SafePcmTotal(f);
    return d;
}

std::unique_ptr<OpusFileDecoder> OpusFileDecoder::CreateFromMemory(const uint8_t* data,
                                                                     uint64_t       size) {
    if (!data || size == 0) return nullptr;
    int err = 0;
    OggOpusFile* f = op_open_memory(static_cast<const unsigned char*>(data),
                                      static_cast<size_t>(size), &err);
    if (!f) return nullptr;

    auto d = std::unique_ptr<OpusFileDecoder>(new OpusFileDecoder());
    d->state_         = std::make_unique<State>();
    d->state_->file   = f;
    const int ch      = op_channel_count(f, -1);
    d->channels_      = (ch > 0) ? static_cast<uint32_t>(ch) : 0;
    d->sampleRate_    = 48000;
    d->totalFrames_   = SafePcmTotal(f);
    return d;
}

OpusFileDecoder::~OpusFileDecoder() {
    if (state_ && state_->file) op_free(state_->file);
}

uint32_t OpusFileDecoder::DecodeFrames(float* out, uint32_t frames) noexcept {
    if (!state_ || !state_->file || !out || frames == 0 || channels_ == 0) return 0;
    // op_read_float returns the number of *samples per channel* decoded,
    // i.e. frames. buf_size argument is the total number of floats
    // available in `out`. Drive the call in a loop because libopusfile
    // may return fewer frames than requested per call (page boundary).
    uint32_t produced = 0;
    while (produced < frames) {
        const uint32_t remaining   = frames - produced;
        const int      bufSize     = static_cast<int>(remaining * channels_);
        const int      n           = op_read_float(state_->file,
                                                     out + produced * channels_,
                                                     bufSize,
                                                     /*li*/ nullptr);
        if (n <= 0) break;          // 0 = EOF, <0 = error
        produced += static_cast<uint32_t>(n);
    }
    return produced;
}

bool OpusFileDecoder::Seek(uint64_t frame) noexcept {
    if (!state_ || !state_->file) return false;
    return op_pcm_seek(state_->file, static_cast<ogg_int64_t>(frame)) == 0;
}

} // namespace audio

#else // AUDIO_ENGINE_DECODERS_OPUS undefined → stub fallback

namespace audio {

struct OpusFileDecoder::State {};
OpusFileDecoder::~OpusFileDecoder() = default;
std::unique_ptr<OpusFileDecoder> OpusFileDecoder::CreateFromFile(const char*)             { return nullptr; }
std::unique_ptr<OpusFileDecoder> OpusFileDecoder::CreateFromMemory(const uint8_t*, uint64_t) { return nullptr; }
uint32_t OpusFileDecoder::DecodeFrames(float*, uint32_t) noexcept { return 0; }
bool     OpusFileDecoder::Seek(uint64_t) noexcept                  { return false; }

} // namespace audio

#endif
