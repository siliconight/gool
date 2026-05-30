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
