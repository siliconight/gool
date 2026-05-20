# addons/gool/prefabs/reverb_zone.gd
#
# Area3D that paints a space's acoustic character onto the reverb
# bus when a listener walks in. Two ways to author:
#
#   1. **Material-aware (the easy path).** Set `material` to one of
#      the Gool.MATERIAL_* constants — Concrete, Wood, Foliage, etc.
#      The zone pulls the matching engine preset (decay + dampings +
#      diffusion) and applies it. Drop the node, pick the material,
#      done.
#
#   2. **Per-parameter override (for fine-tuning).** Leave material
#      at MATERIAL_DEFAULT and dial decay / lf_damping / hf_damping /
#      diffusion in the inspector. The zone uses those values
#      verbatim. Use this when no preset feels exactly right and
#      you want to author the room by hand.
#
# Either way, the zone targets a named reverb bus in your gool
# config (default "Sfx", the standard mixer's reverb-bearing bus).
# It scans the bus's effect chain for the first effect of kind
# "Reverb" and pushes parameters to that effect. If no reverb is on
# the bus, the zone warns once at _ready and goes inert.
#
# On entry the zone smoothly ramps the four reverb parameters
# toward the target values over `transition_ms`. On exit it ramps
# back to whatever the parameters were before the zone first
# applied (captured at first entry). `wet_gain_db` is also pushed
# the same way; it's the most direct control over "how much
# reverb you hear" and the most natural designer knob for spaces
# that should feel more or less reverberant overall.
#
# Stacked / overlapping zones aren't fully supported yet: only the
# most recently entered zone wins, and on exit the params restore
# to the captured defaults regardless of whether another zone is
# still active. For 99% of layouts (a level's rooms don't usually
# overlap) this is fine. Track <github issue link> if you hit a
# layout where stacking matters.

@tool
class_name ReverbZone
extends Area3D

## Material that defines this space's acoustic character. When set
## to anything other than MATERIAL_DEFAULT, the zone uses the
## engine's per-material reverb preset (decay + dampings +
## diffusion) and ignores the per-parameter values below. Set to
## MATERIAL_DEFAULT to author parameters manually instead.
@export_enum("Default:0", "Air:1", "Glass:2", "Wood:3", "Drywall:4",
		"Concrete:5", "Metal:6", "Curtain:7", "Foliage:8")
var material: int = 0

## Reverb decay length in [0..1]: 0 = short tail, 1 = long. Only
## used when `material` is MATERIAL_DEFAULT. (Maps to the engine's
## Reverb_Decay parameter — the same knob that JSON bus configs
## call "decay" or, historically, "room_size".)
@export_range(0.0, 1.0, 0.01) var decay: float = 0.6

## Low-frequency damping in [0..1]: how much the reverb tail's
## bass is absorbed. 0 = full bass tail, 1 = bass cut entirely.
## Only used when `material` is MATERIAL_DEFAULT.
@export_range(0.0, 1.0, 0.01) var lf_damping: float = 0.1

## High-frequency damping in [0..1]: 0 = bright tail, 1 = muffled.
## Only used when `material` is MATERIAL_DEFAULT.
@export_range(0.0, 1.0, 0.01) var hf_damping: float = 0.3

## Diffusion in [0..1]: how smeared the tail is. 0 = comb-like
## "ping-pong" reflections, 1 = smooth wash. Only used when
## `material` is MATERIAL_DEFAULT.
@export_range(0.0, 1.0, 0.01) var diffusion: float = 0.625

## Wet-mix level in dB. The most direct "how much reverb" knob —
## independent from material/per-parameter authoring (always
## applied regardless of which mode you're in). -60 = effectively
## dry (no audible reverb), 0 = unity, positive values boost.
@export_range(-60.0, 0.0, 0.5, "suffix:dB") var wet_gain_db: float = -12.0

## Smoothing time for the parameter ramp on entry/exit (ms).
## Longer values feel more natural (real spaces don't switch
## acoustic identity instantly). 800 ms is the default sweet spot;
## drop to ~200 ms for fast-paced level transitions, push to
## ~2000 ms for cinematic camera moves into a new room.
@export_range(0.0, 5000.0, 1.0, "suffix:ms") var transition_ms: float = 800.0

## Target bus carrying the reverb effect. Default "Sfx" matches
## the standard gool config; change if your project routes reverb
## to a dedicated bus (e.g. "Reverb" or "ReverbBus").
@export var bus_name: String = "Sfx"

## Group that the listener (player) is expected to be in. Match
## this to the group your character body is in — the standard
## convention is "gool_listener".
@export var listener_group: String = "gool_listener"

## v0.35.0 (Phase 6.C): when true, the zone also pushes this
## material's EQ curve to the global listener-space EQ bus on
## entry, modeling "your ears are inside this material". On exit,
## the EQ ramps back to neutral over `transition_ms`.
##
## This is a STRONG editorial effect — being inside a "Wood" zone
## means every diegetic sound the player hears gets the wood
## curve's warm low-mid coloring. Use sparingly: cinematic
## spaces, distinct "you're somewhere unusual" environments,
## not every room in your level. Default off.
##
## Requires the listener-EQ bus to be set up (default name
## "ListenerEq", configurable via project setting
## gool/material_eq/listener_bus). The bus must have at least 3
## biquads at chain indices 0, 1, 2 in order LowShelf, Peak,
## HighShelf. See cookbook section 14 for the authoring contract.
##
## If the bus isn't configured or doesn't match, the zone warns
## once at _ready and the listener-EQ portion silently skips —
## the reverb portion of the zone still works.
##
## Neutral-material zones (MATERIAL_DEFAULT / MATERIAL_AIR) skip
## the listener EQ ramp even when this is true. A Default zone
## applies its reverb settings but doesn't touch the EQ bus.
@export var apply_listener_eq: bool = false

signal listener_entered
signal listener_exited

var _runtime: Node = null
var _occupied: bool = false

# Cached at _ready (or first entry): the bus's reverb effect index
# in the chain, and the parameter values present BEFORE this zone
# ever pushed anything, so we can restore them on exit. -1 means
# "we haven't found a reverb effect on the bus; zone is inert."
var _effect_index: int = -1
var _have_defaults: bool = false
var _default_decay: float = 0.0
var _default_lf_damping: float = 0.0
var _default_hf_damping: float = 0.0
var _default_diffusion: float = 0.0
var _default_wet_gain_db: float = 0.0

# Smoothing state. We tween from current → target over
# transition_ms by linearly interpolating each frame in _process.
# A null target means no ramp is in progress.
var _ramp_progress: float = 1.0   # [0..1] — 1.0 = done
var _ramp_duration_s: float = 0.0
var _ramp_from := {}
var _ramp_to := {}

# v0.35.0 (Phase 6.C): listener-EQ ramp state. Runs in parallel
# with the reverb ramp using the same _ramp_progress (so the EQ
# and reverb transition feel synchronized — you don't hear the
# reverb fade in while the EQ snaps instantly).
#
# Active only when apply_listener_eq=true AND Gool's
# _listener_eq_bus_name is non-empty AND the zone's material is
# non-neutral. Otherwise these stay empty and _process skips the
# listener-EQ apply step entirely.
var _eq_active: bool = false
var _eq_ramp_from := {}
var _eq_ramp_to := {}

# Engine effect parameter IDs (from include/audio_engine/bus.h
# namespace EffectParameter). Copied here so the GDScript zone
# doesn't need a header bridge.
const _PARAM_DECAY:       int = 9
const _PARAM_HF_DAMPING:  int = 10
const _PARAM_WET_GAIN_DB: int = 11
const _PARAM_LF_DAMPING:  int = 24
const _PARAM_DIFFUSION:   int = 25

# v0.35.0 (Phase 6.C): biquad parameter IDs + chain indices for
# the listener-EQ bus. The three biquads at indices 0/1/2 of the
# bus are LowShelf, Peak, HighShelf in that order (cookbook §14
# authoring contract).
const _BIQUAD_CUTOFF_HZ:  int = 2
const _BIQUAD_Q:          int = 3
const _BIQUAD_GAIN_DB:    int = 12
const _EQ_BIQUAD_LOW:     int = 0
const _EQ_BIQUAD_MID:     int = 1
const _EQ_BIQUAD_HIGH:    int = 2

# One-shot flag to suppress repeated warnings if apply_listener_eq
# is on but the bus isn't configured. We warn once on the first
# enter attempt, then stay quiet — the user's seen it.
var _eq_warning_emitted: bool = false

func _ready() -> void:
	if Engine.is_editor_hint():
		return
	_runtime = get_node_or_null("/root/Gool")
	if _runtime == null:
		push_warning("ReverbZone: /root/Gool autoload not found. The gool plugin is installed but not enabled. Fix: open Project Settings → Plugins, find 'gool' in the list, tick the Enable checkbox. (If gool is not in the list, the addon folder is missing — see https://github.com/siliconight/gool for install instructions.)")
		return
	if not _runtime.is_initialized():
		await _runtime.ready_to_play
	# Locate the reverb effect on the target bus by scanning the
	# effect chain. Cache its index for set_effect_parameter calls.
	# Also snapshot the current parameter values as our exit-restore
	# defaults so the zone is a pure overlay on top of whatever the
	# project's reverb is already set to.
	_locate_reverb_effect()
	body_entered.connect(_on_body_entered)
	body_exited.connect(_on_body_exited)

func _locate_reverb_effect() -> void:
	var effects: Array = _runtime.get_bus_effects(bus_name)
	if effects.is_empty():
		push_warning("ReverbZone: bus '%s' has no effects (or doesn't exist). " % bus_name
				+ "Zone is inert. Add a Reverb effect to the bus in your gool config, "
				+ "or set `bus_name` to a bus that has one.")
		return
	for i in range(effects.size()):
		var e: Dictionary = effects[i]
		var kind_name: String = String(e.get("kind_name", ""))
		if kind_name == "Reverb":
			_effect_index = i
			var params: Dictionary = e.get("params", {})
			_default_decay       = float(params.get(_PARAM_DECAY,       0.5))
			_default_lf_damping  = float(params.get(_PARAM_LF_DAMPING,  0.1))
			_default_hf_damping  = float(params.get(_PARAM_HF_DAMPING,  0.3))
			_default_diffusion   = float(params.get(_PARAM_DIFFUSION,   0.625))
			_default_wet_gain_db = float(params.get(_PARAM_WET_GAIN_DB, -60.0))
			_have_defaults = true
			return
	push_warning("ReverbZone: bus '%s' has no Reverb effect. Zone is inert. " % bus_name
			+ "Add a Reverb effect to the bus's effect chain in your gool config.")

func _on_body_entered(body: Node) -> void:
	if not body.is_in_group(listener_group):
		return
	if _occupied:
		return
	_occupied = true
	listener_entered.emit()
	_start_ramp_to_zone_settings()

func _on_body_exited(body: Node) -> void:
	if not body.is_in_group(listener_group):
		return
	if not _occupied:
		return
	_occupied = false
	listener_exited.emit()
	_start_ramp_to_defaults()

func _start_ramp_to_zone_settings() -> void:
	if _effect_index < 0 or not _have_defaults:
		return
	var target := _resolve_zone_target()
	# `_ramp_from` is the current state (whatever's live right now
	# on the engine; if we're ramping mid-transition, the latest
	# interpolated values, otherwise the captured defaults). We
	# read from `_ramp_to` if a ramp is in progress, else from
	# defaults — this makes back-to-back zones smooth.
	_ramp_from = _current_live_values()
	_ramp_to   = target
	# v0.35.0 (Phase 6.C): set up parallel listener-EQ ramp if
	# enabled. Non-neutral materials only; the autoload must have
	# verified the listener-EQ bus at startup (cached in
	# _listener_eq_bus_name).
	_setup_eq_ramp_to_material()
	_begin_ramp()

func _start_ramp_to_defaults() -> void:
	if _effect_index < 0 or not _have_defaults:
		return
	_ramp_from = _current_live_values()
	_ramp_to = {
		_PARAM_DECAY:       _default_decay,
		_PARAM_LF_DAMPING:  _default_lf_damping,
		_PARAM_HF_DAMPING:  _default_hf_damping,
		_PARAM_DIFFUSION:   _default_diffusion,
		_PARAM_WET_GAIN_DB: _default_wet_gain_db,
	}
	# v0.35.0 (Phase 6.C): on exit, ramp listener EQ gains back
	# to neutral (0 dB across all three biquads) over the same
	# transition. Cutoffs/Q are left at their last-applied values
	# since they're inaudible at 0 dB gain.
	_setup_eq_ramp_to_neutral()
	_begin_ramp()

# v0.35.0 (Phase 6.C): listener-EQ ramp setup paths. The Gool
# autoload's _listener_eq_bus_name is the source of truth for
# whether the bus is configured + valid; this method only sets
# _eq_active=true when both that and the per-zone apply_listener_eq
# are true AND the zone's material is non-neutral.
func _setup_eq_ramp_to_material() -> void:
	if not apply_listener_eq:
		_eq_active = false
		return
	# Neutral materials would push 0 dB to all three biquads,
	# which is a valid "reset" — but it would also blow away
	# whatever a previous non-neutral zone set, even though this
	# zone is doing nothing acoustically interesting. Skip
	# entirely; the previous zone's coloring (if any) persists
	# until another non-neutral zone overwrites or this zone
	# exits.
	if material == MATERIAL_DEFAULT or material == MATERIAL_AIR:
		_eq_active = false
		return
	if _runtime._listener_eq_bus_name == "":
		# apply_listener_eq is on but the bus isn't configured.
		# Warn once on this zone (suppressed for subsequent
		# enters), then disable.
		if not _eq_warning_emitted:
			push_warning(
				"ReverbZone: apply_listener_eq=true but the listener-EQ "
				+ "bus isn't configured. Add a 'ListenerEq' bus (or "
				+ "whatever you've set gool/material_eq/listener_bus to) "
				+ "with 3 biquads in LowShelf → Peak → HighShelf order. "
				+ "See cookbook section 14 for the authoring contract."
			)
			_eq_warning_emitted = true
		_eq_active = false
		return

	var curve: Dictionary = _runtime.material_eq_for_material(material)
	# Cutoffs and Q are set immediately at ramp start, not
	# interpolated — they only matter when the gain is non-zero,
	# and we're about to ramp the gains from 0 (or previous) up
	# to target.
	var bus: String = _runtime._listener_eq_bus_name
	_runtime.set_effect_parameter(bus, _EQ_BIQUAD_LOW,  _BIQUAD_CUTOFF_HZ,
			float(curve.get("low_freq_hz",  200.0)))
	_runtime.set_effect_parameter(bus, _EQ_BIQUAD_MID,  _BIQUAD_CUTOFF_HZ,
			float(curve.get("mid_freq_hz", 1000.0)))
	_runtime.set_effect_parameter(bus, _EQ_BIQUAD_MID,  _BIQUAD_Q,
			float(curve.get("mid_q",          1.0)))
	_runtime.set_effect_parameter(bus, _EQ_BIQUAD_HIGH, _BIQUAD_CUTOFF_HZ,
			float(curve.get("high_freq_hz", 8000.0)))

	_eq_from_low  = _eq_to_low   # carry forward whatever was live
	_eq_from_mid  = _eq_to_mid
	_eq_from_high = _eq_to_high
	_eq_to_low    = float(curve.get("low_gain_db",  0.0))
	_eq_to_mid    = float(curve.get("mid_gain_db",  0.0))
	_eq_to_high   = float(curve.get("high_gain_db", 0.0))
	_eq_active = true

func _setup_eq_ramp_to_neutral() -> void:
	if not _eq_active and not apply_listener_eq:
		return
	if _runtime._listener_eq_bus_name == "":
		_eq_active = false
		return
	# Ramp gains back to neutral. From values = whatever's live
	# (carried forward from the last setup); to values = 0 dB.
	_eq_from_low  = _eq_to_low
	_eq_from_mid  = _eq_to_mid
	_eq_from_high = _eq_to_high
	_eq_to_low    = 0.0
	_eq_to_mid    = 0.0
	_eq_to_high   = 0.0
	_eq_active = true

func _resolve_zone_target() -> Dictionary:
	# Material-aware path. If the designer picked a non-Default
	# material, pull the engine's preset for the four character
	# parameters; wet_gain_db is always taken from the zone's own
	# export (it's the volume of the reverb, not a property of
	# the material).
	if material != 0:
		var preset: Dictionary = _runtime.get_reverb_preset_for_material(material)
		return {
			_PARAM_DECAY:       float(preset.get("decay",      _default_decay)),
			_PARAM_LF_DAMPING:  float(preset.get("lf_damping", _default_lf_damping)),
			_PARAM_HF_DAMPING:  float(preset.get("hf_damping", _default_hf_damping)),
			_PARAM_DIFFUSION:   float(preset.get("diffusion",  _default_diffusion)),
			_PARAM_WET_GAIN_DB: wet_gain_db,
		}
	# Manual path — use the per-parameter exports verbatim.
	return {
		_PARAM_DECAY:       decay,
		_PARAM_LF_DAMPING:  lf_damping,
		_PARAM_HF_DAMPING:  hf_damping,
		_PARAM_DIFFUSION:   diffusion,
		_PARAM_WET_GAIN_DB: wet_gain_db,
	}

func _current_live_values() -> Dictionary:
	# Mid-ramp: latest interpolated. Otherwise: most recent target,
	# or defaults if we haven't ramped yet.
	if _ramp_progress < 1.0:
		# Interpolated snapshot at current progress.
		var out := {}
		for k in _ramp_from.keys():
			var f: float = float(_ramp_from[k])
			var t: float = float(_ramp_to.get(k, f))
			out[k] = lerp(f, t, _ramp_progress)
		return out
	if _ramp_to.is_empty():
		# Never ramped — defaults.
		return {
			_PARAM_DECAY:       _default_decay,
			_PARAM_LF_DAMPING:  _default_lf_damping,
			_PARAM_HF_DAMPING:  _default_hf_damping,
			_PARAM_DIFFUSION:   _default_diffusion,
			_PARAM_WET_GAIN_DB: _default_wet_gain_db,
		}
	return _ramp_to.duplicate()

func _begin_ramp() -> void:
	_ramp_progress = 0.0
	_ramp_duration_s = max(0.001, transition_ms / 1000.0)
	# Apply ramp-start values immediately for clean t=0 alignment.
	_apply_values(_ramp_from)
	if _eq_active:
		_apply_eq_gains(_eq_from_low, _eq_from_mid, _eq_from_high)
	set_process(true)

func _process(delta: float) -> void:
	if Engine.is_editor_hint():
		return
	if _ramp_progress >= 1.0:
		set_process(false)
		return
	_ramp_progress = min(1.0, _ramp_progress + delta / _ramp_duration_s)
	# Reverb param interpolation (unchanged from v0.32.0).
	var values := {}
	for k in _ramp_from.keys():
		var f: float = float(_ramp_from[k])
		var t: float = float(_ramp_to.get(k, f))
		values[k] = lerp(f, t, _ramp_progress)
	_apply_values(values)
	# v0.35.0 (Phase 6.C): listener-EQ gain interpolation in
	# lockstep with the reverb ramp. Same _ramp_progress, same
	# duration — the EQ and reverb feel synchronized to the
	# listener.
	if _eq_active:
		var g_low:  float = lerp(_eq_from_low,  _eq_to_low,  _ramp_progress)
		var g_mid:  float = lerp(_eq_from_mid,  _eq_to_mid,  _ramp_progress)
		var g_high: float = lerp(_eq_from_high, _eq_to_high, _ramp_progress)
		_apply_eq_gains(g_low, g_mid, g_high)
	if _ramp_progress >= 1.0:
		# Ramp complete. If we just finished a ramp-to-neutral
		# (both target gains are 0), mark EQ inactive so future
		# enters carry "from = 0".
		if _eq_active \
				and abs(_eq_to_low)  < 0.01 \
				and abs(_eq_to_mid)  < 0.01 \
				and abs(_eq_to_high) < 0.01:
			_eq_active = false
		set_process(false)

func _apply_values(values: Dictionary) -> void:
	if _effect_index < 0:
		return
	# set_effect_parameter takes bus_name (String), not bus_id —
	# it resolves the name to a BusId internally each call. The
	# resolution is O(N) over kMaxBuses (16) but only fires a few
	# times per frame during a ramp, well below any perf concern.
	for k in values.keys():
		_runtime.set_effect_parameter(bus_name, _effect_index,
				int(k), float(values[k]))

# v0.35.0 (Phase 6.C): push the three listener-EQ band gains to
# their biquads on the Gool autoload's _listener_eq_bus_name.
# Called by _begin_ramp (t=0) and _process (each frame during a
# ramp). Cutoffs/Q are NOT pushed here — they were set once when
# _setup_eq_ramp_to_material established the target material,
# since they don't need to interpolate.
func _apply_eq_gains(low_db: float, mid_db: float, high_db: float) -> void:
	var bus: String = _runtime._listener_eq_bus_name
	if bus == "":
		return
	_runtime.set_effect_parameter(bus, _EQ_BIQUAD_LOW,  _BIQUAD_GAIN_DB, low_db)
	_runtime.set_effect_parameter(bus, _EQ_BIQUAD_MID,  _BIQUAD_GAIN_DB, mid_db)
	_runtime.set_effect_parameter(bus, _EQ_BIQUAD_HIGH, _BIQUAD_GAIN_DB, high_db)

# Public helpers retained for back-compat with v0.x scripts that
# invoked the old no-op methods. They now apply the zone settings
# (or defaults) immediately without ramping — useful for tests,
# editor previews, or scripted reverb changes outside the body-
# entered flow.
func _apply_zone_settings() -> void:
	if _effect_index < 0 or not _have_defaults:
		return
	_apply_values(_resolve_zone_target())

func _restore_default_settings() -> void:
	if _effect_index < 0 or not _have_defaults:
		return
	_apply_values({
		_PARAM_DECAY:       _default_decay,
		_PARAM_LF_DAMPING:  _default_lf_damping,
		_PARAM_HF_DAMPING:  _default_hf_damping,
		_PARAM_DIFFUSION:   _default_diffusion,
		_PARAM_WET_GAIN_DB: _default_wet_gain_db,
	})
