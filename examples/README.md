# gool examples

A learning path through gool. Examples are numbered by complexity —
start at the top, work down. Each is a self-contained Godot project
you can open and run.

## Where to start

**→ New to gool? Open [`01_quickstart/`](01_quickstart/) first.**

It's a 5-line GDScript scene that plays a 3D-positioned sound on
F5. The README in that folder walks through every line. If you can
run it successfully, you've validated your gool install and you
know the absolute minimum to make sound happen in your game.

## The Godot examples (read these first)

| # | Example | What it demonstrates | Roughly |
|---|---------|----------------------|---------|
| 01 | [`01_quickstart`](01_quickstart/) | Play one positioned sound. The "hello world." | 5 lines of code |
| 02 | [`02_audition`](02_audition/) | Comprehensive feature showcase: reverb zones, material EQ, footsteps, music, effect chains | Walk-through scene |
| 03 | [`03_voice_chat`](03_voice_chat/) | Real-time Opus voice chat between peers | One scene, one script |
| 04 | [`04_coop_shooter_template`](04_coop_shooter_template/) | Starter template for a co-op FPS — multiplayer audio replication baked in | Project template |
| 05 | [`05_multiplayer_audio_sandbox`](05_multiplayer_audio_sandbox/) | Full multiplayer setup: networked emitters, dialogue director, predictive playback | Sandbox project |

Each example has its own README explaining what it does, why,
and what to look at in the code.

## What each example demonstrates (in case you're looking for a specific feature)

Want to learn how to do **X**? Look at example **Y**.

| You want to... | Look at |
|----------------|---------|
| Play a one-shot sound at a position | `01_quickstart` |
| Set up a 3D listener that follows your camera | `02_audition` (look for `Listener3D` node in the scene) |
| Add reverb that changes when you walk into a room | `02_audition` (ReverbZone prefabs around each room) |
| Make footsteps that sound different on different surfaces | `02_audition` (FootstepSurfacePlayer + material tagging) |
| Configure an effect chain on a bus from script | `02_audition` (`apply_*_preset` calls) |
| Crossfade between music tracks | `02_audition` (MusicStateController) |
| Stream voice chat between players | `03_voice_chat` |
| Replicate sound events across networked players | `04_coop_shooter_template` and `05_multiplayer_audio_sandbox` |
| Use the dialogue director (priority-driven VO) | `05_multiplayer_audio_sandbox` |

If a feature doesn't have a dedicated minimal example yet (we have
plans for `03_reverb_zones`, `04_footsteps_by_surface`, etc. as
focused single-topic demos), look at the relevant section of
`02_audition` — it covers nearly every feature, just bundled into
one scene.

## The C++ examples (advanced, skip unless you're doing custom integration)

If you're building a Godot game, **you don't need these.** They
exist for two audiences:

- People building non-Godot games who want to embed the audio
  engine directly via the C++ API.
- gool contributors testing engine features without going through
  the Godot binding layer.

If that's not you, skip the `cpp/` folder.

| Example | What it shows |
|---------|---------------|
| [`cpp/minimal`](cpp/minimal/) | Smallest possible engine setup |
| [`cpp/hello_audio`](cpp/hello_audio/) | Load + play a single sound |
| [`cpp/playback`](cpp/playback/) | Multi-sound playback through the engine API |
| [`cpp/ducking`](cpp/ducking/) | Sidechain-style ducking |
| [`cpp/multi_tier_ducking`](cpp/multi_tier_ducking/) | Multi-tier compressor routing |
| [`cpp/music_crossfade`](cpp/music_crossfade/) | Crossfade between music states |
| [`cpp/sound_bank`](cpp/sound_bank/) | Bank loading and unloading |
| [`cpp/streaming`](cpp/streaming/) | Streamed-from-disk playback |
| [`cpp/telemetry`](cpp/telemetry/) | Read engine telemetry counters |

All C++ examples build via `cmake` from the repo root. See the
main [README](../README.md) for build instructions.

## After the examples

Once you've worked through 01-02, the next stop is the
[**cheatsheet**](../docs/CHEATSHEET.md) — one page with the 10
most common operations you'll do in everyday game code. Bookmark
that one; it's the page you'll come back to most often.

For deeper material:

- [`docs/cookbook.md`](../docs/cookbook.md) — full recipe set,
  longer than the cheatsheet
- [`docs/godot_quickstart.md`](../docs/godot_quickstart.md) — the
  install-and-first-sound walkthrough if you haven't done it yet
- [`docs/asset_pipeline.md`](../docs/asset_pipeline.md) — how
  sound files become things you can play
- [`docs/terminology.md`](../docs/terminology.md) — what gool means
  by "emitter," "voice," "bus," etc.
