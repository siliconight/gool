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

} // namespace audio

#endif // AUDIO_ENGINE_TYPES_H
