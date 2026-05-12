# Cookbook

Ten one-screen recipes for things you'll actually need. Each
recipe is under 10 lines of GDScript and assumes you've got the
gool addon installed and enabled (see
[`godot_quickstart.md`](godot_quickstart.md) if you haven't).

Every snippet expects `var Gool := get_node("/root/Gool")` somewhere
above it — the autoload the plugin set up for you.

## 1 — Play a sound at a position (fire and forget)

For one-shot SFX like a coin pickup or a UI ping.

```gdscript
# Register the bytes once (e.g. in _ready).
Gool.register_sound_from_file("pickup", "res://sfx/pickup.wav")

# Fire it at a world position from anywhere, any time.
Gool.play_sound_at("pickup", global_position)
```

The emitter is owned by the engine, plays to the end, and is
cleaned up automatically. No handle to track.

## 2 — Looping emitter that follows an object

For a vehicle engine, a campfire, anything that lives in the
world and needs continuous audio.

```gdscript
@onready var _emitter := $AudioEmitter3D    # drag the prefab into the scene

func _ready() -> void:
    _emitter.sound_name = "engine_loop"
    _emitter.looping = true
    _emitter.autoplay = true
    # The prefab keeps the engine emitter's transform synced with
    # this node's global_transform automatically each frame.
```

When the parent moves, the audio follows. When the parent is
queue_freed, the emitter is unregistered cleanly with a 20 ms
fade-out.

## 3 — Mute a remote player's voice (v0.13.0)

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

## 4 — Per-player volume slider (v0.13.0)

A UI slider that quiets a too-loud teammate without fully muting.
Range `[0.0, 4.0]`; values above 1 boost above unity.

```gdscript
func on_volume_slider_changed(peer_id: int, value: float) -> void:
    Gool.set_voice_source_volume(peer_id, value)
```

That's the whole hookup. The engine scales the decoded int16 PCM
on the control thread before pushing to the mixer.

## 5 — Bandwidth budget for mobile players (v0.13.0)

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

## 6 — Adaptive music with crossfade

Two music tracks, switch between them with an equal-power
crossfade. No clicks, total energy held within ±0.3% through the
transition.

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

## 7 — Footstep sounds with surface detection

The `FootstepSurfacePlayer` prefab raycasts down from each step,
checks the hit node's groups, and picks a sound bank entry. You
configure the mapping in the inspector.

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

## 8 — Reverb when entering a room (signal-driven)

`ReverbZone` is an Area3D that fires signals when the listener
enters or leaves. Use those to swap the reverb send level.

```gdscript
@onready var _zone := $ReverbZone

func _ready() -> void:
    _zone.listener_entered.connect(_on_listener_entered)
    _zone.listener_exited.connect(_on_listener_exited)

func _on_listener_entered() -> void:
    # Heavier reverb wet level when inside the cave.
    Gool.set_bus_gain("Reverb", 0.0)    # 0 dB = full wet

func _on_listener_exited() -> void:
    Gool.set_bus_gain("Reverb", -20.0)  # damp the tail back down
```

The zone monitors any node in the `gool_listener` group, so add
`add_to_group("gool_listener")` to your camera or player.

## 9 — Duck SFX while dialogue plays

Configure two compressors in your bus graph: one on Music, one on
SFX, both sidechained to the Dialogue bus. When dialogue plays,
both other buses duck automatically — no per-event code needed.

```gdscript
# In your bus config (res://gool/config.json):
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

# Then route sounds to the right bus when registering them:
var def := SoundDefinition.new()
def.target_bus = "Dialogue"
# ... etc
```

When someone in the game starts talking, music and SFX dip in
volume automatically and recover when speech ends. This is the
L4D2 ducking pattern; the engine documentation walks through the
math.

## 10 — Cancel a predicted sound

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
