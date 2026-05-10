# Changelog

All notable changes to gool are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
While the major version is `0`, minor bumps may include backward-incompatible
changes; consult the per-release `Changed` and `Removed` sections before
upgrading.

## [Unreleased]

Nothing yet — open the next release section here when a feature lands.

## [0.11.2] - 2026-05-10

Bootstrap-experience overhaul, phase A: documentation. Closes the
documented gaps that made the project hard to set up for new
adopters. No engine code changes; this is a docs-and-truth release.

### Added

- **`SETUP.md`** — single-source-of-truth setup guide (530 lines).
  Per-platform prerequisites with concrete package manager commands
  (winget + Chocolatey for Windows; Homebrew for macOS; apt / dnf /
  pacman for Linux). Covers Track A (use prebuilt addon — placeholder
  until binary releases ship in phase C) and Track B (build from
  source). Three-phase build-from-source procedure: install
  prerequisites, build the GDExtension, install the addon into a
  Godot project. Optional features section covering libopus and
  libopusfile install per platform. Verification steps.
  Troubleshooting section covering the 7 most common failure modes
  (`GODOT_CPP_PATH` missing, `miniaudio.h` not found, decoder headers
  not found, compiler too old, MSBuild not found on Windows, addon
  doesn't appear in editor, macOS).

### Fixed

- **README "Build options" table** — corrected the lie that
  `AUDIO_ENGINE_DECODERS_WAV/OGG/FLAC` default ON. They've always
  defaulted OFF in `CMakeLists.txt`. Adopters who followed the
  README ended up with no decoders compiled in. Also added the
  `AUDIO_ENGINE_DECODERS_OPUS` row missing since v0.11.0.

- **README test count** — was 25 in the build instructions block,
  36 in the test suite section. Both now say 36.

- **README "Dependencies" section** — re-labeled "Vendored
  single-header drops" to "Fetched single-header drops" because
  miniaudio / dr_libs / stb_vorbis aren't actually vendored in the
  repo; they're fetched on demand via `scripts/fetch_*.sh`. The
  scripts existed; the README didn't tell adopters to run them.
  Added libopusfile to the optional-dependency table.

- **README "Quick start" section** — was a wall of GDScript API
  examples followed by a buried install block. Restructured so the
  setup pointer (to `SETUP.md`) leads, the 30-second build
  incantation is visible, and the API examples follow as "first
  lines you'll write" once the project is set up.

- **godot/README.md** — same install-section rewrite. Points at
  `SETUP.md` for the per-platform path, adds the missing fetch
  script step that has to run before miniaudio compiles.

### Acknowledged honestly

- **macOS is currently broken** — the build doesn't work on Apple
  Clang. CI matrix has macOS disabled in both `ci.yml` and
  `release.yml`. README and `SETUP.md` now say so up front instead
  of letting macOS users hit cryptic errors.

- **No prebuilt addon binaries ship yet** — Track A in `SETUP.md`
  is a placeholder pointing at the build-from-source path for now.
  Phase C of this work (release pipeline rewrite) will fix this.

### Tests

- Total **36/36** passing (no test changes; this is a docs release).
  Ducking baseline locked at -17.20 dB.

### What's next

This was Phase A of a planned three-phase bootstrap overhaul:

- **Phase A (this release):** fix the documentation lies, write
  the missing setup guide.
- **Phase B (planned next):** bootstrap automation. `scripts/bootstrap.sh`
  + `scripts/bootstrap.ps1` that verify prerequisites, clone and
  build godot-cpp at a pinned ref, build the GDExtension, and
  install into a target Godot project. One-command setup.
- **Phase C (planned):** release pipeline rewrite. Build the
  GDExtension binary per platform on tag push, package as
  `gool-X.Y.Z-godot-addon.zip` ready to drop into a Godot project.
  Closes the Track A gap.

## [0.11.1] - 2026-05-10

GDScript bindings for runtime audio-file loading. Closes the gap
between v0.11.0's engine surface (OpusFileDecoder + the existing
WAV / Vorbis / FLAC / Opus pipeline routed through
`AudioRuntime::RegisterSoundFromFile` / `RegisterSoundFromMemory`)
and what GDScript hosts could actually call. Before v0.11.1, the
GDExtension only exposed `register_pcm_sound(name, samples, sr,
ch)` — Godot projects had to pre-decode files in GDScript or via
synthesis to get audio into the engine. v0.11.1 lets them register
directly from a file path (including res://) or a raw byte buffer.

### Added

- **GDExtension binding `register_sound_from_file(name, path) → int`**
  — reads bytes via Godot's `FileAccess` (works with `res://` paths
  in PCK-packaged builds, not just editor mode) and calls into
  `AudioRuntime::RegisterSoundFromMemory` with format = Auto.
  Returns the AudioSoundId on success, 0 on failure with
  push_error describing the cause (file missing, decoder compiled
  out, etc).

- **GDExtension binding `register_sound_from_bytes(name, bytes,
  format_hint) → int`** — direct memory variant for hosts that
  manage I/O themselves (custom asset packs, network downloads,
  encrypted blobs). `format_hint` matches the C++ AudioFileFormat
  enum: 0=Auto (recommended; sniffs by magic bytes), 1=Wav,
  2=OggVorbis, 3=Flac, 4=Opus.

- **`runtime_singleton.gd::register_sound_from_file()` and
  `register_sound_from_bytes()`** — facade wrappers with
  documentation about which decoders need to be enabled in CMake
  (`AUDIO_ENGINE_DECODERS_*`) for each format.

- **`FORMAT_AUTO` / `FORMAT_WAV` / `FORMAT_OGG_VORBIS` /
  `FORMAT_FLAC` / `FORMAT_OPUS`** constants on the runtime
  singleton, mirroring the C++ AudioFileFormat enum.

- **Diagnostic mapping** — when the underlying engine returns
  `AudioResult::Unsupported` (decoder gated off in CMake), the
  binding pushes a clear error directing the user to set
  `AUDIO_ENGINE_DECODERS_*=ON` for the format they need.

### Usage

```gdscript
# Load any supported file from res:// — magic-byte sniff picks
# the right decoder. Returns the sound id (positive int) or 0
# on failure.
var id := Gool.register_sound_from_file("rifle_fire",
                                          "res://audio/rifle_fire.opus")
if id != 0:
    Gool.register_sound_definition("rifle_fire", true, false,
                                     1.0, 30.0, 80.0,
                                     Gool.CATEGORY_SFX, "LocalSfx")
    Gool.play_3d("rifle_fire", muzzle_position, 200)
```

For memory-managed sources:

```gdscript
var bytes: PackedByteArray = my_pack.read_asset("explosion.opus")
Gool.register_sound_from_bytes("explosion", bytes, Gool.FORMAT_AUTO)
```

### Limitations

- **No streaming-from-bytes binding yet.** `RegisterStreamingSoundFromFile`
  takes a real-fs path (won't work for `res://` in PCK builds), and
  `RegisterStreamingSoundFromMemory` only takes pre-decoded float
  PCM (not compressed bytes). Streaming Opus directly from a
  packed Godot resource needs an engine-side
  `RegisterStreamingFromMemory(bytes, formatHint)` — deferred to a
  follow-up release.
- **Decoder defaults remain OFF.** All `AUDIO_ENGINE_DECODERS_*`
  CMake options default OFF. Projects that want file playback
  must enable the relevant flag(s) at build time. The binding's
  diagnostic error makes the misconfiguration obvious at runtime.

### Tests

- Total **36/36** passing. Existing `decoder_test` continues to
  cover format sniffing on RIFF / OggS+Vorbis / OggS+OpusHead /
  fLaC magic bytes; the new bindings exercise already-tested
  engine paths so no new test files were added this iteration.
  Ducking baseline locked at -17.20 dB.

## [0.11.0] - 2026-05-10

Opus file decoding. Adds `OpusFileDecoder` to the existing decoder
plugin framework alongside WAV / Vorbis / FLAC. Compressed audio
assets in the `.opus` container (Ogg Opus) can now be loaded and
streamed at runtime, gated behind the `AUDIO_ENGINE_DECODERS_OPUS`
build flag the same way the other format decoders gate.

### Why Opus

Opus is the modern royalty-free codec. At ~96 kbit/s it sounds
indistinguishable from raw PCM for game SFX and music; bundle
sizes drop by roughly 15× compared to 48 kHz/16-bit stereo WAV.
The codec is already in this engine for streaming voice (libopus
via `OpusVoiceCodec`); v0.11.0 adds file-based playback via the
sister library libopusfile.

### Added

- **`src/audio_engine/decoders/opus_file_decoder.{h,cpp}`** —
  new `OpusFileDecoder` implementing `IAudioDecoder`. Wraps
  libopusfile (`OggOpusFile*`). `CreateFromFile` and
  `CreateFromMemory` factory helpers; `DecodeFrames` produces
  interleaved float32 in [-1, 1]; `Seek` is sample-accurate
  against the 48 kHz decoded stream.

  Opus always decodes at 48 kHz internally regardless of source
  recording rate, so `SampleRate()` always reports 48000. The
  asset registry's existing resampling path handles engine-rate
  mismatches automatically — no new wiring needed.

- **`AudioFileFormat::Opus`** — new enum value in
  `audio_file_format.h`.

- **`tests/unit/decoder_test::TestOpusFactoryDispatch`** — proves
  the factory routes `.opus` files (and `OpusHead`-marked Ogg
  streams in memory) to `OpusFileDecoder`, not to
  `OggVorbisDecoder` (which would have silently failed). Runs
  unconditionally; in stub mode (the default) it verifies the
  factory returns nullptr cleanly rather than misrouting.

- **`tests/unit/decoder_test::TestFormatSniffing`** — extended
  with explicit Vorbis and Opus codec-ID test cases. Hand-built
  payload representations of `OggS` + `OpusHead` and `OggS` +
  `\x01vorbis` confirm the sniffer disambiguates.

### Changed

- **`DecoderFactory::SniffFormat`** — when the magic bytes are
  `OggS`, the sniffer now probes the page payload for either
  `OpusHead` (8-byte magic) or `\x01vorbis` (7-byte magic) and
  routes accordingly. Previously every `OggS` returned
  `AudioFileFormat::OggVorbis`, which silently misrouted Opus
  streams. Files without enough bytes to disambiguate fall back
  to `OggVorbis` as before (the create-then-fallback path in
  `CreateForFile` handles the rest).

- **`DecoderFactory::FormatFromExtension`** — `.opus` extension
  added; case-insensitive like the others.

- **`DecoderFactory::CreateForFile`** — Opus added to the
  fallback chain. Tried before Vorbis since libopusfile rejects
  non-Opus streams faster (header check) than stb_vorbis rejects
  non-Vorbis streams.

- **`CMakeLists.txt`** — new option
  `AUDIO_ENGINE_DECODERS_OPUS` (default OFF, matching the other
  decoder flags). When ON, resolves libopusfile via
  `find_package(OpusFile)` first, then `pkg-config opusfile`.
  No vendored or FetchContent path because libopusfile uses
  autotools, not CMake — adopters install via system package
  manager (`apt install libopusfile-dev`, `brew install opusfile`,
  `vcpkg install opusfile`) and re-run CMake. Configure-time
  error message is explicit about this.

### Build flag

```
cmake -DAUDIO_ENGINE_DECODERS_OPUS=ON ..
```

The decoder TU is always in the source list; the gate decides
whether it pulls in `<opus/opusfile.h>` and links libopusfile, or
expands to a stub returning nullptr. Same pattern the other
decoders and `OpusVoiceCodec` already use, so adopters get a
predictable surface.

### Tests

- Total **36/36** passing (decoder_test extended with 1 sub-test
  + extended sniffing). Ducking baseline locked at -17.20 dB.

## [0.10.1] - 2026-05-10

Per-emitter bus targeting from GDScript. Closes the v0.10.0
documented gap: the bus graph was *configurable* but every sound
registered through GDScript silently routed to Master, so the
multi-tier sidechain ducking config in `coop_shooter_template`
was a no-op. v0.10.1 makes the ducking actually trigger.

### Added

- **`AudioRuntime::FindBusIdByName(std::string_view) → BusId`** —
  new public method. Resolves a bus by its `debugName` (set via
  `BusConfig::debugName` at build time, including by the JSON
  loader's `name` field). Returns `kInvalidBusId` if no bus matches.
  O(N) over kMaxBuses; intended for init-time and registration-time
  use, not per-frame. Game-thread only.

- **GDExtension binding `find_bus_id_by_name(name) → int`** —
  exposes the lookup to GDScript. Returns -1 if no bus matches.
  Useful for hosts that need to call other BusId-taking bindings
  (`set_bus_gain_db`, `set_effect_parameter`) by name.

- **`bus_config_loader_test::TestFindBusIdByName`** — new
  sub-test (11th in the file) covering: each declared bus name
  resolves to a valid distinct BusId, Master is always pinned to
  `kBusMaster` (id 0), unknown names and empty strings return
  `kInvalidBusId`.

### Changed

- **GDExtension `register_sound_definition`** — extended with two
  new optional parameters at the end:

  ```gdscript
  Gool.register_sound_definition(
      name, spatialized, looping,
      min_distance, max_distance, loop_crossfade_ms,
      category,           # NEW: Gool.CATEGORY_* (default SFX = 0)
      target_bus_name)    # NEW: bus override (default "" = use category routing)
  ```

  Existing call sites (audio_emitter_3d, networked_audio_emitter_3d,
  voice clip registration) keep their behavior because the new
  params have safe defaults.

- **GDExtension `register_sound_definition` — fixed default-target
  bug.** The binding previously hardcoded
  `def.targetBus = audio::kBusMaster`, which silently overrode the
  category routing configured via JSON. The new behavior leaves
  `targetBus` at `kInvalidBusId` (the engine's "use category
  routing" sentinel) when no explicit bus is named, so the bus
  graph configured in v0.10.0 actually receives sounds. For projects
  with no bus config, behavior is preserved (every category default-
  routes to master via `CategoryBusMap`).

- **`runtime_singleton.gd::register_sound_definition`** — wrapper
  signature extended to forward the new parameters. Adds
  `CATEGORY_SFX` / `CATEGORY_VOICE` / `CATEGORY_MUSIC` /
  `CATEGORY_AMBIENCE` / `CATEGORY_UI` / `CATEGORY_DIALOGUE`
  constants mirroring the C++ enum.

- **`runtime_singleton.gd::find_bus_id_by_name(name) → int`** — new
  facade method.

- **`coop_shooter_template`** — the wiring is now real:

  - `audio_setup.gd` registers each weapon sound twice
    (`*_local` → LocalSfx, `*_remote` → RemoteSfx). Same audio
    asset, different bus routing.
  - Music registrations target the Music bus (with sidechain
    compressor keyed off LocalSfx).
  - Ambient registrations target the Ambient bus.
  - Footsteps go to LocalSfx for both player and bots in this
    single-host demo.
  - UI sounds use category UI (default-routed to Master).
  - `weapon.gd::_play_fire` appends `_local` or `_remote` based on
    `is_local`, so the local player's gun audibly ducks both
    music AND remote teammates' gunfire. Multi-tier sidechain
    ducking from L4D2 patterns, working at the engine level.

  The RTPC-driven music attenuation in `combat_music_director.gd`
  still ships as a layered behavior — it drives the music *state*
  machine (explore → suspicion → combat) so adaptive music and
  sidechain ducking compose without conflict.

### Tests

- Total **36/36** passing (existing `bus_config_loader_test`
  extended from 10 → 11 sub-tests). Ducking baseline locked at
  -17.20 dB.

## [0.10.0] - 2026-05-09

Bus-graph configuration from GDScript. Closes the documented gap
between what the C++ engine can do (multi-tier sidechain ducking,
per-bus effect chains) and what GDScript hosts could configure
(sample rate + buffer size only). Godot projects can now ship their
bus topology in a JSON file and get the full L4D2-style ducking
behavior at runtime initialization with no engine code changes.

### Added

- **`include/audio_engine/bus_config_loader.h`** — new public header.
  `audio::BusConfigLoader::ParseFromJson(json)` returns a populated
  `BusGraphConfig` (with category routing nested) ready to drop
  into `AudioConfig::busGraph`. Errors carry line numbers and
  descriptive messages. Forward-compat: unknown keys are tolerated
  silently. Back-compat: configs missing the `"buses"` key parse
  successfully with `busCount=0` (engine auto-builds master-only).

- **`src/audio_engine/runtime/bus_config_loader.cpp`** — minimal
  recursive-descent JSON parser (~480 LOC, no shared scanner
  dependency in this iteration). Supports all five effect kinds
  (gain, biquad, compressor, reverb, saturation) with their full
  field surface. Sidechain bus references resolve by name. Tolerates
  `//` line comments for hand-edited configs.

- **`tests/unit/bus_config_loader_test.cpp`** — 10 sub-tests,
  pure C++, covering: minimal config, full multi-tier ducking shape
  with sidechain refs resolved by name, every effect kind round-
  tripping its fields, end-to-end `AudioRuntime::Initialize()`
  accepting the parsed config, malformed JSON line numbers, unknown
  effect kind error, unresolved sidechain bus error, unresolved
  parent error, forward-compat unknown-key tolerance, and back-
  compat empty-buses-key handling.

- **GDExtension binding `init_with_config(json_text, sample_rate,
  buffer_size)`** — the C++ binding takes the raw JSON text from
  GDScript and routes through the loader. No GDScript-side schema
  translation.

### Changed

- **`addons/gool/runtime_singleton.gd`** — reads the project's
  `res://gool/config.json` at startup. If the JSON contains a
  `"buses"` array, routes through `init_with_config()`. If the
  file is missing, empty, or has no buses key, falls back to plain
  `init(sample_rate, buffer_size)` (legacy behavior).

- **`addons/gool/plugin.gd`** — the editor plugin now writes a
  richer default `gool/config.json` on enable. The default ships
  the L4D2 multi-tier ducking topology: Master / Music (ducks
  under LocalSfx) / SfxAll / LocalSfx / RemoteSfx (ducks under
  LocalSfx) / Voice / Ambient. Out-of-the-box, projects get the
  audio mix architecture proven by the C++ `multi_tier_ducking`
  example.

- **`examples/coop_shooter_template/`** — synced to use the new
  binding. Ships its own `gool/config.json` with the multi-tier
  ducking topology. README updated to reflect the new wiring AND
  to honestly document the remaining gap (per-emitter bus
  targeting from GDScript — a separate iteration).

### JSON schema

```json
{
  "sample_rate": 48000,
  "buffer_size": 512,
  "buses": [
    { "name": "Master", "gain_db": 0.0 },
    { "name": "Music", "parent": "Master", "gain_db": -3.0,
      "effects": [
        { "kind": "compressor",
          "threshold_db": -28.0, "ratio": 8.0,
          "attack_ms": 5.0, "release_ms": 250.0,
          "sidechain_bus": "LocalSfx" }
      ] },
    { "name": "LocalSfx", "parent": "SfxAll" },
    { "name": "RemoteSfx", "parent": "SfxAll",
      "effects": [
        { "kind": "compressor",
          "sidechain_bus": "LocalSfx",
          "threshold_db": -28.0, "ratio": 6.0,
          "attack_ms": 5.0, "release_ms": 200.0 }
      ] }
  ],
  "category_routing": {
    "music": "Music", "sfx": "LocalSfx", "voice": "Voice"
  }
}
```

Sidechain bus references resolve by name, so config files stay
readable (no manual BusId numbering). Master must be one of the
buses. Other parents are resolved against the bus list.

### Honest gap remaining

The GDScript `register_sound_definition` binding doesn't yet take
a target-bus argument — every registered sound routes to Master by
default. The new bus graph is therefore *configured* but not
*exercised* by the coop_shooter_template's audio (the RTPC stand-in
in `combat_music_director.gd` continues to drive the audible
ducking). Closing this gap is the next iteration's deliverable: one
binding method to add (`register_sound_definition_on_bus(name,
bus_name, ...)` or a `play_3d_on_bus(...)` per-play override) plus
test coverage.

### Tests

- Total **36/36** passing (10 added in `bus_config_loader_test`).
  Ducking baseline locked at -17.20 dB.

## [0.9.1] - 2026-05-09

Co-op shooter starter template — the demo that shows the audio
architecture compose into something real. Single-host scene with
one playable character + three AI bots, demonstrating the
multiplayer audio patterns from `docs/multiplayer.md` without
requiring an actual networking transport. Press Play, hear it work.

This is a Godot-side-only release; no engine code changed. All
35 tests still pass, ducking baseline locked at -17.20 dB.

### Added

- **`examples/coop_shooter_template/`** — new Godot 4.2+ project.
  Six GDScript files (~700 LOC), one main scene, full README. Uses
  the existing `addons/gool/` prefabs; no new public API.

  - `scripts/main.gd` — scene controller: bootstrap, listener
    tracking, wiring of all subsystems
  - `scripts/audio_setup.gd` — synthesizes and registers all sounds
    (3 weapons × fire+tail, 3 footstep variants, 3 music states,
    ambience bed, UI feedback). Procedural synthesis only — zero
    asset dependencies, clone-and-press-Play
  - `scripts/player_controller.gd` — WASD movement, fire input,
    weapon cycling. Footsteps emitted on distance-traveled
    threshold (the docs/multiplayer.md §13 pattern: never RPC
    footsteps)
  - `scripts/ai_bot.gd` — wander/pause/burst-fire state machine
    standing in for what would be three remote peers in a real
    co-op session
  - `scripts/weapon.gd` — weapon component with cooldown, three
    weapon kinds (pistol/rifle/shotgun), local-vs-remote sound
    selection (remote fires get a delayed distance "tail")
  - `scripts/combat_music_director.gd` — gunfire intensity tracker
    that drives both the music state machine
    (explore→suspicion→combat) and a `combat_intensity` RTPC
    bound to music volume

### Architecture demonstrated

- Three weapon types with distinct timbres (~70 LOC of synthesis math)
- Local vs remote audio routing (player's gun is loud near-field;
  bot guns get distance attenuation + a delayed tail layer)
- Footsteps generated locally per character via
  `FootstepSurfacePlayer` prefab; never RPC'd
- Music state machine with explore→suspicion→combat transitions
  driven by gunfire activity windowing
- RTPC-driven music ducking under heavy combat (the GDScript-
  exposed analog to the C++ multi-tier sidechain ducking)
- Continuous ambient world bed via long-lived
  `AudioEmitter3D` looping
- UI feedback (weapon cycle blip) routing through the engine's
  separate UI category

### Known limitations (documented in the template's README)

- **Single-host only.** AI bots stand in for three remote peers. The
  README walks through the four-step path to real multiplayer:
  swap the direct `Gool.play_3d` calls in `weapon.gd` for
  `NetworkedAudioEvent.play()`, configure peer-relevancy filtering,
  use Godot's `MultiplayerSpawner` for transforms, drop the bots.
- **RTPC-driven music ducking instead of sidechain bus
  compression.** The runtime singleton's `init(sr, bs)` overload
  uses a default flat bus graph; bus-graph configuration with
  sidechain compressors isn't exposed to GDScript yet. The C++
  engine ships full sidechain compression
  (`examples/multi_tier_ducking/main.cpp`); exposing that to
  GDScript is a roadmap follow-up.
- **Voice chat not exercised.** The `VoiceChatPlayer` prefab is
  available; this demo just doesn't use it because there's no
  second machine to send packets from. The quickstart example
  demonstrates the binding-level hookup.

### Why synthesized audio rather than CC0 freesound packs

Two reasons:

1. **Reproducibility** — anyone clones the repo, opens the
   project, gets the same demo experience. No "go download these
   200 MB of sounds" step.
2. **Demonstrates the data path** —
   `register_pcm_sound(name, PackedFloat32Array, sample_rate, channels)`
   works for any PCM source: synthesized, decoded from your own
   format, captured from a microphone, generated by an LLM, anything.
   Showing it work with synthesized data makes the data-flow point
   clearly. The README explains how to swap to file-based assets
   when you have them.

### Tests

- Total **35/35** passing (no test changes; engine code unchanged).
- The template itself can't be unit-tested in CI without a Godot
  runtime; that's a future addition (Godot headless test mode).

## [0.9.0] - 2026-05-09

Saturation effect + saturation profiles. Adds a fifth bus-effect
kind (tanh waveshaper for subtle bus glue and impact reinforcement)
and a sibling profile library to `compressor_profiles.h`. Designed
for *light* enhancement — engine-side saturation handles glue and
hit reinforcement; aggressive distortion belongs in the DAW.

### Added

- **`SaturationEffect`** — tanh waveshaper, four parameters (drive,
  mix, output gain, bias). Stateless per-sample (no envelope, no
  ring buffer, no allocations). DC-corrected when bias is non-zero.
  Bypass-fast: when mix is 0 (the default), Process exits before
  the per-sample tanh, so installing the effect on a bus and
  modulating mix from gameplay is the documented pattern. Source
  files at `src/audio_engine/dsp/saturation_effect.{h,cpp}`. ~120
  LOC including comments.

- **`EffectKind::Saturation`** added to the bus effect graph.
  `bus.h` gains four `saturation*` descriptor fields (with
  defaults that make adding the effect a no-op until configured)
  and four runtime parameter IDs (`Saturation_Drive`,
  `Saturation_Mix`, `Saturation_OutputGain`, `Saturation_Bias`,
  IDs 19–22).

- **`include/audio_engine/saturation_profiles.h`** — new public
  header with five curated profiles, sibling to
  `compressor_profiles.h`:
  - `BusGlue()` — drive 1.5, mix 0.15, light master cohesion
  - `DialogueWarmth()` — drive 1.3, mix 0.10, bias 0.05 (asymmetric tube-style warmth)
  - `WeaponBody()` — drive 2.5, mix 0.30, gunshot harmonic body
  - `ImpactCharacter()` — drive 4.0, mix 0.45, bias 0.10 (movie-hit grit)
  - `TapeColor()` — drive 2.0, mix 0.25, music/ambience analog feel

- **`tests/unit/saturation_test.cpp`** — 7 sub-tests covering
  bypass identity, unity-drive matches `tanh(x)` exactly, drive>1
  compresses peaks toward `tanh(drive)`, DC-bias correction, mix
  interpolates linearly between dry and wet, symmetry without
  bias (output of -x equals -output(x), confirming odd-harmonic-only
  character), and runtime parameter changes propagate.

- **`tests/unit/saturation_profile_test.cpp`** — 6 sub-tests
  (one per profile + cross-cut sanity) verifying field constants
  and that each profile produces finite, bounded output on a
  known signal.

- **README updates**: bus-graph subsection now lists saturation in
  the shipped effects roster; new "Effect profiles" subsection
  callouts both `compressor_profiles.h` and `saturation_profiles.h`,
  with usage example and explicit "menu will grow" stub for
  future expansion.

### Aliasing note

No oversampling. tanh introduces harmonics above Nyquist that fold
back as aliasing. At documented profile drive values (≤ 4.0) on
typical game-audio source material this is well below the noise
floor and effectively inaudible. Push drive much higher on bright,
transient-rich sources and aliasing becomes audible. The textbook
fix is a 2× polyphase upsampler around the waveshaper; not shipped
here pending profile data showing real demand. Marked in
`saturation_effect.h` as a follow-up.

### Tests

- Total **35/35** passing (13 added across the two new test
  files). Ducking baseline locked at -17.20 dB.

## [0.8.1] - 2026-05-09

Curated compressor profiles. Adds an opinionated, header-only library
of pre-tuned parameter bundles for common game-audio scenarios:
punch shaping for percussive content, impact containment for
explosions and bass, gentle bus glue, voice smoothing, and music
ducking under voice/SFX. Each profile is one constexpr function
returning a fully-populated `EffectConfig`, with `thresholdDb`
tunable per-call (the one parameter that genuinely depends on host
loudness targets) and any other field overridable after the call.

### Added

- **`include/audio_engine/compressor_profiles.h`** — new public
  header. Nine profiles across four categories. All are
  `inline constexpr`, header-only, no runtime cost beyond returning
  a populated descriptor.

  **Punch** (transients preserved with parallel mix):
  - `DrumBusPunch(thresholdDb = -18)` — 4:1, 10 ms attack, 70 % wet, RMS
  - `FootstepGlue(thresholdDb = -22)` — 3:1, 8 ms attack, 60 % wet, Peak
  - `GunshotSnap(thresholdDb = -16)` — 4:1, 5 ms attack, 8 dB range cap

  **Impact** (contained dynamics):
  - `ExplosionImpact(thresholdDb = -14)` — 5:1, 3 ms attack, 12 dB cap
  - `BassImpact(thresholdDb = -20)` — 3:1, 15 ms attack, 80 Hz sidechain HPF

  **Glue / smoothing**:
  - `MasterBusGlue(thresholdDb = -10)` — 1.5:1, RMS, very gentle final-mix cohesion
  - `VoiceSmoothing(thresholdDb = -18)` — 4:1, 30 ms hold, RMS, dialogue-tuned

  **Sidechain duckers** (host wires `compressorSidechainBus` separately):
  - `MusicDuckUnderVoice(thresholdDb = -22)` — 8:1, 200 Hz HPF, 12 dB cap
  - `MusicDuckUnderSfx(thresholdDb = -18)` — 6:1, 150 Hz HPF, 9 dB cap

- **`tests/unit/compressor_profile_test.cpp`** — new test file with
  10 sub-tests. Each profile gets a descriptor sanity check (verifies
  the documented constants haven't drifted) plus an audibility smoke
  test (instantiates a `CompressorEffect` from the profile, runs a
  known signal through, asserts reduction is finite and within the
  range cap if one is set). One cross-cut test verifies determinism
  and that profiles don't touch unrelated `EffectConfig` fields.

### Usage

```cpp
#include "audio_engine/compressor_profiles.h"

// Drop in directly:
bus.effects.push_back(audio::CompressorProfiles::VoiceSmoothing());

// Tune the threshold:
bus.effects.push_back(audio::CompressorProfiles::DrumBusPunch(-15.0f));

// Override anything else after the call:
auto cfg = audio::CompressorProfiles::MusicDuckUnderVoice();
cfg.compressorSidechainBus = kVoiceBusId;     // required for ducker profiles
cfg.compressorReleaseMs    = 300.0f;          // smoother recovery
bus.effects.push_back(cfg);
```

### Tests

- Total **33/33** passing (10 added in the new profile test file).
  Ducking baseline locked at -17.20 dB.

## [0.8.0] - 2026-05-09

Tier A compressor parameters: completes the standard control surface
expected by FMOD/Wwise/plugin users. Six new parameters extend the
existing compressor — knee, mix, range, sidechain HPF, hold, and
detection mode — all defaulted to preserve pre-0.8 behavior, all
runtime-tunable through the existing parameter ID surface.

### Added

- **Soft knee** (`compressorKneeWidthDb`, ID `Compressor_KneeWidthDb`).
  0 = hard knee (legacy behavior, default). Typical musical values
  3–12 dB. Reduction transitions quadratically across a width
  centered on the threshold using the Reiss/McPherson formula.
- **Dry/wet mix** (`compressorMixRatio`, ID `Compressor_MixRatio`).
  1.0 = fully wet (legacy behavior, default), 0.0 = bypass. Enables
  parallel ("New York") compression — keep transient punch while
  adding body.
- **Range cap** (`compressorMaxReductionDb`, ID `Compressor_MaxReductionDb`).
  60 dB ≈ unlimited (legacy behavior, default). Hard cap on gain
  reduction so a runaway transient can't fully duck the signal.
  De-essers and bus glue typically use 3–18 dB.
- **Sidechain high-pass filter** (`compressorSidechainHpfHz`, ID
  `Compressor_SidechainHpfHz`). 0 = bypass (legacy behavior,
  default). Keeps low-frequency content (kicks, explosions) from
  over-triggering compression on a music or VO bus — modern
  game-audio table stakes.
- **Hold** (`compressorHoldMs`, ID `Compressor_HoldMs`). 0 = no hold
  (legacy behavior, default). Delays release engagement by the
  configured duration after the envelope drops below threshold.
  Stabilizes dialogue ducking; prevents compressor chatter on
  choppy trigger sources.
- **Detection mode** (`compressorDetectionMode`, ID
  `Compressor_DetectionMode`). Peak (legacy behavior, default) or
  Rms. Encodes as 0.0f = Peak, 1.0f = Rms when set via runtime ID.

### Changed

- **`CompressorEffect` constructor** now takes a `CompressorConfig`
  struct rather than a positional argument list. The legacy
  6-positional-args form was unsustainable as parameters scaled.
  Hosts constructing the effect directly (rare — most go through
  `EffectKind::Compressor` in `BusEffectDescriptor`) need to
  migrate. The descriptor flow in `BusGraph::Build` is updated.
- **`compressor.h` topology comment** rewritten to reflect the new
  signal path: input → optional sidechain HPF → envelope follower
  (peak or RMS) → soft- or hard-knee gain computer → range cap →
  makeup gain → dry/wet mix → output.

### Behavior preservation

All defaults match v0.7 behavior. Existing descriptors compile
unchanged (the new fields all have defaults). Existing test
suites pass without modification. Ducking baseline preserved
at -17.20 dB.

### Tests

- **`compressor_test.cpp`** gains 6 audibility-verified Tier A
  sub-tests plus its 4 pre-existing tests migrated to the new
  `CompressorConfig` API:
  - `TestSoftKneeMeasurableTransition` — soft knee produces
    measurable reduction at exactly threshold; hard knee does not.
  - `TestMixRatioBlend` — mix=0.0 passes through; mix=0.5 sits
    between dry and fully-wet.
  - `TestMaxReductionCap` — extreme input is bounded to the cap
    even with ratio 100:1 and threshold -40 dB.
  - `TestSidechainHpfFilters` — 60 Hz sidechain triggers
    compression; same content with HPF at 200 Hz does not.
  - `TestHoldDelaysRelease` — reduction stays elevated through
    the hold window before release engages.
  - `TestRmsVsPeakDetection` — RMS detection produces ~2 dB more
    reduction than Peak on a loud/quiet alternating signal at
    slow attack (squaring weights loud samples disproportionately).

- **Total: 32/32 unit tests passing.**

## [0.7.2] - 2026-05-09

Performance baseline pass. Per Rules 9 ("measure before optimizing")
and 25 ("benchmark critical systems"), this release adds the
benchmarking infrastructure and captures a baseline. **No
optimizations were performed in this release.** The data showed
B1 (ParameterSmoother linear scan) and B3 (RTPC binding hash-map
storage) — both flagged as candidates in the v0.7.1 architecture
audit — are not justified for optimization at default budgets.
Documenting that conclusion with real numbers is the deliverable.

### Added

- **`tests/bench/`** — new benchmark directory, CMake'd as
  build-only-not-CTest targets:
  - **`parameter_smoother_bench`** — direct microbenchmark for
    `ParameterSmoother::SetTarget` / `Get` / `Tick` at N=16/64/256/1024
    pre-populated entries.
  - **`rtpc_eval_bench`** — full-path Update measurement: N looping
    emitters with M RTPC bindings each at N×M combinations including
    M=0 for baseline isolation.
- **`tests/bench/bench_util.h`** — minimal harness, no Google
  Benchmark dependency. Wall-clock timing with ns/op reporting and
  a `DoNotOptimize` helper to defeat dead-code elimination on
  microbenchmarks.
- **`docs/perf.md`** — captured baseline numbers, cost decomposition,
  and the Rule-9 conclusion. The "before" any future optimization
  pass must beat.

### Findings

At the documented default budget (`maxActiveEmitters = 128`,
`kRtpcTargetCount = 4`):

| Scenario | µs/tick | % of 16ms frame |
|----------|---------|-----------------|
| N=128, M=0 (no RTPC)        | 11 µs   | 0.07% |
| N=128, M=1 (volume only)    | 93 µs   | 0.6%  |
| N=128, M=4 (all targets)    | 306 µs  | 1.8%  |

The non-RTPC machinery (spatializer, mixer command formation,
occlusion, step 9 itself) is a flat ~11 µs at default budget.
RTPC eval + smoother is ~96% of the variable cost when bindings
are present. At default budgets this is well under any threshold
that justifies refactoring.

At 2× default (N=256) M=4 the cost climbs to 1.15 ms/tick (7% of
frame). Hosts who push budgets aggressively should be aware; the
cost is near-quadratic in active-emitter count.

### Roadmap

- **B1** ParameterSmoother linear scan — measured. **No action.**
  Acceptable at default budgets; bench remains for future
  optimization passes.
- **B3** RTPC binding hash-map storage — measured. Not the dominant
  cost. **No action.**

## [0.7.1] - 2026-05-09

Architecture-rubric cleanup pass. Five small, behavior-preserving
changes that pay down cost surfaces flagged by an audit against
internal C++ engineering rules. No new public features; no breaking
changes; no feature drift (Rule 23: refactors preserve behavior).

### Added

- **`Stats::telemetrySinkExceptions`** and **`Stats::logSinkExceptions`**
  counters (Rules 17, 23). Sink exceptions used to be silently
  swallowed by the runtime's defensive `try`/`catch` so a misbehaving
  host couldn't break Update mid-flight — but invisible failures are
  exactly the silent-failure pattern Rule 17 calls out. Counters are
  atomic (log hooks fire from game and network threads) and surface
  through both `GetStats()` and the per-tick stats sample, so a
  buggy sink shows up on the next non-throwing telemetry emit.
  New sub-test `TestThrowingSinkIncrementsStatsCounter` verifies
  the counter equals the throw count.
- **`AudioRuntimeImpl::ShouldLog_(LogLevel)`** overload (Rules 14, 15).
  The 10 internal call sites that previously did
  `ShouldLog_(static_cast<uint8_t>(LogLevel::Foo))` now read
  `ShouldLog_(LogLevel::Foo)`. The uint8 overload is kept because
  `config_.logMinLevel` is stored as uint8 (to keep `logging.h` out
  of `config.h`) — but that storage detail no longer leaks to call
  sites.
- **`AUDIO_REQUIRES(RenderThread) AUDIO_NO_ALLOC AUDIO_RENDER_PATH`**
  annotations on `IAudioRenderCallback::OnRender` and
  `AudioMixer::OnRender` (Rule 18). Documentary on GCC/MSVC, actively
  enforced under Clang Thread Safety Analysis. The README's
  long-standing "render thread does no allocations, no locks, no
  syscalls, no exceptions" promise is now type-system-supervised
  for the two methods that matter most.

### Changed

- **`globalParameters_` and `soundRtpcBindings_` reserved at Initialize**
  (Rule 8). Both `unordered_map`s now `reserve()` their configured
  caps (`maxGlobalParameters`, `maxSoundRtpcBindings / 4`) on
  Initialize, eliminating the rehash bursts during the first dozen
  inserts. Pure win — predictable runtime memory.
- **`RingTelemetrySink` and `RingLogSink` are now actually thread-safe**
  (Rule 18). Both ring sinks gain an internal `std::mutex` covering
  writes (`OnRuntimeStats` / `OnLogEvent`), `Snapshot()`, `Size()`,
  and `Clear()`. Header docs rewritten to reflect reality:
  - The telemetry ring's old comment claimed "single-threaded by
    contract" but offered no enforcement — now it's locked, callable
    from any thread.
  - The log ring's old comment was outright misleading: the runtime
    holds `logMutex_` around `OnLogEvent`, but host calls to
    `Snapshot()` from a different thread (typical for debug overlays)
    raced against writes. Now it locks; the race is closed.
  - `ForEachInOrder()` deliberately stays unlocked on both ring
    sinks for callers who need allocation-free iteration and can
    guarantee no concurrent emission. The constraint is documented.

### Internal

- `audio_runtime_impl.h` now includes `audio_engine/logging.h` (it's
  an internal header — no compile-time-coupling cost to public headers,
  per Rule 21).
- Sink-exception counters are `mutable std::atomic<uint64_t>` so the
  const `Log_` method can `fetch_add` on the catch path. Loaded
  with `memory_order_relaxed` — a non-torn read is sufficient,
  no happens-before is needed.

### Tests

- Total **32/32** passing (1 added sub-test in `telemetry_test`),
  ducking baseline locked at -17.20 dB.

## [0.7.0] - 2026-05-09

Event-level structured logging. Telemetry told you *that* something
happened (counters); logging now tells you *why* (per-event detail).
Closes Phase 4.8.

### Added

- **`include/audio_engine/logging.h`** — new public header.
  - `IRuntimeLogSink` interface: one method,
    `OnLogEvent(const LogEvent&)`, called from whatever thread
    triggered the underlying event. The runtime serializes calls
    via an internal mutex so sinks **don't need to be thread-safe
    themselves**.
  - `LogLevel` enum: Trace / Debug / Info / Warn / Error.
  - `LogField` tagged union: `int64_t` / `uint64_t` / `double` /
    `bool` / `string_view`. Stack-allocated by callers (no heap
    on the runtime side).
  - `LogCategory::*` constants for built-in hook categories
    (`events`, `mixer`, `voice`, `rtpc`, `emitter`, `prediction`,
    `replication`).
  - **`JsonLinesLogSink`** — one compact JSON object per event.
    Atomic at the FD level for typical line sizes (<PIPE_BUF on
    POSIX). JSON-escapes special characters; thread-local line
    buffer amortizes allocations.
  - **`RingLogSink`** — circular buffer of last N events for
    in-process queries (debug overlays, post-mortems, replay
    correlation). Deep-copies events including string-view fields,
    so stored events remain valid after the originating call
    returns.
- **`AudioRuntimeDependencies::logSink`** — optional raw pointer,
  host-managed.
- **`AudioConfig::logMinLevel`** — minimum severity reaching the
  sink. Default Info; events below the threshold are dropped before
  the sink is consulted, and field-array construction at the call
  site is skipped via `ShouldLog_`. Disabled categories cost a
  branch, not a sink call.

### Hook points wired in v0.7.0

Every hook follows the pattern of *first* incrementing the existing
counter (so telemetry stays correct), *then* checking `ShouldLog_`
*before* building the field array. This keeps the disabled-category
fast-path branch-only.

| Category    | Level | Trigger                                                  |
|-------------|-------|----------------------------------------------------------|
| events      | Debug | Late event discarded (game and replicated paths)         |
| rtpc        | Warn  | RTPC binding rejected: budget exceeded                   |
| emitter     | Debug | One-shot evicted (lower-priority slot freed for incoming)|
| emitter     | Debug | One-shot dropped: pool full, no eviction candidate       |
| emitter     | Warn  | One-shot dropped: post-eviction emitter create failed    |
| replication | Warn  | Replication policy violation rejected                    |
| replication | Debug | Replication event rejected by host validator             |
| replication | Debug | Replication event rate-limited                           |
| mixer       | Warn  | Render-thread underrun(s) since last tick (delta detect) |

The mixer hook deserves special note: render-thread events never
log directly (that thread does no allocations and no syscalls). The
game thread observes the underrun counter delta in Update step 12
and emits the log line from there. Bursts collapse into a single
"underruns: N" event so logs don't drown a flapping audio device.

### Tests

- **`tests/unit/logging_test.cpp`** (new, 9 sub-tests):
  - JSON Lines: compact output, all fields present in expected order
  - JSON Lines: special chars (`\n`, `\t`, `"`) escape correctly
  - Ring sink: chronological order, evicts at capacity, deep-copies
    StrView fields after the original buffer goes out of scope
  - Level filter: Debug events dropped when minLevel=Warn
  - Level filter: Debug events reach sink when minLevel=Debug
  - Null sink with low minLevel is safe (no crash)
  - End-to-end: RTPC budget exceeded fires exactly one Warn
    `rtpc` log line with the expected `budget` field
  - End-to-end: replication policy violation fires exactly one Warn
    `replication` log line with the expected `player_id` field
  - End-to-end: late-event discard fires exactly one Debug `events`
    log line with the expected `replicated` field

Total **32/32** passing, ducking baseline locked at -17.20 dB.

### Limitations carried forward

- The runtime serializes sink calls via one global mutex, not
  per-category locks. Highly contended hot paths (thousands of
  rejections per second) would serialize through this mutex. In
  practice rejections are by definition rare; if a host hits this
  ceiling they likely have a misconfigured rate limiter or DoS in
  flight, and the lock contention is the least of their problems.
- Per-category level filtering is not exposed in v0.7.0 — the only
  knob is global `logMinLevel`. Hosts that want "verbose voice but
  quiet replication" can either filter inside their sink, or wait
  for a future iteration if real users ask.
- No log rotation, retention, or compression in the built-in sinks.
  Those concerns live with the host's log shipper (vector,
  fluentd, journald) — the runtime's job is to emit; the
  pipeline's job is to manage.

## [0.6.0] - 2026-05-09

Telemetry hooks. Teams running real games can now stream the
runtime's `Stats` snapshot into Prometheus, Datadog, journald,
fluentd, custom analytics, or an in-process ring buffer — at a
configurable cadence, with a single sink interface and three
built-in implementations. Closes Phase 4.7.

### Added

- **`include/audio_engine/telemetry.h`** — new public header.
  - `IRuntimeTelemetrySink` interface: one method,
    `OnRuntimeStats(const RuntimeStatsSample&)`, called at
    `telemetryIntervalMs` cadence from `Update()`.
  - **`JsonLinesTelemetrySink`** writes one compact JSON object per
    sample to any `FILE*` (default stdout). Deterministic field
    order, every key always emitted, atomic FD-level writes via a
    single `fprintf`. Pipes cleanly into journald / vector / fluentd /
    a plain log file.
  - **`PrometheusTelemetrySink`** maintains a thread-safe exposition-
    format snapshot. `Snapshot()` returns the latest text from any
    thread; the host's HTTP scrape handler serves it verbatim.
    Output uses `gool_` prefix, `_total` suffix on counters, gauge
    naming for point-in-time values, `# HELP` / `# TYPE` blocks for
    every metric, `category="..."` labels on per-category
    replication counters.
  - **`RingTelemetrySink`** keeps the last N samples (default 512,
    ≈2 minutes at 4 Hz) in a circular buffer. `Snapshot()` returns
    a chronologically ordered vector; `ForEachInOrder()` iterates
    without allocating. Single-threaded by contract — for in-game
    debug overlays, replay correlation, time-series queries that
    don't need to leave the process.
- **`AudioRuntimeDependencies::telemetrySink`** — optional raw
  pointer, host-managed, never deleted by the runtime.
- **`AudioConfig::telemetryIntervalMs`** — default 0 (disabled).
  Recommended values documented inline: 100 ms for tight diagnostics,
  250 ms for live dashboards, 1000 ms for shipped builds.
- **Update step 12** (new): accumulator-based emit scheduling that
  *subtracts* the interval rather than zeroing — so a long host
  frame catches up by emitting again immediately on the next
  Update, rather than losing samples. Sink call wrapped in
  `try`/`catch` so a misbehaving host implementation can't break
  Update mid-flight.
- **`examples/telemetry/main.cpp`** — working demo wiring all three
  sinks side-by-side. Prints a JSON Lines stream, a Prometheus
  scrape body, and the last 5 ring-buffer samples.

### Tests

- **`tests/unit/telemetry_test.cpp`** (new, 9 sub-tests):
  - Every documented JSON field appears in output, deterministic order
  - Null `FILE*` no-ops gracefully
  - Prometheus output has HELP / TYPE blocks and correct label syntax
    for both gauges and counters; per-category labels work
  - Ring buffer chronologically ordered, evicts oldest at capacity,
    `ForEachInOrder` iterates without allocating
  - Runtime emits at configured cadence (9 samples over 1 s at 100 ms
    — within ±1 expected slack from accumulator boundary fuzz)
  - Interval=0 emits zero samples
  - Nullptr sink with non-zero interval is safe (no crash)
  - End-to-end: ring sink fed by runtime captures monotonic time series
- Total **31/31** passing, ducking baseline locked at -17.20 dB.

### Limitations carried into the next iteration

- The sink interface carries global `Stats` only. Per-player voice
  metrics (jitter, packet-loss per player) need host-side iteration
  — cardinality is host-dependent (player IDs come and go, dashboard
  labels would explode). Pattern shown in test setup but not in the
  sink interface itself.
- No event-level structured logging. The runtime emits counter
  *aggregates* through the sink but not the individual events that
  drove those counters (which voice packet was rejected, which RTPC
  binding hit budget). See roadmap Phase 4.8 — separate iteration.

## [0.5.0] - 2026-05-09

Multi-target RTPC. The single-target volume binding from v0.4 generalizes
to four targets, four curves, multiple bindings per sound, and JSON
authoring. The pattern that took 1 binding in v0.4 ("heartbeat volume
follows health") now scales to 4 bindings ("heartbeat volume + pitch
follow health, music volume ducks under combat with a smoothstep,
caves apply lowpass via wetness").

### Added

- **Four RTPC targets**: Volume (multiplicative on gain), Pitch
  (multiplicative on pitch), LowPassCutoff (max with spatial baseline),
  ReverbSend (clamped sum with spatial baseline). `RtpcTarget` enum.
- **Four curves**: Linear, Exponential, InverseExponential, SCurve
  (smoothstep). `RtpcCurve` enum + `curveExponent` for exp/inv-exp.
- **Multiple bindings per sound**: at most one binding per (sound,
  target) pair. A single sound can have volume + pitch + lowpass +
  reverb all driven by different parameters simultaneously.
- **`AudioRuntime::SetSoundRtpc(soundId, binding)`** — unified API
  taking a `SoundRtpcBinding` struct. Replaces v0.4's
  `SetSoundVolumeRtpc` (mechanical migration: pass binding fields
  via the struct).
- **`ClearSoundRtpc(soundId, target)`** — remove one target's binding.
- **`ClearAllSoundRtpc(soundId)`** — remove all bindings for a sound;
  returns count removed.
- **JSON sound bank `rtpc` array** — bindings can now be authored
  alongside sound definitions in `.json` banks. Schema:

  ```json
  {
    "name": "heartbeat",
    "category": "SFX",
    "rtpc": [
      { "parameter": "health",
        "target":    "volume",
        "curve":     "linear",
        "min_value": 0.0, "max_value": 1.0,
        "min_output": 1.0, "max_output": 0.0,
        "smoothing_ms": 50 },
      { "parameter": "fatigue",
        "target":    "pitch",
        "curve":     "exponential", "exponent": 2.0,
        "min_value": 0.0, "max_value": 1.0,
        "min_output": 1.0, "max_output": 0.85 }
    ]
  }
  ```

  Unknown target/curve names produce line-numbered error messages.
- **GDScript autoload facades**:
  - `Gool.bind_volume_rtpc(...)` — Volume + linear (the v0.4 ergonomics, preserved)
  - `Gool.bind_pitch_rtpc(...)` — Pitch + linear
  - `Gool.bind_lowpass_rtpc(...)` — LowPass + linear
  - `Gool.bind_reverb_rtpc(...)` — ReverbSend + linear
  - `Gool.bind_rtpc(sound_name, dict)` — full API: any target, any curve
  - `Gool.clear_rtpc_binding(name, target)` / `Gool.clear_all_rtpc_bindings(name)`
- **GDExtension bindings**: `set_sound_rtpc` (target+curve as strings),
  `clear_sound_rtpc`, `clear_all_sound_rtpc`, `sound_rtpc_binding_count`.

### Changed

- **BREAKING (pre-1.0)**: `AudioRuntime::SetSoundVolumeRtpc` /
  `ClearSoundVolumeRtpc` removed. Migration: replace with
  `SetSoundRtpc(soundId, binding)` where `binding.target = RtpcTarget::Volume`
  and the field names map directly. Same for the GDScript
  `Gool.set_sound_volume_rtpc` / `Gool.clear_sound_volume_rtpc` GDExtension
  methods. The GDScript `Gool.bind_volume_rtpc` facade stays with the
  same signature so call sites that use the facade don't need changes.
- Storage moves from `unordered_map<AudioSoundId, SoundVolumeRtpcBinding>`
  to `unordered_map<AudioSoundId, std::vector<SoundRtpcBinding>>`.
  `AudioConfig::maxSoundRtpcBindings` (256) now caps total bindings
  across all sounds, not distinct sound IDs — a sound with 4 bindings
  counts as 4 against the budget.
- Step 9 of `Update` (per-emitter `UpdateParams` pass) now reads
  `LowPassAmount` and `ReverbSend` from the parameter smoother in
  addition to `Gain` and `Pitch`. Default fallbacks preserve existing
  unbound behavior.

### Tests

- `tests/unit/sound_rtpc_test.cpp` rewritten (7 sub-tests):
  Volume/Pitch/LowPass audibility, multi-binding coexistence, four
  curves at midpoint behave correctly (linear=0.125, exp(2)=0.0625,
  inv-exp(2)=0.1875, scurve=0.125 from a 0.25 reference), skip-when-unset
  per-binding, API validation including out-of-range enums.
- `tests/unit/sound_bank_test.cpp` extended with 2 new sub-tests:
  RTPC array parses and registers bindings; unknown target string is
  rejected with line number. Total 30/30 passing.

### Limitations carried into the next iteration

- Custom point-list curves (arbitrary curve shapes via JSON-authored
  control points) are still future. Linear / Exponential /
  InverseExponential / SCurve cover the typical FMOD/Wwise authoring
  patterns.
- LowPassCutoff combines via `max()` with the spatial baseline (so RTPC
  can never reduce the world's filter). Use cases that want RTPC to
  override spatial filtering (e.g. underwater zone replaces occlusion)
  need a different combiner — roadmap.
- Bindings are still per-sound, not per-emitter or per-bus. Per-bus
  RTPC modulation (e.g. "all music quiets when combat starts" without
  binding every track individually) is a separate feature.

## [0.4.0] - 2026-05-09

Render-thread RTPC volume modulation. The disclaimer in v0.3.0
("`set_rtpc` stores values but does not yet drive sound modulation")
is closed. Calling `bind_volume_rtpc("heartbeat", "health", 0, 1, 1, 0)`
once, then `set_rtpc("health", v)` per frame, now actually changes the
heartbeat's rendered volume in real time.

### Added

- **`AudioRuntime::SetSoundVolumeRtpc`** / **`ClearSoundVolumeRtpc`** /
  **`GetSoundRtpcBindingCount`** — bind a sound's volume to a global
  parameter via a linear curve. Each `Update` tick the runtime walks
  active emitters, looks up each one's binding, reads the parameter,
  computes a target gain, and pushes it through the existing parameter
  smoother (`AudioParameterIds::Gain`). Same code path used by
  `SetEmitterParameter` so authored modulation and manual gain calls
  compose cleanly.
- **`Gool.bind_volume_rtpc(sound_name, param_name, ...)`** /
  **`Gool.clear_volume_rtpc(sound_name)`** — GDScript autoload facade
  + GDExtension bindings (`set_sound_volume_rtpc`,
  `clear_sound_volume_rtpc`, `sound_rtpc_binding_count`).
- **`AudioConfig::maxSoundRtpcBindings`** (default 256). Budget is
  enforced only on new sound IDs — re-binding an existing sound is
  always free. `BudgetExceeded` returned when the cap is hit.
- **Skip-when-unset semantics**: until `set_rtpc(name, ...)` is called
  at least once, the binding has no effect. Authored volume stays in
  place. Binding-installation order is then independent of gameplay
  state so prefab `_ready()` calls can wire bindings without worrying
  about sequencing.
- README Quick Start now shows the bind + set_rtpc pattern.

### Tests

- `tests/unit/sound_rtpc_test.cpp` — 8 sub-tests, audibility-verified
  end-to-end:
  - Unset parameter: rendered RMS = 0.25 (authored volume preserved)
  - Parameter at 0 with `0→0, 1→1` binding: rendered RMS = 0 (silent)
  - Parameter at 1: rendered RMS = 0.25 (full)
  - Parameter at 0.5: rendered RMS = 0.125 (exactly half, ratio = 0.5)
  - Inverted binding `1→0, 0→1` at full health: silent (heartbeat pattern)
  - Out-of-range parameter values clamp correctly to endpoints
  - Clear stops modulation
  - API validation rejects NaN, degenerate range, invalid IDs, negative smoothing

  Total now 30/30 passing.

### Limitations carried into the next iteration

- One binding per sound. Binding multiple parameters to one sound
  (volume + pitch + lowpass independently) is a future M-sized item.
- Volume only. Pitch / lowpass cutoff / send-level modulation are
  roadmap.
- Linear curve only. Exponential and custom-point curves are roadmap.
- The orchestrator's per-emitter `UpdateParams` pass that carries the
  modulated gain to the mixer only runs when a listener is registered.
  This was the original behavior; documented now in the test setup
  comments and `EvaluateRtpcBindings_` docs.

## [0.3.0] - 2026-05-09

The tiny API facade. Four canonical entry points users can copy-paste
verbatim into a fresh Godot project: `Gool.play_3d`, `Gool.play_music_state`,
`Gool.play_voice`, `Gool.set_rtpc`. Each is a thin GDScript wrapper over
the lower-level engine APIs; users drop down to the raw bindings when
they outgrow them.

### Added

- **`Gool.play_3d(name, position, priority=128)`** — one-shot 3D playback
  by authored sound name. Wraps `submit_event_local` with sane defaults.
- **`Gool.play_music_state(state_name, fade_ms=500)`** — equal-power
  crossfade to a new music state. Lazily creates a `GoolMusicChannel`
  on first call. Idempotent: re-passing the current state is a no-op.
- **`Gool.play_voice(player_id, audio_stream)`** — decode an
  `AudioStreamWAV` (FORMAT_16_BITS) to mono float32 PCM, register as
  an ephemeral one-shot, dispatch through the play path. Raises a
  push_error on unsupported formats. AudioStreamOggVorbis support is
  on the roadmap; for raw Opus voice traffic from a network layer,
  use `Gool.submit_voice_packet` directly.
- **`Gool.set_rtpc(name, value)`** / **`get_rtpc`** / **`has_rtpc`** /
  **`clear_rtpc`** — string-keyed real-time parameter store. Authored
  sound definitions reading these to drive volume / cutoff / pitch is
  a future feature; the storage and observability ship now so host
  code can build against the API.
- **`AudioRuntime::SetGlobalParameter`** / **`GetGlobalParameter`** /
  **`ClearGlobalParameter`** / **`GetGlobalParameterCount`** — C++ API
  for the global parameter store. Game-thread access at this stage;
  render-thread modulation is a follow-up.
- **`HashParameterName(name)`** constexpr in `types.h`. FNV-1a, same
  shape as `HashSoundName`. Hashes that would collide with the
  engine-reserved range `[1, HostBase)` are bumped above
  `AudioParameterIds::HostBase` so host names can't mask engine
  semantics.
- **`AudioConfig::maxGlobalParameters`** (default 256). Budget is
  enforced only on new IDs — updating an existing parameter is
  always free.
- **`Gool.stop_music(fade_ms=500)`** — companion to `play_music_state`.
- GDExtension bindings: `set_global_parameter`, `get_global_parameter`,
  `has_global_parameter`, `clear_global_parameter`,
  `global_parameter_count`, `hash_parameter_name`.

### Tests

- `tests/unit/global_parameter_test.cpp` — 7 sub-tests covering hash
  stability + reserved-range remapping, set/get round-trip, unset
  returns false, clear semantics, budget enforcement (only on new
  IDs), NotInitialized, InvalidArgument. Total 29/29 passing.

### Documentation

- README Quick Start now leads with the four facade lines, ahead of
  the prefab-node walkthrough.
- Phase 1.4 marked SHIPPED in 0.3.0 in the roadmap.

## [0.2.0] - 2026-05-09

The first public release with binary artifacts. Adds the multiplayer
hardening pass: rate limiting, replication-policy enforcement, threat
model documentation, and the `DefaultBoundsValidator` for malformed
input.

### Added

- **Replication rate limiter** (Phase 2.3): per-player, per-category
  token-bucket rate limiter on `SubmitReplicatedEvent`. Defaults
  sized for plausible gameplay (50 SFX/sec, 150 voice/sec, etc.).
  Surfaced via `Stats::replicationEventsRateLimited[6]`.
- **`IReplicationValidator`** interface for host-supplied policy
  hooks. Runtime calls before rate limiting; rejection silently
  drops + counts.
- **`AudioCategory category`** field on `AudioEvent`. Defaults to
  `SFX` so existing call sites work unchanged.
- **`AudioRuntime::SetReplicationValidator()`** / **`GetPerPlayerReplicationStats()`**.
- **Voice path rate limiting**: `OnVoicePacket` gates through the
  same per-player Voice category bucket. Per-player drops surface
  in `VoiceNetworkStats::packetsRateLimited` (new field).
- **PlayerId-cycling DoS defense**: per-tick admission cap on
  never-seen-before `playerId`s
  (`ReplicationRateLimitConfig::maxNewPlayersPerTick = 8` default).
  Surfaced via `Stats::replicationEventsRejectedNewIdBudget`.
- **`ReplicationSource` enum** + 2-arg `SubmitReplicatedEvent(event, source)`
  overload (Phase 2.5). Client-sourced `ServerAuthoritative` events
  rejected with `AudioResult::PolicyViolation` — verified via
  audibility check (rendered RMS = 0).
- **`replicationPolicyViolations` counter**, distinct from
  `replicationEventsRejectedByValidator`, so dashboards can tell
  protocol enforcement from host-policy denials.
- **`DefaultBoundsValidator`**: a shipped `IReplicationValidator`
  rejecting NaN/Inf vec3 fields, extreme magnitudes, malformed
  parameters, optional unknown soundIds via host callback.
- **`ChainReplicationValidator`**: composes up to 8 validators with
  short-circuit-on-reject.
- **`audio::GetVersion()`** + `version.h` constants. Compile-time
  major/minor/patch + the git SHA stamped at CMake configure time.
- **Threat model documentation** in `docs/replication_patterns.md` —
  what the runtime can and can't validate, four host-side rules,
  monitoring counters.
- **Release infrastructure**: `CHANGELOG.md`, `RELEASING.md`, this
  file. Release workflow (`release.yml`) builds versioned artifacts
  on `v*` tags.
- **Roadmap**: `docs/roadmap.md` with 28 phased work items,
  effort-sized.
- **`AudioResult::RateLimited`** and **`AudioResult::PolicyViolation`**
  return values.

### Changed

- README rewritten workflow-first (2075 → 559 lines). Leads with
  what online multiplayer audio demands and how gool fits, not the
  engine architecture.
- macOS lane temporarily disabled in CI matrix (Apple-Clang issue
  not yet investigated; Linux + Windows green).

### Fixed

- `examples/hello_audio` include path (was breaking miniaudio
  builds).
- `release.yml` multi-line cmake invocation flattened to single
  line (YAML scalar fragility).
- `tests/CMakeLists.txt` `biquad_eq_test` missing from the
  `src/`-on-include-path foreach.

### Security

- Validator-rejected events from never-seen players no longer
  consume LRU slots, closing a "validator hook is its own DoS
  surface" hole.
- `RecordPolicyViolation()` / `RecordValidatorRejection()` use
  `FindExisting()` instead of `FindOrAllocate()` so spoofs from
  unknown players can't inflate the slot table.

## [0.1.0] - 2026-04-XX

Initial private development snapshot. Not formally released; the
first tagged version is 0.2.0 above.

Headlines:

- C++20 audio engine with 25 unit tests passing
- Spatial audio: distance attenuation, Doppler, occlusion (material-
  aware), air absorption, reverb sends, optional binaural
  (`SphericalHeadSpatializer`)
- Voice chat: Opus codec wrapper, adaptive jitter buffer
  (97.84% continuity at 10% loss / 50 ms jitter), PLC, per-player
  telemetry
- Adaptive music: equal-power crossfade (±0.3% RMS through 300 ms
  transitions), `MusicChannel` helper, loop-boundary crossfade
  (158× click reduction)
- Bus graph + sidechain compressor + EQ palette (LP/HP/BP/Shelf/Peak)
- JSON sound banks, `.gpak` archives, hot reload
- Replication: `SubmitReplicatedEvent`, `UpdateReplicatedTransform`,
  `OnVoicePacket` with deterministic-replay arrival timestamp,
  `CancelPredictedEvent`, interest management
- Godot 4.2+ GDExtension binding with 7 prefab Nodes, editor plugin
  with autoload installation

[Unreleased]: https://github.com/siliconight/gool/compare/v0.11.2...HEAD
[0.11.2]: https://github.com/siliconight/gool/releases/tag/v0.11.2
[0.11.1]: https://github.com/siliconight/gool/releases/tag/v0.11.1
[0.11.0]: https://github.com/siliconight/gool/releases/tag/v0.11.0
[0.10.1]: https://github.com/siliconight/gool/releases/tag/v0.10.1
[0.10.0]: https://github.com/siliconight/gool/releases/tag/v0.10.0
[0.9.1]: https://github.com/siliconight/gool/releases/tag/v0.9.1
[0.9.0]: https://github.com/siliconight/gool/releases/tag/v0.9.0
[0.8.1]: https://github.com/siliconight/gool/releases/tag/v0.8.1
[0.8.0]: https://github.com/siliconight/gool/releases/tag/v0.8.0
[0.7.2]: https://github.com/siliconight/gool/releases/tag/v0.7.2
[0.7.1]: https://github.com/siliconight/gool/releases/tag/v0.7.1
[0.7.0]: https://github.com/siliconight/gool/releases/tag/v0.7.0
[0.6.0]: https://github.com/siliconight/gool/releases/tag/v0.6.0
[0.5.0]: https://github.com/siliconight/gool/releases/tag/v0.5.0
[0.4.0]: https://github.com/siliconight/gool/releases/tag/v0.4.0
[0.3.0]: https://github.com/siliconight/gool/releases/tag/v0.3.0
[0.2.0]: https://github.com/siliconight/gool/releases/tag/v0.2.0
[0.1.0]: https://github.com/siliconight/gool/tree/main
