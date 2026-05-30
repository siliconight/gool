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
#include "audio_engine/thread_annotations.h"

namespace audio {

// Callback invoked on the audio render thread. Implementations of OnRender
// must not allocate, lock, throw, or do any I/O. The engine's mixer is the
// canonical implementation.
class IAudioRenderCallback {
public:
    virtual ~IAudioRenderCallback() = default;

    // Fill `output` with `frames` interleaved float samples across `channels`.
    // Buffer size is frames * channels floats. Output range is [-1, 1].
    //
    // Annotated AUDIO_REQUIRES(RenderThread) so under Clang Thread Safety
    // Analysis, only call sites holding a RenderThread capability can
    // invoke this. The render thread itself does no allocations, no
    // locks, no syscalls, no exceptions — implementations are expected
    // to honor the same constraints.
    virtual void OnRender(float*   output,
                          uint32_t frames,
                          uint32_t channels) noexcept
        AUDIO_REQUIRES(RenderThread) AUDIO_NO_ALLOC AUDIO_RENDER_PATH = 0;
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
