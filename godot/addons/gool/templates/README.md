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

## Notes

Both files live in `addons/gool/templates/` and reference each
other via `res://addons/gool/templates/` absolute paths. You can:

- Open `quickstart_3d.tscn` directly to test
- Copy both files to your own project folders — paths will still
  resolve because the source addon is in `res://`
- Use the scene as a starting point: save-as into your own
  `scenes/` folder, then modify
