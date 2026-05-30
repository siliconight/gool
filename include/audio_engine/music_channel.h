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

#ifndef AUDIO_ENGINE_MUSIC_CHANNEL_H
#define AUDIO_ENGINE_MUSIC_CHANNEL_H

// MusicChannel: helper for the music-crossfade pattern.
//
// A common need that's not quite a play event: "swap the music track,
// fading the old one out and the new one in over the same duration."
// Without this helper, host code has to track two emitter handles,
// coordinate their fade durations, and clean up the previous track.
// MusicChannel does it once, correctly, with equal-power curves so
// the total power stays constant during the transition.
//
// Usage:
//   audio::MusicChannel music(runtime);
//   music.Play(combatTrackId, /*fadeMs=*/1500.0f);
//   ...
//   music.Play(menuTrackId, 800.0f);   // crossfades combat → menu
//   ...
//   music.Stop(2000.0f);                // fade-out current track
//
// MusicChannel does not own the runtime; the caller must keep the
// runtime alive for the channel's lifetime. The channel always plays
// its tracks via CreateEmitter (looping streaming or PCM, depending
// on how the sound was registered) so the existing replication,
// occlusion, and bus-routing semantics apply normally.

#include "audio_engine/handles.h"
#include "audio_engine/types.h"

namespace audio {

class AudioRuntime;

class MusicChannel {
public:
    explicit MusicChannel(AudioRuntime& runtime) noexcept;
    ~MusicChannel();

    MusicChannel(const MusicChannel&)            = delete;
    MusicChannel& operator=(const MusicChannel&) = delete;
    MusicChannel(MusicChannel&&)                 = delete;
    MusicChannel& operator=(MusicChannel&&)      = delete;

    // Crossfade-in `soundId` over `fadeMs` ms. The currently-playing
    // track (if any) fades out over the same duration on an
    // equal-power (cosine) curve while the new track fades in on a
    // sine curve, so total power is constant. fadeMs == 0 cuts.
    //
    // Requires the sound to be registered with the runtime. The
    // SoundDefinition's looping/spatialized/bus settings apply.
    // Returns the new emitter handle, or kNullEmitterHandle on
    // failure (typically: budget exceeded, sound not registered,
    // streaming asset already in use).
    EmitterHandle Play(AudioSoundId soundId, float fadeMs = 1500.0f);

    // Fade-out the current track with no replacement. After the fade
    // completes the channel is silent (Current() returns null).
    // No-op if the channel is already silent.
    void Stop(float fadeMs = 1500.0f);

    // The currently-playing emitter, or kNullEmitterHandle if the
    // channel is silent. Useful for SetEmitterParameter on the live
    // track (e.g. ducking the music volume during dialogue).
    EmitterHandle Current() const noexcept { return current_; }

private:
    AudioRuntime& runtime_;
    EmitterHandle current_ = kNullEmitterHandle;
    // Previous track that's fading out. We hold the handle for one
    // Play() call so the next Play() can clean it up; the mixer slot
    // self-deactivates when the fade completes regardless.
    EmitterHandle previous_ = kNullEmitterHandle;
};

} // namespace audio

#endif // AUDIO_ENGINE_MUSIC_CHANNEL_H
