// audio_engine/bus.h
//
// Bus graph and DSP effect configuration. The host defines a tree of named
// buses at Initialize() time; buses route signal up the tree to the master
// bus. Effects live in a per-bus chain; an effect on bus A can declare a
// sidechain reference to bus B, which constrains the render thread to
// process B before A within the same callback.
//
// Ducking is the canonical use case: place a Compressor on Music with
// sidechainBus = LocalGun. When the player fires their weapon, energy on
// LocalGun drives the compressor's envelope and ducks Music.
//
// All bus and effect configuration is consumed once at Initialize() and is
// immutable after that. Parameter values can be updated at runtime through
// AudioRuntime::SetBusGainDb() and AudioRuntime::SetEffectParameter().

#ifndef AUDIO_ENGINE_BUS_H
#define AUDIO_ENGINE_BUS_H

#include <cstdint>
#include "audio_engine/types.h"

namespace audio {

// ---- Identifiers ----------------------------------------------------------

using BusId = uint16_t;

constexpr BusId kBusMaster      = 0;     // always exists; sink for everything
// Conventional ID for the reverb send target. The mixer routes a fraction of
// each voice's signal here when SpatialParams.reverbSend > 0. The host opts
// in by adding a bus with this id (typically with a single ReverbEffect on
// it). If no bus has this id, sends are silently no-op.
constexpr BusId kBusReverb      = 1;
constexpr BusId kInvalidBusId   = 0xFFFF;

constexpr uint32_t kMaxBuses              = 32;
constexpr uint32_t kMaxEffectsPerBus      = 8;
constexpr uint32_t kMaxEffectsTotal       = kMaxBuses * kMaxEffectsPerBus;

// ---- Effect configuration -------------------------------------------------

enum class EffectKind : uint8_t {
    None        = 0,
    Gain,           // linear gain (smoothed internally on parameter change)
    BiquadFilter,   // LPF / HPF / BPF
    Compressor,     // envelope-driven gain reduction; supports sidechain
    Reverb,         // Schroeder/Freeverb-style algorithmic reverb
};

enum class BiquadType : uint8_t {
    LowPass   = 0,
    HighPass,
    BandPass,
    // Tone-shaping filters. cutoff is the corner / transition / center
    // frequency; Q controls bandwidth (peak) or transition steepness
    // (shelf); gainDb is the boost (positive) or cut (negative) at
    // the relevant frequency band. These types ignore Q for shelves
    // and use it as bandwidth for peak.
    LowShelf,        // boost/cut frequencies below cutoff
    HighShelf,       // boost/cut frequencies above cutoff
    Peak,            // boost/cut a band centered at cutoff
};

// Tagged-struct effect descriptor. The `kind` field selects which named
// parameters are read; unused parameters are ignored. Designed to be a flat
// POD so AudioConfig can carry an array of these by value.
struct EffectConfig {
    EffectKind kind = EffectKind::None;

    // Gain
    float gainDb = 0.0f;

    // Biquad filter
    BiquadType biquadType = BiquadType::LowPass;
    float      biquadCutoffHz = 20000.0f;
    float      biquadQ        = 0.707f;     // butterworth
    float      biquadGainDb   = 0.0f;       // honored by LowShelf/HighShelf/Peak only

    // Compressor
    float compressorThresholdDb  = -20.0f;
    float compressorRatio        = 4.0f;
    float compressorAttackMs     = 10.0f;
    float compressorReleaseMs    = 200.0f;
    float compressorMakeupDb     = 0.0f;
    BusId compressorSidechainBus = kInvalidBusId;  // self-sidechain when invalid

    // Reverb (Schroeder/Freeverb-derived)
    //   roomSize  0..1; feedback gain → tail length
    //   damping   0..1; high-frequency rolloff in feedback path
    //   wetGainDb    ; gain applied to the wet output of this effect.
    //                   The reverb bus sums into master at its configured
    //                   outputGainDb; wetGainDb is the effect-internal mix
    //                   level (default 0 = unity).
    float reverbRoomSize  = 0.7f;
    float reverbDamping   = 0.5f;
    float reverbWetGainDb = 0.0f;
};

// Parameter IDs accepted by SetEffectParameter. Not every effect honors
// every ID; mismatched IDs are ignored on the render thread.
namespace EffectParameter {
    constexpr uint16_t Gain_GainDb              = 1;
    constexpr uint16_t Biquad_CutoffHz          = 2;
    constexpr uint16_t Biquad_Q                 = 3;
    constexpr uint16_t Compressor_ThresholdDb   = 4;
    constexpr uint16_t Compressor_Ratio         = 5;
    constexpr uint16_t Compressor_AttackMs      = 6;
    constexpr uint16_t Compressor_ReleaseMs     = 7;
    constexpr uint16_t Compressor_MakeupDb      = 8;
    constexpr uint16_t Reverb_RoomSize          = 9;
    constexpr uint16_t Reverb_Damping           = 10;
    constexpr uint16_t Reverb_WetGainDb         = 11;
    constexpr uint16_t Biquad_GainDb            = 12;
}

// ---- Bus configuration ----------------------------------------------------

// Distance-driven gain factor applied to voices accumulating into this bus.
// Used to implement the "RemoteGunNearby" pattern from the ducking TDD: a
// silent send-only bus whose level scales with listener proximity, driving
// a sidechain compressor while contributing nothing to the audible mix.
struct ProximityCurve {
    bool  enabled        = false;
    float fullDistance   = 5.0f;    // <= this distance: full gain
    float falloffDistance = 30.0f;  // >= this distance: zero gain
    float curveExponent  = 1.5f;    // >1 = aggressive falloff near max range
};

struct BusConfig {
    BusId       id             = kInvalidBusId;
    BusId       parent         = kBusMaster;     // routes here; ignored for master
    float       outputGainDb   = 0.0f;
    bool        silent         = false;          // true: output not summed into parent (sidechain-source-only)

    // Optional name for diagnostics. Not used for routing.
    char        debugName[16]  = {0};

    ProximityCurve proximityCurve;

    // Effect chain (in order). Effects beyond effectCount are ignored.
    EffectConfig effects[kMaxEffectsPerBus];
    uint32_t     effectCount   = 0;
};

// Map from AudioCategory to a default target BusId. Used when an emitter
// doesn't override its target bus explicitly. A category mapped to
// kInvalidBusId routes that category to master.
struct CategoryBusMap {
    BusId music     = kBusMaster;
    BusId voice     = kBusMaster;
    BusId sfx       = kBusMaster;
    BusId ambience  = kBusMaster;
    BusId ui        = kBusMaster;
    BusId dialogue  = kBusMaster;
};

// Top-level bus graph configuration. Embedded in AudioConfig.
struct BusGraphConfig {
    // Buses, in any order. Master must always exist (id == kBusMaster). If
    // busCount == 0 the runtime auto-creates a single-bus graph (master only,
    // no effects) and routes every voice there.
    BusConfig buses[kMaxBuses];
    uint32_t  busCount = 0;

    CategoryBusMap categoryMap;
};

} // namespace audio

#endif // AUDIO_ENGINE_BUS_H
