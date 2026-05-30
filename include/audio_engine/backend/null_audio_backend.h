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

// audio_engine/backend/null_audio_backend.h
//
// Wall-clock-paced backend with no audio device. Spins a background thread
// that invokes IAudioRenderCallback::OnRender at the configured rate and
// discards the produced samples. Used in tests, headless servers, and as
// the default until a native backend (WASAPI/CoreAudio/ALSA) is wired in.

#ifndef AUDIO_ENGINE_BACKEND_NULL_AUDIO_BACKEND_H
#define AUDIO_ENGINE_BACKEND_NULL_AUDIO_BACKEND_H

#include <atomic>
#include <thread>
#include <vector>

#include "audio_engine/backend.h"

namespace audio {

class NullAudioBackend final : public IAudioBackend {
public:
    NullAudioBackend()  = default;
    ~NullAudioBackend() override;

    AudioResult Start(const AudioBackendConfig& config,
                       IAudioRenderCallback*    callback) override;

    void Stop() override;

    uint32_t SampleRate() const noexcept override { return config_.sampleRate; }
    uint32_t BufferSize() const noexcept override { return config_.bufferSize; }
    uint32_t Channels()   const noexcept override { return config_.channels;   }

    const char* Name() const noexcept override { return "null"; }

private:
    void RenderLoop();

    AudioBackendConfig    config_{};
    IAudioRenderCallback* callback_ = nullptr;
    std::vector<float>    scratch_;
    std::thread           thread_;
    std::atomic<bool>     running_{false};
};

} // namespace audio

#endif // AUDIO_ENGINE_BACKEND_NULL_AUDIO_BACKEND_H
