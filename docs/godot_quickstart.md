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

In v0.23.0+, gool's plugin scaffolds the audio folder structure
and creates a default sound bank for you the moment you enable the
plugin. You'll see a `sounds/` directory in your project root with
five subfolders (`sfx/`, `music/`, `voice/`, `ambience/`, `ui/`)
and a `bank.tres` resource at the root.

The fastest path to verifying gool works in your project:

### Option A: Open the bundled quickstart scene (10 seconds)

The addon ships a pre-configured test scene that proves gool can
produce sound end-to-end.

1. **Project → Tools → Gool → Open quickstart_3d.tscn (verify gool works)**
2. Press **F5**.
3. You should hear a short 440Hz beep within ~1 second.

If you hear it, gool is fully working in your project — emitter,
listener, audio backend, and bus routing all functional. Move on
to Option B for your own scene.

If you don't hear it, see the **Troubleshooting** section below
— the v0.22.4+ diagnostic logging will tell you exactly which
stage failed.

### Option B: Wire gool into your own scene

This is the workflow you'll use for every real scene from now on:

**1. Drop a sound file into `res://sounds/sfx/`.**
   - Any WAV is the most reliable (always imports).
   - Ogg Vorbis, MP3, FLAC also work.
   - **Logic Pro X users:** bounce as WAV, not as Ogg — Logic's
     Ogg export defaults to Opus, which Godot can't import. See
     the [README format-support section](../README.md#audio-file-format-support)
     for details and an ffmpeg conversion command.

Godot will reimport the file automatically and the
`GoolFolderSoundBank` (`res://sounds/bank.tres`) picks it up via
the live filesystem watch — you'll see:

```
[GoolFolderSoundBank] folder rescanned: 0 → 1 sounds
```

in the Output panel.

**2. Open or create a 3D scene** (Scene → New Scene → 3D Scene).

**3. Project → Tools → Gool → Add gool 3D audio scaffolding to current scene.**

This inserts three nodes under your scene root:
- `GoolListener3D` — the "ears" for spatialized audio
- `GoolSoundBankLoader` — registers sounds from `bank.tres` at
  runtime, pre-assigned for you
- `AudioEmitter3D` — a placeholder you'll configure next

You'll get a confirmation dialog with the list of nodes added and
next steps.

**4. Configure the AudioEmitter3D:**
- Select the `AudioEmitter3D` node in the Scene tree
- In the Inspector, click the **`Sound Name`** field — your dropped
  audio file appears in the dropdown (under `sfx/`)
- Pick it
- **Check `Autoplay → On`** ← easy to miss; emitter won't play without it
- Save the scene (Ctrl+S)

**5. Press F5.** You hear the sound.

If you want to play sounds programmatically instead of on autoplay
(e.g. gunshots fired from script), leave Autoplay off and call
`$AudioEmitter3D.play()` from your code on the appropriate event.

### How to know it's working

The Output panel should show, in order:

```
[gool] ready: version=0.23.0 rate=48000Hz buffer=512 buses=7 config=res://gool/config.json
[gool] audio device: WASAPI / <your audio device>
[GoolSoundBankLoader] registered N/M sounds from res://sounds/bank.tres: [sfx/yourfile, ...]
[AudioEmitter3D 'AudioEmitter3D'] play: sound='sfx/yourfile' pos=(0, 0, 0) looping=false
[gool] render: cb=188 (Δ188) frames=96256 (Δ96256) peak=0.4521 mixer_peak=0.4521 voices=1 gain=1.00 exc=0
```

The last line — the periodic `[gool] render:` health check — is
the decisive proof that audio is reaching your speakers. `peak`
is non-zero whenever something is audibly playing. If `peak` stays
at 0.0 even while you expect sound, that's the diagnostic signal
to investigate (see **Troubleshooting**).

## Step 4 — Where to go next

You've got the plumbing working. Now you can:

- **Add SFX.** One AudioEmitter3D per sound source (gunfire,
  footsteps, item pickups). For triggered SFX, leave Autoplay
  off and call `.play()` from your gameplay scripts.
- **Add adaptive music.** Drop a `MusicStateController`, call
  `add_state("explore", "music_calm", 1500.0)` and
  `add_state("combat", "music_intense", 600.0)` for two tracks,
  call `set_state("combat")` from your gameplay code — it
  equal-power crossfades.
- **Add voice chat.** Drag in a `VoiceChatPlayer` per remote
  peer, set its `player_id` to that peer's network id, and feed
  it Opus packets from your networking RPC. The prefab handles
  registration, jitter buffering, and decode.
- **Add networked SFX.** `Gool.play_networked(name, position)`
  fires a sound locally AND broadcasts to all peers via Godot's
  MultiplayerAPI (v0.22.0+).
- **Read the cookbook.** [`docs/cookbook.md`](cookbook.md) has 10
  one-screen recipes for the most common things: persistent
  emitters, mute a player, fade music, ducking SFX during
  dialogue, custom occlusion, etc.
- **Try the demo scenes.** Clone the gool repo, drop the addon
  into `examples/quickstart/` or `examples/coop_shooter_template/`,
  open in Godot, press Play. Reference scenes you can pick apart.

## Troubleshooting

**No sound when I press F5.** Look at the Output panel for the
`[gool] render:` line that appears every 2 seconds. The values
on that line tell you which stage is broken:

| Output | Diagnosis | Fix |
|---|---|---|
| No `[gool] ready:` line at all | Plugin didn't load or autoload didn't init | Verify Project Settings → Plugins shows gool as Enabled. Look for any red errors above. |
| `[gool] audio device:` shows the wrong device | Windows is routing audio to a device other than where you're listening (e.g., HDMI monitor instead of headphones) | Windows Sound Settings → Output → select the correct device. Restart Godot. |
| `DEAD AIR (no active voices)` | Emitter didn't actually wire into the mixer | Check `sound_name` is set on the AudioEmitter3D, OR `stream` is dragged in. Verify `Autoplay` is ✓ ticked. |
| `DEAD AIR (mixer silent, N voices active)` | Voices are mixing but producing silence | Check that the audio file actually contains audio (open it in Godot's Audio Preview). For Logic Pro X exports, see the Opus-in-Ogg note below. |
| `DEAD AIR (master gain = 0)` | Your config has Master at -∞ dB | Edit `res://gool/config.json` — set Master's `gain_db` back to 0. |
| All values look healthy but you hear nothing | Audio is reaching the device but your Windows app volume is muted | Right-click speaker icon → Volume Mixer → ensure Godot's app volume isn't at 0. |

**"AudioStreamPlayer placebo".** If you're testing and added a
Godot-native AudioStreamPlayer alongside the AudioEmitter3D, the
native player can mask whether gool itself is producing sound.
**Test gool in isolation:** delete or disable the AudioStreamPlayer,
press F5, look for the `[gool] render:` peak. If `peak > 0`, gool
is working; if `peak == 0`, something earlier in the chain failed.

**Logic Pro X bouncing as Opus instead of Vorbis.** Logic's "Ogg"
export option uses the Opus codec by default — Godot has no Opus
importer. Either bounce as WAV (always works) or run the file
through ffmpeg to convert to real Ogg Vorbis:

```bash
ffmpeg -i input.wav -c:a libvorbis -q:a 6 output.ogg
```

`GoolFolderSoundBank` (v0.22.4+) detects Opus-in-Ogg files and
emits a specific warning naming this fix.

**Mixed indentation parser error after editing addon files.**
If you've modified files inside `addons/gool/` in Godot's script
editor, Godot may have inserted tabs where the original file used
spaces (or vice versa), producing
`Parser Error: Used tab character for indentation instead of space`.
Fix: re-run `gool-install.cmd` with Godot fully closed; this
restores the addon files to their pristine v0.23.0 state.

**"GoolAudioRuntime class not registered" in the Output panel.**
The C++ binary inside `addons/gool/bin/` is missing or doesn't
match your OS. Re-download the archive for *your* OS — Linux
binaries won't load on Windows, etc. The full error message in
the Output panel walks you through the fix.

**Plugin doesn't appear in Project Settings → Plugins.** The
`addons/gool/plugin.cfg` file isn't where Godot expects it. Path
should be `<your-project>/addons/gool/plugin.cfg`. If you
extracted the archive into the wrong place, move it.

## A note on Godot versions

gool's GDExtension is built against godot-cpp 4.4 (the highest
stable godot-cpp branch as of May 2026) and tested against
Godot 4.6.2. Per GDExtension's forward-compatibility contract,
it loads cleanly into Godot 4.4 through 4.6.x. When godot-cpp's
4.5 or 4.6 stable branches eventually ship, we'll bump in
lockstep. If you're on a development build of Godot or a very
new minor we haven't released a binary for yet, building from
source (see [`SETUP.md`](../SETUP.md)) is the fallback.

## Advanced: registering sounds at runtime (no file)

The Option B workflow above uses Godot's import pipeline — drop
a file, the bank loader registers it, the emitter looks it up.
You can also register sounds entirely from script, useful for:

- **Procedural audio**: synthesize a tone, register the bytes,
  play it.
- **Network-streamed audio**: download bytes from a CDN at
  runtime, register them without a `.tres` file.
- **Custom pack formats**: load from your own asset bundle
  format and feed raw PCM to gool.

```gdscript
extends Node3D

func _ready() -> void:
    var Gool = get_node("/root/Gool")
    if not Gool.is_initialized():
        await Gool.ready_to_play

    # Synthesize a 0.5-second 440 Hz tone, register under a name.
    var sample_rate := 48000
    var n := sample_rate / 2  # 0.5s
    var samples := PackedFloat32Array()
    samples.resize(n)
    for i in range(n):
        samples[i] = 0.4 * sin(TAU * 440.0 * i / sample_rate)
    Gool.register_pcm_sound("synth_beep", samples, sample_rate, 1)
```

Once registered, any AudioEmitter3D referencing `synth_beep` by
name will play it the same way as a file-loaded sound. Mix and
match: most sounds come from files via the bank; the rare
runtime-generated ones go through this API.
