// audio_engine/mixer/mixer_command.h
//
// Commands pushed from the audio control thread into the mixer's SPSC
// command ring, drained on the audio render thread at the start of each
// callback. This is the only data flow into the mixer's per-render state.

#ifndef AUDIO_ENGINE_MIXER_MIXER_COMMAND_H
#define AUDIO_ENGINE_MIXER_MIXER_COMMAND_H

#include <cstdint>
#include "audio_engine/types.h"
#include "audio_engine/bus.h"

namespace audio {

namespace util { class PcmRing; class PcmRingF32; }

enum class MixerCommandKind : uint8_t {
    StartSound,          // play a registered sound asset (pinned float buffer)
    StartStreamingSound, // play a streaming source via a PcmRingF32
    StartVoice,          // play decoded voice from a PcmRing (int16)
    UpdateParams,        // gain/pan/pitch update for an existing voice
    Stop,                // immediate stop of a mix voice
    SetBusGain,          // update output gain on a bus (gainDb -> outputGainLinear)
    SetEffectParameter,  // update an effect's parameter
};

struct MixerCommand {
    MixerCommandKind kind = MixerCommandKind::Stop;
    uint32_t mixSlot      = 0;        // index into mixer's voice array

    // Common voice params
    float gain   = 1.0f;
    float pan    = 0.0f;
    float pitch  = 1.0f;

    // Per-voice low-pass amount (0 = bypass, 1 = heavy muffle). Drives the
    // voice's per-channel biquad LPF. Used for occlusion damping; the
    // spatializer caps occlusion-driven values, but the field accepts the
    // full [0,1] range so future systems (environmental zones, water,
    // user-set design intent) can stack into it.
    float lowPassAmount = 0.0f;

    // Per-voice reverb send level (0 = none, 1 = full). The mixer
    // additionally accumulates this voice's panned/gained signal into
    // the bus identified by kBusReverb (if such a bus exists in the graph)
    // scaled by reverbSend. The dry path through the voice's normal
    // targetBus is unaffected.
    float reverbSend    = 0.0f;

    // Binaural (per-ear) parameters. When `useBinaural` is true the mixer
    // ignores `pan` and `lowPassAmount` and instead applies independent
    // per-ear gain, fractional delay (ITD), and per-ear LPF amount (ILD
    // head shadow). Driven by SphericalHeadSpatializer; DefaultSpatializer
    // leaves `useBinaural` false and the mixer uses the existing pan path.
    bool  useBinaural    = false;
    float gainL          = 1.0f;
    float gainR          = 1.0f;
    float delaySamplesL  = 0.0f;
    float delaySamplesR  = 0.0f;
    float lpfAmountL     = 0.0f;
    float lpfAmountR     = 0.0f;

    // For Stop commands: if > 0, the mixer ramps the voice's gain
    // toward zero over `fadeOutMs` milliseconds before marking it
    // Inactive, using an equal-power (cosine) curve so a simultaneous
    // fade-in on another voice sums to constant power. Best-effort:
    // if the slot gets reused by a fresh StartSound (e.g. after
    // eviction), the new sound preempts the fade. For natural-end
    // stops (emitter destruction, one-shot completion) the fade
    // plays out fully.
    float fadeOutMs = 0.0f;

    // For StartSound / StartStreamingSound: if > 0, the new voice
    // ramps gain from 0 to its target over `fadeInMs` milliseconds
    // using an equal-power (sine) curve, paired with the cosine
    // fade-out so a crossfade has constant power throughout.
    float fadeInMs = 0.0f;

    // StartSound payload
    const float* pcmData    = nullptr;
    uint32_t     pcmFrames  = 0;
    uint32_t     pcmChannels = 1;
    bool         looping    = false;

    // StartStreamingSound payload
    util::PcmRingF32* streamRing     = nullptr;
    uint32_t          streamChannels = 1;

    // StartVoice payload
    util::PcmRing* voiceRing     = nullptr;
    uint32_t       voiceChannels = 1;

    // Voice -> bus routing (StartSound / StartStreamingSound / StartVoice / UpdateParams).
    BusId targetBus = kInvalidBusId;

    // SetBusGain / SetEffectParameter payload
    BusId    busId       = kInvalidBusId;
    uint32_t effectIndex = 0;
    uint16_t paramId     = 0;
    float    paramValue  = 0.0f;
};

} // namespace audio

#endif // AUDIO_ENGINE_MIXER_MIXER_COMMAND_H

