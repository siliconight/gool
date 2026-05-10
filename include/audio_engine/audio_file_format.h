// audio_engine/audio_file_format.h
//
// Format hint for in-memory decode entry points. `Auto` triggers magic-byte
// sniffing; the explicit values short-circuit detection when the host
// already knows what it has.

#ifndef AUDIO_ENGINE_AUDIO_FILE_FORMAT_H
#define AUDIO_ENGINE_AUDIO_FILE_FORMAT_H

#include <cstdint>

namespace audio {

enum class AudioFileFormat : uint8_t {
    Auto = 0,
    Wav,
    OggVorbis,
    Flac,
    Opus,
};

} // namespace audio

#endif // AUDIO_ENGINE_AUDIO_FILE_FORMAT_H
