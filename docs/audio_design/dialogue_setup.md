# Dialogue setup — `DialogueDirector` + bus graph wiring

## TL;DR

`DialogueDirector` ships in v0.43.0 as an autoload that handles
NPC bark playback (queue, priority, per-speaker step-on prevention).
It does NOT do gain-based ducking inside the prefab — that's the
bus graph's job, via sidechain compression.

If you enable the gool addon fresh, the **default `gool/config.json`
writes a bus graph that's already wired for this.** You don't have
to do anything. Just call:

```gdscript
DialogueDirector.register_speaker("survivor_a", $SurvivorA)
DialogueDirector.bark("survivor_a", "callout_tank_spawn", 200)
```

Music, LocalSfx, and RemoteSfx all duck under the Dialogue bus.
"TANK!" cuts through gunfire and music.

If you've **customized your `gool/config.json`** before v0.43.0,
you need to add the Dialogue bus and matching sidechain compressors
yourself. Copy from `addons/gool/templates/dialogue_setup_example.json`.

---

## Why ducking lives in the bus graph

The simpler alternative would have been "DialogueDirector lowers
SFX gain when a bark starts and raises it when the queue empties."
That's a step function — abrupt before/after gain changes, audible
clicks at the edges, and it only ducks bus *gain*, not the *peaks*
that actually compete with dialogue for attention.

Sidechain compression solves both: the compressor reduces SFX gain
*proportionally to how loud the dialogue is right now*, smoothly,
sample-by-sample. Soft callouts duck SFX gently; "TANK!" ducks
hard. No clicks, no abrupt transitions, and the SFX recovers as
the bark trails off.

gool's sidechain compressors already exist (since v0.4) and are
well-tuned (see `docs/audio_design/sidechain_tuning.md`). All
DialogueDirector has to do is route barks through a bus that has
the sidechain wiring on the other side.

## Bus topology (default `gool/config.json` post-v0.43.0)

```
Master
├─ Music
│  ├─ compressor (sidechain: LocalSfx)   ← existing, "your gun wins over soundtrack"
│  └─ compressor (sidechain: Dialogue)   ← new in v0.43.0, "callouts win over soundtrack"
├─ SfxAll
│  ├─ LocalSfx
│  │  └─ compressor (sidechain: Dialogue)   ← new in v0.43.0, "callouts win over your gun"
│  └─ RemoteSfx
│     ├─ compressor (sidechain: LocalSfx)   ← existing, "your action wins over teammates'"
│     └─ compressor (sidechain: Dialogue)   ← new in v0.43.0, "callouts win over teammate guns"
├─ Voice (player VOIP, untouched)
├─ Dialogue (NPC barks, no compressors — drives ducking but isn't ducked)   ← new in v0.43.0
└─ Ambient
```

Three new compressors. DSP cost is small (each is one biquad-grade
loop per channel); the perceptual gain is the L4D2 signature mix
feel that's otherwise expensive to author by hand.

## Sound-bank routing

For DialogueDirector to actually route barks through the Dialogue
bus, your *sounds* need to be assigned to that bus in your sound
bank. Example:

```json
{
  "name": "callout_tank_spawn",
  "file": "voice/barks/tank.ogg",
  "bus": "Dialogue",
  "category": "dialogue"
}
```

If you register a bark with `"bus": "RemoteSfx"` (or leave it
defaulting via category_routing), DialogueDirector's `bark()` call
still plays it — but on the wrong bus, where it'll be ducked *by*
itself instead of doing the ducking. You'll hear "TANK!" mumbled
under the gunfire instead of cutting through.

This is the most common adoption gotcha. The DialogueDirector
docstring repeats the warning. Future tooling may auto-detect
this misrouting and surface a clearer error.

## Tuning knobs

If the default ducking feels too aggressive (callouts make your
gun feel "ducked-out") or too gentle (callouts get buried):

- **`threshold_db`** — how loud the dialogue has to be before
  ducking kicks in. Lower (more negative) = more aggressive.
  Default -22 for LocalSfx/RemoteSfx, -25 for Music.
- **`ratio`** — how hard the duck. 8.0 = strong; 4.0 = subtle.
- **`release_ms`** — how fast SFX recovers after dialogue ends.
  Lower = punchier recovery; higher = sustained duck.
- **`max_reduction_db`** — ceiling on how far the duck goes.
  Prevents "callout cuts everything to silence" in heavy mixes.

Edit these values in your `gool/config.json` (or via the editor
mixer dock at runtime for live tuning).

## Stopping the warning

DialogueDirector emits a one-shot push_warning on first bark if
the Dialogue bus appears unwired. To silence it (e.g. shipping
build, or you're deliberately using a different bus name):

```gdscript
# Anywhere before the first bark, e.g. _ready() of your main scene
DialogueDirector.warn_on_unwired_setup = false
```

Or set it via the Autoloads inspector under Project Settings.
