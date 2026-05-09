# Godot GDExtension binding for the audio engine

This directory contains a minimal-but-functional Godot 4 GDExtension
that exposes the core audio engine API to GDScript. It's the
fastest path for a Godot project to use the engine without writing
C++.

## What's exposed

The binding adds two Node classes:

- **`GoolAudioRuntime`**: the runtime singleton. Initialized from
  the project's autoload. Provides `register_pcm_sound`,
  `play_sound_at_location`, `set_listener`, etc.
- **`GoolMusicChannel`**: the music-crossfade helper, wrapping
  `audio::MusicChannel`. Exposes `play(sound_name, fade_ms)`,
  `stop(fade_ms)`, and a `current` property.

This is **not** a one-to-one wrapping of every engine API. It's the
"play sounds at positions, do music transitions, get voice telemetry"
surface that covers ~90% of indie game-audio integration work. The
remaining 10% (custom bus graphs, advanced spatializer config,
voice chat) gets called through directly from a small companion C++
module if your project needs it.

## Build

GDExtension builds as a shared library. From the engine repo root:

```bash
cmake -S godot -B build-godot \
    -DGODOT_CPP_PATH=/path/to/godot-cpp \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build-godot -j
```

Copy the resulting `gool_godot.{so,dylib,dll}` into your Godot
project's `addons/gool/bin/` directory along with the included
`gool.gdextension` manifest. Reload the project; the new node
classes appear in the editor.

## Status

This is a v0 binding: enough to play sounds and do music transitions
from GDScript. Roadmap:

- Sound bank loading from `.gpak` via the binding (currently host
  code does it before passing IDs into Godot).
- Voice chat exposure (RegisterVoiceSource + OnVoicePacket).
- AudioStreamPlayer3D-compatible drop-in (`Gool3DAudioPlayer` node
  with the same property surface as Godot's stock node, so existing
  scenes migrate by changing the node type).

If you're building a Godot multiplayer game today and want to use
the engine, the v0 binding plus a thin GDScript wrapper around
`GoolAudioRuntime` is enough to ship.

## Why GDExtension specifically (and not a Godot module)

GDExtension loads at runtime as a shared library — no Godot rebuild
required, no C++ knowledge needed by the Godot project itself.
Modules require recompiling Godot from source, which is a real
adoption barrier for the indie target audience.
