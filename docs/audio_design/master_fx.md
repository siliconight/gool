# Master FX: Adaptive Perceptual Mix Management

**Status:** Future feature, captured during v0.59.2 push.
**Scope:** Multi-phase initiative. Larger than any single release.
**Prerequisites:** Phase 6.A-D shipped (per-material EQ + realism
intensity dial — v0.33.0 through v0.36.0), Saturation Phase 3+
shipped (auto-compensation + DC blocker + per-buffer smoothing —
v0.58.0+). Reuses the auto-compensation table pattern and the
parameter-smoothing infrastructure already proven in saturation.
**Target home in roadmap:** Phase 7 — Master Control.

---

## 1. The framing

We are not mastering a song.

We are building an **adaptive perceptual loudness and tonal
management system for an interactive world.**

A traditional music mastering chain assumes a fixed stereo track
and a single playback environment — predictable arrangement, one
intentional dynamic arc, a known listening position. A game engine
violates every one of those assumptions. Players can stack 40
explosions, enter voice chat, trigger UI spam, rotate the camera
into a reflective interior, transition from silence to combat in
one frame, play on laptop speakers at 2 AM. The "song" we're
mastering changes structure, density, content, and listening
context multiple times per second.

So a game master chain must:

- **adapt in real time** to whatever's on the bus right now
- **remain transparent** during sparse moments (most of gameplay)
- **avoid pumping, fatigue, and over-processing**
- **preserve emotional hierarchy** — a boss attack should still
  feel bigger than footsteps
- **translate well** across cheap speakers, TVs, soundbars,
  headphones, handhelds, surround
- **scale with intensity** so the world becomes *more intense*,
  never *more exhausting*

The strongest internal compass is:

> **The player should feel the world becoming more intense,
> never more exhausting.**

If a design decision makes intense moments feel more intense
without making them feel harsh, it's right. If it flattens
emotional dynamics or builds fatigue over a session, it's wrong.

This system should think in:

- **perceived loudness** (LUFS, not peaks)
- **spectral balance** (cumulative tonal buildup)
- **masking** (what's covering what)
- **dynamic density** (how much is happening at once)
- **player intelligibility** (can they hear what they need)
- **adaptive translation** (does this work on their hardware)

Not simply peak amplitude.

---

## 2. Where the engine is today

What gool ships on the Master bus right now is essentially nothing
— Master is a sum, with a gain trim, and the per-bus
sidechain-compressor topology one layer up handles ducking
relationships between Music / LocalSfx / RemoteSfx / Dialogue.
There is no global perceptual stage, no spectral balancing, no
loudness target, no platform translation, no final limiter, no
telemetry.

What gool ALREADY has that this initiative can reuse:

- **`SaturationEffect` Phase 3+ infrastructure** (v0.58.0): the
  per-buffer parameter smoothing pattern (smooths drive/mix/bias/
  tone with ~20 ms time constant), the per-channel DC blocker
  (one-pole HPF at ~30 Hz, replaces stale static DC subtraction),
  and the auto-compensation table pattern (offline-computed
  wet-RMS-vs-dry-RMS ratios baked into a `constexpr` lookup).
  These three patterns are foundational for Master FX — every
  layer below benefits from per-buffer smoothing, every layer
  that does dynamic processing benefits from a DC-blocked path,
  and the auto-comp pattern is exactly the "perceptual loudness
  consistency" mechanism Section 2 of the framing demands.

- **`CompressorEffect` with sidechain support**: the existing
  multi-tier ducking topology (Music ducks under LocalSfx via
  sidechain; LocalSfx ducks under Dialogue; etc.) is the
  conceptual ancestor of dialogue-protection ducking, but it
  operates one bus up from Master, not on the master bus itself.
  Reusable as a code primitive (the same `OnsetDetector` /
  envelope-follower can drive multiple layers).

- **`BiquadEffect`**: the three-biquad chain pattern is well-
  proven (material EQ uses it on per-bus chains; Saturation uses
  it for the v0.59.0 tone tilt). Master FX's spectral balancing
  layer is a natural multi-band biquad chain plus a few
  resonance-suppressor variants.

- **`ReverbEffect` Dattorro v0.29.0+**: not directly relevant to
  Master FX, but its "single effect with internally rich
  pipeline" pattern (predelay → diffuser → tank → damping,
  exposed as one config block) is the architectural template
  for Master FX (input analysis → loudness → spectral → dynamics
  → dialogue → platform → limiter, all one config block).

- **Per-material EQ realism intensity dial** (v0.36.0): the
  `ProjectSettings("gool/material_eq/intensity")` dial scaling
  material-EQ gains across both impact-EQ (6.B) and listener-EQ
  (6.C) without breaking their relative proportions is the
  exact UX precedent for "user-facing perceptual intensity
  knob" that Master FX needs for the Loudness Management layer.

- **Mixer dock with debugger bridge** (v0.24.0-v0.25.0): editor-
  side cross-process channel from running game to editor mixer
  dock, polled at 30 Hz. The right surface to extend with
  Master FX telemetry (LUFS history, spectral heatmaps, masking
  warnings) rather than building a parallel dashboard.

What gool DOESN'T have, that this initiative needs from scratch:

- An integrated-loudness measurement primitive (`EBUR128`-ish
  K-weighted filter + windowed power integration). The existing
  peak meters in mixer dock are amplitude-based, not perceptual.
- Spectral analysis at the bus level — gool measures peak
  amplitude per bus, not spectrum. A `BusSpectrumAnalyzer` (FFT
  ring buffer, ~50 ms window, ~25 Hz update) is the foundation
  for spectral balancing, dialogue masking detection, and
  telemetry.
- A multi-stage `MasterControlEffect` C++ shell that hosts the
  internal pipeline.
- Platform translation profiles (a small enum + per-profile EQ
  curves + per-profile dynamics weighting).
- A true-peak limiter (intersample-aware, lookahead).
- An "intensity" measure derived from gameplay (voice count,
  bus-level energy, recent-onset density) that the system can
  use to scale its own intervention.

---

## 3. Core philosophy

The system should aim to:

| Goal | Behavior |
|---|---|
| Preserve clarity under chaos | Spectral balancing kicks in only when buildup crosses a threshold |
| Maintain perceptual consistency across content | Loudness Management targets LUFS, not peaks |
| Prevent harshness and listener fatigue | Multi-band dynamics that watch the 2-6 kHz fatigue zone specifically |
| Translate well across hardware | Platform Translation profiles compensate per output target |
| Scale with gameplay intensity | Adaptive thresholds tied to bus density / voice count |
| Avoid over-processing during sparse moments | Every stage has an early-out when its input is "boring enough" |
| Maintain emotional impact and transient energy | Transient-aware compression with lookahead protection |
| Preserve headroom for gameplay readability | Dialogue Protection layer guards intelligibility above all else |

The best game mastering chain is the one players never consciously
notice. They feel the world working, not the chain working.

Three rules that distinguish this from a traditional mastering
preset:

1. **Many subtle stages, not three big ones.** Avoid designing
   the Master FX as one giant compressor, one giant limiter,
   one giant EQ. The architecture below has eight stages, each
   doing 0.5-3 dB of work at peak, each with low ratios and
   adaptive thresholds. The cumulative effect is large; no
   individual stage is.

2. **Intervene only when necessary.** Every stage has both an
   audibility threshold (intervene above this) AND a low-density
   gate (skip entirely below that). During sparse gameplay,
   most stages are no-ops — same compute cost as a bypass.

3. **Preserve emotional hierarchy, not flatten it.** Do NOT
   normalize everything to a single perceived loudness. A boss
   attack should still feel bigger than a footstep. Compression
   and loudness management target *consistency across content*
   (footsteps feel like footsteps regardless of room size),
   not *flatness across moments* (everything at -14 LUFS, all
   the time, forever).

---

## 4. Topology

One `EffectKind::MasterControl` effect, conventionally installed
as the last (or only) effect on the Master bus. Internally a
multi-stage pipeline:

```
Master bus input
    ↓
[1] Input Analysis      (LUFS, peak, crest, spectral, transient density)
    ↓
[2] Loudness Management (gain riding toward integrated LUFS target)
    ↓
[3] Spectral Balancing  (perceptual EQ + resonance suppression)
    ↓
[4] Intelligent Dynamics (transient-aware multi-band compression)
    ↓
[5] Dialogue Protection (spectral ducking against the Dialogue bus)
    ↓
[6] Platform Translation (per-output-target EQ + dynamics tweak)
    ↓
[7] Transient Protection (lookahead transient-respecting stage)
    ↓
[8] True-Peak Limiter   (intersample-safe final stage)
    ↓
[9] Output Metering / Telemetry
    ↓
Master bus output
```

Stage 1 is read-only — it produces analysis state used by stages
2-8 but doesn't itself modify the signal. Stage 9 is also
read-only — it taps the post-limiter signal for telemetry but
doesn't modify it. Stages 2-8 form the actual processing chain.

Single config block, single inspector panel, single shipping
unit. The user adds one effect to their Master bus, picks a
profile (Default / Cinema / Headphones / Handheld / Competitive
Clarity / Accessibility / Custom), and tunes a small handful of
top-level dials. The internal pipeline is observable but doesn't
demand attention.

This is the same architectural pattern Reverb uses (one effect,
internally predelay → diffuser → tank → damping, exposed as one
config block). Reverb's success at being "drop-on-bus and forget"
despite its internal complexity is the model.

---

## 5. Engineering plan

Each phase ships standalone value and can be released
independently. Sequence matters — Phase A is foundational
infrastructure that every other phase reads from. Phases B-H
can re-order based on demand.

### Phase A — Input Analysis + scaffolding
**Size:** L. Foundational. No audible change.

Ship the `MasterControlEffect` C++ shell, the analysis primitives
that everything else reads from, and the editor-side telemetry
surface — but NO processing stages yet. Adding the effect to the
Master bus changes nothing audibly; it just produces analysis
state and telemetry.

**New C++ surface:**
- `EffectKind::MasterControl` parameter family. ID block reserved
  in `bus.h` (probably IDs 30-50, leaving room for the 6+ layers).
- `MasterControlConfig` struct with one field per top-level dial
  (target_lufs, intensity, platform_profile, etc.) and one config
  sub-struct per stage (analysis enabled, telemetry enabled, ...).
- New `BusSpectrumAnalyzer`: FFT ring buffer, configurable window
  (default 1024 samples / ~21 ms at 48 kHz), Hann window, 25 Hz
  update rate. Per-bus when installed; aggregate at master level.
- New `LufsMeter`: K-weighted filter + windowed integration
  (400 ms short-term, 10 s integrated, infinite-window).
  Standard EBU R128 measurement; the math is well-documented and
  the K-weighting filter is just two biquads we already have.
- New `TransientDensityMeter`: rolling count of onset-detector
  triggers per second, used by stage 4 to scale its intervention.
- New `MasterControlAnalysisFrame` struct passed forward through
  the pipeline so stages 2-8 can read the analysis without
  recomputing.

**Editor-side surface:**
- Mixer dock gains a "Master FX" inspector panel showing live
  LUFS short-term + integrated, peak / true peak, crest factor,
  current transient density, spectral balance heatmap (8-band
  log meters), masking warnings.
- Same debugger-plugin bridge the mixer dock already uses for
  per-bus peak meters.

**Why this ships first:** every other phase is a consumer of the
analysis frame. Building Phase B against a non-existent analysis
layer means inventing a placeholder that then has to be
retrofitted. Building analysis first means Phase B-H are pure
consumers — they read the frame, they make decisions. Cleaner
incremental adoption too: a user can install Phase A, see what's
happening on their Master bus, and decide whether they even need
the processing stages.

### Phase B — Loudness Management
**Size:** M. Audible: cross-content consistency without flattening.

The first processing stage. Reads short-term and integrated LUFS
from Phase A, applies adaptive gain riding to keep integrated
loudness within a target window (default -16 LUFS for game
content; profiles can shift this).

**Key design points:**
- **Gain riding, not compression.** A slow (~2-5 second) gain
  rider that nudges the post-MasterControl level toward the
  target. NOT a fast compressor — the rider time constant is
  longer than any musical or gameplay phrase, so the rider
  responds to *averages*, not *transients*.
- **Per-content-class hierarchy preservation.** The rider's
  target is integrated LUFS over a multi-second window. Short
  loud events (boss attacks, explosions) push short-term LUFS
  way above the target without triggering the rider, because
  the integrated window hasn't yet absorbed them. Result: loud
  things stay loud relative to quiet things; the only thing
  that stabilizes is *average* across long stretches.
- **Target profiles** (one number per profile, exposed in the
  inspector dropdown):

  | Profile | Integrated LUFS target |
  |---|---|
  | Cinema | -23 |
  | Default | -16 |
  | Streaming | -14 |
  | Competitive clarity | -12 |
  | Headphones | -18 |
  | Night mode | -22 |

- **Bypass at sparse moments**: if integrated LUFS drops more
  than 6 dB below target (silence, sparse exploration), rider
  freezes at last gain. Prevents the "menu silence then huge
  push-up when gameplay starts" failure mode.

### Phase C — Spectral Balancing
**Size:** L. Audible: cumulative harshness disappears.

The biggest single perceptual win. Game audio is additive over
time — thousands of layered sounds accumulate into spectral
buildup that no individual sound contributes to. Long-session
fatigue almost always traces back to a few specific bands
(typically 2-6 kHz, sometimes low-mid mud at 200-400 Hz).

**Components:**
- **Dynamic tilt EQ**: a slow-tracking spectral balance estimator
  compares the actual spectrum (from Phase A's analyzer) against
  a target tilt curve. Discrepancies > ~2 dB drive a wide, gentle
  shelf adjustment. Slow tracking (~3-5 second time constant)
  prevents per-shot pumping; the system responds to *cumulative*
  imbalance, not individual events.
- **Resonance suppression**: per-band peak detection in the
  fatigue zone (2-6 kHz default; configurable). When a single
  narrow band sustains above a threshold for >200 ms,
  surgical dynamic notch suppresses it. Most fatigue comes from
  one or two specific resonances; this is the targeted treatment.
- **Multi-band dynamic control**: three or four crossover bands
  with independent thresholds. Sub-bass overload protection (cut
  if sustained low-frequency energy exceeds threshold; prevents
  bass-rumble accumulation in long combat sequences).
- **Adaptive tilt by content**: dialogue-heavy moments warrant
  a slightly different target than combat. Tilt target shifts
  ~1 dB depending on dialogue-bus energy (read from Phase A).

**Bypass logic:** if the spectral analyzer reports balance within
±2 dB of target on all bands, this entire stage is a no-op.
Most sparse-gameplay buffers will hit the bypass.

### Phase D — Intelligent Dynamics
**Size:** L. Audible: chaos stays controllable.

Traditional bus compression destroys game mixes because it
applies the same time constants to dialogue, ambience, gunfire,
and footsteps. The intelligent variant adapts.

**Components:**
- **Transient-aware compression**: lookahead transient detector
  (~5 ms lookahead, peak-shape recognition); compressor's
  attack delays past detected transients so onsets pass clean
  and only the sustain is compressed. The transient identity
  of impacts and dialogue plosives is preserved.
- **Density-dependent ratios**: low transient density → low ratio
  (1.5:1, gentle). High transient density → higher ratio (3:1,
  more containment). The compression is harder when more is
  happening, lighter when less is.
- **Adaptive release timing**: release time auto-adjusts based
  on program material — fast for percussive content (avoid
  pumping artifacts trailing impacts), slow for sustained
  content (smooth ambience-floor stability).
- **Upward compression** (low-volume detail retrieval): subtle
  expansion of quiet content within a window so footsteps and
  dialogue murmurs remain audible against ambience. Strictly
  optional; off by default.

### Phase E — Dialogue Protection
**Size:** M. Audible: speech is always clear.

Dialogue intelligibility is the most important thing a master
chain protects. The existing per-bus sidechain compressors duck
Music and SFX when Dialogue is active; this phase adds a more
surgical layer.

**Components:**
- **Spectral ducking**: when the Dialogue bus has energy, the
  Master FX applies a *frequency-selective* dip to the non-
  dialogue content in the speech-intelligibility band (~500 Hz
  - 4 kHz). The dip is shallow (~2-3 dB) and band-limited, not
  a broad gain reduction. Music's bass and SFX's brilliance
  are unaffected; only the bands that mask speech get touched.
- **Masking detection**: from Phase A's spectral analyzer,
  detect when non-dialogue spectrum overlaps with dialogue
  spectrum at >threshold dB. Drive the spectral duck only when
  actual masking is happening — not "always when Dialogue is
  playing." Avoids the obvious "radio announcer ducking" feel.
- **Center-channel preservation** (when surround mix is active):
  preserve center-channel dialogue content even when the L/R
  mix is being processed.

This integrates with — but does not replace — the existing
multi-tier ducking compressors one bus up. Those compressors
operate on the full Music / SFX bus signals; the Master FX
spectral duck operates on the master sum post-compression. Both
layers serve different aspects of the same goal.

### Phase F — Platform Translation
**Size:** M. Audible: works on the user's actual hardware.

Profile-based final-stage EQ + dynamics adjustment that
compensates for known characteristics of output devices.

**Profiles** (configurable + auto-detectable):

| Profile | What it does |
|---|---|
| TV speakers | +2 dB @ 200 Hz (bass support), -1 dB @ 4 kHz (avoid screech), tighter dynamics (cheap speakers can't handle wide range) |
| Steam Deck | -2 dB @ 100 Hz (no real bass), +1 dB @ 2 kHz (clarity) |
| Laptop speakers | similar to TV but more aggressive |
| Phone speakers | extreme: -6 dB @ 80 Hz (no bass at all), strong upper-mid for intelligibility |
| Headphones | flat reference + crosstalk damping (optional) |
| Earbuds | gentle HF rolloff at 12 kHz (prevent sibilant fatigue) |
| Soundbar | TV-ish but less aggressive |
| Surround | minimal additional EQ; trust the user's calibration |

The runtime can auto-select based on device hints from the OS
(headphone vs speakers detected; HDMI device class; etc.) OR
the player picks from a settings menu. Same "Audio Output"
dropdown most games have, but the profile is doing real DSP
work instead of just routing.

This is **not** a separate mix per platform. It's an adaptive
translation curve applied to a single mix. Same content,
delivered fairly across hardware.

### Phase G — Transient Protection + True-Peak Limiter
**Size:** M. Audible: nothing breaks, nothing fatigues.

Two stages bundled because they pair conceptually.

**Transient Protection** (Stage 7): a lookahead stage (~5-10 ms)
that protects detected transients from being clipped by the
limiter. When the analyzer reports an incoming peak that would
require the limiter to do >3 dB of work, this stage applies a
brief, transient-preserving gain dip *before* the peak arrives.
The limiter then has less work to do, and the transient
character is preserved.

**True-Peak Limiter** (Stage 8): intersample-aware brickwall
limiter at -1 dBTP (true-peak) by default. Standard 4× oversampled
peak detection. Soft-knee onset, fast attack (~0.5 ms), slow
release (~50 ms) to avoid pumping artifacts. The goal is
*transparency* under normal load and *protection* under chaos;
not loudness-wars-style RMS flattening.

### Phase H — Telemetry dashboard
**Size:** M. Editor-side. Builds on Phase A's analysis output.

Extend the mixer dock with a Master FX panel exposing:

- **LUFS history**: integrated + short-term, last 60 seconds,
  with target window shaded
- **Spectral heatmap**: 8-16 band log meter, color-coded to
  fatigue zones (warm red in the 2-6 kHz band when it sustains
  above target)
- **Dynamic congestion**: a single 0-1 number representing how
  much intervention the chain is currently applying. 0 = chain
  fully transparent, 1 = chain at max work
- **Transient clipping reports**: log of transients that hit the
  limiter's hard ceiling (should be near-zero in a healthy mix)
- **Dialogue intelligibility score**: derived from masking
  detection — 0-100, "would speech currently be clearly
  understandable to a listener who didn't know what was being
  said." Updated every ~100 ms during gameplay.
- **Platform translation preview**: pick a profile from a
  dropdown, hear the master output as it would sound through
  that profile (post-translation, locally).

This turns the system into a **mix assistant + diagnostics
platform**, not just a plugin. A sound designer can run a
playtest session, look at the telemetry log afterward, and see
*exactly* which moments congested the mix, which dialogue
moments got masked, which transients clipped — and tune
accordingly.

---

## 6. Designer experience principles

The technical surface is large. Tools have to absorb that
complexity so designers don't have to.

1. **Profile-first, knobs second.** A designer drops the
   MasterControl effect on Master, picks a profile (default,
   cinema, competitive, etc.), and is done. Sufficient for 80 %
   of projects. Knob-level tuning is available but not required.

2. **One realism-style intensity dial** (0..2, default 1.0)
   scales the chain's intervention uniformly. 0 = pure bypass
   (audit your mix without the chain). 1 = nominal. 2 = stronger
   intervention for chaotic-content games. Same UX precedent as
   v0.36.0's material EQ intensity.

3. **Make telemetry the default authoring path.** Designers
   shouldn't have to *guess* whether dialogue is being masked
   in a given moment; the dock should *show them*. The right
   knob to tune is the knob the telemetry says is currently
   maxed out.

4. **Visible reasoning.** When the chain intervenes, the
   inspector should show *why*. "Spectral duck active — 2.4 kHz
   band masking Dialogue energy at -8 LU." Not just "ducker on."

5. **Default off for invasive features**. Upward compression,
   surround processing, accessibility modes — opt-in. Defaults
   should be conservative enough that adding MasterControl to a
   well-balanced project is *audibly nothing happened*.

---

## 7. Open questions

These are deferred decisions, not blockers. Listing them so the
implementation work doesn't get stalled by re-litigating
architectural choices mid-build.

1. **LUFS measurement window length**: 10 s integrated is the
   broadcast standard, but games may want shorter (~3 s) for
   more responsive gain riding during dynamic scenes. Probably
   ship 10 s default with a configurable override.

2. **Resonance suppressor depth**: how aggressive should the
   surgical notches be? Too gentle and fatigue still builds;
   too aggressive and the sound gets dull. Probably a per-
   profile setting; cinema profile dulls more, competitive less.

3. **Dialogue spectral ducking depth**: 2 dB feels right
   intuitively but needs A/B testing against real game content.
   Could be a per-profile value.

4. **Platform auto-detect reliability**: OS-side device hints
   are imperfect. Auto-detect + manual override (designer
   provides a "platform" setting) is the safe bet.

5. **Surround / multichannel scope**: Phase F's platform
   translation is described in stereo terms. Surround is a
   separate axis (5.1 / 7.1 / Atmos) with its own translation
   needs. Probably out of scope for v1 — ship stereo + headphone
   first, surround as a follow-up.

6. **Editor preview when game isn't running**: should the mixer
   dock's spectral heatmap and intelligibility score work
   against a static loaded scene, or only during F5 playback?
   Probably playback-only — editor-context audio routing is
   the same problem the inspector-audition button has, and it
   shouldn't gate Phase H.

7. **Save/load of telemetry**: should playtest sessions auto-
   record LUFS history and masking events to a `.tres` file
   for later review, or only show live? Live-only for v1;
   recording belongs in a follow-up "session analysis" tool.

---

## 8. Risks

- **CPU budget.** A bus-level FFT + K-weighted LUFS meter +
  per-band dynamic processing + true-peak oversampled limiter
  is real DSP work. Profile early. If the chain costs more
  than ~1.5 % CPU on a Steam Deck-class device, simplify.
  Acceptable mitigation: each stage has an early-out path
  (no spectral imbalance → no spectral processing; no transient
  density → simpler dynamics), so the *average* cost is much
  lower than the peak cost.

- **Over-engineering**. Eight stages is a lot. Resist the urge
  to add "one more thing" mid-build. Each phase should be
  independently shippable and individually justifiable. If a
  proposed feature can't pass the "does it make a measurable
  perceptual difference on real game content" bar, defer it.

- **Bypass / debugging UX**. When something sounds wrong, the
  designer needs to disable parts of the chain to isolate the
  cause. Each stage should have an independent enable/disable
  toggle in the inspector. Without that, a buggy interaction
  becomes "is it the master chain? which part?" and burns
  hours.

- **Loudness target drift across content classes**. If a
  cinematic scene's integrated LUFS sits at -22 and gameplay
  sits at -14, the gain rider will fight one or the other.
  Mitigation: target is per-state, not global. Hand-off from
  cinematic state to gameplay state crossfades the target.

- **Platform translation profile rot**. Hardware ships and
  profiles drift from reality. Mitigation: profiles are data
  (`.tres` resources), not code. Designers and the community
  can tune and contribute profiles without engine rebuilds.

- **Telemetry as theater**. A pretty dashboard that doesn't
  drive decisions is wasted work. Every telemetry signal
  needs a clear "what would I tune in response to this." If
  we can't articulate that for a meter, the meter doesn't
  ship.

---

## 9. Sequencing recommendation

Suggested order, once acoustic-presence Phase 6.E.1 (read-only
inspector, v0.59.2) is shipped and stable:

- **Phase A** — scaffolding + Input Analysis + telemetry surface
  (foundational; everything reads from this)
- **Phase B** — Loudness Management (single biggest perceptual
  win; validates the analysis frame)
- **Phase H subset 1** — basic telemetry (LUFS history + spectral
  meter) lands alongside Phase B so designers can *see* what
  the loudness management is doing
- **Phase C** — Spectral Balancing (biggest fatigue win)
- **Phase D** — Intelligent Dynamics
- **Phase E** — Dialogue Protection (best after C+D since the
  spectral duck reads from C's analysis)
- **Phase F** — Platform Translation profiles
- **Phase G** — Transient Protection + True-Peak Limiter
  (final stage; ship last so we don't keep retuning it as
  earlier stages change)
- **Phase H rest** — complete telemetry dashboard once all
  stages can feed it

Total scope is comparable to Phase 5 + Phase 6 combined —
i.e., 8-12 minor versions. Phase A alone is shippable
foundational value (a designer who installs Phase A gets the
LUFS / spectral / transient telemetry, can SEE their master
bus for the first time, can decide whether to add processing
based on what they see) and the natural starting point.

---

## 10. What "done" looks like

A sound designer is six hours into a playtest. They've built
a level with concrete corridors, intense combat encounters,
voice chat with three teammates, a music score that ducks
under dialogue, and ambient bed loops in every room.

They open the mixer dock's Master FX panel after the session.

They see:
- Integrated LUFS held within ±1 of target (-16) for 94 % of
  the session
- Spectral heatmap shows two yellow zones — 2.8 kHz sustained
  hot during the warehouse combat segment, 220 Hz mud during
  cinematic cuts
- Dialogue intelligibility score averaged 88/100; dropped to
  62 during the boss intro (music + voice + ambience all
  competing)
- Three transient-clipping events, all on grenade explosions
  back-to-back
- Total chain intervention averaged 0.31 (gentle); peaked at
  0.78 (still well below max)

They make four small adjustments based on the telemetry:
- Dial up Phase E (dialogue protection) intensity slightly to
  catch the boss intro
- Add a 220 Hz tilt to the cinematic state's music bus
- Lower the grenade SFX peak by 2 dB at source

They run another session. The numbers are now: 96 % within
target, no spectral zones hot, intelligibility avg 93/100,
zero clipping events.

The player, who doesn't know any of this is happening, feels
that the game's audio is consistently clear, never harsh,
hits hard when it needs to, and doesn't tire them out over a
three-hour session.

The world feels more intense, never more exhausting.

That's the goal. Every stage in the chain serves that outcome.
