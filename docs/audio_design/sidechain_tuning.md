# Sidechain Compression Tuning — gool

**Audience:** game devs setting up `compressor` effects with `sidechain_bus`
in `gool/config.json`, and the host studio's audio designer / Logic / Pro
Tools producer who's coming from music-production sidechain.

This document captures hard-won tuning heuristics for **gunshots and
explosions ducking music in real-time** — the most common use of
sidechain in game audio.

The current sandbox preset is documented in detail below as the
"Action-shooter default" — it's a sensible starting point for cinematic
PvE / shooter contexts, calibrated against Helldivers 2 / DRG / L4D2
mix references.

---

## Intent: game audio sidechain ≠ music production sidechain

The classic music-production use case (Daft Punk / French house pumping,
EDM kick-on-bass) and the game-audio use case sound technically
identical — same DSP, same params — but the **goal is different**:

| Aspect | Music production | Game audio |
|---|---|---|
| **Goal** | Rhythmic pumping effect; the ducking is part of the artistic statement | SFX intelligibility; the ducking is invisible — players hear "the gunshot cut through" without noticing the music dipped |
| **Frequency** | Once per beat, predictable, rhythmic | Triggered by gameplay, sporadic, unpredictable |
| **Threshold** | Low (-25 to -20 dB) — almost everything triggers | Higher (-20 to -15 dB) — only loud events trigger |
| **Ratio** | High (8:1 to ∞:1) — strong pumping | Moderate (3:1 to 6:1) — subtle dip |
| **Attack** | Very fast (1 ms) | Medium-fast (3–10 ms) — let the transient breathe |
| **Release** | Synced to tempo (250–500 ms at 120 BPM) | Length of the SFX body + a hair (150–400 ms typical) |
| **Make-up gain** | Yes, +6 to +10 dB to recover loudness | No, or minimal — the duck is the feature |

**The big philosophical difference:** music production wants you to
*hear* the pumping. Game audio wants you to *not* hear the pumping. The
duck should feel like the gunshot is taking up perceptual space, not
like the music is doing something weird.

References (music-production framing):
- [Hyperbits — Sidechain Compression](https://hyperbits.com/sidechain-compression/)
- [Samples From Mars — Sidechain for kicks](https://samplesfrommars.com/blogs/tips-tricks/18999227-how-to-use-sidechain-compression-to-make-kicks-cut-through-the-mix)
- [Audient — Beginner's Guide to Sidechaining](https://audient.com/tutorial/the-beginners-guide-to-sidechaining/)

These cover the music side excellently. This doc covers what changes
when you bring those techniques into a game-audio context.

---

## Current gool compressor parameters (what each does)

```json
{
  "kind": "compressor",
  "threshold_db":     -20.0,
  "ratio":              6.0,
  "attack_ms":          5.0,
  "release_ms":       200.0,
  "knee_width_db":      4.0,
  "max_reduction_db":  10.0,
  "sidechain_bus":   "Sfx"
}
```

| Param | What it does | Typical range |
|---|---|---|
| `threshold_db` | Sidechain signal level (post-effects, post-gain) above which compression begins | -30 to -10 |
| `ratio` | Reduction ratio — input above threshold by N dB → output above threshold by N/ratio dB | 2 to 10 |
| `attack_ms` | How fast the gain reduction reaches full value after threshold cross | 0.5 to 50 |
| `release_ms` | How fast the gain reduction recovers after sidechain drops below threshold | 50 to 1000 |
| `knee_width_db` | Soft-knee zone width centered on threshold. 0 = hard knee, higher = smoother onset | 0 to 12 |
| `max_reduction_db` | Hard cap on total gain reduction — safety against extreme over-ducking | 6 to 20 |
| `sidechain_bus` | Name of the bus whose signal drives the compression (NOT the bus this effect is on) | bus name |

Note: gool's compressor is full-band (no multiband processing). See
"Future engine improvements" below for multiband and lookahead.

---

## Preset cookbook

The defaults are tuned for an action-shooter context. Here are
alternative presets for specific situations. Drop these into
`gool/config.json` as the compressor block on the bus you want ducked
(usually Music).

### Action-shooter default (current sandbox)

Cinematic PvE / co-op shooter with mix of gunshots and explosions.
Helldivers 2 / DRG / L4D2 territory.

```json
{
  "threshold_db":   -20.0,
  "ratio":            6.0,
  "attack_ms":        5.0,
  "release_ms":     200.0,
  "knee_width_db":    4.0,
  "max_reduction_db":10.0,
  "sidechain_bus": "Sfx"
}
```

**Behavior:** A single gunshot (peak ~ -6 dBFS, 150 ms duration) drives
the sidechain above threshold for ~30 ms. Compressor reaches ~80% of
full reduction in 5 ms (during which the gunshot transient itself
passes through music undampened — exactly what we want). Music ducks
~6-8 dB for the duration of the gunshot's body, then releases over
200 ms — by which time the next gunshot might already be triggering.
For rapid fire, music settles into a continuously-ducked state of ~6
dB until firing stops.

### Cinematic explosion / heavy weapon

For games with sustained explosion bodies (1-3 second decay), grenade
launchers, missile impacts. Longer release so music stays ducked
through the explosion's body.

```json
{
  "threshold_db":   -22.0,
  "ratio":            8.0,
  "attack_ms":        8.0,
  "release_ms":     600.0,
  "knee_width_db":    6.0,
  "max_reduction_db":12.0,
  "sidechain_bus": "Sfx"
}
```

**Tradeoff:** music takes longer to recover, so quieter SFX (footsteps,
ambient pings) won't compete cleanly during the recovery phase. Best
for games where explosions are sparse but impactful.

### Stealth / single-shot dramatic

For games where individual shots should feel surprising — stealth
games, single-shot weapons like sniper rifles. Lower threshold +
faster release so a single transient ducks dramatically then recovers
quickly.

```json
{
  "threshold_db":   -25.0,
  "ratio":            8.0,
  "attack_ms":        3.0,
  "release_ms":     120.0,
  "knee_width_db":    2.0,
  "max_reduction_db":12.0,
  "sidechain_bus": "Sfx"
}
```

**Tradeoff:** more pumping-prone if any continuous sound (footsteps,
ambient noise) trips the threshold. Best when Sfx bus is reserved for
discrete events.

### Constant-action / horde mode

For games with continuous gunfire (top-down shooters, horde survival).
Higher threshold so only the loudest events duck, preventing the music
from being stuck at -10 dB the entire mission.

```json
{
  "threshold_db":   -12.0,
  "ratio":            4.0,
  "attack_ms":       10.0,
  "release_ms":     150.0,
  "knee_width_db":    6.0,
  "max_reduction_db": 6.0,
  "sidechain_bus": "Sfx"
}
```

**Behavior:** music ducks only on the heavier impacts (rocket
launchers, boss attacks, multiple-shot bursts). Background fire passes
through without disturbing the music.

### "Transparent" mode (minimal ducking)

If the sound designer's mix is already balanced and ducking should be
near-imperceptible.

```json
{
  "threshold_db":   -10.0,
  "ratio":            3.0,
  "attack_ms":       15.0,
  "release_ms":     250.0,
  "knee_width_db":    8.0,
  "max_reduction_db": 4.0,
  "sidechain_bus": "Sfx"
}
```

**Behavior:** music only dips 2-4 dB on the loudest impacts. Useful
when the music itself is sparse / well-arranged to leave room for SFX.

---

## Tuning methodology — how to dial in your own

Three iterative steps. Use the v0.24+ mixer dock to watch what's
actually happening per bus.

### Step 1 — Set the ceiling

Play a typical loud SFX event (gunshot) in isolation. Watch the **Sfx**
meter on the mixer dock. Note the peak in dB.

**Set `threshold_db`** to about **10-15 dB below** that peak. If
gunshots peak at -6 dB, threshold goes to -18 to -22.

Higher threshold = only louder events duck. Lower threshold = even
moderate SFX duck.

### Step 2 — Set the depth

Watch the **Music** meter while firing. Read how many dB it drops.

**Set `ratio` + `max_reduction_db` together:**
- Want subtle (3-6 dB duck) → ratio 3-4, max_reduction 6-8
- Want noticeable (8-10 dB duck) → ratio 6-8, max_reduction 10-12
- Want dramatic (12+ dB duck) → ratio 8-10, max_reduction 12-15

`max_reduction_db` is the hard ceiling — even if the math says "duck
20 dB", it'll stop at this value. Keep it below ~15 to avoid the
music feeling like it disappeared.

### Step 3 — Set the timing

This is where most tuning happens. The other params are coarse; attack
and release are where the feel comes from.

**`attack_ms`** — controls whether the transient pokes through:
- 0-3 ms: music starts ducking AT the transient. The gunshot's
  initial crack feels less "stuck out of the mix".
- 5-10 ms: music ducks JUST AFTER the transient. The crack hits at
  full music level, then music dips. This is usually what feels best
  for games — the transient is exciting, the dip is intelligibility.
- 15-50 ms: music ducks well after the transient. Feels lazy / late.
  Only use if you want the duck to feel separate from the SFX.

**`release_ms`** — controls how the music recovers:
- 50-100 ms: very fast recovery. Music is "back" almost immediately.
  Risk: pumping artifact on rapid-fire SFX (the recovery starts before
  the next shot, then re-ducks — perceived as pulsing).
- 150-250 ms: medium recovery. Music feels reactive but smooth.
  **This is the sweet spot for most action contexts.**
- 400-800 ms: slow recovery. Music stays ducked through extended SFX
  bodies (explosions). Risk: music feels "stuck low" between events.
- 1000 ms+: very slow. Music almost can't recover during an action
  scene. Use deliberately for "everything is intense" sections.

**Rule of thumb:** release should be roughly the length of the SFX
body. For 150 ms gunshots, release 150-250 ms. For 1.5 second
explosions, release 600-1000 ms.

### Step 4 — Smooth the edges

**`knee_width_db`** controls how gradual the compression onset is.

- Hard knee (0-2 dB): compression kicks in suddenly at threshold.
  Audible "click" if threshold crossings are right at the edge.
- Soft knee (4-8 dB): gradual onset over a range. Smoother but more
  diffuse — the duck feels like an envelope, not a switch.
- Very soft (10+ dB): so gradual it's almost not a compressor anymore.
  Useful for transparent ducking.

For game audio, soft knees (4-8 dB) are almost always better than hard
knees because SFX levels are unpredictable. A hard knee with a poorly-
chosen threshold creates audible artifacts on borderline events.

---

## Common tuning mistakes

### Over-compressing

**Symptom:** music sounds buried even when no SFX is firing. Verify
with mixer dock — music meter sits at -10+ dB consistently.

**Cause:** threshold too low, OR background bus activity (footsteps,
ambient pings) constantly tripping the sidechain.

**Fix:** raise threshold. Or move minor SFX to a separate bus that
doesn't feed the sidechain.

### Pumping artifact

**Symptom:** during rapid SFX (machine gun, sustained fire), music
sounds like it's breathing rhythmically with the shots.

**Cause:** release too fast for the SFX rate. Music recovers, ducks,
recovers, ducks at rapid intervals.

**Fix:** lengthen release until music settles into a continuously-
ducked state during sustained action. 250-400 ms typically.

### Lazy duck

**Symptom:** the gunshot fires, then a beat later the music dips.
Feels disconnected.

**Cause:** attack too slow.

**Fix:** lower attack to 3-5 ms. If still lazy, lower further (but
attacks below 1 ms can cause audible clicks on the duck onset).

### Music doesn't duck at all

**Symptom:** mixer dock shows Sfx meter spiking but Music meter
unchanged.

**Causes (debug ladder):**
1. Threshold too high — SFX never crosses it. Lower threshold.
2. Sidechain bus name typo — `"sidechain_bus": "Sfx"` not `"SFX"` or
   `"sfx"`. Bus names are case-sensitive.
3. SFX routed to wrong bus — the SFX you're firing isn't on the bus
   named in `sidechain_bus`. Verify with the mixer dock: which meter
   moves when you fire?
4. (Through v0.25.1) — the `create_emitter` → SoundDefinition routing
   bug. Fixed in v0.25.2. If on older gool, upgrade or use
   `play_sound_at_location` instead.

---

## Multiple compressors per bus

gool supports multiple compressors in series on one bus. Useful for:

- **Different attack/release for different SFX**: one compressor with
  fast attack short release for gunshots, another with slow release
  for explosions. Both sidechain on Sfx but tuned differently.
- **Stacked light compression**: two compressors each doing -3 dB is
  smoother than one doing -6 dB. Industry-standard mix trick.

Config:

```json
{ "name": "Music", "parent": "Master", "gain_db": -3.0,
  "effects": [
    {
      "_comment": "Fast comp for short transients (gunshots).",
      "kind": "compressor",
      "threshold_db": -20.0, "ratio": 6.0,
      "attack_ms": 3.0, "release_ms": 150.0,
      "knee_width_db": 4.0, "max_reduction_db": 8.0,
      "sidechain_bus": "Sfx"
    },
    {
      "_comment": "Slow comp for sustained explosions.",
      "kind": "compressor",
      "threshold_db": -15.0, "ratio": 8.0,
      "attack_ms": 10.0, "release_ms": 800.0,
      "knee_width_db": 6.0, "max_reduction_db": 6.0,
      "sidechain_bus": "Sfx"
    }
  ]
}
```

Total possible reduction: 8 + 6 = 14 dB across both. In practice the
first compressor's reduction reduces the signal feeding the second, so
the perceived combined reduction is less than the sum. Tune
iteratively.

---

## Future engine improvements

Things gool's compressor doesn't currently do that would expand the
sidechain toolkit. None blocking — the current single-band compressor
is sufficient for "make gunshots cut through music". These are nice
to have, tracked in `docs/roadmap.md`:

### Multiband sidechain compression

Pro game audio mixers use **multiband sidechain** — duck only the
frequency range where the SFX and music compete (typically 500 Hz – 4
kHz where gunshot energy lives). Music's bass (sub-200 Hz) and air
(above 8 kHz) stay full level. Better intelligibility, less
perceptible pumping.

Engine work: split the bus signal into 3-4 bands via crossover
filters, apply per-band compression, sum. Or implement as a single
multi-band compressor effect.

### Lookahead

Pro compressors look 5-10 ms ahead so the duck starts *before* the
transient. Adds latency but produces "anticipated" ducking that feels
natural.

For gool's 0 ms latency target on gunfire, lookahead is incompatible
with the SFX bus itself. But the **music bus** can have lookahead
without affecting gameplay latency — music latency is perceptually
unimportant.

Engine work: add `lookahead_ms` parameter to compressor, implement
ring buffer delay on the main signal path while the sidechain detector
runs ahead.

### Per-event ducking depth

Currently ducking is uniform — every gunshot ducks music the same
amount. Real cinematic mixing varies by event "importance":

- Boss attack: heavy duck (-10 dB)
- Player gunshot: moderate duck (-6 dB)
- Background NPC gunshot: light duck (-3 dB)

Could be implemented as per-emitter "ducking intensity" multiplier on
the sidechain contribution. Each emitter contributes its peak to the
sidechain bus scaled by its intensity.

### Frequency-selective sidechain trigger

Currently the sidechain bus's full signal drives compression. Could
add a "sidechain HPF" (high-pass filter) on the detector path so only
SFX above e.g. 200 Hz trigger ducking — prevents low-frequency rumbles
or ambient bass from triggering.

Engine work: add `sidechain_hpf_hz` parameter, apply HPF to the
sidechain signal before the level detector.

---

## Mix philosophy for game audio

A few principles that emerge from cinematic mixing for games. Not
gool-specific, but useful framing when tuning:

1. **Mix for the loudest moments**: tune compression for peak action,
   then verify quiet passages still feel right. The other direction
   (tune for quiet, hope action survives) usually produces
   over-compressed action sections.

2. **Dialogue trumps music. SFX trumps dialogue (sometimes)**: in
   cutscenes, dialogue ducks music. In action gameplay, SFX ducks
   music. In rare moments (a critical line during a firefight), the
   priority depends on the moment's intent. gool's bus topology lets
   you express this via separate dialogue / sfx / music buses with
   layered sidechain.

3. **Spatial separation reduces masking**: if a gunshot is panned
   right and music is centered, they compete less than if both are
   center. Spatialized SFX (gool's emitter spatialization) helps
   intelligibility without needing aggressive ducking.

4. **Less is more**: every game audio mix that's been criticized as
   "the music disappears every time something explodes" used too much
   reduction or too slow release. Start with the Action-shooter
   default, only crank it up if real player testing reveals the SFX
   doesn't cut through.

---

## See also

- `docs/roadmap.md` — Phase 3.3b/c/d will add an interactive mixer
  editor for tuning these values in real-time, with config.json
  round-trip.
- `examples/05_multiplayer_audio_sandbox/gool/config.json` — current
  reference config using the Action-shooter default.
- `src/audio_engine/dsp/compressor.cpp` — implementation. The level
  detector uses peak detection (not RMS); single-band; soft knee
  via quadratic interpolation.

---

*Document started: 2026-05-17, alongside the v0.25.x mixer dock work
that finally made sidechain compression visually verifiable. Updates
should preserve the preset cookbook section as a stable reference;
new content goes into the "Future engine improvements" section as
features land.*
