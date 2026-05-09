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
