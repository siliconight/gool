// audio_engine/decoders/decoder_factory.cpp

#include "audio_engine/decoders/decoder_factory.h"
#include "audio_engine/decoders/flac_decoder.h"
#include "audio_engine/decoders/ogg_vorbis_decoder.h"
#include "audio_engine/decoders/wav_decoder.h"

#include <cstring>

namespace audio {

namespace {

bool MatchAscii(const uint8_t* p, const char* sig, size_t n) noexcept {
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != static_cast<uint8_t>(sig[i])) return false;
    }
    return true;
}

bool EqIgnoreCase(const char* a, const char* b) noexcept {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

const char* FindExtension(const char* path) noexcept {
    if (!path) return nullptr;
    const char* dot = nullptr;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') dot = nullptr;     // reset across path separators
        else if (*p == '.')          dot = p;
    }
    return dot;
}

} // namespace

AudioFileFormat DecoderFactory::SniffFormat(const uint8_t* data, uint64_t size) noexcept {
    if (!data || size < 4) return AudioFileFormat::Auto;

    // RIFF....WAVE; at least 12 bytes are needed to confirm.
    if (size >= 12
        && MatchAscii(data, "RIFF", 4)
        && MatchAscii(data + 8, "WAVE", 4)) {
        return AudioFileFormat::Wav;
    }

    // OggS; Ogg page header magic.
    if (MatchAscii(data, "OggS", 4)) {
        return AudioFileFormat::OggVorbis;
    }

    // fLaC; FLAC stream marker.
    if (MatchAscii(data, "fLaC", 4)) {
        return AudioFileFormat::Flac;
    }

    return AudioFileFormat::Auto;
}

AudioFileFormat DecoderFactory::FormatFromExtension(const char* path) noexcept {
    const char* ext = FindExtension(path);
    if (!ext) return AudioFileFormat::Auto;
    if (EqIgnoreCase(ext, ".wav"))  return AudioFileFormat::Wav;
    if (EqIgnoreCase(ext, ".ogg"))  return AudioFileFormat::OggVorbis;
    if (EqIgnoreCase(ext, ".oga"))  return AudioFileFormat::OggVorbis;
    if (EqIgnoreCase(ext, ".flac")) return AudioFileFormat::Flac;
    return AudioFileFormat::Auto;
}

std::unique_ptr<IAudioDecoder> DecoderFactory::CreateForFile(const char* path) {
    const AudioFileFormat ext = FormatFromExtension(path);
    switch (ext) {
        case AudioFileFormat::Wav:       return WavDecoder::CreateFromFile(path);
        case AudioFileFormat::OggVorbis: return OggVorbisDecoder::CreateFromFile(path);
        case AudioFileFormat::Flac:      return FlacDecoder::CreateFromFile(path);
        case AudioFileFormat::Auto:      break;
    }
    // Extension didn't match anything we recognise. We could read the first
    // 16 bytes for a sniff, but doing that here would mean opening the file
    // twice; instead, attempt each decoder in turn and return the first one
    // that initialises successfully.
    if (auto d = WavDecoder::CreateFromFile(path))         return d;
    if (auto d = OggVorbisDecoder::CreateFromFile(path))   return d;
    if (auto d = FlacDecoder::CreateFromFile(path))        return d;
    return nullptr;
}

std::unique_ptr<IAudioDecoder> DecoderFactory::CreateForMemory(const uint8_t* data,
                                                                uint64_t       size,
                                                                AudioFileFormat hint) {
    AudioFileFormat fmt = hint;
    if (fmt == AudioFileFormat::Auto) fmt = SniffFormat(data, size);
    switch (fmt) {
        case AudioFileFormat::Wav:       return WavDecoder::CreateFromMemory(data, size);
        case AudioFileFormat::OggVorbis: return OggVorbisDecoder::CreateFromMemory(data, size);
        case AudioFileFormat::Flac:      return FlacDecoder::CreateFromMemory(data, size);
        case AudioFileFormat::Auto:      return nullptr;
    }
    return nullptr;
}

} // namespace audio
