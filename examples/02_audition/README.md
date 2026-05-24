# gool audition — runnable feature showcase

A single-player Godot scene that demonstrates the gool feature set
in a single walkthrough. Open this project in Godot 4.x, press F5.

## What's in here

A hub-and-spokes level: a central outdoor hub with four enclosed
rooms branching off through doorways. Each room has its own reverb
preset, its own EQ shaping (v0.47.0), and two material impact
targets you can shoot.

```
                    ┌───────────────────┐
                    │     Cathedral     │   long stone tail, HPF 300 / LPF 6k
                    │   14×9×14 m       │   Concrete + Wood targets
                    └──────┬────────────┘
                           │ doorway south
                           │
   ┌────────┐       ┌──────┴──────────┐       ┌─────────┐
   │Bathroom│←─door─┤      Hub        ├─door─→│  Cave   │
   │  Tile  │       │  Outdoor reverb │       │  Dark   │
   │ 5×3×5  │       │  Material gallery│       │ 10×5×12 │
   └────────┘       │  Player spawn   │       └─────────┘
   Glass + Metal    └──────┬──────────┘       Concrete + Foliage
                           │ doorway north
                           │
                    ┌──────┴──────────┐
                    │   Small Room    │   intimate, LPF 10k
                    │   6×3×6 m       │   Wood + Drywall targets
                    └─────────────────┘
```

## Controls

| Key | What |
|---|---|
| WASD | Move |
| Mouse | Look |
| Space | Jump |
| **LMB** | **Shoot — fires a raycast impact at the hit point with material-aware sound** |
| **B** | **DialogueDirector bark (synthesized) — ducks Music + Sfx via sidechain** |
| **K / L** | **Capture / apply GoolMixSnapshot** |
| **F3** | **Toggle debug overlay** |
| Esc | Release mouse (LMB recaptures) |

## What to listen for

1. **Reverb transitions across doorways.** Walk through any doorway
   and the reverb ramps over `transition_ms` (default 800ms) from
   the hub's Outdoor Open to the room's preset. Smooth crossfade,
   not snap.

2. **Per-room EQ shaping (v0.47.0).** Each room's reverb has its
   own HPF + LPF baked in via the presets — Cathedral's tail rolls
   off at 6kHz, Cave at 4.5kHz, Bathroom keeps brightness up at
   5kHz, Small Room is wide-open at 10kHz.

3. **Material × reverb composition.** Shoot a Concrete panel in
   the Cathedral, then shoot the Wood panel right next to it. Same
   reverb, different impact character. Walk to the Cave and shoot
   the Concrete panel there — same material, totally different
   tail.

4. **Wall occlusion.** Stand inside the hub, aim through the
   Cathedral doorway at the Wood panel inside, fire. Listen for the
   muffled-through-wall character. Walk inside, fire the same shot
   — clear, full-band, immediate.

5. **Dialogue sidechain ducking.** Press B anywhere. The
   synthesized bark plays on the Dialogue bus, and the sidechain
   compressors wired in `gool/config.json` duck Music + LocalSfx +
   RemoteSfx by ~6-10 dB so the bark cuts through.

6. **Mix snapshot.** K captures the current per-bus mix state into
   memory. Walk to a different room (different reverb activates),
   then press L to restore the original mix.

## How it maps to gool features

| What you experience | Where it lives in gool |
|---|---|
| Player ears tracking the camera | `GoolListener3D` (not used in this minimal example — the FpsPlayer uses Gool's autoload directly) |
| Spatial gunshots | `Gool.play_3d(name, position, priority)` |
| Material-aware impacts | `Gool.play_impact_sound(name, position, material_id)` |
| Per-room reverb | `ReverbZone` prefab (Area3D) with `material` or per-param @exports |
| EQ shaping per room | `ReverbZone.send_hpf_hz` + `return_lpf_hz` (v0.47.0) |
| Wall occlusion | StaticBody3D walls + gool's per-emitter occlusion raycasts |
| Dialogue ducking | `DialogueDirector.bark()` + sidechain compressors in `config.json` |
| Mix snapshots | `GoolMixSnapshot.from_current()` / `apply()` |
| Debug overlay | F3 toggles `GoolDebugOverlay` prefab |

## How to learn more

- **`docs/quickstart_fps.md`** — the canonical "I want to build an
  MP FPS, here's the recipe" doc. Start here.
- **`docs/audio_design/reverb_eq.md`** — the v0.47.0 EQ shaping
  recipes drawn from Sound on Sound + MixingLessons articles.
- **`docs/audio_design/dialogue_setup.md`** — sidechain ducking
  setup, what the compressors are doing.
- **`docs/networking_bridge.md`** — for the multiplayer side
  (this example is single-player; the bridge isn't exercised here).
- **`docs/terminology.md`** — gool's vocabulary vs FMOD/Wwise.

## Notes

- This example does NOT exercise multiplayer (no GoolMultiplayerBridge
  usage). For that, see `examples/04_coop_shooter_template/`.
- The audition's `register_sound_definition` calls demonstrate the
  current 9-positional-arg signature. A Dictionary form is on the
  roadmap.
- `gool/config.json` is a copy of `addons/gool/templates/config_fps.json`
  — the FPS-ready bus topology with v0.47.0 reverb chain pre-wired.
