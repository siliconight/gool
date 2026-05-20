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
