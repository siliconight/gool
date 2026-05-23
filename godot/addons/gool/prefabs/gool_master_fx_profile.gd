# addons/gool/prefabs/gool_master_fx_profile.gd
#
# v0.63.0 — Phase 7 Master FX Lite profile prefab.
#
# Drop into the scene tree, pick a GoolMasterFxPreset, F5 — the
# master bus chain is reconfigured at scene load. Five built-in
# presets ship with gool at res://addons/gool/master_fx_presets/:
#
#   - None / bypass        — chain installed but all stages off.
#                              For A/B comparing your raw mix.
#   - Subtle glue          — conservative, -18 LUFS, light limiter.
#   - Standard FPS         — the v0.63.0 default that ships with
#                              fresh projects. -16 LUFS, -1 dBTP.
#   - Loud and aggressive  — competitive shooter loudness target.
#                              -14 LUFS, harder compression.
#   - Cinema/quiet         — story-driven, -22 LUFS, light touch.
#
# User presets live at res://gool/master_fx_presets/ and appear
# in the Resource picker alongside the built-ins.
#
# How this differs from GoolSceneProfile (v0.62.0):
#
#   GoolSceneProfile targets the REVERB bus and configures the
#   space's reverb characteristics. ReverbZones override per-room.
#
#   GoolMasterFxProfile targets the MASTER bus and configures the
#   loudness / clip-protection chain. Applies to the entire mix.
#
# These are orthogonal — most multiplayer FPS projects will have
# both: one GoolMasterFxProfile in the persistent game scene to
# set master-bus character, plus one GoolSceneProfile per level
# scene to set per-level reverb.

@tool
class_name GoolMasterFxProfile
extends Node


## The master-FX preset to apply. Use one of the built-in presets
## shipping with gool, or one of your project's saved presets.
##
## When null, the prefab is inert — useful while a designer is
## setting up a scene and hasn't picked a preset yet.
@export var preset: GoolMasterFxPreset = null:
	set(value):
		preset = value
		if Engine.is_editor_hint():
			update_configuration_warnings()


## Whether to apply the preset automatically at _ready. Default
## true matches the "drop it in and forget" workflow. Set false
## for script-driven control (e.g. a level-change cinematic that
## switches mastering profiles mid-game).
@export var apply_on_ready: bool = true


## Name of the bus carrying the MasterControl effect. Default
## "Master" matches the gool stock bus topology — the v0.63.0
## DEFAULT_CONFIG ships master_control on the Master bus. Change
## if your project routes mastering through a different bus.
@export var bus_name: String = "Master"


# --- Internal state ---

var _runtime: Node = null
var _effect_index: int = -1


# Engine EffectParameter IDs for MasterControl. Match values in
# audio::EffectParameter:: (bus.h on the C++ side). v0.63.0 owns
# the 30-58 block.
const _PARAM_GLUE_ENABLED:           int = 30
const _PARAM_RIDER_ENABLED:          int = 31
const _PARAM_LIMITER_ENABLED:        int = 32
const _PARAM_GLUE_THRESHOLD_DB:      int = 33
const _PARAM_GLUE_RATIO:             int = 34
const _PARAM_GLUE_ATTACK_MS:         int = 35
const _PARAM_GLUE_RELEASE_MS:        int = 36
const _PARAM_GLUE_KNEE_DB:           int = 37
const _PARAM_GLUE_MAKEUP_DB:         int = 38
const _PARAM_RIDER_TARGET_LUFS:      int = 39
const _PARAM_RIDER_TIME_CONST_MS:    int = 40
const _PARAM_RIDER_MAX_GAIN_DB:      int = 41
const _PARAM_RIDER_MIN_GAIN_DB:      int = 42
const _PARAM_RIDER_FREEZE_BELOW_LUFS: int = 43
const _PARAM_LIMITER_CEILING_DBTP:   int = 44
const _PARAM_LIMITER_RELEASE_MS:     int = 45
const _PARAM_LIMITER_LOOKAHEAD_MS:   int = 46


# Suppress repeated warnings about a misconfigured bus across all
# GoolMasterFxProfile instances in the session.
static var _warned_missing_buses: Dictionary = {}


func _ready() -> void:
	if Engine.is_editor_hint():
		return

	_runtime = get_node_or_null("/root/Gool")
	if _runtime == null:
		push_warning("GoolMasterFxProfile: /root/Gool autoload not "
				+ "found. The gool plugin is installed but not "
				+ "enabled. Fix: open Project Settings → Plugins, "
				+ "find 'gool' in the list, tick the Enable "
				+ "checkbox.")
		return

	if preset == null:
		# Inert mode — designer is mid-setup.
		return

	_locate_master_control_effect()
	if _effect_index < 0:
		return  # _locate already warned

	if apply_on_ready:
		apply()


## Apply the preset's settings to the MasterControl effect on the
## configured bus. Called automatically at _ready if
## apply_on_ready is true; call manually from script otherwise.
##
## Idempotent — calling apply() twice just rewrites the same
## values. The engine ramps internally for click-free transitions.
##
## No-op (with a push_warning) if /root/Gool is unreachable, if
## the preset is null, or if the bus lacks a MasterControl effect.
func apply() -> void:
	if _runtime == null:
		_runtime = get_node_or_null("/root/Gool")
		if _runtime == null:
			push_warning("GoolMasterFxProfile.apply: /root/Gool "
					+ "autoload not reachable. Enable the gool "
					+ "plugin in Project Settings → Plugins.")
			return

	if preset == null:
		push_warning("GoolMasterFxProfile.apply: no preset assigned.")
		return

	if _effect_index < 0:
		_locate_master_control_effect()
		if _effect_index < 0:
			return  # _locate already warned

	# Push all 17 config params. set_effect_parameter takes a
	# float; bools convert to 1.0/0.0 per the engine's enable-flag
	# convention.
	#
	# Note: this local must NOT be named `set` — `set` is a
	# reserved keyword in GDScript 2.0 (property setter syntax).
	# gdparse rejects it as an identifier even though some Godot
	# 4.x runtimes are lenient.
	var set_param := func(param_id: int, value: float) -> void:
		_runtime.set_effect_parameter(bus_name, _effect_index,
				param_id, value)

	set_param.call(_PARAM_GLUE_ENABLED,    1.0 if preset.glue_enabled    else 0.0)
	set_param.call(_PARAM_RIDER_ENABLED,   1.0 if preset.rider_enabled   else 0.0)
	set_param.call(_PARAM_LIMITER_ENABLED, 1.0 if preset.limiter_enabled else 0.0)
	set_param.call(_PARAM_GLUE_THRESHOLD_DB,     preset.glue_threshold_db)
	set_param.call(_PARAM_GLUE_RATIO,            preset.glue_ratio)
	set_param.call(_PARAM_GLUE_ATTACK_MS,        preset.glue_attack_ms)
	set_param.call(_PARAM_GLUE_RELEASE_MS,       preset.glue_release_ms)
	set_param.call(_PARAM_GLUE_KNEE_DB,          preset.glue_knee_db)
	set_param.call(_PARAM_GLUE_MAKEUP_DB,        preset.glue_makeup_db)
	set_param.call(_PARAM_RIDER_TARGET_LUFS,     preset.rider_target_lufs)
	set_param.call(_PARAM_RIDER_TIME_CONST_MS,   preset.rider_time_constant_ms)
	set_param.call(_PARAM_RIDER_MAX_GAIN_DB,     preset.rider_max_gain_db)
	set_param.call(_PARAM_RIDER_MIN_GAIN_DB,     preset.rider_min_gain_db)
	set_param.call(_PARAM_RIDER_FREEZE_BELOW_LUFS, preset.rider_freeze_below_lufs)
	set_param.call(_PARAM_LIMITER_CEILING_DBTP,  preset.limiter_ceiling_dbtp)
	set_param.call(_PARAM_LIMITER_RELEASE_MS,    preset.limiter_release_ms)
	set_param.call(_PARAM_LIMITER_LOOKAHEAD_MS,  preset.limiter_lookahead_ms)


# Locate the MasterControl-kind effect on the configured bus.
# Caches the effect's index in _effect_index for apply() reuse.
# Mirrors GoolSceneProfile._locate_reverb_effect — same fallback
# (warn once per session per bus name on missing, then go inert).
func _locate_master_control_effect() -> void:
	var effects: Array = _runtime.get_bus_effects(bus_name)
	if effects.is_empty():
		if not _warned_missing_buses.has(bus_name):
			_warned_missing_buses[bus_name] = true
			push_warning("GoolMasterFxProfile: bus '%s' has no "
					% bus_name
					+ "effects (or doesn't exist). Profile inert. "
					+ "Add a master_control effect to the bus in "
					+ "your gool config, or set `bus_name` to a "
					+ "bus that has one. (Further warnings for "
					+ "this bus suppressed.)")
		return

	for i in range(effects.size()):
		var e: Dictionary = effects[i]
		var kind_name: String = String(e.get("kind_name", ""))
		if kind_name == "MasterControl":
			_effect_index = i
			return

	# Bus exists but has no MasterControl effect.
	if not _warned_missing_buses.has(bus_name):
		_warned_missing_buses[bus_name] = true
		push_warning("GoolMasterFxProfile: bus '%s' has no "
				% bus_name
				+ "MasterControl effect. Profile inert. Add one "
				+ "to your gool config. (Further warnings for "
				+ "this bus suppressed.)")


func _get_configuration_warnings() -> PackedStringArray:
	var warnings: PackedStringArray = []
	if preset == null:
		warnings.append("No GoolMasterFxPreset assigned. Pick "
				+ "one from the Inspector dropdown.")
	return warnings
