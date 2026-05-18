# Player Audio Preferences — gool

**Audience:** game devs building settings menus in the shipped game,
and anyone working on gool's Phase 4 implementation. This document
locks in the architecture *before* code lands so the API surface
doesn't need to be repainted later.

This is the **plan**, not yet the implementation. Engine code, binding,
persistence, schema tagging, and the settings-menu template are
scheduled across v0.29.0–v0.29.4. The game-side UI work can begin as
soon as v0.29.1 ships (Phase 4.2 — the GDScript API).

---

## Intent: dev mix ≠ player mix

A common audio-middleware failure mode is conflating two genuinely
different concerns. gool keeps them architecturally separate:

```
gool/config.json         ← dev-authored mix     (ships with the game, read-only at runtime)
                              base gain_db per bus
                              effect chains (compressor, reverb, etc.)
                              routing topology, sidechains
        ↓ multiplied by ↓
user://audio_prefs.cfg   ← player preferences   (lives in player's appdata, mutable at runtime)
                              per-bus gain offset
                              per-bus mute
```

**The dev sets what the mix should sound like. The player sets their
personal preference on top.** Neither overrides the other — the offsets
multiply at sample-render time inside the engine.

| Aspect | Dev mix (`gool/config.json`) | Player mix (`user://audio_prefs.cfg`) |
|---|---|---|
| **Who authors it** | Game developer / audio designer | The person playing the game |
| **When** | At build time (or during editor sessions via the mixer dock) | At runtime, via in-game settings menu |
| **Where it lives** | Project files, shipped inside the PCK | Player's OS appdata (`user://`) |
| **What it controls** | Bus topology, base gains, effect chains, sidechains | Per-bus gain *offset*, per-bus mute |
| **Persistence model** | Git, version-controlled, ships with patches | Survives game updates and reinstalls (Godot's standard `user://`) |
| **UI** | gool's mixer dock (editor-only) | Your game's settings menu (you build it) |

---

## Why the separation matters: three failure modes if you collapse them

These are the patterns that show up in shipping games when the two
layers aren't kept separate. gool's design specifically avoids each.

### Failure mode 1: no player volume controls at all

Game ships `gool/config.json` baked into the PCK; player has no way to
adjust anything. Music is too loud → the player either quits or alt-tabs
to mute their system. **Unshippable.** Every player-facing game needs
runtime volume controls; even "no music" players need a master.

### Failure mode 2: settings menu overwrites `gool/config.json`

If the in-game settings menu writes back into the project files, two
things break:

- **The next game update overwrites the player's preferences.** Patch
  ships a new `config.json` (perhaps a tuning revision from the audio
  designer) → player's "music quieter" preference is gone.
- **Read-only installs fail.** Steam installs games into read-only
  directories. Console sandboxes forbid writes outside designated save
  paths. Writing to the project folder is unportable.

### Failure mode 3: cross-player data leakage

If multiple players share a machine (family PC, console with multiple
user accounts) and prefs live in the project folder, they share the
same prefs. Godot's `user://` is per-OS-user, so the platform handles
this automatically — but only if we use it.

---

## The 5-phase plan

Five releases, each independently shippable. The split is sized so that
**game-side UI work can start at Phase 4.2** without waiting for the
final phases to land.

```
v0.29.0  Phase 4.1  Engine player gain overlay (C++ only)
v0.29.1  Phase 4.2  GDScript binding (Gool.set_player_bus_* API)
v0.29.2  Phase 4.3  Persistence to user://audio_prefs.cfg
v0.29.3  Phase 4.4  Bus schema "player_adjustable" tagging
v0.29.4  Phase 4.5  Drop-in settings menu template (optional)
```

### Phase 4.1 — Engine player gain overlay (v0.29.0)

A `PlayerGainOverlay` class living inside the engine, queried by the
bus graph during sample rendering. Per-bus state:

```cpp
struct PlayerBusPreference {
    float gain_offset_db = 0.0f;
    bool  muted          = false;
};
```

Effective gain at render time:
```
effective_db = static_config_gain_db + player_offset_db
effective_linear = pow(10, effective_db / 20) * (muted ? 0 : 1)
```

Default state is zeros + all unmuted, so existing projects behave
identically until the API is called.

**Audio-thread cost:** one float add and one branch per bus per buffer.
Sub-microsecond on modern hardware. Real-time safe (no allocations, no
locks — the overlay uses an atomic snapshot pattern like the rest of
gool's editor→runtime communication).

**C++ API on `AudioRuntime`:**
```cpp
void  SetPlayerBusGainOffsetDb(BusName, float db);
float GetPlayerBusGainOffsetDb(BusName) const;
void  SetPlayerBusMuted(BusName, bool);
bool  IsPlayerBusMuted(BusName) const;
void  ResetPlayerPreferences();
```

This phase is testable in isolation via C++ unit tests. No GDScript
binding yet, no persistence — pure in-memory layer.

### Phase 4.2 — GDScript binding (v0.29.1)

Expose the C++ API through the existing GDExtension binding:

```gdscript
Gool.set_player_bus_gain_offset("Music", -8.0)
Gool.get_player_bus_gain_offset("Music")   # → -8.0
Gool.set_player_bus_muted("Dialogue", true)
Gool.is_player_bus_muted("Dialogue")       # → true
Gool.reset_player_preferences()
```

**Critical milestone for game-side UI work.** As of this release, your
settings menu UI can be built and wired against a real API — sliders
emit `value_changed` → call `set_player_bus_gain_offset` → audio
changes immediately.

Still no persistence in this phase. Settings vanish when the game
closes. The UI work and the persistence work are split deliberately:
the UI shape doesn't depend on serialization decisions.

### Phase 4.3 — Persistence to `user://audio_prefs.cfg` (v0.29.2)

Reads `user://audio_prefs.cfg` at `Gool` autoload init; auto-writes on
every `set_*` call (debounced, same pattern as the dock's
`config_model.gd`).

**Format: standard Godot `ConfigFile`.** Not JSON — `ConfigFile` is
the right tool for player prefs because:
- It's the Godot-native format for `user://` data.
- Players or support staff can inspect/edit the file directly.
- Standard library handles platform path resolution for us.

```ini
[buses]
Master.gain_offset_db=0.0
Music.gain_offset_db=-8.0
Music.muted=false
"Voice Chat".gain_offset_db=3.0
Dialogue.muted=true
```

**Why bus names not bus indices:** bus order in `config.json` can
change between game versions; bus names are stable. A player who
adjusts "Music" expects their preference to survive a patch that
reorders the bus declarations.

**Reset behavior:** `Gool.reset_player_preferences()` deletes
`user://audio_prefs.cfg` entirely. Cleaner than writing zeros — also
covers the "I want to start fresh" support case.

### Phase 4.4 — Bus schema tagging (v0.29.3)

Add an optional `"player_adjustable": true` field to bus blocks in
`gool/config.json`:

```json
{ "name": "Music",            "parent": "Master", "gain_db": -3.0, "player_adjustable": true }
{ "name": "Sfx",              "parent": "Master",                  "player_adjustable": true }
{ "name": "SidechainHelper",  "parent": "Music" }
```

**Backward compatible.** Missing flag defaults sensibly:
- A bus that's a `category_routing` target (Music, Sfx, Voice, etc.) → defaults to `true`
- A bus that's not routed-to (internal/helper buses) → defaults to `false`

This means existing configs work without modification; the flag is for
the cases where the default isn't right (a hidden helper bus that
happens to be routed-to, or a routing target you intentionally want
hidden from the player UI).

**New API:** `Gool.get_player_adjustable_buses() -> Array[String]`.
A settings menu walks this list to know what sliders to show — so if
the dev adds a new player-facing bus later, the menu picks it up
without a code change.

### Phase 4.5 — Drop-in settings menu template (v0.29.4)

Ship `addons/gool/templates/audio_settings_menu.tscn` — a single-scene
template that:

- Calls `Gool.get_player_adjustable_buses()` at `_ready` to discover
  what to render
- Builds one row per bus: label + horizontal slider (percent) + mute
  toggle
- Adds a "Reset to defaults" button at the bottom
- Handles the percent ↔ dB conversion internally: `dB = 20·log10(pct/100)`
- Persists immediately (since persistence landed in 4.3)

Style is intentionally minimal so users restyle it for their game's
visual language. **Optional** — games can use the API directly and
build their own bespoke menu; the template exists for the people who
want a working settings menu in five minutes.

---

## Decisions locked in now

These shape the API surface, so locking them in early prevents
repaint after Phase 4.1 ships.

| Decision | Choice | Reason |
|---|---|---|
| Naming: `player_*` vs `user_*` vs `volume_preference` | `player_*` | "user" overlaps with Godot's `user://` path concept; "preference" is wordy. "Player" maps to the human. |
| API unit: dB or normalized [0,1]? | **dB everywhere in the API** | Native gool unit, matches dock readouts and `config.json`. Settings menu UI does the pct↔dB conversion (`dB = 20·log10(pct/100)`). |
| Mute API: separate, or `gain_offset_db = -INF`? | **Separate `set_player_bus_muted`** | -INF is awkward in a ConfigFile (`inf`? `-1e9`?) and ambiguous semantically (player asked for -inf vs player muted). |
| Player mute vs dev's M button in the mixer dock? | **Independent layers; both must be unmuted for audio to pass** | Dev M button is editor-time preview only (already session-transient — never persisted to config.json). Player mute is the shipped-game mechanism. They don't interact. |
| Offset range in settings menu UI? | **Recommend ±24 dB** | Beyond ±24 is rarely useful for end users; UI choice only — the engine API itself is uncapped (a game could set -60 dB programmatically if it wants). |
| Storage format | **Godot `ConfigFile`** | Native to `user://`, transparent to players, platform-portable. |
| Bus identifier in storage | **String bus name** | Stable across config.json reorderings; matches the API. |
| Persistence trigger | **Debounced auto-save (~500ms)** | Same pattern as the editor dock — UX consistency. |
| Reset semantics | **Delete the file** | Cleaner than zeroing; matches what most support contexts want. |

## Decisions deferred

Explicitly **not** locked in by this design. Future phases or specific
player feedback may bring these back into scope.

- **Per-emitter player overrides** ("I want all gunshots quieter, not
  all SFX"). Out of scope for Phase 4 — sound-tier granularity is the
  middleware's job, not the player's. Maybe Phase 5+ if a specific
  accessibility ask drives it.
- **Frequency-based player adjustments** ("boost dialogue clarity").
  Hearing-aid / accessibility territory; deferred indefinitely unless
  a concrete player ask materializes. Implementing this would mean
  exposing biquad params in the settings menu, which is the wrong UX
  for most players.
- **Cross-platform path conventions.** Godot's `user://` already
  abstracts this (Windows AppData, macOS Library, Linux XDG, console
  save partitions). We use `user://`; we don't reinvent.
- **Cloud sync of preferences** across player devices. Out of scope for
  gool — that's the game's job if it cares (Steam Cloud, etc.).
- **Profile / per-account separation** within a single OS user account.
  Godot's `user://` is per-OS-user; per-account-within-game is the
  game's save system, not gool's.
- **Per-bus EQ adjustments by the player.** Same family as
  frequency-based adjustments above. Deferred.

---

## What game devs should do *before* Phase 4 ships

The 4.1→4.5 sequence is designed so that **game-side UI work can begin
at 4.2** (v0.29.1). But there's something useful to do even earlier —
right now during Phase 3.3:

**Build the settings menu UI shell against placeholder calls.** Create
the `SettingsMenu.tscn` scene in your project's lobby. Add Master + a
slider per category (Music, SFX, Ambience, Voice Chat, Dialogue, UI),
mute toggles, and a Reset button. Wire each slider's `value_changed` to
a method that just `print`s for now. When v0.29.1 ships, replace the
placeholders with `Gool.set_player_bus_gain_offset(name, db)`
one-line-per-slider.

This is the kind of UI work that gets harder later (focus order,
controller support, accessibility, layout polish). Doing the structural
pass now while wireframing is cheap.

---

## Implementation notes for Phase 4 contributors

### Engine-side: where the overlay lives

`PlayerGainOverlay` should live in `include/audio_engine/player_gain_overlay.h`
and `src/audio_engine/player_gain_overlay.cpp`. It's queried by
`BusGraph::ApplyGain()` during the per-buffer render. Snapshot pattern:
the editor/main thread writes via atomic exchanges; the audio thread
reads a stable snapshot per buffer (same pattern as
`StaticBusConfig`'s runtime updates).

### Binding-side: GDExtension surface

Methods land on the `Gool` autoload (matching the existing `Gool.play_3d`
pattern). Use the same name-resolution layer that already maps bus
name → bus index in the binding; no new lookup table needed.

### Persistence-side: when to read, when to write

- **Read** at `Gool._ready`, once. Apply every loaded offset via the
  engine API.
- **Write** debounced ~500ms after the last `set_*` call. Same logic
  as the editor `config_model.gd` — reuse the pattern, not the code
  (different concerns, different file).
- **Don't** write on every `set_*` call (slider drag would write
  dozens of times per second).
- **Do** force-write on `Gool._exit_tree` so settings survive a
  clean shutdown even if the debounce hasn't fired.

### Default-by-category logic for Phase 4.4

When loading `config.json`, the `"player_adjustable"` flag resolves as:

```
1. If the bus has "player_adjustable" set explicitly → use that value
2. If the bus is a target in category_routing → default true
3. Otherwise → default false
```

This is computed once at config-load time and cached per-bus. No
per-frame cost.

---

## Open questions

These don't block Phase 4 but should be discussed before Phase 5.

- **Should player prefs sync across save slots within a single game?**
  My current lean: no — Godot's `user://` is per-OS-user, which is
  the right granularity for audio prefs. A second player on the same
  OS account playing the same game should get their own prefs only if
  the *game* implements per-profile save systems and asks gool to
  switch contexts. This would be a gool API like
  `Gool.load_player_preferences_from_path("user://profile_alice/audio.cfg")`
  — defer to Phase 5+.

- **Should there be a "reset to dev mix" button separate from "reset all"?**
  Trivial UI add but unclear if anyone wants it. Defer until a player
  asks.

- **Surround / spatial player prefs?** ("I'm on stereo headphones,
  downmix from 5.1"). Probably belongs in gool but as a *system*
  setting, not a per-bus preference. Out of scope for Phase 4;
  revisit when surround output ships.

---

## References

- Godot `ConfigFile` docs: <https://docs.godotengine.org/en/stable/classes/class_configfile.html>
- Godot `user://` path conventions: <https://docs.godotengine.org/en/stable/tutorials/io/data_paths.html>
- gool's existing mixer dock persistence (the pattern we mirror):
  `addons/gool/editor/config_model.gd`
- gool's audio-thread snapshot pattern (real-time-safe runtime config
  updates): `include/audio_engine/bus.h` `StaticBusConfig` docs
