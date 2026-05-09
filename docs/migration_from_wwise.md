# Migrating from Wwise to gool

A practical guide for teams switching their audio middleware from
Wwise to gool. Concept-by-concept mapping plus the API differences
you'll hit on day one.

The TL;DR: Wwise's authoring is the deepest in the industry and we
don't try to match it. gool covers the runtime feature set that 95%
of Wwise projects actually use — RTPCs, switches, states, busses,
positional voice — with a far simpler integration surface.

## Concept mapping

| Wwise                              | gool                                                              | Notes                                                            |
|------------------------------------|-------------------------------------------------------------------|------------------------------------------------------------------|
| Event (PostEvent)                  | `runtime.SubmitEvent(AudioEvent::MakePlaySoundAtLocation(...))`   | Same shape: trigger by id at position                            |
| Sound SFX                          | `SoundDefinition` (single-file)                                   | Defined in JSON sound bank                                       |
| Random / Sequence Container        | Group + `random` / `random_no_repeat` / `sequential` policy       | Configure per group in the JSON bank                             |
| Switch Container                   | Group + host-driven group selection                               | Host calls `runtime.SetGroupVariant("footsteps", "stone")`       |
| Blend Container                    | Multiple emitters with shared parameter; layered manually         | First-class blend container is roadmapped                        |
| RTPC (Real-Time Parameter Control) | `runtime.SetEmitterParameter(handle, paramId, value)`             | gool uses uint16 ids; map RTPC names to ids in your code         |
| State                              | Bus parameter snapshot (planned) + group selection                | State changes are application-level today                        |
| Bus / Audio Bus                    | `BusId` in the runtime bus graph                                  | 1:1 mapping; create with `audio::BusConfig`                      |
| Auxiliary Bus / Send               | `audio::ReverbSend` per emitter, multiple buses with sidechain    | Per-emitter send levels                                          |
| Sound Bank (`.bnk`)                | `.gpak` (uncompressed) + JSON index                               | gpak is the binary, JSON is the schema                           |
| 3D Position / Attenuation Curve    | `AttenuationSettings` (min/max distance + curve type)             | Linear, inverse, custom                                          |
| Spatial Audio (geometry)           | `IAudioGeometryQuery` for occlusion + obstruction                 | Inject host's physics world for ray-based occlusion              |
| Voice Chat (Wwise SoundEngine + custom) | First-class voice path with Opus + jitter buffer + PLC      | gool's only built-in plugin; Wwise voice usually external        |

## Key API differences

### 1. RTPC mapping

Wwise's RTPC names are application-level strings. gool uses
`uint16_t` parameter ids. Define a mapping table once:

```cpp
namespace ParamIds {
    constexpr uint16_t Intensity      = 100;
    constexpr uint16_t WeaponType     = 101;
    constexpr uint16_t HealthPercent  = 102;
}

// At trigger time:
runtime.SetEmitterParameter(handle, ParamIds::Intensity, 0.85f);
```

Numeric ids cost ~50 ns per lookup vs. ~500 ns for string-keyed
maps Wwise uses internally; the trade-off is no live editor
introspection. For a project that already has hundreds of RTPCs,
generate the id table from your existing Wwise project XML.

### 2. State as application logic

Wwise's State concept couples a state machine, group switches,
and bus parameter changes into one authoring object. gool
unbundles these:

- **Group variants** (Switch in Wwise) → `SetGroupVariant`
- **Bus param presets** (Snapshot in Wwise) → `SetBusParameter`
- **State machine** → host code

So a Wwise State change like `Player_Health → Critical` becomes:

```cpp
void OnPlayerStateChanged(PlayerState s) {
    switch (s) {
        case PlayerState::Critical:
            runtime.SetGroupVariant("breathing", "labored");
            runtime.SetBusParameter(audio::kBusMaster,
                audio::EffectParameter::LowShelf_GainDb, -3.0f);
            break;
        // ...
    }
}
```

The trade-off: more code, but a state change is auditable in your
game's source instead of buried in middleware authoring.

### 3. Sound Bank loading is synchronous

Wwise's bank loading is async with completion callbacks. gool's
is synchronous because banks are designed to be tiny — JSON +
gpak combined are typically <50 MB and load in milliseconds.

If your project has gigabyte-scale banks, partition them by
level/area and load on level change. Streaming individual files
within a bank works the same as Wwise (decoder + ring buffer).

### 4. Spatial Audio (geometry)

Wwise's Spatial Audio offers ray-based occlusion + obstruction
through a built-in geometry system. gool defines the same hook
points via `IAudioGeometryQuery`:

```cpp
class HostGeometry : public audio::IAudioGeometryQuery {
public:
    audio::OcclusionResult Query(const audio::Vec3& a,
                                    const audio::Vec3& b) override {
        // Use your physics world's raycast.
        return {.material = MaterialAt(hit), .distance = hit.distance};
    }
};
```

The host supplies the physics; gool applies the per-material
occlusion lowpass.

## Practical migration steps

1. **Export your Wwise structure as JSON.** Wwise has a Python
   API; write a small script that walks your project file and
   emits a gool sound bank JSON.
2. **Translate RTPC names to numeric ids.** Generate a header.
3. **Re-implement state machines** as host code.
4. **Test voice chat.** If you were using third-party voice on
   top of Wwise, gool's built-in voice is a net reduction in
   surface area — fewer SDKs to license and integrate.
5. **Drop Wwise integration.** Remove `Ak::SoundEngine::Init`,
   replace `PostEvent` with `runtime.SubmitEvent`.

## Open items

- **Snapshot equivalent** for Wwise State-driven mixer changes.
  Today: drive bus parameters manually.
- **Wwise project importer** — would need a Python tool that walks
  the `.wproj` and emits gool JSON. Not on the immediate roadmap.
- **Visual mixer/event editor.** Wwise's authoring UI is its
  flagship; gool is text-based today.

## Why migrate

You're considering this if Wwise's per-title licensing,
audio-team-specialist authoring requirement, or AAA-engine
complexity are misaligned with your indie multiplayer scope. gool
is a C++20 library you compile yourself, ships voice chat that
covers most multiplayer needs, integrates natively with Godot via
GDExtension, and is explicitly multiplayer-first (deterministic
replay, jitter-buffered voice, per-player telemetry). The
authoring is JSON instead of a visual editor — a meaningful loss
for content-heavy AAA teams, often a non-issue for indie teams
where the audio designer also writes scripts.
