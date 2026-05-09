# Asset pipeline (sound banks)

The engine ships a JSON-based sound bank loader so audio designers can
author a game's sounds without touching C++. This document is for the
person editing the JSON: the schema, the patterns that show up in
real games, hot-reload, and what every error message means.

If you're a programmer integrating the bank into a host game, the
short version is in [the README's Sound bank section](../README.md#sound-bank-data-driven-asset-pipeline)
and the full API surface is in [`include/audio_engine/sound_bank.h`](../include/audio_engine/sound_bank.h).

---

## Why a bank, instead of `RegisterSoundFromFile` calls in code

Three reasons that all matter:

1. **Stable hashed IDs.** A sound named `weapon.ak47.shot` always gets
   the same `AudioSoundId` (FNV-1a 32-bit hash of the name). Hot-reload
   doesn't shuffle the IDs, so an in-flight emitter holding the id
   keeps working after the bank reloads.
2. **One source of truth.** The defaults block sets sane values for
   category, priority, attenuation, bus, etc.; per-sound entries
   override only what they need to. The same JSON file describes the
   whole game's audio.
3. **Designer iteration.** Designers edit the JSON, hit save, the
   engine calls `Reload()`, the new attenuation curve takes effect on
   the next play. No recompile. No restart.

---

## Schema reference

A bank file is one top-level JSON object with up to four fields:

```json
{
  "version":  1,
  "defaults": { ... },
  "sounds":   [ ... ],
  "groups":   [ ... ]
}
```

`version` is reserved for future breaking changes. The current parser
accepts `1` (or omitted, treated as 1) and rejects others.

### `defaults` block

Every field is optional. Anything you set here applies to every sound
entry that doesn't override it.

| Field              | Type     | Values                                                     | Default          |
|--------------------|----------|------------------------------------------------------------|------------------|
| `category`         | string   | `"SFX"`, `"Voice"`, `"Music"`, `"Ambience"`, `"UI"`, `"Dialogue"` | `"SFX"`     |
| `priority`         | string   | `"Lowest"`, `"Low"`, `"Normal"`, `"High"`, `"Critical"`   | `"Normal"`       |
| `bus`              | string   | bus name, resolved via host's `BusResolver`                | `"master"`       |
| `spatialized`      | boolean  | `true` or `false`                                          | `true`           |
| `looping`          | boolean  | `true` or `false`                                          | `false`          |
| `occlusionEnabled` | boolean  | `true` or `false`                                          | `true`           |
| `replication`      | string   | `"LocalOnly"`, `"OwnerOnly"`, `"RemoteRelevant"`, `"Global"`, `"ServerAuthoritative"`, `"Predicted"` | `"LocalOnly"` |
| `attenuation`      | object   | see below                                                  | `{1.0, 50.0, 0.0, "Logarithmic"}` |

Attenuation sub-object:

| Field     | Type   | Meaning                                                  | Default           |
|-----------|--------|----------------------------------------------------------|-------------------|
| `min`     | number | distance below which gain stays at 1.0                   | `1.0`             |
| `max`     | number | distance at/beyond which gain drops to `floor`           | `50.0`            |
| `floor`   | number | gain past `max` (typically 0.0; non-zero for ambience)   | `0.0`             |
| `falloff` | string | `"Linear"`, `"Logarithmic"`, `"InverseSquare"`           | `"Logarithmic"`   |

### `sounds` array

Each entry is an object describing one sound. The only mandatory
field is `name`; everything else is optional and falls back to either
the defaults block or, where there's no default, an entry-level
default in the schema.

| Field              | Type     | Notes                                                              |
|--------------------|----------|--------------------------------------------------------------------|
| `name`             | string   | Hashed to an `AudioSoundId`. Must be unique. Required.             |
| `file`             | string   | Path relative to the JSON file's directory. Optional — see "Procedural sounds" below. |
| `category`         | string   | Overrides default if set.                                          |
| `priority`         | string   | Overrides default if set.                                          |
| `bus`              | string   | Overrides default if set.                                          |
| `spatialized`      | boolean  | Overrides default if set.                                          |
| `looping`          | boolean  | Overrides default if set.                                          |
| `occlusionEnabled` | boolean  | Overrides default if set.                                          |
| `replication`      | string   | Overrides default if set.                                          |
| `attenuation`      | object   | Overrides default if set. (Whole object replaces; not merged.)     |

### `groups` array

A group is a named handle that resolves to one of several member
sounds at play-time. Useful for footstep variations, gunshot variants,
impact sounds — anywhere you don't want the same sample to play on
back-to-back triggers.

| Field     | Type     | Notes                                                                   |
|-----------|----------|-------------------------------------------------------------------------|
| `name`    | string   | Hashed and stored alongside sound names. Must be unique. Required.      |
| `policy`  | string   | `"random"`, `"random_no_repeat"`, `"sequential"`. Required.             |
| `members` | string[] | Names of declared sounds (must already appear in `sounds`). Required.   |

`Find("group_name")` runs the policy and returns one of the member
ids each call. Find on a group is just as cheap as Find on a sound:
hash lookup + (for `sequential`) a relaxed atomic increment, or +
(for `random_no_repeat`) a relaxed atomic load and store.

---

## Patterns that show up in real games

### Footstep variations (random_no_repeat)

The single most common sound bank pattern. Each material has 3-8
variations; the group's `random_no_repeat` policy guarantees the same
clip never plays twice in a row.

```json
{
  "defaults": {
    "category": "SFX",
    "attenuation": { "min": 1.0, "max": 25.0, "falloff": "Logarithmic" }
  },
  "sounds": [
    { "name": "footstep.concrete.01", "file": "sfx/foot/concrete_01.wav" },
    { "name": "footstep.concrete.02", "file": "sfx/foot/concrete_02.wav" },
    { "name": "footstep.concrete.03", "file": "sfx/foot/concrete_03.wav" },
    { "name": "footstep.grass.01",    "file": "sfx/foot/grass_01.wav" },
    { "name": "footstep.grass.02",    "file": "sfx/foot/grass_02.wav" },
    { "name": "footstep.grass.03",    "file": "sfx/foot/grass_03.wav" }
  ],
  "groups": [
    { "name": "footstep.concrete", "policy": "random_no_repeat",
      "members": ["footstep.concrete.01", "footstep.concrete.02", "footstep.concrete.03"] },
    { "name": "footstep.grass",    "policy": "random_no_repeat",
      "members": ["footstep.grass.01",    "footstep.grass.02",    "footstep.grass.03"] }
  ]
}
```

In game code:

```cpp
const auto id = soundBank.Find("footstep." + materialName);
runtime.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(id, footPos));
```

### Weapon shots (priority + custom attenuation)

Gunshots cut through other audio (high priority) and travel further
than ambient SFX (large `max`). They're not group members — typically
one shot sample per weapon, optionally with one or two variants.

```json
{
  "sounds": [
    {
      "name":        "weapon.ak47.shot",
      "file":        "sfx/weapons/ak47_shot.wav",
      "priority":    "High",
      "attenuation": { "min": 2.0, "max": 200.0, "falloff": "Logarithmic" }
    }
  ]
}
```

For shots that need close-range punch and far-range falloff, raise
`max` and consider `"InverseSquare"` for sharper distance-based
attenuation.

### Music and ambience (non-spatialized, looping)

Music tracks aren't positioned in 3D and need to loop seamlessly.
Route them to a non-master bus the host configured at Initialize.

```json
{
  "sounds": [
    { "name":        "music.combat",
      "file":        "music/combat.ogg",
      "category":    "Music",
      "spatialized": false,
      "looping":     true,
      "bus":         "music" }
  ]
}
```

The host has to make `"music"` resolve to the right BusId via the
`busResolver` callback. See the README for the resolver pattern.

### UI sounds (UI category, no occlusion)

UI clicks, menu open/close, scoreboard transitions: not spatialized,
not occluded, normally on a separate bus the player can mute
independently of SFX.

```json
{
  "sounds": [
    { "name":             "ui.click",
      "file":             "sfx/ui/click.wav",
      "category":         "UI",
      "spatialized":      false,
      "occlusionEnabled": false,
      "bus":              "ui" }
  ]
}
```

### Procedural / debug sounds (no `file`)

The `file` field is optional. If you leave it out, the bank assumes
the audio data was already registered under the hashed id by the host
calling `runtime.RegisterPcmSound(HashSoundName(name), ...)` directly.
Useful for:

- Debug tones generated at runtime (sine sweeps, click tracks).
- Procedurally-generated audio (footstep impacts synthesized from
  velocity + material parameters).
- TTS or other dynamic sources where there's no on-disk file.
- **Testing**: the bank's own unit test uses this form to avoid a
  build-time dependency on the WAV decoder.

```cpp
// Host code, before LoadFromJsonString:
const auto id = audio::HashSoundName("debug.beep");
runtime.RegisterPcmSound(id, mySamples, 48000, 1);
```

```json
{
  "sounds": [
    { "name": "debug.beep" }
  ]
}
```

The bank still registers the SoundDefinition, so attenuation,
priority, bus routing, etc. apply normally.

### Asset packs and encrypted content (custom file loader)

The default file loader reads relative to the JSON file's directory.
For asset packs (Steam Workshop downloads, encrypted .pak files,
in-memory archives), supply your own:

```cpp
audio::SoundBankLoadOptions opts;
opts.fileLoader = [&](std::string_view path,
                       std::vector<uint8_t>& outBytes) -> bool {
    return myAssetPak.Read(path, outBytes);
};
soundBank.LoadFromJsonFile(runtime, "sounds.json", opts);
```

The callback receives whatever path string was in the JSON's `file`
field; interpret it however your asset system needs.

---

## Hot reload

`SoundBank::Reload(runtime)` re-parses the most recent source (file or
string) and re-registers every entry. Because IDs are hashes of names,
emitters and queued events that hold an old id keep working as long as
the entry is still present.

Typical dev-mode loop:

```cpp
// Every frame, or on a filesystem watcher:
if (jsonFileChangedOnDisk) {
    auto r = soundBank.Reload(runtime);
    if (!r.success) {
        Log("Sound bank reload failed at line %d: %s",
             r.errorLine, r.errorMessage.c_str());
    }
}
```

What changes between reloads:
- Sounds added or removed (by adding/removing entries).
- Per-sound metadata: category, priority, attenuation, bus,
  spatialization flags, etc. The next play picks up the new values.
- Group composition and policy.

What doesn't transfer mid-flight:
- A sound currently playing keeps the metadata it was created with
  for its remaining duration. Reload affects the *next* trigger.
- If you remove a sound's entry and re-add it under a different
  name, the id changes and any cached references break. Don't do
  this in production (only dev-mode hot-reload).

In production builds, you typically don't call `Reload()` — load the
bank once at level start and leave it.

---

## Error message reference

Every error reports a 1-based line number where available, and a
human-readable message. The line points at the offending entry's
opening brace, the malformed token, or the validation failure site.

| Error message                                                        | What it means                                                                          | How to fix                                                              |
|----------------------------------------------------------------------|----------------------------------------------------------------------------------------|-------------------------------------------------------------------------|
| `expected '{' to open ...`                                           | A required JSON object is missing its opening brace.                                   | Check syntax around the named context.                                   |
| `expected string`                                                    | Where a string was needed (a key, a value), the parser saw something else.             | Quote the value, or check for a stray comma/colon.                       |
| `unterminated string at end of file`                                 | A `"` was opened but never closed.                                                     | Close the string.                                                        |
| `unsupported string escape`                                          | The parser only handles `\" \\ \/ \n \r \t \b \f`. Unicode `\u` is not supported.       | Avoid non-ASCII characters in identifiers, or escape via raw UTF-8.      |
| `unexpected trailing data after root object`                         | Content past the closing `}`.                                                          | Remove the extra characters, or move them inside the root object.        |
| `unsupported bank version N`                                         | The `version` field has a value the parser doesn't know.                               | Set `version` to 1 or omit the field.                                    |
| `unknown category 'X'`                                               | Category enum value isn't one of the six valid names.                                  | Use exactly one of `SFX`, `Voice`, `Music`, `Ambience`, `UI`, `Dialogue`.|
| `unknown priority 'X'`                                               | Priority enum value isn't valid.                                                       | Use exactly one of `Lowest`, `Low`, `Normal`, `High`, `Critical`.        |
| `unknown falloff model 'X'`                                          | Falloff value isn't valid.                                                             | Use `Linear`, `Logarithmic`, or `InverseSquare`.                         |
| `unknown replication policy 'X'`                                     | Replication enum value isn't valid.                                                    | See the schema table.                                                    |
| `unknown group policy 'X'`                                           | Group `policy` value isn't valid.                                                      | Use `random`, `random_no_repeat`, or `sequential`.                       |
| `expected boolean for X`                                             | A field that must be `true`/`false` got something else.                                | Use a JSON boolean, not a quoted string.                                 |
| `sound entry has empty 'name'`                                       | A sound entry has `"name": ""` or no `name`.                                           | Add a non-empty name.                                                    |
| `duplicate sound name 'X'`                                           | Two sound entries (or one sound + one group) declared the same name.                   | Rename one.                                                              |
| `name 'X' is declared as both sound and group`                       | A group's name collides with a sound's name.                                           | Rename one. Group names share a namespace with sound names by design.    |
| `hash collision between 'X' and 'Y'; rename one of them`             | Two distinct names happen to hash to the same `AudioSoundId`. Extremely rare (≤ 1 per ~70K names). | Rename one.                                                  |
| `group 'X' has no members`                                           | The `members` array is empty or missing.                                               | Add at least one member.                                                 |
| `group 'X' member 'Y' is not a declared sound`                       | A group references a name that isn't in the `sounds` array.                            | Fix the typo, or declare the missing sound. Set `validateReferences = false` to skip. |
| `sound 'X' references unknown bus 'Y'`                               | The host's `busResolver` returned `kInvalidBusId` for the bus name.                    | Add the bus to the host's resolver, or change the entry's `bus` field.   |
| `sound 'X': failed to load file 'Y'`                                 | The file loader (default or custom) returned false.                                    | Check the path is correct relative to the JSON file's directory.         |
| `sound 'X': RegisterSoundFromMemory failed (decoder may be disabled at build time)` | The runtime rejected the loaded bytes — most often because the build wasn't compiled with the relevant decoder flag. | Build with the right `AUDIO_ENGINE_DECODERS_*` flag, or check the file's actual format. |

---

## Performance characteristics

Measured on commodity x86_64 (your hardware will vary, but the shape
is consistent):

| Operation                                | Cost                      |
|------------------------------------------|---------------------------|
| Load a 1000-entry bank from JSON string  | ~0.6 ms total             |
| `Find()` (hash lookup)                   | ~54 ns/op (~18.5 M ops/s) |
| `Find()` on a group with policy          | same + 1 atomic op        |
| `Reload()`                               | same as load              |

What allocates and what doesn't:

- **Load** allocates: the parsed-JSON intermediate, the name→id map,
  the per-group member-id arrays. All proportional to bank size, all
  released before Load returns. The only retained allocation is the
  hash maps and member arrays.
- **`Find()`** does not allocate. It does one `unordered_map` lookup
  on the sound name, falls through to the group map if absent, and
  for groups does at most one relaxed atomic load and one relaxed
  atomic store. Safe to call from any thread that isn't concurrently
  loading.
- **`Reload()`** allocates the same things Load does. Don't call it
  from the audio render thread.

There is no per-tick or per-play cost from having a bank loaded. The
runtime sees the registered sounds and definitions like any other,
and the bank is touched only when host code calls `Find()`.

---

## What the bank does NOT do

To set expectations, here's what's deliberately out of scope:

- **No RTPCs.** The schema doesn't have parameter-driven mappings
  ("change low-pass cutoff based on player health"). If you need
  this, do it in game code by mutating per-emitter state.
- **No switch containers.** Selection is per-call via `Find()`; the
  bank doesn't track per-emitter switch state.
- **No music systems.** No interactive music graphs, no stingers, no
  beat-synchronized triggering. Music is just a sound with `looping:
  true` and a music bus.
- **No file packing.** The bank reads files at load time via the file
  loader callback. Packing N sounds into one shippable blob is a
  separate problem; supply a `fileLoader` that reads from your pack.
- **No editor.** Designers edit the JSON directly, in their normal
  text editor. A small standalone authoring tool may ship later if
  there's enough demand.

These omissions are why the bank is ~1100 lines instead of Wwise's
hundreds of thousands. The line we've drawn: enough structure that
designers can author without writing code, no more than that.
