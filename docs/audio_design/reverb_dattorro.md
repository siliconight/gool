# Reverb redesign: Dattorro plate

**Status:** Shipped in v0.29.0 (broke CI on a test ctor signature miss), rolled back in v0.29.1, restored in v0.29.2 with the test fix. Implementation: `src/audio_engine/dsp/reverb_effect.{h,cpp}`. This document remains the authoritative reference for the topology and parameter semantics; the implementation tracks it directly.

**Implementation note vs. the original design:** The damping shelves use stacked one-pole filters (HF lowpass + parallel low-cutoff lowpass with subtraction) rather than analytical Cytomic SVFs. The parameter surface is identical (`lf_damping` and `hf_damping`, both 0..1) and material distinctions are audibly preserved; a Cytomic SVF upgrade can land in v0.30.x if material shapes need cleaner shelf transitions.

**Goal:** Replace the current Freeverb implementation with a Dattorro plate
topology, giving the engine frequency-shaped damping (the load-bearing
capability for Phase 5.1's `AudioMaterial` taxonomy to perceptually
distinguish surfaces) and the musical/spatial quality required for Phase
5.3's per-source acoustic environments.

## Why not keep Freeverb

The current implementation (8 parallel combs + 4 series Schroeder allpasses
per channel, ~256 LOC in `src/audio_engine/dsp/reverb_effect.{h,cpp}`) is
working, stable, and 2000-era public-domain code. It earns its space for
prototype work but cannot deliver Phase 5's promise on three axes:

1. **No early reflections.** Freeverb is all tail. Listeners can't
   distinguish "small concrete bathroom" from "small drywall bathroom" —
   they only hear decay length, never the room.
2. **No predelay.** Predelay (3–80 ms between dry signal and reverb onset)
   is the strongest perceptual cue for room *size*. Currently size is
   modeled only by decay length, which is musically a much weaker handle.
3. **Single-pole damping.** A single damping coefficient per comb means a
   single "darkness" knob — `Glass`, `Concrete`, `Curtain`, `Foliage`
   cannot be perceptually distinguished without frequency-shaped damping.

A fourth issue, static delays causing metallic ringing on tonal material,
matters less for transient game audio but still shows up on synth pads
and held tones.

## References

- **Dattorro, *Effect Design Part 1: Reverberator and Other Filters* (1997).**
  Canonical paper. Topology and tuning constants below come directly from
  it. https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf
- **Spinsemi effects KB.** Hardware-oriented but the structural concepts
  (input diffusion, modulated allpasses, tap positioning) apply directly.
  http://www.spinsemi.com/knowledge_base/effects.html
- **Signalsmith Audio, *Let's Write a Reverb* (2021).** Modern FDN-style
  approach. Considered as Tier 3 below — out of scope for v0.29.0 but
  worth keeping in mind if reverb requirements grow past what Dattorro
  delivers. https://signalsmith-audio.co.uk/writing/2021/lets-write-a-reverb/
- **Cytomic / Andrew Simper, *SVF Input Mixing*.** Analytical state-variable
  filter form. Used here for the frequency-shaped damping inside the tank.
  https://cytomic.com/files/dsp/SvfInputMixing.pdf

## Topology

```
mono input ─→ [predelay buffer] ─→ [input diffuser] ─→ tank input
                                   ┌──────────────────┐
                                   │ AP1 (142, 0.75)  │
                                   │ AP2 (107, 0.75)  │
                                   │ AP3 (379, 0.625) │
                                   │ AP4 (277, 0.625) │
                                   └────────┬─────────┘
                                            │
        ┌───────────────────────────────────┴──────────────────────────────────┐
        │                                                                       │
        ▼                                                                       │
  ┌── tank half A ──────────────────────────┐  ┌── tank half B ─────────────────┴───┐
  │                                          │  │                                    │
  │   + ←──────── decay × ────────────────── │ ←┤   + ←──── decay × ─────────────── │ ← (close loop)
  │   │                                      │  │   │                                │
  │   ▼                                      │  │   ▼                                │
  │ modAP1 (672 ± mod, −0.7) ←── 0.5 Hz LFO  │  │ modAP3 (908 ± mod, −0.7) ←── LFO  │
  │   │                                      │  │   │                                │
  │   ▼                                      │  │   ▼                                │
  │ delay1 (4453)  ─── taps to L            │  │ delay3 (4217)  ─── taps to R       │
  │   │                                      │  │   │                                │
  │   ▼                                      │  │   ▼                                │
  │ SVF shelf (lf_damp, hf_damp)             │  │ SVF shelf (lf_damp, hf_damp)       │
  │   │                                      │  │   │                                │
  │   ▼                                      │  │   ▼                                │
  │ × decay                                  │  │ × decay                            │
  │   │                                      │  │   │                                │
  │   ▼                                      │  │   ▼                                │
  │ AP2 (1800, 0.5)                          │  │ AP4 (2656, 0.5)                    │
  │   │                                      │  │   │                                │
  │   ▼                                      │  │   ▼                                │
  │ delay2 (3720) ─── taps to L              │  │ delay4 (3163) ─── taps to R        │
  │   │                                      │  │   │                                │
  └───┼──────────────────────────────────────┘  └───┼────────────────────────────────┘
      │                                              │
      └─→ feeds half B input                         └─→ feeds half A input
                                  L output ←── 7 weighted taps        R output
```

Sample-count constants are Dattorro's original at 29761 Hz. Scaled to runtime
sample rate via the same `ScaleDelay` helper the current Freeverb uses.

**Modulation is internal-only.** LFOs at ~0.5 Hz, depth ~±8 samples.
Hardcoded rather than exposed — too small to hear at the right setting, too
big to sound natural at any other.

**Output taps are at specific positions inside the delays.** The published
"magic numbers" are what give Dattorro plate its distinctive stereo image
from mono input. Faithful port keeps them.

## C++ header skeleton

```cpp
class ReverbEffect final : public IDspEffect {
public:
    ReverbEffect(float predelayMs, float decay,
                 float lfDamping, float hfDamping,
                 float diffusion, float wetGainDb);

    void Prepare(uint32_t sampleRate, uint32_t channels) override;
    void Process(float* output, uint32_t frames, uint32_t channels,
                 const float* sidechain, uint32_t scChannels) noexcept override;
    void OnParameter(uint16_t paramId, float value) noexcept override;
    EffectKind Kind() const noexcept override { return EffectKind::Reverb; }
    float GetParameter(uint16_t paramId) const noexcept override;
    uint16_t SidechainBusId() const noexcept override { return kInvalidBusId; }

private:
    struct DelayLine {
        std::vector<float> buf;
        uint32_t pos = 0;
        float Read() const noexcept { return buf[pos]; }
        float ReadTap(uint32_t offsetBack) const noexcept;
        void  Write(float x) noexcept;
        void  Advance() noexcept;
        void  Reset() noexcept;
    };

    struct Allpass {
        DelayLine line;
        float gain = 0.5f;
        float Step(float input) noexcept;
        void  Reset() noexcept;
    };

    struct ModulatedAllpass {
        DelayLine line;
        float gain    = -0.7f;
        float lfoPhase = 0.0f;
        float lfoIncr  = 0.0f;       // 0.5 Hz / SR
        float modDepth = 8.0f;       // samples
        float Step(float input) noexcept;
        void  Reset() noexcept;
    };

    struct ShelfPair {                // Cytomic SVF — separable LF/HF
        float ic1eq_lo = 0.0f, ic2eq_lo = 0.0f;
        float ic1eq_hi = 0.0f, ic2eq_hi = 0.0f;
        float g_lo, k_lo, m1_lo, m2_lo;
        float g_hi, k_hi, m1_hi, m2_hi;
        void  Configure(float lfDamping, float hfDamping,
                        uint32_t sampleRate) noexcept;
        float Step(float input) noexcept;
        void  Reset() noexcept;
    };

    struct TankHalf {
        ModulatedAllpass modAP;
        DelayLine        delay1;
        ShelfPair        damping;
        Allpass          ap;
        DelayLine        delay2;
    };

    // Param surface.
    float predelayMs_ = 30.0f;
    float decay_      = 0.5f;
    float lfDamping_  = 0.0f;
    float hfDamping_  = 0.3f;
    float diffusion_  = 0.625f;
    float wetGainDb_  = 0.0f;

    // Derived.
    float decayCoupling_ = 0.5f;     // = 0.25 + decay * 0.74
    float wetLin_        = 1.0f;

    uint32_t sampleRate_ = 48000;
    uint32_t channels_   = 2;

    DelayLine                       predelay_;
    uint32_t                        predelaySamples_ = 0;
    std::array<Allpass, 4>          inputDiffuser_;
    std::array<TankHalf, 2>         tank_;
    float                           crossA_ = 0.0f, crossB_ = 0.0f;
};
```

Internals of `Allpass::Step`, `ModulatedAllpass::Step`, `ShelfPair::Step`
are each ~5 lines of arithmetic. Total .cpp LOC estimate: ~450.

## Parameter surface

| EffectParameter | JSON key | Range | Default | Dock label | Curve |
|---|---|---|---|---|---|
| `Reverb_PredelayMs` | `predelay_ms` | 0 – 200 | 30.0 | Predelay | linear, "ms" |
| `Reverb_Decay` | `decay` | 0 – 1 | 0.5 | Decay | linear |
| `Reverb_LfDamping` | `lf_damping` | 0 – 1 | 0.0 | LF Damp | linear |
| `Reverb_HfDamping` | `hf_damping` | 0 – 1 | 0.3 | HF Damp | linear |
| `Reverb_Diffusion` | `diffusion` | 0 – 1 | 0.625 | Diffusion | linear |
| `Reverb_WetGainDb` | `wet_gain_db` | -24 – 12 | 0.0 | Wet | linear, "+dB" |

Three new `EffectParameter::*` enum values. Existing `Reverb_RoomSize` and
`Reverb_Damping` IDs: see migration question below.

## Material → preset table (Phase 5.1 integration)

| AudioMaterial | decay | lf_damp | hf_damp | diffusion | notes |
|---|---|---|---|---|---|
| `Glass`    | 0.85 | 0.00 | 0.05 | 0.50 | hard + bright; rings on highs |
| `Wood`     | 0.55 | 0.10 | 0.40 | 0.70 | warm, scattered |
| `Drywall`  | 0.45 | 0.20 | 0.55 | 0.70 | typical interior |
| `Concrete` | 0.85 | 0.05 | 0.15 | 0.55 | hard, slappy, slight LF boom |
| `Metal`    | 0.80 | 0.00 | 0.10 | 0.40 | specular; long bright tail |
| `Curtain`  | 0.20 | 0.70 | 0.85 | 0.85 | heavy absorption everywhere |
| `Foliage`  | 0.30 | 0.40 | 0.85 | 0.95 | high-freq scatter + absorb |
| `Meat`     | 0.10 | 0.60 | 0.95 | 0.85 | soft + wet; minimal reflection, dead tail |
| `Cardboard`| 0.25 | 0.30 | 0.75 | 0.75 | light + porous; short tail, papery damping |
| `Rubber`   | 0.08 | 0.65 | 0.90 | 0.80 | dense + dead; the most absorptive non-Liquid material |
| `Liquid`   | 0.15 | 0.30 | 0.98 | 0.70 | wet surface; near-total HF kill, sloshy LF |
| `Default`  | 0.50 | 0.10 | 0.30 | 0.625 | sensible average |

Initial values; designer-overridable per-instance. Lives in
`geometry_query.h` alongside the existing `AudioMaterialDefaults` (which
holds occlusion absorption/damping coefficients — same pattern, different
table).

## Open decisions

### (1) Migration: hard or soft?

`Reverb_RoomSize` and `Reverb_Damping` enum values already exist in
`effect_parameter.h`. Three options:

- **Hard break.** Retire the old enums, rename. Old code referencing them
  stops compiling — clean but loud. Affects external consumers; none yet,
  but the API ossifies once Phase 4 GDScript bindings ship.
- **Soft migration.** Keep old enums; map `RoomSize → Decay` 1:1, map
  `Damping → HfDamping` 1:1. `LfDamping` / `Diffusion` / `Predelay` only
  reachable via new enums. Old configs auto-translate at parse time in
  `bus_config_loader.cpp`.
- **Coexist.** Keep old enums as deprecated, write a one-time migration
  that rewrites `gool/config.json` on first load. Loudest but cleanest
  medium-term.

**Recommendation: soft migration.** Pre-1.0, the migration cost is real
but small, and "soft" preserves existing sandbox configs without surprising
behavior.

### (2) Mono input or stereo?

Dattorro's published topology is mono-in / stereo-out — same as what
Freeverb does now via sum-to-mono. Pure plate sounds best this way; the
stereo image comes from tap positions, not from preserving input stereo.

Alternative: dual-path Dattorro (two independent input diffusers, L and R)
preserves spatial info into the wet tail. Costs 2× the input-diffuser CPU;
marginal benefit for send-bus use.

**Recommendation: mono input.** Simpler, matches the paper, matches the
existing `SpatialParams.reverbSend` send-bus design. Future creative-use
case for stereo-in is a follow-up.

## Implementation plan

Assumes both decisions land at the recommended defaults.

1. Header rewrite — `reverb_effect.h`
2. Implementation — `reverb_effect.cpp` (~450 lines)
3. New enum values — `effect_parameter.h`
4. JSON parser additions — `bus_config_loader.cpp`
5. Soft-migration map for `Reverb_RoomSize` / `Reverb_Damping` —
   same file
6. Dock metadata + ranges — `mixer_dock.gd` `PARAM_META` table
7. Material preset table — `geometry_query.h`
   (new `ReverbPresetByMaterial`)
8. CHANGELOG entry for v0.29.0

Estimated 1–2 working sessions of implementation + testing. The
risk-bearing piece is the SVF coefficient calculation: the Cytomic paper
has the analytical form, but it's easy to flip a sign and get nonsense.
Worth a unit test that hits the filter with white noise and checks the
magnitude response at a handful of frequencies (e.g., 100 Hz, 1 kHz,
10 kHz) against expected gains.

## Done when

- Existing sandbox configs (using `room_size` / `damping` keys) load and
  sound roughly equivalent to current Freeverb behavior via the soft-
  migration mapping. No silent breakage.
- Sandbox demo: drop a reverb on a bus, set `decay=0.85, hf_damping=0.05`
  (Glass-like) vs. `decay=0.85, hf_damping=0.85` (Curtain-like), audibly
  distinguishable on a transient stimulus (snare hit or gunshot).
- Six dock sliders authoring all six params, persisting via the v0.28.x
  topology layer, surviving editor restart.
- Magnitude-response unit test for the SVF shelves passes within ±0.5 dB
  of expected at the spot-check frequencies.

## Future work — Tier 3 (FDN with Hadamard feedback)

If Dattorro proves limiting (e.g., need for per-mode-density control,
impulse-response-driven preset matching), the next step would be a full
FDN with Hadamard feedback matrix per the Signalsmith reference. Roughly
3–4 weeks of work; out of scope for v0.29.0 but worth flagging as the
escape valve.

---

## v0.57.0 audit: gool's reverb against the public reverb-design literature

A user supplied a checklist of the canonical reverb-quality factors
(echo density, in-loop damping, modulation, feedback matrix, separate
ERs, nonlinear aliasing, "ping" killing, full-bandwidth preservation),
along with the academic references — Dattorro 1997, Smith's *Physical
Audio Signal Processing*, *DAFX*, Pirkle. This is an audit of gool's
reverb against that checklist, with the design rationale for what
gool does, doesn't do, and might do later.

### What gool does well

- **Topology faithful to Dattorro 1997.** Delay lengths (142/107/379/277
  diffuser, 4453/3720/4217/3163 tank delays, 1800/2656 tank APs,
  672/908 modAP base) are Dattorro's published values, scaled to the
  runtime SR via `ScaleDelay()`. Output tap positions inside the tank
  match the paper. The Schroeder allpass writeback (`x + gain*y`, not
  `x + gain*d`) was corrected in v0.29.4 after a regression.

- **No nonlinearities anywhere in the chain.** The damping shelf is a
  pair of one-pole IIRs (linear). Allpasses, delays, and multiplies
  are linear. Per the framework: "linear delay/filter structures do
  not inherently alias" — so aliasing/foldover are not concerns for
  gool's reverb at any SR.

- **Modulation depth scales with sample rate** (v0.56.0). The original
  Dattorro 8-sample mod depth is interpreted as 269 µs of LFO sweep
  and scaled to give the equivalent at any SR.

### What v0.57.0 fixed

- **Tank delay1/delay2 had reversed read/write ordering.** From
  v0.29.0 ship through v0.56.1, the Process loop did
  `Write → Read → Advance` on delay1 and delay2, returning the
  just-written value (zero delay). The long Dattorro tank delays —
  the literal foundation of the plate's diffuse late-field character
  — were short-circuited. The tail being heard was generated by the
  modAP + AP allpasses + cross-feedback alone, which is exactly the
  topology you'd expect to sound metallic/harsh: no long delays to
  smear the recirculating energy.

  Why this wasn't caught earlier: the system still produced a
  reverb-like tail (because cross-feedback alone produces decay), and
  the output taps (which read at offsets via `ReadTap`) returned
  non-zero values because they don't depend on the immediate
  Read-after-Write ordering. So the reverb sounded *plausible enough*
  to pass smoke tests, just consistently harsh.

- **`hfDamping` default raised 0.3 → 0.45.** At 0.3, the one-pole
  damping shelf had cutoff near 9.6 kHz at 48 kHz SR — essentially
  transparent per circulation. Combined with the broken tank delays,
  HF energy accumulated faster than it dissipated, producing the
  bright metallic tonal buildup the framework calls out as a primary
  metallic-reverb cause ("real spaces absorb highs over time; digital
  reverbs often leave highs untouched too long").

### Deferred to future releases

- **Tank delay modulation (point 3 in the framework).** Currently
  only the modAPs modulate. Adding subtle (1–2 sample, slow LFO,
  decorrelated from the modAP LFOs) modulation to the long tank
  delays would further break standing-wave artifacts. Now that the
  tank delays actually function, this becomes meaningful work.
  Probably v0.58.0.

- **Hadamard-matrix FDN reverb as a separate effect (point 4).** A
  Dattorro plate has 2×2 cross-coupling by design — it's the
  *plate's* sonic signature. For concert-hall / cathedral character,
  a 4×4 or 8×8 FDN with an orthogonal feedback matrix
  (Hadamard / Householder) sounds substantially smoother and more
  diffuse. This would be a separate effect kind (`Reverb` would
  remain the plate; new `ReverbHall` or similar for the FDN), not a
  replacement. Reference: Smith's PASP chapter on FDNs.

- **Dedicated early-reflection stage (point 5).** Currently the
  input diffuser produces "early reflections" but they're not
  spatially distinct (mono in, mono diffused, fed into both tank
  halves). A tapped delay network ahead of the tank could produce
  directionally distinct ERs that anchor source perception. Same
  scope category as the FDN — meaningful work, probably its own
  release.

### Not relevant / already addressed

- **Nonlinear aliasing (point 6).** Nothing nonlinear in the chain;
  not applicable.

- **"Ping" elimination (point 7).** Achieved through points 1, 3, 4
  — addressed structurally as those points get addressed.

### References used

- Dattorro, J. (1997). *Effect Design Part 1: Reverberator and Other
  Filters*. JAES Vol. 45, No. 9. The topology gool implements.
- Smith, J. O. *Physical Audio Signal Processing*. The Schroeder /
  FDN chapters informed how to think about feedback-matrix quality.
- *DAFX: Digital Audio Effects* (Zölzer et al.). General reference
  for the framework points above.
- Pirkle, W. *Designing Audio Effect Plug-Ins in C++*. Practical
  reference for the read/write ordering on delay lines that v0.57.0
  fixed.

gool's design intentionally implements **a faithful Dattorro plate** —
not a stylized re-creation of any specific commercial unit. The plate
is one of the few well-published topologies whose reference
implementation is academic rather than proprietary; the improvements
in v0.57.0 are about making gool's faithful implementation actually
behave as Dattorro's paper describes, not about chasing any specific
commercial sound.
