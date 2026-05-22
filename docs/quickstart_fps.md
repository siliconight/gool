# FPS quickstart — building multiplayer FPS audio with gool

> **Who this is for:** Godot 4.x devs building a 3D first-person
> shooter — single-player or multiplayer — who want a working audio
> loop in their first session with gool. Covers local + remote
> player audio, footsteps with material lookup, per-room reverb,
> music states, dialogue ducking, and voice chat scaffolding.
>
> **What you'll have at the end:** a player who fires a positioned
> gunshot with 0ms local latency, hears teammates' fire at the
> right network position, walks across different surfaces with
> appropriate footstep sounds, transitions between reverb zones
> as they enter rooms, and ducks under NPC barks automatically.
>
> **Time:** ~45 minutes if you're following along with code.

## Prerequisites

- Godot 4.2 or later
- A new or existing Godot project
- gool installed at `res://addons/gool/` (Project Settings ▸
  Plugins ▸ enable gool)

The plugin scaffolds `res://sounds/{sfx,music,voice,ambience,ui}/`
folders and writes a default `res://gool/config.json` on first
enable. Three autoloads register themselves: `Gool`,
`DialogueDirector`, `GoolMultiplayerBridge`.

## Step 0 — Start with the FPS-ready config

The default `gool/config.json` ships with a generic bus topology
that's fine for getting started but doesn't include a reverb
chain. For an FPS, replace it with the FPS template:

```bash
cp res://addons/gool/templates/config_fps.json res://gool/config.json
```

What's different vs default:

- **Sfx parent bus** with the v0.47.0 reverb chain pre-wired:
  `[HPF 20Hz bypass] → [Reverb] → [LPF 22kHz bypass]`. ReverbZone
  prefabs default to `bus_name = "Sfx"` so they automatically
  target this chain. The HPF/LPF biquads start in bypass and are
  shaped per-zone by the EQ-shaping feature when a ReverbZone is
  active.
- **LocalSfx + RemoteSfx** both route through Sfx so local and
  remote audio share the same world reverb.
- **Sidechain compressors** wired so:
  - Dialogue ducks Music + LocalSfx + RemoteSfx (~6-10 dB)
  - LocalSfx ducks Music + RemoteSfx (your gun wins over teammate audio)
- **Voice** bus is separate and never ducked (intelligibility
  priority).

You can audit and tweak the values via the editor mixer dock.

## Step 1 — Drop in your sounds

Put your `.wav` files into the scaffolded folders by category:

```
res://sounds/
├── sfx/         # gunshots, footsteps, reloads, impacts
├── music/       # combat / explore / boss tracks
├── voice/       # voice chat (typically empty — fed at runtime)
├── ambience/    # room tone, wind, distant gunfire
└── ui/          # menu clicks, hit markers, kill confirms
```

The `GoolFolderSoundBank` resource at `res://sounds/bank.tres`
auto-scans these folders and registers each file by its basename.
A file at `res://sounds/sfx/gunshot.wav` becomes the sound name
`"gunshot"`.

For per-sound metadata (volume, pitch, loop, distance, target
bus), you have two options:

### Option A — bank.tres entry per sound (recommended)

Open `res://sounds/bank.tres` in the inspector, add a
`SoundDefinition` entry for each sound that needs custom settings.
This is the data-driven path and survives reloads.

### Option B — register at runtime

```gdscript
func _ready() -> void:
    Gool.register_sound_definition(
            "gunshot",          # name
            true,               # spatialized
            false,              # looping
            1.0,                # min_distance (meters)
            80.0,               # max_distance (meters)
            0.0,                # loop_crossfade_ms
            Gool.CATEGORY_SFX,  # category
            "LocalSfx",         # target_bus_name
            true)               # occlusion_enabled
```

> **Pitfall.** `register_sound_definition` takes 9 positional
> arguments. Argument order is easy to get wrong silently — if
> you skip the `target_bus_name` argument, your gunshot still
> plays but routes to the wrong bus and skips the sidechain
> compressor. Double-check the arg list against the example
> above. A Dictionary form is on the roadmap.

## Step 2 — Add the listener (player's ears)

The listener tracks where audio is heard from. Add a
`GoolListener3D` as a child of your player's `Camera3D`:

```
FpsPlayer (CharacterBody3D)
├── CollisionShape3D
├── Camera3D
│   └── GoolListener3D  ← here
└── (weapon, etc.)
```

`GoolListener3D._physics_process` automatically tracks the
camera's global transform and pushes position + forward to the
runtime each frame. **No code needed.** Without a listener, all
3D audio plays at the world origin and spatialization is broken.

## Step 3 — Hello, gunshot

```gdscript
# weapon.gd
func _on_fire() -> void:
    Gool.play_3d("gunshot", muzzle.global_position, 200)
```

Run the game, fire the weapon, hear a positioned gunshot. If you
don't hear it:

1. Check `res://sounds/sfx/gunshot.wav` exists and is a valid
   16-bit PCM WAV.
2. Check `Gool.has_sound("gunshot")` returns true.
3. Watch the editor Output panel for `[gool]` warnings.
4. Press F3 in-game to toggle the debug overlay — it shows live
   voice count, master peak, and drops.

## Step 4 — Multiplayer: replicated gunshots

For other players to hear your gunshot, use
`GoolMultiplayerBridge.fire_predicted_event`:

```gdscript
# weapon.gd — multiplayer-aware
func _on_fire() -> void:
    if not is_multiplayer_authority():
        return  # only the firing client predicts
    var pid: int = GoolMultiplayerBridge.fire_predicted_event(
            "gunshot",
            muzzle.global_position,
            200)
    _pending_predictions.append(pid)

# If the server rejects this shot (out of ammo, lag, anti-cheat):
func _on_shot_rejected(prediction_id: int) -> void:
    Gool.cancel_predicted_event(prediction_id)
```

What the bridge does:

1. **Plays locally immediately** (0ms latency on the firing client
   — hard requirement for FPS feel)
2. **Replicates via Godot's MultiplayerAPI RPC** to other peers
3. **On receiving peers**, forwards to `Gool.submit_replicated_event`
   which schedules the same play at the same network position

The bridge is transport-agnostic. With Godot's `MultiplayerAPI` it
"just works." If you switch to Steam P2P via `SteamMultiplayerPeer`,
it still works without code changes. For a custom protocol, set
`GoolMultiplayerBridge.transport_mode = CUSTOM` and connect the
`event_should_be_sent` signal to your own network layer.

See `docs/networking_bridge.md` for the full API.

## Step 5 — Moving emitters (engines, drones)

For a sound source that follows a moving body (vehicle engine,
enemy chase drone), use a long-lived emitter:

```gdscript
# vehicle.gd
var _engine_handle: int = -1

func _ready() -> void:
    _engine_handle = Gool.create_emitter(
            "engine_loop",
            global_position,
            true,    # looping
            200.0)   # max_distance

func _physics_process(_delta: float) -> void:
    GoolMultiplayerBridge.update_replicated_transform_networked(
            _engine_handle,
            global_position,
            -global_transform.basis.z,
            linear_velocity)

func _exit_tree() -> void:
    Gool.destroy_emitter(_engine_handle, 200.0)  # 200ms fade
```

The bridge throttles transform updates to a configurable rate
(default 20 Hz network, 60 Hz local) so you don't flood the
network with per-frame transforms.

## Step 6 — Footsteps with material lookup

For surface-aware footsteps:

```gdscript
# player_footsteps.gd
@onready var raycast: RayCast3D = $FootRaycast

func _on_step() -> void:
    if not raycast.is_colliding():
        return
    var collider := raycast.get_collider()
    var material_id: int = 0
    if collider:
        material_id = int(collider.get_meta("gool_audio_material", 0))
    Gool.play_impact_sound(
            "footstep",
            raycast.get_collision_point(),
            material_id)
```

`gool_audio_material` is metadata set by the `AudioMaterialTag`
prefab. In your level, drop an `AudioMaterialTag` as a child of
each `StaticBody3D` that needs material-aware sound:

```
WoodenFloor (StaticBody3D)
├── CollisionShape3D
├── MeshInstance3D
└── AudioMaterialTag (material = Wood)
```

The tag's `@tool _ready` writes the material ID to the parent's
metadata when you save the scene, so the raycast-and-read pattern
above works without any runtime registration.

> **13 materials are built-in:** Default, Air, Glass, Wood,
> Drywall, Concrete, Metal, Curtain, Foliage, Meat, Cardboard,
> Rubber, Liquid. Custom materials aren't currently supported via
> public API — pick the closest match.

The `play_impact_sound` path applies per-material EQ (configured
per material in `gool_presets.gd`) and picks the appropriate bank
entry. So footsteps on Wood sound different from Concrete without
you authoring 13 separate sound files — one base `footstep.wav`
plus the material EQ does the work.

## Step 7 — Reverb zones

Drop a `ReverbZone` prefab as an `Area3D` in your level. Each zone
covers a room or space:

```
Level
├── Cathedral (Node3D)
│   ├── Walls, floor, etc. (StaticBody3D)
│   └── ReverbZone (Area3D)
│       └── CollisionShape3D  (fills the room interior)
└── Hallway (Node3D)
    └── ReverbZone
```

In the ReverbZone inspector, set `material` to one of the
material enum values (Concrete, Wood, etc.) — this picks up the
matching preset from `GoolPresets.REVERB_*` automatically,
including:

- `decay`, `predelay_ms`, `lf_damping`, `hf_damping`, `diffusion`,
  `wet_gain_db` — the reverb tail shape
- `send_hpf_hz`, `return_lpf_hz` — v0.47.0 EQ shaping, per the
  Sound on Sound + MixingLessons recipes

The EQ shaping only fires if the bus has adjacent Biquad slots
flanking the Reverb in its effect chain. The FPS config template
(Step 0) pre-wires these slots. If you authored your own config,
see `docs/audio_design/reverb_eq.md`.

## Step 8 — Wall occlusion

You get this **for free** with the bus topology from Step 0 — no
explicit setup. Walls that are `StaticBody3D` block sound via
gool's per-emitter occlusion raycasts.

The raycast goes from emitter → listener. If it hits a
`StaticBody3D` along the way, the engine applies low-pass +
attenuation per the occlusion preset (configurable in
`config.json`).

Enable per-sound:

```gdscript
Gool.register_sound_definition(
        "gunshot", true, false, 1.0, 80.0, 0.0,
        Gool.CATEGORY_SFX, "LocalSfx",
        true)  # ← occlusion_enabled
```

Disable for sounds that shouldn't occlude (UI sounds, voice chat,
music — typically anything not spatially diegetic).

## Step 9 — Music states

Use the `MusicStateController` prefab:

```
Level
└── MusicController (MusicStateController prefab)
```

In the inspector, define your states (`"explore"`, `"combat"`,
`"boss"`) each pointing to a music sound name from your bank.

Transitions in code:

```gdscript
# When combat starts
$MusicController.transition_to("combat", 800)  # 800ms crossfade

# When combat ends
$MusicController.transition_to("explore", 1500)
```

Music routes to the Music bus, which is sidechained under
LocalSfx + Dialogue. So when you fire (LocalSfx triggers) or an
NPC barks (Dialogue triggers), the music ducks ~6-10 dB
automatically. This is the L4D2 mix shape out of the box.

For continuous intensity ramping (not just discrete states),
stems/layers are on the v0.48.0+ roadmap.

## Step 10 — Dialogue / NPC barks

```gdscript
# enemy.gd
func _on_spot_player() -> void:
    DialogueDirector.bark(
            "enemy_42",              # speaker_id (used for step-on rules)
            "enemy_alert",           # sound name from bank
            200,                     # priority (higher = louder/wins)
            2.5,                     # expected duration (seconds)
            global_position)         # 3D position
```

`DialogueDirector` routes through the Dialogue bus, which triggers
the sidechain compressors on Music + LocalSfx + RemoteSfx wired in
the FPS config. Music + SFX duck by ~6-10 dB so the bark cuts
through gunfire.

The director enforces:

- **Priority** — higher-priority barks step on lower ones
- **Step-on rules** — same speaker can't step on themselves
- **Duration** — director knows when bark is "done" without
  needing the audio to actually finish

See `docs/audio_design/dialogue_setup.md` for the sidechain wiring
details.

## Step 11 — Voice chat (mic → bridge)

**This is the biggest open area.** gool ships the runtime
primitives (`register_voice_source`, `submit_voice_packet`, jitter
buffer, packet-loss tracking) but NOT mic capture or Opus encoding.
You need to integrate one of:

- **Steamworks voice** — recommended for Steam shipments. Steam
  Audio captures from the local mic, encodes, transmits via Steam
  P2P, and gives you packets to feed into
  `Gool.submit_voice_packet(player_id, packet_bytes)`.
- **Godot AudioStreamMicrophone + custom encoder** — for non-Steam
  shipments. Capture via AudioStreamMicrophone, encode with an
  Opus addon, transmit via your network layer (ENet, custom UDP),
  feed into `submit_voice_packet` on the receiving side.

Minimal wiring once you have packets flowing:

```gdscript
# On peer_connected for each peer
Gool.register_voice_source(peer_id)

# When a voice packet arrives from peer_id
Gool.submit_voice_packet(peer_id, packet_bytes)

# Monitor health
var jitter_ms: float = Gool.get_voice_jitter_ms(peer_id)
var loss_ratio: float = Gool.get_voice_packet_loss_ratio(peer_id)
```

Voice routes to the dedicated Voice bus from the FPS config —
intentionally NOT ducked, intelligibility priority.

> **There's no working mic-capture example shipping with gool
> yet.** This is on the v0.48.0+ roadmap (see audit). For now,
> integrate Steamworks if you're shipping on Steam.

## Step 12 — Tune the mix

Open the **mixer dock** in the editor (Project ▸ Tools ▸ gool ▸
mixer, or the dock should auto-appear). The dock shows per-bus
faders + effect parameters, real-time updating during F5.

Workflow:

1. F5 your scene
2. Open the dock alongside the running game
3. Drag faders, tweak effect parameters
4. Changes apply instantly to the running engine (live)
5. Changes also auto-save to `res://gool/config.json` (debounced)
6. When you want a clean rewrite, press **Save Mix to Config**
   (v0.48.0)

The **Live Stats panel** at the bottom of the dock shows during
F5:

- Voice count (active / dropped / category breakdown)
- Master peak / pre-mix peak (dBFS)
- Render jitter
- Voice chat health (jitter, packet loss per registered player)

Watch for **drops** — when voice count exceeds the per-category
limit (default ~32-64 SFX voices), low-priority sounds get
evicted. Drops show up in the stats panel.

## Step 13 — Performance budgets

Defaults you should know:

| Category | Default voice limit | Typical use |
|---|---|---|
| `SFX` | 64 | gunshots, impacts, world sounds |
| `MUSIC` | 4 | music tracks (usually 1-2 active) |
| `VOICE` | 16 | voice chat players |
| `AMBIENCE` | 8 | room tones, looping environment |
| `UI` | 8 | menu / HUD sounds |
| `DIALOGUE` | 4 | NPC barks |

When you exceed the limit, lower-priority sounds get evicted to
make room. Priority is the third argument to `play_3d`.

For a 32-player MP FPS with all players firing, you'll be at the
SFX limit constantly. Strategies:

- Increase `SFX` voice limit in `config.json`
- Use `RemoteSfx` with lower priority for teammate audio (so
  yours always wins)
- Use `AudioRelevancyFilter` to skip far-away sounds before they
  consume a voice (see `audio_relevancy_filter.gd`)

## Try-this-now: the audition example

Open `examples/audition/` in Godot, press F5. Walk through the
hub-and-spokes scene to hear all the features above in one
runnable demo:

- 4 enclosed rooms + 1 outdoor hub, each with its own reverb
  preset and EQ shaping
- 2 material targets per room (8 total + 13 in the hub gallery)
- LMB fires impact raycast with material-aware sound
- B triggers a synthesized DialogueDirector bark with sidechain
  ducking
- K / L capture / apply mix snapshots
- F3 toggles the debug overlay

The audition's `audition_builder.gd` is heavily commented and
mirrors most of the patterns in this doc. It's the canonical
reference for "what does this look like working?"

## Common pitfalls

| Symptom | Cause | Fix |
|---|---|---|
| Sound plays but at wrong position | Listener missing or not under Camera3D | Add `GoolListener3D` as Camera3D child |
| Sound doesn't play at all | Bank doesn't have a definition for that name | Check `Gool.has_sound(name)` |
| Sound is too quiet / muffled | Bus chain has an aggressive low-pass biquad with cutoff < 5kHz | Check `gool/config.json` for biquad cutoffs |
| Reverb doesn't audibly change between zones | Reverb effect missing from Sfx bus chain in config | Use the FPS template from Step 0 |
| EQ shaping isn't audible per-zone | No biquad slots flanking Reverb in bus chain | Use the FPS template, or see `audio_design/reverb_eq.md` |
| Replicated gunshots lag for the firing client | Calling `Gool.play_3d` AND bridge fire | Just use the bridge — it plays locally + replicates |
| Voice chat is silent | No mic capture wired (gool doesn't ship one) | Integrate Steamworks voice or AudioStreamMicrophone + Opus |
| Crash on `play_3d` | (Pre-v0.46.1 bug) | Update to v0.46.1+ |
| `register_sound_definition` argument order silently wrong | 9 positional args is footgun | Compare against this doc's example carefully |

## Where to go next

- **`docs/networking_bridge.md`** — the bridge's full API (host
  migration, transport modes, custom network layer integration)
- **`docs/audio_design/reverb_eq.md`** — the v0.47.0 EQ shaping
  recipes (Sound on Sound + MixingLessons articles)
- **`docs/audio_design/dialogue_setup.md`** — the sidechain
  ducking wiring + tuning guide
- **`docs/terminology.md`** — gool's vocabulary vs FMOD / Wwise
- **`docs/asset_pipeline.md`** — the sound bank schema in detail
- **`examples/audition/`** — runnable feature showcase (Step 12)
- **`examples/coop_shooter_template/`** — a fuller multiplayer
  example with weapon firing + AI bots + combat music

If you hit something this doc doesn't cover, open an issue with
the "fps-quickstart" label and we'll patch it in.
