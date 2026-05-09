# Quickstart

The 60-second demo. Open this folder in **Godot 4.2+**, press **Play**,
hear all five gool features at once.

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
   register/submit path.
5. **Reverb zone** — the listener drifts in and out of the
   sphere-shaped reverb area. The zone fires
   `listener_entered`/`listener_exited` signals; the actual
   reverb mix change is wired into the engine bus graph.

## Prerequisites

- **Godot 4.2 or newer**
- The `gool_godot` GDExtension built and copied to
  `examples/quickstart/addons/gool/bin/`. Build instructions at
  `godot/README.md` in the engine repo.

If the extension binary isn't present, the project still opens
but you'll see "GoolAudioRuntime class not registered" errors in
the console — that's the GDExtension load failing because the
shared library isn't where Godot expects it.

## Notes

- All sounds are synthesized procedurally on `_ready()`. Zero
  asset dependencies; the entire demo lives in `main.gd` plus the
  prefab scripts.
- The demo runs at the engine's default 48 kHz / 512-frame buffer.
  If your audio device doesn't support that, tweak
  `gool/config.json` (auto-generated when the plugin enables).
- The reverb-zone parameter changes are tracked but not yet
  audible; the final hookup to `SetBusParameter` lands when the
  binding adds bus-parameter exposure (planned for v0.2).
