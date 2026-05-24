# addons/gool/prefabs/gool_scene_profile.gd
#
## v0.62.0 — Phase 6.E.4 follow-up: scene-level acoustic profile prefab.
#
## The "drop me in, pick a profile, done" Node. Designer's workflow:
#
##   1. Add a GoolSceneProfile node to the scene root (or anywhere
##      in the tree — position doesn't matter, it's not spatial).
##   2. In the inspector, click the `profile` field and pick a
##      GoolAcousticProfile.tres — either one of the ~8 built-in
##      profiles shipped with gool (Tight corridor, Open atrium,
##      Wet cave, Dry forest, Industrial bunker, Suburban interior,
##      Underwater, Outdoor field) or a custom one the project has
##      saved under res://gool/acoustic_profiles/.
##   3. Press F5. The reverb bus is configured with the profile's
##      decay/damping/diffusion/predelay/wet at scene load. The
##      scene sounds like the profile.
#
## That's the whole workflow. ReverbZones still work on top — they
## override the reverb params on entry, restore them on exit. The
## "restore" target is whatever the GoolSceneProfile applied at
## scene load, so the layering is "scene profile = baseline, zones
## = sub-region overrides".
#
## Customization path (per v0.62.0 simplicity decision): a designer
## who wants to tune doesn't tweak the Node's fields. They open the
## profile .tres and tweak its fields; the Node re-applies on next
## scene load. Or they Save… a new profile.tres and assign it.
## Keeping the Node's surface minimal (just a profile reference)
## means there's exactly one source of truth for any given sound.
#
## Threading: apply() runs on the game thread (Node._ready) and
## pushes parameters through Gool.set_effect_parameter, which the
## engine ramps internally on the render thread (~5 ms ramp). No
## clicks at scene load.

@tool
class_name GoolSceneProfile
extends Node


## The acoustic profile to apply. Use one of the built-in profiles
## shipping with gool, or one of your project's saved profiles.
##
## When the field is null, the prefab is inert — useful while a
## designer is setting up a scene and hasn't picked a profile yet.
## A push_warning at _ready notes the empty assignment.
@export var profile: GoolAcousticProfile = null:
	set(value):
		profile = value
		# Editor-side: if the user assigns a profile while the
		# scene tree is open, the inspector rebuilds. No runtime
		# action needed; apply() fires at next _ready.
		if Engine.is_editor_hint():
			update_configuration_warnings()


## Whether to apply the profile automatically at _ready. Default true
## matches the "drop it in and forget" workflow. Set false if you
## want script-driven control over when the profile activates (e.g.
## a level-change cinematic that switches profiles mid-game).
@export var apply_on_ready: bool = true


## Name of the bus carrying the reverb effect. Default "Sfx"
## matches the gool stock bus topology where the Sfx bus has the
## reverb effect. Change to match your project's bus naming if
## different.
@export var bus_name: String = "Sfx"


# --- Internal state ---
#
# Cached on _ready so apply() doesn't have to walk the bus list
# every time it's called.

var _runtime: Node = null
var _effect_index: int = -1


# Engine EffectParameter IDs for Reverb. Match the values in
# audio::EffectParameter:: (bus.h on the C++ side). Mirrored from
# ReverbZone's _PARAM_* constants — keep in sync.
const _PARAM_DECAY:       int = 9
const _PARAM_HF_DAMPING:  int = 10
const _PARAM_WET_GAIN_DB: int = 11
const _PARAM_LF_DAMPING:  int = 24
const _PARAM_DIFFUSION:   int = 25
const _PARAM_PREDELAY_MS: int = 23


# Track whether we've warned about a missing bus this session, so
# multiple GoolSceneProfile instances (rare, but possible) don't
# spam push_warning when the project's reverb bus is misconfigured.
static var _warned_missing_buses: Dictionary = {}


func _ready() -> void:
	# Editor-side: don't try to apply at scene-build time. The
	# autoload isn't reachable in editor context anyway, and we'd
	# just be queueing up warnings about a missing /root/Gool.
	if Engine.is_editor_hint():
		return

	_runtime = get_node_or_null("/root/Gool")
	if _runtime == null:
		push_warning("GoolSceneProfile: /root/Gool autoload not found. "
				+ "The gool plugin is installed but not enabled. Fix: "
				+ "open Project Settings → Plugins, find 'gool' in the "
				+ "list, tick the Enable checkbox.")
		return

	if profile == null:
		# Inert mode — designer is mid-setup. Don't push a warning,
		# they'll see the empty field in the inspector and know.
		return

	_locate_reverb_effect()
	if _effect_index < 0:
		return  # _locate_reverb_effect already warned

	if apply_on_ready:
		apply()


## Apply the profile's reverb characteristics to the reverb bus.
## Called automatically at _ready if apply_on_ready is true; call
## manually from script if apply_on_ready is false.
##
## Idempotent — calling apply() twice just rewrites the same
## values to the engine. The engine's ~5 ms ramp handles the
## transition each time.
##
## No-op (with a push_warning) if /root/Gool is unreachable, if
## the profile is null, or if the reverb bus has no reverb effect.
func apply() -> void:
	if _runtime == null:
		_runtime = get_node_or_null("/root/Gool")
		if _runtime == null:
			push_warning("GoolSceneProfile.apply: /root/Gool autoload "
					+ "not reachable. Enable the gool plugin in "
					+ "Project Settings → Plugins.")
			return

	if profile == null:
		push_warning("GoolSceneProfile.apply: no profile assigned.")
		return

	if _effect_index < 0:
		_locate_reverb_effect()
		if _effect_index < 0:
			return  # _locate_reverb_effect already warned

	# Push the six reverb params. All take floats; engine clamps
	# to its internal ranges if our values fall outside (shouldn't
	# happen since GoolAcousticProfile's @export_range matches the
	# engine's expected ranges, but cheap insurance).
	_runtime.set_effect_parameter(bus_name, _effect_index,
			_PARAM_DECAY,       profile.reverb_decay)
	_runtime.set_effect_parameter(bus_name, _effect_index,
			_PARAM_LF_DAMPING,  profile.reverb_lf_damping)
	_runtime.set_effect_parameter(bus_name, _effect_index,
			_PARAM_HF_DAMPING,  profile.reverb_hf_damping)
	_runtime.set_effect_parameter(bus_name, _effect_index,
			_PARAM_DIFFUSION,   profile.reverb_diffusion)
	_runtime.set_effect_parameter(bus_name, _effect_index,
			_PARAM_PREDELAY_MS, profile.reverb_predelay_ms)
	_runtime.set_effect_parameter(bus_name, _effect_index,
			_PARAM_WET_GAIN_DB, profile.reverb_wet_gain_db)


# Locate the Reverb-kind effect on the configured bus. Caches the
# effect's index in _effect_index for apply() to reuse. Mirrors
# ReverbZone._locate_reverb_effect — same fallback behavior on
# missing bus / missing reverb effect (warn once per session per
# bus name, then go inert).
func _locate_reverb_effect() -> void:
	var effects: Array = _runtime.get_bus_effects(bus_name)
	if effects.is_empty():
		if not _warned_missing_buses.has(bus_name):
			_warned_missing_buses[bus_name] = true
			push_warning("GoolSceneProfile: bus '%s' has no effects "
					% bus_name
					+ "(or doesn't exist). Profile inert. Add a Reverb "
					+ "effect to the bus in your gool config, or set "
					+ "`bus_name` to a bus that has one. (Further "
					+ "warnings for this bus suppressed.)")
		return

	for i in range(effects.size()):
		var e: Dictionary = effects[i]
		var kind_name: String = String(e.get("kind_name", ""))
		if kind_name == "Reverb":
			_effect_index = i
			return

	# Bus exists but has no reverb effect.
	if not _warned_missing_buses.has(bus_name):
		_warned_missing_buses[bus_name] = true
		push_warning("GoolSceneProfile: bus '%s' has no Reverb effect. "
				% bus_name
				+ "Profile inert. Add a Reverb to the bus in your gool "
				+ "config. (Further warnings for this bus suppressed.)")


# Editor-side configuration warnings. Surface as the yellow triangle
# Godot shows next to misconfigured nodes in the scene tree.
func _get_configuration_warnings() -> PackedStringArray:
	var warnings: PackedStringArray = []
	if profile == null:
		warnings.append("No GoolAcousticProfile assigned. Pick one "
				+ "from the dropdown or assign a .tres in the "
				+ "Inspector.")
	return warnings
