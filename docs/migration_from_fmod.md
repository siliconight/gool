# Migrating from FMOD Studio to gool

A practical guide for teams switching their audio middleware from
FMOD Studio to gool. Concept-by-concept mapping plus the API
differences you'll hit on day one.

The TL;DR: gool's authoring model is JSON sound banks instead of
FMOD's `.bank` files, and the API is a single `AudioRuntime`
class instead of `Studio::System` plus `Core::System`. Most FMOD
concepts have a direct counterpart; a few (snapshots, command
instruments) need re-expression.

## Concept mapping

| FMOD                          | gool                                                       | Notes                                                                 |
|-------------------------------|------------------------------------------------------------|-----------------------------------------------------------------------|
| Event (`EventInstance`)       | `Emitter` + `SoundDefinition`                              | gool emitters are spatialized by default; non-spatial = music/UI      |
| Bank (`.bank`)                | `.gpak` file + JSON sound bank                             | gpak is the binary; the JSON is the authoring index                   |
| Bus / Mix Group               | `BusId` in the runtime bus graph                           | 1:1; create with `audio::BusConfig`                                   |
| Snapshot                      | Bus parameter preset (planned, see Open Items)             | Runtime today: drive bus params manually via `SetBusParameter`        |
| Parameter (continuous/labeled)| Sound/emitter parameters via `SetEmitterParameter`         | gool parameters drive DSP; named via `EffectParameter::*` IDs         |
| Command Instrument (Events)   | Group + group policy in the SoundBank                      | Random / random_no_repeat / sequential — see `sound_bank.cpp`         |
| 3D Min/Max Distance           | `AttenuationSettings::minDistance`, `maxDistance`          | Same semantics; configure per `SoundDefinition`                       |
| Spatializer (built-in/plugin) | `ISpatializer` + built-in `SphericalHeadSpatializer`       | Plug a custom spatializer via dependency injection                    |
| Reverb (zones, snapshots)     | Reverb bus effect + `ReverbZone` Area3D in Godot           | Send levels per emitter; zone trigger via Godot signals               |
| Studio::System::loadBankFile  | `SoundBank::LoadFromJsonFile` (with optional `.gpak` path) | Async: hot reload via `LoadFromJsonString` on file change             |
| eventInstance->start()        | `runtime.SubmitEvent(MakePlaySoundAtLocation(id, pos))`    | Or `runtime.CreateEmitter(desc)` for handle-based control             |
| eventInstance->setParameterByName | `runtime.SetEmitterParameter(handle, paramId, value)`  | gool uses uint16 parameter ids; FMOD uses strings                     |
| eventInstance->release()      | `runtime.DestroyEmitter(handle, fadeOutMs)`                | Built-in fade-out; FMOD requires manual envelope                      |
| Voice limit / culling         | Per-bus voice cap with priority eviction                   | Configure in `AudioConfig::voiceCap`; lower priority evicts on cap    |

## Practical migration

### 1. Convert your banks

FMOD `.bank` files are proprietary. Re-author content as a gool
JSON sound bank:

```json
{
  "defaults": { "spatialized": true, "minDistance": 1.0, "maxDistance": 50.0 },
  "sounds": [
    { "name": "footstep_stone", "file": "sfx/footstep_stone.ogg",
      "group": "footsteps_stone", "policy": "random_no_repeat" },
    { "name": "music_combat",   "file": "music/combat.flac",
      "spatialized": false, "looping": true, "loopCrossfadeMs": 100.0 }
  ]
}
```

Pack the referenced files into a `.gpak`:

```bash
./build/tools/gpak_create assets.gpak --base assets/ \
    assets/sfx/footstep_stone.ogg assets/music/combat.flac
```

Load both at runtime:

```cpp
audio::PakReader pak;
pak.Open("assets.gpak");
audio::SoundBank bank;
audio::SoundBankLoadOptions opts;
opts.fileLoader = pak.MakeSoundBankLoader();
bank.LoadFromJsonFile(runtime, "sounds.json", opts);
```

### 2. Replace event API calls

```cpp
// FMOD
EventInstance* inst;
event->createInstance(&inst);
inst->set3DAttributes(&attr);
inst->setParameterByName("intensity", 0.7f);
inst->start();
// ...
inst->release();

// gool
auto h = runtime.CreateEmitter({
    .soundId = audio::HashSoundName("explosion"),
    .position = {x, y, z},
    .isSpatialized = true,
    .fadeInMs = 0.0f,
});
runtime.SetEmitterParameter(h.value(),
    audio::ParameterId::Custom_Intensity, 0.7f);
// ...
runtime.DestroyEmitter(h.value(), /*fadeOutMs=*/100.0f);
```

### 3. Snapshots — current workaround

FMOD snapshots capture a set of bus parameter values and let you
crossfade between them. gool has the underlying machinery
(`SetBusParameter`) but doesn't yet ship a snapshot wrapper.
Workaround: define a struct of bus parameter targets per "mood"
and apply them with a hand-written tween. A snapshot helper is
on the roadmap.

### 4. Voice chat is included

FMOD doesn't ship voice chat — typical FMOD-based games license
Vivox or similar. gool ships an Opus-backed voice path with
adaptive jitter buffer, PLC, and per-player telemetry.
`runtime.OnVoicePacket()` is the entry point; see the multiplayer
section of the main README.

## Open items

- **Snapshots** as a first-class concept. Today: roll your own
  with `SetBusParameter` + tween.
- **FMOD Studio importer.** Reading `.bank` files would require
  reverse-engineering the FMOD bank format; not on the roadmap
  but contributions welcome.
- **Visual event editor.** FMOD Studio's visual authoring is its
  killer feature for non-programmers. gool's authoring is
  text-based today; a Godot dock plugin is planned.

## Why migrate

You're considering this if FMOD's licensing, AAA-tier complexity,
or lack of multiplayer voice integration are real costs for your
team. gool is an open-source C++20 library that builds in 60
seconds, ships voice chat in the box, and integrates natively
with Godot via the GDExtension binding. The trade-off is the
authoring story: text-driven JSON instead of FMOD Studio's
visual graph. For indie multiplayer teams shipping with
programmer-time as the bottleneck, the simpler runtime API and
free voice chat usually outweigh the missing visual editor.
