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

// audio_engine/listeners/listener_manager.h
//
// Single-listener manager. The interface is designed so that adding
// multi-listener support later (per-listener spatial passes, mixed
// buses) doesn't change the public AudioRuntime API.

#ifndef AUDIO_ENGINE_LISTENERS_LISTENER_MANAGER_H
#define AUDIO_ENGINE_LISTENERS_LISTENER_MANAGER_H

#include "audio_engine/listener.h"
#include "audio_engine/spatializer.h"

namespace audio {

class ListenerManager {
public:
    void SetPrimary(const AudioListener& listener) noexcept;

    bool                  HasPrimary() const noexcept { return hasPrimary_; }
    const AudioListener&  Primary()    const noexcept { return primary_; }

    // Build the per-tick view consumed by the spatializer.
    SpatialListenerView BuildView() const noexcept;

private:
    AudioListener primary_{};
    bool          hasPrimary_ = false;
};

} // namespace audio

#endif // AUDIO_ENGINE_LISTENERS_LISTENER_MANAGER_H
