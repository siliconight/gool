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

// audio_engine/runtime/memory_budget.cpp
//
// See include/audio_engine/memory_budget.h for the rationale and
// scope of this estimate. Pure-function impl — no allocation, no I/O.

#include "audio_engine/memory_budget.h"

#include "audio_engine/config.h"

#include <cstdint>

namespace audio {

namespace {

// Conservative per-voice MixVoice size estimate. The real struct
// varies with optional voice features (binaural-LPF state, fade
// counters, etc.) — we use a generous 512 bytes which over-counts
// for typical voices and under-counts for the heaviest. Within the
// stated ±50% accuracy of this estimator.
constexpr uint64_t kMixVoiceApproxBytes = 512;

// Per-ear binaural delay buffer. Mirrors audio_mixer.cpp's
// kBinauralDelayCapacity (192 frames per ear, two ears, float).
constexpr uint64_t kBinauralDelayBytesPerVoice =
    192ull * 2ull * sizeof(float);

// Typical bus graph for game audio: ~16 buses (master + reverb +
// SFX/voice/music/UI categories + a couple of submixes). The real
// graph is supplied as BusConfig at Initialize; this estimator
// can't see it, so we use a reasonable default. Off by a small
// factor for games with more buses; either way, this dominates only
// at large bufferSize.
constexpr uint64_t kTypicalBusCount = 16;

// Asset registry has 3 hash maps keyed by AudioSoundId, all reserved
// to maxRegisteredSounds. Per-bucket overhead in libstdc++/libc++ is
// typically ~16-32 bytes (linked-list-of-buckets approach).
constexpr uint64_t kHashBucketBytes = 64;

} // namespace

uint64_t EstimateBaselineBytes(const AudioConfig& cfg) noexcept {
    const uint64_t kFloat = sizeof(float);
    const uint64_t channels =
        (cfg.outputMode == AudioOutputMode::Stereo) ? 2ull : 1ull;

    // --- Bus graph buffers: 16 typical buses × (input + output) × frames × channels.
    const uint64_t busBufferBytes =
        kTypicalBusCount * 2ull * cfg.bufferSize * channels * kFloat;

    // --- Voice mix pool: (maxActiveEmitters + 1) voices with per-voice state + delay lines.
    const uint64_t voicePoolBytes =
        (static_cast<uint64_t>(cfg.budget.maxActiveEmitters) + 1ull)
        * (kMixVoiceApproxBytes + kBinauralDelayBytesPerVoice);

    // --- Voice source rings: per-source PCM ring + per-source packet ring.
    // PCM ring is float-typed (voicePcmRingFrames floats); packet ring is
    // voicePacketRingDepth × voiceMaxPacketBytes.
    const uint64_t voiceRingBytes = cfg.enableVoice ?
        static_cast<uint64_t>(cfg.budget.maxVoiceSources)
        * (static_cast<uint64_t>(cfg.voicePcmRingFrames) * kFloat
         + static_cast<uint64_t>(cfg.voicePacketRingDepth)
           * static_cast<uint64_t>(cfg.voiceMaxPacketBytes))
        : 0ull;

    // --- Streaming asset rings: one float ring per streaming asset.
    // streamingRingFrames × channels × float.
    const uint64_t streamingBytes =
        static_cast<uint64_t>(cfg.budget.maxStreamingAssets)
        * cfg.streamingRingFrames * channels * kFloat;

    // --- Asset registry hash buckets (3 maps × maxRegisteredSounds).
    const uint64_t registryBytes =
        static_cast<uint64_t>(cfg.budget.maxRegisteredSounds)
        * 3ull * kHashBucketBytes;

    return busBufferBytes
         + voicePoolBytes
         + voiceRingBytes
         + streamingBytes
         + registryBytes;
}

} // namespace audio
