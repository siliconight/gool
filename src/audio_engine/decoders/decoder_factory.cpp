// Copyright 2026 Brannen Graves
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing permissions
// and limitations under the License.

// audio_engine/decoders/decoder_factory.cpp

#include "audio_engine/decoders/decoder_factory.h"
#include "audio_engine/decoders/flac_decoder.h"
#include "audio_engine/decoders/ogg_vorbis_decoder.h"
#include "audio_engine/decoders/opus_file_decoder.h"
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

// Both Vorbis and Opus use the Ogg container, both start with "OggS".
// The codec identification lives in the first packet's payload, which
// for a typical short Ogg page begins around byte 28. We probe a small
// window starting there to find either "OpusHead" (8 bytes) or
// "\x01vorbis" (7 bytes). Returns the resolved format, or OggVorbis
// as the legacy default when we have OggS but can't see far enough
// to disambiguate (the create-then-fallback path will sort it out).
AudioFileFormat ResolveOggVariant(const uint8_t* data, uint64_t size) noexcept {
    // Be generous about where the codec ID lives — Ogg page header is
    // 27 bytes plus a variable-length segment table. Scan a 64-byte
    // window starting at byte 27 for the magic strings.
    constexpr size_t kProbeStart = 27;
    constexpr size_t kProbeWindow = 64;
    if (size < kProbeStart + 8) return AudioFileFormat::OggVorbis;
    const size_t end = (size < kProbeStart + kProbeWindow)
        ? static_cast<size_t>(size)
        : kProbeStart + kProbeWindow;
    for (size_t i = kProbeStart; i + 8 <= end; ++i) {
        if (MatchAscii(data + i, "OpusHead", 8)) return AudioFileFormat::Opus;
    }
    for (size_t i = kProbeStart; i + 7 <= end; ++i) {
        if (data[i] == 0x01 && MatchAscii(data + i + 1, "vorbis", 6)) {
            return AudioFileFormat::OggVorbis;
        }
    }
    return AudioFileFormat::OggVorbis; // legacy default
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

    // OggS; could be Vorbis OR Opus. Disambiguate by the codec magic
    // in the first packet payload.
    if (MatchAscii(data, "OggS", 4)) {
        return ResolveOggVariant(data, size);
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
    if (EqIgnoreCase(ext, ".opus")) return AudioFileFormat::Opus;
    if (EqIgnoreCase(ext, ".flac")) return AudioFileFormat::Flac;
    return AudioFileFormat::Auto;
}

std::unique_ptr<IAudioDecoder> DecoderFactory::CreateForFile(const char* path) {
    const AudioFileFormat ext = FormatFromExtension(path);
    switch (ext) {
        case AudioFileFormat::Wav:       return WavDecoder::CreateFromFile(path);
        case AudioFileFormat::OggVorbis: return OggVorbisDecoder::CreateFromFile(path);
        case AudioFileFormat::Opus:      return OpusFileDecoder::CreateFromFile(path);
        case AudioFileFormat::Flac:      return FlacDecoder::CreateFromFile(path);
        case AudioFileFormat::Auto:      break;
    }
    // Extension didn't match anything we recognise. We could read the first
    // 16 bytes for a sniff, but doing that here would mean opening the file
    // twice; instead, attempt each decoder in turn and return the first one
    // that initialises successfully.
    if (auto d = WavDecoder::CreateFromFile(path))         return d;
    // Try Opus before Vorbis: an Ogg-Opus file given to OggVorbisDecoder
    // would silently fail anyway, but Opus rejects non-Opus streams
    // faster (header check) than Vorbis rejects non-Vorbis ones.
    if (auto d = OpusFileDecoder::CreateFromFile(path))    return d;
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
        case AudioFileFormat::Opus:      return OpusFileDecoder::CreateFromMemory(data, size);
        case AudioFileFormat::Flac:      return FlacDecoder::CreateFromMemory(data, size);
        case AudioFileFormat::Auto:      return nullptr;
    }
    return nullptr;
}

} // namespace audio
