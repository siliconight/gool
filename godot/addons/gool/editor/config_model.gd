# addons/gool/editor/config_model.gd
#
# v0.28.4 (Phase 3.3c-3): persistence layer for the mixer dock.
#
# Owns the editor-side view of gool/config.json. Two roles:
#
# 1. **Source of truth at rest**. When no game is running (F5 off),
#    the dock can't poll bus stats — so it falls back to this model
#    to render strips, fader values, and effect chains. The data
#    comes from config.json read at editor startup.
#
# 2. **Persistence on every edit**. When the user drags a fader or
#    a slider in the dock, the dock calls into this model:
#
#        model.set_bus_gain_db("Sfx", -3.0)
#        model.set_effect_param("Music", 0, 4, -22.0)
#
#    Each call: updates the parsed tree, marks the bus dirty, and
#    schedules a debounced disk write. After SAVE_DEBOUNCE_SEC of
#    inactivity, the write fires and the dirty flag clears.
#
# The write strategy is **targeted byte patching** (one of the four
# locked design calls for v0.28.4). For value edits, only the bytes
# of the value itself are replaced — every byte of every comment,
# every key, every other value, every whitespace character is
# preserved bit-for-bit. For values that don't yet exist in
# config.json (e.g. dragging the fader on a bus whose block has
# no "gain_db" key), the key is inserted with minimal disturbance
# to surrounding formatting.
#
# Topology changes (adding effects, adding buses) are explicitly
# out of scope here — that's the v0.28.5 territory, which will
# need a richer write strategy (bus-block re-serialization).
#
# Safety:
#   - Before every write, the on-disk file is copied to a .bak.
#   - After every write, the new file is re-parsed. If parsing
#     fails, the .bak is restored and the save is reported failed.
#   - On open, if the on-disk mtime has changed since our last
#     read/write, external_change_detected is emitted instead of
#     blindly overwriting — the dock prompts the user to choose
#     Reload-from-disk / Overwrite / Cancel.

@tool
class_name GoolConfigModel
extends RefCounted

const CONFIG_PATH: String = "res://gool/config.json"
const BACKUP_PATH: String = "res://gool/config.json.gool-backup"

# Debounce window: writes batch edits within this many seconds into
# a single disk hit. 500ms is comfortable for slider drags (a long
# drag is many value_changed events) without making the user wait
# noticeably to "see saved" after stopping.
const SAVE_DEBOUNCE_SEC: float = 0.5

# v0.28.4: mapping from engine paramId (audio::EffectParameter::)
# to config.json key (snake_case). Mirror of the dispatch table in
# src/audio_engine/runtime/bus_config_loader.cpp. Must stay in
# sync — if the engine adds a parameter, add a row here too.
const PARAM_ID_TO_JSON_KEY: Dictionary = {
	1:  "gain_db",            # Gain_GainDb
	2:  "cutoff_hz",          # Biquad_CutoffHz
	3:  "q",                  # Biquad_Q
	12: "biquad_gain_db",     # Biquad_GainDb
	4:  "threshold_db",       # Compressor_ThresholdDb
	5:  "ratio",              # Compressor_Ratio
	6:  "attack_ms",          # Compressor_AttackMs
	7:  "release_ms",         # Compressor_ReleaseMs
	8:  "makeup_db",          # Compressor_MakeupDb
	13: "knee_width_db",      # Compressor_KneeWidthDb
	14: "mix_ratio",          # Compressor_MixRatio
	15: "max_reduction_db",   # Compressor_MaxReductionDb
	16: "sidechain_hpf_hz",   # Compressor_SidechainHpfHz
	17: "hold_ms",            # Compressor_HoldMs
	18: "detection_mode",     # Compressor_DetectionMode (string!)
	# Reverb (v0.29.0+ Dattorro plate). IDs 9 and 10 were renamed —
	# room_size → decay, damping → hf_damping — same numeric IDs.
	# 23/24/25 are new.
	9:  "decay",              # Reverb_Decay (was Reverb_RoomSize)
	10: "hf_damping",         # Reverb_HfDamping (was Reverb_Damping)
	11: "wet_gain_db",        # Reverb_WetGainDb
	23: "predelay_ms",        # Reverb_PredelayMs
	24: "lf_damping",         # Reverb_LfDamping
	25: "diffusion",          # Reverb_Diffusion
	26: "dry_gain_db",        # Reverb_DryGainDb (v0.29.5)
	19: "drive",              # Saturation_Drive
	20: "mix",                # Saturation_Mix
	21: "output_gain",        # Saturation_OutputGain
	22: "bias",               # Saturation_Bias
	# v0.69.0: Saturation Mode (string in JSON, like detection_mode)
	# and Tone (numeric, ±1 tilt). Both are existing engine params
	# (Saturation_Mode = 27, Saturation_Tone = 28 in bus.h) that
	# weren't surfaced in the dock until now.
	27: "mode",               # Saturation_Mode (string!)
	28: "tone",               # Saturation_Tone
}

# detection_mode is the one non-numeric param. Engine protocol
# uses 0.0=peak, 1.0=rms; config JSON uses "peak"/"rms" strings.
const _DETECTION_MODE_PARAM_ID: int = 18

# v0.69.0: second non-numeric param — Saturation mode. Engine
# protocol uses 0/1/2/3 (the SaturationMode enum order in bus.h);
# config JSON uses the lowercase strings the bus_config_loader
# parses (see bus_config_loader.cpp around line 322). Index into
# this array IS the engine float value, so they must stay aligned
# with the enum order.
const _SATURATION_MODE_PARAM_ID: int = 27
const _SATURATION_MODE_LABELS: Array = ["tanh", "tube", "tape", "diode"]

# v0.69.0: tone is numeric but uses the upsert write path so dock
# edits land for users whose pre-v0.69.0 saturation blocks didn't
# have a "tone" key. Constant kept here for write-loop dispatch.
const _SATURATION_TONE_PARAM_ID: int = 28

# paramId → engine EffectKind. Used to attach kind/kind_name to
# effects returned by get_effects() when serving rest-time queries.
# Matches the EffectKind enum values (None=0, Gain=1, ...).
const PARAM_ID_TO_KIND: Dictionary = {
	1: 1,                                          # Gain
	2: 2,  3: 2,  12: 2,                           # BiquadFilter
	4: 3,  5: 3,  6: 3,  7: 3,  8: 3,
	13: 3, 14: 3, 15: 3, 16: 3, 17: 3, 18: 3,      # Compressor
	9: 4,  10: 4, 11: 4, 23: 4, 24: 4, 25: 4, 26: 4, # Reverb
	19: 5, 20: 5, 21: 5, 22: 5, 27: 5, 28: 5,      # Saturation
}

# Reverse: config "kind" string → EffectKind int. Used when
# loading effects from config.json.
const KIND_STRING_TO_INT: Dictionary = {
	"gain":           1,
	"biquad":         2,
	"compressor":     3,
	"reverb":         4,
	"saturation":     5,
	"master_control": 6,
}
const KIND_INT_TO_NAME: Dictionary = {
	1: "Gain", 2: "BiquadFilter", 3: "Compressor",
	4: "Reverb", 5: "Saturation", 6: "MasterControl",
}
# v0.64.0: short uppercase pill labels for the mixer dock's FX_BAND
# chain summary (Phase 5 of the UI evolution plan). Mirrors the C++
# binding's _gool_effect_kind_abbreviation in gool_godot.cpp — that
# function serves the runtime path (F5 session running); this table
# serves the editor-time path (config-only, no live runtime). Keep
# the two in sync.
#
# Audience: gool's audience is Godot devs without a strong audio
# background. BiquadFilter → "EQ" is technically lossy but matches
# what users will recognize from every DAW. KIND_INT_TO_NAME above
# remains the accurate display string for the effects-panel header.
const KIND_INT_TO_ABBREV: Dictionary = {
	1: "GAIN", 2: "EQ", 3: "COMP",
	4: "REVERB", 5: "SAT", 6: "MSTR",
}
# NOTE: KIND_INT_TO_JSON_KEYS and KIND_INT_TO_KEY_TO_PARAM_ID below
# do NOT yet have a 6: entry for MasterControl. That means a bus
# with a master_control effect, when queried via get_effects() with
# no live runtime, will surface kind/kind_name/kind_abbrev correctly
# but its params dict will be empty. The FX chain pill in the dock
# renders fine from name+abbrev alone; the gap only affects editor-
# time param introspection. Adding the full param mapping is queued
# for a separate release — needs the 17-parameter master_control
# schema and a matching default-values table mirroring StaticBusConfig.
# Per-kind list of JSON keys that the loader recognizes for that
# kind's parameters. Used by get_effects() to surface only the
# values that actually exist in config (vs. relying on engine
# defaults for keys not present).
const KIND_INT_TO_JSON_KEYS: Dictionary = {
	1: ["gain_db"],
	2: ["cutoff_hz", "q", "biquad_gain_db"],
	3: ["threshold_db", "ratio", "attack_ms", "release_ms",
		"makeup_db", "knee_width_db", "mix_ratio",
		"max_reduction_db", "sidechain_hpf_hz", "hold_ms",
		"detection_mode"],
	# Reverb (v0.29.0): predelay → decay → lf/hf damping → diffusion → dry → wet.
	# Matches PARAM_ORDER_BY_KIND in mixer_dock.gd. Dry added v0.29.5.
	4: ["predelay_ms", "decay", "lf_damping", "hf_damping", "diffusion", "dry_gain_db", "wet_gain_db"],
	5: ["drive", "mix", "output_gain", "bias", "mode", "tone"],
	# v0.64.0: MasterControl param-key list is intentionally empty —
	# see the NOTE under KIND_INT_TO_ABBREV. Surfaces kind/name/abbrev
	# correctly (enough for the FX chain pill to render); editor-time
	# param introspection is queued for a separate release.
	6: [],
}
# Per-kind map: JSON key → paramId. Used by get_effects() to
# build the {paramId: value} dict in the same shape that the
# runtime substrate emits.
const KIND_INT_TO_KEY_TO_PARAM_ID: Dictionary = {
	1: { "gain_db": 1 },
	2: { "cutoff_hz": 2, "q": 3, "biquad_gain_db": 12 },
	3: {
		"threshold_db": 4, "ratio": 5, "attack_ms": 6, "release_ms": 7,
		"makeup_db": 8, "knee_width_db": 13, "mix_ratio": 14,
		"max_reduction_db": 15, "sidechain_hpf_hz": 16, "hold_ms": 17,
		"detection_mode": 18,
	},
	4: {
		"predelay_ms": 23, "decay": 9, "lf_damping": 24,
		"hf_damping": 10, "diffusion": 25, "dry_gain_db": 26,
		"wet_gain_db": 11,
	},
	5: { "drive": 19, "mix": 20, "output_gain": 21, "bias": 22,
		"mode": 27, "tone": 28 },
	# v0.64.0: empty stub — see NOTE under KIND_INT_TO_ABBREV.
	6: {},
}
# Engine compile-time defaults for every paramId, mirroring
# StaticBusConfig defaults in include/audio_engine/bus.h. Used to
# fill in values for params that aren't present in config.json
# (engine would use these defaults at runtime; the dock surfaces
# them so the user sees a complete panel even at rest).
const PARAM_ID_TO_ENGINE_DEFAULT: Dictionary = {
	# Gain
	1: 0.0,
	# BiquadFilter
	2: 1000.0, 3: 0.707, 12: 0.0,
	# Compressor
	4: -18.0, 5: 4.0, 6: 5.0, 7: 50.0, 8: 0.0, 13: 6.0,
	14: 1.0, 15: 60.0, 16: 0.0, 17: 0.0, 18: 0.0,
	# Reverb (v0.29.0 Dattorro defaults — mirror EffectConfig in bus.h).
	# v0.29.5 added id 26 (dry_gain_db) at default 0 dB (unity passthrough).
	9: 0.5, 10: 0.3, 11: 0.0, 23: 30.0, 24: 0.0, 25: 0.625, 26: 0.0,
	# Saturation
	19: 1.0, 20: 0.0, 21: 1.0, 22: 0.0,
}


# ---- Signals --------------------------------------------------------

# Emitted after a successful load_from_disk(). Dock listens to rebuild
# strips from the model when this fires.
signal model_loaded

# Emitted after a successful save_to_disk(). Dock listens to clear
# the dirty indicator.
signal model_saved(bus_names_saved: Array)

# Emitted when save_to_disk() failed — disk error, parse-after-write
# corruption, etc. The failure reason is human-readable; the dock
# surfaces it as a warning.
signal save_failed(reason: String)

# Emitted when an attempted save detected an external change on disk
# (mtime newer than what we last saw). The dock prompts the user.
# pending_dirty_buses is the list of buses with unsaved local edits.
signal external_change_detected(pending_dirty_buses: Array)

# v0.80.12: emitted when _do_save discovers config.json has been
# removed externally (renamed, deleted) since the dock last loaded
# it — i.e. `not FileAccess.file_exists(CONFIG_PATH)` while
# `_raw_text` still holds previously-loaded content. Pre-v0.80.12
# this condition was silent: _do_save's existence-guards skipped
# external-change detection AND backup, then proceeded to write
# the in-memory state, recreating config.json from under the
# user. The mixer dock listens for this signal and surfaces the
# choice between accepting the removal (discard in-memory edits,
# go empty-state) and recreating the file from dock state.
signal external_removal_detected(pending_dirty_buses: Array)


# ---- State ---------------------------------------------------------

# Last full file text we read from disk. Used as the basis for
# every patching operation. After a successful save_to_disk this
# is updated to the new contents.
var _raw_text: String = ""

# Parsed JSON dictionary. Updated in-place by set_bus_gain_db /
# set_effect_param so queries against the model reflect pending
# edits immediately (before the debounced save fires).
var _parsed: Dictionary = {}

# unix mtime of the file at last read/write. Compared against the
# current on-disk mtime before each save to detect external edits.
var _last_seen_mtime: int = 0

# Bus names with edits not yet persisted to disk. Cleared on save.
var _dirty_buses: Dictionary = {}  # name → true

# True when at least one set_* call has been made since the last
# save; used to decide whether the debounce timer needs to fire.
var _has_pending_save: bool = false

# Tree node that owns the debounce SceneTreeTimer. Set by the
# dock via set_owner_node so we can call get_tree() from a
# RefCounted (which isn't in the tree itself).
var _owner_node: Node = null

# Reference to the currently-running debounce timer, kept so a
# new edit can cancel and restart it (sliding window). May be null.
var _save_timer: SceneTreeTimer = null


# ---- Public API ----------------------------------------------------

# Set by the dock after construction; gives us a Node we can use
# to call get_tree() and schedule the debounce timer.
func set_owner_node(n: Node) -> void:
	_owner_node = n


# Read config.json, parse it, cache raw text + mtime. Returns OK
# on success, ERR_* on failure. Failure modes: file missing,
# unreadable, or unparseable JSON. The model is left in a known
# empty state on failure so subsequent get_* calls return safe
# defaults.
func load_from_disk() -> int:
	_raw_text = ""
	_parsed = {}
	_last_seen_mtime = 0
	_dirty_buses.clear()
	_buses_array_dirty = false
	_has_pending_save = false

	if not FileAccess.file_exists(CONFIG_PATH):
		return ERR_FILE_NOT_FOUND
	var f := FileAccess.open(CONFIG_PATH, FileAccess.READ)
	if f == null:
		return ERR_CANT_OPEN
	_raw_text = f.get_as_text()
	f.close()

	var parsed_v: Variant = JSON.parse_string(_raw_text)
	if parsed_v == null or not (parsed_v is Dictionary):
		return ERR_PARSE_ERROR
	_parsed = parsed_v

	_last_seen_mtime = FileAccess.get_modified_time(CONFIG_PATH)
	model_loaded.emit()
	return OK


# Return the list of bus dictionaries as parsed from config.json,
# in their config-declaration order. Each entry is the dict that
# was in the "buses" array. Safe to call before/after load — empty
# array if not yet loaded.
func get_buses() -> Array:
	var arr_v: Variant = _parsed.get("buses", [])
	if not (arr_v is Array):
		return []
	return arr_v


# Return a single bus's parsed dict by name, or {} if not found.
func get_bus(name: String) -> Dictionary:
	for b in get_buses():
		if b is Dictionary and String(b.get("name", "")) == name:
			return b
	return {}


# Build a runtime-compatible effects array for a bus, derived from
# config.json. Same shape the runtime substrate emits on the
# gool:bus_stats channel:
#   [{kind: int, kind_name: String, params: {paramId: value}}, ...]
#
# For params present in config.json: their parsed values.
# For params not present in config.json: the engine compile-time
# default from PARAM_ID_TO_ENGINE_DEFAULT — so the dock at rest
# shows a complete panel, not a half-populated one.
func get_effects(bus_name: String) -> Array:
	var bus := get_bus(bus_name)
	if bus.is_empty():
		return []
	var raw_v: Variant = bus.get("effects", [])
	if not (raw_v is Array):
		return []
	var out: Array = []
	for e in raw_v:
		if not (e is Dictionary):
			continue
		var kind_s: String = String(e.get("kind", "")).to_lower()
		if not KIND_STRING_TO_INT.has(kind_s):
			continue
		var kind_i: int = int(KIND_STRING_TO_INT[kind_s])
		var keys_for_kind: Array = KIND_INT_TO_JSON_KEYS[kind_i] as Array
		var key_to_pid: Dictionary = KIND_INT_TO_KEY_TO_PARAM_ID[kind_i] as Dictionary
		var params: Dictionary = {}
		for k in keys_for_kind:
			var pid: int = int(key_to_pid[k])
			if e.has(k):
				if pid == _DETECTION_MODE_PARAM_ID:
					params[pid] = 1.0 if String(e[k]).to_lower() == "rms" else 0.0
				elif pid == _SATURATION_MODE_PARAM_ID:
					# v0.69.0: Saturation mode is a string in JSON
					# ("tanh"/"tube"/"tape"/"diode") but a float in
					# the engine protocol (0..3, matching the
					# SaturationMode enum order). Unknown strings
					# fall back to 0.0 (tanh) — the engine parser
					# would have rejected them at load time, so
					# this branch only sees valid values in
					# practice, but the fallback is defensive.
					var idx: int = _SATURATION_MODE_LABELS.find(
							String(e[k]).to_lower())
					params[pid] = float(idx if idx >= 0 else 0)
				else:
					params[pid] = float(e[k])
			else:
				params[pid] = float(PARAM_ID_TO_ENGINE_DEFAULT.get(pid, 0.0))
		out.append({
			"kind":        kind_i,
			"kind_name":   KIND_INT_TO_NAME[kind_i],
			"kind_abbrev": KIND_INT_TO_ABBREV[kind_i],
			"params":      params,
		})
	return out


# Editing API. Updates the in-memory _parsed tree, marks the bus
# dirty, schedules a debounced save. The dock calls these from
# its existing _on_strip_db_changed / _on_effect_param_changed
# handlers; the runtime forwarding (send_set_bus_gain etc) stays
# separate so live edits affect a running game immediately.
func set_bus_gain_db(bus_name: String, db: float) -> void:
	var b := get_bus(bus_name)
	if b.is_empty():
		return
	b["gain_db"] = db
	_mark_dirty(bus_name)


func set_effect_param(bus_name: String, effect_index: int,
		param_id: int, value: float) -> void:
	var b := get_bus(bus_name)
	if b.is_empty():
		return
	var effects_v: Variant = b.get("effects", [])
	if not (effects_v is Array):
		return
	var effects: Array = effects_v
	if effect_index < 0 or effect_index >= effects.size():
		return
	var e_v: Variant = effects[effect_index]
	if not (e_v is Dictionary):
		return
	var e: Dictionary = e_v
	if not PARAM_ID_TO_JSON_KEY.has(param_id):
		return
	var key: String = String(PARAM_ID_TO_JSON_KEY[param_id])
	if param_id == _DETECTION_MODE_PARAM_ID:
		e[key] = "rms" if value >= 0.5 else "peak"
	elif param_id == _SATURATION_MODE_PARAM_ID:
		# v0.69.0: round-trip the OptionButton's int selection
		# back into the lowercase string the engine parser expects.
		# Clamp defensively — the dock should never emit out-of-
		# range values for a 4-choice OptionButton, but defensive
		# clamping costs nothing and prevents an out-of-bounds.
		var idx: int = clampi(int(value), 0,
				_SATURATION_MODE_LABELS.size() - 1)
		e[key] = String(_SATURATION_MODE_LABELS[idx])
	else:
		e[key] = value
	_mark_dirty(bus_name)


# True if this bus has unsaved local edits.
func is_bus_dirty(bus_name: String) -> bool:
	return _dirty_buses.has(bus_name)


# Force an immediate save, bypassing the debounce timer. Used for
# things like editor shutdown, F5-start handoff, or an explicit
# "Save now" button. Returns OK on success.
func force_save() -> int:
	if _save_timer != null:
		# Don't bother canceling — we'll early-out in the callback
		# via _has_pending_save once we clear it below.
		_save_timer = null
	if not _has_pending_save:
		return OK  # nothing to save
	return _do_save()


# ---- Internal: dirty tracking + debounce --------------------------

func _mark_dirty(bus_name: String) -> void:
	# v0.28.8: don't downgrade a topology-dirty bus to value-dirty.
	# Topology re-serialization subsumes value patches.
	if _dirty_buses.get(bus_name) == _DIRTY_TOPOLOGY:
		_has_pending_save = true
		_schedule_save()
		return
	_dirty_buses[bus_name] = _DIRTY_VALUE
	_has_pending_save = true
	_schedule_save()


func _schedule_save() -> void:
	if _owner_node == null:
		# No tree — can't schedule. Save will happen on force_save
		# (typically from a manual Save action). This path is hit
		# in unit tests or before the dock has wired us up.
		return
	# Cancel any existing pending save by detaching from it.
	# Godot's SceneTreeTimer has no cancel(); we just ignore its
	# timeout when _has_pending_save flips back to false, and use
	# the latest scheduled time-out as the only one that fires
	# the save. The _save_timer reference helps a future cancel-
	# on-shutdown path but currently we let stale timers tick.
	_save_timer = _owner_node.get_tree().create_timer(SAVE_DEBOUNCE_SEC)
	_save_timer.timeout.connect(_on_save_timer_fired.bind(_save_timer),
			CONNECT_ONE_SHOT)


func _on_save_timer_fired(timer_ref: SceneTreeTimer) -> void:
	# Only the LAST-scheduled timer's callback should actually save.
	# Earlier timers fire too (we can't cancel SceneTreeTimers in
	# Godot 4) — they're ignored here via the identity check.
	if timer_ref != _save_timer:
		return
	if not _has_pending_save:
		return
	_do_save()


# ---- Internal: the actual save ------------------------------------

# --- v0.80.20: Unified config writer (Cluster B #26) ------------
#
# Before v0.80.20, FIVE different code paths wrote res://gool/config.json:
#
#   1. ConfigModel._do_save       (canonical dirty-tracked save)
#   2. mixer_dock _on_create_default_config_pressed   (empty-state)
#   3. mixer_dock _on_use_fps_template_pressed         (empty-state)
#   4. getting_started_banner _on_use_fps_template     (banner)
#   5. getting_started_banner _write_minimal_template_and_finish (banner)
#
# Only #1 had the full safety stack: external-change pre-flight, backup,
# JSON verify before write, disk readback for _raw_text sync, _parsed
# re-population. The other four wrote directly via store_string or
# dir.copy, skipping some or all of those. That meant template installs
# could clobber unsaved edits without warning, could silently produce
# unparseable JSON, and required callers to manually call
# load_from_disk() afterward to refresh _parsed.
#
# v0.80.20 collapses all five paths through write_config below. _do_save
# becomes a thin wrapper that builds new_text via the patcher and then
# delegates. The empty-state and banner paths call install_config_text
# (which is write_config + force=true + model_loaded emission). Five
# code paths → ONE bedrock writer.
#
# Why store_buffer instead of store_string: store_string applies the
# platform's default text-mode line-ending conversion (LF→CRLF on
# Windows). store_buffer writes raw UTF-8 bytes verbatim. Using
# store_buffer gives us byte-for-byte fidelity — the same property
# v0.80.8's dir.copy fix preserved for the FPS-template install — so
# the unified path doesn't lose anything the special-case template
# copy used to gain. As a side effect, the v0.80.14 false-positive
# external-change-after-our-own-write issue is structurally
# impossible now (we're not transforming bytes during write); the
# defensive readback stays as belt-and-suspenders against any future
# encoding edge case.


# Source-of-truth writer for res://gool/config.json. ALL paths that
# need to persist config state — whether canonical dirty-tracked
# saves, empty-state template installs, or recovery overwrites — go
# through this method.
#
# What it does:
#   1. Pre-flight: detect external removal (config.json gone but
#      _raw_text remembers content) → emit external_removal_detected,
#      return ERR_FILE_NOT_FOUND. Caller must reconcile.
#   2. Pre-flight: detect external content change (disk doesn't match
#      _raw_text) → emit external_change_detected, return
#      ERR_FILE_UNRECOGNIZED. Caller must reconcile.
#   3. Backup existing config.json to config.json.gool-backup (if
#      config.json exists). Skipped on first install.
#   4. Verify new_text parses as JSON before touching disk. Bad
#      candidates get dumped to config.json.failed for diagnosis;
#      live config.json is untouched.
#   5. Write via store_buffer for byte-for-byte fidelity (no platform
#      EOL conversion).
#   6. Re-read disk for _raw_text sync.
#   7. Re-parse for _parsed sync.
#   8. Clear dirty state, refresh _last_seen_mtime.
#
# What it does NOT do:
#   - Emit signals to listeners. Callers control which signal fires
#     (model_saved for canonical _do_save, model_loaded for fresh-
#     install paths). This keeps the contract uncoupled from caller
#     intent.
#
# Pass force=true to skip the external-removal / external-change
# pre-flight checks. Empty-state and template-install callers use
# this because they intend to overwrite whatever's on disk
# (typically nothing or a stale template). Canonical _do_save uses
# force=false to protect the user from clobbering external edits.
#
# `source_label` is purely informational — appears in error
# messages and the .failed sidecar so log readers can tell which
# path triggered the write.
#
# Returns one of:
#   - OK: write succeeded; _raw_text + _parsed + dirty state synced
#   - ERR_FILE_NOT_FOUND: external removal detected (force=false)
#   - ERR_FILE_UNRECOGNIZED: external change detected (force=false)
#   - ERR_CANT_CREATE: backup failed, or res://gool/ dir creation failed
#   - ERR_INVALID_DATA: new_text isn't parseable JSON
#   - ERR_CANT_OPEN: couldn't open config.json for write
func write_config(new_text: String, source_label: String,
		force: bool = false) -> int:
	# Pre-flight 1: external removal
	if not force and not FileAccess.file_exists(CONFIG_PATH) \
			and not _raw_text.is_empty():
		var dirty_list_removal: Array = _dirty_buses.keys()
		external_removal_detected.emit(dirty_list_removal)
		return ERR_FILE_NOT_FOUND

	# Pre-flight 2: external content change. v0.28.6: content-based,
	# not mtime-based — Godot's filesystem watcher / resource cache
	# was bumping mtime after our own writes and producing false
	# positives on every save.
	if not force and FileAccess.file_exists(CONFIG_PATH):
		var f_check := FileAccess.open(CONFIG_PATH, FileAccess.READ)
		if f_check != null:
			var disk_text: String = f_check.get_as_text()
			f_check.close()
			if disk_text != _raw_text and not _raw_text.is_empty():
				var dirty_list_change: Array = _dirty_buses.keys()
				external_change_detected.emit(dirty_list_change)
				return ERR_FILE_UNRECOGNIZED

	# Backup. Skipped on first install (no existing file to back up).
	if FileAccess.file_exists(CONFIG_PATH):
		if not _copy_file(CONFIG_PATH, BACKUP_PATH):
			save_failed.emit("could not create %s backup [%s]"
					% [BACKUP_PATH, source_label])
			return ERR_CANT_CREATE

	# Verify new_text BEFORE touching live config. Pre-v0.54.3 order
	# was write → verify → restore-from-bak; that left a window
	# where corrupted JSON was on disk. New order: parse in memory
	# first, only open for write after we know the candidate is
	# valid.
	var verify_v: Variant = JSON.parse_string(new_text)
	if verify_v == null or not (verify_v is Dictionary):
		var failed_path: String = "res://gool/config.json.failed"
		var f_dump := FileAccess.open(failed_path, FileAccess.WRITE)
		if f_dump != null:
			f_dump.store_string(new_text)
			f_dump.close()
		save_failed.emit(
				"save aborted [%s]: candidate is not valid JSON. "
				+ "res://gool/config.json is UNCHANGED. Failing "
				+ "candidate dumped to %s for diagnosis."
				% [source_label, failed_path])
		return ERR_INVALID_DATA

	# Ensure res://gool/ exists. New projects don't have it yet
	# until the first install runs.
	if not _ensure_gool_dir():
		save_failed.emit("could not create res://gool/ directory [%s]"
				% source_label)
		return ERR_CANT_CREATE

	# Write. store_buffer (not store_string) — see header comment.
	var f := FileAccess.open(CONFIG_PATH, FileAccess.WRITE)
	if f == null:
		save_failed.emit("could not open %s for write [%s]"
				% [CONFIG_PATH, source_label])
		return ERR_CANT_OPEN
	f.store_buffer(new_text.to_utf8_buffer())
	f.close()

	# Sync internal state from canonical disk content. The readback
	# is structurally redundant with store_buffer (which doesn't
	# transform bytes), but it's belt-and-suspenders for BOMs,
	# future encoding quirks, and any path-specific filesystem
	# layer that might intervene.
	var f_readback := FileAccess.open(CONFIG_PATH, FileAccess.READ)
	if f_readback != null:
		_raw_text = f_readback.get_as_text()
		f_readback.close()
	else:
		# Read-back failed (very unlikely — we JUST wrote it). Fall
		# back to new_text so in-memory state stays self-consistent.
		_raw_text = new_text

	# Re-parse for _parsed sync. We already parsed once for verify;
	# reuse that result rather than parsing again.
	_parsed = verify_v as Dictionary

	_last_seen_mtime = FileAccess.get_modified_time(CONFIG_PATH)
	_dirty_buses.clear()
	_buses_array_dirty = false
	_has_pending_save = false
	return OK


# Install fresh config content, replacing whatever's on disk. Thin
# wrapper for empty-state and template-install callers — write_config
# with force=true (the install intent IS overwrite, not preserve),
# plus a model_loaded emission so the dock rebuilds from the new
# _parsed.
#
# Use this from:
#   - "Create default config" buttons (empty-state)
#   - "Use FPS template" / "Use minimal template" buttons
#   - Any "overwrite on-disk state with this content" UX
#
# Do NOT use this from canonical save paths. The canonical path is
# _do_save (which uses write_config with force=false), respecting
# external-change protection.
func install_config_text(new_text: String, source_label: String) -> int:
	var result: int = write_config(new_text, source_label, true)
	if result == OK:
		model_loaded.emit()
	return result


# Helper: ensure res://gool/ exists. Idempotent; returns true if the
# directory exists (or was just created), false on any failure.
func _ensure_gool_dir() -> bool:
	var dir := DirAccess.open("res://")
	if dir == null:
		return false
	if not dir.dir_exists("gool"):
		var err: int = dir.make_dir("gool")
		if err != OK:
			return false
	return true


# Returns OK on success, or an Error code on failure. Emits
# model_saved or save_failed accordingly. Also emits
# external_change_detected if disk contents differ from what we
# last saw.
#
# v0.80.20: the entire safety stack (external-removal pre-flight,
# external-change pre-flight, backup, JSON verify, store_buffer
# write, readback, state sync) now lives in write_config — see the
# header comment there. _do_save is the canonical *dirty-tracked*
# save: it runs the patcher to convert _parsed edits into a new
# JSON text, snapshots the dirty bus list (because write_config
# clears it), delegates the actual write, and emits model_saved
# on success.
func _do_save() -> int:
	# Apply every dirty bus's pending edits to the raw text. The
	# walk uses the current _parsed (which already reflects all
	# the user's edits) as the source of values; the raw text is
	# what we patch.
	#
	# v0.28.8: dispatch by dirty type:
	#   - _buses_array_dirty (add/remove bus): re-serialize the
	#     whole buses array; subsumes any per-bus dirty flags.
	#   - per-bus topology: re-serialize that bus's {...} block.
	#   - per-bus value: existing targeted byte patcher.
	var new_text: String = _raw_text
	var write_errors: Array = []
	if _buses_array_dirty:
		new_text = _re_serialize_buses_array(new_text, write_errors)
	else:
		for bus_name in _dirty_buses.keys():
			var dirty_type: Variant = _dirty_buses[bus_name]
			if dirty_type == _DIRTY_TOPOLOGY:
				new_text = _re_serialize_bus_block(
						new_text, String(bus_name), write_errors)
			else:
				new_text = _patch_bus_in_text(
						new_text, String(bus_name), write_errors)
	if not write_errors.is_empty():
		# At least one bus failed to patch (key not found and
		# insertion also failed somehow). Don't write a partial
		# result. Surface the error and abort.
		save_failed.emit("patch errors: " + ", ".join(write_errors))
		return ERR_INVALID_DATA

	# Snapshot the dirty list BEFORE write_config — it clears
	# _dirty_buses as part of the post-write state sync, so we
	# couldn't read the list after the call for the signal payload.
	var saved_buses: Array = _dirty_buses.keys()

	# Delegate the safe-write stack. force=false because canonical
	# saves MUST respect the external-change / external-removal
	# protection (the user might have edited config.json outside
	# the editor since we loaded).
	var result: int = write_config(new_text, "dirty-tracked save", false)
	if result == OK:
		model_saved.emit(saved_buses)
	return result


# Force-reload from disk, dropping any pending local edits. Used
# by the mtime-conflict prompt's "Reload" option.
func reload_from_disk_discarding_edits() -> int:
	return load_from_disk()


# Force-write our local edits over disk, ignoring mtime conflict.
# Used by the mtime-conflict prompt's "Overwrite" option.
func overwrite_disk() -> int:
	_last_seen_mtime = 0  # disables the mtime check on next _do_save
	return _do_save()


# ---- Internal: the patcher ----------------------------------------

# Patch a single bus's persistable values in `text`. Returns the
# new text. On any patch sub-failure, appends a human-readable
# reason to `errors` and returns text unchanged.
#
# Strategy:
# 1. Locate the bus's JSON block in the text by its "name" key.
# 2. Within the block, find or insert "gain_db".
# 3. Within the block's "effects" array, for each effect, find or
#    insert each param's JSON key for params that have a value in
#    the parsed model.
#
# Effects array TOPOLOGY is read-only here — we don't add or
# remove effect entries, only edit values inside existing ones.
# Topology edits are v0.28.5 territory.
func _patch_bus_in_text(text: String, bus_name: String,
		errors: Array) -> String:
	var bus_range: Vector2i = _find_bus_block_range(text, bus_name)
	if bus_range.x < 0:
		errors.append("bus '%s' not found in text" % bus_name)
		return text

	var bus_dict := get_bus(bus_name)
	if bus_dict.is_empty():
		errors.append("bus '%s' not in model" % bus_name)
		return text

	# Step 1: patch gain_db (or insert if missing). Only if the
	# model has a gain_db value — otherwise leave the file alone.
	if bus_dict.has("gain_db"):
		var gain_value: float = float(bus_dict["gain_db"])
		text = _patch_or_insert_number_in_range(
				text, bus_range, "gain_db", gain_value, errors)
		# bus_range may have shifted if we inserted bytes; recompute.
		bus_range = _find_bus_block_range(text, bus_name)
		if bus_range.x < 0:
			errors.append("bus block lost after gain_db patch")
			return text

	# Step 2: patch effects. Iterate the bus's effects array.
	var effects_v: Variant = bus_dict.get("effects", [])
	if not (effects_v is Array):
		return text
	var effects: Array = effects_v

	for i in effects.size():
		var e_v: Variant = effects[i]
		if not (e_v is Dictionary):
			continue
		var effect: Dictionary = e_v
		var effect_range: Vector2i = _find_nth_effect_block_range(
				text, bus_range, i)
		if effect_range.x < 0:
			errors.append("bus '%s' effect %d not found in text"
					% [bus_name, i])
			continue
		# Patch each known persistable key in this effect block.
		var kind_s: String = String(effect.get("kind", "")).to_lower()
		if not KIND_STRING_TO_INT.has(kind_s):
			continue
		var kind_i: int = int(KIND_STRING_TO_INT[kind_s])
		var keys_for_kind: Array = KIND_INT_TO_JSON_KEYS[kind_i] as Array
		for key in keys_for_kind:
			if not effect.has(key):
				continue  # don't insert keys that weren't there originally
			var pid: int = int(KIND_INT_TO_KEY_TO_PARAM_ID[kind_i][key])
			if pid == _DETECTION_MODE_PARAM_ID:
				var mode_s: String = String(effect[key])
				text = _patch_string_in_range(
						text, effect_range, key, mode_s, errors)
			elif pid == _SATURATION_MODE_PARAM_ID:
				# v0.69.0: use the upsert variant so dock edits to
				# the saturation mode persist even for users whose
				# pre-v0.69.0 saturation blocks didn't have a "mode"
				# key. The effect dict already contains "mode" by
				# this point (set_effect_param adds it), so the
				# insert branch fires when the JSON itself is missing
				# the key.
				var smode_s: String = String(effect[key])
				text = _patch_or_insert_string_in_range(
						text, effect_range, key, smode_s, errors)
			elif pid == _SATURATION_TONE_PARAM_ID:
				# Same reasoning as mode — tone is a v0.69.0 add,
				# user configs predating it won't have the key,
				# upsert handles both insert and update.
				var tv: float = float(effect[key])
				text = _patch_or_insert_number_in_range(
						text, effect_range, key, tv, errors)
			else:
				var v: float = float(effect[key])
				text = _patch_number_in_range(
						text, effect_range, key, v, errors)
			# Re-locate effect range since size may have changed.
			bus_range = _find_bus_block_range(text, bus_name)
			if bus_range.x < 0:
				return text
			effect_range = _find_nth_effect_block_range(
					text, bus_range, i)
			if effect_range.x < 0:
				return text

	return text


# Find the JSON byte range [start, end) of the bus whose "name"
# field matches `bus_name`. Returns (-1, -1) on miss.
#
# Algorithm: scan for "name"\s*:\s*"<bus_name>" in the text. Walk
# back from that match to find the opening "{" of the bus's
# block, and forward to find the matching closing "}". Brace
# counting is done on the OUTSIDE-of-strings characters only —
# JSON strings may contain "{" or "}".
func _find_bus_block_range(text: String, bus_name: String) -> Vector2i:
	# Walk char-by-char tracking string state. When we find a literal
	# `"name"` (the 6-byte sequence) OUTSIDE any string, parse the
	# value that follows, and either return the enclosing bus block
	# range (on match) or skip past the value (on mismatch) without
	# losing track of which chars are inside vs. outside strings.
	var i: int = 0
	var in_string: bool = false
	while i < text.length():
		var ch: String = text.substr(i, 1)
		if in_string:
			if ch == "\\":
				i += 2
				continue
			if ch == '"':
				in_string = false
			i += 1
			continue
		if ch == '"':
			if text.substr(i, 6) == '"name"':
				# After "name", walk to the colon then the opening
				# quote of the bus name value.
				var j: int = i + 6
				while j < text.length() and text.substr(j, 1) in [" ", "\t", "\n", "\r"]:
					j += 1
				if j < text.length() and text.substr(j, 1) == ":":
					j += 1
					while j < text.length() and text.substr(j, 1) in [" ", "\t", "\n", "\r"]:
						j += 1
					if j < text.length() and text.substr(j, 1) == '"':
						# Parse the bus name value, honoring backslash escapes.
						var k: int = j + 1
						var parsed_chars: Array = []
						while k < text.length():
							var c: String = text.substr(k, 1)
							if c == "\\":
								if k + 1 < text.length():
									parsed_chars.append(text.substr(k + 1, 1))
								k += 2
								continue
							if c == '"':
								break
							parsed_chars.append(c)
							k += 1
						var parsed_name: String = "".join(parsed_chars)
						if k < text.length() and parsed_name == bus_name:
							var brace_open: int = _find_enclosing_brace_open(text, i)
							if brace_open < 0:
								return Vector2i(-1, -1)
							var brace_close: int = _find_matching_brace_close(text, brace_open)
							if brace_close < 0:
								return Vector2i(-1, -1)
							return Vector2i(brace_open, brace_close + 1)
						# Mismatch: skip PAST the closing quote so the
						# outer scanner doesn't re-enter the bus name
						# value string in confused string-state mode.
						i = (k + 1) if k < text.length() else text.length()
						continue
			in_string = true
		i += 1
	return Vector2i(-1, -1)


# Within `bus_range` (the {} range of a bus block), find the Nth
# effect's {} range. Counts immediate children of the "effects"
# array, skipping nested objects. Returns (-1, -1) on miss.
func _find_nth_effect_block_range(text: String, bus_range: Vector2i,
		n: int) -> Vector2i:
	var slice: String = text.substr(
			bus_range.x, bus_range.y - bus_range.x)
	var effects_at: int = slice.find('"effects"')
	if effects_at < 0:
		return Vector2i(-1, -1)
	# Find the "[" that opens the effects array.
	var bracket_open_rel: int = slice.find("[", effects_at)
	if bracket_open_rel < 0:
		return Vector2i(-1, -1)
	# Walk forward, counting top-level "{" inside the array. The
	# Nth one (0-indexed) is the one we want. Skip everything
	# inside strings.
	var i: int = bracket_open_rel + 1
	var top_level_starts: Array = []
	var in_string: bool = false
	var brace_depth: int = 0
	while i < slice.length():
		var ch: String = slice.substr(i, 1)
		if in_string:
			if ch == "\\":
				i += 2
				continue
			if ch == '"':
				in_string = false
			i += 1
			continue
		if ch == '"':
			in_string = true
			i += 1
			continue
		if ch == "{":
			if brace_depth == 0:
				top_level_starts.append(i)
			brace_depth += 1
			i += 1
			continue
		if ch == "}":
			brace_depth -= 1
			i += 1
			continue
		if ch == "]" and brace_depth == 0:
			break
		i += 1
	if n >= top_level_starts.size():
		return Vector2i(-1, -1)
	var effect_open_rel: int = int(top_level_starts[n])
	var effect_close_rel: int = _find_matching_brace_close(slice, effect_open_rel)
	if effect_close_rel < 0:
		return Vector2i(-1, -1)
	# Convert back to absolute positions.
	return Vector2i(
			bus_range.x + effect_open_rel,
			bus_range.x + effect_close_rel + 1)


# Replace OR insert a numeric key inside a JSON block range.
func _patch_or_insert_number_in_range(text: String, block_range: Vector2i,
		key: String, value: float, errors: Array) -> String:
	if _find_key_in_block(text, block_range, key) >= 0:
		return _patch_number_in_range(text, block_range, key, value, errors)
	return _insert_pair_in_block(text, block_range, key,
			_format_number(value), errors)


# v0.69.0: Replace OR insert a string key inside a JSON block
# range. Parallel to the number version, used for saturation mode
# so dock edits land even on configs that predate the v0.69.0
# mode/tone exposure (and so didn't include the keys originally).
# detection_mode intentionally keeps the strict UPDATE-only
# variant — its v0.27.0-era behavior hasn't changed and changing
# it now risks subtle regressions in existing compressor configs.
func _patch_or_insert_string_in_range(text: String, block_range: Vector2i,
		key: String, value: String, errors: Array) -> String:
	if _find_key_in_block(text, block_range, key) >= 0:
		return _patch_string_in_range(text, block_range, key, value, errors)
	# _insert_pair_in_block takes the value as a pre-formatted
	# string; for a JSON string we need to wrap in quotes.
	return _insert_pair_in_block(text, block_range, key,
			'"' + value + '"', errors)


# Replace the value of an EXISTING numeric key in a block.
func _patch_number_in_range(text: String, block_range: Vector2i,
		key: String, value: float, errors: Array) -> String:
	var value_range: Vector2i = _find_value_range_for_key(
			text, block_range, key)
	if value_range.x < 0:
		errors.append("key '%s' not found in block" % key)
		return text
	return text.substr(0, value_range.x) \
			+ _format_number(value) \
			+ text.substr(value_range.y)


# Replace the value of an EXISTING string key in a block.
# Used for detection_mode ("peak" / "rms"). New value is JSON-
# quoted; embedded quotes in `value` are not expected (only "peak"
# and "rms" reach this path) and aren't escaped.
func _patch_string_in_range(text: String, block_range: Vector2i,
		key: String, value: String, errors: Array) -> String:
	var value_range: Vector2i = _find_value_range_for_key(
			text, block_range, key)
	if value_range.x < 0:
		errors.append("key '%s' not found in block" % key)
		return text
	return text.substr(0, value_range.x) \
			+ '"' + value + '"' \
			+ text.substr(value_range.y)


# Format a float as a JSON-acceptable number string. The integer-
# valued case uses "%0.1f" so we always emit a decimal point (e.g.
# 6.0 → "6.0" not "6") — keeps the type tag consistent with the
# rest of the config and round-trips cleanly through JSON parsers.
#
# v0.28.7: the non-integer branch was "%g" % value, which Python's
# % operator supports but GDScript's DOES NOT. GDScript supports
# %s %d %f %c %o %x %X %v — no %g. When the user dragged a fader
# to a non-integer value like -21.1, this would fire two errors:
#   "String formatting error: unsupported format character"
# and the expression would return garbage, which got inserted into
# the JSON and failed parse-verify. The .bak restore covered for
# it but every save with a non-integer value silently failed.
# Fix: String.num(value) uses Godot's default float-to-string
# conversion which handles arbitrary precision and trims trailing
# zeros (-21.1 → "-21.1", 0.707 → "0.707").
func _format_number(value: float) -> String:
	# v0.54.3: defensive validation. The patched-save path is the
	# only place that emits float-to-JSON, and corrupted output
	# (e.g. literal "%g" from a v0.28.7-era bug, or "nan"/"inf" from
	# IEEE 754 sentinels) is unsalvageable downstream — the C++ JSON
	# parser rejects the whole config. Clamp non-finite values to 0.0
	# and surface a warning so the bug source is visible in the
	# editor log rather than dying silently in a .failed dump.
	if is_nan(value) or is_inf(value):
		push_warning(
				"[gool] config_model: refusing to serialize non-finite "
				+ "value (%s); clamping to 0.0. "
				+ "This indicates a bug — the model shouldn't be carrying "
				+ "NaN or Inf. Report the steps that produced it."
				% str(value))
		return "0.0"
	var s: String
	if value == floor(value) and absf(value) < 1.0e15:
		s = "%0.1f" % value
	else:
		s = String.num(value)
	# Belt-and-suspenders: validate the output IS a JSON number
	# before returning. If a future Godot version or platform quirk
	# makes String.num emit something the JSON parser would reject,
	# we catch it here rather than letting it reach disk.
	if not _is_valid_json_number(s):
		push_warning(
				"[gool] config_model: serializer produced non-JSON "
				+ "output '%s' for value %f; clamping to 0.0. "
				+ "This indicates a bug — please report the steps."
				% [s, value])
		return "0.0"
	return s


# JSON number grammar (RFC 8259 §6):
#   number = [ minus ] int [ frac ] [ exp ]
#   int    = zero / ( digit1-9 *DIGIT )
#   frac   = decimal-point 1*DIGIT
#   exp    = e [ minus / plus ] 1*DIGIT
# Used by _format_number to validate its own output. Catches the
# %g class of bug (literal "%g" wouldn't match anywhere in this
# grammar) as well as any "nan"/"inf"/"infinity" that might slip
# through from String.num on some platforms.
func _is_valid_json_number(s: String) -> bool:
	if s.is_empty():
		return false
	var re := RegEx.new()
	re.compile("^-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?$")
	return re.search(s) != null


# Locate `key` (a JSON string field name) inside `block_range`.
# Returns the absolute byte position of the opening quote of the
# key, or -1 if not present. Only finds at the TOP nesting level
# of the block — keys nested inside child objects are not
# returned. This is enough for our patcher because we always call
# this on a known-flat scope (a bus block's top level, or a
# single effect's top level).
func _find_key_in_block(text: String, block_range: Vector2i,
		key: String) -> int:
	var slice: String = text.substr(
			block_range.x, block_range.y - block_range.x)
	var needle: String = '"' + key + '"'
	var i: int = 0
	var in_string: bool = false
	var brace_depth: int = 0
	var bracket_depth: int = 0
	while i < slice.length():
		var ch: String = slice.substr(i, 1)
		if in_string:
			if ch == "\\":
				i += 2
				continue
			if ch == '"':
				in_string = false
			i += 1
			continue
		if ch == '"':
			# Are we at the start of `needle` at top level?
			if brace_depth == 1 and bracket_depth == 0 \
					and slice.substr(i, needle.length()) == needle:
				# Make sure the character after the closing quote
				# is a colon (so we matched a key, not a value).
				var after: int = i + needle.length()
				var j: int = after
				while j < slice.length() and slice.substr(j, 1) in [" ", "\t", "\n", "\r"]:
					j += 1
				if j < slice.length() and slice.substr(j, 1) == ":":
					return block_range.x + i
			in_string = true
			i += 1
			continue
		if ch == "{":
			brace_depth += 1
		elif ch == "}":
			brace_depth -= 1
		elif ch == "[":
			bracket_depth += 1
		elif ch == "]":
			bracket_depth -= 1
		i += 1
	return -1


# Locate the VALUE range for `key` in `block_range`. Returns
# (start, end) of the bytes spanning just the value text (not
# including the colon or any whitespace). For "key": 3.14, the
# range covers "3.14" only. For "key": "rms", it covers the
# QUOTED string "rms" (including quotes), so the patcher can
# overwrite both quotes and content together.
func _find_value_range_for_key(text: String, block_range: Vector2i,
		key: String) -> Vector2i:
	var key_pos: int = _find_key_in_block(text, block_range, key)
	if key_pos < 0:
		return Vector2i(-1, -1)
	# Move past the key, the closing quote, whitespace, the colon,
	# and any whitespace after the colon.
	var i: int = key_pos + key.length() + 2  # 2 = surrounding quotes
	while i < text.length() and text.substr(i, 1) in [" ", "\t", "\n", "\r"]:
		i += 1
	if i >= text.length() or text.substr(i, 1) != ":":
		return Vector2i(-1, -1)
	i += 1
	while i < text.length() and text.substr(i, 1) in [" ", "\t", "\n", "\r"]:
		i += 1
	# Now i is at the start of the value. Find its end.
	if i >= text.length():
		return Vector2i(-1, -1)
	var first_ch: String = text.substr(i, 1)
	if first_ch == '"':
		# Quoted string value. Walk to matching close quote.
		var j: int = i + 1
		while j < text.length():
			var ch2: String = text.substr(j, 1)
			if ch2 == "\\":
				j += 2
				continue
			if ch2 == '"':
				return Vector2i(i, j + 1)
			j += 1
		return Vector2i(-1, -1)
	# Numeric (or bool/null) value. Walk to next delimiter.
	var j2: int = i
	while j2 < text.length():
		var ch3: String = text.substr(j2, 1)
		if ch3 in [",", "}", "]", " ", "\t", "\n", "\r"]:
			break
		j2 += 1
	return Vector2i(i, j2)


# Insert "key": <value_text> into a JSON block. Inserted right
# before the block's closing "}", with a leading ", " separator
# if the block already has at least one key. Whitespace and
# indentation outside the block are preserved verbatim.
#
# Insertion is intentionally minimal — we do NOT try to match the
# existing block's indentation style. The result is well-formed
# JSON but may not be perfectly aesthetically formatted. For the
# v0.28.4 use case (filling in gain_db on a bus that didn't have
# one) this trade-off is fine; the value the user just dragged
# matters more than the layout, and the user is likely to format
# the file by hand or via a JSON tool anyway.
func _insert_pair_in_block(text: String, block_range: Vector2i,
		key: String, value_text: String, errors: Array) -> String:
	var slice: String = text.substr(
			block_range.x, block_range.y - block_range.x)
	# Walk from the END of the block backward to find the closing
	# "}". (block_range.y is exclusive past the closing }.)
	var close_at: int = block_range.y - 1  # position of the "}"
	# Is the block empty? Walk from the OPENING "{" forward, skipping
	# whitespace, and see if we immediately hit the closing "}".
	var open_at: int = block_range.x
	var j: int = open_at + 1
	while j < close_at and text.substr(j, 1) in [" ", "\t", "\n", "\r"]:
		j += 1
	var separator: String = ""
	if j < close_at:
		# Block isn't empty. Need a ", " before our new pair.
		separator = ", "
		# Find the last non-whitespace char before the close. If
		# it's a comma we don't need to add one; otherwise we do.
		var k: int = close_at - 1
		while k > open_at and text.substr(k, 1) in [" ", "\t", "\n", "\r"]:
			k -= 1
		if k > open_at and text.substr(k, 1) == ",":
			separator = " "  # trailing comma in source (unusual but legal in some dialects)
	var insertion: String = separator + '"' + key + '": ' + value_text + " "
	return text.substr(0, close_at) + insertion + text.substr(close_at)


# Find the position of the "{" that opens the smallest enclosing
# JSON object containing `pos`. Returns -1 if not found. Used to
# locate a bus block's opener given the position of its "name" key.
func _find_enclosing_brace_open(text: String, pos: int) -> int:
	var i: int = pos - 1
	var depth: int = 0
	# Scan backwards, but tolerate strings since they may contain
	# stray "{". The trick: scan backwards skipping strings in
	# reverse. Simpler approach — count braces forward from 0 and
	# remember the LAST top-level "{" before `pos` whose matching
	# "}" is AFTER `pos`.
	var open_stack: Array = []
	var in_string: bool = false
	var k: int = 0
	while k <= pos and k < text.length():
		var ch: String = text.substr(k, 1)
		if in_string:
			if ch == "\\":
				k += 2
				continue
			if ch == '"':
				in_string = false
			k += 1
			continue
		if ch == '"':
			in_string = true
		elif ch == "{":
			open_stack.append(k)
		elif ch == "}":
			if not open_stack.is_empty():
				open_stack.pop_back()
		k += 1
	if open_stack.is_empty():
		return -1
	return int(open_stack[-1])


# Given the position of an "{", find the position of its matching
# "}". Skips strings and nested braces. Returns -1 on unbalanced.
func _find_matching_brace_close(text: String, open_pos: int) -> int:
	var depth: int = 1
	var i: int = open_pos + 1
	var in_string: bool = false
	while i < text.length():
		var ch: String = text.substr(i, 1)
		if in_string:
			if ch == "\\":
				i += 2
				continue
			if ch == '"':
				in_string = false
			i += 1
			continue
		if ch == '"':
			in_string = true
		elif ch == "{":
			depth += 1
		elif ch == "}":
			depth -= 1
			if depth == 0:
				return i
		i += 1
	return -1


# Copy a file. Returns true on success. Used for the .bak path
# in _do_save. Uses FileAccess.READ/WRITE rather than DirAccess
# because godot's bundled DirAccess.copy didn't reliably work
# across all 4.x versions during gool development.
func _copy_file(src: String, dst: String) -> bool:
	var fin := FileAccess.open(src, FileAccess.READ)
	if fin == null:
		return false
	var bytes := fin.get_buffer(fin.get_length())
	fin.close()
	var fout := FileAccess.open(dst, FileAccess.WRITE)
	if fout == null:
		return false
	fout.store_buffer(bytes)
	fout.close()
	return true


# ===================================================================
# v0.28.8 Phase 3.3d: topology editing
# ===================================================================
#
# Adds/removes/reorders effects on a bus and adds/removes buses
# themselves. Topology changes can't be expressed as targeted byte
# patches (the structure itself changes), so they're written via
# re-serialization of the affected scope:
#
#   - Effect topology (one bus changed): re-serialize that bus's
#     {...} block. Outer file structure (other buses, _comment,
#     sample_rate, category_routing, etc.) bit-for-bit preserved.
#
#   - Bus topology (add/remove bus): re-serialize the whole `buses`
#     [...] array. Outer top-level structure preserved; comments
#     WITHIN the buses array are lost (acceptable: users typically
#     comment at the top level, not between bus elements).
#
# Save-time dispatch in _do_save picks the right path based on a
# per-bus dirty type ("value" → byte patcher, "topology" → bus-block
# re-serializer) plus a flag _buses_array_dirty that subsumes
# everything when set.
#
# Why JSON.stringify (not custom number formatting): the %g lesson
# from v0.28.4-v0.28.7 still applies. Godot's native serializer is
# the only thing guaranteed to round-trip GDScript dict-of-Variants
# back through GDScript JSON.parse_string. No printf format strings
# anywhere on this path.

# Engine-default param values for each effect kind. Mirror of the
# *Config struct field initializers in src/audio_engine/dsp/. When
# the user adds an effect via the dock, ALL params for that kind
# are written into config.json so the dock has values to display
# immediately (vs. relying on engine fallback defaults at load time).
const EFFECT_DEFAULTS_BY_KIND: Dictionary = {
	"gain": {
		"kind": "gain",
		"gain_db": 0.0,
	},
	"biquad": {
		"kind": "biquad",
		"biquad_type": "lowpass",
		"cutoff_hz": 1000.0,
		"q": 0.707,
		"biquad_gain_db": 0.0,
	},
	"compressor": {
		"kind": "compressor",
		"threshold_db": -20.0,
		"ratio": 4.0,
		"attack_ms": 10.0,
		"release_ms": 200.0,
		"makeup_db": 0.0,
		"knee_width_db": 0.0,
		"mix_ratio": 1.0,
		"max_reduction_db": 60.0,
		"sidechain_hpf_hz": 0.0,
		"hold_ms": 0.0,
		"detection_mode": "peak",
	},
	"reverb": {
		"kind": "reverb",
		# v0.29.0 Dattorro plate. Defaults match
		# include/audio_engine/bus.h::EffectConfig and the Dattorro
		# design doc (docs/audio_design/reverb_dattorro.md). The
		# wet_gain_db default of 0.0 dB plus the moderate predelay /
		# decay settings give a usable "small-to-medium room" send
		# without further tuning. dry_gain_db (v0.29.5) defaults to
		# 0 dB so a fresh reverb on an insert sounds like signal+tail.
		"predelay_ms": 30.0,
		"decay": 0.5,
		"lf_damping": 0.0,
		"hf_damping": 0.3,
		"diffusion": 0.625,
		"dry_gain_db": 0.0,
		"wet_gain_db": 0.0,
	},
	"saturation": {
		"kind": "saturation",
		"drive": 1.0,
		"mix": 0.0,
		"output_gain": 1.0,
		"bias": 0.0,
		# v0.69.0: surface the existing engine params via the dock UI.
		# Default mode "tanh" matches the engine's default; tone 0.0
		# = flat tilt (no shaping bias toward lows or highs).
		"mode": "tanh",
		"tone": 0.0,
	},
	# v0.64.1: master_control defaults block. Mirrors the Standard
	# FPS preset that fresh projects ship with on their Master bus
	# (see plugin.gd lines 117-137 of the default config template).
	# When the user picks "Master Control" from the Add Effect
	# dropdown, this dict gets appended to the bus's effects array
	# via add_effect, producing the same JSON shape that fresh
	# projects have built in by default.
	#
	# Preset rationale: -16 LUFS target + -1 dBTP ceiling is the
	# baseline streaming-loudness target for game-style mixes.
	# Conservative glue (2:1, soft knee) and rider (±6 dB envelope,
	# 3s time constant) so a user dropping this on an existing mix
	# doesn't get aggressive processing they have to immediately
	# tune down.
	#
	# Per-bus tuning happens through the effects-panel sliders
	# after the effect is added; this dict is just the starting
	# point. To opt out of any stage, the user can flip the
	# corresponding mc_*_enabled to false.
	#
	# Note: editor-time param introspection for master_control's
	# 17 parameters is still deferred (see NOTE under
	# KIND_INT_TO_ABBREV) — adding this effect from the dropdown
	# WILL write the correct JSON block and the engine will load
	# it correctly when the game runs, but the per-parameter
	# sliders in the dock's effects panel are blank at editor time
	# until KIND_INT_TO_JSON_KEYS and KIND_INT_TO_KEY_TO_PARAM_ID
	# get their kind=6 entries populated.
	"master_control": {
		"kind":                       "master_control",
		"mc_glue_enabled":            true,
		"mc_rider_enabled":           true,
		"mc_limiter_enabled":         true,
		"mc_glue_threshold_db":      -12.0,
		"mc_glue_ratio":               2.0,
		"mc_glue_attack_ms":          10.0,
		"mc_glue_release_ms":        250.0,
		"mc_glue_knee_db":             6.0,
		"mc_glue_makeup_db":           0.0,
		"mc_rider_target_lufs":      -16.0,
		"mc_rider_time_constant_ms": 3000.0,
		"mc_rider_max_gain_db":        6.0,
		"mc_rider_min_gain_db":       -6.0,
		"mc_rider_freeze_below_lufs": -6.0,
		"mc_limiter_ceiling_dbtp":    -1.0,
		"mc_limiter_release_ms":      50.0,
		"mc_limiter_lookahead_ms":     5.0,
	},
}

# v0.64.2: master_control preset values, mirroring the five .tres
# files in godot/addons/gool/master_fx_presets/. Used by the Add
# Effect dropdown's Master Control submenu — the user picks a
# preset name, add_effect() looks up the matching entry here, and
# the resulting effect block lands in config.json with that
# preset's calibrated parameter values.
#
# Why mirrored here instead of loaded from the .tres files: the
# Add Effect picker runs in the editor-time path before any
# runtime/engine is involved. The .tres files are designed for
# the runtime path (GoolMasterFxProfile node loads them at
# game-start to push params to the live engine), so they're
# behind ResourceLoader which only works once the project has
# loaded. Editor-time tooling that needs the same values has to
# carry them inline.
#
# If a preset's calibration changes, BOTH places need updating:
# the .tres file (runtime) and this dict (editor-time). The
# preset names and IDs here must match what _MASTER_CONTROL_PRESETS
# in mixer_dock.gd uses to build the submenu — that table is the
# UX-facing source for label text, this table is the params-facing
# source for what gets written to config.json.
const MASTER_CONTROL_PRESETS: Dictionary = {
	"standard_fps": {
		"kind":                       "master_control",
		"mc_glue_enabled":            true,
		"mc_rider_enabled":           true,
		"mc_limiter_enabled":         true,
		"mc_glue_threshold_db":      -12.0,
		"mc_glue_ratio":               2.0,
		"mc_glue_attack_ms":          10.0,
		"mc_glue_release_ms":        250.0,
		"mc_glue_knee_db":             6.0,
		"mc_glue_makeup_db":           0.0,
		"mc_rider_target_lufs":      -16.0,
		"mc_rider_time_constant_ms": 3000.0,
		"mc_rider_max_gain_db":        6.0,
		"mc_rider_min_gain_db":       -6.0,
		"mc_rider_freeze_below_lufs": -6.0,
		"mc_limiter_ceiling_dbtp":    -1.0,
		"mc_limiter_release_ms":      50.0,
		"mc_limiter_lookahead_ms":     5.0,
	},
	"cinema_quiet": {
		"kind":                       "master_control",
		"mc_glue_enabled":            true,
		"mc_rider_enabled":           true,
		"mc_limiter_enabled":         true,
		"mc_glue_threshold_db":      -16.0,
		"mc_glue_ratio":               1.3,
		"mc_glue_attack_ms":          25.0,
		"mc_glue_release_ms":        600.0,
		"mc_glue_knee_db":            10.0,
		"mc_glue_makeup_db":           0.0,
		"mc_rider_target_lufs":      -22.0,
		"mc_rider_time_constant_ms": 5000.0,
		"mc_rider_max_gain_db":        3.0,
		"mc_rider_min_gain_db":       -3.0,
		"mc_rider_freeze_below_lufs": -8.0,
		"mc_limiter_ceiling_dbtp":    -1.5,
		"mc_limiter_release_ms":     120.0,
		"mc_limiter_lookahead_ms":     5.0,
	},
	"loud_and_aggressive": {
		"kind":                       "master_control",
		"mc_glue_enabled":            true,
		"mc_rider_enabled":           true,
		"mc_limiter_enabled":         true,
		"mc_glue_threshold_db":      -10.0,
		"mc_glue_ratio":               3.5,
		"mc_glue_attack_ms":           5.0,
		"mc_glue_release_ms":        150.0,
		"mc_glue_knee_db":             4.0,
		"mc_glue_makeup_db":           1.5,
		"mc_rider_target_lufs":      -12.0,
		"mc_rider_time_constant_ms": 2000.0,
		"mc_rider_max_gain_db":        8.0,
		"mc_rider_min_gain_db":       -8.0,
		"mc_rider_freeze_below_lufs": -6.0,
		"mc_limiter_ceiling_dbtp":    -1.0,
		"mc_limiter_release_ms":      30.0,
		"mc_limiter_lookahead_ms":     5.0,
	},
	"subtle_glue": {
		"kind":                       "master_control",
		"mc_glue_enabled":            true,
		"mc_rider_enabled":           true,
		"mc_limiter_enabled":         true,
		"mc_glue_threshold_db":      -14.0,
		"mc_glue_ratio":               1.5,
		"mc_glue_attack_ms":          15.0,
		"mc_glue_release_ms":        400.0,
		"mc_glue_knee_db":             8.0,
		"mc_glue_makeup_db":           0.0,
		"mc_rider_target_lufs":      -18.0,
		"mc_rider_time_constant_ms": 4000.0,
		"mc_rider_max_gain_db":        4.0,
		"mc_rider_min_gain_db":       -4.0,
		"mc_rider_freeze_below_lufs": -6.0,
		"mc_limiter_ceiling_dbtp":    -1.0,
		"mc_limiter_release_ms":      80.0,
		"mc_limiter_lookahead_ms":     5.0,
	},
	"none_bypass": {
		"kind":                       "master_control",
		"mc_glue_enabled":            false,
		"mc_rider_enabled":           false,
		"mc_limiter_enabled":         false,
		"mc_glue_threshold_db":      -12.0,
		"mc_glue_ratio":               2.0,
		"mc_glue_attack_ms":          10.0,
		"mc_glue_release_ms":        250.0,
		"mc_glue_knee_db":             6.0,
		"mc_glue_makeup_db":           0.0,
		"mc_rider_target_lufs":      -16.0,
		"mc_rider_time_constant_ms": 3000.0,
		"mc_rider_max_gain_db":        6.0,
		"mc_rider_min_gain_db":       -6.0,
		"mc_rider_freeze_below_lufs": -6.0,
		"mc_limiter_ceiling_dbtp":    -1.0,
		"mc_limiter_release_ms":      50.0,
		"mc_limiter_lookahead_ms":     5.0,
	},
}

# Display order for the effect kind picker. Five kinds, signal-flow
# logical order: source-shaping first, then dynamics, then space.
const EFFECT_KIND_ORDER: Array = ["gain", "biquad", "compressor", "saturation", "reverb"]

# Per-bus dirty-type sentinels. _dirty_buses[bus_name] holds one of
# these strings when the bus has unsaved edits.
#
# Topology supersedes value: once a bus has been marked topology-dirty,
# subsequent value edits don't downgrade it (the re-serializer already
# rewrites the whole block, picking up every in-memory change).
const _DIRTY_VALUE: String = "value"
const _DIRTY_TOPOLOGY: String = "topology"

# When true, the whole `buses` array gets re-serialized at save time.
# Set by add_bus / remove_bus. Subsumes per-bus dirty flags.
var _buses_array_dirty: bool = false

signal topology_changed(bus_name: String)
signal bus_added(bus_name: String)
signal bus_removed(bus_name: String)
# v0.80.17: bus rename signal. Fires AFTER rename_bus has propagated
# the rename through every in-config reference (the bus's own name,
# child buses' "parent" fields, compressor sidechain_bus fields, and
# category_routing entries). Listeners that hold bus-name references
# OUTSIDE config.json — currently the ProjectSettings entries
# `gool/material_eq/impact_bus` and `gool/material_eq/listener_bus`
# used by runtime_singleton for material-EQ routing — should
# propagate the rename to their own state in this handler.
signal bus_renamed(old_name: String, new_name: String)


# ===================================================================
# Public topology API: effects (in-bus)
# ===================================================================

# Append a new effect of the given kind to bus_name's effect chain.
# Effect is populated with EFFECT_DEFAULTS_BY_KIND[kind_string] —
# the user can adjust params via the existing sliders afterward.
# Returns true on success.
#
# v0.64.2: preset_id parameter. For master_control kind only: when
# preset_id is non-empty, looks up the matching entry in
# MASTER_CONTROL_PRESETS and uses ITS params (not the
# EFFECT_DEFAULTS_BY_KIND fallback). Other kinds ignore preset_id.
# Unknown preset_id for master_control is an error — silent
# fallback would mask a UI bug, so we return false and push_error.
func add_effect(bus_name: String, kind_string: String, preset_id: String = "") -> bool:
	if not EFFECT_DEFAULTS_BY_KIND.has(kind_string):
		push_error("[gool config] add_effect: unknown kind '%s'" % kind_string)
		return false
	var b := get_bus(bus_name)
	if b.is_empty():
		push_error("[gool config] add_effect: bus not found: %s" % bus_name)
		return false
	var effects_v: Variant = b.get("effects", null)
	if effects_v == null:
		b["effects"] = []
		effects_v = b["effects"]
	elif not (effects_v is Array):
		push_error("[gool config] add_effect: bus '%s' has non-array effects" % bus_name)
		return false
	var effects: Array = effects_v
	# v0.64.2: pick the params dict. Master Control with a preset
	# uses MASTER_CONTROL_PRESETS; everything else uses
	# EFFECT_DEFAULTS_BY_KIND.
	var params_source: Dictionary
	if kind_string == "master_control" and preset_id != "":
		if not MASTER_CONTROL_PRESETS.has(preset_id):
			push_error("[gool config] add_effect: unknown master_control preset '%s'" % preset_id)
			return false
		params_source = MASTER_CONTROL_PRESETS[preset_id] as Dictionary
	else:
		params_source = EFFECT_DEFAULTS_BY_KIND[kind_string] as Dictionary
	var new_effect: Dictionary = params_source.duplicate(true)
	effects.append(new_effect)
	_mark_topology_dirty(bus_name)
	topology_changed.emit(bus_name)
	return true


# Remove the effect at effect_index from bus_name's chain.
# Returns true on success.
func remove_effect(bus_name: String, effect_index: int) -> bool:
	var b := get_bus(bus_name)
	if b.is_empty():
		return false
	var effects_v: Variant = b.get("effects", null)
	if not (effects_v is Array):
		return false
	var effects: Array = effects_v
	if effect_index < 0 or effect_index >= effects.size():
		return false
	effects.remove_at(effect_index)
	_mark_topology_dirty(bus_name)
	topology_changed.emit(bus_name)
	return true


# Move the effect at from_index to to_index in bus_name's chain.
# from_index/to_index are both 0-based positions in the chain after
# any prior removes. Returns true on success.
func reorder_effect(bus_name: String, from_index: int, to_index: int) -> bool:
	var b := get_bus(bus_name)
	if b.is_empty():
		return false
	var effects_v: Variant = b.get("effects", null)
	if not (effects_v is Array):
		return false
	var effects: Array = effects_v
	var n: int = effects.size()
	if from_index < 0 or from_index >= n:
		return false
	if to_index < 0 or to_index >= n:
		return false
	if from_index == to_index:
		return true
	var moved: Variant = effects[from_index]
	effects.remove_at(from_index)
	effects.insert(to_index, moved)
	_mark_topology_dirty(bus_name)
	topology_changed.emit(bus_name)
	return true


# ===================================================================
# Public topology API: buses
# ===================================================================

# Append a new bus to the buses array.
#
# Defaults: gain_db = 0.0, parent = "Master" (if Master exists and
# this isn't Master itself), no effects array.
#
# Returns OK on success, ERR_INVALID_PARAMETER for empty name,
# ERR_ALREADY_EXISTS for name conflict, ERR_INVALID_DATA if the
# config doesn't have a valid buses array.
func add_bus(bus_name: String) -> int:
	if bus_name.is_empty():
		push_error("[gool config] add_bus: empty name rejected")
		return ERR_INVALID_PARAMETER
	if not get_bus(bus_name).is_empty():
		return ERR_ALREADY_EXISTS
	var arr_v: Variant = _parsed.get("buses", null)
	if not (arr_v is Array):
		push_error("[gool config] add_bus: buses array missing/invalid")
		return ERR_INVALID_DATA
	var arr: Array = arr_v
	var new_bus: Dictionary = { "name": bus_name }
	# Default parent to Master if it exists; skip for Master itself.
	if bus_name != "Master" and not get_bus("Master").is_empty():
		new_bus["parent"] = "Master"
	new_bus["gain_db"] = 0.0
	arr.append(new_bus)
	_buses_array_dirty = true
	_has_pending_save = true
	_schedule_save()
	bus_added.emit(bus_name)
	return OK


# Remove bus_name from the buses array.
#
# Refuses if any other bus or category_routing entry references this
# bus (parent, sidechain_bus, routing target). Caller should pre-check
# via collect_bus_references and surface the list to the user; this
# function returns ERR_INVALID_PARAMETER if any refs exist.
#
# Returns OK on success, ERR_INVALID_PARAMETER on dangling refs or
# empty name, ERR_DOES_NOT_EXIST if no such bus.
func remove_bus(bus_name: String) -> int:
	if bus_name.is_empty():
		return ERR_INVALID_PARAMETER
	var refs := collect_bus_references(bus_name)
	if not refs.is_empty():
		push_error("[gool config] remove_bus: '%s' has %d dangling reference(s)"
				% [bus_name, refs.size()])
		return ERR_INVALID_PARAMETER
	var arr_v: Variant = _parsed.get("buses", null)
	if not (arr_v is Array):
		return ERR_INVALID_DATA
	var arr: Array = arr_v
	var idx: int = -1
	for i in range(arr.size()):
		var b_v: Variant = arr[i]
		if b_v is Dictionary and (b_v as Dictionary).get("name") == bus_name:
			idx = i
			break
	if idx < 0:
		return ERR_DOES_NOT_EXIST
	arr.remove_at(idx)
	# A removed bus can't have pending per-bus edits to save anymore.
	if _dirty_buses.has(bus_name):
		_dirty_buses.erase(bus_name)
	_buses_array_dirty = true
	_has_pending_save = true
	_schedule_save()
	bus_removed.emit(bus_name)
	return OK


# v0.80.17: rename a bus and propagate the rename through every
# in-config reference atomically. Closes triage findings #12 (no
# dirty-tracking for renames because there was no proper mutator),
# #23 (renaming a bus left dangling effect/parent/category_routing
# references), and #27 (ProjectSettings bus-name strings — handled
# via the bus_renamed signal listener in mixer_dock.gd).
#
# Propagation in _parsed (handled here):
#   1. The bus's own "name"
#   2. Other buses' "parent" field
#   3. Compressor effects' "sidechain_bus" field on every bus
#   4. category_routing values pointing at old_name
#
# Propagation outside _parsed (handled by mixer_dock via signal):
#   5. ProjectSettings gool/material_eq/impact_bus (if matches)
#   6. ProjectSettings gool/material_eq/listener_bus (if matches)
#
# Reserved-name policy: refuses to rename "Master". The C++ bus
# parser at bus_config_loader.cpp:818 hardcodes "Master" (and
# lowercase "master") as the root sentinel — renaming it would
# break parent resolution for every other bus. To rename what's
# conceptually the master bus, you'd have to either accept that
# constraint or do an engine-side change to look up the root by ID
# instead of name. Not in scope here.
#
# Returns OK on success, or:
#   ERR_INVALID_PARAMETER if new_name is empty/whitespace
#   ERR_INVALID_DATA     if old_name doesn't exist, or old_name is
#                        "Master" (reserved as engine root)
#   ERR_ALREADY_EXISTS   if new_name collides with another bus
#
# Idempotent no-op (returns OK) if new_name trims to old_name.
func rename_bus(old_name: String, new_name: String) -> int:
	var trimmed_new := new_name.strip_edges()
	if trimmed_new.is_empty():
		push_error("[gool config] rename_bus: empty new name rejected")
		return ERR_INVALID_PARAMETER
	if trimmed_new == old_name:
		return OK  # idempotent no-op
	if old_name == "Master":
		push_error("[gool config] rename_bus: 'Master' is reserved as "
				+ "the engine root sentinel and cannot be renamed")
		return ERR_INVALID_DATA

	var arr_v: Variant = _parsed.get("buses", null)
	if not (arr_v is Array):
		push_error("[gool config] rename_bus: buses array missing/invalid")
		return ERR_INVALID_DATA
	var buses: Array = arr_v

	# Single pass to find the source bus and check for collision.
	var src_idx: int = -1
	for i in range(buses.size()):
		var b_v: Variant = buses[i]
		if not (b_v is Dictionary):
			continue
		var n: String = String((b_v as Dictionary).get("name", ""))
		if n == old_name:
			src_idx = i
		elif n == trimmed_new:
			push_error("[gool config] rename_bus: '%s' already exists"
					% trimmed_new)
			return ERR_ALREADY_EXISTS
	if src_idx == -1:
		push_error("[gool config] rename_bus: '%s' not found" % old_name)
		return ERR_INVALID_DATA

	# Propagate through _parsed:

	# 1. The bus's own name
	(buses[src_idx] as Dictionary)["name"] = trimmed_new

	# 2 + 3. Other buses' parent + sidechain_bus references. Single
	# pass since we're iterating buses for both. Matches the field
	# set in collect_bus_references — keeping the two functions
	# symmetric on what counts as "a reference to this bus".
	for b_v in buses:
		if not (b_v is Dictionary):
			continue
		var b: Dictionary = b_v
		if String(b.get("parent", "")) == old_name:
			b["parent"] = trimmed_new
		var effects_v: Variant = b.get("effects", [])
		if effects_v is Array:
			for e_v in (effects_v as Array):
				if not (e_v is Dictionary):
					continue
				var e: Dictionary = e_v
				if e.get("kind") == "compressor" \
						and String(e.get("sidechain_bus", "")) == old_name:
					e["sidechain_bus"] = trimmed_new

	# 4. category_routing entries pointing at old_name
	var routing_v: Variant = _parsed.get("category_routing", null)
	if routing_v is Dictionary:
		var routing: Dictionary = routing_v
		for cat in routing.keys():
			if String(routing[cat]) == old_name:
				routing[cat] = trimmed_new

	# Move dirty-edit tracking off the old name. The _buses_array_dirty
	# flag below means per-bus tracking is moot for the save itself
	# (full re-serialization rewrites everything from _parsed), but
	# keeping _dirty_buses free of stale names maintains the invariant
	# that its keys are valid bus names — important for any code that
	# reads _dirty_buses.keys() (e.g. external_change_detected's
	# emit signature).
	if _dirty_buses.has(old_name):
		_dirty_buses.erase(old_name)

	# Full re-serialization required. The surgical patcher operates
	# per-bus; cross-bus reference updates (parent, sidechain_bus,
	# category_routing) aren't representable in its per-bus model.
	_buses_array_dirty = true
	_has_pending_save = true
	_schedule_save()
	bus_renamed.emit(old_name, trimmed_new)
	return OK


# Returns human-readable descriptions of references to bus_name that
# would dangle if bus_name were removed. Empty array means safe to
# remove. Used by the dock to surface refs to the user before commit.
#
# Reference sources:
#   - Other buses with parent = bus_name
#   - category_routing values pointing at bus_name
#   - compressor effects with sidechain_bus = bus_name
func collect_bus_references(bus_name: String) -> Array:
	var refs: Array = []
	var arr_v: Variant = _parsed.get("buses", [])
	if arr_v is Array:
		for b_v in (arr_v as Array):
			if not (b_v is Dictionary):
				continue
			var b: Dictionary = b_v
			if b.get("name") == bus_name:
				continue
			if b.get("parent") == bus_name:
				refs.append("bus '%s' has parent='%s'"
						% [b.get("name", "?"), bus_name])
			var effects_v: Variant = b.get("effects", [])
			if effects_v is Array:
				var ei: int = 0
				for e_v in (effects_v as Array):
					if e_v is Dictionary \
							and (e_v as Dictionary).get("kind") == "compressor" \
							and (e_v as Dictionary).get("sidechain_bus") == bus_name:
						refs.append("bus '%s' effect #%d sidechain_bus='%s'"
								% [b.get("name", "?"), ei, bus_name])
					ei += 1
	var routing_v: Variant = _parsed.get("category_routing", {})
	if routing_v is Dictionary:
		for cat in (routing_v as Dictionary).keys():
			if (routing_v as Dictionary)[cat] == bus_name:
				refs.append("category_routing.%s → '%s'" % [cat, bus_name])
	return refs


# ===================================================================
# Topology dirty-marking
# ===================================================================

# Topology supersedes value. Once marked topology, subsequent value
# edits don't downgrade it — the re-serializer rewrites the whole
# bus block from current model state.
func _mark_topology_dirty(bus_name: String) -> void:
	_dirty_buses[bus_name] = _DIRTY_TOPOLOGY
	_has_pending_save = true
	_schedule_save()


# ===================================================================
# Serializer
# ===================================================================

# Replace a single bus's {...} block in text with canonical JSON from
# the in-memory model. Outer file structure (other buses, comments,
# top-level keys) bit-for-bit preserved.
#
# On failure (bus not in source, bus not in model) appends to errors
# and returns text unchanged.
func _re_serialize_bus_block(text: String, bus_name: String,
		errors: Array) -> String:
	var rng := _find_bus_block_range(text, bus_name)
	if rng.x < 0:
		errors.append("bus '%s': block not found in source" % bus_name)
		return text
	var b := get_bus(bus_name)
	if b.is_empty():
		errors.append("bus '%s': not in model" % bus_name)
		return text
	var canonical: String = JSON.stringify(b, "\t")
	var indent_prefix := _infer_indent_prefix(text, rng.x)
	var reindented := canonical.replace("\n", "\n" + indent_prefix)
	return text.substr(0, rng.x) + reindented + text.substr(rng.y)


# Replace the entire `buses` array in text with canonical JSON from
# the in-memory model. Used for bus add/remove.
#
# Comments WITHIN the buses array are lost. Comments at the top
# level (the `_comment` key, sample_rate notes, category_routing
# block) survive.
func _re_serialize_buses_array(text: String, errors: Array) -> String:
	var rng := _find_buses_array_range(text)
	if rng.x < 0:
		errors.append("buses array: not found in source")
		return text
	var arr_v: Variant = _parsed.get("buses", null)
	if not (arr_v is Array):
		errors.append("buses array: not an array in model")
		return text
	var canonical: String = JSON.stringify(arr_v, "\t")
	var indent_prefix := _infer_indent_prefix(text, rng.x)
	var reindented := canonical.replace("\n", "\n" + indent_prefix)
	return text.substr(0, rng.x) + reindented + text.substr(rng.y)


# Find the byte range of the `buses` array's [...] in text. Returns
# Vector2i(open_pos, close_pos+1) on success, Vector2i(-1, -1) on miss.
# Mirror of _find_bus_block_range's structure but for the array literal.
func _find_buses_array_range(text: String) -> Vector2i:
	var i: int = 0
	var in_string: bool = false
	while i < text.length():
		var ch: String = text.substr(i, 1)
		if in_string:
			if ch == "\\":
				i += 2
				continue
			if ch == '"':
				in_string = false
			i += 1
			continue
		if ch == '"':
			if text.substr(i, 7) == '"buses"':
				var j: int = i + 7
				while j < text.length() and text.substr(j, 1) in [" ", "\t", "\n", "\r"]:
					j += 1
				if j < text.length() and text.substr(j, 1) == ":":
					j += 1
					while j < text.length() and text.substr(j, 1) in [" ", "\t", "\n", "\r"]:
						j += 1
					if j < text.length() and text.substr(j, 1) == "[":
						var bracket_open: int = j
						var bracket_close: int = _find_matching_bracket_close(
								text, bracket_open)
						if bracket_close < 0:
							return Vector2i(-1, -1)
						return Vector2i(bracket_open, bracket_close + 1)
			in_string = true
		i += 1
	return Vector2i(-1, -1)


# From the '[' at open_pos walk forward and return offset of the
# matching ']'. Tracks bracket depth, brace depth, and string state
# so the close fires only when nesting is fully balanced.
func _find_matching_bracket_close(text: String, open_pos: int) -> int:
	var i: int = open_pos + 1
	var bracket_depth: int = 1
	var brace_depth: int = 0
	var in_string: bool = false
	while i < text.length():
		var ch: String = text.substr(i, 1)
		if in_string:
			if ch == "\\":
				i += 2
				continue
			if ch == '"':
				in_string = false
			i += 1
			continue
		if ch == '"':
			in_string = true
		elif ch == "{":
			brace_depth += 1
		elif ch == "}":
			brace_depth -= 1
		elif ch == "[":
			bracket_depth += 1
		elif ch == "]":
			bracket_depth -= 1
			if bracket_depth == 0 and brace_depth == 0:
				return i
		i += 1
	return -1


# Returns the whitespace prefix on the line containing pos, up to pos.
# Used to align canonical JSON's subsequent lines with the bus block /
# buses array's surrounding indent. Falls back to "" if the prefix
# isn't pure whitespace (weird configs; better to produce ugly-but-
# parseable output than to refuse).
func _infer_indent_prefix(text: String, pos: int) -> String:
	var line_start: int = pos
	while line_start > 0 and text.substr(line_start - 1, 1) != "\n":
		line_start -= 1
	var raw_indent: String = text.substr(line_start, pos - line_start)
	for c in raw_indent:
		if c != " " and c != "\t":
			return ""
	return raw_indent
