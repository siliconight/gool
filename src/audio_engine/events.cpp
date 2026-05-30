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

// audio_engine/events.cpp

#include "audio_engine/events.h"

namespace audio {

AudioEvent AudioEvent::MakePlaySoundAtLocation(
    AudioSoundId sound,
    const Vec3& pos,
    AudioReplicationPolicy policy,
    AudioPriority pri,
    TimestampMs ts) {
    AudioEvent e;
    e.type              = AudioEventType::PlaySoundAtLocation;
    e.soundId           = sound;
    e.position          = pos;
    e.priority          = pri;
    e.replicationPolicy = policy;
    e.timestampMs       = ts;
    return e;
}

AudioEvent AudioEvent::MakePlaySoundAttachedToActor(
    AudioSoundId sound,
    AudioActorId actor,
    AudioReplicationPolicy policy,
    AudioPriority pri,
    TimestampMs ts) {
    AudioEvent e;
    e.type              = AudioEventType::PlaySoundAttachedToActor;
    e.soundId           = sound;
    e.actorId           = actor;
    e.priority          = pri;
    e.replicationPolicy = policy;
    e.timestampMs       = ts;
    return e;
}

AudioEvent AudioEvent::MakeStopEmitter(EmitterHandle h) {
    AudioEvent e;
    e.type    = AudioEventType::StopEmitter;
    e.emitter = h;
    return e;
}

AudioEvent AudioEvent::MakeSetEmitterParameter(
    EmitterHandle h,
    AudioParameterId param,
    float value,
    float smoothingMs) {
    AudioEvent e;
    e.type                  = AudioEventType::SetEmitterParameter;
    e.emitter               = h;
    e.parameterId           = param;
    e.parameterValue        = value;
    e.parameterSmoothingMs  = smoothingMs;
    return e;
}

AudioEvent AudioEvent::MakeTriggerSequence(AudioSequenceId seq, AudioActorId actor) {
    AudioEvent e;
    e.type       = AudioEventType::TriggerSequence;
    e.sequenceId = seq;
    e.actorId    = actor;
    return e;
}

} // namespace audio
