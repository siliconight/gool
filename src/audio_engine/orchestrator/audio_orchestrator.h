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

// audio_engine/orchestrator/audio_orchestrator.h
//
// Concrete orchestrator. Holds a ParameterSmoother and a SequencePlayer.
// Owned by AudioRuntimeImpl; called once per Update tick. Designed so
// future additions (state machines, rule graphs, blueprint integration)
// can add new fields without changing AudioRuntime's public surface.

#ifndef AUDIO_ENGINE_ORCHESTRATOR_AUDIO_ORCHESTRATOR_H
#define AUDIO_ENGINE_ORCHESTRATOR_AUDIO_ORCHESTRATOR_H

#include "audio_engine/orchestrator/parameter_smoother.h"
#include "audio_engine/orchestrator/sequence_player.h"

namespace audio {

class AudioOrchestrator {
public:
    ParameterSmoother& Smoother() noexcept { return smoother_; }
    SequencePlayer&    Sequencer() noexcept { return sequencer_; }

    const ParameterSmoother& Smoother() const noexcept { return smoother_; }
    const SequencePlayer&    Sequencer() const noexcept { return sequencer_; }

    void Tick(float deltaSeconds, const SequencePlayer::PlayCallback& playSound) {
        smoother_.Tick(deltaSeconds);
        sequencer_.Tick(deltaSeconds, playSound);
    }

private:
    ParameterSmoother smoother_;
    SequencePlayer    sequencer_;
};

} // namespace audio

#endif // AUDIO_ENGINE_ORCHESTRATOR_AUDIO_ORCHESTRATOR_H
