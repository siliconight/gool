// audio_engine/geometry_query.h
//
// Geometry query seam. The audio engine does not own physics; the host
// supplies an implementation that delegates to its physics system. One
// of the four real polymorphism seams. NullGeometryQuery is the default
// fallback and always reports "no hit".
//
// Calls happen on the audio control thread (never the render thread) and
// are budgeted per Update tick.

#ifndef AUDIO_ENGINE_GEOMETRY_QUERY_H
#define AUDIO_ENGINE_GEOMETRY_QUERY_H

#include "audio_engine/types.h"
#include <algorithm>
#include <cmath>

namespace audio {

// Coarse material classification. The host returns one of these from a
// raycast hit (typically by mapping its own material/surface tags) and
// the engine applies sensible defaults for both absorption (overall
// gain reduction) and damping (high-frequency rolloff). Hosts that want
// finer control can ignore this and write `absorption` / `damping`
// directly on AudioOcclusionHit.
enum class AudioMaterial : uint8_t {
    Default     = 0,    // unknown; treat as a moderately damping wood-like surface
    Air         = 1,    // pass-through; same as no hit
    Glass       = 2,    // mostly transparent, slight HF cut
    Wood        = 3,
    Drywall     = 4,
    Concrete    = 5,    // strong both
    Metal       = 6,    // strong absorption, less damping (rings more)
    Curtain     = 7,    // strong damping, modest absorption
    Foliage     = 8,    // diffuse, mostly damping
    Meat        = 9,    // soft, dense, wet — bodies, creatures, fleshy props
};

// Number of values in AudioMaterial. Used to size per-material arrays
// (e.g., the sound bank's by_material group buckets). Update whenever
// AudioMaterial gains a new value.
inline constexpr uint8_t kAudioMaterialCount = 10;

struct AudioOcclusionHit {
    bool  hit                 = false;
    Vec3  hitPoint{};
    float distance            = 0.0f;

    // Decoupled occlusion components. The legacy `materialAbsorption`
    // field below is mapped onto both at the spatializer when `damping`
    // is left at its sentinel of -1.0; new code should set both
    // explicitly (or set `material` and let the engine fill them in).
    //
    //   absorption  0..1  overall gain reduction (volume cue)
    //   damping     0..1  contribution to the per-voice LPF (HF cue)
    //
    // A curtain has high damping but modest absorption (the highs die
    // but you still hear the level). Concrete has both. Glass has
    // neither, much. Modeling these independently is what lets a
    // designer tune "soft" vs "hard" surfaces.
    float absorption          = 0.0f;
    float damping             = -1.0f;   // -1 = inherit from absorption

    // Material preset, optional. If `material != Default`, the
    // spatializer maps it to absorption + damping defaults below
    // (overriding any explicit values). Hosts choose either path.
    AudioMaterial material    = AudioMaterial::Default;

    // Legacy field. Hosts that still set only this leave the new fields
    // at defaults; the spatializer treats `materialAbsorption` as
    // setting both `absorption` and `damping` to that value, preserving
    // pre-existing behavior bit-for-bit.
    float materialAbsorption  = 0.0f;
};

// Map an AudioMaterial preset to (absorption, damping) defaults. The
// values are deliberately conservative: a host that wants more
// aggressive occlusion can scale them in their own raycast wrapper.
inline void AudioMaterialDefaults(AudioMaterial m,
                                    float& absorption,
                                    float& damping) noexcept {
    switch (m) {
        case AudioMaterial::Air:      absorption = 0.00f; damping = 0.00f; break;
        case AudioMaterial::Glass:    absorption = 0.10f; damping = 0.05f; break;
        case AudioMaterial::Wood:     absorption = 0.50f; damping = 0.40f; break;
        case AudioMaterial::Drywall:  absorption = 0.55f; damping = 0.55f; break;
        case AudioMaterial::Concrete: absorption = 0.90f; damping = 0.75f; break;
        case AudioMaterial::Metal:    absorption = 0.70f; damping = 0.30f; break;
        case AudioMaterial::Curtain:  absorption = 0.30f; damping = 0.80f; break;
        case AudioMaterial::Foliage:  absorption = 0.35f; damping = 0.60f; break;
        case AudioMaterial::Meat:     absorption = 0.65f; damping = 0.85f; break;
        case AudioMaterial::Default:
        default:                      absorption = 0.50f; damping = 0.50f; break;
    }
}

// v0.29.0: Recommended (decay, lf_damping, hf_damping, diffusion)
// preset for a Dattorro plate reverb tuned to feel like a small-to-medium
// room finished with the given material. These are the values that
// `Gool.reverb_preset_for_material()` will return once Phase 5.1 ships
// the GDScript wrapper; in the meantime they're available to anyone
// constructing a ReverbEffect programmatically.
//
// The values come from docs/audio_design/reverb_dattorro.md. They are
// starting points, not authoritative — designers are expected to
// override per-zone for specific spaces.
struct ReverbMaterialPreset {
    float decay;
    float lfDamping;
    float hfDamping;
    float diffusion;
};

inline ReverbMaterialPreset ReverbPresetByMaterial(AudioMaterial m) noexcept {
    switch (m) {
        case AudioMaterial::Glass:    return { 0.85f, 0.00f, 0.05f, 0.50f };
        case AudioMaterial::Wood:     return { 0.55f, 0.10f, 0.40f, 0.70f };
        case AudioMaterial::Drywall:  return { 0.45f, 0.20f, 0.55f, 0.70f };
        case AudioMaterial::Concrete: return { 0.85f, 0.05f, 0.15f, 0.55f };
        case AudioMaterial::Metal:    return { 0.80f, 0.00f, 0.10f, 0.40f };
        case AudioMaterial::Curtain:  return { 0.20f, 0.70f, 0.85f, 0.85f };
        case AudioMaterial::Foliage:  return { 0.30f, 0.40f, 0.85f, 0.95f };
        case AudioMaterial::Meat:     return { 0.10f, 0.60f, 0.95f, 0.85f };
        // Air and Default both use a balanced "average room" preset.
        case AudioMaterial::Air:
        case AudioMaterial::Default:
        default:                      return { 0.50f, 0.10f, 0.30f, 0.625f };
    }
}

// v0.33.0 (Phase 6.A): per-material EQ curves.
//
// Each material carries a perceptual fingerprint — the frequency
// contour that makes "concrete" sound like concrete and "wood"
// sound like wood, beyond the gain reduction (occlusion) and
// reverb tail (acoustic envelope) already covered in Phases 5.2
// and 5.3. Phase 6.A defines those fingerprints as a 3-band EQ
// curve per material; subsequent phases will wire them into:
//
//   6.B  the impact playback path — a sound played as an
//        impact on Concrete gets the Concrete EQ applied at
//        play time, even if the source .wav was authored
//        material-neutral
//   6.C  the reverb-zone path — the listener's current space
//        colors what they hear (their ears are "inside" the
//        material), via the same curves applied to a listener-
//        space EQ bus
//
// The 3-band shape is deliberately small: a low shelf (boost or
// cut everything below a knee), a peaking band (boost or cut a
// region around a center frequency with a configurable Q), and
// a high shelf (boost or cut above a knee). That's enough to
// capture the perceptual character of every material in the
// AudioMaterial enum without becoming an authoring nightmare.
// Designers needing finer control can author multi-Biquad chains
// on a bus directly via JSON or set_effect_parameter.
//
// Sign conventions: positive gains are boosts in dB, negative are
// cuts. Q values around 0.7 are gentle, 1.0 is moderate, 1.5+ is
// surgical. Frequencies in Hz.
//
// The presets here are STARTING POINTS designed to be audible but
// not extreme. Concrete's upper-mid bite is real but not piercing;
// foliage's broad cut is real but not telephone-y. Designers are
// expected to override per-context, especially for stylized games.

struct MaterialEqCurve {
    float lowGainDb;    // shelf gain below lowFreqHz
    float lowFreqHz;    // low shelf knee
    float midGainDb;    // peaking band gain around midFreqHz
    float midFreqHz;    // peaking band center
    float midQ;         // peaking band Q (sharpness)
    float highGainDb;   // shelf gain above highFreqHz
    float highFreqHz;   // high shelf knee
};

inline MaterialEqCurve MaterialEqByMaterial(AudioMaterial m) noexcept {
    switch (m) {
        // Glass: bright, neutral mids. Slight HF lift gives the
        // characteristic ring; small mid dip keeps it from feeling
        // boxy.
        case AudioMaterial::Glass:
            return { 0.0f,  200.0f,  -1.5f, 1000.0f, 1.0f,  +1.0f, 8000.0f };

        // Wood: warm low-mid body, soft top. Gentle low shelf
        // boost + peaking band at 500 Hz captures the "thwack" of
        // wood; HF cut removes the brittle edge that synthesized
        // wood often has.
        case AudioMaterial::Wood:
            return { +2.0f, 250.0f,  +1.5f,  500.0f, 0.7f,  -1.5f, 6000.0f };

        // Drywall: neutral, slightly damped. Subtle cuts at mid
        // and high keep things flat and dulled — the "indoor
        // residential" feel.
        case AudioMaterial::Drywall:
            return { 0.0f,  200.0f,  -1.0f, 1000.0f, 0.7f,  -1.0f, 8000.0f };

        // Concrete: bright, hard, upper-mid "crack". Slight low
        // shelf boost for body; the signature 1.5 kHz peaking
        // band gives concrete its bite; HF lift adds the
        // hardness that distinguishes it from drywall.
        case AudioMaterial::Concrete:
            return { +1.0f, 200.0f,  +2.5f, 1500.0f, 1.0f,  +2.0f, 6000.0f };

        // Metal: hard, ringing, slightly nasal. Upper-mid peak
        // captures the "clang"; bright HF gives the ringing
        // overtones. Tighter Q than concrete — metal's
        // resonance is more focused.
        case AudioMaterial::Metal:
            return { 0.0f,  200.0f,  +2.0f, 2000.0f, 1.5f,  +1.5f, 10000.0f };

        // Curtain: very dulled, soft top. Broad mid cut + strong
        // HF cut — the classic "thick fabric" sound. Asymmetric:
        // low end is untouched (heavy fabric doesn't attenuate
        // bass much).
        case AudioMaterial::Curtain:
            return { 0.0f,  200.0f,  -2.0f,  800.0f, 0.5f,  -4.0f, 4000.0f };

        // Foliage: broadband softness, no specific resonance.
        // Wide gentle cuts across mid + high. Lower Q than
        // curtain because foliage is more chaotic — many leaves
        // contribute slightly different absorption.
        case AudioMaterial::Foliage:
            return { 0.0f,  200.0f,  -1.5f, 1000.0f, 0.4f,  -2.0f, 6000.0f };

        // Meat: soft, dense, wet — body shots on creatures.
        // Low shelf boost gives the "thump" of dense soft tissue;
        // broad mid scoop because there's no resonant structure
        // (irregular fleshy shape, water content); strong HF cut
        // because wet surfaces absorb highs aggressively. Wider
        // Q than curtain because flesh damps more chaotically
        // than woven fabric.
        case AudioMaterial::Meat:
            return { +1.5f, 250.0f,  -1.0f,  800.0f, 0.5f,  -3.5f, 5000.0f };

        // Air and Default: no coloration. These return a
        // null curve (all gains 0 dB); any consumer applying
        // this should skip the EQ stage entirely.
        case AudioMaterial::Air:
        case AudioMaterial::Default:
        default:
            return { 0.0f,  200.0f,   0.0f, 1000.0f, 0.7f,   0.0f, 8000.0f };
    }
}

// Helper: does this curve represent any meaningful EQ at all?
// Used by consumers to skip the EQ stage entirely for
// Air/Default and to avoid setting up Biquad chains that would
// be 0 dB no-ops.
inline bool MaterialEqIsNeutral(const MaterialEqCurve& c) noexcept {
    constexpr float kEpsilonDb = 0.01f;
    return std::abs(c.lowGainDb)  < kEpsilonDb
        && std::abs(c.midGainDb)  < kEpsilonDb
        && std::abs(c.highGainDb) < kEpsilonDb;
}

// Resolve the effective (absorption, damping) pair from an
// AudioOcclusionHit, honoring the precedence:
//   1. material != Default     -> AudioMaterialDefaults(material)
//   2. damping >= 0            -> use explicit absorption + damping
//   3. otherwise (legacy)      -> absorption = damping = materialAbsorption
inline void ResolveOcclusion(const AudioOcclusionHit& h,
                                float& outAbsorption,
                                float& outDamping) noexcept {
    if (h.material != AudioMaterial::Default) {
        AudioMaterialDefaults(h.material, outAbsorption, outDamping);
        return;
    }
    if (h.damping >= 0.0f) {
        outAbsorption = std::clamp(h.absorption, 0.0f, 1.0f);
        outDamping    = std::clamp(h.damping,    0.0f, 1.0f);
        return;
    }
    const float v = std::clamp(h.materialAbsorption, 0.0f, 1.0f);
    outAbsorption = v;
    outDamping    = v;
}

class IAudioGeometryQuery {
public:
    virtual ~IAudioGeometryQuery() = default;

    // Synchronous raycast from `from` to `to`. Implementations must be
    // thread-safe with respect to the host's physics scene; the engine calls
    // this from the audio control thread.
    virtual bool RaycastAudioOcclusion(
        const Vec3&        from,
        const Vec3&        to,
        AudioOcclusionHit& outHit) noexcept = 0;
};

} // namespace audio

#endif // AUDIO_ENGINE_GEOMETRY_QUERY_H
