# Acoustic Presence: Dynamic EQ Across Materials and Spaces

**Status:** Future feature, captured during v0.30.1 push.
**Scope:** Multi-phase initiative. Larger than any single release.
**Prerequisites:** Phase 5.1 (AudioMaterial taxonomy — ✅ shipped in
v0.30.0), Phase 5.2 (default Godot geometry query — queued), Phase
5.3 (source-aware acoustic environment — queued).
**Target home in roadmap:** Phase 6 — Acoustic Presence.

---

## The vision

> The real world is a constantly shifting sonic tapestry. Every
> surface, material, space, and object around us absorbs, reflects,
> diffuses, and amplifies different parts of the frequency spectrum.
> Concrete reflects harsh upper mids. Carpet softens transients
> and absorbs high frequencies. Metal rings with resonant
> brightness. Wood carries warmth through the low mids. Open air
> disperses sound differently than a narrow hallway, and a crowded
> room reshapes the tonal balance of every voice inside it.
>
> What we perceive as "real" sound is not just the source itself,
> but the interaction between sound and environment across the
> entire EQ band. The world is continuously filtering audio
> through physical materials and acoustic spaces, creating an
> ever-changing mix of frequencies that our brains subconsciously
> use to understand presence, distance, scale, mood, and danger.
>
> A video game audio engine has the opportunity to recreate and
> even artistically exaggerate this phenomenon. By dynamically
> shaping EQ response based on material properties, spatial
> geometry, atmospheric conditions, and environmental context,
> the game can make sounds feel grounded in the world instead of
> simply being played back within it. Footsteps on wet stone
> should not just trigger a different sample. They should carry
> different tonal characteristics, reflections, resonance, and
> frequency decay than footsteps on sand, wood, or rusted metal.
> A cavern should bloom with low-end resonance and softened highs,
> while a sterile sci-fi corridor might emphasize sharp transients
> and cold upper frequencies.
>
> But realism is only the foundation. Once a system understands
> how to emulate reality, it can push beyond it into surrealism.
> Dreams often feel believable because they obey enough real-world
> sensory logic to anchor us, while subtly bending acoustics in
> ways reality never could. Frequencies can linger too long.
> Reverb tails can shimmer unnaturally. Certain tones can feel
> emotionally "closer" than physically possible. A forest can
> whisper with impossible clarity. A monster's voice can resonate
> as though the environment itself is speaking.
>
> This is where adaptive EQ and environmental acoustics become
> not just technical simulation, but emotional storytelling. By
> shaping how frequencies move through the world, a game can
> manipulate tension, intimacy, awe, loneliness, scale,
> nostalgia, or dread at a subconscious level. The player does
> not merely hear the world. They feel physically embedded
> within it.
>
> The ultimate goal is not simply accurate sound reproduction.
> It is acoustic presence: creating a world where sound behaves
> with enough believable complexity that the player's brain
> accepts it as tangible, while still allowing the audio
> landscape to drift into something heightened, dreamlike, and
> emotionally unforgettable.
>
> — Brannen, on the morning of the v0.30.1 push

---

## Where the engine is today

The foundation pieces this builds on already exist in the codebase.
Each one is real, tested, and shipping:

- **AudioMaterial taxonomy** (`include/audio_engine/geometry_query.h`):
  9 enum values — Default, Air, Glass, Wood, Drywall, Concrete,
  Metal, Curtain, Foliage. Each carries `absorption` and `damping`
  coefficients for occlusion DSP.
- **Material-aware reverb presets**
  (`ReverbPresetByMaterial`): per-material `decay`,
  `lf_damping`, `hf_damping`, `diffusion` for spaces where
  reverb takes the material's character.
- **Biquad EQ as a DSP primitive**
  (`src/audio_engine/dsp/biquad_filter.cpp`): peaking, low/high
  shelf, low/high pass, band pass. The primitive any "material
  has an EQ curve" path would lean on.
- **`AudioOcclusionHit`** carries `absorption` (broadband gain
  reduction) and `damping` (HF rolloff). The signal path through
  a material already attenuates and softens — but only by gain
  and a low-pass.
- **Sound bank `by_material` variant set** (Phase 5.1): designers
  already pick the sample variant per material. The natural
  parallel is picking the EQ curve per material.
- **`GoolAudioMaterial` Resource** (Phase 5.1): designer-facing
  asset for tagging colliders. The obvious place to grow EQ
  fields.
- **Reverb with Dry/Wet** (v0.29.5): the wet-tail level is
  designer-controllable per bus, so material reverb character
  doesn't fight the source.

What's missing isn't infrastructure — it's the connections between
existing pieces and a handful of new DSP nodes.

---

## What's missing for the vision

1. **Materials don't currently carry an EQ curve.** Absorption +
   damping is a 2-parameter model — broadband loss + HF rolloff.
   Real materials shape mids and lows differently from highs.
   Concrete is bright in the upper mids. Wood is warm in the low
   mids. Carpet is dark everywhere. Two parameters can't model
   that.
2. **The signal path through a material isn't EQ-shaped.** When a
   gunshot is occluded by a wall today, it's attenuated + low-passed.
   It should also be shaped by the wall's EQ character — concrete's
   upper-mid reflection, wood's low-mid warmth.
3. **Listener spaces don't shape frequency response globally.**
   Standing in a cavern, every sound should feel low-mid-bloomed.
   Standing in a sci-fi corridor, every sound should feel
   upper-mid-bright. Right now only reverb shapes the space; the
   direct path is uncolored.
4. **No way to layer "realistic" + "exaggerated" character.** The
   vision explicitly wants both — anchor the player in believable
   acoustics, then bend into surrealism for emotional storytelling.
   That needs a single dial designers can push from "physically
   accurate" through to "dreamlike."
5. **No designer-facing authoring tool.** A material's EQ curve
   needs to be visible, editable, A/B-comparable, and reusable
   across levels. Without that, the system is unusable by anyone
   except the engine author.

---

## Engineering plan

Five phases, sequenced so each ships standalone value and informs
the next.

### Phase A — Per-material EQ curves in the taxonomy
**Size:** M. Ships standalone value: materials shape their own sounds
more accurately.

**Status: ✅ shipped as v0.33.0.** See `CHANGELOG.md` for the
final tabled values; the per-material curve table lives in
`include/audio_engine/geometry_query.h::MaterialEqByMaterial()`
and is pinned by `tests/unit/material_eq_curve_test.cpp`. The
v0.56.0 retune adjusted Glass and Metal curves to better match
their perceptual intent (see CHANGELOG).

Extend `AudioMaterial` defaults beyond `{absorption, damping}` to
include a 3-band EQ tuple:

```cpp
struct AudioMaterialEqProfile {
    float lowShelfFreqHz;    // typically 200-400 Hz
    float lowShelfGainDb;    // ±6 dB; positive = warmer
    float midBellFreqHz;     // typically 1-3 kHz
    float midBellGainDb;     // ±6 dB; positive = present
    float midBellQ;          // typically 1.0-2.0
    float highShelfFreqHz;   // typically 4-8 kHz
    float highShelfGainDb;   // ±6 dB; positive = brighter
};
```

Tuned defaults per material grounded in acoustic engineering data:

| Material | Low shelf | Mid bell | High shelf | Character |
|----------|-----------|----------|------------|-----------|
| Concrete | +1 dB @ 250 Hz | +3 dB @ 2.5 kHz, Q 1.5 | +0 dB | Bright upper-mid reflection |
| Wood     | +3 dB @ 200 Hz | +1 dB @ 800 Hz, Q 1.0  | -2 dB | Warm, low-mid forward |
| Drywall  | +0 dB | -2 dB @ 1.5 kHz, Q 1.0 | -3 dB | Damped, muffled |
| Glass    | -2 dB @ 250 Hz | +2 dB @ 3 kHz, Q 2.0   | +2 dB | Bright, glassy |
| Metal    | -1 dB @ 200 Hz | +4 dB @ 4 kHz, Q 2.5   | +3 dB | Resonant, harsh |
| Carpet   | +0 dB | -3 dB @ 2 kHz, Q 0.7   | -8 dB | Dark, soft transients |
| Curtain  | +0 dB | -1 dB @ 1.5 kHz, Q 0.7 | -6 dB | HF killer |
| Foliage  | -1 dB @ 300 Hz | -2 dB @ 1 kHz, Q 0.7   | -5 dB | Broadband diffuse |
| Default  | flat (all zero) | | | Bypass |

(Values are starting points — tune against real material recordings.
The reverb preset table revision we deferred from v0.29.6 should
happen alongside this so reverb character and EQ character agree
per material.)

**API surface:** new accessor
`AudioMaterialEqProfile EqProfileByMaterial(AudioMaterial)` in
`geometry_query.h`. Loaders that consume `AudioMaterial` pick this
up automatically.

**DSP:** repurpose the existing biquad. Three biquads in series
per material curve: low shelf, peaking, high shelf. Tested DSP
primitive; the work is plumbing, not new math.

### Phase B — Source-aware EQ
**Size:** L. Ships standalone value: emitters in tagged zones
inherit the zone's character.

**Status: ✅ shipped as v0.34.0** — via the shared-bus approach,
**not** per-emitter EQ. `play_impact_sound(name, position, material)`
pushes the material's EQ to a designated `ImpactEq` bus's three
biquads just before triggering the sound. The per-emitter path
described below was acknowledged as the higher-fidelity option but
deferred since it requires multi-file engine surgery (new per-voice
DSP chain in EmitterDescriptor + VoiceSource + the mixer +
create-emitter path). For typical FPS gameplay (rapid-fire into one
wall = same material per shot) the shared-bus approach is invisible;
the failure mode is back-to-back impacts of different materials in
the same ~5 ms audio block sharing the most-recently-set EQ for their
overlapping tails. **Per-emitter EQ remains future work** —
worth promoting if profiling against a real game with mixed-material
chaos shows the artifact in practice.

When an emitter is inside a `GoolAudioMaterial`-tagged volume
(walls, rooms, environments), apply that material's EQ on the
emitter's output chain — before spatializer, before bus send. So
the gunshot inside the concrete bunker carries the bunker's EQ
character even on the direct path.

Two implementation options:

- **Per-emitter EQ slot.** Each emitter gets a triple-biquad
  reserved for material EQ; the EQ coefficients update when the
  emitter's material context changes. Cheap per-sample (3
  multiplies per band), expensive on context switches.
- **Pre-mix bus per material.** Group emitters by their current
  material into shared buses with shared EQ. Cheap per-sample
  (EQ only computed once per material bus), expensive on
  emitter creation/migration.

Per-emitter is simpler; the bus-per-material approach is more
performant at scale (128+ sources). Start with per-emitter and
measure; promote to bus-per-material if the audio thread budget
demands it.

**Context resolution:** the emitter's `current_material` is
resolved at emitter creation time and on transform updates. Uses
the same `material_from_collider` logic from Phase 5.1.

### Phase C — Listener-aware acoustic spaces
**Size:** L. Builds directly on Phase 5.3 (source-aware acoustic
environment) infrastructure if shipped first. Otherwise standalone L.

**Status: ✅ shipped as v0.35.0.** Implemented as an opt-in
`apply_listener_eq` field on `ReverbZone`. When the listener is
inside a Wood zone with the flag set, every diegetic sound gets
the wood EQ curve via a dedicated `ListenerEq` bus that sits
between Sfx (and other diegetic buses) and Master. Opt-in by
default — a global listener-EQ stage is a stronger editorial
effect than reverb alone, so designers must explicitly enable it
per zone. The 250-500 ms boundary-crossing crossfade is wired
through the existing reverb parameter ramp using the same
`transition_ms` so listener-EQ and reverb ramp in lockstep.

When the listener is in a tagged space (cavern, corridor, forest),
apply a global EQ to the master bus that colors every source the
listener hears. The cavern blooms low-mids on everything; the
corridor brightens upper-mids on everything.

**DSP:** master-bus triple-biquad following the reverb. Source
spaces (Phase 5.3) handle "what is the sound made in"; listener
spaces (Phase C) handle "what is the sound heard from." Both are
real-world acoustic phenomena — sources colored by their origin
space, listeners colored by their current space — and both should
compose.

**Smooth boundary crossing:** EQ coefficient interpolation when the
listener crosses a tag boundary. 250-500 ms crossfade prevents
audible parameter pops; conservative default.

### Phase D — Artistic / surreal modes
**Size:** Exploratory. Could be M if scoped tight; could grow.

The vision is explicit: realism is the floor, not the ceiling.
Once the system models reality faithfully, designers want a dial
to push past it. Three concrete features:

#### D.1 — Realism multiplier (0.0 .. 2.0)
A per-material and per-space "intensity" slider. At 1.0, EQ
matches the realistic default. At 0.0, EQ is bypassed
(materials act like empty space — useful for cinematic moments
where the world goes "quiet"). At 2.0, every band's deviation
from flat doubles (concrete's +3 dB mid becomes +6 dB; carpet's
-8 dB high becomes -16 dB). Designers exaggerate the world's
acoustic identity by pushing past 1.0.

**Status: ✅ shipped as v0.36.0** — a single global dial,
`ProjectSettings("gool/material_eq/intensity")`, scales every
per-material EQ gain uniformly across both 6.B impact EQ and
6.C listener EQ. Cutoff frequencies and Q stay put — only the
three gain_db values per curve get multiplied. The runtime
setter is `Gool.set_eq_intensity(value)` and reads from
ProjectSettings on _ready. Implemented as a GDScript-only
change to the autoload; no engine binary rebuild needed for
projects already on v0.34.0+.

#### D.2 — Temporal frequency effects
Beyond what biquads can model:
- **HF lingering**: high frequencies decay slower than they
  physically should. Implemented as a high-shelf with a long
  release smoother — when an HF transient hits, the shelf gain
  rises and decays over hundreds of ms. Cavern-shimmer territory.
- **Impossible resonance**: a narrow peaking filter swept to a
  fundamental tone that the environment "speaks" at. The
  monster's voice resonating as though the environment is
  responding to it.
- **Frequency drift**: subtle, slow modulation of band frequencies
  to make the acoustic identity feel "wrong" without naming why.
  Dreams territory.

These need DSP work beyond biquads — release smoothers, peaking
filters with modulated centers, frequency-domain stretching for
the most extreme cases. Some can ride on existing primitives;
some need new ones.

#### D.3 — Emotional EQ presets
A named library of EQ + temporal-effect combinations:
- **Awe**: low-mid bloom, slow HF lingering, gentle drift
- **Dread**: low-mid bloom, narrow upper-mid peak (vocal range),
  slow gain ducking of mids when no source is present
- **Intimacy**: HF clarity above what physics allows, slight
  low-shelf reduction (makes everything feel close)
- **Loneliness**: HF rolloff, slight low-mid resonance, broad
  diffuse coloration — like the world's response is just slightly
  delayed
- **Nostalgia**: tape-like high cut, narrow mid-bell warmth
- **Scale**: dramatic high-shelf reduction, low-end emphasis,
  reverb integration

Each preset is just a configuration tuple. The presets are
discovery aids for designers — "I want this scene to feel awe" →
load the Awe preset → tune from there. Not a black box.

### Phase E — Designer authoring tool
**Size:** L. Spans the whole project.

The technical infrastructure is useless without designer-facing
tooling. The principles in the vision — "the player feels
physically embedded within it" — require sound designers to
shape EQ behavior intuitively, not write configuration files.

#### E.1 — Inspector EQ curve editor on `GoolAudioMaterial`
When a designer selects a `GoolAudioMaterial.tres` in the Godot
inspector, they see:
- A visual EQ curve (frequency-response plot, log frequency axis,
  dB amplitude axis)
- Draggable handles for low shelf, mid bell, high shelf — drag
  the dot to move both frequency and gain at once; right-click to
  adjust Q
- Realism multiplier slider (0.0 .. 2.0), wired to D.1
- Preset dropdown — pick a starting point from the material
  library
- Audition button — play a reference sample (pink noise, voice,
  drum hit) through the current curve in the editor

**Status: ✅ Phase 6.E.1 complete.**

Shipped in three releases:

- **v0.59.2 (read-only visualizer)** — frequency-response plot (log freq axis 20 Hz–20 kHz, dB axis ±24 dB), per-material hint, three-band numerical readout, realism intensity slider wired to ProjectSettings. The plot uses the engine's curve via a hardcoded mirror of the C++ `MaterialEqByMaterial()` table (since the Gool autoload isn't reachable in editor context).
- **v0.59.3 (audition button)** — one-click "▶ Play (pink noise, 1 s)" plays one second of Voss-McCartney pink noise through the material's EQ curve at the current intensity, using the exact same `BiquadFilterEffect` code path the runtime uses for impact and listener EQ. The engine exposes a static `GoolAudioRuntime.process_buffer_through_material_eq()` method so the editor inspector can call into offline DSP without the Gool autoload reachable.
- **v0.60.0 (editable per-material curves — Option B)** — drag-handle interaction on the plot dots when `override_enabled=true` on the resource. Three handles (low shelf / mid peak / high shelf), 2D drag = (freq, gain) per band, Q slider for the mid band. Override fields seed from the engine table on toggle-on so the curve doesn't snap. Save on drag-end via `ResourceSaver`. Audition routes through a sibling `process_buffer_through_curve()` static method when override is on, so the audition reflects the designer's tweaks rather than the engine table.

`GoolAudioMaterial` schema gained `override_enabled: bool` + seven per-band `@export` fields + `get_curve()` method. Backward compatible: existing `.tres` files default to `override_enabled = false` (zero runtime overhead — same C++ fast path as v0.59.x). `Gool.play_impact_sound()` now accepts either an int (legacy) or a `GoolAudioMaterial` Resource (preserves overrides). A new `Gool.material_resource_from_collider()` returns the Resource-or-int form for callers that want overrides to flow through.

#### E.2 — Mixer dock EQ visualization
The existing mixer dock gets a section showing the active per-bus
EQ — what's currently being applied to each source as a result of
its material context. Useful for debugging "why does this sound
muffled" — designer sees the EQ chain, identifies the offending
material, fixes its profile or the tagging.

**Status: ✅ Phase 6.E.2 complete (v0.61.1).**

When a designer expands a bus's Fx panel and that bus's first three effects are biquads (the material-EQ convention), the dock prepends a read-only `MaterialEqCurveView` showing the cumulative frequency response of those three biquads. Refreshes at 30 Hz during F5 (matches the existing peak meter cadence), refreshes on config edits at rest. Designer can correlate the audible "muffled" symptom with the visible EQ shape in one glance, then drag the offending biquad slider or follow the chain back to the offending material.

Implementation: nested class `_BusEqVisualizer extends VBoxContainer` in `mixer_dock.gd`, wraps the existing v0.59.2 `MaterialEqCurveView` widget read-only (`editable = false` — the widget's docstring planted this hook explicitly). Pure GDScript editor-side; no engine changes. Limitation: biquad subtype (LowShelf vs Peak vs HighShelf vs LPF etc.) is not exposed via the engine's effect-introspection API, so the visualizer assumes positional ordering — matching what `apply_material_eq_to_bus` writes. A non-EQ-shaped 3-biquad chain renders a misleading curve; the per-effect param sliders below are still accurate.

#### E.3 — A/B compare with realism slider
Two-state toggle in the editor: "Realistic" (multiplier = 1.0) vs
"Designed" (multiplier = whatever the designer set). Click toggles
between them in real-time so designers can hear the difference.
Powerful for tuning surrealism: "is this scene better at 1.0 or
1.6?"

#### E.4 — Material library
A project-level library of `.tres` resources, each a complete
material profile (occlusion coefficients + EQ profile + reverb
preset overrides). Drag a material from the library onto a
collider's metadata; the whole acoustic identity is set in one
gesture. Save your own materials back to the library to share
across projects.

**Status: ⚠️ Phase 6.E.4 mostly complete (v0.61.2 + v0.61.3) — EQ tonal-character library; full profile bundling deferred.**

v0.61.2 shipped the per-project save/load infrastructure: `GoolMaterialEqPreset` Resource type, Save…/Load… inspector buttons, presets stored as `.tres` files under `res://gool/material_eq_presets/`.

v0.61.3 shipped the **built-in tonal-character library** at `res://addons/gool/material_eq_presets/` — twelve curated presets covering common acoustic situations: hard reflective ("Tile bathroom", "Concrete bunker", "Empty warehouse", "Cathedral stone"), warm/wooden ("Wooden cabin", "Library"), damped ("Carpeted office", "Forest clearing"), and stylistic ("Underwater", "Phone speaker", "Through a wall", "Old radio"). The Load… picker shows built-ins (★ prefix) above user presets, with descriptions inline and tooltips disclosing the source path. This delivers the design-doc principle "Defaults that 'just work'" extended from per-material defaults to per-space starting points: a Godot dev installing gool can immediately pick a preset that's close to the space they're designing, and tune from there rather than authoring from scratch.

What's deferred (full Phase 6.E.4 per design-doc literal; candidate work for v0.62.x with its own design pass):

1. **Occlusion overrides on materials.** Currently per-material absorption/damping coefficients live only in the engine table; there's no `.tres`-level override path equivalent to v0.60.0's EQ override fields. Adding that would let presets bundle "punchier concrete" with "less occlusive concrete" together.
2. **Reverb preset binding.** Materials don't currently influence which reverb preset gets applied. The design-doc vision of "drag a material → set the full acoustic identity" requires associating reverb presets (or send levels per zone type) with material profiles — substantial engine work.
3. **Full GoolMaterialProfile Resource bundling EQ + occlusion + reverb.** Once 1 and 2 land, the preset Resource grows from "EQ curve only" into "complete acoustic identity". The v0.61.2 `preset_schema_version` field is in place for forward-compatible loading.
4. **Inspector drag-to-apply on colliders.** Drop a material profile onto a CollisionShape3D in the inspector and have the right metadata set automatically. The design-doc workflow, but requires an inspector plugin for CollisionShape3D, which is currently outside gool's editor scope.

The deferred items are scoped as v0.62.x because (1) and (2) require schema changes to `GoolAudioMaterial` itself with thoughtful backward compatibility, and (3) reorganizes what a "material" is. That's design-doc work, not an addon-side patch. The v0.61.x cluster ships the EQ slice that's genuinely useful today, with the schema set up to grow into the larger vision.

---

## Designer experience principles

The technical surface is large. Tools have to absorb that
complexity rather than expose it. The principles, in priority order:

1. **Defaults that "just work."** A fresh project with no material
   tags should already sound acoustically grounded — using the
   v0.30.0 material taxonomy + Phase A defaults. Designers tune,
   they don't author from scratch.
2. **One slider for the realism-to-surrealism dial.** The D.1
   multiplier is the primary expressive control. Everything
   downstream (per-band gain, temporal effects, presets) flows
   from this single intuitive concept.
3. **Visual first, numeric second.** EQ curves are drawn, not
   typed. Frequency response is shown, not abstracted into
   numbers. Numeric fields exist for precision work, but the
   primary interaction is direct manipulation of the curve.
4. **Audition in context, not in isolation.** Designers should
   be able to hear the material's EQ as it actually applies in
   the level — same reverb context, same listener position —
   not in a sterile preview booth.
5. **A/B is one click away.** Realism vs Designed, Material A vs
   Material B, Preset X vs Preset Y. The fastest way to know if
   a choice is good is to hear the alternative immediately.
6. **Discoverable presets, editable foundations.** Presets are
   teachers, not black boxes. Loading "Awe" should reveal the
   underlying EQ + temporal config so the designer learns what
   makes Awe feel like Awe, and can tune from there.

---

## Open questions

These are deferred decisions, not blockers. List them now to avoid
re-deriving them when the work starts.

- **DSP cost ceiling.** Per-emitter EQ is ~6 multiplies per sample
  per source for 3 biquads. At 48 kHz × 128 sources, that's ~37M
  multiplies/sec — well within budget, but worth measuring against
  the existing reverb + spatializer cost when prototyping.
- **Authoring schema location.** Does the EQ profile live inside
  `GoolAudioMaterial.tres` (one resource per material kind, EQ
  baked in)? Or in a separate `GoolMaterialEqProfile.tres` that
  the `GoolAudioMaterial` references (so you can mix and match
  taxonomy + EQ)? The former is simpler; the latter is more
  flexible. Lean toward the simpler unless flexibility need
  emerges.
- **Listener space vs reverb zone overlap.** Phase C and the
  existing ReverbZone both characterize "what the listener is
  inside." Should they merge into a single "acoustic space"
  concept, or stay separate? Probably merge — designers already
  think of "the cave" as one thing, not "the cave's reverb" +
  "the cave's EQ."
- **How does realism multiplier interact with reverb decay?** At
  multiplier = 2.0, does reverb decay double too? Probably yes,
  to keep the acoustic identity coherent — but designers may want
  to override.
- **Temporal effects (D.2) DSP scope.** HF lingering is cheap;
  impossible resonance is cheap; frequency drift via modulated
  biquads is cheap. Frequency-domain stretching (for the most
  extreme dreamlike effects) needs FFT — significantly more
  expensive, only worth it if playtest demands it.
- **Cross-platform consistency.** Biquads on consoles vs PC at 48
  kHz should produce bit-identical output (we already have
  determinism guarantees). Temporal smoothers need the same
  treatment.

---

## Risks

- **Per-source EQ DSP cost at high voice counts.** Mitigated by
  bus-per-material grouping in Phase B if needed.
- **Designer overload.** Too many knobs makes the system unusable.
  Mitigated by the "one slider for realism-to-surrealism"
  principle and aggressive use of presets.
- **Realism vs surrealism toggle fights with itself.** If a
  designer sets the realism multiplier to 2.0 on a material AND
  loads a surreal preset, they could get a runaway feedback loop
  of exaggeration. Mitigated by clamping the multiplier's effect
  on already-extreme presets, and by visual feedback in the
  curve editor when the result clips or saturates.
- **Reverb + EQ + occlusion phase relationships.** Three filtered
  paths summing into the listener's ears can produce comb-filter
  artifacts at certain geometries. Mitigated by careful
  scheduling of when each filter applies — direct path filtered
  first, reverb tail filtered separately, then summed.

---

## Sequencing recommendation

Suggested order in the roadmap, once Phase 5.1 sandbox demo
(v0.30.x) ships and Phase 5.2 / 5.3 land:

- **Phase 6.A** — Per-material EQ curves in the taxonomy
  (small-medium, foundational, immediate audible improvement)
- **Phase 6.B** — Source-aware EQ
  (large, builds on 5.3 source-aware acoustic environment)
- **Phase 6.C** — Listener-aware acoustic spaces
  (large, may merge with reverb zone concept)
- **Phase 6.E.1-2** — Inspector EQ editor + mixer dock visualizer
  (medium, prerequisite for designer adoption of A-C)
- **Phase 6.D** — Artistic / surreal modes
  (exploratory; ships once realism baseline is solid)
- **Phase 6.E.3-4** — A/B compare + material library
  (medium, polish layer once D presets are interesting enough to
  warrant a library)

Total scope is comparable to Phase 5 — i.e., multiple minor
versions, not a single release. Phase 6.A alone is shippable
value and the natural starting point.

---

## What "done" looks like

A sound designer opens a fresh level. The default `GoolAudioMaterial`
tags applied to surfaces give the level immediate acoustic
character — concrete corridors sound bright and upper-mid forward,
wooden buildings sound warm, foliage sounds diffuse. The designer
tunes one material's EQ in the inspector and the level feels
different in a way that's hard to articulate but easy to perceive.
They push the realism multiplier on the central plot location to
1.5, and the location takes on an unmistakable identity — every
sound that happens there carries the location's signature. They
A/B compare against 1.0 and decide 1.3 is the right amount of
heightening. They save the tuned material to the project library.

The player, who doesn't know any of this is happening, feels
embedded in a world rather than told about one. The level has
acoustic identity. Sounds belong somewhere instead of just being
played.

That's the goal. Everything in this document serves that
outcome.
