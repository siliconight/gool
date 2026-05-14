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

    // v0.15.0: monotonic count of exceptions caught at the render-thread
    // callback boundary. Non-zero in steady state means the engine's
    // render path is throwing (which it shouldn't — but third-party DSP
    // plugins in the mixer's effect chain could) and audio frames are
    // being dropped to silence. Read on the control thread; the counter
    // is atomic. Safe to poll every Update() tick.
    uint64_t RenderCallbackExceptions() const noexcept;

    // v0.22.7: render-thread diagnostic accessors. Each answers a
    // specific "the audio is silent — why?" question:
    //
    //   CallbackInvocations() == 0  →  miniaudio never invoked our
    //                                  data callback. Audio thread
    //                                  didn't actually start despite
    //                                  Start() returning Success.
    //
    //   FramesRendered() > 0
    //   AND PeakSampleAbs() == 0    →  callback IS running, frames
    //                                  ARE being written, but every
    //                                  sample is zero. Silence is
    //                                  upstream of the device — in
    //                                  the engine's render callback,
    //                                  the bus chain, or the decoder.
    //
    //   PeakSampleAbs() > 0         →  non-silent samples ARE reaching
    //                                  the device. Any inaudibility
    //                                  is downstream of gool (wrong
    //                                  Windows output device, app
    //                                  volume muted, exclusive-mode
    //                                  capture by another app).
    //
    // All three are lock-free reads from atomics updated on the
    // render thread. Cheap; safe to poll every Update() tick. PeakSampleAbs
    // is a running maximum — call ResetPeakSampleAbs() periodically
    // to get a fresh window. Typical pattern: read peak, log it,
    // reset, wait, repeat.
    uint64_t CallbackInvocations() const noexcept;
    uint64_t FramesRendered()       const noexcept;
    float    PeakSampleAbs()        const noexcept;
    void     ResetPeakSampleAbs() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace audio

#endif // AUDIO_ENGINE_BACKEND_MINIAUDIO_BACKEND_H
