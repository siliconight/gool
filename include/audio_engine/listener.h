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
