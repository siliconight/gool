// audio_engine/orchestrator/sequence_player.h
//
// Sequence playback: an ordered list of (delaySeconds, soundId) pairs
// that fire as one-shot PlaySound events at the given offsets from
// sequence trigger time. No branching, no rule graphs, no fade groups;
// those are future additions.

#ifndef AUDIO_ENGINE_ORCHESTRATOR_SEQUENCE_PLAYER_H
#define AUDIO_ENGINE_ORCHESTRATOR_SEQUENCE_PLAYER_H

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "audio_engine/types.h"

namespace audio {

struct SequenceStep {
    float        delaySeconds = 0.0f;
    AudioSoundId soundId      = kInvalidSoundId;
};

struct SequenceDefinition {
    AudioSequenceId           id    = 0;
    std::vector<SequenceStep> steps;
};

class SequencePlayer {
public:
    using PlayCallback = std::function<void(AudioSoundId)>;

    void Register(SequenceDefinition def);

    // Trigger a sequence by ID. Returns false if no sequence is registered.
    bool Trigger(AudioSequenceId id);

    // Tick all active triggered sequences. PlaySound side-effects are
    // routed through `cb` (typically a closure that pushes a PlaySound event
    // into the runtime).
    void Tick(float deltaSeconds, const PlayCallback& cb);

    void Clear();

private:
    struct Active {
        AudioSequenceId id          = 0;
        size_t          nextStep    = 0;
        float           elapsed     = 0.0f;
    };

    std::unordered_map<AudioSequenceId, SequenceDefinition> registry_;
    std::vector<Active>                                       active_;
};

} // namespace audio

#endif // AUDIO_ENGINE_ORCHESTRATOR_SEQUENCE_PLAYER_H
