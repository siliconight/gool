# gool addon templates

Pre-configured Godot scenes you can open as-is to verify gool is
working in your project, or as starting points for your own scenes.

## `quickstart_3d.tscn`

The minimum viable gool scene. Contains a `GoolListener3D` and an
`AudioEmitter3D` set to autoplay a tiny test beep (`test_beep.wav`)
bundled in this folder. Open the scene, press F5, and you should
hear a 0.5-second 440 Hz beep.

If you hear it: gool is fully working in your project — emitter,
listener, audio backend, and bus routing all functional. Build
your real scene with confidence.

If you don't hear it: check the Output panel for `[gool]` warnings
or errors. The single most diagnostic thing is the `[gool] ready:`
line — if it's missing, the runtime didn't initialize.

## `test_beep.wav`

A tiny (22 KB) test audio file: 0.5 seconds, 440 Hz sine wave, mono,
22050 Hz, 16-bit PCM, with 20 ms fades at the start and end to avoid
click artifacts. Used by `quickstart_3d.tscn`.

You can use this file in your own scenes too — it's a useful
"reference tone" for testing bus routing, distance attenuation,
and other audio properties.

## `config_fps.json`

A drop-in working `gool/config.json` for FPS-shaped games. Copy
it into your project at `res://gool/config.json` and gool comes
up with a sensible starting topology: hierarchical buses
(Master / Music / Sfx / LocalSfx / RemoteSfx / Voice / Dialogue
/ Ambient), sidechain ducking wiring (Music ducks under LocalSfx
and Dialogue; RemoteSfx ducks under LocalSfx and Dialogue),
and reverb-shaping slots in the right places.

What's pre-wired worth knowing about:

- **Master bus has `master_control`** with the Standard FPS preset
  baked in (Glue+Limiter+Rider, -16 LUFS target, -1 dBTP ceiling).
  The v0.63.0 design-doc "Default" profile. Open it in the mixer
  dock to swap to another preset (Cinema Quiet, Subtle Glue, Loud
  & Aggressive, None/Bypass) or tweak parameters.
- **Sfx bus has the canonical `[biquad-HPF, reverb, biquad-LPF]`
  effect chain.** ReverbZone prefabs find the HPF slot via the
  index-immediately-before-Reverb convention and the LPF slot via
  the index-immediately-after — both `send_hpf_hz` and
  `return_lpf_hz` ramps work without touching the config.
- **Three sidechain compressors** showcasing the multi-tier ducking
  pattern: Music ducks under LocalSfx, Music ducks deeper under
  Dialogue, LocalSfx ducks under Dialogue, RemoteSfx ducks under
  both LocalSfx and Dialogue.

Category routing maps gameplay intent (music / sfx / voice /
ambience / ui / dialogue) to the right bus, so
`Gool.play_3d("rifle_fire", pos)` lands on LocalSfx and
`Gool.play_music_state("combat")` lands on Music without any
per-call routing.

## `dialogue_setup_example.json`

Companion JSON showing the `DialogueDirector` configuration
shape — voice lines, priority tiers, ducking targets. Reference
it when wiring up the dialogue prefabs; not loaded at runtime.

## Notes

Both files live in `addons/gool/templates/` and reference each
other via `res://addons/gool/templates/` absolute paths. You can:

- Open `quickstart_3d.tscn` directly to test
- Copy both files to your own project folders — paths will still
  resolve because the source addon is in `res://`
- Use the scene as a starting point: save-as into your own
  `scenes/` folder, then modify
