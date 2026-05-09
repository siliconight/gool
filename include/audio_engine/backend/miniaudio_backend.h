// audio_engine/backend/miniaudio_backend.h
//
// Cross-platform audio device backend backed by miniaudio
// (https://github.com/mackron/miniaudio). Optional: only compiled when the
// CMake option AUDIO_ENGINE_BACKEND_MINIAUDIO is ON, or when the consumer
// adds miniaudio_backend.cpp to their build manually with miniaudio.h on
// the include path.
//
// This header deliberately does NOT include miniaudio.h. miniaudio is a
// ~95k-line single-header C library; pulling it through every translation
// unit that wants to construct a MiniaudioBackend would be expensive. The
// implementation is pimpl'd into miniaudio_backend.cpp, which is the only
// TU that touches the miniaudio API.
//
// Platforms: Windows (WASAPI), macOS / iOS (CoreAudio), Linux (ALSA /
// PulseAudio / JACK), Android (AAudio / OpenSL), FreeBSD (OSS),
// Emscripten (Web Audio).

#ifndef AUDIO_ENGINE_BACKEND_MINIAUDIO_BACKEND_H
#define AUDIO_ENGINE_BACKEND_MINIAUDIO_BACKEND_H

#include <memory>

#include "audio_engine/backend.h"

namespace audio {

class MiniaudioBackend final : public IAudioBackend {
public:
    MiniaudioBackend();
    ~MiniaudioBackend() override;

    MiniaudioBackend(const MiniaudioBackend&) = delete;
    MiniaudioBackend& operator=(const MiniaudioBackend&) = delete;
    MiniaudioBackend(MiniaudioBackend&&) = delete;
    MiniaudioBackend& operator=(MiniaudioBackend&&) = delete;

    AudioResult Start(const AudioBackendConfig& config,
                       IAudioRenderCallback*    callback) override;

    void Stop() override;

    uint32_t SampleRate() const noexcept override;
    uint32_t BufferSize() const noexcept override;
    uint32_t Channels()   const noexcept override;

    const char* Name() const noexcept override { return "miniaudio"; }

    // Returns a human-readable string describing the negotiated device, e.g.
    // "WASAPI / Speakers (Realtek)" once Start has succeeded. Empty before.
    const char* DeviceDescription() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace audio

#endif // AUDIO_ENGINE_BACKEND_MINIAUDIO_BACKEND_H
