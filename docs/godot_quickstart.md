# Godot quickstart

A start-here guide for anyone who's never installed a Godot addon
before. By the end you'll have a 3D-positioned sound playing in
your own Godot 4.2+ project. No C++ compiler required.

## What is gool, in 30 seconds

gool is an audio engine that ships with seven prefab Nodes you
can drag into your scene like any other Godot node. Drag in an
`AudioEmitter3D`, type a sound name in the inspector, tick
**autoplay**, press F5 — you hear it positioned in 3D space.
Drag in a `VoiceChatPlayer`, give it a player id, push Opus
packets at it from your networking layer — voice chat works.

It's a C++ engine under the hood — that's why the bundle includes
a small compiled binary (`gool_godot.dll` / `.so` / `.dylib`) —
but you never touch C++. Everything you write is GDScript, same
as the rest of your game.

## What you need

- **Godot 4.2 or newer.** Get it from
  [godotengine.org](https://godotengine.org/download). The
  standard build is fine — you don't need the .NET version.
- **No compiler, no SDK, no extra tools.** The addon comes with
  a prebuilt binary for your OS.

## Step 1 — Get the addon

Two options. Pick whichever looks easier.

### Option A: Download from Releases (recommended)

1. Open [gool's Releases page](https://github.com/siliconight/gool/releases).
2. Find the latest release at the top.
3. Under **Assets**, click the archive matching your OS:
   - Windows → `gool-X.Y.Z-godot-addon-windows-x86_64.zip`
   - macOS → `gool-X.Y.Z-godot-addon-macos-arm64.tar.gz` (Apple Silicon)
   - Linux → `gool-X.Y.Z-godot-addon-linux-x86_64.tar.gz`
4. Extract the archive. You'll get a folder called `addons/`
   containing `gool/`.
5. Drag the whole `addons/gool/` folder into your Godot project's
   root, alongside `project.godot`. (If your project already has
   an `addons/` folder, drop `gool/` inside that one — same
   result.)

### Option B: One-line installer

If you don't want to download manually, gool ships an installer
script you can run from inside your project folder:

```powershell
# Windows PowerShell (in the project folder):
iwr -useb https://raw.githubusercontent.com/siliconight/gool/main/scripts/quickinstall.ps1 | iex
```

```bash
# Linux / macOS (in the project folder):
curl -sSL https://raw.githubusercontent.com/siliconight/gool/main/scripts/quickinstall.sh | bash
```

Either script downloads the right archive for your OS and unpacks
it for you. Same outcome as Option A.

## Step 2 — Enable the plugin

In Godot:

1. **Project** menu → **Project Settings…**
2. Click the **Plugins** tab (top-right area).
3. You should see **gool** in the list with status "Disabled".
   Tick the **Enable** checkbox.
4. Close the dialog.

What this just did, automatically:

- Added `Gool` to your project's autoloads (Project Settings →
  Autoload tab — you'll see it there).
- Registered seven custom Nodes you can find in the
  **Add Child Node** dialog: `AudioEmitter3D`, `VoiceChatPlayer`,
  `MusicStateController`, `ReverbZone`, `FootstepSurfacePlayer`,
  `NetworkedAudioEvent`, `NetworkedAudioEmitter3D`.
- Wrote a default config to `res://gool/config.json`. Sane
  defaults — leave it alone unless you need to tune buffer size
  or bus setup later.

If you don't see gool in the Plugins list, the addon folder isn't
where Godot expects it. Re-check that `addons/gool/plugin.cfg`
exists relative to your `project.godot`.

## Step 3 — Make your first sound

We'll play a positional sound in 3D space. Open any 3D scene
(create one if you don't have one — File → New Scene → 3D Scene).

1. Right-click the root node → **Add Child Node**.
2. In the search box type **AudioEmitter3D**. It's in the list.
3. Click **Create**.
4. In the Inspector (right panel), set:
   - **Sound Name**: `my_first_sound`
   - **Autoplay**: ✓ ticked
5. Save the scene.

You won't hear anything yet — Godot doesn't know what
`my_first_sound` is. We need to register the sound bytes with
gool before the emitter can play it. Attach a script to the
scene root and put this in it:

```gdscript
extends Node3D

func _ready() -> void:
    var Gool = get_node("/root/Gool")
    if not Gool.is_initialized():
        await Gool.ready_to_play

    # Synthesize a 0.5-second 440 Hz tone, register under our name.
    var sample_rate := 48000
    var n := sample_rate / 2  # 0.5s
    var samples := PackedFloat32Array()
    samples.resize(n)
    for i in range(n):
        samples[i] = 0.4 * sin(TAU * 440.0 * i / sample_rate)
    Gool.register_pcm_sound("my_first_sound", samples, sample_rate, 1)
```

Press F5. You hear a 440 Hz tone positioned wherever the
emitter is in your scene. Move the emitter in the editor, press
Play again — it'll pan and attenuate based on its world position
relative to the listener (which defaults to the origin).

## Step 4 — Where to go next

You've got the plumbing working. Now you can:

- **Use real sound files.** Instead of `register_pcm_sound`, call
  `Gool.register_sound_from_file("my_sound", "res://assets/blip.wav")`.
  WAV, OGG, and FLAC all work.
- **Add voice chat.** Drag in a `VoiceChatPlayer` per remote
  peer, set its `player_id` to that peer's network id, and feed
  it Opus packets from your networking RPC. The prefab handles
  registration, jitter buffering, and decode.
- **Add adaptive music.** Drag in a `MusicStateController`, call
  `add_state("explore", "music_calm", 1500.0)` and
  `add_state("combat", "music_intense", 600.0)` for two tracks,
  call `set_state("combat")` from your gameplay code — it
  equal-power crossfades.
- **Read the cookbook.** [`docs/cookbook.md`](cookbook.md) has 10
  one-screen recipes for the most common things: persistent
  emitters, mute a player, fade music, ducking SFX during
  dialogue, custom occlusion, etc.
- **Try the demo scene.** Clone the gool repo, drop the
  addon into `examples/quickstart/`, open it in Godot, press
  Play. All five prefabs running at once in 100 lines of
  GDScript — a good reference if you're stuck.

## Troubleshooting

**No sound when I press Play, no errors in the Output panel.**
Three things to check, in order:
1. Did you tick **Autoplay** on the AudioEmitter3D?
2. Did `register_pcm_sound` (or `register_sound_from_file`) run
   successfully? Check the Output panel — gool prints a warning
   if registration failed.
3. Is your OS audio output working? Other apps making sound?

**"GoolAudioRuntime class not registered" in red in the Output panel.**
The C++ binary inside `addons/gool/bin/` is missing or doesn't
match your OS. Re-download the archive for *your* OS — Linux
binaries won't load on Windows, etc. The full error message in
the Output panel walks you through the fix.

**"register_voice_source(N) failed" warning.**
Either the voice budget is full (default 16 — raise
`maxVoiceSources` in `res://gool/config.json`), the player_id is
already registered, or the runtime didn't initialize. The
warning lists which case to check.

**Plugin doesn't appear in Project Settings → Plugins.**
The `addons/gool/plugin.cfg` file isn't where Godot expects it.
Path should be `<your-project>/addons/gool/plugin.cfg`. If
you extracted the archive into the wrong place, move it.

## A note on Godot versions

gool's GDExtension is built against a specific Godot minor
version (4.2 currently). It will load in 4.2, 4.3, and 4.4
without issue. If a future 4.5 changes the GDExtension ABI,
you'd need a new binary — but that's rare; we'll publish updated
builds when it happens. If you're on a development build of Godot
or a very new minor we haven't released a binary for yet,
building from source (see [`SETUP.md`](../SETUP.md)) is the
fallback.
