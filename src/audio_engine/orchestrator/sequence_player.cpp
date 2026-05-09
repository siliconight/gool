// audio_engine/orchestrator/sequence_player.cpp

#include "audio_engine/orchestrator/sequence_player.h"

#include <algorithm>

namespace audio {

void SequencePlayer::Register(SequenceDefinition def) {
    if (def.id == 0) return;
    registry_[def.id] = std::move(def);
}

bool SequencePlayer::Trigger(AudioSequenceId id) {
    if (registry_.find(id) == registry_.end()) return false;
    active_.push_back(Active{id, 0, 0.0f});
    return true;
}

void SequencePlayer::Tick(float deltaSeconds, const PlayCallback& cb) {
    const float dt = std::max(0.0f, deltaSeconds);
    for (auto& a : active_) {
        a.elapsed += dt;

        auto it = registry_.find(a.id);
        if (it == registry_.end()) {
            a.nextStep = SIZE_MAX;
            continue;
        }
        const auto& steps = it->second.steps;

        while (a.nextStep < steps.size() && a.elapsed >= steps[a.nextStep].delaySeconds) {
            const auto& step = steps[a.nextStep];
            if (step.soundId != kInvalidSoundId && cb) {
                cb(step.soundId);
            }
            ++a.nextStep;
        }
    }
    // Drop completed entries.
    active_.erase(
        std::remove_if(active_.begin(), active_.end(),
                       [&](const Active& a) {
                           auto it = registry_.find(a.id);
                           return it == registry_.end() ||
                                  a.nextStep >= it->second.steps.size();
                       }),
        active_.end());
}

void SequencePlayer::Clear() {
    active_.clear();
}

} // namespace audio
