// audio_engine/assets/audio_asset_registry.h
//
// Concrete registry for pre-loaded PCM assets and streaming-asset
// metadata. Once registered, asset PCM data lives until runtime
// shutdown; pointers handed to the mixer stay valid for the runtime's
// lifetime. This is a deliberate simplification; it removes a lifetime
// hazard from the render path at the cost of pinning all decoded sound
// bytes in memory. Streaming assets follow the same pinning rule for
// their state object; the producer-side decode buffers are owned by the
// streaming voice and ring-bounded, not stored here.

#ifndef AUDIO_ENGINE_ASSETS_AUDIO_ASSET_REGISTRY_H
#define AUDIO_ENGINE_ASSETS_AUDIO_ASSET_REGISTRY_H

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio_engine/audio_file_format.h"
#include "audio_engine/types.h"
#include "audio_engine/handles.h"
#include "audio_engine/result.h"
#include "audio_engine/emitter.h"
#include "audio_engine/decoders/audio_decoder.h"
#include "audio_engine/util/pcm_ring_f32.h"

namespace audio {

// Decoded PCM held resident for the lifetime of the runtime.
struct PcmAsset {
    std::vector<float> samples;     // interleaved
    uint32_t           sampleRate = 0;
    uint32_t           channels   = 0;
    uint32_t           frames     = 0;
};

// Streaming asset; decoder + float ring. The decoder is opened once at
// registration and kept open for the registry's lifetime. Each streaming
// asset supports at most ONE concurrent playback instance: that is the right
// shape for music and ambience tracks (the actual use case for streaming),
// and it removes the need for a separate streaming-voice pool.
//
// State machine:
//   Idle     -- not currently playing; ring may hold residual data
//   Pumping  -- a voice is playing this asset; control thread tops up ring
//   Draining -- decoder has hit EOF and not looping; ring still draining
//
// A returning play call seeks the decoder to 0 and resets the ring.
struct StreamingAsset {
    enum class State : uint8_t { Idle, Pumping, Draining };

    std::unique_ptr<IAudioDecoder>   decoder;
    std::unique_ptr<util::PcmRingF32> ring;
    uint32_t channels   = 0;
    uint32_t sampleRate = 0;     // == engine sample rate (decoder is wrapped in resampler if needed)
    uint64_t totalFrames = 0;    // 0 if unknown
    bool     looping     = false;
    State    state       = State::Idle;
    uint32_t playingMixSlot = 0; // valid when state != Idle
};

class AudioAssetRegistry {
public:
    explicit AudioAssetRegistry(uint32_t maxRegisteredSounds);

    // Sound metadata (priority, attenuation defaults). Idempotent on the
    // same id; later registrations overwrite.
    AudioResult RegisterDefinition(const SoundDefinition& def);

    // Pre-load PCM bytes for a soundId. Decoded float samples are copied into
    // the registry. Caller releases its buffer after this returns.
    AudioResult RegisterPcm(AudioSoundId id,
                             std::span<const float> samples,
                             uint32_t sampleRate,
                             uint32_t channels);

    // Decoded-file registration. The file is decoded fully and stored as a
    // pinned PcmAsset, resampled to engineSampleRate if needed and
    // downmixed to maxChannels (1 or 2). Returns Unsupported if no decoder
    // is compiled in for the file's format.
    AudioResult RegisterDecodedFromFile(AudioSoundId id,
                                          const char*  path,
                                          uint32_t     engineSampleRate,
                                          uint32_t     maxChannels);

    AudioResult RegisterDecodedFromMemory(AudioSoundId             id,
                                            std::span<const uint8_t> bytes,
                                            AudioFileFormat          formatHint,
                                            uint32_t                 engineSampleRate,
                                            uint32_t                 maxChannels);

    // Streaming registration. The decoder is opened, wrapped in a resampler
    // if needed, and held for the registry lifetime. The per-asset ring is
    // sized by ringFrames frames at decoder.channels channels.
    AudioResult RegisterStreamingFromFile(AudioSoundId id,
                                            const char*  path,
                                            uint32_t     engineSampleRate,
                                            uint32_t     maxChannels,
                                            uint32_t     ringFrames);

    // Streaming registration over an in-memory pre-decoded buffer. The
    // registry takes ownership of `samples` and constructs a
    // MemoryPcmDecoder over it. Useful for tests, synthesized streaming
    // examples, and any host that decodes PCM elsewhere and wants to feed
    // it through the streaming voice path. Resampled to engineSampleRate
    // and downmixed to maxChannels at decode time as usual.
    AudioResult RegisterStreamingFromMemory(AudioSoundId        id,
                                              std::vector<float>&& samples,
                                              uint32_t             srcSampleRate,
                                              uint32_t             srcChannels,
                                              uint32_t             engineSampleRate,
                                              uint32_t             maxChannels,
                                              uint32_t             ringFrames);

    const SoundDefinition* GetDefinition(AudioSoundId id) const noexcept;

    // Returns a stable pointer valid until Shutdown. Render thread reads
    // through this; do not free or relocate.
    const PcmAsset* GetAsset(AudioSoundId id) const noexcept;

    // Streaming asset accessor. Returns nullptr if id is not a registered
    // streaming asset. Pointers are stable for the registry's lifetime.
    StreamingAsset*       GetStreamingAsset(AudioSoundId id) noexcept;
    const StreamingAsset* GetStreamingAsset(AudioSoundId id) const noexcept;

    // Iterate every registered streaming asset (for the control-thread pump).
    template <typename F>
    void ForEachStreamingAsset(F&& fn) {
        for (auto& [id, asset] : streams_) fn(id, asset);
    }

    uint32_t Capacity() const noexcept { return capacity_; }
    uint32_t Count()    const noexcept;

private:
    uint32_t capacity_;

    // Both maps keyed by AudioSoundId. unordered_map allocates on insert,
    // but registration is on the game thread during setup; never on the hot
    // path.
    std::unordered_map<AudioSoundId, SoundDefinition> defs_;
    std::unordered_map<AudioSoundId, PcmAsset>        pcms_;
    std::unordered_map<AudioSoundId, StreamingAsset>  streams_;
};

} // namespace audio

#endif // AUDIO_ENGINE_ASSETS_AUDIO_ASSET_REGISTRY_H
