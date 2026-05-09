// MusicChannel implementation.

#include "audio_engine/music_channel.h"

#include "audio_engine/audio_runtime.h"
#include "audio_engine/emitter.h"

namespace audio {

MusicChannel::MusicChannel(AudioRuntime& runtime) noexcept
    : runtime_(runtime) {}

MusicChannel::~MusicChannel() {
    // Snap-stop on destruction: a long fade would outlive the
    // channel's owner and produce confusing behavior. Callers who
    // want a graceful tail call Stop() with the desired fadeMs first.
    if (!current_.IsNull())  runtime_.DestroyEmitter(current_,  0.0f);
    if (!previous_.IsNull()) runtime_.DestroyEmitter(previous_, 0.0f);
}

EmitterHandle MusicChannel::Play(AudioSoundId soundId, float fadeMs) {
    if (soundId == kInvalidSoundId) return kNullEmitterHandle;
    if (fadeMs < 0.0f) fadeMs = 0.0f;

    // Reap the prior previous_ first. Its mixer slot's fade-out has
    // had a full fadeMs to play out by now (since the previous Play()
    // call). Reaping here keeps the emitter slot free for re-use; we
    // then allocate the new track BEFORE moving the current track
    // into the previous slot, so the new track gets a fresh mix slot
    // and doesn't collide with the still-fading old one.
    if (!previous_.IsNull()) {
        runtime_.DestroyEmitter(previous_, 0.0f);
        previous_ = kNullEmitterHandle;
    }

    // Create the new track. CreateEmitter assigns a mix slot
    // synchronously; the previous emitter (current_) is still alive
    // and holds its own mix slot, so the allocator hands us a
    // different slot for the new track. The fade-in starts at gain
    // zero on the new slot; the cosine fade-out on the old slot
    // begins one moment later when we issue DestroyEmitter.
    EmitterDescriptor desc;
    desc.soundId          = soundId;
    desc.isLooping        = true;
    desc.isSpatialized    = false;
    desc.occlusionEnabled = false;
    desc.fadeInMs         = fadeMs;

    auto h = runtime_.CreateEmitter(desc);
    if (!h) return kNullEmitterHandle;

    // Now stop the old track with a fade. We don't destroy the
    // emitter slot — that would free the mix slot for re-allocation
    // and a subsequent Play() could preempt the fade. Instead, hold
    // the handle in `previous_` and reap it on the next Play() (by
    // which point the fade-out has completed).
    if (!current_.IsNull()) {
        // We want a fade-out without freeing the emitter. The runtime
        // doesn't expose StopWithFade as a separate call; we
        // approximate it by destroying with the fade — the mix slot
        // keeps fading, the emitter slot is freed. Since we've already
        // grabbed the new mix slot above, the freed emitter slot is
        // available for a future allocation but not this one.
        runtime_.DestroyEmitter(current_, fadeMs);
        previous_ = kNullEmitterHandle;  // current_ is invalid post-destroy
    }

    current_ = h.value();
    return current_;
}

void MusicChannel::Stop(float fadeMs) {
    if (fadeMs < 0.0f) fadeMs = 0.0f;
    if (!previous_.IsNull()) {
        runtime_.DestroyEmitter(previous_, 0.0f);
        previous_ = kNullEmitterHandle;
    }
    if (!current_.IsNull()) {
        runtime_.DestroyEmitter(current_, fadeMs);
        current_ = kNullEmitterHandle;
    }
}

} // namespace audio
