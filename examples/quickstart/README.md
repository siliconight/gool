# Quickstart

The 60-second demo. Open this folder in **Godot 4.2+**, press **Play**,
hear all five gool features at once.

## Get the GDExtension binary

The demo needs `gool_godot`'s GDExtension binary in
`addons/gool/bin/`. Pick one of two paths.

### Path 1 — Download from Releases (recommended)

1. Open the [Releases page](https://github.com/siliconight/gool/releases).
2. Find the latest release. Download the asset matching your OS:
   - **Windows**: `gool-X.Y.Z-godot-addon-windows-x86_64.zip`
   - **macOS** (Apple Silicon): `gool-X.Y.Z-godot-addon-macos-arm64.tar.gz`
   - **Linux**: `gool-X.Y.Z-godot-addon-linux-x86_64.tar.gz`
3. Extract it. Inside is an `addons/gool/` folder.
4. Copy that whole `addons/gool/` folder over the one in
   `examples/quickstart/addons/`. (Overwriting is fine — the
   release zip includes the prefabs alongside the binary.)
5. Open `examples/quickstart` in Godot, press Play.

### Path 2 — Build from source

For contributors, custom platforms, or if your Godot version is
newer than the release supports:

```bash
git clone https://github.com/siliconight/gool.git && cd gool
./scripts/bootstrap.sh --install-to examples/quickstart
```

Windows: `scripts\bootstrap.ps1 -InstallTo examples\quickstart` from the
**x64 Native Tools Command Prompt for VS 2022**.

See [SETUP.md](../../SETUP.md) for per-platform prerequisites.

## What you hear

1. **Adaptive music** — a low warm pad ("explore" state) that
   crossfades to a brighter pad ("combat") every 6 seconds and
   back. Equal-power crossfade, 600-1500 ms transitions.
2. **Spatial audio** — a 220 Hz drone orbits the listener at 5 m
   radius. You hear it pan around the stereo field; if you move
   the listener (the camera at origin), distance attenuation
   kicks in.
3. **Footsteps** — short percussive clicks every 450 ms,
   randomized between three variants. Triggered by the
   `FootstepSurfacePlayer` prefab; surface detection uses raycast
   + group lookup.
4. **(Mock) voice chat** — registers a voice source for a
   synthetic player and submits 40-byte fake packets to exercise
   the engine's voice path. Real microphone capture is
   platform-specific and out of scope for the prefab demo;
   what's demonstrated is that the binding handles the
   register/submit path. Press **M** during play to toggle
   `voice.muted` and watch the `voice_frames_dropped_due_to_mute`
   counter rise — showcases v0.13.0's mute API.
5. **Reverb zone** — the listener drifts in and out of the
   sphere-shaped reverb area. The zone fires
   `listener_entered`/`listener_exited` signals; the actual
   reverb mix change is wired into the engine bus graph.

## Troubleshooting

**"GoolAudioRuntime class not registered" in the Output panel.**
The GDExtension binary isn't where Godot expects it. Verify
`addons/gool/bin/` contains a file matching your OS:
`libgool_godot.so` (Linux), `gool_godot.dll` (Windows), or
`libgool_godot.dylib` (macOS). If you downloaded from Releases,
make sure the archive matched your OS (Linux binaries won't load
in a Windows Godot, etc.).

**No sound but no errors.** Godot might have selected a different
output device. Check Project Settings → Audio → Driver, or
inspect the default device in your OS sound settings.

**"No audio device?" error on startup.** The engine couldn't open
the system audio device at 48 kHz / 512-frame buffer. Edit
`gool/config.json` (auto-generated on first run) and lower
`buffer_size` to 1024, or change `sample_rate` to match your
device's native rate.

## Notes

- All sounds are synthesized procedurally on `_ready()`. Zero
  asset dependencies; the entire demo lives in `main.gd` plus the
  prefab scripts.
- The demo runs at the engine's default 48 kHz / 512-frame buffer.
  If your audio device doesn't support that, tweak
  `gool/config.json` (auto-generated when the plugin enables).
- The reverb-zone parameter changes are tracked but not yet
  audible; the final hookup to `SetBusParameter` lands when the
  binding adds bus-parameter exposure (planned for a future release).

## Next steps

- **New to Godot?** Read [`docs/godot_quickstart.md`](../../docs/godot_quickstart.md)
  for a step-by-step from "I've never installed a Godot addon" to
  "I have spatial audio in my own project."
- **Looking for code snippets?** See [`docs/cookbook.md`](../../docs/cookbook.md)
  for 10 common recipes — play a sound at a position, mute a
  player, fade music, etc. Each one is under 10 lines.
- **Want to see all the features?** The main
  [`README.md`](../../README.md) has the full feature catalog
  with measured numbers.
