// audio_engine/config.h
//
// Configuration consumed by AudioRuntime::Initialize(). Values are read once
// and used to size all internal storage; they do not mutate after Initialize
// returns. Resizing is not supported.

#ifndef AUDIO_ENGINE_CONFIG_H
#define AUDIO_ENGINE_CONFIG_H

#include <cstdint>
#include "audio_engine/types.h"
#include "audio_engine/bus.h"

namespace audio {

struct AudioRuntimeBudget {
    uint32_t maxActiveEmitters         = 128;
    uint32_t maxSpatialEmitters        = 64;
    uint32_t maxVoiceSources           = 16;
    uint32_t maxOcclusionChecksPerFrame = 12;
    uint32_t maxStreamingAssets        = 32;
    uint32_t maxStreamingVoices        = 8;     // concurrent streaming voices
    uint32_t maxRegisteredSounds       = 256;
    uint32_t maxGameEventsPerFrame     = 256;
    uint32_t maxNetworkEventsPerFrame  = 256;
    // Interest-management cap on per-tick spatial processing. When > 0,
    // each tick the runtime sorts active emitters by distance to the
    // listener and only runs the spatializer + posts UpdateParams for
    // the closest N. Emitters outside the top-N are muted by posting a
    // single zero-gain UpdateParams the tick they fall out, and remain
    // silent until they re-enter the top-N (at which point a fresh
    // UpdateParams unmutes them). 0 (default) = unlimited, every active
    // emitter is processed every tick.
    //
    // Use this for shooters that have many more potential audible
    // sources than CPU budget allows (e.g. 60-player battle royales
    // where every player's footstep emitter exists but only 30 should
    // be heard at any moment).
    uint32_t maxActiveEmittersProcessedPerTick = 0;
};

struct AudioConfig {
    // Output
    uint32_t        sampleRate    = 48000;
    uint32_t        bufferSize    = 512;            // frames per backend callback
    AudioOutputMode outputMode    = AudioOutputMode::Stereo;

    // Subsystem toggles
    bool enableVoice            = true;
    bool enableOcclusion        = true;
    bool enableReverbZones      = true;
    bool enableHrtf             = false;
    bool enableDoppler          = true;
    bool enableAirAbsorption    = true;

    // Speed of sound in m/s used by the Doppler model. 343 is air at ~20 °C;
    // dial down for thin atmospheres or up for water (1480) if you want
    // more pronounced Doppler.
    float speedOfSound = 343.0f;

    // Air absorption: amount per meter contributed to the per-voice LPF's
    // lowPassAmount. With the default 1/250 = 0.004, a source 250 m away
    // saturates the air-absorption contribution at 1.0 (heavy muffle).
    // Combined with occlusion via max(occlusionAmount, airAmount) so the
    // strongest source wins; cascading both LPFs would over-damp.
    float airAbsorptionPerMeter = 1.0f / 250.0f;

    // Global reverb send level [0, 1]: how much of every spatialized voice's
    // signal is forwarded to the conventional reverb bus (kBusReverb) by
    // the default spatializer. Defaults to 0 so a runtime with no reverb
    // bus configured stays silent on the send path. The host opts in by:
    //   (1) adding a bus with id=kBusReverb (typically with a single
    //       EffectKind::Reverb effect on its chain),
    //   (2) bumping this knob above 0.
    float globalReverbSend = 0.0f;

    // Late-event discard horizon (ms). Events older than this when drained
    // from the queue are dropped rather than played. Applies to events; voice
    // packets have their own jitter-buffer policy and are independent.
    uint32_t lateEventDiscardMs = 250;

    // Voice
    uint32_t voicePacketRingDepth = 32;     // per sender
    uint32_t voicePcmRingFrames   = 9600;   // 200 ms at 48 kHz
    uint32_t voiceMaxPacketBytes  = 1500;
    uint32_t voiceJitterTargetMs  = 60;

    // Streaming
    uint32_t streamingRingFrames        = 24000;   // ~500 ms at 48 kHz, per voice
    uint32_t streamingDecodeChunkFrames = 2048;    // chunk size pumped per Update

    AudioRuntimeBudget budget;

    // Bus graph and DSP. When busGraph.busCount == 0, the runtime auto-builds
    // a single-bus topology with master only and no effects, and routes
    // every voice there. To use ducking / submixes / sidechain, define a
    // multi-bus graph here.
    BusGraphConfig busGraph;
};

} // namespace audio

#endif // AUDIO_ENGINE_CONFIG_H
