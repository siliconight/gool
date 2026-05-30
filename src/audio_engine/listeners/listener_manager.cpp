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

// audio_engine/listeners/listener_manager.cpp

#include "audio_engine/listeners/listener_manager.h"

namespace audio {

void ListenerManager::SetPrimary(const AudioListener& listener) noexcept {
    primary_    = listener;
    hasPrimary_ = true;
}

SpatialListenerView ListenerManager::BuildView() const noexcept {
    SpatialListenerView v;
    if (!hasPrimary_) return v;
    v.position = primary_.position;
    v.forward  = primary_.forward;
    v.up       = primary_.up;
    v.velocity = primary_.velocity;
    return v;
}

} // namespace audio
