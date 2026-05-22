# Reverb EQ shaping

Added in v0.47.0. ReverbZone gains two new `@export` properties —
`send_hpf_hz` and `return_lpf_hz` — that shape the static EQ of
the reverb's contribution to a bus, separately from the
time-varying `hf_damping` / `lf_damping` parameters that have
shipped since v0.29.

Both knobs are baked into every preset in `GoolPresets.REVERB_*`,
so projects using the material-aware ReverbZone path get the
right shaping per-room automatically. Projects that bypass
GoolPresets can author their own EQ via the same fields.

## Why this matters

Two complementary sources, both come at the same conclusion from
different directions:

- **Sound on Sound (Paul White), "Improving the Sound of Your
  Reverb"**: real concert halls have ~nothing above 5 kHz in
  their reflected field. Rolling off the top end of a reverb's
  output is the single most realism-improving move you can make
  on a stock preset. White's specific recipe per preset type:
  3 kHz LPF for choral / intimate spaces, 5 kHz for tiled rooms,
  6 kHz for halls, 10 kHz for small bright rooms.
  <https://www.soundonsound.com/techniques/improving-sound-your-reverb>

- **MixingLessons (Alex), "How to EQ Your Reverb"**: reverb is a
  new signal — EQ it like any other. The "Abbey Road trick" is
  to put an HPF (≈600 Hz) and LPF (≈10 kHz) on the SEND into the
  reverb, with 12–18 dB/oct slopes. Keeps bass mud and harsh
  high-end out of the tail before the algorithm even sees them.
  <https://www.mixinglessons.com/eq-reverb/>

The articles describe **two distinct operations** on the bus,
both useful, both addressable now in gool:

| Operation | Where | What it does |
|---|---|---|
| Pre-EQ (HPF on send) | Biquad effect **immediately before** the Reverb in the bus chain | Removes low frequencies before the reverb generates a tail. The tail won't muddy the mix's bass. |
| Post-EQ (LPF on return) | Biquad effect **immediately after** the Reverb in the bus chain | Rolls top off the entire wet output. Mimics how real rooms attenuate HF over distance and absorption. |
| Damping (existing) | Reverb effect's `hf_damping` / `lf_damping` parameters | Time-varying — the tail loses HF *over time as it decays*. Different from a static LPF. |

Damping and post-LPF do related but not identical jobs. A space
with bright early reflections and a dark long tail (a stone
cathedral, where path-length is the absorber) has **low**
return-LPF and **high** hf_damping. A space with uniformly
darker reflected sound (a heavily upholstered hall) has **low**
return-LPF and lower hf_damping. You generally want both knobs
active.

## How ReverbZone uses them

ReverbZone discovers the adjacent biquad slots once, at scene
start, by scanning the bus chain for the Reverb effect and
checking whether the effects immediately before and after are
Biquad kinds. If they are, their indices are cached and the
zone pushes cutoffs to them during ramps. If they're not,
a one-time `push_warning` fires (only if the zone is actually
trying to use that side of the shaping), and that side is
silently skipped.

This means:

- **You don't HAVE to add the biquad slots.** Zones without
  them still work — they just don't get pre/post EQ shaping.
- **You don't HAVE to use both sides.** Add only an HPF slot
  if you want Abbey Road pre-EQ but no post-LPF, or vice
  versa.
- **The default values are bypass.** `send_hpf_hz = 0.0` and
  `return_lpf_hz = 22000.0` (above audible) mean "leave the
  biquad cutoff at whatever it was set to in config." Material-
  aware ReverbZones (`material != Default`) pick up the
  per-preset values from `GoolPresets.REVERB_*` automatically.

## Recommended bus chain shape

For the Sfx bus (or whichever bus carries your world reverb):

```
[ListenerEq biquads, optional]
      ↓
[Biquad — HPF slot, default cutoff 20 Hz (bypass)]   ← NEW for v0.47.0
      ↓
[Reverb]
      ↓
[Biquad — LPF slot, default cutoff 22000 Hz (bypass)] ← NEW for v0.47.0
      ↓
[Saturation, Compressors, etc — everything else]
```

The bypass defaults are crucial: when no ReverbZone is active,
the chain behaves exactly as if those biquads weren't there.
Only when a ReverbZone is in scope and its `send_hpf_hz` /
`return_lpf_hz` are non-bypass do you hear them work.

## config.json — adding the slots

If your config already has a Reverb on the Sfx bus and you want
to enable v0.47.0 shaping, add the two biquad entries flanking
the existing reverb entry:

```json
{
  "name": "Sfx",
  "parent": "Master",
  "effects": [
    {
      "kind": "biquad",
      "biquad_type": "highpass",
      "cutoff_hz": 20.0,
      "q": 0.707,
      "biquad_gain_db": 0.0
    },
    {
      "kind": "reverb",
      "predelay_ms": 30.0,
      "decay": 0.6,
      "diffusion": 0.625,
      "lf_damping": 0.1,
      "hf_damping": 0.3,
      "wet_gain_db": -12.0
    },
    {
      "kind": "biquad",
      "biquad_type": "lowpass",
      "cutoff_hz": 22000.0,
      "q": 0.707,
      "biquad_gain_db": 0.0
    },
    /* ... your existing other effects continue here ... */
  ]
}
```

The 20 Hz HPF and 22000 Hz LPF defaults are bypass — they don't
affect the signal until a ReverbZone sets them to something
audible. Q=0.707 (Butterworth response) is the standard
non-resonant cutoff curve.

## GoolPresets values shipped in v0.47.0

The eight default `REVERB_*` presets each ship with `send_hpf_hz`
and `return_lpf_hz` mapped from the source articles (Sound on
Sound recipes where they exist, principles-based extrapolation
where they don't). Adjusted slightly from the article numbers
where game-audio context (player inside the space) differs from
mix context (listener outside the space):

| Preset | send_hpf_hz | return_lpf_hz | Rationale |
|---|---|---|---|
| SMALL_ROOM       |   150 | 10000 | Bright — small rooms retain HF in close field. SoS "Small Room" recipe. |
| MEDIUM_ROOM      |   200 |  7000 | Between SoS choral (3k) and bright hall (6k). |
| LARGE_HALL       |   250 |  6000 | SoS "Bright Concert Hall" applied verbatim. |
| CATHEDRAL        |   300 |  6000 | Same LPF as hall — cathedrals have natural HF loss over long paths; HPF a bit higher because long tails compound LF mud. |
| CAVE             |   200 |  4500 | Lower LPF than cathedral — stone is absorptive at HF, but the comb-filtering in irregular caves drops more above 4–5 k. |
| BATHROOM_TILE    |   200 |  5000 | SoS "Tiled Room" — tile is reflective at all frequencies but close-quarters comb-filtering kills HF presence. |
| OUTDOOR_OPEN     |   100 |  4000 | Significant HF loss over open distance; HPF is lenient because outdoor lows are spacious, not muddy. |
| UNDERWATER       |   100 |  2500 | Dominant mid character; hf_damping=0.95 + LPF together. |

## Authoring your own presets

If you're writing a custom preset Dictionary outside of
`GoolPresets`, include `send_hpf_hz` and `return_lpf_hz` in the
keys — `ReverbZone._resolve_zone_target` looks them up with
`preset.get(...)` and falls back to the zone's @export if absent.
Either way works.

```gdscript
# Manual call site
var custom_preset := {
    "predelay_ms":   40.0,
    "decay":          0.78,
    "lf_damping":     0.15,
    "hf_damping":     0.25,
    "diffusion":      0.80,
    "wet_gain_db":   -2.0,
    "send_hpf_hz":  220.0,   # custom Abbey-Road-style HPF
    "return_lpf_hz": 5500.0, # custom LPF
}
Gool.apply_reverb_preset("Sfx", 0, custom_preset)
```

## What this is NOT a substitute for

- **Compositional EQ on individual sounds**. If a gunshot sample
  has too much 200 Hz boom, fix the sample (or use a per-emitter
  EQ on its source bus) — not the reverb shaper. The shaper sits
  on the bus and affects everything routed there.
- **The Dialogue sidechain ducking** (v0.43.0). Different problem
  — sidechain ducks the *level* of music/SFX under dialogue.
  Reverb shaping fixes the *tone* of the reverb itself.
- **Material EQ via ListenerEq** (v0.35.0). That system applies
  per-material tonal coloring to the listener's bus before reverb.
  Reverb EQ shaping is post-reverb on the same bus. Different
  layers, both compose.
