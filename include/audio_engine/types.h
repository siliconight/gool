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

// audio_engine/types.h
//
// Core value types shared across the runtime. Kept dependency-free so it sits
// at the bottom of the include graph.

#ifndef AUDIO_ENGINE_TYPES_H
#define AUDIO_ENGINE_TYPES_H

#include <cstdint>
#include <string_view>

namespace audio {

// ---- Vec3 -----------------------------------------------------------------
// Plain-data 3-vector. The runtime does not impose a coordinate system; the
// host's convention (Y-up vs Z-up, left-handed vs right-handed) flows through
// transparently. Listener.forward and listener.up disambiguate orientation.
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float xv, float yv, float zv) : x(xv), y(yv), z(zv) {}
};

// ---- Strong runtime IDs ---------------------------------------------------
// Compact integer IDs for hot-path identification. Strings live in tools and
// asset registries; runtime systems resolve to these compact forms before
// doing per-frame work.
using AudioSoundId        = uint32_t;
using AudioActorId        = uint64_t;
using AudioPlayerId       = uint64_t;
using AudioZoneId         = uint32_t;
using AudioReverbPresetId = uint32_t;
using AudioVoiceProfileId = uint32_t;
using AudioParameterId    = uint32_t;
using AudioSequenceId     = uint32_t;
using AudioBusId          = uint8_t;
using SimulationTick      = uint32_t;
using TimestampMs         = uint64_t;

constexpr AudioSoundId  kInvalidSoundId  = 0;
constexpr AudioActorId  kInvalidActorId  = 0;
constexpr AudioPlayerId kInvalidPlayerId = 0;
constexpr AudioZoneId   kInvalidZoneId   = 0;

// ---- Categories and policies ---------------------------------------------
enum class AudioCategory : uint16_t {
    SFX       = 0,
    Voice     = 1,
    Music     = 2,
    Ambience  = 3,
    UI        = 4,
    Dialogue  = 5,
    Count
};

// Higher numeric value = higher priority. Survives culling when budgets
// tighten.
enum class AudioPriority : uint8_t {
    Lowest   = 0,
    Low      = 64,
    Normal   = 128,
    High     = 192,
    Critical = 255,
};

// Trust label for SubmitReplicatedEvent. The runtime uses this to
// enforce policy on sensitive replication modes — e.g. a client cannot
// submit a ServerAuthoritative event and have remote listeners hear it.
//
// `Server`: the event was authored by the host's own (trusted) server
// logic. The host calls SubmitReplicatedEvent for its own authoritative
// state changes, and the call carries this label.
//
// `Client`: the event arrived over the wire from a remote peer. The
// host has authenticated the peer at the network layer, but the
// payload itself is still untrusted. This is the form to use when
// forwarding inbound RPCs into the runtime.
//
// `Unknown`: legacy / unspecified. The single-arg SubmitReplicatedEvent
// overload uses this label; the runtime treats it permissively for
// backward compatibility but does not enforce policy on it. New code
// should use the explicit two-arg form.
enum class ReplicationSource : uint8_t {
    Server  = 0,
    Client  = 1,
    Unknown = 2,
};

// v0.18.0 — Delivery class for replicated audio events. Mirrors the
// four-class taxonomy from the Tribes networking paper at gool's
// API surface, so hosts that use any modern reliable / unreliable
// channel split (ENet, GameNetworkingSockets, KCP, Steam Datagram
// Relay, custom UDP) can pass through the delivery semantics their
// transport already establishes. The runtime consults this enum
// when applying its own late-event discard policy on the control
// thread.
//
// Two values in v0.18.0; a third (`LowLatency`, for the SFX immediate
// path) is reserved for v0.19.0 Tier-B once the immediate-event
// ring lands. Adding an enumerator is non-breaking; existing callers
// that switch on the enum will keep compiling unless they explicitly
// require exhaustive coverage.
enum class EventDelivery : uint8_t {
    // Drop if late. The runtime applies its standard late-event
    // discard (per-event `maxStalenessMs` falling back to
    // `AudioConfig::lateEventDiscardMs`) to events in this class.
    // Appropriate for time-sensitive SFX where a stale trigger is
    // worse than silence: gunshots, footsteps, ambient effects.
    // This is the default for backward compatibility with
    // pre-v0.18.0 SubmitReplicatedEvent calls — every call site
    // that doesn't explicitly pass a delivery class gets Drop, which
    // matches the runtime's behavior before the enum existed.
    Drop       = 0,

    // Process even if late. The runtime bypasses late-event discard
    // for events in this class; the host has presumably already done
    // the reliability work at the transport layer (retransmit until
    // delivered) and we trust the resulting ordering. Appropriate
    // for music transitions, bus-graph hot-swaps, "player joined
    // voice chat" coordination, mute-state changes — anything that
    // must stick even if it arrives a few ticks late. A high counter
    // of `eventsAcceptedGuaranteedLate` in Stats indicates either
    // the host's reliable transport is slow, or events are being
    // misclassified (something marked Guaranteed that should be Drop).
    Guaranteed = 1,

    // v0.19.0 Tier-B: sub-tick latency. Events in this class go
    // through `SubmitImmediateEvent`, land in a small (8-entry)
    // ring, and drain at the top of `Update()` before Phase 1 —
    // bypassing the per-player/per-category rate limiter and the
    // late-event discard policy. Use for time-critical SFX where
    // the player's perception of the gameplay breaks if the sound
    // arrives a tick late: hit confirmations, melee-impact frames,
    // weapon-readiness chirps.
    //
    // The 8-entry ring is the natural rate limit — Tribes' "8
    // moves per packet" applied to the audio analog. Hosts that
    // try to push more than 8 immediate events between two
    // Update() ticks get `AudioResult::QueueFull` for the
    // overflow; well-behaved hosts catch this and fall back to
    // the regular Drop-class path. Telemetry:
    // `Stats::eventsImmediateProcessed` and
    // `Stats::eventsImmediateRejected` for the bucket and the
    // overflow respectively.
    LowLatency = 2,
};

// v0.18.0 — Subfield-level state mask for UpdateReplicatedTransform.
// Tribes' Ghost Manager tracks one bit per chunk of independent state;
// gool applies the same idea at sub-field granularity for the three
// transform components. Hosts that update only one component (a
// rotating turret whose position doesn't change, a sliding crate
// whose forward stays constant) can pass a mask covering only the
// dirty fields and save the bandwidth + interpolator work of the
// untouched ones. Combinable via bitwise OR.
enum class TransformStateMask : uint8_t {
    None     = 0,
    Position = 1 << 0,
    Forward  = 1 << 1,
    Velocity = 1 << 2,

    // Convenience: all three. The pre-v0.18.0 four-component
    // UpdateReplicatedTransform overload internally passes this.
    All      = Position | Forward | Velocity,
};

// Bitwise operators so callers can `Mask::Position | Mask::Forward`
// naturally. `constexpr` so the enum is usable in non-type template
// contexts (the runtime keeps a few constexpr lookup tables that
// want the operations available at compile time).
constexpr TransformStateMask operator|(TransformStateMask a, TransformStateMask b) noexcept {
    return static_cast<TransformStateMask>(
        static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
constexpr TransformStateMask operator&(TransformStateMask a, TransformStateMask b) noexcept {
    return static_cast<TransformStateMask>(
        static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
constexpr bool operator!=(TransformStateMask a, int b) noexcept {
    return static_cast<uint8_t>(a) != b;
}

enum class FalloffModel : uint8_t {
    Linear,
    Logarithmic,
    InverseSquare,
    CustomCurve     // host supplies curve via SoundDefinition; not yet implemented
};

enum class AudioReplicationPolicy : uint8_t {
    LocalOnly,
    OwnerOnly,
    RemoteRelevant,
    Global,
    ServerAuthoritative,
    Predicted        // play unconditionally, do not reconcile
};

enum class AudioOutputMode : uint8_t {
    Mono,
    Stereo
    // Surround variants are a future addition.
};

// Recommended codec for production voice. The first scaffolding pass ships a
// pass-through stub; real Opus is a follow-up target.
enum class VoiceCodec : uint8_t {
    Stub,
    Opus
};

// ---- Well-known parameter IDs --------------------------------------------
// Runtime parameters are addressed by ID. Host games can extend with their
// own IDs starting at AudioParameterIds::HostBase. Engine reserves the
// low range.
namespace AudioParameterIds {
    constexpr AudioParameterId Gain          = 1;
    constexpr AudioParameterId Pitch         = 2;
    constexpr AudioParameterId LowPassAmount = 3;
    constexpr AudioParameterId ReverbSend    = 4;
    constexpr AudioParameterId VehicleRpm    = 5;
    constexpr AudioParameterId HostBase      = 1024;
}

constexpr AudioParameterId kInvalidParameterId = 0;

// FNV-1a hash of a parameter name into an AudioParameterId. Same
// construction as HashSoundName so behavior is consistent. constexpr
// so host code can hash compile-time-known names with zero runtime
// cost ("health", "fatigue", "cave_dampness", etc.).
//
// Hashes that collide with the engine-reserved IDs (1..1023) are
// remapped above HostBase to keep host-defined names from masking
// engine semantics. Host games should treat parameter names with the
// same "no underscores at the start, ASCII only" discipline they use
// for sound names.
constexpr AudioParameterId HashParameterName(std::string_view name) noexcept {
    uint32_t h = 2166136261u;
    for (char c : name) {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    // Reserve 0 (kInvalidParameterId) and the engine-reserved range
    // [1, HostBase). Anything that lands there gets bumped above the
    // host base.
    if (h < AudioParameterIds::HostBase) {
        h += AudioParameterIds::HostBase;
    }
    return h;
}

// ---- RTPC binding types --------------------------------------------------

// Per-voice parameter targeted by an RTPC binding. The runtime maps each
// value to the matching well-known AudioParameterId on the internal
// parameter smoother (see EvaluateRtpcBindings_ in the runtime).
//
//   * Volume:        multiplicative on sp.gain. Identity = 1.0.
//   * Pitch:         multiplicative on sp.pitch. Identity = 1.0.
//                    1.0 = unchanged; 2.0 = one octave up; 0.5 = one octave down.
//   * LowPassCutoff: combined as max(spatial_baseline, rtpc) so RTPC can
//                    add filtering on top of occlusion / air absorption,
//                    but never reduce filtering applied by the world.
//                    Range [0, 1]; 0 = no filter, 1 = fully muffled.
//   * ReverbSend:    combined as min(1, spatial_baseline + rtpc) so
//                    RTPC adds wetness atop the global send. Range [0, 1].
enum class RtpcTarget : uint8_t {
    Volume        = 0,
    Pitch         = 1,
    LowPassCutoff = 2,
    ReverbSend    = 3,
};

constexpr size_t kRtpcTargetCount = 4;

// Shape function applied between input remap and output remap.
//   * Linear:             y = t
//   * Exponential:        y = pow(t, exponent)         — convex, accelerates late
//   * InverseExponential: y = 1 - pow(1 - t, exponent) — concave, accelerates early
//   * SCurve:             y = t*t*(3 - 2t)             — sigmoidal smoothstep
//
// `curveExponent` is consulted only for Exponential / InverseExponential;
// 2.0 is a reasonable default.
enum class RtpcCurve : uint8_t {
    Linear             = 0,
    Exponential        = 1,
    InverseExponential = 2,
    SCurve             = 3,
};

// One binding entry. Multiple bindings can attach to the same sound,
// but at most one per (target). See AudioRuntime::SetSoundRtpc.
struct SoundRtpcBinding {
    AudioParameterId paramId       = kInvalidParameterId;
    RtpcTarget       target        = RtpcTarget::Volume;
    RtpcCurve        curve         = RtpcCurve::Linear;
    float            curveExponent = 2.0f;

    // Input remap: parameter values in [minValue, maxValue] map linearly
    // to [0, 1] before the curve. Out-of-range values clamp.
    float            minValue      = 0.0f;
    float            maxValue      = 1.0f;

    // Output remap: curve output (always in [0, 1]) maps linearly to
    // [minOutput, maxOutput]. minOutput > maxOutput is allowed (inverted).
    float            minOutput     = 0.0f;
    float            maxOutput     = 1.0f;

    // Smoothing on the resulting per-voice parameter change. 0 = snap.
    float            smoothingMs   = 50.0f;
};

} // namespace audio

#endif // AUDIO_ENGINE_TYPES_H
