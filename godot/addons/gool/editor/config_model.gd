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
	9:  "room_size",          # Reverb_RoomSize
	10: "damping",            # Reverb_Damping
	11: "wet_gain_db",        # Reverb_WetGainDb
	19: "drive",              # Saturation_Drive
	20: "mix",                # Saturation_Mix
	21: "output_gain",        # Saturation_OutputGain
	22: "bias",               # Saturation_Bias
}

# detection_mode is the one non-numeric param. Engine protocol
# uses 0.0=peak, 1.0=rms; config JSON uses "peak"/"rms" strings.
const _DETECTION_MODE_PARAM_ID: int = 18

# paramId → engine EffectKind. Used to attach kind/kind_name to
# effects returned by get_effects() when serving rest-time queries.
# Matches the EffectKind enum values (None=0, Gain=1, ...).
const PARAM_ID_TO_KIND: Dictionary = {
	1: 1,                                          # Gain
	2: 2,  3: 2,  12: 2,                           # BiquadFilter
	4: 3,  5: 3,  6: 3,  7: 3,  8: 3,
	13: 3, 14: 3, 15: 3, 16: 3, 17: 3, 18: 3,      # Compressor
	9: 4,  10: 4, 11: 4,                           # Reverb
	19: 5, 20: 5, 21: 5, 22: 5,                    # Saturation
}

# Reverse: config "kind" string → EffectKind int. Used when
# loading effects from config.json.
const KIND_STRING_TO_INT: Dictionary = {
	"gain":        1,
	"biquad":      2,
	"compressor":  3,
	"reverb":      4,
	"saturation":  5,
}
const KIND_INT_TO_NAME: Dictionary = {
	1: "Gain", 2: "BiquadFilter", 3: "Compressor",
	4: "Reverb", 5: "Saturation",
}
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
	4: ["room_size", "damping", "wet_gain_db"],
	5: ["drive", "mix", "output_gain", "bias"],
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
	4: { "room_size": 9, "damping": 10, "wet_gain_db": 11 },
	5: { "drive": 19, "mix": 20, "output_gain": 21, "bias": 22 },
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
	# Reverb
	9: 0.5, 10: 0.5, 11: -12.0,
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
				else:
					params[pid] = float(e[k])
			else:
				params[pid] = float(PARAM_ID_TO_ENGINE_DEFAULT.get(pid, 0.0))
		out.append({
			"kind":      kind_i,
			"kind_name": KIND_INT_TO_NAME[kind_i],
			"params":    params,
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
	_dirty_buses[bus_name] = true
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

# Returns OK on success, or an Error code on failure. Emits
# model_saved or save_failed accordingly. Also emits
# external_change_detected if disk contents differ from what we
# last saw.
func _do_save() -> int:
	# v0.28.6: external-change detection switched from mtime-based
	# to CONTENT-based. The mtime approach (v0.28.4) was producing
	# a false positive on every save: Godot's filesystem watcher /
	# resource cache touches the file's mtime AFTER our write,
	# bumping it past our cached _last_seen_mtime, so the next
	# save saw it as "modified externally" and prompted. Comparing
	# actual byte contents is robust against any mtime weirdness:
	# if the disk content matches _raw_text, no external edit has
	# happened regardless of what mtime says.
	if FileAccess.file_exists(CONFIG_PATH):
		var f_check := FileAccess.open(CONFIG_PATH, FileAccess.READ)
		if f_check != null:
			var disk_text: String = f_check.get_as_text()
			f_check.close()
			if disk_text != _raw_text and not _raw_text.is_empty():
				var dirty_list: Array = _dirty_buses.keys()
				external_change_detected.emit(dirty_list)
				return ERR_FILE_UNRECOGNIZED  # generic "needs user input"

	# Backup current on-disk contents. If the patcher produces
	# something the engine can't parse, this is what we restore.
	if FileAccess.file_exists(CONFIG_PATH):
		if not _copy_file(CONFIG_PATH, BACKUP_PATH):
			var msg := "could not create %s backup" % BACKUP_PATH
			save_failed.emit(msg)
			return ERR_CANT_CREATE

	# Apply every dirty bus's pending edits to the raw text. The
	# walk uses the current _parsed (which already reflects all
	# the user's edits) as the source of values; the raw text is
	# what we patch.
	var new_text: String = _raw_text
	var write_errors: Array = []
	for bus_name in _dirty_buses.keys():
		new_text = _patch_bus_in_text(new_text, String(bus_name), write_errors)
	if not write_errors.is_empty():
		# At least one bus failed to patch (key not found and
		# insertion also failed somehow). Don't write a partial
		# result. Surface the error and abort.
		save_failed.emit("patch errors: " + ", ".join(write_errors))
		return ERR_INVALID_DATA

	# Write the patched text.
	var f := FileAccess.open(CONFIG_PATH, FileAccess.WRITE)
	if f == null:
		save_failed.emit("could not open %s for write" % CONFIG_PATH)
		return ERR_CANT_OPEN
	f.store_string(new_text)
	f.close()

	# Re-parse to verify the patcher produced valid JSON. If parse
	# fails, restore from backup — leaves the user's previous good
	# config intact, surfaces the bug for us to fix.
	#
	# v0.28.6: on parse failure, ALSO dump the failed text to
	# `gool/config.json.failed` so we have ground truth on what the
	# patcher produced. Without this, all we get is "JSON invalid"
	# with no way to see what was actually wrong. The file is left
	# on disk until the next successful save (which doesn't touch
	# it) or a manual delete — read once, fix the bug, then delete.
	var verify_v: Variant = JSON.parse_string(new_text)
	if verify_v == null or not (verify_v is Dictionary):
		var failed_path: String = "res://gool/config.json.failed"
		var f_dump := FileAccess.open(failed_path, FileAccess.WRITE)
		if f_dump != null:
			f_dump.store_string(new_text)
			f_dump.close()
		_copy_file(BACKUP_PATH, CONFIG_PATH)
		save_failed.emit(
				"post-write JSON invalid; restored from .bak. "
				+ "Failing text dumped to %s for diagnosis." % failed_path)
		return ERR_INVALID_DATA

	# All good. Commit the new state.
	_raw_text = new_text
	_last_seen_mtime = FileAccess.get_modified_time(CONFIG_PATH)
	var saved_buses: Array = _dirty_buses.keys()
	_dirty_buses.clear()
	_has_pending_save = false
	model_saved.emit(saved_buses)
	return OK


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
	if value == floor(value) and absf(value) < 1.0e15:
		return "%0.1f" % value
	return String.num(value)


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
