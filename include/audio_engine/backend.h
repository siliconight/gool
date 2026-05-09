// audio_engine/backend.h
//
// Audio output seam. The backend owns the device callback that pulls
// samples from the engine's render path. One of the four real
// polymorphism seams. Two implementations ship: NullAudioBackend
// (default, spins a wall-clock-paced thread that invokes OnRender at the
// configured rate and discards the output) and MiniaudioBackend
// (opt-in, drives a real audio device via miniaudio).

#ifndef AUDIO_ENGINE_BACKEND_H
#define AUDIO_ENGINE_BACKEND_H

#include <cstdint>
#include "audio_engine/result.h"

namespace audio {

// Callback invoked on the audio render thread. Implementations of OnRender
// must not allocate, lock, throw, or do any I/O. The engine's mixer is the
// canonical implementation.
class IAudioRenderCallback {
public:
    virtual ~IAudioRenderCallback() = default;

    // Fill `output` with `frames` interleaved float samples across `channels`.
    // Buffer size is frames * channels floats. Output range is [-1, 1].
    virtual void OnRender(float*   output,
                          uint32_t frames,
                          uint32_t channels) noexcept = 0;
};

struct AudioBackendConfig {
    uint32_t sampleRate = 48000;
    uint32_t bufferSize = 512;      // frames per callback
    uint32_t channels   = 2;
};

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    // Begin invoking `callback->OnRender` on a dedicated audio render thread
    // (or whatever the platform provides) at the configured rate. The
    // callback pointer must outlive the backend; ownership is not taken.
    virtual AudioResult Start(const AudioBackendConfig& config,
                               IAudioRenderCallback*    callback) = 0;

    // Stops invocation of OnRender and joins the render thread. Idempotent.
    virtual void Stop() = 0;

    virtual uint32_t SampleRate() const noexcept = 0;
    virtual uint32_t BufferSize() const noexcept = 0;
    virtual uint32_t Channels()   const noexcept = 0;

    virtual const char* Name() const noexcept = 0;
};

} // namespace audio

#endif // AUDIO_ENGINE_BACKEND_H
