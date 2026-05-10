# Co-op shooter starter template

A single-host audio architecture demo for a 4-player PvE shooter,
built on the gool engine's primitives. One playable character + three
AI bots that wander and fire weapons periodically. Demonstrates the
audio-replication patterns from `docs/multiplayer.md` without
requiring an actual multiplayer transport.

The point: **press Play, hear it work.** This is the proof that the
primitives compose into something a Godot dev can actually build a
co-op shooter on top of.

## Prerequisites

- **Godot 4.2 or newer**
- The `gool_godot` GDExtension built and copied to
  `examples/coop_shooter_template/addons/gool/bin/`. Build instructions
  at `godot/README.md` in the engine repo.

If the extension binary isn't present, the project still opens but
you'll see "GoolAudioRuntime class not registered" errors in the
console — that's the GDExtension failing to load because the shared
library isn't where Godot expects it.

## Controls

| Input         | Action                              |
|---------------|-------------------------------------|
| `WASD`        | Move local player                   |
| Left mouse    | Fire current weapon                 |
| `Q`           | Cycle weapon (pistol/rifle/shotgun) |

## What you should hear

| Element                     | Source                                                                     |
|-----------------------------|----------------------------------------------------------------------------|
| Three weapon types          | distinct `pistol_fire`, `rifle_fire`, `shotgun_fire` synthesized timbres   |
| Local vs remote audio       | bot weapons have a delayed "tail" emulating distance travel                |
| Footsteps for all 4 chars   | `FootstepSurfacePlayer` prefab on each, fires every 0.85 m of horizontal travel |
| Combat music state machine  | explore → suspicion → combat, driven by gunfire activity in last N seconds |
| RTPC-driven music attenuation | `combat_intensity` parameter modulates music volume during heavy fire (layer over the sidechain ducking) |
| Continuous ambient bed      | low-passed wind loop, played as a long-lived `AudioEmitter3D`              |
| UI feedback                 | weapon-cycle blip                                                          |

The AI bots fire in bursts (3–6 shots) on a randomized 2.5–6 second
cycle. Stand near a couple of them and the music director will push
into "combat" within a few seconds; back off and let things calm down
and you'll hear it relax through "suspicion" back to "explore."

## Architecture

```
                                   ┌──────────────────────────┐
                                   │    /root/Gool (autoload) │
                                   │   GoolAudioRuntime + Gool│
                                   │   facade (play_3d, RTPC, │
                                   │   music, etc.)           │
                                   └─────────▲────────────────┘
                                             │
              ┌──────────────────┬───────────┼─────────────┬─────────────────┐
              │                  │           │             │                 │
        ┌─────┴─────┐   ┌────────┴───────┐  ┌┴──────────┐ ┌┴──────────────┐ ┌┴─────────────┐
        │ Player    │   │ AiBot ×3       │  │ Music     │ │ Combat        │ │ Ambience     │
        │ controller│   │ (wander + fire)│  │ State     │ │ Music         │ │ AudioEmitter │
        │ (WASD,fire)│  │                │  │ Controller│ │ Director      │ │ 3D (loop)    │
        └────┬──────┘   └────────┬───────┘  └───────────┘ └──────▲────────┘ └──────────────┘
             │                   │                                │
             └─Weapon───┬────────┘─Weapon                          │
                       │                                           │
                  fired signal ───────────────────────────────────►│
                                                                   │
                                                       (drives RTPC + state)
```

`Player`, `AiBot ×3`, and a `Camera` make up the gameplay layer.
Each character owns a `Weapon` component (in the `weapons` group)
and a `FootstepSurfacePlayer` prefab. `MusicStateController` is the
music backbone; `CombatMusicDirector` is the policy that decides what
state should be playing based on gunfire activity. `Ambience` is a
single long-lived `AudioEmitter3D` looping the wind bed at world origin.

### Multi-tier ducking — now wired through the bus config

As of v0.10, this demo ships a real multi-tier sidechain ducking
bus graph at `gool/config.json`:

```
Master
├── Music     (Compressor, sidechain = LocalSfx)   ← music ducks under local action
├── Voice
├── Ambient
└── SfxAll
    ├── LocalSfx                                    ← your gun, your footsteps
    └── RemoteSfx (Compressor, sidechain = LocalSfx) ← teammate gun, NPC barks
                                                       duck under your local action
```

The L4D2 pattern verbatim — local gunshots win over both music
AND remote teammate gunshots. Frame-accurate, pre-mix, the same
DSP path the C++ `examples/multi_tier_ducking/main.cpp` reference
implementation uses.

The bus graph is loaded from `gool/config.json` by the runtime
singleton at startup (see `addons/gool/runtime_singleton.gd::_ready`).
The C++ side parses the JSON, builds an `AudioConfig.busGraph`, and
calls `AudioRuntime::Initialize()` with it. No GDScript-side
schema translation; the binding takes the raw JSON text.

The `combat_music_director.gd` script's RTPC-driven music attenuation
ALSO ships, layered on top. The two mechanisms compose without
conflict:

- **Sidechain compression** (the new bus config): the music *bus*
  is processed by an envelope follower keyed off the LocalSfx bus.
  Triggers on every local gun fire; frame-accurate.
- **RTPC ducking** (existing): the `combat_intensity` parameter
  drives a smoothed gameplay-loudness signal that the music director
  tracks and uses for state transitions (explore → suspicion →
  combat) AND for an additional volume offset.

For a project that wants only the bus-config approach and no RTPC
stand-in, delete the `combat_music_director.gd` reference from
`main.tscn`. The bus graph alone produces clean L4D2-style ducking.
For richer adaptive-music shaping (state machine + RTPC modulation),
keep both — the audible result is the union of their behaviors.

### Per-emitter bus targeting — gap closed in v0.10.1

The v0.10 release added bus-graph configuration from JSON but left
a hole: every sound registered via `register_sound_definition`
routed to Master regardless of the bus config. v0.10.1 closes
that gap.

The GDScript binding now takes two new optional parameters:

```gdscript
Gool.register_sound_definition(
    name, spatialized, looping,
    min_distance, max_distance, loop_crossfade_ms,
    Gool.CATEGORY_SFX,        # category routing fallback
    "LocalSfx"                # explicit target bus override
)
```

`target_bus_name` resolves through the new `Gool.find_bus_id_by_name()`
helper (which the binding calls internally). Empty string falls
through to category routing; unknown bus names produce a warning
and fall back to category routing.

This template's `audio_setup.gd` registers each weapon sound in
two variants: `*_local` routed to LocalSfx, `*_remote` routed to
RemoteSfx. The same audio asset goes to both — only the bus
routing differs. `weapon.gd::_play_fire` appends the right suffix
based on `is_local`. So when the player fires while a bot is also
firing, the local gunshot triggers the sidechain compressors on
both the Music bus AND the RemoteSfx bus, audibly ducking both
under the local action. L4D2-style multi-tier ducking, working at
the engine level for the first time.

The RTPC-driven music attenuation in `combat_music_director.gd`
still ships as an additional layer (drives the music *state*
machine, not just the volume), so adaptive music + sidechain
ducking compose without conflict.

### Networking — what's wired and what's not

This is a **single-host** demo. The four characters all live in the
same scene; AI bots stand in for what would be three remote peers
in a real co-op session. The audio architecture treats them
appropriately — bot weapons go through the "remote" path with a
delayed distance tail; only the player's gun gets the loud near-field
treatment.

To go from this demo to real 4-player networking:

1. Replace the direct `Gool.play_3d(...)` call in `weapon.gd::_play_fire`
   with a `NetworkedAudioEvent.play(...)` call.
2. Configure each `NetworkedAudioEvent` with `mode =
   SERVER_AUTHORITATIVE` (or `CLIENT_PREDICTED` if you want low-latency
   client prediction with server reconciliation — see
   `addons/gool/prefabs/networked_audio_event.gd`).
3. Set up an `AudioRelevancyFilter` for distance/team-aware peer
   filtering (`addons/gool/audio_relevancy_filter.gd`).
4. Use Godot's `MultiplayerSpawner` / `MultiplayerSynchronizer` to
   replicate character transforms.
5. Drop the AI bots; they're just standing in for remote peers.

The audio layer doesn't change. The `Weapon` component, the
`CombatMusicDirector`, the footstep generation, the ambience bed,
the music state machine — all unchanged. That's the whole point of
the architecture: replicate gameplay events, play audio locally.

See `docs/multiplayer.md` in the engine repo for the full
replication-vs-presentation split, voice chat integration, and
per-event staleness rules.

## Sounds

All sounds in this demo are **synthesized procedurally on `_ready()`**
so the project has zero asset dependencies — clone, build, press Play.
The synthesis lives in `scripts/audio_setup.gd` and is intentionally
minimal. To replace with real assets:

1. Stop synthesizing the placeholder PCM in
   `scripts/audio_setup.gd::_register_*` functions.
2. Use `Gool.register_sound_from_file(name, path)` (or load via a
   `gpak` archive) instead.
3. Keep the gameplay-facing names (`pistol_fire`, `step_stone_a`, etc.)
   so no other code in the demo has to change.

The naming convention is documented in `docs/multiplayer.md` §16:
gameplay code never sees asset paths, only event names.

### Why synthesized rather than CC0 freesound packs?

Two reasons:
1. **Reproducibility.** Anyone clones the repo, opens the project,
   and gets the same demo experience. No "go download these 200 MB
   of sounds and put them in this folder" step.
2. **Demonstrates the audio data path.** `register_pcm_sound(name,
   PackedFloat32Array, sample_rate, channels)` works for any source
   of PCM — synthesized, decoded from your own format, captured
   from a microphone, generated by an LLM, whatever. Showing it
   working with synthesized data makes the data-flow point clearly.

## Scripts

| File                                 | Purpose                                                     |
|--------------------------------------|-------------------------------------------------------------|
| `scripts/main.gd`                    | Scene controller: bootstrap, listener tracking, wiring     |
| `scripts/audio_setup.gd`             | Synthesizes + registers all sounds, binds RTPC             |
| `scripts/player_controller.gd`       | Local player WASD movement, fire input, weapon cycle       |
| `scripts/ai_bot.gd`                  | Wander/pause/burst-fire state machine                      |
| `scripts/weapon.gd`                  | Weapon component, cooldown, fire signal                    |
| `scripts/combat_music_director.gd`   | Gunfire tracker, music state policy, intensity RTPC        |

## Files outside `scripts/`

- `project.godot` — Godot project config + input map + autoload
- `main.tscn` — scene with all nodes, cross-references the scripts
- `icon.svg` — placeholder project icon (replace with your own)
- `addons/gool/` — copy of the engine addon (prefabs, runtime singleton)

## What this demo doesn't include (and why)

- **Multiplayer transport.** Single-host with bots; see "Networking"
  above for how to extend.
- **Voice chat.** The voice prefab exists at
  `addons/gool/prefabs/voice_chat_player.gd` but is not exercised
  in this scene because there's no second machine to send packets
  from. The quickstart example demonstrates the binding-level hookup.
- **Damage / health / mission systems.** This is an audio demo. The
  weapons make sound; nothing dies. Composing the audio architecture
  with real gameplay is the next layer.
- **Polished visuals.** Capsules on a flat floor. Audio is the point.
- **Real sidechain bus ducking.** RTPC-based instead; see "Multi-tier
  ducking" above.
