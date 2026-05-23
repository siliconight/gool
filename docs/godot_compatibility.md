# Playing nice with Godot

A reference guide for what you need to know about **Godot itself** when
using gool — not how to use gool's features, but how gool sits inside
a Godot project and where Godot's design choices ripple into yours.
Read this if you've finished
[`godot_quickstart.md`](godot_quickstart.md) and now you want to
understand the platform-level constraints before you start shipping.

Source: this doc reflects gool's behavior as of v0.63.0 against Godot
4.2+ stable. Where Godot's docs say "x is true", we say so. Where
gool's behavior diverges from what a new user might expect, we flag
it explicitly.

---

## TL;DR

| Concern | What to know |
|---|---|
| **gool's audio path is separate from Godot's `AudioServer`** | gool ships its own bus graph (`res://gool/config.json`), its own DSP, its own mixer. `AudioStreamPlayer` and gool's `AudioEmitter3D` are parallel systems, not nested. They mix to the same OS output. |
| **gool runs through GDExtension** | The `gool_godot` shared library loads at engine startup. Feature tags (`web`, `arm64`, etc.) decide which binary loads. |
| **Don't touch gool nodes from worker threads** | gool's prefabs are scene-tree nodes. Same Godot rule as everything else: call from a thread → use `call_deferred`. |
| **The `/root/Gool` autoload isn't reachable in the editor** | gool prefabs all `Engine.is_editor_hint()` -gate to avoid touching it. If you write your own gool-using script and call gool APIs unconditionally in `_ready`, the editor will spam warnings. |
| **Web export is the biggest gotcha** | GDExtension support must be ticked in the export. Single-threaded export is fine; multi-threaded export needs cross-origin headers. Tab backgrounding pauses `_process` (which pauses gool's update tick). |
| **Dedicated server (`--headless`) silently uses the Dummy audio driver** | gool still works — voices play, ducking ducks, profiles apply — but no sound reaches a speaker (there is no speaker). This is fine for headless game logic and tests. |

---

## How gool sits inside a Godot project

```
┌──────────────────────────────────────────────────────────────────┐
│  Godot scene tree                                                │
│                                                                  │
│   /root                                                          │
│     ├── Gool                ← autoload (gool's runtime)          │
│     ├── YourMainScene                                            │
│     │     ├── AudioEmitter3D (gool prefab)                       │
│     │     ├── GoolListener3D (gool prefab)                       │
│     │     ├── ReverbZone (gool prefab)                           │
│     │     └── AudioStreamPlayer (vanilla Godot — optional)       │
│     └── ...                                                      │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
            │                                  │
            ▼                                  ▼
┌─────────────────────────┐         ┌──────────────────────────┐
│ gool engine (C++)       │         │ Godot AudioServer        │
│  - own bus graph        │         │  - own bus graph         │
│  - own DSP              │         │  - own DSP               │
│  - own decoder pool     │         │  - own decoders          │
│  - own voice pool       │         │  - own stream players    │
└─────────────────────────┘         └──────────────────────────┘
            │                                  │
            └─────────► OS audio output ◄──────┘
              (miniaudio backend)    (Godot's driver)
```

gool and Godot's `AudioServer` are **parallel** subsystems. They don't
share buses, effects, listeners, or any state. The mixing into your
speakers happens in the OS audio output layer where both contribute
audio frames. This is intentional — it lets gool implement audio
features (per-material EQ, multi-tier sidechain, voice chat, master
FX) that Godot's `AudioServer` doesn't offer, without fighting
Godot's own audio pipeline.

The two systems coexisting has one practical consequence: **gain
levels are independent**. The Godot master bus's volume slider has
nothing to do with gool's master bus level. If your game lets the
user adjust audio in the Settings menu, you need to expose both knobs
(or pick one system and stick with it).

---

## Threading rules (Godot ↔ gool boundary)

gool has its own threading story documented in
[`THREADING.md`](THREADING.md). The summary is: real-time audio runs
on a dedicated render thread owned by miniaudio; everything else
(`AudioRuntime::Tick`, voice decode, telemetry) runs on the control
thread, which the host (your Godot project) drives.

In Godot terms, **the control thread is your `_process` thread** —
the main game thread that the scene tree runs on. The `Gool` autoload
calls `Tick()` and `Update()` from `_process`.

What this means for you:

| If you do this... | Then... |
|---|---|
| Call `Gool.play_3d(...)` from `_process` / `_physics_process` / signal handlers | ✅ Fine — that's the control thread. |
| Call `Gool.play_3d(...)` from `Thread.new(...).start()` worker | ⚠️ Race condition. Use `Gool.play_3d.call_deferred(...)` or marshal back to the main thread some other way. |
| Add an `AudioEmitter3D` child node from a worker thread | ⚠️ Same Godot rule that applies to any node. Use `add_child.call_deferred(emitter)`. |
| Load a `GoolSoundBank.tres` from a `ResourceLoader.load_threaded_request` | ✅ Fine — same as any Godot resource. Don't request the same one twice in parallel. |
| Construct a `GoolMaterialEqPreset` programmatically and assign it from a worker | ⚠️ Construct on the worker (fine, no scene-tree contact), assign to the node from the main thread via `call_deferred`. |

For voice chat specifically: incoming Opus packets typically arrive
on the networking thread. The right pattern is to enqueue them into
gool from any thread (gool's voice packet queue is internally
SPSC-safe), but never call `VoiceChatPlayer` methods that touch the
scene tree from off-thread.

---

## How gool's bus graph relates to Godot's

Godot's audio model centers on **`AudioBusLayout`** (the
`res://default_bus_layout.tres` file the editor writes whenever you
edit the **Audio** bottom panel). Buses are named (Master, Music,
SFX, etc.); `AudioStreamPlayer` nodes reference a bus by name; the
mix flows right-to-left into Master.

gool's audio model centers on **`res://gool/config.json`** (or
whatever path your project uses — the addon writes it on first
enable). gool's buses are also named, also reference each other,
also flow into a Master sink — but they're a different graph in a
different format with different effects.

**The two graphs never connect.** You cannot route a gool bus into a
Godot `AudioStreamPlayer`'s bus, and you cannot route the other way.
Each system mixes to its own master, and the OS combines the outputs.

This is usually what you want — but be aware of two specific
consequences:

1. **Bus rearrangement / renaming.** Godot's docs note that
   `AudioStreamPlayer` references buses by name, so renaming a bus
   breaks the reference. gool follows the same convention: gool
   prefabs (`AudioEmitter3D`, `VoiceChatPlayer`, etc.) reference gool
   buses by name through the `bus` `@export`. Renaming a bus in
   `config.json` will silently break any prefabs that pointed at it
   (they fall back to the Master bus).

2. **Default bus layout.** Godot will create
   `res://default_bus_layout.tres` whenever the user opens the audio
   panel. That file is for Godot's `AudioStreamPlayer` system —
   ignore it for gool purposes. If your project doesn't use Godot's
   audio nodes at all, the file is still created but never read by
   gool.

If you want to share **volume preferences** between the two systems
(e.g. one master volume knob in your Settings UI), drive both by the
same `ProjectSettings` value:

```gdscript
# Settings UI: when the master slider moves
func _on_master_volume_changed(value: float) -> void:
    # Update Godot's master bus
    AudioServer.set_bus_volume_db(AudioServer.get_bus_index("Master"),
                                   linear_to_db(value))
    # Update gool's master bus (its name in default config is also "Master")
    Gool.set_bus_volume_db("Master", linear_to_db(value))
```

---

## GDExtension binary selection

gool is a GDExtension — a native shared library that Godot loads at
startup. The `.gdextension` file at `res://addons/gool/gool.gdextension`
tells Godot which binary to load for each platform, using
[feature tags](https://docs.godotengine.org/en/stable/tutorials/export/feature_tags.html).

The relevant tags for gool's binary selection:

| Feature tag combination | Binary loaded |
|---|---|
| `linux.x86_64` | `gool_godot.linux.x86_64.so` |
| `macos.arm64` | `gool_godot.macos.arm64.dylib` |
| `macos.x86_64` | `gool_godot.macos.x86_64.dylib` |
| `windows.x86_64` | `gool_godot.windows.x86_64.dll` |
| `web.wasm32` | `gool_godot.web.wasm32.wasm` |

The `editor` vs `template` vs `debug` vs `release` axes aren't
currently split — gool ships one binary per platform/arch that works
in both editor and runtime contexts. If gool's `.gdextension` file
shows a target you don't have a binary for, the addon will fail to
load at startup with a console error.

If a custom export preset adds custom feature tags, gool ignores
them — the binary selection only uses the standard platform tags.

---

## The `@tool` requirement and editor-context safety

gool's prefabs (`AudioEmitter3D`, `GoolSceneProfile`,
`GoolMasterFxProfile`, etc.) are all `@tool` scripts so they show in
the editor's Add Node dialog and so their `@export` properties drive
the inspector.

`@tool` has a consequence Godot's docs are explicit about: **the
script runs in the editor too, not just at runtime**. If a gool
prefab's `_ready` tried to call `Gool.play_3d(...)`, the editor would
crash because `/root/Gool` doesn't exist in editor-context (autoloads
are only added when a scene is being run, not when it's open in the
editor).

Every gool prefab guards against this with:

```gdscript
func _ready() -> void:
    if Engine.is_editor_hint():
        return
    # ... runtime behavior
```

If you write your own gool-using scripts and mark them `@tool` (for
example, an editor utility that uses `GoolSoundBankLoader`), you
need the same guard. As a rule:

- **Anything that calls a method on `/root/Gool`** must check
  `Engine.is_editor_hint()` first.
- **Anything that only reads gool's Resource types** (e.g. opens a
  `GoolMaterialEqPreset.tres` to display its curve in an editor
  panel) is safe to do in editor context, because Resources don't
  need the autoload.

---

## Lifecycle: when does `/root/Gool` exist?

Godot's notification order (per its docs) is:

```
AUTOLOADS → ROOT SCENE → child nodes → _enter_tree → _ready → _process
```

Autoloads are added to `/root` before your main scene's nodes call
`_enter_tree`, but the autoloads' own `_ready` may not have fired yet
when your nodes' `_enter_tree` fires. The safe pattern:

| Hook | Can you call `Gool.*`? |
|---|---|
| `_init` (constructor) | ❌ No. Scene tree not built. |
| `_enter_tree` | ⚠️ `/root/Gool` exists but may not have run `_ready`. Don't call methods that touch the engine runtime. |
| `_ready` | ✅ Safe. This is where gool prefabs do all initialization. |
| `_process` / `_physics_process` | ✅ Safe. |
| `_exit_tree` | ✅ Safe — gool autoload outlives child scenes. Good place to disconnect signals. |
| `NOTIFICATION_WM_CLOSE_REQUEST` | ⚠️ Application shutdown. gool's autoload handles its own clean-up; don't call gool methods here. |

If you need to do gool work before any scene loads (e.g. preload
sound banks before the title screen), do it in your **own** autoload
script that depends on the `Gool` autoload being available, NOT in
the title screen's `_ready`. Autoload load order follows the order
declared in **Project Settings → AutoLoad**, so put your script
**below** `gool` in that list.

---

## Web export: the biggest set of gotchas

Web is the most constrained Godot platform for audio, and it
interacts with gool in several specific ways. Read
[Godot's web export docs](https://docs.godotengine.org/en/stable/tutorials/export/exporting_for_web.html)
first; the highlights that affect gool:

### 1. GDExtension must be explicitly enabled

In the web export preset, **Extensions Support** must be ticked.
Without it, gool's shared library won't load and the addon will fail
to initialize. Godot will print an error in the browser console
(F12); the rest of your game runs but gool is inert.

### 2. Single-threaded export is the default since Godot 4.3 — and that's fine

Godot 4.3+ defaults to single-threaded web export to avoid the
`SharedArrayBuffer` / cross-origin isolation deployment overhead.
gool works fine in single-threaded mode — gool doesn't spawn its own
worker threads in the GDExtension binary, and miniaudio's web
backend uses the browser's audio thread via the Web Audio API.

If you DO enable Thread Support in the export, you need cross-origin
isolation headers on the hosting server:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Without these headers, the threaded build won't even start. Many
hosts (itch.io, Poki, CrazyGames) don't support these headers, which
is why single-threaded is the recommended default.

### 3. Background tabs pause gool

When the user switches to another browser tab, Godot pauses
`_process` and `_physics_process`, which means gool's `Tick()` and
`Update()` stop running. Consequences:

- Voice chat will drop packets that accumulate while the tab is
  backgrounded (gool's voice ring buffer overflows; new packets
  evict old ones).
- Active ducking compressors freeze their envelope at the last value
  before the pause; on resume, the envelope continues from there
  (briefly mistracks the new audio for a few ms until the envelope
  catches up).
- Sound effects in flight finish playing (because the audio
  backend's render thread keeps running), but no NEW effects will
  start until `_process` resumes.

If you're shipping a multiplayer Web game, treat
`Window.visibilitychange === "hidden"` as effectively a network
disconnect from gool's voice chat's perspective — don't try to
reconnect mid-tab-switch; let it ride.

### 4. Browser audio policy

Browsers don't allow audio to start until the user has interacted
with the page (a tap, click, or keypress). Godot handles the
`AudioContext` resume internally when input arrives, but **the first
sound gool tries to play before any input may silently fail to
audible**. The voice chat usually doesn't hit this because
microphone access requires user permission anyway, but autoplay
music or ambient loops on the title screen will be silent until the
player clicks "Start".

Mitigation: don't autoplay anything important before the player has
clicked. Put your first sound on a "Press any key to continue" or
similar gated screen.

### 5. The Sample vs Stream Godot playback type doesn't apply to gool

Godot's audio docs talk a lot about **Sample** (Web Audio
API-based, low latency, no effects) vs **Stream** (Godot's own audio
pipeline, higher latency, full effects) playback modes for the web.
**That distinction is entirely about Godot's `AudioStreamPlayer`
nodes — gool's audio path is separate.** gool always uses its own
DSP pipeline regardless of the Godot project setting; gool's
effects, EQ, reverb, ducking, master FX all work on web exports.

You only need to think about Sample/Stream mode if you're also using
Godot's `AudioStreamPlayer` for some sounds alongside gool.

---

## Dedicated server / `--headless`

For multiplayer games with a dedicated server build, Godot's
recommended approach is the dedicated server export preset, which
auto-adds `--headless` and uses the **Dummy** audio driver. Per
Godot's docs, this means no audio output happens.

gool's behavior on a headless build:

- **gool still initializes.** The autoload comes up, the config
  loads, the bus graph builds, voice/decoder pools allocate.
- **gool's audio path still runs through miniaudio's null backend.**
  Frames are mixed, effects process, ducking ducks — but the output
  is discarded instead of reaching a speaker.
- **API behavior is identical to a normal build.** `play_3d()`
  returns voice handles, `get_render_stats()` returns sensible
  numbers, telemetry continues to update. The only observable
  difference is silence.

This is what you want for a dedicated server. Server-side game logic
that depends on audio events (e.g. "voice chat is currently active
on this client" telemetry) keeps working.

If you want to **skip gool initialization entirely** on a dedicated
server to save the ~5 ms startup cost and ~30 MB memory, gate the
`Gool` autoload with a feature tag in your project setup:

```gdscript
# Your bootstrap autoload, sitting ABOVE Gool in the autoload list
extends Node

func _ready() -> void:
    if OS.has_feature("dedicated_server"):
        # Skip gool entirely on the server
        var gool_node := get_node_or_null("/root/Gool")
        if gool_node:
            gool_node.queue_free()
```

**Don't** strip the gool binary out of the export — gool's
GDExtension `.gdextension` file is still loaded; the server just
doesn't use it.

**Don't** use the **Strip Visuals** option on `.tres` files
representing gool resources (presets, profiles, sound banks). Godot's
strip mechanism handles textures and materials, not arbitrary
resource types; stripping a gool resource may produce a placeholder
that fails to load with a confusing error.

---

## Audio file imports: how gool reads audio vs. Godot's pipeline

Godot has its own audio import pipeline that generates `.import`
sidecar files for `.wav`, `.ogg`, and `.mp3` assets. The pipeline
applies compression, normalization, trim, loop mode, BPM tagging
(for interactive music), etc. The result is consumed by
`AudioStreamPlayer` via `AudioStream` resources.

**gool doesn't use Godot's import pipeline.** gool's `GoolSoundBank`
resource lists audio files by `res://...` path; gool's decoder pool
(`wav_decoder`, `ogg_vorbis_decoder`, `opus_file_decoder`,
`flac_decoder`) opens the raw file bytes at runtime through Godot's
`FileAccess` and decodes them with gool's own decoders. The import
settings — sample rate force, mono force, normalization, loop mode —
have no effect on what gool plays.

| If you want this... | Do this for gool |
|---|---|
| Reduce file size on disk | Encode the source file at the desired bitrate before placing it in `res://gool/audio/`. gool doesn't re-encode. |
| Use a sound through both gool AND `AudioStreamPlayer` | Place the file once in `res://`; both systems read it independently. gool decodes the raw file, Godot decodes its `.import` cache. (You're paying for decode twice if you play it through both; usually negligible.) |
| Loop a clip | Encode loop points into the source file (WAV `cue` chunks for WAV; Ogg/MP3 — see gool's decoder docs). Godot's loop-mode import settings don't affect gool. |
| Trim silence | Trim it in your DAW before saving. |

If you only use gool, you can disable Godot's audio import for files
under `res://gool/audio/` by adding a `.gdignore` file in that
directory — Godot won't scan or import audio it contains. gool
reads the files directly via `res://` paths, which still works
through Godot's resource filesystem even with `.gdignore`.

---

## Feature tags to know

A short list of the feature tags gool cares about, from Godot's
[feature tags reference](https://docs.godotengine.org/en/stable/tutorials/export/feature_tags.html):

| Tag | What it means for gool |
|---|---|
| `editor_hint` | Running inside the Godot editor. gool prefabs no-op on this; use the same pattern in your own gool-using scripts. |
| `editor_runtime` | Project is running, launched from the editor. Same gool behavior as a release build. |
| `template` | Running from an exported binary (not the editor). Normal gool behavior. |
| `web` | Web build. See the web section above for the constraints. |
| `nothreads` | Threading is disabled. gool itself is single-thread-friendly; doesn't change behavior. |
| `dedicated_server` | Dedicated server export. gool initializes but uses the Dummy backend. Consider gating gool off — see the dedicated server section. |
| `mobile` | Mobile platform. gool runs but CPU budget is tighter; consider lower polyphony / disabled effects via your gool config. |
| `arm64`, `x86_64`, `wasm32` | Architecture. Selects which gool binary loads. |

To check at runtime:

```gdscript
if OS.has_feature("dedicated_server"):
    # server-only code
if OS.has_feature("web"):
    # web-only fallback (e.g. lower-quality reverb)
```

---

## Sync with audio (rhythm games, beat-matched gameplay)

Godot exposes `AudioServer.get_time_to_next_mix()`,
`AudioServer.get_output_latency()`, and
`AudioServer.get_time_since_last_mix()` for projects that need to
align gameplay events to the audio clock with sub-frame precision —
the standard rhythm-game pattern.

**gool does not currently expose equivalents.** gool's playback
position is tracked internally per voice, but the
`get_time_to_next_mix()` style query against gool's render thread
isn't yet bound to GDScript. If your project needs sub-frame audio
sync today, the recommended path is:

1. Play the music track through Godot's `AudioStreamPlayer` (NOT
   gool). Godot's `AudioStreamPlayer` is well-supported for the sync
   case and the API is documented and stable.
2. Play sound effects, voice, ambience, and ducking through gool.
3. Drive gameplay timing from `AudioServer.get_time_to_next_mix()`
   against the Godot `AudioStreamPlayer` playing your music.

This split works because gool and Godot's `AudioServer` are
independent — they mix into the OS output in parallel without
interfering. It's not elegant, but it's what works today.

If sub-frame sync against gool's clock becomes a real need, it's a
candidate for a future gool release (the engine has the data; it
just isn't bound).

---

## Performance budget interactions

[`performance_budgets.md`](performance_budgets.md) lists gool's
internal CPU targets per subsystem. From a Godot perspective, the
relevant interaction is: **gool's `Tick()` runs on the main thread
inside `_process`**, so anything gool spends comes out of the same
~16 ms (60 FPS) or ~33 ms (30 FPS) frame budget your game logic uses.

Typical gool cost on a modern desktop:

| Subsystem | Cost per frame |
|---|---|
| `Tick()` (control thread) | ~0.1 ms |
| `Update()` (decoder pump + telemetry) | ~0.2 ms |
| Render thread (off the game thread, doesn't compete) | not in your frame budget |

Total: ~0.3 ms of your game thread per frame in a typical
configuration. The Master FX chain (~0.5% CPU) runs on the render
thread, not the game thread, so it doesn't count against `_process`
time.

Profiling tip: Godot's built-in profiler (Debug menu → Profiler)
breaks down `_process` time per script. The `Gool` autoload's
`_process` will show up as a single line item — that's gool's total
game-thread cost. If that line is hot (>1 ms), open an issue with
your config and gool will help you find the culprit.

---

## Editor plugin (mixer dock, inspector panels)

gool ships an editor plugin at `res://addons/gool/plugin.gd` that
registers custom prefab types, the mixer dock, and inspector panels
for gool's Resource types (`GoolMaterialEqPreset`,
`GoolAcousticProfile`, `GoolMasterFxPreset`, etc.).

For the plugin to function:

- It must be **enabled** in Project Settings → Plugins. The
  installer / quickstart enables it for you; if you import
  someone's gool-using project and gool seems inert, check this
  first.
- The plugin uses `EditorInterface` APIs that may change between
  Godot versions. gool tests against Godot 4.2, 4.3, and 4.4 stable;
  newer versions may surface deprecation warnings but should
  function until a hard break.

If you write your own editor plugins that interact with gool's
data (e.g. a custom dock that visualizes voice chat latency), the
gool autoload is **not** available in editor context. Your plugin
needs to either:

- Use `EditorInterface.get_editor_main_screen()` style APIs to
  introspect gool's resources without invoking the runtime.
- Run during play mode only (check `EditorInterface.is_playing_scene()`).

---

## Common pitfalls

A non-exhaustive list of "wait, why doesn't this work" cases:

**"My `AudioEmitter3D` shows in the editor but is silent at runtime."**
Check: is the `Gool` autoload enabled in **Project Settings →
AutoLoad**? The quickstart's one-line installer adds it; manual
installs sometimes miss it. The autoload name must be exactly
`Gool` (case-sensitive).

**"I edited `res://gool/config.json` and Godot's editor doesn't see the changes."**
Godot re-reads `config.json` when the autoload (re-)initializes,
which happens at scene F5, not when the editor scans the file.
Close and re-open the scene, or restart the editor, to pick up
config changes.

**"Volume goes to zero on web export when I switch tabs and come back."**
Browser audio context suspends when the tab loses focus and
Godot doesn't always restart it on regain. Workaround: a short
"silent play" of any sound on focus regain (`window.focus`
listener via JavaScript bridge) usually wakes the context.

**"My `VoiceChatPlayer` works in the editor but not in exported builds."**
Almost always: the export preset is missing the microphone permission
(Android: `RECORD_AUDIO`, iOS: `NSMicrophoneUsageDescription`, macOS:
`com.apple.security.device.audio-input`). gool's voice chat needs
microphone permission to capture.

**"Calling `Gool.set_bus_volume_db(...)` from a thread crashes."**
gool doesn't take the same Godot scene-tree thread protection
automatically. Use `call_deferred`:
```gdscript
Gool.set_bus_volume_db.call_deferred("Master", -6.0)
```

---

## Related reading

- [`godot_quickstart.md`](godot_quickstart.md) — install gool, play
  your first 3D sound. Start here.
- [`THREADING.md`](THREADING.md) — gool's internal thread model.
- [`performance_budgets.md`](performance_budgets.md) — gool's CPU
  budget per subsystem.
- [`cookbook.md`](cookbook.md) — recipe-style how-tos for common
  gool patterns.
- Godot official docs:
  [Audio buses](https://docs.godotengine.org/en/stable/tutorials/audio/audio_buses.html),
  [Thread-safe APIs](https://docs.godotengine.org/en/stable/tutorials/performance/thread_safe_apis.html),
  [Exporting for the Web](https://docs.godotengine.org/en/stable/tutorials/export/exporting_for_web.html),
  [Feature tags](https://docs.godotengine.org/en/stable/tutorials/export/feature_tags.html).
