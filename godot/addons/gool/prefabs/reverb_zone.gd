# addons/gool/prefabs/reverb_zone.gd
#
## Area3D that paints a space's acoustic character onto the reverb
## bus when a listener walks in. Two ways to author:
#
##   1. **Material-aware (the easy path).** Set `material` to one of
##      the Gool.MATERIAL_* constants — Concrete, Wood, Foliage, etc.
##      The zone pulls the matching engine preset (decay + dampings +
##      diffusion) and applies it. Drop the node, pick the material,
##      done.
#
##   2. **Per-parameter override (for fine-tuning).** Leave material
##      at MATERIAL_DEFAULT and dial decay / lf_damping / hf_damping /
##      diffusion in the inspector. The zone uses those values
##      verbatim. Use this when no preset feels exactly right and
##      you want to author the room by hand.
#
## Either way, the zone targets a named reverb bus in your gool
## config (default "Sfx", the standard mixer's reverb-bearing bus).
## It scans the bus's effect chain for the first effect of kind
## "Reverb" and pushes parameters to that effect. If no reverb is on
## the bus, the zone warns once at _ready and goes inert.
#
## On entry the zone smoothly ramps the four reverb parameters
## toward the target values over `transition_ms`. On exit it ramps
## back to whatever the parameters were before the zone first
## applied (captured at first entry). `wet_gain_db` is also pushed
## the same way; it's the most direct control over "how much
## reverb you hear" and the most natural designer knob for spaces
## that should feel more or less reverberant overall.
#
## Stacked / overlapping zones aren't fully supported yet: only the
## most recently entered zone wins, and on exit the params restore
## to the captured defaults regardless of whether another zone is
## still active. For 99% of layouts (a level's rooms don't usually
## overlap) this is fine. Track <github issue link> if you hit a
## layout where stacking matters.

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

## v0.46.1: Predelay in milliseconds — gap between the dry signal
## and the start of the reverb tail. Defining perceptual cue for
## space SIZE: small rooms have ~5ms, medium rooms ~20ms,
## halls/cathedrals 50–100ms. Without predelay variation, "small
## bathroom" and "large cathedral" sound smushed together because
## the human ear uses predelay as the primary size cue.
##
## Engine cap is 200ms (the DSP's allocated buffer); useful range
## is roughly 5..80ms for normal interior spaces, 0 for outdoor.
@export_range(0.0, 200.0, 1.0, "suffix:ms") var predelay_ms: float = 30.0

## Wet-mix level in dB. The most direct "how much reverb" knob —
## independent from material/per-parameter authoring (always
## applied regardless of which mode you're in). -60 = effectively
## dry (no audible reverb), 0 = unity, positive values boost.
@export_range(-60.0, 0.0, 0.5, "suffix:dB") var wet_gain_db: float = -12.0

## v0.47.0: HPF cutoff applied to the bus signal BEFORE the Reverb
## effect — the Abbey Road trick (per MixingLessons). Keeps mud
## and low-frequency content out of the reverb tail so the tail
## doesn't pile onto the bass of the dry signal. 0 = bypass (no
## HPF applied). Typical values: 200Hz for warmth, 400Hz for
## clarity, 600Hz for the classic Abbey Road setting.
##
## Requires a Biquad effect in the bus chain at index
## (reverb_index - 1). ReverbZone auto-discovers it. If missing,
## logs a one-time warning at scene start; the send-HPF path is
## skipped gracefully but all other reverb shaping still works.
@export_range(0.0, 2000.0, 10.0, "suffix:Hz") var send_hpf_hz: float = 0.0

## v0.47.0: LPF cutoff applied to the bus signal AFTER the Reverb
## effect. Per Sound on Sound's research, this is the single biggest
## realism knob — real spaces rarely have content above ~5kHz in
## the reflected field. Typical values: 3kHz for warm/intimate
## (vocal ambience), 5–6kHz for natural (concert hall, room),
## 7–10kHz for bright (plate, small room).
##
## Different from hf_damping: damping fades highs as the tail
## decays (time-varying); LPF cuts highs uniformly (frequency-only).
## Both knobs at once is normal — damping for tail character, LPF
## for mix placement.
##
## 22000 = effectively bypass. Requires a Biquad effect at index
## (reverb_index + 1). Same auto-discovery + graceful skip as
## send_hpf_hz.
@export_range(2000.0, 22000.0, 100.0, "suffix:Hz") var return_lpf_hz: float = 22000.0

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

# v0.55.0: dedupe set for the "bus has no effects (or doesn't
# exist)" warning. Shared across all ReverbZone instances via
# `static var` — without this, N zones × M state-change refreshes
# produced N×M identical warnings (a scene with 5 reverb zones
# all pointing at a misconfigured "Sfx" bus easily hit 40+ in
# the first F5 second). Keys are bus_name strings, values true.
# Lifetime is the scene-tree/editor session.
static var _warned_missing_buses: Dictionary = {}

# v0.47.0: bus-chain slot indices for the optional pre/post EQ
# biquads. -1 means "not present in the bus chain at the expected
# adjacent slot; skip this side of the EQ shaping." Discovery
# happens once in _locate_reverb_effect; missing slots fire a
# one-time push_warning so the integrator knows what to add.
var _send_hpf_index: int = -1
var _return_lpf_index: int = -1
# Cutoffs that were on the discovered biquad slots at startup. We
# snapshot them so exit-restore puts them back, matching the
# semantics for the reverb params themselves.
var _default_send_hpf_hz: float = 20.0    # near-bypass HPF default
var _default_return_lpf_hz: float = 22000.0  # bypass LPF default

var _have_defaults: bool = false
var _default_decay: float = 0.0
var _default_lf_damping: float = 0.0
var _default_hf_damping: float = 0.0
var _default_diffusion: float = 0.0
var _default_wet_gain_db: float = 0.0
# v0.46.1: predelay default. 30ms matches the DSP's ReverbEffect
# default constructor value — what the bus has if config.json
# doesn't override.
var _default_predelay_ms: float = 30.0

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
# non-neutral. Otherwise these stay at 0 and _process skips the
# listener-EQ apply step entirely.
#
# Six scalar floats (not a Dictionary) — only the three band
# gains interpolate. Cutoff frequencies and Q are set once at
# ramp start since they're inaudible at 0 dB gain.
var _eq_active: bool = false
var _eq_from_low: float = 0.0
var _eq_from_mid: float = 0.0
var _eq_from_high: float = 0.0
var _eq_to_low: float = 0.0
var _eq_to_mid: float = 0.0
var _eq_to_high: float = 0.0

# Engine effect parameter IDs (from include/audio_engine/bus.h
# namespace EffectParameter). Copied here so the GDScript zone
# doesn't need a header bridge.
const _PARAM_DECAY:       int = 9
const _PARAM_HF_DAMPING:  int = 10
const _PARAM_WET_GAIN_DB: int = 11
const _PARAM_LF_DAMPING:  int = 24
const _PARAM_DIFFUSION:   int = 25
# v0.46.1: Reverb_PredelayMs = 23 in the engine's param enum
# (include/audio_engine/bus.h). The DSP has supported it since
# v0.29 but ReverbZone didn't @export it, so cathedral/outdoor
# distinctions were neutered. Adding now.
const _PARAM_PREDELAY_MS: int = 23

# v0.47.0: sentinel keys for the pre/post EQ shaping params.
# These don't correspond to engine parameter IDs — they're routed
# in _apply_values to the adjacent biquad slots (set_effect_parameter
# at _send_hpf_index / _return_lpf_index) instead of to the Reverb
# effect itself. Value range is outside the engine's param-ID
# space (0..255) so there's no collision risk.
const _SENTINEL_SEND_HPF_HZ:   int = 100000
const _SENTINEL_RETURN_LPF_HZ: int = 100001

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
		# v0.55.0: rate-limit the missing-bus warning across all
		# ReverbZone instances. Previously, N zones × M state-change
		# refreshes produced N×M identical warnings. Now each
		# unique bus_name warns once per session.
		if not _warned_missing_buses.has(bus_name):
			_warned_missing_buses[bus_name] = true
			push_warning("ReverbZone: bus '%s' has no effects (or doesn't exist). " % bus_name
					+ "Zone is inert. Add a Reverb effect to the bus in your gool config, "
					+ "or set `bus_name` to a bus that has one. "
					+ "(Further warnings for this bus suppressed.)")
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
			_default_predelay_ms = float(params.get(_PARAM_PREDELAY_MS, 30.0))
			_have_defaults = true
			# v0.47.0: probe for adjacent Biquad slots used by the EQ
			# shaping pre/post-reverb path. Per Sound on Sound + the
			# Abbey Road trick, the recommended chain shape is
			# ...→ HPF biquad → Reverb → LPF biquad → ... so we look
			# at i-1 and i+1 specifically.
			if i - 1 >= 0:
				var e_prev: Dictionary = effects[i - 1]
				if String(e_prev.get("kind_name", "")) == "Biquad":
					_send_hpf_index = i - 1
					var p_prev: Dictionary = e_prev.get("params", {})
					_default_send_hpf_hz = float(p_prev.get(
							_BIQUAD_CUTOFF_HZ, 20.0))
			if i + 1 < effects.size():
				var e_next: Dictionary = effects[i + 1]
				if String(e_next.get("kind_name", "")) == "Biquad":
					_return_lpf_index = i + 1
					var p_next: Dictionary = e_next.get("params", {})
					_default_return_lpf_hz = float(p_next.get(
							_BIQUAD_CUTOFF_HZ, 22000.0))
			# One-time warnings if either slot is missing AND the
			# zone is configured to use that side of the shaping.
			# We check the @export here at scene start; if the
			# integrator switches it on at runtime later with no
			# slot present, the warning fires again on next ramp.
			if _send_hpf_index < 0 and send_hpf_hz > 0.0:
				push_warning(
					"ReverbZone: send_hpf_hz=%.0f set but no Biquad effect at index %d "
					% [send_hpf_hz, i - 1]
					+ "(immediately before Reverb) on bus '%s'. " % bus_name
					+ "Send-HPF will be skipped. To enable: add a Biquad to the bus's "
					+ "effect chain right before the Reverb effect — gool ships a "
					+ "default config with this slot pre-populated as of v0.47.0; if "
					+ "your config predates that, copy the relevant entry from "
					+ "addons/gool/templates/config_fps.json.")
			if _return_lpf_index < 0 and return_lpf_hz < 22000.0:
				push_warning(
					"ReverbZone: return_lpf_hz=%.0f set but no Biquad effect at index %d "
					% [return_lpf_hz, i + 1]
					+ "(immediately after Reverb) on bus '%s'. " % bus_name
					+ "Return-LPF will be skipped. To enable: add a Biquad to the bus's "
					+ "effect chain right after the Reverb effect — the slot is also "
					+ "pre-populated in addons/gool/templates/config_fps.json.")
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
	#
	# Literals 0 and 1 match the @export_enum declaration above
	# (Default:0, Air:1) — the Gool.MATERIAL_* constants live on
	# the autoload and aren't in scope here.
	if material == 0 or material == 1:
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
	# v0.36.0 (Phase 6.D): apply the realism multiplier to the
	# three gain bands. Pulled from the Gool autoload's cached
	# value (set at startup, runtime-adjustable via
	# Gool.set_eq_intensity). Cutoff/Q stay unscaled — they're
	# frequency anchors, not amplitudes.
	var intensity: float = _runtime._eq_intensity
	_eq_to_low    = float(curve.get("low_gain_db",  0.0)) * intensity
	_eq_to_mid    = float(curve.get("mid_gain_db",  0.0)) * intensity
	_eq_to_high   = float(curve.get("high_gain_db", 0.0)) * intensity
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
			# v0.46.1: pull predelay from the material preset too.
			# Materials like Liquid have very different predelay
			# characters (60ms) from Wood (10ms) — without this it
			# all collapsed to the engine default.
			_PARAM_PREDELAY_MS: float(preset.get("predelay_ms", _default_predelay_ms)),
			# v0.47.0: pre/post EQ shaping. Falls back to the zone's
			# own @export if the material preset doesn't specify —
			# materials map to acoustic-space presets, not necessarily
			# to mix-context EQ choices, so the zone @export wins
			# when present.
			_SENTINEL_SEND_HPF_HZ:   float(preset.get("send_hpf_hz",
					send_hpf_hz if send_hpf_hz > 0.0 else _default_send_hpf_hz)),
			_SENTINEL_RETURN_LPF_HZ: float(preset.get("return_lpf_hz",
					return_lpf_hz if return_lpf_hz < 22000.0 else _default_return_lpf_hz)),
		}
	# Manual path — use the per-parameter exports verbatim.
	return {
		_PARAM_DECAY:       decay,
		_PARAM_LF_DAMPING:  lf_damping,
		_PARAM_HF_DAMPING:  hf_damping,
		_PARAM_DIFFUSION:   diffusion,
		_PARAM_WET_GAIN_DB: wet_gain_db,
		_PARAM_PREDELAY_MS: predelay_ms,
		# v0.47.0: pre/post EQ shaping. Zone export is the source of
		# truth in manual mode. Effective bypass (0 / 22000) means
		# "leave the biquad cutoff at its captured default."
		_SENTINEL_SEND_HPF_HZ:   send_hpf_hz if send_hpf_hz > 0.0
				else _default_send_hpf_hz,
		_SENTINEL_RETURN_LPF_HZ: return_lpf_hz if return_lpf_hz < 22000.0
				else _default_return_lpf_hz,
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
			_PARAM_PREDELAY_MS: _default_predelay_ms,
			# v0.47.0
			_SENTINEL_SEND_HPF_HZ:   _default_send_hpf_hz,
			_SENTINEL_RETURN_LPF_HZ: _default_return_lpf_hz,
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
		var key: int = int(k)
		# v0.47.0: sentinel keys route to adjacent biquad slots
		# rather than the Reverb effect. Skip if the slot wasn't
		# discovered (graceful no-op — warning already fired in
		# _locate_reverb_effect).
		if key == _SENTINEL_SEND_HPF_HZ:
			if _send_hpf_index >= 0:
				_runtime.set_effect_parameter(bus_name, _send_hpf_index,
						_BIQUAD_CUTOFF_HZ, float(values[k]))
			continue
		if key == _SENTINEL_RETURN_LPF_HZ:
			if _return_lpf_index >= 0:
				_runtime.set_effect_parameter(bus_name, _return_lpf_index,
						_BIQUAD_CUTOFF_HZ, float(values[k]))
			continue
		# Regular reverb parameter — goes to the Reverb effect itself.
		_runtime.set_effect_parameter(bus_name, _effect_index,
				key, float(values[k]))

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
