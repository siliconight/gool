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

// audio_engine/listener.h
//
// A listener is the point of hearing. The current runtime supports one
// active listener; the type and its registration path are designed so
// that adding multi-listener support later doesn't change the public
// emitter/event API.

#ifndef AUDIO_ENGINE_LISTENER_H
#define AUDIO_ENGINE_LISTENER_H

#include "audio_engine/types.h"

namespace audio {

struct AudioListener {
    AudioPlayerId playerId   = kInvalidPlayerId;
    Vec3          position{};
    Vec3          forward{0.0f, 0.0f, -1.0f};   // host coord-system
    Vec3          up{0.0f, 1.0f, 0.0f};
    Vec3          velocity{};
    AudioZoneId   activeZone = kInvalidZoneId;

    // Reserved for weighted multi-listener mixing. The single-listener
    // path ignores this.
    float         weight     = 1.0f;
};

} // namespace audio

#endif // AUDIO_ENGINE_LISTENER_H
