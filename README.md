# audio_engine

A standalone C++20 runtime audio engine for 3D spatial multiplayer games.
Distance attenuation, occlusion, Doppler, reverb, binaural HRTF, sidechain
ducking, Opus voice chat, and the threading model to make all of it run
without allocations or locks on the render thread.

The library namespace is `audio`, the CMake target is `audio_engine`, and
the host-facing orchestrator class is `audio::AudioRuntime`. The engine
ships with a silent backend (for headless tests and CI) and an opt-in
miniaudio backend (for actually hearing things). It does not own physics,
networking, or game state — those are seams the host plugs into.

## Table of contents

- [Quick start](#quick-start) — clone, build, hear audio in 5 minutes
- [Dependencies](#dependencies) — what you need to build and run
- [Integrating into your game](#integrating-into-your-game) — minimal walkthrough
- [Features at a glance](#features-at-a-glance) — what's available
- [Sound bank (data-driven asset pipeline)](#sound-bank-data-driven-asset-pipeline) — JSON schema, hot reload
- [Music transitions and playback speed](#music-transitions-and-playback-speed) — equal-power crossfade, runtime speed control
- [Online multiplayer](#online-multiplayer) — voice chat, replication, prediction, determinism
- [Reference](#reference) — design intent, per-subsystem deep dives, internals
- [License](#license)

---

## Quick start

```bash
git clone <your-fork-url> audio_engine
cd audio_engine
cmake -B build -S . -DAUDIO_ENGINE_BACKEND_MINIAUDIO=ON
cmake --build build -j
./build/examples/audio_engine_playback     # audible spatial demo
ctest --test-dir build                      # 16 unit tests
```

That gets you a build with the cross-platform device backend, runs a
demo that plays a moving sound source through your speakers, and runs
the full unit test suite. Without `-DAUDIO_ENGINE_BACKEND_MINIAUDIO=ON`
the build still succeeds but uses the silent `NullAudioBackend` (every
test still passes; no audio comes out of your speakers).

For voice chat, add `-DAUDIO_ENGINE_VOICE_OPUS=ON`. See the
[Dependencies](#dependencies) section below for what that pulls in.

---

## Dependencies

The engine is intentionally light on external dependencies. Most of what
it needs is either part of the C++ standard library or vendored as
single-header files.

### Required to build

| Tool                 | Version       | Notes                                          |
|----------------------|---------------|------------------------------------------------|
| C++ compiler         | C++20         | g++ 10+, Clang 11+, MSVC 19.29+ all work       |
| CMake                | 3.20+         | Specified in `CMakeLists.txt`                  |
| POSIX threads        | —             | `find_package(Threads REQUIRED)` (Windows uses |
|                      |               | the Win32 thread API automatically)            |

That's the entire required surface. No Boost, no Qt, no fmtlib. The
standard library and threads.

### Vendored, ship-with-the-source

These live in `third_party/` as single-header drop-ins. You don't
install anything; CMake either uses the local copy or fetches them via
`FetchContent` if missing. All are public-domain or MIT-equivalent and
add no legal complexity.

| Component       | Purpose                          | License           | Vendored? |
|-----------------|----------------------------------|-------------------|-----------|
| miniaudio       | Cross-platform audio device I/O  | Unlicense / MIT-0 | yes       |
| dr_wav          | WAV file decoding                | Unlicense / MIT-0 | yes       |
| dr_flac         | FLAC file decoding               | Unlicense / MIT-0 | yes       |
| stb_vorbis      | Ogg Vorbis file decoding         | public domain / MIT | yes     |

Each of these is gated behind a CMake option (`AUDIO_ENGINE_DECODERS_WAV`,
`_OGG`, `_FLAC`, `AUDIO_ENGINE_BACKEND_MINIAUDIO`) so you only compile in
what you use.

### Optional, build-time enabled

| Component       | Purpose                          | License           | Vendored?       |
|-----------------|----------------------------------|-------------------|-----------------|
| libopus         | Voice chat encode/decode         | BSD-3-Clause      | no (fetched or system) |

Voice chat is opt-in via `-DAUDIO_ENGINE_VOICE_OPUS=ON`. With that flag
on, CMake resolves libopus in three strategies, in order: a vendored
CMake project under `third_party/opus/`, a system package via
`find_package(Opus)` / `pkg-config`, or a fetch from xiph/opus via
`FetchContent`. Without the flag, the codec wrapper compiles to an
empty translation unit and the engine reports voice as unsupported at
runtime — host code can still reference `OpusVoiceCodec` without a
preprocessor guard.

### What your game must provide

The engine has four polymorphism seams. It ships defaults for all of
them so the engine compiles and runs out of the box, but production
games will replace at least one:

| Seam                    | Default                            | When to replace                                          |
|-------------------------|------------------------------------|----------------------------------------------------------|
| `IAudioBackend`         | `NullAudioBackend` (silent)        | Always, for shipping. Use `MiniaudioBackend` or write your own. |
| `IAudioGeometryQuery`   | `NullGeometryQuery` (always clear) | When you want occlusion to reflect game geometry. Wrap your physics raycaster. |
| `ISpatializer`          | `DefaultSpatializer` (pan + LPF)   | When you want HRTF (use `SphericalHeadSpatializer`).      |
| `IVoiceCodec`           | `StubVoiceCodec` (PCM passthrough) | For network-shipped voice chat, use `OpusVoiceCodec`.     |

You don't need to provide all four to start. The minimum useful host
provides an `IAudioBackend` (or uses the bundled miniaudio one) and an
`IAudioGeometryQuery` if it cares about occlusion.

### Platform support

The engine itself is portable C++20. Tested with:

- Linux (Ubuntu 22.04 / 24.04, glibc, g++ 11+)
- macOS (12+, Apple Clang)
- Windows (10+, MSVC 19.29+ via Visual Studio 2019/2022)

`MiniaudioBackend` extends the same set: ALSA / PulseAudio on Linux,
CoreAudio on macOS, WASAPI on Windows. No vendor SDK required.

The render thread does no allocations, no locks, no syscalls, and no
exceptions — that's enforced architecturally, not just by policy. A
real-time-priority audio thread on any of the supported platforms will
behave identically to the test suite's offline backend.

---

## Integrating into your game

The shortest path from "I have a game with a tick loop" to "I'm hearing
3D-positioned audio." This is a complete, working program.

```cpp
#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"          // IAudioBackend
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"
#include "audio_engine/events.h"

// If you build with -DAUDIO_ENGINE_BACKEND_MINIAUDIO=ON the bundled
// backend is available:
#include "audio_engine/backend/miniaudio_backend.h"
// Otherwise write your own IAudioBackend (or use NullAudioBackend
// for headless tests).

#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

using namespace audio;

int main() {
    // 1) Configure. Defaults are fine for most games; override budgets
    //    if you have an unusual emitter/voice count.
    AudioConfig cfg;
    cfg.sampleRate = 48000;
    cfg.bufferSize = 512;
    cfg.outputMode = AudioOutputMode::Stereo;

    // 2) Wire dependencies. Backend is required; spatializer, geometry,
    //    and voice codec all have null/default fallbacks.
    AudioRuntimeDependencies deps;
    deps.backend = std::make_unique<MiniaudioBackend>();   // hear it

    // 3) Initialize.
    AudioRuntime rt;
    if (rt.Initialize(cfg, std::move(deps)) != AudioResult::Success) {
        return 1;
    }

    // 4) Tell the engine where the listener is.
    AudioListener lis;
    lis.position = {0.0f, 0.0f, 0.0f};
    lis.forward  = {0.0f, 0.0f, -1.0f};
    lis.up       = {0.0f, 1.0f,  0.0f};
    rt.SetListener(lis);

    // 5) Register a PCM sound. Real games load WAV/Ogg/FLAC via
    //    decoders in src/audio_engine/decoders/; here we just generate
    //    a 1-second 440 Hz sine for the example.
    constexpr AudioSoundId kBeep = 1;
    std::vector<float> beep(48000);
    for (size_t i = 0; i < beep.size(); ++i) {
        beep[i] = 0.5f * std::sin(2.0f * 3.14159f * 440.0f * i / 48000.0f);
    }
    rt.RegisterPcmSound(kBeep, beep, 48000, /*channels*/ 1);

    SoundDefinition def;
    def.soundId     = kBeep;
    def.category    = AudioCategory::SFX;
    def.targetBus   = kBusMaster;
    def.spatialized = true;
    def.attenuation.minDistance = 1.0f;
    def.attenuation.maxDistance = 50.0f;
    def.attenuation.volumeFloor = 0.0f;
    rt.RegisterSoundDefinition(def);

    // 6) Fire a one-shot event from a position 5 m to the listener's right.
    AudioEvent ev = AudioEvent::MakePlaySoundAtLocation(
        kBeep, Vec3{5.0f, 0.0f, 0.0f});
    rt.SubmitEvent(ev);

    // 7) Tick the engine each game frame. Pass elapsed wall time.
    for (int frame = 0; frame < 60; ++frame) {
        rt.Update(1.0f / 60.0f);          // 60 Hz tick
        // ... your game logic here ...
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    rt.Shutdown();
    return 0;
}
```

That's the full picture. Real games extend this with:

- **Persistent emitters** for things that stay in the world (vehicle
  engines, music): use `CreateEmitter` to get a handle, call
  `SetEmitterTransform` each tick to keep it positioned.
- **Streaming sounds** for long-form dialogue/music: use the streaming
  decoder path instead of `RegisterPcmSound`.
- **Voice chat** between players: register a voice source per remote
  player, push received Opus packets through `OnVoicePacket`.
- **Custom occlusion**: implement `IAudioGeometryQuery::RaycastAudioOcclusion`
  to wrap your physics raycaster, set the resulting `AudioMaterial` so
  curtains and concrete sound different.
- **Bus graph and ducking**: configure buses in `AudioConfig::busGraph`
  before `Initialize`. See `examples/ducking/` and
  `examples/multi_tier_ducking/` for the L4D2-style "your shot ducks
  music and remote shots" pattern.
- **Replicated events** from network: call `SubmitReplicatedEvent` on
  the network thread when you receive a remote sound trigger.

Each of these has a dedicated section in the [Reference](#reference)
below.

### Threading model in one paragraph

You'll have four threads talking to the engine:

- **Game thread** calls `Update`, `SubmitEvent`, `CreateEmitter`,
  `SetEmitterTransform`, `RegisterPcmSound`, `GetStats`.
- **Network thread** calls `OnTickAdvanced`, `SubmitReplicatedEvent`,
  `UpdateReplicatedTransform`, `OnVoicePacket`.
- **Control thread** is internal to the engine — it runs the spatializer,
  occlusion raycasts, and asset streaming pump. You never touch it.
- **Render thread** is also internal — driven by the backend, runs the
  mixer. Allocates nothing, locks nothing.

These boundaries are enforced via Clang Thread Safety annotations (the
`AUDIO_REQUIRES(GameThread)` etc. attributes). You won't get
"accidentally call from the wrong thread" bugs at runtime — the
compiler catches them at build time.

---

## Features at a glance

- 3D spatial: distance attenuation, equal-power pan, Doppler with
  per-voice pitch smoothing, distance-driven air absorption
- Occlusion: per-voice biquad LPF + gain, material-aware
  (Concrete/Curtain/Glass/...) via `AudioMaterial` enum
- Binaural (opt-in): `SphericalHeadSpatializer` with Woodworth ITD +
  head-shadow ILD. Seam for HRTF data plug-in later.
- Bus graph: hierarchical buses, per-bus effect chain, sidechain
  compressor, biquad EQ, Freeverb-derived reverb send pattern
- Voice cap with priority eviction; persistent emitters immune to
  eviction
- File decoding: WAV (dr_wav), Ogg Vorbis (stb_vorbis), FLAC (dr_flac)
- Streaming: control-thread pump + per-voice SPSC ring
- Voice chat: Opus codec wrapper, VOIP-tuned (FEC, complexity 5,
  32 kbps VBR), per-player decoder pool with LRU eviction.
  **Adaptive jitter buffer** with RFC 3550 inter-arrival jitter
  estimation, packet-loss concealment via libopus PLC, sequence
  reorder handling, and per-player network telemetry. Tested at
  97.84% audible-frame continuity on 10% loss / 50 ms jitter
  (real residential-internet conditions).
- Network seam: `OnTickAdvanced`, `SubmitReplicatedEvent`,
  `UpdateReplicatedTransform`, `OnVoicePacket`. Per-event staleness
  override; predicted-event cancellation; interest management for
  large emitter counts.
- **Asset pipeline**: JSON sound banks with stable hashed IDs,
  defaults blocks, three group selection policies (random,
  random_no_repeat, sequential), hot reload, optional file-loader
  callback for asset packs. Load 1000 entries in 0.6 ms; `Find()` at
  18.5 M ops/s. Designers author JSON; programmers reference sounds
  by string name.
- **Music transitions and playback speed**: equal-power crossfade
  curves (cos/sin) on `EmitterDescriptor::fadeInMs` and
  `DestroyEmitter(handle, fadeOutMs)`; total power held within ±0.3%
  of baseline through a 300 ms A→B transition (measured).
  `MusicChannel` helper for the canonical two-track crossfade
  pattern. `SetEmitterPlaybackSpeed(handle, speed)` adjusts playback
  rate at runtime with built-in smoothing — 1.5× speed gives exactly
  1.5× pitch (verified to ±0.5%).

---

## Sound bank (data-driven asset pipeline)

Audio designers shouldn't have to write C++ to add a sound. The
engine ships a JSON-based sound bank loader so the entire game's
audio inventory — file paths, categories, attenuation curves, bus
routing, priority, group/variation behavior — lives in a designer-
editable text file. Programmers reference sounds by stable string
names; the bank hashes those names to `AudioSoundId`s and registers
everything with the runtime.

```cpp
audio::SoundBankLoadOptions opts;
opts.busResolver = [&](std::string_view name) -> audio::BusId {
    if (name == "master") return audio::kBusMaster;
    if (name == "music")  return musicBusId;   // from runtime config
    if (name == "ui")     return uiBusId;
    return audio::kInvalidBusId;
};

audio::SoundBank bank;
auto r = bank.LoadFromJsonFile(runtime, "sounds.json", opts);
if (!r.success) {
    Log("bank load failed at line %d: %s", r.errorLine, r.errorMessage.c_str());
}

const auto id = bank.Find("weapon.ak47.shot");           // hashed lookup
runtime.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(id, position));

// Designer changed sounds.json on disk → re-register without restart:
bank.Reload(runtime);
```

A minimal bank file looks like:

```json
{
  "version": 1,
  "defaults": {
    "category":     "SFX",
    "priority":     "Normal",
    "attenuation":  { "min": 1.0, "max": 50.0, "falloff": "Logarithmic" }
  },
  "sounds": [
    { "name": "footstep.grass.01", "file": "sfx/foot/grass_01.wav" },
    { "name": "footstep.grass.02", "file": "sfx/foot/grass_02.wav" },
    { "name": "weapon.ak47.shot",  "file": "sfx/weapons/ak47.wav",
      "priority": "High",
      "attenuation": { "min": 2.0, "max": 200.0 } }
  ],
  "groups": [
    { "name": "footstep.grass", "policy": "random_no_repeat",
      "members": ["footstep.grass.01", "footstep.grass.02"] }
  ]
}
```

### Stable hashed IDs

Sound IDs are FNV-1a 32-bit hashes of the names. The hash is
exposed as `audio::HashSoundName(name)`, which is a `constexpr`
function — code that wants to skip the `Find()` indirection in the
hottest inner loops can compute the id at compile time. Stable
hashes mean **hot-reload preserves IDs**: an in-flight emitter
holding `id == HashSoundName("weapon.ak47.shot")` keeps working
after the bank reloads.

### Three group selection policies

| Policy             | Behavior                                                         |
|--------------------|------------------------------------------------------------------|
| `random`           | Uniform random over the members.                                 |
| `random_no_repeat` | Uniform random, never the same member twice in a row.            |
| `sequential`       | Cycles through members in declaration order.                     |

### Hot reload, asset packs, and procedural sounds

- **Hot reload** via `bank.Reload(runtime)`. Re-parses the most recent
  source (file or string) and re-registers every entry. IDs survive
  because they're hashes of names, so emitters and queued events
  holding old IDs keep working.
- **Asset packs** via `SoundBankLoadOptions::fileLoader`: any callback
  that turns a path string into bytes. Use it for Steam Workshop
  downloads, encrypted archives, or in-memory bundles.
- **Procedural sounds**: a sound entry can omit the `file` field. The
  bank assumes the audio data was registered separately under the
  hashed id (`runtime.RegisterPcmSound(HashSoundName(name), ...)`),
  and only registers the SoundDefinition. Useful for synthesized
  audio, debug tones, TTS, and testing.

### Runtime cost

| Operation                                | Cost                       |
|------------------------------------------|----------------------------|
| Load 1000-entry bank from JSON           | ~0.6 ms                    |
| `Find(name)` hash lookup                 | ~54 ns/op (~18.5 M ops/s)  |
| `Find()` on a group with policy          | + 1 relaxed atomic op      |

`Find()` allocates nothing and is safe to call from any thread that
isn't concurrently loading. There's no per-tick or per-play cost
from having a bank loaded.

### Schema, errors, and patterns

Full reference, error message catalog, and worked examples
(footsteps with random_no_repeat, weapon shots with custom
attenuation, music with looping + non-spatialized, UI sounds, asset
packs with custom file loaders, procedural debug sounds) in
[`docs/asset_pipeline.md`](docs/asset_pipeline.md). End-to-end
runnable example under `examples/sound_bank/`.

---

## Music transitions and playback speed

Two pieces fit naturally together: smooth crossfades between tracks,
and runtime control of playback rate. Both became "yes, basic only"
items in earlier audits; this section is what got built when that
turned into "we need this properly."

### Equal-power crossfade

Linear fades dip ~3 dB at the midpoint of a crossfade — audible as a
"hole" in the music. The engine uses equal-power curves instead:
fade-out follows `cos(t·π/2)`, fade-in follows `sin(t·π/2)`, and
`cos² + sin² = 1` so the summed power stays constant for
uncorrelated material. Measured on the test rig: total RMS held
within **±0.3% of baseline** through a 300 ms A→B transition between
two sine tones (200 Hz and 600 Hz).

### `MusicChannel` helper for the canonical pattern

```cpp
audio::MusicChannel music(runtime);

// Start a track. fadeMs == 0 cuts; > 0 fades in over that duration.
music.Play(combatTrackId, /*fadeMs=*/0.0f);

// ... later ...

// Crossfade to a new track over 1.5 seconds.
// MusicChannel handles fade-out on the old track + fade-in on the
// new one with coordinated equal-power curves.
music.Play(menuTrackId, 1500.0f);

// Fade out and leave the channel silent.
music.Stop(2000.0f);

// Tweak the live track's gain (e.g. duck during dialogue):
runtime.SetEmitterParameter(music.Current(),
                              audio::AudioParameterIds::Gain,
                              0.4f, /*smoothingMs=*/200.0f);
```

`MusicChannel` configures the new emitter as `isLooping=true`,
`isSpatialized=false`, `occlusionEnabled=false` — the music
conventions. The previous track is held alive across one Play() call
so its fade-out runs to completion before its mix slot is reclaimed.

### Per-emitter primitives

If you don't want the helper, the underlying knobs are public and
work on any emitter (sound effects, dialogue, ambience):

```cpp
// Fade-in on creation:
audio::EmitterDescriptor desc;
desc.soundId   = mySoundId;
desc.isLooping = true;
desc.fadeInMs  = 1000.0f;          // 1-second fade-in
auto h = runtime.CreateEmitter(desc).value();

// Fade-out on destruction:
runtime.DestroyEmitter(h, /*fadeOutMs=*/500.0f);
```

The same equal-power curves apply, so any pair of fade-out + fade-in
timed to overlap produces a constant-power crossfade.

### Playback speed (pitch + tempo coupled, tape-style)

```cpp
// 0.5 = half speed (one octave down), 2.0 = double speed (one octave up).
// Smoothed over 50 ms by default; abrupt changes don't click.
runtime.SetEmitterPlaybackSpeed(emitter, /*speed=*/1.5f);

// Custom smoothing:
runtime.SetEmitterPlaybackSpeed(emitter, 2.0f, /*smoothingMs=*/200.0f);
```

This is a thin alias over
`SetEmitterParameter(handle, AudioParameterIds::Pitch, speed,
smoothingMs)` — the implementation has been there all along; the
named entry point makes the intent obvious. Verified at 1.5× speed
producing exactly 1.5× the fundamental frequency (zero-crossing
ratio 1.500 measured) and 2.0× speed doubling fundamental (ratio
1.991 measured).

**Note: pitch and time are coupled.** Doubling the speed doubles the
pitch (it's tape-style playback). True pitch-independent
time-stretching (WSOLA / phase vocoder) is not currently supported —
that's a much larger undertaking and rarely needed for game audio.
File a request if you need it for a specific use case.

### Runtime cost

| Operation                                  | Cost                              |
|--------------------------------------------|-----------------------------------|
| Per-frame fade gain (one trig call)        | ~10 ns extra per active fade voice|
| `MusicChannel::Play(crossfade)`            | One CreateEmitter + one Destroy   |
| `SetEmitterPlaybackSpeed`                  | One smoother SetTarget            |
| Steady-state (no fade in progress)         | Zero (fade branch is one compare) |

The fade branch in the mixer is a single `if (fadeTotalFrames == 0)`
short-circuit when no fade is active, so non-fading voices pay
nothing. Active fades cost one `cos`/`sin` call per frame per voice
— negligible compared to the rest of the per-voice mix work.

End-to-end runnable example under `examples/music_crossfade/`.

---

## Online multiplayer

The engine is shaped for indie multiplayer. Every layer that touches
the network — replicated emitter state, voice chat, predictive
playback, late-event handling — has both API support and a
documented integration pattern.

### Adaptive jitter buffer (voice chat that survives real connections)

LAN voice is easy. Voice chat that holds up on residential connections
is the differentiator. The engine ships an adaptive jitter buffer
between Opus packet ingest and decode that:

- Tracks observed inter-arrival jitter via the RFC 3550 EMA (1/16
  weighting), reported per-source via `observedJitterMs`.
- Adapts target buffer depth toward `2 × jitterFrames` clamped to a
  configurable `[minTargetDepth, maxTargetDepth]` band (default 3-10
  frames, ~60-200 ms at 20 ms Opus frames).
- Drops the codec into PLC mode (`IVoiceCodec::DecodeLost`, backed by
  `opus_decode(decoder, NULL, 0, ...)` for the Opus codec) when a
  packet's slot is missed but later packets are present, so a
  single-frame loss is filled with extrapolated audio rather than
  silence.
- Survives 16-bit sequence wraparound (validated against 70k packets
  starting near `seq=60000`).
- De-duplicates retransmits, drops late arrivals against the consumer
  cursor, and reports overwrite/reorder/duplicate counts as cumulative
  telemetry.

Headline numbers from `tests/unit/jitter_buffer_test.cpp` (run on
commodity x86_64; YMMV but the shape is consistent):

| Scenario                              | Continuity¹ | Observed jitter | Target depth |
|---------------------------------------|-------------|-----------------|--------------|
| Clean LAN (0% loss, 1 ms jitter)      | 100.0%      | 0 ms            | 3 frames     |
| Residential (5% loss, 30 ms jitter)   | 100.0%      | 21 ms           | 4 frames     |
| Rough internet (10% loss, 50 ms jitter) | **97.8%** | 29 ms           | 4 frames     |
| Burst loss (200 ms outage)            | 100.0%²     | 0 ms            | 3 frames     |
| Duplicates (every 5th retransmitted)  | 100.0%      | 0 ms            | 3 frames     |

¹ `(packets decoded + PLC frames) / total expected frames`. PLC counts
because it produces continuous audio even though no real packet
arrived.
² Burst recovers cleanly after the outage; the 10 missed frames during
the outage are PLC-filled, not dropped.

Throughput: **~65 million push+pop operations per second** in the
hot-path benchmark on the same hardware. At 50 Hz tick × 16 active
voices that's 800 ops/sec required, so the buffer has roughly five
orders of magnitude of headroom over the production load.

### Runtime cost

The voice path is allocation-free in steady state. Specifically:

- `JitterBuffer::Push` and `JitterBuffer::PopNext` are both `noexcept`
  and allocate nothing. Slot storage is pre-sized to
  `capacityDepth × maxBytesPerPacket` bytes at construction; packet
  data is `memcpy`'d into and out of those slots.
- `VoiceSourceManager::DecodeAndPush` uses `thread_local` scratch
  buffers sized once on first use. Subsequent calls reuse the same
  storage.
- Per-pop work is bounded by `O(capacityDepth)` atomic loads
  (typically 8-16) when the consumer cursor misses, plus one
  `memcpy` and one atomic-store on the hit path. No locks, no
  syscalls, no allocations, no exceptions.
- The decode path delegates to your `IVoiceCodec`; `OpusVoiceCodec`'s
  `Decode` and `DecodeLost` go straight into libopus with no engine-
  side allocations.

### Three deeper integration guides under `docs/`:

- **[`docs/multiplayer.md`](docs/multiplayer.md)** —
  how to wire the engine into Steam GameNetworkingSockets, ENet, or
  raw UDP. Channel design, packet formats, sender/receiver code,
  and cross-pattern recommendations for tick advancement, late-event
  staleness, interest management, and per-player voice telemetry.
- **[`docs/predictive_playback.md`](docs/predictive_playback.md)** —
  when to predict client-side audio, when to wait for server
  confirmation, and how to recover from misprediction without the
  audio sounding broken. Predict-then-reconcile pattern, fade-out
  tuning per sound type (5-10 ms hit-confirm to 100-150 ms
  continuous), recovery patterns (action substitution, reload
  mid-fire, multi-shot bursts), telemetry-driven debugging.
- **[`docs/determinism.md`](docs/determinism.md)** — what the engine
  guarantees about reproducibility, what it doesn't, and the
  tractable path to bit-identical replay. Honest audit: the math
  is deterministic, three wall-clock-driven sources break full
  bit-identity (each documented with location + estimated effort
  to fix).

### Per-player voice telemetry API

```cpp
audio::AudioRuntime::VoiceNetworkStats stats;
if (runtime.GetVoiceNetworkStats(remotePlayerId, stats)) {
    // Show "voice signal weak" UI when jitter > 80 ms
    // or PLC fires more than 10% of the time.
    if (stats.observedJitterMs > 80 ||
        stats.plcGenerated > stats.packetsAccepted / 10) {
        ui.ShowVoiceQualityWarning(remotePlayerId);
    }
}
```

The full counter set: `packetsReceived`, `packetsAccepted`,
`packetsLate`, `packetsDuplicate`, `packetsReordered`, `packetsLost`,
`packetsOverwritten`, `plcGenerated`, `silentFrames`,
`observedJitterMs`, `targetBufferDepthFrames`. All are cumulative
since the source was registered; the read is a single struct copy
under no lock — cheap enough to poll every frame for in-game UI.

---

## Reference

Everything below is for engineers integrating, modifying, or extending
the engine. The earlier sections cover what you need to ship a game on
top of it; what follows documents the design choices, per-subsystem
internals, and test methodology.

## Design intent

The engine **consumes** game and network data; it does not own any of it. The
host (the engine that drives the game tick and the network stack) calls into
the runtime through a small set of seam methods. The runtime owns the audio
clock, the mixer, and the render thread, and is the only writer of audio
state once it is initialized.

Four polymorphism seams, and only four:

- `IAudioBackend`; drives the render thread. Two implementations ship:
  `NullAudioBackend` (default; wall-clock-paced, silent) and
  `MiniaudioBackend` (opt-in via CMake; cross-platform device output backed by
  [miniaudio](https://github.com/mackron/miniaudio)). Native WASAPI/CoreAudio/
  ALSA bypassing miniaudio is follow-up work.
- `IVoiceCodec`; encode and decode in one interface. Two
  implementations ship: `StubVoiceCodec` (int16 PCM passthrough) and
  `OpusVoiceCodec` (libopus, opt-in via `AUDIO_ENGINE_VOICE_OPUS`).
- `IAudioGeometryQuery`; host-supplied raycast for occlusion
  (`NullGeometryQuery` always reports clear line of sight).
- `ISpatializer`; pluggable spatial math (`DefaultSpatializer` does
  distance attenuation + equal-power pan).

`AudioRuntime`, `AudioOrchestrator`, and `AudioAssetRegistry` are concrete.
The TDD's `IAudioEngine` / `IAudioOrchestrator` / `IAudioAssetRegistry` were
treated as illustrative API boundaries, not runtime polymorphism seams.

### Threading model

Four roles, encoded in the type system via Clang Thread Safety Analysis
capability tags (`thread_annotations.h`). On Clang, mixing them is a
compile-time error; on GCC the annotations decay to no-ops but the public
header still documents intent.

| Thread   | Owns                                                                          |
|----------|-------------------------------------------------------------------------------|
| Game     | Calls `Update()`, `CreateEmitter()`, `SetEmitterTransform()`, `SubmitEvent()` |
| Network  | Calls `OnTickAdvanced()`, `SubmitReplicatedEvent()`, `OnVoicePacket()`        |
| Control  | Owned by the runtime; drains rings, runs orchestrator, posts mixer commands  |
| Render   | Owned by the backend; pulls mixer commands, mixes into the device buffer     |

The render thread allocates nothing, takes no locks, makes no syscalls, and
throws no exceptions. All control→render communication goes through the
SPSC mixer command ring (`mixer/mixer_command.h`).

### Memory model

Pre-sized once at `Initialize()`, never resized after. Runtime config
(`AudioConfig`), not `constexpr`. Sizes that matter:

- `maxActiveEmitters` → `EmitterManager` slot map and parallel SoA arrays.
- `maxActiveVoiceSources` → `VoiceSourceManager` slot map.
- `maxMixerVoices` → `AudioMixer` voice array.
- `mixerCommandQueueDepth` → control→render SPSC ring.
- `voicePcmRingFrames` → per-voice-source PCM ring.

All handles are slot-map index plus generation counter (`handles.h`), so a
freed slot reused by a different emitter is detected and rejected.

### Tick→audio interpolation

The host calls `OnTickAdvanced(tickIndex, hostTimestampNs)` from the network
thread when a replicated tick lands. `UpdateReplicatedTransform()` records
the new transform with the same tick index. The audio thread then
interpolates linearly between the last two replicated transforms with a
one-tick lag, and uses velocity-based extrapolation for the trailing edge.
Predicted local events play unconditionally; reconciliation is not
currently performed.

### State vs. event boundary

Two emitter handles by default. A vehicle ignition is an event; the engine
loop is a state-driven emitter. The host decides where the line is; the
runtime never re-fires state-based emitters as events.

---

## Layout

```
audio_engine/
├── CMakeLists.txt
├── include/audio_engine/        # public headers
│   ├── audio_runtime.h          # the orchestrator class
│   ├── audio_file_format.h      # AudioFileFormat enum (Auto/Wav/Ogg/Flac)
│   ├── backend.h                # IAudioBackend seam
│   ├── voice_codec.h            # IVoiceCodec seam
│   ├── spatializer.h            # ISpatializer seam
│   ├── geometry_query.h         # IAudioGeometryQuery seam
│   ├── handles.h                # generation-counted handles
│   ├── result.h                 # Result<T> + AudioResult enum
│   ├── thread_annotations.h     # Clang TSA capability tags
│   ├── types.h                  # Vec3, IDs, enums
│   ├── config.h                 # AudioConfig, AudioRuntimeBudget
│   ├── listener.h, emitter.h, attenuation.h, events.h, export.h
├── src/audio_engine/            # internal implementation
│   ├── runtime/                 # AudioRuntime + AudioRuntimeImpl
│   ├── emitters/                # slot map + SoA mirror + tick interpolation
│   ├── listeners/               # single-listener, multi-listener-ready
│   ├── voice/                   # voice sources, jitter buffer, stub codec
│   ├── spatial/                 # default spatializer, occlusion budgeter
│   ├── mixer/                   # SPSC-driven render-thread mixer + bus graph
│   ├── dsp/                     # gain, biquad LPF/HPF/BPF, sidechain compressor
│   ├── decoders/                # IAudioDecoder + dr_wav / stb_vorbis / dr_flac wrappers,
│   │                            #   resampler, format dispatcher, MemoryPcmDecoder
│   ├── backend/                 # NullAudioBackend, MiniaudioBackend
│   ├── assets/                  # AudioAssetRegistry: PcmAsset (pinned) + StreamingAsset
│   ├── orchestrator/            # parameter smoothing + sequence playback
│   └── util/                    # SpscRing, SlotMap, PcmRing (i16), PcmRingF32
├── examples/
│   ├── minimal/main.cpp         # silent end-to-end smoke test
│   ├── ducking/main.cpp         # bus graph + sidechain compressor (offline)
│   ├── streaming/main.cpp       # streaming voice path, 32k→48k resampler (offline)
│   └── playback/main.cpp        # audible spatial demo (miniaudio)
├── tests/unit/                  # SpscRing, SlotMap, BusGraph, Compressor, Decoder
├── third_party/
│   ├── miniaudio/               # vendoring slot for miniaudio.h
│   ├── dr_libs/                 # vendoring slot for dr_wav.h, dr_flac.h
│   └── stb/                     # vendoring slot for stb_vorbis.c
└── scripts/                     # fetch_miniaudio.{sh,bat}, fetch_decoders.{sh,bat}
```

---

## Build

### CMake

```bash
cmake -S . -B build
cmake --build build -j
./build/examples/minimal/audio_minimal
```

The library is static by default. Pass `-DAUDIO_ENGINE_SHARED=ON` to build
a shared library; the export macro is hand-written in `export.h`.

C++20, CMake 3.20+. No third-party dependencies.

### Raw g++ (no CMake)

The library has no third-party dependencies, so it builds with a single
g++ invocation:

```bash
mkdir -p build && cd build
g++ -std=c++20 -Wall -Wextra -Wpedantic -O2 \
    -I../include -I../src \
    -c $(find ../src -name "*.cpp")
ar -rc libaudio_engine.a *.o

g++ -std=c++20 -O2 \
    -I../include -I../src \
    ../examples/minimal/main.cpp \
    libaudio_engine.a -lpthread -o audio_minimal
./audio_minimal
```

Expected output: a stats dump after one simulated second of audio, with
zero render underruns.

---

## Hearing it work (miniaudio backend)

The default build is silent on purpose; `NullAudioBackend` is great for CI
and headless servers but never opens a device. To hear actual sound, opt
into the miniaudio backend:

```bash
# 1. Get miniaudio.h (one-time setup; pick either path)
./scripts/fetch_miniaudio.sh           # downloads to third_party/miniaudio/
# ...or skip this; CMake can FetchContent it for you on first configure.

# 2. Configure with the backend enabled
cmake -S . -B build -DAUDIO_ENGINE_BACKEND_MINIAUDIO=ON
cmake --build build -j

# 3. Run the playback example
./build/examples/playback/audio_engine_playback
```

You should hear three 1-second 440 Hz tones over five seconds, panning from
left → center → right as their world positions change relative to the
listener at the origin. Console output prints which backend miniaudio
negotiated (e.g. `WASAPI / Speakers`, `coreaudio / MacBook Pro Speakers`,
`alsa / default`).

The miniaudio backend is **opt-in**; the core library has no third-party
dependencies and the default build configuration ignores `third_party/`
entirely. miniaudio is dual-licensed public domain / MIT-0; see
`third_party/miniaudio/README.md` for vendoring details.

### Raw g++ with miniaudio

```bash
./scripts/fetch_miniaudio.sh
mkdir -p build && cd build
g++ -std=c++20 -O2 \
    -I../include -I../src -I../third_party/miniaudio \
    -c $(find ../src -name "*.cpp")
ar -rc libaudio_engine.a *.o

# Linux: -ldl -lm; macOS: link CoreAudio frameworks; Windows: nothing extra
g++ -std=c++20 -O2 \
    -I../include -I../src \
    ../examples/playback/main.cpp \
    libaudio_engine.a -lpthread -ldl -lm -o audio_playback
./audio_playback
```

---

## Bus graph and sidechain ducking

The mixer routes voices through a configurable bus graph instead of summing
straight to the device. A bus is a named mix point with an input buffer, an
output buffer, an ordered chain of DSP effects, an output gain, and a parent
bus. Voices select their target bus via `EmitterDescriptor::targetBus` (or
fall back to a category-based map). On each render callback the mixer:

1. Drains its command ring (start/stop voices, voice param updates, **bus
   gain updates, effect parameter updates**).
2. Clears every bus's input buffer.
3. Mixes every active voice into the input buffer of its target bus.
4. Walks the buses in topological order; sidechain sources first; copying
   each bus's input to its output, running its effect chain (which may read
   another bus's output buffer as a sidechain signal), and summing the
   gained output into its parent's input. Buses marked `silent=true` are
   processed but their output is *not* summed into the parent; useful for
   sidechain-only sources like the TDD's `RemoteGunNearby`.
5. Copies the master bus's output to the device buffer.

The render path stays allocation-free, lock-free, and exception-free. All
buffers and effect instances are allocated up-front during `BusGraph::Build`,
which validates the configuration (no duplicate IDs, no parent cycles,
sidechain references resolve to known buses) and computes the render order.

### Effects available out of the box

- **Gain**; internal 5 ms linear ramp; param-update is smoothed.
- **Biquad**; RBJ cookbook LPF / HPF / BPF, direct-form II transposed,
  per-channel state.
- **Compressor**; peak detector + one-pole envelope follower, hard-knee
  static gain computer, optional sidechain reference to another bus's
  *output* buffer. This is the building block for the TDD's three-tier
  ducking.

Adding a new effect is a matter of implementing `IDspEffect`
(`Prepare/Process/OnParameter/SidechainBusId`) and exposing a config tag in
`EffectConfig`.

### Proximity-aware sends (the `RemoteGunNearby` pattern)

The TDD's "decoupled audible vs. trigger" pattern; where a remote weapon
shot has both an audible 3D voice on `RemoteGun` *and* a silent 2D voice on
`RemoteGunNearby` whose volume is driven by listener distance; is built
into the runtime. Set `silent=true` and a `proximityCurve` on the trigger
bus, route the emitter to it, and the control thread (in step 9 of
`Update()`) computes distance-to-listener, evaluates the curve, and folds
that into the voice's gain before the next mixer command goes out. No
network cost; the curve is a host-tunable parameter set.

### Three-tier ducking

The TDD's three-tier ducking model (local gun deepest, nearby remote gun
medium, explosion deepest+longest with concussion LPF) is expressible
directly:

- Three buses for the trigger sources (`LocalGun`, `RemoteGunNearby`,
  `Explosion`); the first audible, the second silent with a proximity
  curve, the third audible.
- Each ducked bus (`Music`, `Voice`, `SFX`) carries three Compressor
  effects with the appropriate sidechain references and tuning from §7 of
  the TDD.
- The `Voice` bus additionally carries a Biquad LPF whose cutoff is
  modulated from the host via `SetEffectParameter` for the concussion
  sweep.

`examples/ducking/main.cpp` demonstrates the simplest version of this
end-to-end: a music loop on `Music`, a noise-burst gunshot on a silent
`LocalGun`, a sidechain compressor on `Music`. The example uses a
synchronous offline backend so the duck is measurable as RMS / dB columns:
baseline ≈ −17 dB, deepest duck ≈ −32 dB while the gunshot's energy is
above threshold, smooth recovery over the configured 250 ms release.

---

## Loading sounds: decoded files and streaming

The engine takes audio from three places. They sit at different points on
the latency / memory tradeoff and route through the same emitter and
mixer paths, so the host code that *plays* a sound doesn't care which
mode produced it.

### `RegisterPcmSound`; for sounds the host already has as PCM

Pass an interleaved float buffer at the engine's sample rate and channel
count; the registry copies it and pins it. This is the path for
synthesized SFX, host-decoded content, and tests.

### `RegisterSoundFromFile` / `RegisterSoundFromMemory`; short SFX

```cpp
runtime.RegisterSoundFromFile(soundId, "assets/footstep.wav");
runtime.RegisterSoundFromMemory(soundId, bytes);   // format auto-detected
```

The file is opened, decoded fully, resampled to the engine's rate (linear
interp) if it isn't already, downmixed to stereo if it has more than two
channels, and stored as a pinned `PcmAsset`; same lifetime as
`RegisterPcmSound`. Format is dispatched by extension first
(`.wav` / `.ogg` / `.oga` / `.flac`), with magic-byte sniffing as a
fallback. Multi-channel WAV/FLAC drops the surrounds (stereo: take ch0/1;
mono: equal-weight average).

These calls block the game thread during decode, so the rule is:
*sub-second SFX through this path; anything longer goes through streaming
or behind a loading screen.*

### `RegisterStreamingSoundFromFile` / `RegisterStreamingSoundFromMemory`; long material

```cpp
runtime.RegisterStreamingSoundFromFile(soundId, "assets/music_loop.ogg");
```

The decoder is opened once and held for the registry's lifetime. Each
streaming asset owns a per-asset float SPSC ring (`PcmRingF32`,
default 24 000 frames ≈ 500 ms at 48 kHz, configurable via
`config.streamingRingFrames`). When a voice plays the asset, a
control-thread pump in `Update()` tops up the ring in chunks of
`config.streamingDecodeChunkFrames` (default 2048); the render thread
drains the ring through a dedicated `VoiceMode::StreamingSound` path in
the mixer. No new threads, no allocations on the render thread.

Streaming is the right choice for music, dialogue, and ambience; any
asset where pinning the full decoded buffer would burn memory you don't
want to spend.

The memory variant is useful for procedurally-generated music and tests
that want to exercise the streaming pipeline without depending on a
file decoder:

```cpp
std::vector<float> pcm = synthesize_track(...);
runtime.RegisterStreamingSoundFromMemory(soundId, std::move(pcm),
                                          /*sampleRate*/ 32000,
                                          /*channels*/   1);
```

`examples/streaming/main.cpp` runs this exact path: it builds a 10 s mono
buffer at 32 kHz, registers it as a streaming sound, plays it through
the mixer at the engine's 48 kHz, and prints RMS per tick; the
resampler and the pump are exercised together.

### Build flags

The decoders are conditionally compiled and off by default to keep the
core library dependency-free:

| Option                          | What it pulls in                       | Default |
|---------------------------------|----------------------------------------|---------|
| `AUDIO_ENGINE_DECODERS_WAV`     | `dr_wav.h` (single-header public domain) | OFF     |
| `AUDIO_ENGINE_DECODERS_OGG`     | `stb_vorbis.c` (single-file public domain) | OFF     |
| `AUDIO_ENGINE_DECODERS_FLAC`    | `dr_flac.h` (single-header public domain) | OFF     |
| `AUDIO_ENGINE_FETCH_DECODERS`   | FetchContent fallback for the above   | ON      |

When a decoder is disabled, its TU is empty and its factory returns
`AudioResult::Unsupported`; calls into the registry surface that result
through `RegisterSoundFromFile` and friends.

Headers are searched first under `third_party/dr_libs/` and
`third_party/stb/`; if missing and `AUDIO_ENGINE_FETCH_DECODERS=ON`,
CMake fetches them from `mackron/dr_libs` and `nothings/stb`. Pin a
specific revision with `-DAE_DRLIBS_TAG=<ref>` /
`-DAE_STB_TAG=<ref>`. The standalone helper script
`scripts/fetch_decoders.{sh,bat}` does the same vendoring step without
running CMake.

---

## Voice cap and priority eviction

Game audio is by nature contested: rocket explosions, eight grenades,
ambient bugs, bullet whips, footsteps. The fixed emitter pool will be
saturated repeatedly. Instead of refusing new sounds when the pool is
full, the engine **evicts the cheapest playing one-shot** if the
incoming sound is more important.

### Effective priority

For each playing one-shot the engine computes:

```
effPri = (priority << 32) - distance_to_listener_mm
```

Priority dominates; a `Critical` sound always beats a `Low` sound.
Within a priority tier, **closer-to-listener wins**, breaking the tie
in the direction players actually care about (a footstep five feet
behind you matters more than the same priority footstep fifty metres
across the map).

### What can and can't be evicted

Persistent emitters created via `CreateEmitter` are **immune**;
they're host-owned and the engine never reclaims them. Only one-shots
spawned by `PlaySoundAtLocation` events are eligible victims. This
keeps the host's mental model simple: handles you hold are stable;
fire-and-forget events compete for the remaining slots.

Voice-chat (`VoiceMode::Voice`) and streaming sounds use separate slot
ranges and aren't part of the one-shot eviction pool.

### Observability

`AudioRuntime::Stats` exposes:

| Field                       | Meaning                                                              |
|-----------------------------|----------------------------------------------------------------------|
| `oneShotEvictions`          | Times the priority system stole a slot from a lower-priority sound   |
| `oneShotsDroppedFullPool`   | Times a one-shot was dropped because no eviction candidate beat it   |

A high `oneShotEvictions` rate is the priority system doing its job
under contention. A high `oneShotsDroppedFullPool` rate means the pool
is sized too small for the gameplay scenario; bump
`config.budget.maxActiveEmitters` until that counter drops.

---

## Occlusion damping (per-voice low-pass filter)

A sound on the other side of a wall doesn't just get quieter; it gets
*muffled*. The engine's `OcclusionSystem` already produces a per-source
occlusion amount in `[0, 1]` from the host's `IAudioGeometryQuery`
raycasts. The spatializer translates that into two outputs:

```
gain          *= lerp(1.0,  0.35, occlusion)   //  up to 65 % gain reduction
lowPassAmount  = lerp(0.0,  0.7,  occlusion)   //  up to 70 % filter damping
```

`lowPassAmount` rides the `UpdateParams` mixer command alongside
`gain` / `pan` / `pitch`. The mixer maintains a per-voice biquad LPF
(cookbook coefficients, Q = 0.707) and applies it inline in each of the
three voice mix paths (`Sound`, `StreamingSound`, `Voice`). When
`lowPassAmount < 0.001` the filter is bypassed at zero cost.

### Cutoff curve

The amount is mapped to filter cutoff via an exponential curve so the
muffle ramps in fast; most of the perceptual difference happens in the
first 0.5 of the amount range:

| amount | cutoff   | what it sounds like              |
|--------|----------|----------------------------------|
| 0.0    | 22 kHz   | bypass                            |
| 0.5    |  3.3 kHz | door closed between you and source |
| 0.7    |  1.7 kHz | wall between you and source       |
| 1.0    |  0.5 kHz | submerged / heavy obstruction     |

The spatializer's 0.7 cap means default occlusion lands at the
"thick wall" point. Future systems (environmental zones, water, design
intent) can drive the amount the rest of the way to 1.0.

### State preservation

Filter coefficients are recomputed only when `lowPassAmount` changes by
more than 0.001 between updates; per-channel state (`z1`, `z2`) is
preserved across coefficient swaps so smooth occlusion changes don't
click. Voice-start (`StartSound` / `StartStreamingSound` / `StartVoice`)
zeros the state to give a clean envelope from silence.

### Test coverage

`tests/unit/occlusion_lpf_test.cpp` runs four checks:

- **Bypass at amount = 0**; 10 kHz sine through the mixer at peak 0.707
  (equal-power center pan).
- **Heavy damping at amount = 0.7**; same 10 kHz sine attenuated to peak
  ≈ 0.0125 (≈ 35 dB cut at 10 kHz with cutoff at 1.7 kHz).
- **Low frequencies pass through**; 200 Hz sine at amount = 0.7 still
  peaks at 0.707 (well below cutoff).
- **End-to-end through runtime**; registering a 10 kHz sine, attaching
  it to an emitter, and swapping in an `IAudioGeometryQuery` that always
  reports occluded drops measured peak from 0.500 to 0.004 (≈ 42 dB cut).

---

## Doppler pitch shift and per-voice pitch smoothing

A sound source moving toward the listener should rise in pitch; one
moving away should drop. The `DefaultSpatializer` computes a pitch ratio
each tick from the radial component of relative velocity:

```
pitch = c / (c + radial_velocity)         // c = speed of sound, default 343 m/s
```

clamped to `[0.5, 2.0]` so a single bad velocity sample can't cause an
extreme resampling step. `SpatialEnvironmentState.dopplerEnabled` is on
by default; flip it off to bypass the calculation entirely.

### Per-voice pitch smoothing

Pitch arrives at the mixer through `UpdateParams` once per control-thread
tick. If the runtime sets pitch to 1.0 in one tick and 1.2 in the next,
applying that step abruptly at the buffer boundary would cause a click;
the resampling cursor would jump suddenly, the waveform would skip a
fractional sample, and you'd hear it. Instead the mixer keeps two
fields per voice:

| Field          | Meaning                                                  |
|----------------|----------------------------------------------------------|
| `pitch`        | Latest target value posted via `UpdateParams`            |
| `pitchCurrent` | Smoothed running value the cursor actually advances by   |

Each render call linearly ramps `pitchCurrent` from its old value to
`pitch` across the buffer (5.3 ms at the default 48 kHz / 256 frames),
committing `pitchCurrent = pitch` at the end. Steady-state voices pay
one extra add per frame compared to the constant-pitch case; the
difference is invisible in real workloads and the click is gone.

Voice start (`StartSound` etc.) initialises `pitchCurrent = pitch` so
the source plays at its target rate from the first sample; no
fade-in artefact when slot indices are recycled.

### Sample interpolation

At pitch ≠ 1 the cursor lands between integer source frames. The mix
loop reads the two adjacent frames and linearly interpolates between
them rather than taking the nearest sample. Nearest-neighbour at
non-integer step sounds gritty (an audible high-frequency staircase
aliases into the result); linear interp is essentially exact for any
content well below Nyquist and costs one extra mul + add per channel
per frame. Looping wraps `idx + 1` to source frame 0 so there's no
discontinuity at the loop point.

`AudioConfig` exposes:

| Field            | Default | Meaning                                         |
|------------------|---------|-------------------------------------------------|
| `enableDoppler`  | `true`  | Pass the Doppler ratio through; off ⇒ pitch ≡ 1 |
| `speedOfSound`   | `343.0` | m/s; lower ⇒ stronger Doppler. Water ≈ 1480.    |

### Test coverage

`tests/unit/doppler_smoothing_test.cpp` runs the system at three layers:

- **Mixer-level**: a "ramp source" buffer where `pcm[i] = i × 0.001` is
  fed through the Sound voice. Because the source is a perfect linear
  ramp, the mixer's output samples directly reveal the resampling
  step: `output[i+1] − output[i] ≈ 0.001 × pitch_at_frame_i`. Three
  sub-tests assert (a) starting at non-unity pitch produces no fade-in
  from 1.0, (b) when pitch jumps mid-stream the first sample of the
  new block is continuous with the previous block's slope (smoothing)
  and the final sample reaches the new target (convergence within one
  block), (c) holding the target keeps the slope locked.
- **Spatializer-level**: feeds the `DefaultSpatializer` known
  positions and velocities and verifies the resulting pitch ratios;
  static = 1.0, receding 30 m/s = 0.9196, approaching 30 m/s = 1.0958,
  disabled = 1.0, far-supersonic clamps to 2.0, perpendicular motion = 1.0.
- **Resampler quality**: a 1 kHz sine played at pitch = 1.5 reconstructs
  a 1.5 kHz sine within 0.0011 RMS of an analytical reference; proves
  the Sound mix path runs linear interpolation between source frames,
  not nearest-neighbour (which would alias audibly at non-integer pitch
  ratios).
- **Runtime end-to-end**: an emitter approaching the listener at 50 m/s
  produces ≈ 1170 Hz output from a 1000 Hz source (measured 1168 Hz);
  receding produces ≈ 873 Hz (measured 888 Hz). Proves the chain
  emitter velocity → spatializer → `UpdateParams` → mixer pitch ramp
  → cursor step → resampled audio composes correctly through the full
  `AudioRuntime.Update` cycle.

---

## Air absorption (distance-driven high-frequency rolloff)

A sound 200 m away in real air doesn't just arrive quieter; its highs
arrive *more* attenuated than its lows. The `DefaultSpatializer`
contributes an air-absorption amount to the same per-voice LPF the
occlusion path drives, scaling linearly with distance to the listener:

```
airLpfAmount = clamp(distance * AudioConfig.airAbsorptionPerMeter, 0, 1)
out.lowPassAmount = max(occlusionLpfAmount, airLpfAmount)
```

Combining with `max()` rather than addition keeps the LPF inside
sensible bounds; both occlusion and distance damp the *same* biquad,
and cascading their amounts would over-attenuate when both are active.
"The dominant cause of muffle wins" is a workable approximation that
lines up with how the ear cues source distance.

### Defaults

| Field                                | Default | Meaning                                              |
|--------------------------------------|---------|------------------------------------------------------|
| `AudioConfig.enableAirAbsorption`    | `true`  | Master toggle. `false` ⇒ air contributes nothing.    |
| `AudioConfig.airAbsorptionPerMeter`  | `1/250` | Distance at which air alone saturates the LPF (≈ 250 m). |

The default puts the saturation point at 250 m, which suits open-air
FPS scales. Drop it (e.g. `1/500`) for fog-of-war low-frequency
ambience that should still carry; raise it for tight indoor spaces
where you want close-range sources to already feel a little muffled.

### Test coverage

`tests/unit/air_absorption_test.cpp` runs a 10 kHz sine through the full
runtime at three distances:

| Distance | Air absorption ON | Air absorption OFF |
|----------|-------------------|--------------------|
| 1 m      | peak ≈ 0.998       | peak ≈ 1.000       |
| 100 m    | peak ≈ 0.090       | peak ≈;           |
| 250 m    | peak ≈ 0.001       | peak ≈ 0.500       |

The "ON" column shows monotonic high-frequency attenuation of more than
50 dB by 250 m; well into "you can hear it's there but you can't tell
what it is" territory. The "OFF" column shows the same 10 kHz sine
arriving at 250 m with only the volume-floor distance attenuation
applied; no LPF damping at all.

---



## Reverb sends (per-voice send to a Freeverb-derived reverb bus)

A reverb tail is what tells you a sound just happened in a room rather
than at a microphone. The engine implements reverb as a **send bus**;
voices route a configurable fraction of their signal to a dedicated
bus carrying a reverb effect; the dry path through their normal
target bus is unaffected; both sum at the master.

### Topology

```
voice ──gain,pan,LPF──┬─►  Music / SFX / Voice / ...    ──►  Master
                       │
                       └─►  kBusReverb [ReverbEffect]   ──►  Master
                            (scaled by gain * reverbSend)
```

The send is **post-fader, post-LPF, post-pan**: the reverb hears the
same processed signal the dry mix gets, which keeps the tail
spatially consistent with the dry source and lets occluded sources
naturally produce muffled tails.

### Opting in

Two steps. First, declare the bus in your `BusGraphConfig`:

```cpp
cfg.busGraph.busCount = 2;
cfg.busGraph.buses[0].id = kBusMaster;
cfg.busGraph.buses[1].id = kBusReverb;
cfg.busGraph.buses[1].parent = kBusMaster;
cfg.busGraph.buses[1].effects[0].kind            = EffectKind::Reverb;
cfg.busGraph.buses[1].effects[0].reverbRoomSize  = 0.7f;
cfg.busGraph.buses[1].effects[0].reverbDamping   = 0.5f;
cfg.busGraph.buses[1].effects[0].reverbWetGainDb = 0.0f;
cfg.busGraph.buses[1].effectCount                = 1;
```

Second, raise `AudioConfig.globalReverbSend` above zero (default is
0). This becomes the per-voice send level on every spatialized voice
via `SpatialParams.reverbSend`. With no `kBusReverb` bus configured
or with `globalReverbSend = 0`, the mixer's send code is a true no-op
with no extra cost.

### The reverb effect

`ReverbEffect` is a Freeverb-derived stereo algorithmic reverb (8
parallel comb filters with per-comb damping LPF feeding 4 series
allpass filters per channel; right-channel delay lengths offset by
~23 samples to broaden the stereo image; sample-rate-scaled delay
lengths so it sounds the same at 44.1 / 48 / 96 kHz).

Two perceptual parameters:

| Parameter   | Range  | Effect                                          |
|-------------|--------|-------------------------------------------------|
| `roomSize`  | 0..1   | Comb feedback gain → tail length (bigger = longer) |
| `damping`   | 0..1   | LPF in the feedback path → tail brightness (more = darker) |

Plus a `wetGainDb` post-effect trim for matching with the dry mix.
All three are runtime-modulatable via `AudioRuntime::SetEffectParameter`
using `EffectParameter::Reverb_RoomSize / _Damping / _WetGainDb`.

### Test coverage

`tests/unit/reverb_send_test.cpp` runs five checks at three layers:

- **DSP impulse response**; feed a single-sample impulse, confirm a
  tail exists at 0–50 ms and is still decaying at 400–600 ms.
- **Big room vs small room**; at 400 ms a `roomSize=0.95` reverb has
  ~10× more residual energy than `roomSize=0.30`. Proves the
  feedback parameter is doing what it claims.
- **Runtime end-to-end (with reverb)**; register a 100 ms noise
  click, fire it via `PlaySoundAtLocation`, render through the offline
  backend, measure RMS in the 250–450 ms window (well after the dry
  signal ended). Tail RMS ≈ 0.016; clearly audible reverb.
- **Runtime no-reverb-bus**; same scenario without `kBusReverb` in
  the graph. Tail RMS = 0.000000; the send code is a verified no-op.
- **Runtime zero-send**; reverb bus present but `globalReverbSend=0`.
  Tail RMS = 0.000000; gating on the threshold is correct.

The third assertion required a subtlety: the test originally used a
5 ms click, which is shorter than the 25 ms `Update` tick.
`TickOneShots` decremented the voice's remaining-frame count by
1200 frames per tick (= ~25 ms at 48 kHz), so the 240-frame click
expired in the same tick it was created and `Stop` was posted before
any audio rendered. Bumping the click to 100 ms (longer than one
tick) gives the voice four ticks of audible life while the dry sound
plays, after which the reverb tail decays naturally.

---



## Binaural spatialization (`SphericalHeadSpatializer`, opt-in)

`DefaultSpatializer` produces stereo via equal-power pan; that's the
right default for fast-moving sources where pan is enough and you want
the mix to be predictable. `SphericalHeadSpatializer` is a second
implementation of `ISpatializer`: drop it into
`AudioRuntimeDependencies::spatializer` and the same source positions
produce per-ear directional cues instead of a pan value.

### What it gets you

Two of the three classical HRTF cues, computed from physics rather than
from measured data:

1. **ITD** (interaural time difference) — the dominant horizontal-plane
   localization cue below ~1.5 kHz. Computed from Woodworth's
   spherical-head formula:
   ```
   ITD = (a/c) × (sin θ + θ)        for |θ| ≤ π/2
   ITD = (a/c) × (sin θ + π - θ)    for |θ| > π/2
   ```
   At a 0.0875 m head radius and 343 m/s sound speed, the maximum ITD
   is about 0.65 ms — roughly 31 samples at 48 kHz. The mixer applies
   this as a fractional per-ear delay with linear interpolation, ramped
   per buffer so listener-turn updates don't click.

2. **ILD** (interaural level difference) — the head shadowing the
   contralateral ear. Modelled as a `sin² θ` ramp on the lateral
   component, applied as both a direct gain reduction (up to ~4 dB)
   and an additional contribution to the per-ear LPF amount (up to a
   ~2.5 kHz cutoff at full lateral). The LPF reuses the same biquad
   the occlusion and air-absorption paths drive, with independent
   coefficients per ear.

What it doesn't get you, and why this is fine:

3. **Pinna spectral cues** — the 4-16 kHz notches and peaks that
   resolve elevation and front/back ambiguity. These are
   listener-specific and cannot be modelled from physics; they need
   measured HRIR data (MIT KEMAR, CIPIC, SADIE). The seam to add this
   is `ISpatializer` itself: a future `HrirConvolutionSpatializer`
   would slot in beside `SphericalHeadSpatializer` without any other
   change to the engine.

### Honest limitations

- **Cone of confusion.** ITD + ILD alone produce identical cues for
  a sound 30° in front and the same sound 30° behind. The brain uses
  pinna cues to disambiguate; without them, front/back swaps are real
  and audible. This is the cost of the no-data approach.
- **In-head localization.** Some listeners perceive sounds as inside
  their head rather than externalized in space when only ITD + ILD
  are present. Light reverb (the existing reverb send) helps this
  meaningfully; pinna cues solve it.
- **Stereo source handling.** The binaural path downmixes any stereo
  source to mono before delaying. Music and ambience usually want pan
  semantics; leave them on `DefaultSpatializer` (or set
  `def.spatialized = false` so neither path applies).

### Wiring it in

```cpp
AudioRuntimeDependencies deps;
deps.backend     = std::make_unique<MiniaudioBackend>();
deps.spatializer = std::make_unique<SphericalHeadSpatializer>();
runtime.Initialize(cfg, std::move(deps));
```

`SphericalHeadSpatializer::Settings` exposes `headRadiusMeters`
(default 0.0875 m, the KEMAR-dummy value), `maxShadowLpfAmount`
(default 0.55), and `minShadowGain` (default 0.6). Smaller heads
produce smaller ITDs; tweaking the shadow values trades perceived
externalization against forward-bias sounding source.

### Test coverage

`tests/unit/binaural_spatializer_test.cpp` runs four checks:

- **Woodworth at full lateral** — ITD = 31.48 samples at 48 kHz with
  default settings, within half a sample of the textbook value.
- **L/R symmetry** — sources at +X and -X produce mirror-image params.
- **Center is symmetric** — straight-ahead source has zero delay,
  unity gain, and zero LPF on both ears.
- **End-to-end through the runtime** — a 10 kHz tone with the source
  hard right produces peak L=0.027, peak R=1.000 on the rendered
  output; hard left mirrors it; straight ahead gives equal peaks.
  Verifies that the spatializer's params plumb correctly through the
  mixer's binaural mix path.

### Cost on the render thread

Per voice in binaural mode: one mono downmix, two writes into per-ear
delay rings, two linear-interpolated reads, two biquad filter steps,
two gain multiplies. Compared to non-binaural: one extra LPF and one
extra delay-line read per ear. The 192-sample delay rings are
allocated once per voice at mixer construction; the binaural switch
is per-voice, so non-binaural sources skip the entire path with one
predicate.

---

## FPS production readiness

Four concerns surface when integrating an audio engine into a real
co-op shooter. The engine handles each of them; this section
documents how, with pointers to tests and examples that prove the
behavior.

### 1. Material-aware occlusion

`IAudioGeometryQuery` is host-supplied; the engine doesn't know
your level geometry. What the engine *does* provide is a richer
hit type than a single occlusion float. `AudioOcclusionHit` carries
two independent components:

- `absorption` (0..1) — overall gain reduction (level cue)
- `damping` (0..1) — high-frequency rolloff (HF cue, drives the
  per-voice biquad LPF)

Plus an `AudioMaterial` enum the host can pass instead, with
defaults the engine fills in:

```cpp
enum class AudioMaterial : uint8_t {
    Default, Air, Glass, Wood, Drywall, Concrete, Metal, Curtain, Foliage
};
```

The defaults table (`AudioMaterialDefaults`) gives Concrete
high absorption + high damping, Curtain low absorption + high
damping, Glass low both. So a host can return a single material
enum from its raycast and get a perceptually correct response, or
pass explicit `absorption` and `damping` values for finer control.

`OcclusionSystem` runs `ResolveOcclusion()` on every hit and writes
two arrays (target absorption, target damping) which are smoothed
exponentially over 150 ms before flowing to the spatializer. The
spatializer then applies absorption to gain and damping to the LPF
amount independently. A curtain produces level reduction comparable
to glass but HF rolloff comparable to concrete, exactly as the ear
expects.

`production_readiness_test.cpp::TestMaterialDifferentiation` runs
the three canonical materials through `DefaultSpatializer` and
asserts that gain spread and LPF spread across them both exceed
0.4. Concrete: gain=0.415 lpf=0.525. Curtain: gain=0.805 lpf=0.560.
Glass: gain=0.935 lpf=0.035.

### 2. One-shot timing race (sub-tick sounds)

Audio designers will produce 5-10 ms one-shots (foley clicks,
single-step footsteps). With a 25 ms control tick, the naive
implementation has a race: an emitter is created at tick T, its
`oneShotFramesRemaining` is set to (asset frames at sample rate),
and at the next tick `TickOneShots` decrements past zero and posts
a Stop before the render thread had a chance to play the asset.
Result: the sound is born and killed in the same tick interval
without producing audio.

The fix is a single bool. `EmitterRecord::firstTickPassed` starts
false. `TickOneShots` skips the decrement on the first pass it
sees, sets the flag to true, and only decrements on subsequent
passes. This guarantees a one-shot lives at least one full tick
period regardless of asset duration.

Verified by `production_readiness_test.cpp::TestOneShotGracePeriod`:
a one-shot with `oneShotFramesRemaining = 1` (one sample, sub-millisecond)
survives the first tick, gets destroyed on the second.

### 3. Fade-out on Stop (no eviction clicks)

Without fade, evicting a voice mid-playback produces an audible
click — the speaker cone is held at whatever sample value the
voice was outputting and snaps to zero in one sample. At loud
levels this is a hard pop. `MixerCommand::Stop` now carries a
`fadeOutMs` field; when non-zero, the mixer rampdown the voice's
output over that many samples linearly to zero, then marks the
voice Inactive. Per-buffer applied: `ApplyFadeIfActive(MixVoice&)`
returns false when the fade reaches zero, mid-buffer if necessary.

The eviction site in `audio_runtime.cpp` posts Stop with 20 ms of
fade. Designers can override per-call where needed (a tank-roar
voice ending naturally would want 100 ms of fade; a hit-confirm
beep cutting under a higher-priority replacement might want 5 ms).

A subsequent `StartSound` on the same mix slot preempts the fade —
the new voice gets full level immediately. This is "best effort":
under heavy eviction churn a fade can be cut short, but it never
holds up the new voice.

Verified by `production_readiness_test.cpp::TestFadeOutMonotonicAndSilent`:
across ten 2 ms slices through a 20 ms fade, peak amplitude is
non-increasing in every slice; the post-fade tail is silent
(< 1e-4); the very first slice still lands at full pre-fade level
so the ramp begins from 1.0 rather than from somewhere lower.
`TestImmediateStopWithoutFade` confirms the legacy `fadeOutMs = 0`
path still cuts cleanly.

### 4. Multi-tier sidechain ducking (the L4D2 pattern)

L4D2's mix wasn't just music ducking under gunfire; the player's
own gunfire also reduced the volume of *other* SFX (teammate
gunfire, ambient horde) so the local shot had perceptual presence.
That's two compressors keyed off the same sidechain bus:

```
Master
├── Music     (Compressor, sidechain = LocalSfx)
└── SfxAll
    ├── LocalSfx                                  ← your weapons
    └── RemoteSfx (Compressor, sidechain = LocalSfx)  ← teammate weapons, ambience
```

When you fire, `LocalSfx` accumulates energy. Both compressors
detect it on their sidechain input and engage:
- Music ducks aggressively (-30 dB threshold, 8:1 ratio, 250 ms
  release)
- RemoteSfx ducks moderately (-28 dB threshold, 4:1 ratio, 180 ms
  release)

This composes from primitives the engine already exposes — bus
graph hierarchy, per-bus compressor effect with sidechain bus
reference, no engine code changes required.

`examples/multi_tier_ducking/main.cpp` demonstrates the pattern
end-to-end. Sample run:

```
Phase 2: remote shot fires alone
  +50ms                  rms=0.1586  (-16.00 dB)   ← remote at full level
Phase 4: remote starts, local fires 75 ms later
  +50ms (L+R)            rms=0.0822  (-21.70 dB)   ← remote pushed under
```

5.7 dB of additional duck on the remote shot when it's heard
alongside a local shot. Music dips to -27 dB during the local
attack, then recovers over ~200 ms.

---



## Online multiplayer readiness

Three additions for multiplayer-specific concerns. Each is additive,
opt-in, and does not change the behavior of single-player or
LAN-coop usage.

### Per-event staleness override

`AudioConfig::lateEventDiscardMs` (default 250 ms) is a global
late-event horizon: events older than this when drained are
dropped. That's a sensible single-player default but wrong for
multiplayer where different event categories want different
tolerance. A gunshot from 500 ms ago doesn't help anyone (the
visual moved on); a music transition from 3 seconds ago is still
worth playing.

`AudioEvent::maxStalenessMs` lets the host stamp a per-event
override. When > 0, the engine uses it instead of the global
`lateEventDiscardMs`. Suggested defaults per category are in
`DefaultStalenessMsForCategory()`:

| Category | Suggested max staleness |
|----------|-------------------------|
| SFX      | 200 ms                  |
| Voice    | 1 s                     |
| Music    | 5 s                     |
| Dialogue | 2 s                     |
| Ambience | 1.5 s                   |
| UI       | 0 (never stale)         |

```cpp
AudioEvent ev = AudioEvent::MakePlaySoundAtLocation(kSndShot, pos);
ev.timestampMs    = wallClockMs;
ev.maxStalenessMs = DefaultStalenessMsForCategory(AudioCategory::SFX);
runtime.SubmitEvent(ev);
```

Verified by `multiplayer_readiness_test.cpp::TestStalenessOverride`:
of four events submitted in one tick (fresh, stale-by-global-default,
stale-but-overridden, fresh-but-tight-override), exactly two are
dropped — the second and the fourth — matching the policy.

### `CancelPredictedEvent` for client-side prediction

The engine plays predicted local events immediately at the
predicted time. If the server later rejects the prediction, the
host calls `CancelPredictedEvent(predictionId, fadeOutMs = 50.0f)`
and the engine fades the resulting voice out.

The host stamps a non-zero `predictionId` on the outgoing event:

```cpp
AudioEvent ev = AudioEvent::MakePlaySoundAtLocation(kSndShot, pos);
ev.predictionId = nextPredictionId++;
runtime.SubmitEvent(ev);

// ... server replies that this shot was rejected ...
runtime.CancelPredictedEvent(ev.predictionId);
```

The engine stores the prediction id on the resulting emitter
record. `CancelPredictedEvent` does a linear scan over the
bounded emitter pool, finds the match, and posts a faded Stop
through the same plumbing eviction uses. When no match exists
(prediction already retired, never started, or evicted), the call
returns Success and increments `predictionsCancelledNotFound` —
"nothing to cancel" and "successfully cancelled" are the same
outcome from the host's view.

Verified by `multiplayer_readiness_test.cpp::TestCancelPredictedEvent`:
a sustained sine emitted with `predictionId = 42` reaches steady
peak 0.707, gets cancelled with 50 ms fade, the post-fade tail
peak measures < 1e-3 (silent). Counters: `predictionsCancelled = 1`,
`predictionsCancelledNotFound = 0`. A second cancel for the same
id increments `NotFound` instead. `CancelPredictedEvent(0)`
returns `InvalidArgument`.

### Interest management for replicated emitters

Battle royales and large co-op shooters can have many more
potential audible emitters than make sense to spatialize each
tick — 60 players × footstep loops × ambient loops adds up fast.
`AudioRuntimeBudget::maxActiveEmittersProcessedPerTick` caps the
work:

```cpp
cfg.budget.maxActiveEmitters                  = 128;
cfg.budget.maxActiveEmittersProcessedPerTick  = 32;
```

Each tick the runtime collects active emitters into a pre-sized
scratch vector, partitions by squared distance to the listener
with `std::nth_element`, and runs the spatializer + posts
UpdateParams only for the closest N. Emitters outside the budget
get a single zero-gain UpdateParams the tick they fall out
(edge-triggered via `EmitterRecord::inInterestMute`); the mixer
voice keeps running silently. When the membership changes —
listener moves, an emitter walks closer — the in-set gets fresh
UpdateParams and unmutes, the new out-set gets zero-gained.

Default is 0 = unlimited, which preserves the existing single-
player behavior bit-for-bit.

Stats counters surface what actually happened:

```cpp
const auto stats = runtime.GetStats();
stats.emittersProcessedLastTick;           // closest N this tick
stats.emittersSkippedByInterestLastTick;   // total - N
```

Verified by `multiplayer_readiness_test.cpp::TestInterestManagementBudget`:
six emitters at increasing distances with budget = 3 produce
processed = 3, skipped = 3 each tick. Moving the listener so far
emitters become close and vice versa preserves processed = 3 / skipped
= 3 (membership swap, count unchanged). With budget = 0 (unlimited),
all 6 emitters are processed every tick.

### Cost summary

| Mechanism                    | Per-tick overhead added                              |
|------------------------------|-------------------------------------------------------|
| Per-event staleness          | One uint32 compare per drained event                 |
| `CancelPredictedEvent`       | One linear scan when called (≤ maxActiveEmitters)    |
| Interest management          | One `nth_element` per tick (O(N) average); one extra mute command per skipped emitter on the membership-change tick |

None of these touch the render thread; all costs are control-thread.

---



## What ships and what's deferred

| Area              | Shipped                                                              | Deferred                                                        |
|-------------------|----------------------------------------------------------------------|-----------------------------------------------------------------|
| Backend           | `NullAudioBackend` (default), `MiniaudioBackend` (opt-in, audible)   | Native WASAPI / CoreAudio / ALSA backends bypassing miniaudio   |
| Voice codec       | `StubVoiceCodec` (int16 PCM passthrough) + `OpusVoiceCodec` (libopus, opt-in via `AUDIO_ENGINE_VOICE_OPUS`) | Per-player codec selection, codec parameter automation         |
| Spatializer       | `DefaultSpatializer` (distance attenuation, equal-power pan, occlusion gain + per-voice LPF, Doppler with per-voice pitch smoothing, distance-driven air absorption, per-voice reverb send); `SphericalHeadSpatializer` (Woodworth ITD + head-shadow ILD, opt-in) | Pinna cues from measured HRIRs, ambisonics                      |
| Geometry          | `NullGeometryQuery` (always clear)                                   | Host-bound raycaster (interface is host-supplied)               |
| Bus graph         | Hierarchical buses, per-bus effect chain, sidechain compressor, biquad, Freeverb-derived reverb, proximity sends | SIMD effect kernels, parameter automation curves, multiple reverb zones |
| Asset lifetime    | Pre-loaded + pinned PcmAssets, decoded files (WAV/Ogg/FLAC), streaming with control-thread pump | Async load, eviction, virtual-voice retirement                  |
| Voice cap         | Priority + distance-tie-breaker eviction of one-shots; persistent emitters immune | Virtual voices with re-promotion, fade-out under eviction        |
| Listeners         | Single listener; multi-listener-ready interface                      | Split-screen mixing, per-listener buses                         |
| Orchestrator      | Parameter smoothing + simple sequence playback                       | State machines, rule graphs, designer-facing tools              |
| Predicted events  | Play unconditionally, no reconciliation                              | Fade-out / correction for long-running predicted sounds         |
| Network seam      | `OnTickAdvanced`, `SubmitReplicatedEvent`, `UpdateReplicatedTransform`, `OnVoicePacket` | Bandwidth budgeter, server-side mixing for spectators           |

The shipped surface is concrete and small. Every seam exists because it
carries long-term value; nothing was added "in case."

---

## Opus voice codec (libopus, opt-in)

`OpusVoiceCodec` is a production `IVoiceCodec` implementation that wraps
libopus. It's compiled in but **disabled at runtime** unless the build
flag is on, mirroring the pattern used by the file decoders; host code
references the type unconditionally and probes `IsSupported()` to decide
whether to hand it to `AudioRuntime` or fall back to `StubVoiceCodec`.

### Configuration

```cpp
OpusVoiceCodec::Settings s;
s.sampleRate      = 48000;     // 8/12/16/24/48 kHz; 48 is libopus's native
s.channels        = 1;         // mono; voice in every shipping FPS
s.frameSize       = 960;       // 20 ms @ 48 kHz, the VOIP default
s.bitrateBps      = 32000;     // 32 kbps; comfortable VOIP quality
s.maxDecoders     = 16;        // size to AudioConfig.budget.maxVoiceSources
s.applicationVoip = true;      // OPUS_APPLICATION_VOIP

OpusVoiceCodec codec(s);
if (codec.IsSupported()) {
    runtime.UseVoiceCodec(&codec);
} else {
    runtime.UseVoiceCodec(&fallbackStub);     // gated-OFF or libopus init failed
}
```

### Per-player decoder pool

A single Opus decoder shared across all incoming players would corrupt
itself the moment two players' packets interleaved (the decoder carries
PLC + crossfade state across frames). The wrapper keeps a fixed pool of
`Settings.maxDecoders` decoder instances, binds an `AudioPlayerId` to a
slot on first packet, and evicts the least-recently-used slot when the
pool is full (the evicted decoder is `OPUS_RESET_STATE`'d before being
reassigned, so the new player doesn't crossfade out of the old player's
last frame).

### Build flag

```
cmake -DAUDIO_ENGINE_VOICE_OPUS=ON ..
```

CMake resolves libopus in three strategies, in order:

1. **Vendored**; if `third_party/opus/CMakeLists.txt` exists, that source
   tree is added via `add_subdirectory()`. Use `scripts/fetch_opus.sh`
   (or `.bat`) to populate this slot from xiph/opus.
2. **System**; `find_package(Opus)` then `pkg_check_modules(opus)`. On
   Debian-derived systems: `apt install libopus-dev`.
3. **FetchContent**; clones xiph/opus at configure time. Gated by
   `AUDIO_ENGINE_FETCH_OPUS=ON` (defaults to ON), so this is the
   automatic fallback in network-enabled CI environments.

| Flag                          | Default | Effect                                              |
|-------------------------------|---------|-----------------------------------------------------|
| `AUDIO_ENGINE_VOICE_OPUS`     | OFF     | Compile + link libopus into `audio_engine`          |
| `AUDIO_ENGINE_FETCH_OPUS`     | ON      | If VOICE_OPUS is on and no vendored/system libopus, fetch from xiph/opus |

### VOIP-tuned encoder defaults

The wrapper sets these on the encoder at construction:

- `OPUS_APPLICATION_VOIP`; speech-optimised (vs music)
- `OPUS_SET_VBR(1)`; variable bitrate, lets the codec spend bits where they matter
- `OPUS_SET_COMPLEXITY(5)`; balance encoder CPU against quality (0–10)
- `OPUS_SET_INBAND_FEC(1)` + `OPUS_SET_PACKET_LOSS_PERC(5)`; forward error correction tuned for typical FPS network conditions; pairs naturally with the engine's jitter-buffer late-discard semantics
- Bitrate from `Settings.bitrateBps` (default 32 kbps)

### Test coverage

`tests/unit/voice_codec_test.cpp` runs three checks:

- **`StubVoiceCodec` round-trip**; encode/decode preserves int16 PCM
  exactly; over-capacity buffers return `BudgetExceeded`; null inputs
  return `InvalidArgument`. Validates the `IVoiceCodec` contract
  independent of any third-party dep.
- **`OpusVoiceCodec` gated-OFF**; codec is constructible,
  `IsSupported()` returns false, `Encode`/`Decode` return
  `AudioResult::Unsupported`. Hosts can integrate the codec
  unconditionally and probe at runtime.
- **`OpusVoiceCodec` gated-ON**; codec is supported, encode produces
  a non-empty packet, decode produces 960 frames, and total signal
  energy survives the round-trip within ±50% of the input (a generous
  tolerance for a lossy codec at 32 kbps).

The gated-ON test runs against either real libopus or a compile-only
stub (`opus.h` shim that exercises the same API surface). In this
repo's offline CI lane, the stub is what gets used; in deployment
environments with libopus available, the real library exercises the
same code paths and the same energy-survival assertion.

---

## Integration test

`tests/unit/integration_kitchen_sink_test.cpp` runs every subsystem at
once for 5 seconds of simulated time and asserts on cross-cutting
invariants. Per-feature unit tests catch regressions in their feature;
this one catches regressions in how the features compose under
sustained load.

The scenario:

- An 8-slot emitter pool; deliberately tight to force priority
  eviction
- Master + Reverb bus graph, Freeverb on the reverb bus, global send
  level 0.45
- A streaming "music" voice on a persistent emitter
- A persistent "bullet whip" emitter doing a continuous fly-by past
  the listener (drives Doppler + spatial pan)
- Critical-priority gunshots fired every 250 ms
- Low-priority ambient bugs fired every tick (= every 25 ms); about
  4× the gunshot rate, well above what the pool can absorb
- A custom `IAudioGeometryQuery` that occludes everything more than
  30 m away; combined with the air-absorption coefficient, makes
  distant sources both quieter and muffled
- Listener position translating in a circle, exercising spatial pan +
  Doppler

After 5 seconds the test prints a stats dump and asserts on six
cross-cutting invariants:

| Invariant                                  | Assertion                              | Observed (typical run)              |
|--------------------------------------------|----------------------------------------|-------------------------------------|
| Streaming pump kept ring fed               | `renderUnderruns == 0`                 | 0                                   |
| Render callbacks delivered                 | `> 800` over 5 s                       | 1000                                |
| Priority eviction fired                    | `oneShotEvictions > 0`                 | 101 (Critical evicted Low)          |
| Pool saturation reached                    | `oneShotsDroppedFullPool > 0`          | 32 (Low dropped against full pool)  |
| Audio actually came out                    | `peak > 0.05`, `rms > 0.005`           | peak 0.795, rms 0.207               |
| Reverb tail measurable after sources stop  | `tailRms > 1e-4` over 250 ms           | 0.065                               |

The test caught the very first integration-level concern that came
up during the reverb send work; the timing race between
`TickOneShots` (control thread, decrements remaining frames per tick)
and the mixer's actual sample consumption (render thread, decrements
per render callback). Sounds shorter than one tick get killed before
their first render. The integration test sidesteps this by using
multi-tick-duration sounds throughout, and the reverb test now
explicitly comments on the constraint. The architectural fix
(round-tripping mixer playback completion back to the control thread)
remains future work; it's a soft race, not unsafe, and the rule
"one-shot PCM assets should be at least one tick long" is workable
for now.

---

## Public surface

```cpp
audio::AudioRuntime runtime;
audio::AudioConfig cfg;            // pre-size everything here
audio::AudioRuntimeDependencies deps; // backend, codec, geometry, spatializer
runtime.Initialize(cfg, deps);

runtime.RegisterPcmSound(soundId, samples, sampleRate, channels);
runtime.SetListener(listener);

audio::EmitterDescriptor desc{...};
auto handle = runtime.CreateEmitter(desc).Value();

// Per game tick:
runtime.SetEmitterTransform(handle, position, velocity, forward);
runtime.SubmitEvent(audio::MakePlaySoundAtLocation(soundId, position));
runtime.Update(deltaSeconds);

runtime.Shutdown();
```

See `examples/minimal/main.cpp` for the full smoke test.

---

## License

The engine source under `include/`, `src/`, `tests/`, and `examples/`
is MIT-licensed; see `LICENSE` at the repository root. Vendored
dependencies in `third_party/` carry their own licenses, all
permissive (Unlicense / MIT-0 / public domain / BSD-3-Clause for
libopus when enabled). Retain those notices alongside your own when
redistributing.
