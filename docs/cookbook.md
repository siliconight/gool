# Cookbook

Ten one-screen recipes for things you'll actually need. Each
recipe is under 10 lines of GDScript and assumes you've got the
gool addon installed and enabled (see
[`godot_quickstart.md`](godot_quickstart.md) if you haven't).

Every snippet expects `var Gool := get_node("/root/Gool")` somewhere
above it — the autoload the plugin set up for you.

## Honest framing — when you actually need gool

Godot's built-in audio system is more capable than you might
realize. Some of the recipes below describe things vanilla Godot
already does well; the gool version is more convenient for
multiplayer-shaped workflows but not strictly necessary. Each
recipe is tagged:

- **🎮 Needs gool** — voice chat, server-authoritative events,
  bandwidth budget, priority-driven eviction at scale. These are
  the multiplayer features gool was built for.
- **✨ Convenience** — gool's version is shorter or better-
  organized for game audio, but native Godot can do the same
  thing with `AudioStreamPlayer3D`, `Area3D`, or bus effects.
  Use whichever fits your project's style.

If you don't have multiplayer voice chat or need server-
authoritative audio, you may not actually need gool — vanilla
Godot will serve you well. The integration helpers in v0.14.0
(`register_sound_from_stream`, `set_bus_gain_db`,
`sync_volume_from_godot_bus`) let gool coexist with Godot's audio
system rather than replace it.

## 1 — Play a sound at a position (fire and forget) ✨ Convenience

For one-shot SFX like a coin pickup or a UI ping.

**Vanilla Godot:**
```gdscript
@onready var _player := $AudioStreamPlayer3D    # in the scene

func play_pickup_at(pos: Vector3) -> void:
    _player.stream = preload("res://sfx/pickup.wav")
    _player.global_position = pos
    _player.play()
```

**gool:** (saves a few lines, handles polyphony / eviction
automatically when many sounds fire at once)
```gdscript
# Register the bytes once (e.g. in _ready). v0.14.0+ accepts
# Godot AudioStream resources directly:
Gool.register_sound_from_stream("pickup", preload("res://sfx/pickup.wav"))

# Fire it at a world position from anywhere, any time.
Gool.play_sound_at("pickup", global_position)
```

The gool emitter is owned by the engine, plays to the end, and is
cleaned up automatically. No handle to track, no scene-tree node.

## 2 — Looping emitter that follows an object ✨ Convenience

For a vehicle engine, a campfire, anything that lives in the
world and needs continuous audio.

**Vanilla Godot** — `AudioStreamPlayer3D` as a child of your
object. Set `stream`, `autoplay`, set the stream's `loop` property
in the import settings, and you're done. Zero code.

**gool:**
```gdscript
@onready var _emitter := $AudioEmitter3D    # drag the prefab in

func _ready() -> void:
    _emitter.sound_name = "engine_loop"
    _emitter.looping = true
    _emitter.autoplay = true
    # The prefab keeps the engine emitter's transform synced.
```

gool's emitter cleans up with a 20 ms fade-out when the parent is
queue_freed. Native AudioStreamPlayer3D cuts off abruptly — see
Godot's "cutting audio issue" docs.

## 3 — Mute a remote player's voice 🎮 Needs gool

The local user's "mute this teammate" button. Persistence (saving
the mute across sessions) is your job — the engine just exposes
the state.

```gdscript
@onready var _voice := $VoiceChatPlayer

func on_mute_button_pressed(peer_id: int) -> void:
    _voice.muted = not _voice.muted
    # Or, without holding the prefab reference:
    # Gool.set_voice_source_muted(peer_id, true)
```

Muted sources still receive packets (the network layer doesn't
have to know) but the engine skips Opus decode entirely. CPU
savings are real and measurable; the muted player's audio stops
within one tick.

## 4 — Per-player volume slider 🎮 Needs gool

A UI slider that quiets a too-loud teammate without fully muting.
Range `[0.0, 4.0]`; values above 1 boost above unity.

```gdscript
func on_volume_slider_changed(peer_id: int, value: float) -> void:
    Gool.set_voice_source_volume(peer_id, value)
```

That's the whole hookup. The engine scales the decoded int16 PCM
on the control thread before pushing to the mixer.

## 5 — Bandwidth budget for mobile players 🎮 Needs gool

When a teammate is on a phone or hotspot, cap their upstream
voice rate so they don't burn data. The engine maintains the
token bucket and tells the encoder what bitrate to use frame by
frame.

```gdscript
# 2 KB/sec budget = sustainable 16 kbps Opus.
Gool.set_voice_bandwidth_budget(my_peer_id, 2000)

# In your encode loop (typically 50 Hz, 20ms frames):
var br := Gool.suggest_voice_bitrate(my_peer_id, 20)
if br == 0:
    return    # drop this frame, budget exhausted
encoder.set_bitrate(br)
var bytes := encoder.encode(pcm_frame)
network.send_voice_packet(bytes)
Gool.report_voice_bytes_sent(my_peer_id, bytes.size(), br)
```

Bitrate downgrades on its own as bandwidth tightens — 32 kbps →
24 kbps → 16 kbps → drop.

## 6 — Adaptive music with crossfade ✨ Convenience

Two music tracks, switch between them with an equal-power
crossfade. No clicks, total energy held within ±0.3% through the
transition.

**Vanilla Godot** (~20 lines, no plugin needed):
```gdscript
@onready var _explore := $ExplorePlayer    # AudioStreamPlayer
@onready var _combat  := $CombatPlayer     # AudioStreamPlayer

func _ready() -> void:
    _explore.volume_db = 0
    _combat.volume_db  = -80
    _explore.play()
    _combat.play()

func crossfade_to(target: AudioStreamPlayer, ms: float) -> void:
    var other = _combat if target == _explore else _explore
    create_tween().tween_property(target, "volume_db", 0,    ms / 1000.0)
    create_tween().tween_property(other,  "volume_db", -80,  ms / 1000.0)
```

**gool:** (one config call, equal-power curve is exact rather
than approximate, the engine handles re-loading streams when
states are pushed/popped)
```gdscript
@onready var _music := $MusicStateController

func _ready() -> void:
    _music.add_state("explore", "music_explore", 1500.0)  # 1.5s fade
    _music.add_state("combat",  "music_combat",  600.0)   # 0.6s fade
    _music.set_state("explore")

func on_combat_started() -> void:
    _music.set_state("combat")    # 0.6s crossfade to combat track

func on_combat_ended() -> void:
    _music.set_state("explore")   # 1.5s crossfade back
```

Slower fade into ambient calm, faster fade into combat — typical
shooter pattern.

## 7 — Footstep sounds with surface detection ✨ Convenience

You can do this entirely in vanilla Godot with a `RayCast3D`,
`get_collider()`, group checking, and a few `AudioStreamPlayer3D`
nodes (one per surface type) plus `pitch_scale` randomization to
avoid that mechanical sameness. ~30 lines. gool's version
collapses the same logic into a single prefab plus sound-bank
group selection (e.g. `footstep.grass` picks randomly from
`grass.01` / `.02` / `.03`).

```gdscript
@onready var _footsteps := $FootstepSurfacePlayer

func _ready() -> void:
    # In the inspector you'd set surface_sounds as a Dictionary:
    #   { "grass": "footstep.grass", "concrete": "footstep.concrete",
    #     "wood":  "footstep.wood" }
    # Add the matching groups to your ground StaticBody3D nodes.
    pass

func _on_step() -> void:    # called from your animation system
    _footsteps.step()
```

The "footstep.grass" sound name refers to a group in your sound
bank — gool randomly picks one of `grass.01 / .02 / .03` so steps
don't sound mechanical.

## 8 — Reverb when entering a room ✨ Convenience

Godot does this natively without any plugin. Set up a reverb bus
in your default bus layout, then route an `Area3D` to it via the
`area_mask` property on your `AudioStreamPlayer3D` nodes — Godot's
docs cover this under "Reverb buses". The `area_mask` is a
bitfield; sounds with overlapping bits with the area route through
its reverb. Zero code.

If you've already organized your audio around gool's bus graph
(maybe for sidechain compressors that route to a Voice bus), the
`ReverbZone` prefab triggers signals you can use to swap the
reverb send level on gool's internal Reverb bus:

```gdscript
@onready var _zone := $ReverbZone

func _ready() -> void:
    _zone.listener_entered.connect(_on_listener_entered)
    _zone.listener_exited.connect(_on_listener_exited)

func _on_listener_entered() -> void:
    # Heavier reverb wet level when inside the cave.
    Gool.set_bus_gain_db("Reverb", 0.0)    # 0 dB = full wet

func _on_listener_exited() -> void:
    Gool.set_bus_gain_db("Reverb", -20.0)  # damp the tail back down
```

The zone monitors any node in the `gool_listener` group, so add
`add_to_group("gool_listener")` to your camera or player.

## 9 — Duck SFX while dialogue plays ✨ Convenience

This is the canonical case where Godot's built-in audio is the
right tool. `AudioEffectCompressor` has a `sidechain` parameter
that names a bus whose level drives the threshold detection — set
it to `"Dialogue"` on a compressor in your `SFX` and `Music`
buses and you're done. No plugin needed. The Godot audio-effects
docs walk through the exact technique under "Compressor —
sidechain", and the L4D2 ducking pattern is one
`add_bus_effect` call per ducked bus.

gool's version is the same compressor model with the same
sidechain semantics, plus the bus graph is loaded from JSON
instead of authored in the editor — useful if you're sharing bus
configs across projects, generating them from a tool, or want
them under version control as plain text rather than the binary
`.tres` resource format. Pick whichever fits your workflow:

```gdscript
# Option A: vanilla Godot. In the editor, on your SFX bus, add an
# AudioEffectCompressor. Set its sidechain property to "Dialogue".
# Repeat on Music. The bus layout is saved to default_bus_layout.tres
# automatically.

# Option B: gool, via res://gool/config.json:
# {
#   "buses": [
#     { "name": "Master" },
#     { "name": "Dialogue", "parent": "Master" },
#     { "name": "Music", "parent": "Master", "effects": [
#         { "kind": "Compressor", "sidechain": "Dialogue",
#           "threshold_db": -30, "ratio": 8, "release_ms": 300 }
#     ]},
#     { "name": "SFX", "parent": "Master", "effects": [
#         { "kind": "Compressor", "sidechain": "Dialogue",
#           "threshold_db": -25, "ratio": 4, "release_ms": 200 }
#     ]}
#   ]
# }
```

When someone in the game starts talking, music and SFX dip in
volume automatically and recover when speech ends.

## 10 — Cancel a predicted sound 🎮 Needs gool

For client-side prediction: you fire a gunshot locally on input,
the server later rejects it (the player was lagging, the shot
didn't actually land), you fade the sound out cleanly so the
mistake isn't audible.

```gdscript
var _next_prediction_id := 1

func on_local_shoot(world_pos: Vector3) -> void:
    var prediction_id := _next_prediction_id
    _next_prediction_id += 1
    Gool.play_sound_at("weapon.ak47.shot", world_pos,
                        {"prediction_id": prediction_id})
    # Remember prediction_id alongside whatever state you'll
    # reconcile against the server later.

func on_server_rejected_shot(prediction_id: int) -> void:
    Gool.cancel_predicted_event(prediction_id, 50.0)
    # 50 ms equal-power fade — fast enough to feel snappy,
    # slow enough not to click.
```

Cancelling an id that already finished playing or never started
is a no-op (the engine increments
`predictionsCancelledNotFound`) — safe to call from your
reconciliation code without tracking which predictions are still
in flight.

## 11 — Material-aware impact sounds ✨ Convenience

A bullet hits a concrete wall vs. a wooden door vs. a glass window
and you want a different impact thud for each — without writing
a Dictionary lookup in your weapon code.

### What you need

A sound bank entry using the `by_material` group policy, plus
material tags on your level geometry.

### Author the bank

In your sound bank JSON, add a group with `policy: "by_material"`
and a `members_by_material` dict keyed by material name:

```json
{
  "sounds": [
    { "name": "impact.concrete.01", "file": "sfx/impacts/concrete_01.wav" },
    { "name": "impact.concrete.02", "file": "sfx/impacts/concrete_02.wav" },
    { "name": "impact.wood.01",     "file": "sfx/impacts/wood_01.wav" },
    { "name": "impact.metal.01",    "file": "sfx/impacts/metal_01.wav" },
    { "name": "impact.metal.02",    "file": "sfx/impacts/metal_02.wav" },
    { "name": "impact.generic",     "file": "sfx/impacts/generic.wav" }
  ],
  "groups": [
    {
      "name":   "bullet_impact",
      "policy": "by_material",
      "members_by_material": {
        "Concrete": ["impact.concrete.01", "impact.concrete.02"],
        "Wood":     ["impact.wood.01"],
        "Metal":    ["impact.metal.01", "impact.metal.02"],
        "Default":  ["impact.generic"]
      }
    }
  ]
}
```

Material name keys are case-sensitive: `Default`, `Air`, `Glass`,
`Wood`, `Drywall`, `Concrete`, `Metal`, `Curtain`, `Foliage`.

### Tag your geometry

For each surface in your level (CollisionObject3D, Area3D,
StaticBody3D, etc.), tell gool what material it is. Pick one of:

**Option A — Resource (recommended for reuse).** Create a
GoolAudioMaterial resource (right-click in FileSystem dock →
New Resource → GoolAudioMaterial). Set its `material` field to
one of the `Gool.MATERIAL_*` constants. Save as e.g.
`res://materials/audio/concrete.tres`. Then on each surface
collider, add metadata named `gool_audio_material` pointing at
the resource. One resource shared by every concrete surface —
change it in one place, everything updates.

**Option B — Inline metadata.** On the collider, add metadata
named `gool_audio_material` with type Int, value matching the
material (5 for Concrete, 3 for Wood, etc.). Faster than option
A for one-off surfaces; tedious when you have many.

**Option C — Group membership (legacy / FootstepSurfacePlayer
compat).** Add the collider to a group named
`audio_material:Concrete`, `audio_material:Wood`, etc. Useful
when you're already organizing colliders by group for other
reasons.

All three paths work simultaneously and are checked in the order
A → B → C by `Gool.material_from_collider`.

### Play impact sounds from your weapon code

Raycast on fire, resolve the hit's material, play the impact:

```gdscript
func _try_fire() -> void:
    var space = get_world_3d().direct_space_state
    var origin = global_transform.origin
    var forward = -global_transform.basis.z
    var query := PhysicsRayQueryParameters3D.create(
        origin, origin + forward * 100.0)
    var hit := space.intersect_ray(query)
    if hit.is_empty():
        return
    var material := Gool.material_from_collider(hit.collider)
    Gool.play_impact_sound("bullet_impact", hit.position, material)
```

That's it. The bank picks the right variant for the surface, and
you don't need to write any per-material switch logic.

### What happens when a material has no bucket

This is the part designers worry about. The rule is **lenient**:

- If the surface's material has a bucket in the group (the
  Concrete wall got hit, Concrete is in your `members_by_material`):
  pick a random variant from that bucket. Plays normally.
- If the surface's material has *no* bucket, but the group
  defines a `Default` bucket: pick from Default. The "generic
  impact" plays as a fallback.
- If neither matches: **nothing plays.** No error, no crash.
  The bullet hit a surface you didn't anticipate, and there's
  no audible response.

Why lenient? So you can ship a level with only Concrete and Wood
authored, add Foliage and Metal later, and your game keeps
running between authoring passes. Missing materials show as
silent gaps, not broken builds.

To audit which materials are missing, watch the gool debug
overlay during playtest — it logs every `play_impact_sound` call
that returned no audible variant.

### Where to find more

The full schema for `by_material` groups (including parser error
messages) is in
[`docs/asset_pipeline.md`](asset_pipeline.md#the-by_material-policy).
For the engine-level API and helper functions, see
[`include/audio_engine/sound_bank.h`](../include/audio_engine/sound_bank.h).

## 12 — Occlusion through walls ✨ Convenience

> *"Occlusion as a system capability should absolutely be
> foundational. Occlusion behavior and intensity should be
> contextually authored and scalable. If you enable harsh, fully
> physicalized occlusion globally by default, games can quickly
> become muddy, unintelligible, or frustrating from a gameplay
> perspective. Real life acoustics are not always fun acoustics.
> Players still need clarity, readability, and emotional emphasis."*
>
> — design intent for gool's occlusion system

### What this is

Sounds played by gool are automatically muffled when a wall is
between the listener and the source. Concrete walls block harder
than wooden ones. Curtains kill the highs more than the volume.
Glass barely does anything. The same audio file plays through —
the engine just applies a per-material absorption (gain reduction)
and damping (low-pass filter) to it on the fly.

This works as long as:

1. Your scene has a `GoolListener3D` somewhere in the tree (it
   already needs one for any spatialized audio at all). The
   listener tells the audio engine which `World3D` to raycast
   in.
2. The colliders between sound sources and the listener are
   tagged with the material they represent — same metadata that
   `play_impact_sound` uses (see section 11). Untagged colliders
   fall through to `MATERIAL_DEFAULT` (a neutral mid-range
   absorption).

That's it. No per-emitter wiring, no scene-level configuration,
no manual raycasting. Drop your sound somewhere in the world; if
a tagged wall is in the way, it sounds like there's a wall in
the way.

### The three knobs

**Global enable.** Default `true`. Set under Project Settings →
General → Gool → Occlusion → Enabled. Or toggle at runtime:

```gdscript
Gool.set_occlusion_enabled(false)  # accessibility menu, etc.
```

When disabled the engine stops raycasting; per-emitter occlusion
state smooths back to flat over ~150 ms. Re-enabling resumes the
same gentle ramp.

**Global intensity multiplier.** Default `0.7`. Set under
Project Settings → General → Gool → Occlusion → Intensity. Or at
runtime via `Gool.set_occlusion_intensity(value)`. The full
range:

| Intensity | Feel | Use for |
|---|---|---|
| `0.0` | bypass — no audible occlusion | accessibility off |
| `0.4`–`0.6` | conservative; clarity-first | competitive shooter gameplay |
| `0.7` | **default** — present but not aggressive | most game content |
| `1.0` | physically realistic per-material | immersive single-player |
| `1.5`–`2.0` | exaggerated, surreal | horror, cinematic moments |

The default of `0.7` honours the philosophy above — audible
occlusion that doesn't compromise readability. Dial it up
when you want acoustic realism to dominate; dial it down when
players need to hear something critical no matter what.

**Per-sound opt-out.** Default `true` (sound participates in
occlusion). Pass `occlusion_enabled: false` when registering a
sound that should never be muffled:

```gdscript
# A dialogue sound that must always be intelligible, regardless
# of where the player or the speaker is.
Gool.register_sound_definition(
    "dialogue.tutorial.01",
    true,                    # spatialized
    false,                   # looping
    1.0,                     # min_distance
    100.0,                   # max_distance
    0.0,                     # loop_crossfade_ms
    Gool.CATEGORY_DIALOGUE,
    "",                      # target_bus_name (default routing)
    false                    # occlusion_enabled — OFF for clarity
)

# UI feedback: also opt-out. UI sounds aren't part of the
# diegetic world and feel wrong when muffled by world geometry.
Gool.register_sound_definition(
    "ui.notification",
    false,                   # NOT spatialized — UI is global
    false, 1.0, 1.0, 0.0,
    Gool.CATEGORY_UI,
    "", false)
```

### Tagging colliders

Identical to the impact-sound workflow in section 11. Either:

1. **Inspector metadata** — add a `gool_audio_material` metadata
   entry on the StaticBody3D/Area3D, value is one of the
   `Gool.MATERIAL_*` constants (int).
2. **GoolAudioMaterial resource** — assign a saved resource
   with a `material` property as the metadata value. Useful when
   many colliders share the same material; edit the resource
   once and every reference updates.

A collider with no metadata still occludes — it falls back to
`MATERIAL_DEFAULT`, which has moderate absorption and damping.
Players will still hear that *something* is in the way; the
designer just hasn't told the engine what.

### What about the per-emitter API?

`SoundDefinition.occlusionEnabled` is per-*sound* (set at
registration time), not per-emitter instance. That's the right
granularity for the common cases:

- "This UI bing should never occlude" → register once with
  `occlusion_enabled=false`, done forever.
- "Music shouldn't occlude" → music sounds auto-opt-out via
  `music_channel.cpp`. Nothing to do.
- "Dialogue should never occlude" → register dialogue sounds
  with `occlusion_enabled=false`.

If a single sound needs to occlude sometimes and not others
(e.g. a player's own footstep when the camera is first-person
vs. spectator-style), that's a per-emitter need. The descriptor
struct supports it (`EmitterDescriptor.occlusionEnabled`) but
the GDScript binding doesn't expose it yet. File an issue if
you hit this; the binding is small.

### Performance notes

The engine raycasts at most `maxOcclusionChecksPerFrame` rays
per frame (default 8), round-robin across active emitters. A
scene with 32 emitters → each emitter's occlusion updates ~7 Hz,
which the ~150 ms smoother absorbs invisibly. Bump the budget
if you have very many emitters and want faster geometry
response; lower it if profiler shows physics-query cost climbing.

The query runs on Godot's main thread, against the listener's
`World3D.space`. PhysicsServer3D's direct-state queries are
safe from this context when Godot's 3D physics is configured to
run on the main thread (the default). If you've enabled
`physics/3d/run_on_separate_thread`, the raycast still works
but stale-by-one-frame snapshots are possible — usually
inaudible thanks to smoothing, but if it matters file an issue
and we'll add a queued-update path.

### Multiplayer

Each peer evaluates occlusion against their own world geometry
and their own listener. Two peers in the same scene will hear
the same gunshot differently if a wall is between source and
listener for one of them but not the other. No network state
is exchanged for occlusion — it's purely a local rendering
decision against locally-replicated geometry. This is what
players expect ("I hear it muffled, my teammate doesn't,
because they're on the other side of the wall") and matches
how positional audio already works.

## 13 — Reverb that matches the room ✨ Convenience

### What this is

The same material taxonomy that drives impact sounds (section 11)
and through-wall occlusion (section 12) now drives the *acoustic
character of the spaces themselves*. A concrete corridor reverbs
like a concrete corridor. A wooden cabin sounds warmer. A foliage-
dense clearing has barely any tail at all. Drop a `ReverbZone`
into your scene, pick a material, done.

This completes the Phase 5 material trilogy:

| What it is | What changes |
|---|---|
| **Section 11** — impacts | the sound of *hitting* a material |
| **Section 12** — occlusion | what a material does to sounds *passing through* it |
| **Section 13** — reverb zones | what a material does to the sound of *being inside it* |

All three read from the same engine tables. Tag a slab as Concrete
once and it influences impact sounds, occlusion muffling, and (if
the slab is inside a ReverbZone marked Concrete) the room's
acoustic character — three different physical phenomena, one
piece of authoring metadata.

### The minimum viable setup

You need three things in your scene:

1. A reverb effect on a bus (the standard gool config has one on
   "Sfx" by default — nothing for you to do here unless you've
   customized your bus graph).
2. A `GoolListener3D` in the tree (which you already have if any
   spatialized sound is working at all). Player character is in
   the `gool_listener` group.
3. A `ReverbZone` Area3D with a CollisionShape3D child defining
   where the zone applies.

In the `ReverbZone`'s inspector, set **Material** to one of the
Gool.MATERIAL_* values. Walk into the zone — the reverb should
change. Walk out — it ramps back. That's the entire workflow.

### The materials, in plain terms

| Material | Feel | Good for |
|---|---|---|
| **Glass** | bright, ringing, long-ish tail | greenhouses, observatories, dome rooms |
| **Wood** | warm, mid-rich, short-ish | cabins, wooden corridors, attics |
| **Drywall** | balanced, slightly damped, medium tail | residential interiors, offices |
| **Concrete** | bright, very long, hard reflections | bunkers, parking garages, stairwells |
| **Metal** | bright, slightly metallic ring, long | industrial corridors, ship interiors |
| **Curtain** | very damped, very short | theatre booths, sound rooms, plush dens |
| **Foliage** | very damped, very short, soft | dense forest clearings, jungle interiors |
| **Default** | balanced "average room" | anything that doesn't fit the above |

These are starting points the engine chose to feel right at common
listening levels. Designers are expected to override per-zone for
specific spaces — a small wooden bathroom isn't the same as a
wooden cathedral, even if they share the material category.

### When the presets aren't right: per-parameter override

Set **Material** to `Default` and the zone uses the four per-parameter
exports verbatim:

- **Decay** (0..1) — how long the tail lasts. 0.6 is "normal room",
  0.85 is "long corridor", 0.95 is "cathedral".
- **Lf Damping** (0..1) — how much bass dies in the tail. Higher
  damping = less rumbling, more focused tail.
- **Hf Damping** (0..1) — how much treble dies. Higher damping =
  duller, more muffled tail. Bright spaces (tile, glass) want
  low values; soft spaces (curtained rooms) want high.
- **Diffusion** (0..1) — how smeared the reflections are.
  0 = comb-like "ping-pong" echo, 1 = smooth wash. Most real
  rooms sit around 0.6-0.7.

The **Wet Gain Db** is always applied regardless of which path
you're using — it's the "how much reverb you hear" knob,
independent from acoustic character.

### When to use Default + manual values

- A space where no preset feels exactly right and you've got a
  specific sound in mind.
- A space that's a hybrid (e.g. wood walls but tile floor) where
  you want to dial in something between two presets.
- A *non-physical* space — surreal, dream, or stylized environments
  that aren't trying to match a real-world material.

### Targeting a different reverb bus

The `ReverbZone` defaults to the "Sfx" bus because that's where
gool's standard config puts the reverb effect. If your project
has carved out a dedicated "Reverb" bus, set the zone's **Bus
Name** export to that. The zone scans the bus's effect chain for
the first `Reverb` effect and pushes parameters to it. If no
reverb effect is on the named bus, the zone warns at scene load
and goes inert (no silent failure).

### Programmatic usage

For custom triggers, cinematic moments, or hand-written reverb
logic without a zone, the preset lookup is exposed directly:

```gdscript
var preset = Gool.reverb_preset_for_material(Gool.MATERIAL_CONCRETE)
# preset == { "decay": 0.85, "lf_damping": 0.05,
#             "hf_damping": 0.15, "diffusion": 0.55 }
# Apply yourself via set_effect_parameter, or feed into your own
# blending logic if you're crossfading between multiple presets.
```

### Stacked / overlapping zones

Currently, only the most recently entered zone is active. Walking
out of zone B while still inside zone A ramps reverb back to the
captured defaults, not back to zone A's settings. For most level
layouts (rooms don't typically nest or overlap meaningfully) this
is fine. If you have a case where stacking matters — a large
zone containing smaller "sub-zones" — file an issue and we'll
build a zone-stack abstraction.

### Performance

Per-zone cost is negligible — three integer-keyed Dictionary
operations per frame during a ramp (typically 800 ms after a
zone transition), then nothing until the next entry/exit. The
underlying engine parameter changes are atomic writes; no
audio-thread synchronization.

## 14 — The sound of a material (Phase 6.A + 6.B + 6.C + 6.D) ✨ Convenience

### Why a material has a sound

Each material in the AudioMaterial taxonomy carries more than just
the gain reduction (Phase 5.2 occlusion) and the reverb envelope
(Phase 5.3 zones). It also has a perceptual *fingerprint* — the
frequency contour that makes "concrete" sound like concrete and
"wood" sound like wood, even when the source audio is identical.

This is what Phase 6 (Acoustic Presence — dynamic EQ) is about.
Phase 6.A defined the per-material EQ curves as a 3-band shape
(low shelf + peak + high shelf). Phase 6.B wires those curves
into `play_impact_sound` automatically — the same `impact.generic.wav`
now sounds audibly different on Concrete vs Wood vs Foliage with
zero designer setup beyond the one-time bus authoring. Phase 6.C
wires curves into `ReverbZone` so being *inside* a wooden cabin
colors everything you hear with the wood curve — modeling "your
ears are inside this material." Phase 6.D adds a single realism
dial that scales the whole EQ effect uniformly, from "barely
there" through "as defined" to "amplified for atmosphere."

| Phase | What it does |
|---|---|
| **6.A** (v0.33.0) | defines the curves, exposes them as a Dictionary |
| **6.B** (v0.34.0) | auto-applies curves to impact sounds via the configured EQ bus |
| **6.C** (v0.35.0) | optional listener-space EQ on ReverbZone — coloring everything you hear inside a material |
| **6.D** (v0.36.0, this) | global realism multiplier (0..2) scaling all material EQ |
| **6.E** (next) | inspector EQ editor on a custom GoolAudioMaterial resource |

### The curves, in plain terms

Each material returns a 3-band EQ curve via
`Gool.material_eq_for_material(material)`. The bands target
specific frequency ranges that correspond to perceptual qualities
designers usually think about:

- **Low band** (~200–250 Hz) — body, warmth, weight
- **Mid band** (~500 Hz–2 kHz) — character, bite, presence
- **High band** (~4–10 kHz) — air, brightness, sparkle, sibilance

Here's what each material's curve emphasizes:

| Material | Low | Mid | High | Why |
|---|---|---|---|---|
| **Glass** | flat | slight cut @ 1k | gentle lift @ 8k | bright, neutral mids — the ring |
| **Wood** | +2 dB body @ 250 | +1.5 dB warmth @ 500 | -1.5 dB top | warm low-mid thwack, no brittleness |
| **Drywall** | flat | slight cut @ 1k | slight cut @ 8k | dulled, indoor-neutral |
| **Concrete** | +1 dB body @ 200 | +2.5 dB bite @ 1.5k | +2 dB top @ 6k | bright hard crack — the most "present" material |
| **Metal** | flat | +2 dB peak @ 2k | +1.5 dB ring @ 10k | clang + ringing overtones, narrower Q |
| **Curtain** | flat | -2 dB broad mid cut | -4 dB strong HF cut | thick fabric — bass passes, top dies |
| **Foliage** | flat | -1.5 dB broad cut | -2 dB HF cut | broadband softness, no specific resonance |
| **Air / Default** | — | — | — | neutral, no coloration (curve.is_neutral == true) |

These are STARTING POINTS chosen to be audible but not extreme.
Designers will dial them in once 6.B/6.C ship and the curves
become directly audible.

### Reading a curve in GDScript

```gdscript
var curve = Gool.material_eq_for_material(Gool.MATERIAL_CONCRETE)
# curve == {
#     "low_gain_db":  1.0,  "low_freq_hz":  200.0,
#     "mid_gain_db":  2.5,  "mid_freq_hz":  1500.0, "mid_q": 1.0,
#     "high_gain_db": 2.0,  "high_freq_hz": 6000.0,
#     "is_neutral": false
# }

if curve.is_neutral:
    # Air / Default — every band is 0 dB. Skip the EQ stage
    # entirely rather than installing a no-op chain.
    return
# else: apply the curve via Biquad effects (see below).
```

### Applying a curve right now: 3-biquad chain on a bus

Until 6.B/6.C automate this, designers apply material EQ by
authoring three Biquad effects on a bus and pushing the
returned values via `set_effect_parameter`. The three biquads
must be of types LowShelf, Peak, and HighShelf in that order.

**Step 1: author the bus in `gool/config.json`** with the three
biquad effects in place. The biquad type is structural — set
once at config time — but cutoff/Q/gain are tunable at runtime.

```json
{
  "name": "MaterialColor",
  "parent": "Sfx",
  "effects": [
    { "kind": "biquad", "biquad_type": "lowshelf",
      "cutoff_hz": 200, "q": 0.7, "biquad_gain_db": 0 },
    { "kind": "biquad", "biquad_type": "peak",
      "cutoff_hz": 1000, "q": 1.0, "biquad_gain_db": 0 },
    { "kind": "biquad", "biquad_type": "highshelf",
      "cutoff_hz": 8000, "q": 0.7, "biquad_gain_db": 0 }
  ]
}
```

Initial gains of 0 dB make the chain a no-op until the first
material is applied — safe default.

**Step 2: push curve values at runtime** when a material should
color sounds on this bus. The three biquads are at chain
indices 0, 1, 2 respectively:

```gdscript
func apply_material_eq(bus_name: String, material: int) -> void:
    var c = Gool.material_eq_for_material(material)
    if c.is_neutral:
        # Reset all three to 0 dB — silently bypasses the chain.
        Gool._runtime.set_effect_parameter(bus_name, 0, 12, 0.0)  # low gain
        Gool._runtime.set_effect_parameter(bus_name, 1, 12, 0.0)  # mid gain
        Gool._runtime.set_effect_parameter(bus_name, 2, 12, 0.0)  # high gain
        return
    # LowShelf at index 0
    Gool._runtime.set_effect_parameter(bus_name, 0,  2, c.low_freq_hz)
    Gool._runtime.set_effect_parameter(bus_name, 0, 12, c.low_gain_db)
    # Peak at index 1
    Gool._runtime.set_effect_parameter(bus_name, 1,  2, c.mid_freq_hz)
    Gool._runtime.set_effect_parameter(bus_name, 1,  3, c.mid_q)
    Gool._runtime.set_effect_parameter(bus_name, 1, 12, c.mid_gain_db)
    # HighShelf at index 2
    Gool._runtime.set_effect_parameter(bus_name, 2,  2, c.high_freq_hz)
    Gool._runtime.set_effect_parameter(bus_name, 2, 12, c.high_gain_db)
```

(Param IDs: 2 = `Biquad_CutoffHz`, 3 = `Biquad_Q`, 12 =
`Biquad_GainDb`. They live in `include/audio_engine/bus.h`'s
`EffectParameter` namespace; copy these constants into your
own GDScript if you want named symbols.)

### The Phase 6.B automatic path: zero-touch impact coloring

In v0.34.0 (this release), `Gool.play_impact_sound` handles the
parameter-pushing automatically. The designer setup:

**Step 1: author an "ImpactEq" bus** in your `gool/config.json`,
parent it under `Sfx` (or wherever you want material-colored
impacts to route), with the standard 3-biquad chain:

```json
{
  "name": "ImpactEq",
  "parent": "Sfx",
  "effects": [
    { "kind": "biquad", "biquad_type": "lowshelf",
      "cutoff_hz": 200,  "q": 0.7, "biquad_gain_db": 0 },
    { "kind": "biquad", "biquad_type": "peak",
      "cutoff_hz": 1000, "q": 1.0, "biquad_gain_db": 0 },
    { "kind": "biquad", "biquad_type": "highshelf",
      "cutoff_hz": 8000, "q": 0.7, "biquad_gain_db": 0 }
  ]
}
```

The chain starts at 0 dB across the board — a no-op until the
first impact pushes a material curve into it.

**Step 2: register your impact sounds with `target_bus_name="ImpactEq"`**
so they route through the EQ chain:

```gdscript
Gool.register_sound_definition(
    "impact.generic", true, false, 1.0, 30.0, 0.0,
    Gool.CATEGORY_SFX,
    "ImpactEq")   # target_bus_name — sends to the EQ-aware bus
```

**Step 3: call `play_impact_sound` as before.** Gool detects the
configured bus at startup, verifies the chain shape, and from
then on pushes the per-material curve before each impact plays:

```gdscript
Gool.play_impact_sound("bullet_impact", hit.position, material)
```

That's it. Concrete hits sound concrete. Wood hits sound wood.
Foliage hits sound soft. No per-emitter wiring, no extra code.

**Project setting** to point at a different bus or disable
entirely: `gool/material_eq/impact_bus` (default `"ImpactEq"`).
Set to `""` to opt out of auto-EQ — `play_impact_sound` then
behaves exactly as in v0.33.0.

**Graceful degradation.** If the configured bus doesn't exist
in your config, or doesn't have the 3-biquad chain shape, the
auto-EQ behavior silently disables on startup with a single
warning explaining what to fix. Impacts still play correctly
through whatever bus they're registered to target — they just
lack the per-material coloring.

### What you trade off

The implementation pushes EQ parameters to a single shared bus
just before each impact. This means **back-to-back impacts of
different materials share the most-recent material's coloring
for a few milliseconds** while the new params propagate (~5 ms
at typical buffer sizes). For an FPS where a player is firing
into one wall (same material per shot) this is invisible. For
the unusual case of rapid-fire alternation across materials —
e.g. a shotgun pellet pattern hitting concrete + wood + foliage
simultaneously — the last material the engine processes wins
for the overlapping tails. Practically rare; if it ever becomes
audible in a real situation, the fix is a per-emitter EQ stage
(planned for a future release if needed).

> **Deferred: per-emitter EQ.** The bus-routing approach is
> deliberately the v0.34.0 design. Per-emitter EQ would mean a
> new per-voice DSP chain in the engine — multi-file engine
> surgery touching `EmitterDescriptor`, `VoiceSource`, the
> mixer, and the create-emitter path. The right time to do that
> work is when there's evidence from real gameplay that the
> shared-bus trade-off is audibly hurting something. Until then
> it stays on the deferred list. If you hit a case where it
> matters, file an issue describing the symptom (which
> materials, what cadence, what the player should be hearing
> vs. what they actually hear) and the per-emitter path
> graduates from deferred to scheduled.

### When you'd reach for the manual API

The manual `set_effect_parameter`-driven path from earlier in
this section is still the right tool when:

- You want EQ to follow a non-standard event (player crouches
  behind a concrete pillar → push the concrete curve onto an
  ambient bus that isn't the impact bus).
- You're crossfading between two materials' curves smoothly
  (lerp the gain values yourself; the cutoff frequencies don't
  need to crossfade because they're frequency-domain anchors,
  not amplitudes).
- You want material coloring on something that isn't an impact
  sound — `Gool.apply_material_eq_to_bus(bus_name, material)`
  is the explicit form, with the same authoring contract
  (first 3 biquads on the bus are LowShelf / Peak / HighShelf).

### What's coming

**Phase 6.D** will add a realism multiplier (0..2) — same
pattern as the v0.31.0 occlusion intensity dial. Gentle
defaults (probably 0.5–0.7), with headroom for cinematic or
surreal amplification.

**Phase 6.E** (eventually) will be a designer authoring tool
— inspector-side EQ curve editor on a `GoolAudioMaterial`
resource, so studios can author custom material curves
without writing code.

### Phase 6.C — listener-space EQ on ReverbZone

Phase 6.C extends the material-aware `ReverbZone` (cookbook
section 13) with an optional EQ stage that colors everything
the listener hears while inside a zone. A Wood-marked zone with
`apply_listener_eq = true` doesn't just reverb like a wooden
room — it also runs every diegetic sound through the wood EQ
curve, modeling "your ears are inside a wooden box."

This is a **strong editorial effect** — more dramatic than
reverb alone. The default is `apply_listener_eq = false`;
opt in per-zone where you want it. Good fits: cutscenes inside
a single distinctive space (a foliage clearing, a concrete
bunker), atmospheric/horror moments where you want the
environment to feel oppressive, or any zone where you want
the player to *feel* the material rather than just hear it as
backdrop.

**Setup** is the same authoring contract as 6.B's impact EQ
bus, just on a different bus — a `ListenerEq` bus with 3
biquads in LowShelf → Peak → HighShelf order. Most natural
placement is between Sfx (and other diegetic buses) and
Master, so that the listener EQ is the *last* stage of the
playback chain before output. That matches the acoustic order:
source colored by impact EQ → reverbs in space → listener
perceives through their own environmental coloring.

```json
{
  "name": "ListenerEq",
  "parent": "Master",
  "effects": [
    { "kind": "biquad", "biquad_type": "lowshelf",
      "cutoff_hz": 200,  "q": 0.7, "biquad_gain_db": 0 },
    { "kind": "biquad", "biquad_type": "peak",
      "cutoff_hz": 1000, "q": 1.0, "biquad_gain_db": 0 },
    { "kind": "biquad", "biquad_type": "highshelf",
      "cutoff_hz": 8000, "q": 0.7, "biquad_gain_db": 0 }
  ]
}
```

Re-parent your existing diegetic buses (Sfx, Ambience) under
`ListenerEq` rather than directly under Master. Music, UI,
Dialogue, and Voice should *not* be re-parented — non-diegetic
sound shouldn't get colored by the listener's physical space.

**Project setting** `gool/material_eq/listener_bus` (default
`"ListenerEq"`) — same opt-out pattern as 6.B. Set to `""` to
disable the listener-EQ feature entirely (existing zones with
`apply_listener_eq=true` will silently skip the EQ ramp).

**Zone usage:**

```gdscript
# In the inspector, on a ReverbZone:
material:            5         # Concrete
apply_listener_eq:   true      # color everything you hear, too
```

That's the entire workflow. On listener entry, the zone ramps
both reverb parameters *and* the listener EQ in lockstep over
`transition_ms`. On exit, both ramp back to neutral.

### Phase 6.C — what trades off

The same shared-bus model as 6.B: listener EQ values are
pushed to a single `ListenerEq` bus, so only one zone's EQ is
active at a time. With multiple `apply_listener_eq=true` zones
in proximity, the most recently entered wins (consistent with
the rest of ReverbZone's overlapping-zone behavior, which we
already document as a v0.32.0 known limitation).

Neutral-material zones (`MATERIAL_DEFAULT`, `MATERIAL_AIR`)
skip the EQ ramp entirely even when `apply_listener_eq=true`.
A Default-material zone applies its reverb but doesn't touch
the EQ bus — which means if another zone previously set EQ
coloring, that coloring persists until another non-neutral
zone overrides or this zone exits. This is intentional: the
listener EQ is a positive editorial choice, not a state that
every zone resets.

### Phase 6.D — the realism multiplier

Phase 6.D adds a single dial — `gool/material_eq/intensity` — that
scales every per-material EQ gain uniformly. Defaults to 1.0
(curves as defined in `MaterialEqByMaterial`). Below 1.0 softens
the coloring; above 1.0 amplifies it. The dial affects both 6.B
impact EQ and 6.C listener EQ in lockstep, so the two stages stay
in proportion no matter where the slider sits.

This is the same shape as v0.31.0's `gool/occlusion/intensity`
dial — two parallel knobs that designers and players can use to
trade physical accuracy against gameplay clarity:

| Intensity | Feel |
|---|---|
| **0.0** | EQ disabled — every material sounds the same tonally (still occluded, still reverbs differently) |
| **0.5** | gentle — material is a flavor, not a dominant texture. Good clarity-first default for competitive shooters |
| **1.0** | realistic — the v0.33.0 table values, physically modeled |
| **1.5** | amplified — material identity reads strongly, good for atmospheric games where the *world* matters more than the *fight* |
| **2.0** | surreal — caricatured coloring; horror, dream sequences, intentionally unreal spaces |

Cutoff frequencies and Q values are NOT scaled — they're
frequency-domain anchors, not amplitudes. Only the three gain_db
values per curve get multiplied. This means turning the dial
preserves the *shape* of each material's spectral fingerprint
while changing its *prominence*.

**Set at project load:** Project Settings → Gool → Material Eq →
Intensity. The setting registers automatically on first run with
default 1.0; you'll see it appear in the editor's project settings
UI after the first project open with gool v0.36.0+.

**Adjust at runtime** (e.g. from a player audio settings menu —
Phase 4):

```gdscript
# Player picked "gentle EQ" in the audio settings menu:
Gool.set_eq_intensity(0.5)

# Or read the current value to populate a slider:
var current = Gool.get_eq_intensity()
```

Changes take effect on the next impact play (for 6.B) and the
next zone enter/exit (for 6.C). Currently-active ramps aren't
rescaled retroactively — the change applies from here forward.

**At intensity 0.0**, the auto-EQ paths still execute (no perf
saving), but every gain pushed to the biquads is 0 dB. The
chains pass signal through cleanly with no coloration. If you
want to actually skip the EQ machinery entirely for perf, set
`gool/material_eq/impact_bus` and `gool/material_eq/listener_bus`
to empty strings instead — that disables the bus shape checks
at startup and short-circuits the per-impact apply path.

**Intended Phase 4 hookup:** the player audio settings menu will
expose this as a "Material EQ intensity" slider with discrete
presets ("Subtle / Realistic / Amplified") behind the raw 0..2
scrubber. The slider's on_value_changed handler calls
`Gool.set_eq_intensity(value)`; that's the entire integration.

### What's coming next

**Phase 6.E** — designer authoring tool for custom material
curves. Likely a `GoolAudioMaterial` resource with an inspector
EQ-curve editor, so studios can author their own materials
beyond the built-in 9-entry taxonomy.

## 15 — Player audio settings menu (Phase 4) ✨ Convenience

### What ships

Phase 4 closes a gap that's been open since v0.31.0: the engine
has had volume / occlusion / EQ controls all along, but no
standard pattern for letting *players* adjust them in-game.
v0.37.0 ships two pieces:

1. **`GoolAudioSettings`** — a static-methods helper (in
   `addons/gool/audio_settings.gd`) that bridges three layers:
   - On-disk save (`user://gool_audio_settings.cfg`, ConfigFile
     format, survives between sessions)
   - The Gool autoload's runtime state
   - Whatever UI you build
2. **`GoolAudioSettingsPanel`** — a reference UI prefab (in
   `addons/gool/prefabs/gool_audio_settings_panel.gd`) that
   builds a working settings menu in code: volume sliders for
   Master + each standard bus, an occlusion toggle, occlusion
   intensity slider, material EQ intensity slider, reset button.

Most projects will use the prefab as-is; serious projects will
copy the prefab and restyle, or write fully custom UI calling
the static helpers directly.

### What settings the panel covers

```
Volumes
  Master           [────────●────] +0.0 dB
  SFX              [────────●────] +0.0 dB
  Music            [────────●────] +0.0 dB
  UI               [────────●────] +0.0 dB
  Voice            [────────●────] +0.0 dB
  Dialogue         [────────●────] +0.0 dB
  Ambience         [────────●────] +0.0 dB

Acoustic
  Occlusion enabled  ☑
  Occlusion intensity  [─────●─────] 1.00
  Material EQ intensity [─────●─────] 1.00

  [ Reset to defaults ]  [ Close ]
```

The volume sliders range -60 to +6 dB with 0.5 dB steps. Intensity
sliders are 0..2 with 0.05 steps (the natural range for the two
Phase 5.2 / 6.D realism dials).

The "Reset to defaults" button calls `GoolAudioSettings.reset_to_defaults()`
which restores hard-coded factory defaults (everything at unity /
on). Different from "reset to project defaults" — if you want that
behavior, write your own reset that reads the project settings
table instead.

### The simplest possible integration

In your pause menu or options scene:

```gdscript
extends Control  # your existing options menu

func _on_audio_button_pressed() -> void:
    var panel = GoolAudioSettingsPanel.new()
    panel.closed.connect(panel.queue_free)  # remove when done
    add_child(panel)
```

That's the whole hookup. The panel:
1. Loads the on-disk save (or factory defaults if no save yet)
2. Applies those values to the Gool runtime so the player hears
   them
3. Builds its UI, populates sliders from the loaded settings
4. Wires each slider to apply live + save on drag release

Open it, close it, the panel is self-contained. No autoload
required on top of the existing Gool autoload.

### Startup integration — apply saved settings before the player hears anything

Drop one line into your game's boot scene (or main menu's
`_ready`):

```gdscript
func _ready() -> void:
    await GoolAudioSettings.load_and_apply()
    # ... continue with game startup
```

This reads the save file and pushes every value into the engine
before any sound plays. Without this, the player would hear the
project setting defaults for the first frame of audio, then see
the saved values "snap into place" when they later open the
menu — small but jarring.

### Writing custom UI

If the prefab's layout doesn't match your game's style, ignore
it entirely and call the static helpers from whatever UI you
build:

```gdscript
class_name MyCustomAudioMenu
extends Control

var _settings: Dictionary = {}

func _ready() -> void:
    _settings = GoolAudioSettings.load_from_disk()
    await GoolAudioSettings.apply_to_runtime(_settings)
    _populate_my_sliders()

func _on_master_slider_changed(value: float) -> void:
    _settings.volumes.master_db = value
    GoolAudioSettings.apply_to_runtime(_settings)  # live preview

func _on_master_slider_drag_ended(_changed: bool) -> void:
    GoolAudioSettings.save_to_disk(_settings)
```

The two-step pattern — apply on every tick, save on drag end —
gives the player a responsive live preview while not hammering
the disk. The reference panel uses this exact pattern internally.

### What's saved, where it saves

On-disk format is Godot's ConfigFile, INI-style:

```ini
[volumes]
master_db=-3.0
sfx_db=0.0
music_db=-6.0
ui_db=0.0
voice_db=0.0
dialogue_db=0.0
ambience_db=0.0

[occlusion]
enabled=true
intensity=0.7

[material_eq]
intensity=1.0
```

Path: `user://gool_audio_settings.cfg`. The exact location on
disk depends on platform — `%APPDATA%\Godot\app_userdata\<project_name>\`
on Windows, `~/.local/share/godot/app_userdata/<project_name>/`
on Linux. Players can hand-edit this file if needed but the
expectation is they'll use the menu.

If the file is missing or malformed, `load_from_disk()` returns
factory defaults silently — no error, no crash, just the
first-run experience.

### Backward compatibility

`load_from_disk()` falls back to `DEFAULTS` for any missing
section or key. This means save files from older versions of
gool keep working when new settings get added — the new fields
inherit their defaults on first load, get saved correctly on
next slider release.

So you can keep iterating on the menu's scope (adding new
sliders, deprecating old ones) without breaking player saves.

### What this release doesn't cover

Intentionally out of scope for v0.37.0:

- **Audio device selection** (output picker). Godot's
  `AudioServer.get_output_device_list()` would be the API, but
  it's gnarly enough to deserve its own pass.
- **Surround / headphones toggle.** Same — Godot exposes the
  channel count but auto-detection is platform-dependent.
- **Subtitles / language.** Out of scope (different system).
- **Per-channel mute.** The volume sliders go to -60 dB which is
  effectively silent; a dedicated mute toggle could be added
  per-bus but the slider-to-minimum pattern works for most
  cases.
- **Hearing accessibility presets.** "Boost speech" / "reduce
  bass" presets would be a layer above this — a preset would
  set multiple sliders at once. Easy to add: write a function
  that builds a settings dictionary and calls
  `save_to_disk` + `apply_to_runtime`.

These are all candidates for a Phase 4.1 release if/when the
project demands them.

## Where to find more

- [`docs/godot_quickstart.md`](godot_quickstart.md) — start here
  if you haven't installed the addon yet.
- [`docs/multiplayer.md`](multiplayer.md) — full integration
  patterns with Steam GameNetworkingSockets, ENet, raw UDP.
- [`docs/asset_pipeline.md`](asset_pipeline.md) — designer-driven
  sound banks (the JSON file that holds all your sound names,
  attenuation curves, bus routing, group selection).
- Main [`README.md`](../README.md) — full feature catalog with
  measured numbers and design intent.
