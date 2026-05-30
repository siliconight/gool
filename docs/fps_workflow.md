# Wiring gool into a Godot 4 online multiplayer FPS

This is the end-to-end recipe for an audio-equipped, online-multiplayer
first-person shooter built on Godot 4 + gool. It assumes:

- 4-or-so player coop, but the patterns scale to 8 or 16 with care
- Networking via Godot's high-level `MultiplayerAPI` (ENet by default,
  or Steam's P2P transport via a third-party plugin) — the audio
  patterns are transport-agnostic
- A first-time-3D-FPS developer who wants the audio side to be
  mostly solved by tools, leaving cognitive budget for the gameplay

If your project isn't networked, you don't need most of this — the
cookbook's individual recipes cover single-player audio fine. The
multiplayer story is where gool earns its keep, and this guide is
about that story.

---

## The architectural rule, stated once

> **Gameplay emits events. Networking transports events. Audio
> interprets events. No gameplay system directly owns final sound
> playback.**

Concretely:

- A weapon firing produces an **event**, not a sound. The event has
  a name (`weapon_fire`), a position, a category, a sound id, and
  enough metadata for authority decisions.
- The event flows through the **network layer** under multiplayer's
  authority rules — locally for the firing player (instant feedback),
  replicated to remote peers (positioned 3D playback at their end).
- Audio playback is the **last step**, owned by gool. The same event
  feeds local playback and remote replicated playback; gool doesn't
  know or care which path got it there.

This separation is the difference between an audio system that scales
to "30 enemies firing simultaneously across 4 networked players" and
one that doesn't. Skip the rule, ship rubber-band audio.

---

## What gool covers and what it doesn't

Before wiring anything: be clear about the boundary. gool is audio
middleware, not a game framework. The split looks like:

**Your game owns:**
- `CharacterBody3D` player controller + input handling
- Weapon scenes (model, animation, hit detection, ammo state)
- Network authority decisions (who can fire, hit-validation, etc.)
- `MultiplayerSpawner` for replicated entities (players, enemies,
  projectiles)
- The transport layer (ENet, WebRTC, Steam P2P) — gool doesn't pick
  one for you

**gool owns:**
- Spatial 3D audio (listener, emitter, distance falloff, panning)
- Replicated audio events (the `NetworkedAudioEvent` pattern, with
  three replication modes for different sound priorities)
- Sound bank loading + category routing → bus topology
- Voice chat (Opus codec, jitter buffer, packet-loss concealment,
  encryption)
- Footstep surface awareness (`AudioMaterialTag` + `FootstepSurfacePlayer`)
- Reverb zones, occlusion via `GodotGeometryQuery`
- Music state machine (`MusicStateController` + `GoolMusicState`)
- The mixer dock for authoring bus topology

The intersection is the **integration surface**. Most of this guide
is about that surface.

---

## Step 1 — Scaffold the audio foundation

Open your game project, create or open your gameplay scene (the one
that's a 3D Node3D root). Then:

**Project → Tools → gool → Scaffold FPS audio setup (recommended starter)**

This drops six prefabs into your scene root (the v0.82.0 verb):

| Prefab | What it does |
|---|---|
| `GoolListener3D` | Becomes the listener; reparent to your camera/head |
| `GoolSoundBankLoader` | Loads `res://sounds/bank.tres` at scene start |
| `FootstepSurfacePlayer` | Surface-aware footsteps; reads `AudioMaterialTag` |
| `ReverbZone` | Placeholder; resize/position for indoor areas |
| `GoolDebugOverlay` | Dev-time diagnostics; delete before shipping |
| `MusicStateController` | Music state machine for heist phases |

Save the scene. F5 → you should hear a test beep + see the debug
overlay. If you don't, refer to `first_enable_verification.md`.

**One important post-scaffold step**: reparent `GoolListener3D` from
the scene root onto your camera or head node so the listener tracks
the player's view. The default placement at scene root works for a
fixed-camera test, but for FPS you want it bound to look direction:

```
Player (CharacterBody3D)
├─ Head (Node3D)
│  ├─ Camera3D
│  └─ GoolListener3D   ← move it here
└─ ...
```

---

## Step 2 — Configure your bus topology

The scaffolded scene works but uses defaults. For a coop FPS the
canonical topology looks something like:

```
Master
├─ SFX → Master
│  ├─ Weapons → SFX        (gun fire, explosions, mechanical)
│  ├─ Foley → SFX          (cloth, footsteps, interactions)
│  └─ Impacts → SFX        (bullets hitting surfaces)
├─ Voice → Master
│  ├─ Player → Voice       (your crew's voice chat — Step 8)
│  └─ NPC → Voice          (enemy yelling, civilian screams)
├─ Music → Master
│  └─ Diegetic → Music     (radios, jukeboxes — positional music)
└─ Ambient → Master
```

Two ways to author this:

**Option A — start from the FPS template, then customize:**
- Project → Tools → gool → Reset config from FPS template
- Use the mixer dock to rename / reparent / add buses to taste

**Option B — author entirely in the mixer dock:**
- Bottom panel → "gool Mixer"
- Add buses with the + Add Bus column
- Right-click any non-Master bus → Change parent...

Either way, the bus topology is one file (`res://gool/config.json`).
Check it in to version control; every player's client uses the same
topology for consistent mix.

---

## Step 3 — Register your sounds in category folders

The bank system maps folder paths to categories. If your `res://sounds/`
directory looks like this:

```
res://sounds/
├─ bank.tres                (the GoolFolderSoundBank resource)
├─ sfx/
│  ├─ weapons/
│  │   ├─ tec9_fire.wav
│  │   ├─ tec9_reload.wav
│  │   └─ mac10_fire.wav
│  ├─ foley/
│  │   ├─ footstep_concrete_01.wav
│  │   └─ footstep_wood_01.wav
│  └─ impacts/
│      ├─ bullet_concrete.wav
│      └─ bullet_metal.wav
├─ voice/
│  ├─ player/                (voice chat — handled differently, Step 8)
│  └─ npc/
│      ├─ cop_freeze.wav
│      └─ civilian_scream.wav
└─ music/
    ├─ planning.wav
    └─ alarm.wav
```

…then drop in those .wav files, save the bank.tres in the inspector
(or right-click → Reimport in the FileSystem dock), and the categories
auto-populate from the folder names.

To play a categorized sound from anywhere in your game code:

```gdscript
# At the top of any script, or via autoload bootstrap:
@onready var Gool := get_node("/root/Gool")

# Fire-and-forget categorized playback:
Gool.play_one_shot("tec9_fire", global_position, "weapons")
```

The third arg (`"weapons"`) is the category. `config.json`'s
`category_routing` maps it to a bus. Sounds in that category get
playback parameters from your config (default radius, priority, etc.).

---

## Step 4 — Wire local prediction (instant feedback for the firing player)

When the local player fires their gun, they should hear the sound
**instantly** — no network round-trip, no waiting for replication.
This is "input feel" territory; latency here is felt as input lag.

In your weapon script:

```gdscript
# weapon.gd
extends Node3D

@export var fire_sound: StringName = &"tec9_fire"
@export_category("Audio")
@export var category: StringName = &"weapons"

@onready var Gool := get_node("/root/Gool")

func fire() -> void:
    # Local prediction: play immediately for the firing player.
    # No network call yet — that's the next step.
    Gool.play_one_shot(fire_sound, global_position, category)
    # ...spawn muzzle flash, deduct ammo, trigger animation, etc.
```

This works for the firing player only. The other 3 peers in the
session don't hear anything yet — that's Step 5.

---

## Step 5 — Wire remote replication (other players hear your shots)

To make remote peers hear the sound at the firing player's position,
emit a **networked audio event**. The `NetworkedAudioEvent` prefab is
gool's primitive for this:

```
Player (CharacterBody3D)
├─ Head
│  └─ Camera3D
│     └─ GoolListener3D
├─ WeaponSocket
│  └─ Weapon
│     ├─ Muzzle (Marker3D)
│     ├─ WeaponModel
│     └─ NetworkedAudioEvent  ← add this child
```

In the weapon script, alongside the local prediction call:

```gdscript
@onready var fire_event: NetworkedAudioEvent = $NetworkedAudioEvent

func fire() -> void:
    # 1. Local prediction: instant feedback
    Gool.play_one_shot(fire_sound, global_position, category)
    # 2. Replicate to other peers — they'll play at our position
    fire_event.fire(fire_sound, global_position, category)
```

On the remote peers, `NetworkedAudioEvent` receives the RPC and plays
the sound spatially at the source position. The listener on each
remote peer handles the 3D panning + distance falloff automatically.

**Three replication modes** for different sound priorities — set on
the `NetworkedAudioEvent` node's `replication_mode` property:

- `RELIABLE` — every peer gets it, ordering preserved. Use sparingly
  (big events like "boss spawned"). High network cost.
- `UNRELIABLE` — fire-and-forget UDP. Some peers may miss it. Cheap.
  Right choice for **most weapon fire** — if one packet of bullet
  audio drops out of a hundred shots, nobody notices.
- `ORDERED_UNRELIABLE` — UDP but later events override earlier ones
  on the same channel. Good for things like "boss roar state" where
  only the latest matters.

For an FPS, default to `UNRELIABLE` for shots/impacts/footsteps,
`RELIABLE` for state changes (explosion that triggers mission
progress, boss phase change). Anti-spam rate limiting is built in;
even a player spam-firing won't flood the network.

---

## Step 6 — The Audio Event Bridge pattern (optional, when wiring gets repetitive)

After you've wired Steps 4–5 for a few weapons, you'll notice each
weapon script has the same local-then-remote two-step. That repetition
is a signal that you might want to extract a helper — call it an
`Audio Event Bridge` if you like:

```gdscript
# audio_bridge.gd — your own helper, not a gool prefab
extends Node

@onready var Gool := get_node("/root/Gool")

func fire_event_at(sound: StringName, position: Vector3,
        category: StringName, event_node: NetworkedAudioEvent) -> void:
    Gool.play_one_shot(sound, position, category)
    event_node.fire(sound, position, category)
```

Then in your weapon scripts:

```gdscript
@onready var bridge := get_node("/root/AudioBridge")

func fire() -> void:
    bridge.fire_event_at(fire_sound, global_position, category, $NetworkedAudioEvent)
```

**This is your code**, not gool's. gool deliberately doesn't ship a
canonical `AudioEventBridge` prefab because the right shape depends
on your game's specifics:

- Some games want the bridge per-player (local prediction +
  per-entity audio events)
- Some want one global bridge (single dispatch point)
- Some want NO bridge (per-weapon `NetworkedAudioEvent` children
  with the two-line inline pattern — that's fine for small games)

Build it from real friction once you've shipped 3-5 weapon scripts.
The right helpers will be obvious by then.

**Server-authority variant**: for things that need server
validation (a kill confirmation sound, for example), the firing
peer doesn't replicate directly. Instead they request the action
from the server, the server validates and broadcasts the resulting
event to everyone (including the original requester). The audio
emerges on every peer simultaneously as a side effect of the
server's authoritative state change. gool's `NetworkedAudioEvent`
supports the authoritative-peer variant — set its `authority_only`
property to true and only the multiplayer authority will trigger
playback.

---

## Step 7 — Footsteps with surface material

Footsteps are where the abstract "audio event" pattern gets real
gameplay benefit. The flow:

1. Your level geometry has `AudioMaterialTag` children on Surface
   nodes, declaring their material ("concrete", "wood", "carpet",
   "metal").
2. Player movement raycasts down each step, finds the surface
   below, reads the tag.
3. `FootstepSurfacePlayer` (already in your scaffolded scene)
   picks the right sound from the bank's `sounds/sfx/foley/`
   folder based on the material name.
4. Plays it as a local-only sound (per-player footstep audio
   doesn't usually need replication — too high-frequency, low
   importance).

The scaffold already wired this. To use it:

```gdscript
# In your player controller, on each footstep frame:
@onready var footsteps: FootstepSurfacePlayer = $FootstepSurfacePlayer

func _physics_process(_d: float) -> void:
    if _stepped_this_frame():
        footsteps.play_step_at(global_position)
```

Footsteps for **remote players** (the other 3 crew members) — you
can either replicate them as cheap `UNRELIABLE` events from each
peer, or skip them entirely. Most games skip; the cost-to-immersion
ratio is bad for high-frequency player movement noise.

---

## Step 8 — Reverb zones for indoor sections

The scaffolded scene has one `ReverbZone`. Resize and position it
to cover an interior space — say, the bank vault you're robbing.
When the listener enters the zone, the reverb characteristics
apply to all spatial audio. When they leave, it crossfades back.

Multiple zones can coexist; the listener picks the smallest
containing zone (or the highest-priority one if they overlap).

For your gangster game, plausible zones:

```
ReverbZone (vault interior)        — small, tight, metallic
ReverbZone (bank lobby)            — medium, marble/glass
ReverbZone (back office)           — small, carpeted
ReverbZone (parking garage)        — large, concrete
ReverbZone (sewer escape route)    — narrow, wet
```

The street (outdoor) gets no reverb zone — falls through to the
config.json's default reverb characteristics.

Zones replicate naturally: every peer's listener moves
independently through their own copy of the geometry, so each
peer hears their own correct reverb. No network traffic for zone
transitions.

---

## Step 9 — Occlusion (sounds through walls)

Already enabled by default. When walls or geometry block the line
between listener and emitter, gool applies attenuation +
low-pass filtering to simulate occlusion. The wall doesn't have
to be a special node — gool uses Godot's `PhysicsServer3D` to
test occlusion against any `CollisionShape3D` in the scene.

Tunable in Project Settings → General → gool → Occlusion:

- `enabled` — master toggle (default true)
- `intensity` — 0.0 (no effect) to 1.0 (heavy filtering), default 0.7

If you find a specific surface SHOULDN'T occlude (a thin curtain,
a paper wall), make its `CollisionShape3D` non-occluding —
typically by setting it to a different physics layer that gool
doesn't query. Per-emitter `occlusion_enabled` override is also
available for special cases.

For an FPS this matters a lot: hearing footsteps through walls
gives tactical advantage; hearing them clearly through a closed
metal vault door breaks immersion. Tune `intensity` until it
feels right for your geometry.

---

## Step 10 — Voice chat for your crew

The 4-player coop story. Each peer captures their own mic input,
encodes it with Opus, sends it via the same multiplayer transport
your game uses, receives + decodes + plays back voice from the
other 3 peers.

Setup per peer:

```
Player (the local one — your player scene)
└─ VoiceCaptureSource   ← captures mic, encodes, replicates

OtherPlayer (replicated peer scenes — one per remote player)
└─ VoiceChatPlayer      ← receives, decodes, plays 3D-positional
```

`VoiceCaptureSource` (v0.81.8) handles PTT (push-to-talk) and VAD
(voice activity detection). PTT is the safer default for an FPS —
players use the same input loop they're already wired to (a key
press) instead of trusting VAD to not transmit silence/breathing.

`VoiceChatPlayer` plays the remote voice as 3D-positional audio
on the Voice/Player bus — your crew sounds like they're coming
from where they are in the world. Great for tactical communication;
players can localize their teammates by voice alone.

`VoiceCipher` (v0.81.10) encrypts the voice frames between peers.
Lightweight (XOR-based for now; intended for casual integrity, not
strong adversarial protection). For private heist crews this is
fine; for competitive ranked multiplayer you'd want a stronger
crypto layer above this.

**Bandwidth budget**: voice runs ~6-12 KB/sec per active speaker
per peer. For a 4-player game with one speaker at a time, that's
~36 KB/sec total egress from the speaker; trivial on broadband,
manageable on mobile. If you target peers with constrained budgets,
see cookbook recipe 5 ("Bandwidth budget for mobile players") for
how to clamp encode bitrate.

---

## Step 11 — Music state for heist phases

Your gangster game has phases — planning, infiltration, alarm,
combat, escape, victory/busted. `MusicStateController` lets you
author each as a `.tres` resource and crossfade between them as
the game progresses.

The scaffolded scene has a `MusicStateController` placeholder.
Author one `GoolMusicState` resource per phase, set their stream
+ crossfade durations, then in your game code:

```gdscript
@onready var music: MusicStateController = $MusicStateController

# When the heist starts:
music.set_state("planning")

# When the alarm trips:
music.set_state("alarm")  # crossfades from planning over its
                           # configured duration

# When you escape:
music.set_state("escape")
```

Music state is typically authoritative — the server decides the
phase, peers follow. RPC the state change from the host to all
peers; each peer's local `MusicStateController` handles the
crossfade in their own session. Sync isn't sample-accurate (each
peer's audio clock drifts independently), but it doesn't need to
be — phase changes are coarse enough that "everyone gets the new
music within ~500ms of the trigger" is plenty.

---

## Step 12 — Debug overlay during development

The scaffolded scene has `GoolDebugOverlay`. While you're building
your game, leave it in — it shows live diagnostics:

- Active voices / emitters count
- Per-bus peak levels
- Recent events fired
- Voice chat jitter / packet loss per peer
- Memory / streaming asset usage

Press F3 to toggle (default; configurable on the node). Delete the
node before shipping, or condition it on a debug build flag.

The mixer dock (bottom panel) shows the same info from the editor
side without needing the overlay node — but the overlay's value is
that it's visible **in the running game**, including in exported
builds where the editor isn't attached.

---

## Common pitfalls

**Tying audio to weapon animation timing.** It's tempting to play
the fire sound on the muzzle-flash animation frame. Don't — fire
the sound from the gameplay event (the moment the trigger logic
fires), not the animation. Animation can drift; the gameplay event
is the source of truth. Audio that drifts behind input is felt as
lag.

**Replicating every footstep as a NetworkedAudioEvent.** That's
~400 events per second for 4 players moving. Use local-only
footsteps unless your game design specifically requires teammates
to hear each other's steps with precise timing. If you do need it,
use the `UNRELIABLE` mode + per-peer rate limiting.

**Putting voice chat on the same bus as SFX.** Voice should be its
own bus chain with its own ducking + EQ pipeline, separate from
weapon SFX. Otherwise tactical voice gets buried in firefight
chaos. The FPS template config does this correctly; if you build
your bus topology by hand, mirror it.

**Forgetting to reparent GoolListener3D to the camera.** Default
scaffold places it at scene root; the listener will work but
audio panning won't follow head rotation. This breaks "I can hear
where the gunfire is coming from" feedback — the most important
audio cue in an FPS. Reparent it during Step 1, verify in F5 by
turning your character and confirming panning shifts.

**Mixing local-only sounds with replication assumptions.** If you
fire `Gool.play_one_shot(...)` from a game script that runs on
every peer (because the script is in a replicated scene), every
peer plays the sound *locally*. That's not replication — it's
duplication, and it desyncs. Only the firing player should call
`Gool.play_one_shot` directly; the rest should hear it via the
`NetworkedAudioEvent` path.

**Designing for 4 players, testing with 1.** Single-player testing
masks all the network-shaped issues. Get a second peer running
(another machine, or a second Godot editor instance with a debug
build) before declaring any networked audio code "works."

---

## Where gool ends and your game begins

gool gives you: spatial audio, replicated events, voice chat,
footsteps, reverb, occlusion, music states, the mixer dock, the
scaffolding verbs, the debug overlay.

You build: the player controller, the weapon scenes, the network
authority decisions, the spawning logic, the game state, the
match loop, the menus.

The integration surface is **events**. Your game emits them; gool
plays them. The two halves are independent enough that you can
build them in parallel — wire a weapon stub that emits the fire
event with no actual gameplay logic, hear the audio work end-to-end
across peers, then build the gameplay around the event surface
afterward.

That's the workflow. Build the gangsters.

---

## Reference: the document this guide was distilled from

This guide builds on a more general "Godot 4 FPS workflow"
reference document that covered the broader architecture
(CharacterBody3D, MultiplayerSpawner, raycast vs projectile, etc.).
This guide takes the audio-related points from that document and
maps each one to gool's prefabs + APIs explicitly. For the broader
non-audio FPS workflow concepts, that source document remains a
good reference.

## See also

- `cookbook.md` — individual recipes for one-off audio tasks
- `multiplayer.md` — gool's voice chat + replication internals
- `networking_integration.md` — bridging gool events with your
  transport choice (Godot HighLevel / ENet / Steam P2P)
- `first_enable_verification.md` — sanity check the addon loaded
  correctly before debugging anything else
