## v0.37.0 (Phase 4): player audio settings persistence helper.
##
## Bridges three layers:
##   1. Engine state — the Gool autoload's runtime values
##      (master_volume_db, bus gains, occlusion settings, EQ
##      intensity, ...)
##   2. On-disk save — a ConfigFile at user://gool_audio_settings.cfg
##      that survives between sessions
##   3. UI state — sliders and toggles in the settings menu
##
## The static methods here implement the two flows between these
## layers:
##
##   on game start                  on user changes a control
##   ┌──────────────┐               ┌─────────────────┐
##   │ load from    │               │ apply to engine │
##   │ disk         │ ┐             │ (immediate)     │
##   └──────┬───────┘ │             └────────┬────────┘
##          ↓         │                      ↓
##   ┌──────────────┐ │             ┌─────────────────┐
##   │ apply to     │ │             │ save to disk    │
##   │ engine       │ │             │ (debounced)     │
##   └──────────────┘ │             └─────────────────┘
##                    │
##   (UI reads what's now in the runtime to set its sliders)
##
## Designers can use either the reference prefab
## (GoolAudioSettingsPanel) which wires all of this for you,
## OR call the static methods here directly from a custom UI.
##
## The on-disk save sits ON TOP of project settings. Project
## settings provide design-time defaults (gool/occlusion/intensity,
## gool/material_eq/intensity, etc.); user saves override at
## runtime per-player. If no save file exists yet, project
## settings are what the menu shows initially.
##
## What this DOESN'T cover (deliberately, for v0.37.0 scope):
##   - Audio device selection (output picker)
##   - Surround / headphones toggles
##   - Subtitles / language
##   - Per-channel mute (just gain sliders)
##   - Hearing accessibility presets (e.g., "boost speech")
## Add as needed for your project; the persistence pattern below
## scales to any number of additional sections.

class_name GoolAudioSettings
extends RefCounted

const _SAVE_PATH := "user://gool_audio_settings.cfg"

## Hard-coded factory defaults. Used when no save file exists
## OR when reset_to_defaults() is called. These represent
## "unity / full / on" across the board — most projects will
## want their *design-time* defaults to live in Project Settings
## (so they appear in the editor UI), and have these fallbacks
## only fire on completely fresh installs.
const DEFAULTS := {
	"volumes": {
		"master_db":   0.0,
		"sfx_db":      0.0,
		"music_db":    0.0,
		"ui_db":       0.0,
		"voice_db":    0.0,
		"dialogue_db": 0.0,
		"ambience_db": 0.0,
	},
	"occlusion": {
		"enabled":   true,
		"intensity": 1.0,
	},
	"material_eq": {
		"intensity": 1.0,
	},
}

## Per-bus volume keys → standard category bus names. Override
## this if your project uses different bus names than the gool
## defaults. Used by apply_to_runtime() to push each volume slider
## value to the corresponding bus via Gool.set_bus_gain_db().
const _VOLUME_KEY_TO_BUS := {
	"sfx_db":      "Sfx",
	"music_db":    "Music",
	"ui_db":       "UI",
	"voice_db":    "Voice",
	"dialogue_db": "Dialogue",
	"ambience_db": "Ambience",
}

## Read settings from disk. Returns a Dictionary with the same
## shape as DEFAULTS. Missing sections or keys fall back to
## DEFAULTS values — so partial saves from older versions of the
## menu still work and just inherit the new defaults for any
## new fields.
##
## If the save file doesn't exist or is malformed, returns
## DEFAULTS verbatim. No exceptions are raised — the menu should
## always have *something* to show.
static func load_from_disk() -> Dictionary:
	var result := DEFAULTS.duplicate(true)
	var cfg := ConfigFile.new()
	var err := cfg.load(_SAVE_PATH)
	if err != OK:
		# Not an error condition — just means this is a first run
		# or the file was deleted. Defaults are correct.
		return result
	for section in result.keys():
		for key in result[section].keys():
			if cfg.has_section_key(section, key):
				result[section][key] = cfg.get_value(section, key)
	return result

## Write the in-memory settings dictionary to disk. Returns OK
## on success, or the Godot Error code that ConfigFile.save
## reported (typically ERR_FILE_CANT_WRITE if the user:// dir
## isn't writable, which only happens in unusual sandbox
## configurations).
##
## Designers using the panel prefab don't need to call this
## directly — the panel auto-saves on slider release. Call it
## yourself only if building a custom UI.
static func save_to_disk(settings: Dictionary) -> int:
	var cfg := ConfigFile.new()
	for section in settings.keys():
		for key in settings[section].keys():
			cfg.set_value(section, key, settings[section][key])
	var err := cfg.save(_SAVE_PATH)
	if err != OK:
		push_warning("[gool] GoolAudioSettings.save_to_disk: ConfigFile.save returned err=%d" % err)
	return err

## Push every value in the settings dictionary to the Gool
## autoload's runtime. After this returns, the player hears
## whatever the settings dictionary describes.
##
## Skips missing buses silently — if your project doesn't have
## a "Dialogue" bus, the dialogue_db value just isn't applied.
## No warning, no error.
##
## Awaits Gool.ready_to_play if the autoload isn't initialized
## yet. Safe to call from a menu's _ready() — it will block on
## the first call until the engine is up.
static func apply_to_runtime(settings: Dictionary) -> void:
	if not Gool.is_initialized():
		await Gool.ready_to_play

	var volumes: Dictionary = settings.get("volumes", {})
	if volumes.has("master_db"):
		Gool.set_master_volume_db(float(volumes.master_db))
	for key in _VOLUME_KEY_TO_BUS.keys():
		if not volumes.has(key):
			continue
		var bus_name: String = _VOLUME_KEY_TO_BUS[key]
		# Existence check — set_bus_gain_db on a non-existent bus
		# is a silent no-op engine-side, but checking here lets us
		# skip the call entirely for projects that don't use the
		# full standard bus set.
		if Gool.find_bus_id_by_name(bus_name) < 0:
			continue
		Gool.set_bus_gain_db(bus_name, float(volumes[key]))

	var occlusion: Dictionary = settings.get("occlusion", {})
	if occlusion.has("enabled"):
		Gool.set_occlusion_enabled(bool(occlusion.enabled))
	if occlusion.has("intensity"):
		Gool.set_occlusion_intensity(float(occlusion.intensity))

	var material_eq: Dictionary = settings.get("material_eq", {})
	if material_eq.has("intensity"):
		Gool.set_eq_intensity(float(material_eq.intensity))

## Convenience: load from disk, then immediately apply to runtime.
## The most common startup pattern — call this from your game's
## boot scene or main menu's _ready() and forget about it.
##
## Returns the loaded settings dictionary in case the caller
## needs to mirror it (e.g., to populate UI sliders without
## reading them back from the engine).
static func load_and_apply() -> Dictionary:
	var settings := load_from_disk()
	await apply_to_runtime(settings)
	return settings

## Restore factory defaults: write DEFAULTS to disk + apply to
## runtime. Returns the defaults dictionary so the caller can
## update any in-memory UI mirror.
##
## Used by the panel's "Reset to defaults" button. Does NOT
## reset to project setting values — it resets to the hard-coded
## DEFAULTS above. If you want "reset to project defaults"
## instead, write your own version that reads the project
## settings.
static func reset_to_defaults() -> Dictionary:
	var defaults := DEFAULTS.duplicate(true)
	save_to_disk(defaults)
	await apply_to_runtime(defaults)
	return defaults
