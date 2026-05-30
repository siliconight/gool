# gool addon — bundled fonts

This directory ships the **Ubuntu Font Family** alongside the gool
Godot addon. Bundling a known font makes gool's runtime UI
(`gool_debug_overlay.gd`, `gool_audio_settings_panel.gd`, future
HUD-style prefabs) look identical across hosts regardless of what
the user has installed.

## What's here

Four weights, the practical minimum that covers headers, body
prose, emphasis, and bold callouts:

| File | Use |
|------|-----|
| `Ubuntu-Regular.ttf` | Default body text. Used by `gool_debug_overlay.gd` (the in-game stats HUD) and `gool_audio_settings_panel.gd` (the runtime settings panel). |
| `Ubuntu-Medium.ttf` | Slightly heavier weight for headers / labels above values. Bundled for future prefabs and per-project use; not yet referenced by code as of v0.59.1. |
| `Ubuntu-Bold.ttf` | True bold, for strong emphasis (e.g. "WARNING:" prefixes, peak indicators when clipping). Bundled for future use. |
| `Ubuntu-Italic.ttf` | Italic for hint text and placeholder values. Bundled for future use. |

We deliberately did **not** ship all eight weights from the
upstream Ubuntu Font Family. Light, LightItalic, BoldItalic, and
MediumItalic each add ~280-360 KB and aren't needed by anything
gool's UI currently does. Add them per-project if a custom HUD
needs them.

## License

The fonts are distributed under the **Ubuntu Font Licence (UFL)
version 1.0**, included as `UFL.txt`. The UFL permits embedding,
bundling, and redistribution of the fonts and their derivatives,
subject to the licence's conditions. Crucially:

- The licence applies to the **fonts only** — it does not extend
  to documents or software that merely **use** the fonts to render
  text. The gool addon itself remains Apache-2.0; the fonts in
  this directory are UFL-1.0.
- If you modify and redistribute the .ttf files themselves, the
  derivatives must remain under UFL-1.0 and must be renamed
  (see UFL §RESERVED FONT NAME).
- If you ship the gool addon (which is the only thing most users
  do), you must continue to include `UFL.txt` somewhere accessible
  alongside the .ttf files. Keeping them all in this directory
  satisfies that.

For the full licence text and the precise conditions, read
[`UFL.txt`](UFL.txt).

## Why this directory exists (history)

Before v0.59.1, the debug overlay used `ThemeDB.fallback_font` —
Godot's built-in default. That worked but it meant the gool HUD's
typography drifted with whatever Godot's bundled font happened to
be in any given engine version, and screenshots from different
hosts didn't match each other. Bundling Ubuntu pins the visual
appearance and lets us standardize the documentation
(README screenshots, video captures, the consultant deck) on
known typography. The size cost (~1.2 MB) is small relative to
the gool addon's overall footprint and is a fixed one-time cost
rather than a per-feature one.
