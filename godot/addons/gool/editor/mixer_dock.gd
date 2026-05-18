@tool
class_name GoolMixerDock
extends Control

# v0.26.0 — Phase 3.3b-1 mixer dock.
#
# Static, config-driven bus strip layout that's visible AND
# interactive at editor time (no F5 required). Reads bus topology
# from `res://gool/config.json` so the dock is populated as soon
# as the project loads. Live peak meters during F5 playback;
# interactive faders that send `gool:set_bus_gain` commands to
# the running game via the EngineDebugger bridge from v0.25.x.
#
# Visual reference: Godot's built-in Audio bus mixer panel.
# Bus name → meter + fader + dB scale → numeric dB readout.
#
# What's in this release (3.3b-1):
#   ✅ Static strips visible at editor time from config.json
#   ✅ Live peak meters during F5 (via debugger plugin)
#   ✅ Interactive gain fader per bus, drag = live update
#   ✅ dB scale markers (+6 / 0 / -6 / -24 / -72)
#   ✅ Persistent dB readout per strip
#
# What's NOT in this release (queued for 3.3b-2):
#   ⏳ Solo / Mute / Bypass buttons (need new engine APIs)
#   ⏳ Save fader changes to config.json (that's 3.3d)
#   ⏳ Auto-sync from running game's current gain on F5 start

# ---- Constants -------------------------------------------------------

# Polling cadence for peak meter updates. 30 Hz matches the
# game-side emit rate from runtime_singleton.gd.
const POLL_HZ: float = 30.0

# Visual smoothing — exponential decay applied to the linear peak
# value displayed on the bar. 0.85 per poll is "Pro Tools snappy".
const PEAK_DECAY: float = 0.85

# Strip dimensions.
const STRIP_WIDTH: float = 96.0
const STRIP_HEIGHT: float = 362.0  # v0.28.3: +22 for FX_BAND below readout
const STRIP_GAP: float = 4.0

# v0.28.3: extra vertical room reserved when an effects panel is
# open. Sized for a typical 2–3 effect bus (e.g. Compressor + Reverb
# at ~280px combined including the panel header and stylebox
# margins). Compressor alone fits comfortably; longer chains scroll
# inside the existing ScrollContainer.
const _PANEL_MIN_EXTRA: float = 280.0

# Fader range (matches Godot's built-in audio bus mixer convention).
const FADER_MAX_DB: float = 6.0
const FADER_MIN_DB: float = -72.0

# dB values shown as scale markers next to the fader. The mapping
# from dB → vertical pixel uses non-uniform spacing so the 0 dB
# region gets more resolution than the deep cut region (matching
# the visual feel of pro DAW faders).
const SCALE_MARKS_DB: Array = [6.0, 0.0, -6.0, -24.0, -72.0]

# Color thresholds (dBFS) — Pro Tools / Logic convention.
const DB_GREEN_MAX: float = -12.0
const DB_YELLOW_MAX: float = -6.0

# Palette.
const COLOR_GREEN := Color(0.36, 0.83, 0.46)
const COLOR_YELLOW := Color(0.95, 0.82, 0.32)
const COLOR_RED := Color(0.93, 0.40, 0.32)
const COLOR_BG := Color(0.10, 0.10, 0.12)
const COLOR_BG_OUTLINE := Color(0.20, 0.20, 0.22)
const COLOR_TEXT := Color(0.85, 0.88, 0.92)
const COLOR_TEXT_DIM := Color(0.55, 0.58, 0.62)
const COLOR_TICK := Color(0.45, 0.45, 0.48)
const COLOR_FADER_TRACK := Color(0.20, 0.22, 0.26)
const COLOR_FADER_HANDLE := Color(0.65, 0.72, 0.85)
const COLOR_FADER_HANDLE_ACTIVE := Color(0.78, 0.88, 0.95)

# Meter display range — minimum dBFS visible.
const DB_METER_FLOOR: float = -60.0
const DB_METER_CEIL: float = 6.0

# Path to the project's gool config.json. Editor reads this at
# project-load time to build the static strip layout.
const CONFIG_PATH: String = "res://gool/config.json"


# ---- State -----------------------------------------------------------

var _strip_container: HBoxContainer = null
var _empty_label: Label = null
var _strips: Array = []  # of _BusStrip
# v0.28.3: each strip lives inside a VBoxContainer column so the
# effects panel can stack below the strip without disturbing
# siblings. _columns[i] is the column holding _strips[i]; the
# two arrays stay index-synchronized through every rebuild and
# free cycle. Freeing the column frees the strip recursively.
var _columns: Array = []  # of VBoxContainer, parallel to _strips
# Name of the bus whose Fx panel is currently expanded, or "" if
# none. One panel at a time keeps the dock from turning into a
# wall of scrollbars and forces the user to commit attention to
# the effect they're editing.
var _expanded_bus: String = ""
# Cache of the latest get_latest_bus_stats() payload so the
# Fx-toggle handler can look up effects without re-polling.
# Updated on every successful _poll; never null but may be empty
# (game not running). Index-aligned with _strips at the moment of
# the most recent poll — though _strips may rebuild between polls,
# so always look up by bus_name not by index.
var _last_stats: Array = []
var _last_bus_names: PackedStringArray = PackedStringArray()
var _accum: float = 0.0
var _debugger_plugin: EditorDebuggerPlugin = null

# v0.28.4 (Phase 3.3c-3): persistence layer. The model owns
# gool/config.json — reads on _ready, patches+writes on every
# fader / slider edit (debounced). The dock uses it for:
#   - view-at-rest: when no F5 session is running, _lookup_effects_for_bus
#     falls through to model.get_effects() so the Fx panel still works
#   - persistence: _on_strip_db_changed and _on_effect_param_changed
#     now ALSO write through to the model in addition to the runtime
#   - dirty indicator: a small dot rendered on a strip's name band
#     when that bus has unsaved local edits
#   - conflict prompt: if config.json was modified externally
#     between our last load and our next save, the dock shows a
#     ConfirmationDialog with Reload / Overwrite / Cancel
var _config_model: GoolConfigModel = null
var _mtime_conflict_dialog: ConfirmationDialog = null


# ---- Plugin lifecycle ------------------------------------------------

func set_debugger_plugin(p: EditorDebuggerPlugin) -> void:
	_debugger_plugin = p


func _ready() -> void:
	custom_minimum_size = Vector2(0, STRIP_HEIGHT + 24)

	var root_vbox := VBoxContainer.new()
	root_vbox.anchor_right = 1.0
	root_vbox.anchor_bottom = 1.0
	add_child(root_vbox)

	var scroll := ScrollContainer.new()
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	root_vbox.add_child(scroll)

	_strip_container = HBoxContainer.new()
	_strip_container.add_theme_constant_override("separation", int(STRIP_GAP))
	scroll.add_child(_strip_container)

	_empty_label = Label.new()
	_empty_label.text = "  No gool/config.json found.\n  Mixer is unavailable until a config is present."
	_empty_label.add_theme_color_override("font_color", COLOR_TEXT_DIM)
	_strip_container.add_child(_empty_label)

	# v0.28.4 (Phase 3.3c-3): instantiate the persistence model and
	# load gool/config.json. This is BEFORE strips are built so
	# _load_static_layout_from_config can read from the model rather
	# than parsing config.json a second time. Model failure (missing
	# file, bad JSON) leaves it empty — dock falls back to the
	# "No gool/config.json found" empty-label as before.
	_config_model = GoolConfigModel.new()
	_config_model.set_owner_node(self)
	_config_model.model_saved.connect(_on_model_saved)
	_config_model.save_failed.connect(_on_model_save_failed)
	_config_model.external_change_detected.connect(
			_on_model_external_change_detected)
	var load_err: int = _config_model.load_from_disk()
	if load_err != OK and load_err != ERR_FILE_NOT_FOUND:
		push_warning("[gool] config_model load_from_disk: error %d" % load_err)

	# v0.26.0: build static layout from config.json IMMEDIATELY.
	# Strips are visible at editor time; live data comes later
	# when F5 starts (via debugger plugin).
	_load_static_layout_from_config()

	# v0.28.4: pre-build (but hide) the mtime conflict dialog so it's
	# ready to pop the instant the model emits external_change_detected.
	_mtime_conflict_dialog = ConfirmationDialog.new()
	_mtime_conflict_dialog.title = "gool: config.json changed on disk"
	_mtime_conflict_dialog.ok_button_text = "Reload from disk"
	_mtime_conflict_dialog.add_button("Overwrite with dock state", true,
			"overwrite_disk_action")
	_mtime_conflict_dialog.confirmed.connect(_on_mtime_dialog_reload)
	_mtime_conflict_dialog.custom_action.connect(_on_mtime_dialog_custom_action)
	add_child(_mtime_conflict_dialog)


func _process(delta: float) -> void:
	_accum += delta
	var poll_period := 1.0 / POLL_HZ
	if _accum >= poll_period:
		_accum = 0.0
		_poll()


# ---- Static layout from config.json ----------------------------------

# Read gool/config.json from the project filesystem and build one
# _BusStrip per declared bus. Faders are initialized to each bus's
# `gain_db` (or 0.0 if absent). Topology (parent chain) is captured
# for future use (3.3d's indent/visual-hierarchy work).
#
# Failure modes are non-fatal: empty file → empty state, malformed
# JSON → empty state with a warning print. The dock recovers as
# soon as a valid config is present.
func _load_static_layout_from_config() -> void:
	var buses := _read_buses_from_config()
	if buses.is_empty():
		_empty_label.text = "  No gool/config.json found, or it's empty.\n  Add buses to see them here."
		_empty_label.visible = true
		return
	_rebuild_strips_from_config(buses)
	_last_bus_names = _strip_names_packed(buses)


func _read_buses_from_config() -> Array:
	if not FileAccess.file_exists(CONFIG_PATH):
		return []
	var f := FileAccess.open(CONFIG_PATH, FileAccess.READ)
	if f == null:
		return []
	var txt := f.get_as_text()
	f.close()
	var parsed: Variant = JSON.parse_string(txt)
	if parsed == null or not (parsed is Dictionary):
		push_warning("[gool] mixer dock: %s is malformed JSON" % CONFIG_PATH)
		return []
	var buses_raw: Variant = parsed.get("buses", [])
	if not (buses_raw is Array):
		return []
	var out: Array = []
	for b in buses_raw:
		if not (b is Dictionary):
			continue
		var d: Dictionary = b
		out.append({
			"name":     String(d.get("name", "")),
			"parent":   String(d.get("parent", "")),
			"gain_db":  float(d.get("gain_db", 0.0)),
		})
	return out


func _strip_names_packed(buses: Array) -> PackedStringArray:
	var out := PackedStringArray()
	for b in buses:
		out.append(String(b.get("name", "")))
	return out


func _rebuild_strips_from_config(buses: Array) -> void:
	_empty_label.visible = false
	# v0.28.3: freeing the column also frees the strip (and any
	# attached effects panel) recursively. Reset _expanded_bus
	# because any prior panel is now gone.
	for c in _columns:
		c.queue_free()
	_strips.clear()
	_columns.clear()
	_expanded_bus = ""
	for d in buses:
		var bus_name: String = String(d.get("name", "(unnamed)"))
		var initial_db: float = float(d.get("gain_db", 0.0))
		var strip := _BusStrip.new()
		strip.bus_name = bus_name
		strip.set_fader_db(initial_db, false)  # silent (no signal emit)
		strip.custom_minimum_size = Vector2(STRIP_WIDTH, STRIP_HEIGHT)
		strip.db_changed.connect(_on_strip_db_changed)
		# v0.27.0: S/M/B signal forwarding.
		strip.mute_changed.connect(_on_strip_mute_changed)
		strip.solo_changed.connect(_on_strip_solo_changed)
		strip.bypass_changed.connect(_on_strip_bypass_changed)
		# v0.28.3: Fx toggle.
		strip.fx_toggled.connect(_on_strip_fx_toggled)
		# Wrap in a column so the effects panel can stack below.
		var col := VBoxContainer.new()
		col.add_theme_constant_override("separation", 4)
		col.add_child(strip)
		_strip_container.add_child(col)
		_strips.append(strip)
		_columns.append(col)


# ---- Live data polling (during F5) -----------------------------------

func _poll() -> void:
	if _debugger_plugin == null:
		# No debugger plugin wired yet (very early editor startup).
		# Keep showing the static config layout.
		return
	var stats: Array = _debugger_plugin.get_latest_bus_stats()
	_last_stats = stats  # cache for fx_toggled handler
	if stats.is_empty():
		# Game not running. Reset all meters to floor; faders keep
		# whatever the user has set them to.
		# v0.28.3: also reset effect counts to 0 — this force-collapses
		# any open Fx panel via set_effect_count's internal logic.
		# Same UX rationale as muting the meters: with no game running,
		# stale effect parameters from a previous session shouldn't
		# stay clickable.
		# v0.28.4 (Phase 3.3c-3): when at rest, effect_count is sourced
		# from the config model rather than forced to 0 — so view-at-rest
		# shows Fx buttons populated from gool/config.json without
		# requiring an F5. The button click → panel open path then uses
		# the model too via _lookup_effects_for_bus's fallback.
		for s in _strips:
			s.push_peak(0.0)
			var count_at_rest: int = 0
			if _config_model != null:
				count_at_rest = _config_model.get_effects(s.bus_name).size()
			s.set_effect_count(count_at_rest)
		return

	# If the runtime's reported bus list differs from our config-based
	# static layout, rebuild from the runtime list. This handles the
	# case where the runtime's config differs from what's on disk
	# (e.g. user edited config.json then F5'd before reloading the
	# editor). Runtime-reported topology wins during gameplay.
	var names := PackedStringArray()
	for d in stats:
		names.append(String(d.get("name", "")))
	if names != _last_bus_names:
		_rebuild_strips_from_runtime(stats)
		_last_bus_names = names

	# Push peak values into the existing strips.
	#
	# v0.27.0: stats payload now also carries muted/soloed/bypassed,
	# but we DO NOT push those into the strips. Rationale: the dock's
	# S/M/B state is authoritative for what the USER clicked locally.
	# If we synced runtime → dock here, any local click would visually
	# flicker (toggle on → next poll says "still off in runtime" →
	# button un-pops) until the runtime's atomic update lands. Better
	# UX is "click is instant + sticky." The trade-off is dock state
	# can diverge from runtime state if some other code path changes
	# the runtime state mid-session (unusual). Future improvement:
	# push dock state TO runtime on F5 start so initial state matches.
	#
	# v0.28.3: effects payload IS pushed (as effect_count), since
	# the count drives Fx button visibility and the user can't have
	# diverging local truth for "does this bus have effects" — either
	# they're there or they aren't. Param values are still local
	# truth while a panel is open; build_from_effects is only called
	# on panel open.
	for i in stats.size():
		if i >= _strips.size():
			break
		var d: Dictionary = stats[i]
		var peak: float = float(d.get("peak_linear", 0.0))
		_strips[i].push_peak(peak)
		var effects_v: Variant = d.get("effects", [])
		var effects_arr: Array = effects_v if effects_v is Array else []
		_strips[i].set_effect_count(effects_arr.size())


# Runtime topology differs from config — build strips from runtime
# stats. Fader values default to 0 dB since runtime stats don't
# currently include them (a future addition could).
func _rebuild_strips_from_runtime(stats: Array) -> void:
	_empty_label.visible = false
	# v0.28.3: see _rebuild_strips_from_config for free semantics.
	for c in _columns:
		c.queue_free()
	_strips.clear()
	_columns.clear()
	_expanded_bus = ""
	for d in stats:
		var strip := _BusStrip.new()
		strip.bus_name = String(d.get("name", "(unnamed)"))
		strip.set_fader_db(0.0, false)
		strip.custom_minimum_size = Vector2(STRIP_WIDTH, STRIP_HEIGHT)
		strip.db_changed.connect(_on_strip_db_changed)
		# v0.27.0: S/M/B signal forwarding (matches _rebuild_strips_from_config).
		strip.mute_changed.connect(_on_strip_mute_changed)
		strip.solo_changed.connect(_on_strip_solo_changed)
		strip.bypass_changed.connect(_on_strip_bypass_changed)
		# v0.28.3: Fx toggle.
		strip.fx_toggled.connect(_on_strip_fx_toggled)
		var col := VBoxContainer.new()
		col.add_theme_constant_override("separation", 4)
		col.add_child(strip)
		_strip_container.add_child(col)
		_strips.append(strip)
		_columns.append(col)


# Strip fader was dragged. Forward to the running game via the
# debugger plugin's send helper. Silently dropped if no game
# running (correct behavior — fader still moves in the UI).
func _on_strip_db_changed(bus_name: String, db: float) -> void:
	# v0.28.4: persist the edit to config.json via the model. Done
	# unconditionally — runtime forwarding only happens when a session
	# is attached, but the config write should happen either way so
	# the edit survives F5 cycles and editor restarts.
	if _config_model != null:
		_config_model.set_bus_gain_db(bus_name, db)
		_refresh_dirty_indicators()
	if _debugger_plugin == null:
		return
	if _debugger_plugin.has_method("send_set_bus_gain"):
		_debugger_plugin.send_set_bus_gain(bus_name, db)


# v0.27.0: S/M/B click forwarders. Same drop-when-no-session
# behavior as the fader: editor-mode clicks update the dock visually
# but go nowhere; F5-mode clicks reach the running game.
func _on_strip_mute_changed(bus_name: String, muted: bool) -> void:
	if _debugger_plugin == null:
		return
	if _debugger_plugin.has_method("send_set_bus_mute"):
		_debugger_plugin.send_set_bus_mute(bus_name, muted)


func _on_strip_solo_changed(bus_name: String, soloed: bool) -> void:
	if _debugger_plugin == null:
		return
	if _debugger_plugin.has_method("send_set_bus_solo"):
		_debugger_plugin.send_set_bus_solo(bus_name, soloed)


func _on_strip_bypass_changed(bus_name: String, bypassed: bool) -> void:
	if _debugger_plugin == null:
		return
	if _debugger_plugin.has_method("send_set_bus_bypass"):
		_debugger_plugin.send_set_bus_bypass(bus_name, bypassed)


# v0.28.3: Fx button toggled on a strip.
#
# On expand:
#   - Auto-collapse any previously-expanded strip (one panel at a time).
#   - Look up the latest effects payload for this bus.
#   - Build a new _EffectsPanel, attach to the strip's column.
#
# On collapse:
#   - Find and free the panel from the strip's column.
#
# Defensive: if the bus name isn't found in _strips (shouldn't happen
# since the signal is emitted by an _BusStrip we own), silently no-op.
func _on_strip_fx_toggled(bus_name: String, expanded: bool) -> void:
	var idx: int = _index_of_bus(bus_name)
	if idx < 0:
		return
	var col: VBoxContainer = _columns[idx] as VBoxContainer

	if expanded:
		# Collapse previous if there is one (and it's not this same strip).
		if _expanded_bus != "" and _expanded_bus != bus_name:
			var prev_idx: int = _index_of_bus(_expanded_bus)
			if prev_idx >= 0:
				# Silent set so we don't reentrantly fire the signal.
				(_strips[prev_idx] as _BusStrip).set_fx_expanded(false, false)
				_remove_panel_from_column(_columns[prev_idx] as VBoxContainer)
		# Now build the new panel.
		var effects: Array = _lookup_effects_for_bus(bus_name)
		var panel := _EffectsPanel.new()
		panel.bus_name = bus_name
		panel.param_changed.connect(_on_effect_param_changed)
		col.add_child(panel)
		# build_from_effects must happen AFTER add_child so child
		# Controls have a tree parent before any internal init paths
		# query the tree (some Godot Controls latch theme/style values
		# at _enter_tree).
		panel.build_from_effects(effects)
		_expanded_bus = bus_name
		# v0.28.3: grow the dock's minimum height so the bottom panel
		# expands to show the effects panel without the user having to
		# drag the bottom panel taller. _PANEL_MIN_EXTRA covers a
		# typical 2–3 effect bus; very tall chains (e.g. 3 compressors)
		# will scroll inside the existing ScrollContainer.
		custom_minimum_size = Vector2(0, STRIP_HEIGHT + 24 + _PANEL_MIN_EXTRA)
	else:
		_remove_panel_from_column(col)
		if _expanded_bus == bus_name:
			_expanded_bus = ""
		# Restore the strips-only minimum height. The user keeps any
		# manual height they dragged (custom_minimum_size is a floor,
		# not a forced height).
		custom_minimum_size = Vector2(0, STRIP_HEIGHT + 24)


# Effect slider was dragged. Forward to the running game via the
# debugger plugin. Silently dropped if no game running — same
# convention as fader/SMB forwarders.
#
# v0.28.4: also writes the edit through to GoolConfigModel so the
# value persists to config.json. The runtime forwarding only fires
# during F5; the model write fires either way (at-rest edits land
# in config.json without a game running, runtime edits both ramp
# the engine value AND get persisted).
func _on_effect_param_changed(bus_name: String, effect_index: int,
		param_id: int, value: float) -> void:
	if _config_model != null:
		_config_model.set_effect_param(bus_name, effect_index, param_id, value)
		_refresh_dirty_indicators()
	if _debugger_plugin == null:
		return
	if _debugger_plugin.has_method("send_set_effect_parameter"):
		_debugger_plugin.send_set_effect_parameter(
				bus_name, effect_index, param_id, value)


# Linear search _strips for a bus name. O(n) but n is tiny (max 32
# buses) and this is called on user click, not in a hot loop.
func _index_of_bus(bus_name: String) -> int:
	for i in _strips.size():
		if (_strips[i] as _BusStrip).bus_name == bus_name:
			return i
	return -1


# Look up the effects array for a bus from the most recent poll.
# Returns [] if the bus isn't in stats (game not running, or the
# bus exists in static config but not in the runtime topology).
#
# v0.28.4: if the runtime stats cache is empty (no F5 running),
# fall back to GoolConfigModel.get_effects() which serves the same
# shape from gool/config.json. This is what makes view-at-rest work:
# the user can open Fx panels and see param values populated from
# config without needing to F5.
func _lookup_effects_for_bus(bus_name: String) -> Array:
	for d in _last_stats:
		if String(d.get("name", "")) == bus_name:
			var effects_v: Variant = d.get("effects", [])
			return effects_v if effects_v is Array else []
	if _config_model != null:
		return _config_model.get_effects(bus_name)
	return []


# v0.28.4: model save signals → dock visual response.
#
# _on_model_saved fires after a successful disk write. Strips that
# were previously dirty (showing the modified dot) now refresh to
# clear their indicators.
func _on_model_saved(_bus_names_saved: Array) -> void:
	_refresh_dirty_indicators()


# Save failures are logged loudly. The dock keeps running; the model
# preserves its in-memory state, so the next edit + save attempt will
# include the unsaved edits. The user sees the failure in Godot's
# Output panel and can investigate.
func _on_model_save_failed(reason: String) -> void:
	push_warning("[gool] mixer dock: config save failed — " + reason)


# External-change conflict (disk mtime advanced since last load).
# Show the prompt: Reload from disk (discards dock edits) or
# Overwrite (clobbers external edits with dock state).
func _on_model_external_change_detected(pending_dirty_buses: Array) -> void:
	if _mtime_conflict_dialog == null:
		return
	var bus_list: String = ", ".join(pending_dirty_buses)
	if bus_list.is_empty():
		bus_list = "(none)"
	_mtime_conflict_dialog.dialog_text = (
		"gool/config.json was modified outside the editor while you had "
		+ "unsaved changes to: " + bus_list + ".\n\n"
		+ "  Reload from disk: discard the dock's in-memory edits, "
		+ "reread config.json from disk.\n"
		+ "  Overwrite with dock state: clobber the external changes "
		+ "with the dock's current state."
	)
	_mtime_conflict_dialog.popup_centered()


func _on_mtime_dialog_reload() -> void:
	# ConfirmationDialog's "OK" button = our "Reload" action.
	if _config_model == null:
		return
	var err: int = _config_model.reload_from_disk_discarding_edits()
	if err == OK:
		_load_static_layout_from_config()
		_refresh_dirty_indicators()


func _on_mtime_dialog_custom_action(action: StringName) -> void:
	if String(action) != "overwrite_disk_action":
		return
	if _config_model == null:
		return
	_config_model.overwrite_disk()
	_mtime_conflict_dialog.hide()


# Walk strips and push their bus's dirty state. Called whenever the
# model emits a change that affects any bus's dirty status (an edit,
# a save, a reload). Strip rendering decides what the dot looks like.
func _refresh_dirty_indicators() -> void:
	if _config_model == null:
		return
	for s in _strips:
		var strip: _BusStrip = s as _BusStrip
		strip.set_dirty(_config_model.is_bus_dirty(strip.bus_name))


# Remove the _EffectsPanel child from a column, if any. The strip
# itself stays. Safe to call on a column without a panel (no-op).
func _remove_panel_from_column(col: VBoxContainer) -> void:
	for child in col.get_children():
		if child is _EffectsPanel:
			child.queue_free()
			return


# ---- _BusStrip widget ------------------------------------------------

class _BusStrip extends Control:
	signal db_changed(bus_name: String, new_db: float)

	# v0.26.2: constants duplicated from the outer GoolMixerDock
	# rather than referenced via `GoolMixerDock.X`. This is required
	# because Godot's --headless mode (used by the headless-smoke CI
	# job) does NOT populate the global class_name registry — so
	# `GoolMixerDock.FADER_MIN_DB` from inside this inner class fails
	# to resolve at parse time, load() returns null, SMOKE FAIL.
	#
	# The fix is small (a few duplicated values) and keeps this inner
	# class self-contained. The outer-class copies (mixer_dock.gd
	# top-level) drive layout sizing for the outer code; these
	# inner-class copies drive the strip's own drawing/input math.
	# Keep them in sync if either side needs to change.
	const FADER_MAX_DB: float = 6.0
	const FADER_MIN_DB: float = -72.0
	const PEAK_DECAY: float = 0.85
	const DB_GREEN_MAX: float = -12.0
	const DB_YELLOW_MAX: float = -6.0
	const DB_METER_FLOOR: float = -60.0
	const DB_METER_CEIL: float = 6.0
	const SCALE_MARKS_DB: Array = [6.0, 0.0, -6.0, -24.0, -72.0]
	const COLOR_GREEN := Color(0.36, 0.83, 0.46)
	const COLOR_YELLOW := Color(0.95, 0.82, 0.32)
	const COLOR_RED := Color(0.93, 0.40, 0.32)
	const COLOR_BG := Color(0.10, 0.10, 0.12)
	const COLOR_BG_OUTLINE := Color(0.20, 0.20, 0.22)
	const COLOR_TEXT := Color(0.85, 0.88, 0.92)
	const COLOR_TEXT_DIM := Color(0.55, 0.58, 0.62)
	const COLOR_TICK := Color(0.45, 0.45, 0.48)
	const COLOR_FADER_TRACK := Color(0.20, 0.22, 0.26)
	const COLOR_FADER_HANDLE := Color(0.65, 0.72, 0.85)
	const COLOR_FADER_HANDLE_ACTIVE := Color(0.78, 0.88, 0.95)
	# v0.27.0: S/M/B button colors. Inactive uses the same dark grey
	# as the fader track for visual consistency. Active colors picked
	# to match DAW convention:
	#   - Solo: yellow (the existing COLOR_YELLOW from the meter zones,
	#     repurposed — solo is the "focus / attention" button in most
	#     DAWs)
	#   - Mute: red (the existing COLOR_RED — mute kills audio, red is
	#     the universal "stop / silenced" signal)
	#   - Bypass: muted purple, distinct from the other two and from
	#     any meter zone color so an active bypass can't be confused
	#     with a hot peak or a solo
	const COLOR_BUTTON_INACTIVE := Color(0.20, 0.22, 0.26)
	const COLOR_BUTTON_BYPASS_ACTIVE := Color(0.55, 0.55, 0.85)
	const COLOR_BUTTON_LABEL := Color(0.95, 0.95, 0.95)
	const COLOR_BUTTON_OUTLINE := Color(0.35, 0.37, 0.42)

	var bus_name: String = ""
	var _peak_smoothed: float = 0.0
	var _peak_held: float = 0.0
	var _peak_held_age: float = 0.0
	var _fader_db: float = 0.0
	var _fader_dragging: bool = false
	# v0.27.0: S/M/B local state. Authoritative for the dock's visual
	# state; mirrored to the runtime via the debugger channel on each
	# user click. Does NOT auto-sync from poll (see notes in
	# CHANGELOG and the GoolMixerDock-level _poll handler below).
	var _is_muted: bool = false
	var _is_soloed: bool = false
	var _is_bypassed: bool = false
	# v0.28.3 (Phase 3.3c-2): Fx button state. _effect_count drives
	# button visibility (no effects → no button, avoids click-leads-
	# nowhere UX). _is_fx_expanded drives the toggle visual and is
	# kept in sync with the parent column's panel presence.
	var _effect_count: int = 0
	var _is_fx_expanded: bool = false

	# v0.28.4 (Phase 3.3c-3): dirty indicator state. Set by the outer
	# dock via set_dirty() whenever the config model reports this bus
	# has unsaved edits. Drawn as a small pip to the left of the bus
	# name in _draw. Uses the same yellow as Solo so the visual
	# vocabulary stays compact — yellow = "this needs your attention".
	var _is_dirty: bool = false
	const COLOR_DIRTY_DOT := Color(0.95, 0.82, 0.32)

	const PEAK_HOLD_TIME: float = 1.5
	const PEAK_DROP_RATE: float = 0.5

	# Geometry inside the strip. Computed in _draw / _gui_input from
	# size, derived consistently so the fader-handle hit region
	# matches the visually-rendered handle position.
	const NAME_BAND: float = 18.0
	const READOUT_BAND: float = 18.0
	const FADER_TRACK_W: float = 4.0
	const FADER_HANDLE_W: float = 32.0
	const FADER_HANDLE_H: float = 10.0
	const METER_W: float = 14.0
	const SCALE_LABEL_W: float = 30.0
	# v0.27.0: S/M/B button strip between NAME_BAND and the fader
	# region. 20px tall total: 16px for the buttons + 2px top padding
	# + 2px bottom padding. Three buttons in a row, BUTTON_W wide each
	# with BUTTON_GAP between, centered horizontally. Total content
	# width: BUTTON_W*3 + BUTTON_GAP*2 = 86px (leaves 5px margin on
	# either side of a 96px-wide strip — see STRIP_WIDTH in the outer
	# class).
	const BUTTON_BAND: float = 20.0
	const BUTTON_W: float = 26.0
	const BUTTON_H: float = 16.0
	const BUTTON_GAP: float = 4.0
	# v0.28.3 (Phase 3.3c-2): Fx button lives in its own band below
	# the readout, full-width minus a small margin. Spatial choice:
	# placing it at the bottom of the strip puts it adjacent to where
	# the effects panel expands BELOW the strip, so the click target
	# and the result are visually contiguous. Not crammed into the
	# S/M/B band because Fx is a different kind of action (reveal,
	# not toggle) and visually grouping it with the modal S/M/B
	# buttons would confuse the hierarchy.
	const FX_BAND: float = 22.0
	const FX_BUTTON_H: float = 18.0
	const FX_BUTTON_INSET_X: float = 6.0
	const COLOR_FX_BUTTON_ACTIVE := Color(0.45, 0.60, 0.85)

	# v0.27.0: S/M/B change signals. Emitted when the user clicks the
	# corresponding button. The outer GoolMixerDock forwards these to
	# the debugger plugin's send_set_bus_mute / send_set_bus_solo /
	# send_set_bus_bypass helpers.
	signal mute_changed(bus_name: String, muted: bool)
	signal solo_changed(bus_name: String, soloed: bool)
	signal bypass_changed(bus_name: String, bypassed: bool)
	# v0.28.3 (Phase 3.3c-2): Fx button click. Emitted with the NEW
	# expansion state. Outer dock catches this and builds / removes
	# the effects panel below the strip in the column wrapper.
	signal fx_toggled(bus_name: String, expanded: bool)

	# v0.26.5: LineEdit child for click-to-type dB entry. Hidden by
	# default; shown when the user clicks the dB readout at the
	# bottom of the strip. Lifecycle: see _start_db_edit /
	# _commit_db_edit / _cancel_db_edit below.
	var _db_editor: LineEdit = null

	func _ready() -> void:
		set_process(true)
		mouse_filter = Control.MOUSE_FILTER_STOP
		# v0.26.5: build the LineEdit overlay for click-to-edit dB
		# entry. Lives in the bottom 18px (READOUT_BAND), normally
		# hidden, shown on click of the readout text. Connected
		# signals: text_submitted (Enter) → commit, focus_exited
		# (click elsewhere) → commit, gui_input → check for Escape.
		_db_editor = LineEdit.new()
		_db_editor.visible = false
		_db_editor.alignment = HORIZONTAL_ALIGNMENT_CENTER
		_db_editor.placeholder_text = "dB"
		# Reasonable text size; LineEdit's defaults are fine for the
		# rest of styling — we don't override the theme.
		_db_editor.text_submitted.connect(_on_db_edit_submitted)
		_db_editor.focus_exited.connect(_on_db_edit_focus_exited)
		_db_editor.gui_input.connect(_on_db_edit_gui_input)
		add_child(_db_editor)

	# Set the fader value programmatically. emit_signal=false used at
	# init time to avoid feedback before the parent has connected.
	func set_fader_db(db: float, emit: bool = true) -> void:
		_fader_db = clampf(db, FADER_MIN_DB, FADER_MAX_DB)
		queue_redraw()
		if emit:
			db_changed.emit(bus_name, _fader_db)

	# v0.27.0: programmatic setters for S/M/B state. emit=false is the
	# init-time path (no parent connection yet). The runtime-sync path
	# (e.g. on F5 start to push editor-state into the game) would use
	# emit=true to fire the signal and forward through the debugger
	# channel — currently unused; reserved for the "sync local state
	# to runtime on F5 start" follow-up flagged in v0.27.0 CHANGELOG.
	func set_muted(muted: bool, emit: bool = true) -> void:
		if _is_muted == muted:
			return
		_is_muted = muted
		queue_redraw()
		if emit:
			mute_changed.emit(bus_name, _is_muted)

	func set_soloed(soloed: bool, emit: bool = true) -> void:
		if _is_soloed == soloed:
			return
		_is_soloed = soloed
		queue_redraw()
		if emit:
			solo_changed.emit(bus_name, _is_soloed)

	func set_bypassed(bypassed: bool, emit: bool = true) -> void:
		if _is_bypassed == bypassed:
			return
		_is_bypassed = bypassed
		queue_redraw()
		if emit:
			bypass_changed.emit(bus_name, _is_bypassed)

	# v0.28.3 (Phase 3.3c-2): set the effect count for this bus.
	# Drives the Fx button visibility (count > 0 → button shown
	# with "Fx (N)" label) and its label refresh on count change.
	# Called from the outer dock during _poll from each stats[i].effects
	# array size.
	func set_effect_count(n: int) -> void:
		if _effect_count == n:
			return
		_effect_count = n
		# If the bus dropped to 0 effects while expanded, force
		# collapse — there's nothing to show.
		if _effect_count == 0 and _is_fx_expanded:
			set_fx_expanded(false, true)
			return
		queue_redraw()

	# v0.28.3 (Phase 3.3c-2): programmatic Fx expansion setter. The
	# outer dock uses emit=false when forcing a collapse (e.g. to
	# auto-collapse a previously-expanded strip when the user opens
	# a different one) to avoid a signal echo.
	func set_fx_expanded(expanded: bool, emit: bool = true) -> void:
		if _is_fx_expanded == expanded:
			return
		_is_fx_expanded = expanded
		queue_redraw()
		if emit:
			fx_toggled.emit(bus_name, _is_fx_expanded)

	# v0.28.4 (Phase 3.3c-3): dirty-indicator setter. Outer dock
	# calls this from _refresh_dirty_indicators when the model
	# reports a change. No-op if state is unchanged so we don't
	# trigger redundant queue_redraw calls.
	func set_dirty(dirty: bool) -> void:
		if _is_dirty == dirty:
			return
		_is_dirty = dirty
		queue_redraw()

	# Button rect helpers. Compute once, used by both _draw and
	# _gui_input — keeping these in one place prevents the
	# rendered-vs-hittable mismatch the lessons doc warns about
	# under "Hit rect math must match draw rect math".
	func _button_row_y() -> float:
		return NAME_BAND + 2.0  # 2px top padding inside BUTTON_BAND

	func _button_row_start_x() -> float:
		var content_w: float = BUTTON_W * 3.0 + BUTTON_GAP * 2.0
		return (size.x - content_w) * 0.5

	func _solo_rect() -> Rect2:
		return Rect2(_button_row_start_x(), _button_row_y(),
				BUTTON_W, BUTTON_H)

	func _mute_rect() -> Rect2:
		return Rect2(_button_row_start_x() + (BUTTON_W + BUTTON_GAP),
				_button_row_y(),
				BUTTON_W, BUTTON_H)

	func _bypass_rect() -> Rect2:
		return Rect2(_button_row_start_x() + (BUTTON_W + BUTTON_GAP) * 2.0,
				_button_row_y(),
				BUTTON_W, BUTTON_H)

	# v0.28.3: Fx button rect (bottom of strip). Geometry mirrors the
	# FX_BAND constants — FX_BUTTON_INSET_X of margin on each side,
	# FX_BUTTON_H tall with 2px of breathing room above. Both _draw
	# and _gui_input call this so the visual and the hit region are
	# guaranteed to match.
	func _fx_button_rect() -> Rect2:
		return Rect2(FX_BUTTON_INSET_X,
				size.y - FX_BAND + 2.0,
				size.x - FX_BUTTON_INSET_X * 2.0,
				FX_BUTTON_H)

	# ---- v0.26.5: click-to-edit dB readout ----

	# Activate the inline LineEdit over the readout band. Pre-fills
	# with the current dB as a plain number (no "+" prefix, no
	# " dB" suffix — those are display, not input) so typing
	# replaces cleanly.
	func _start_db_edit() -> void:
		if _db_editor == null:
			return
		if _db_editor.visible:
			return  # already editing
		# Position over the readout band. Some horizontal margin so
		# the editor doesn't run flush to the strip edges.
		# v0.28.3: y shifted by FX_BAND since the readout is no
		# longer bottom-anchored — FX_BAND sits below it.
		var pad: float = 4.0
		_db_editor.position = Vector2(pad, size.y - FX_BAND - READOUT_BAND)
		_db_editor.size = Vector2(size.x - pad * 2.0, READOUT_BAND)
		_db_editor.text = "%.1f" % _fader_db
		_db_editor.visible = true
		_db_editor.grab_focus()
		_db_editor.select_all()
		# Hide the drawn readout (drawn in _draw) while editing.
		queue_redraw()

	# Parse the text as a float, clamp to the fader range, apply.
	# Invalid input (non-numeric, empty) is silently discarded —
	# the fader value stays at its prior position.
	func _commit_db_edit(text: String) -> void:
		var stripped: String = text.strip_edges()
		if stripped.is_empty():
			_hide_db_editor()
			return
		# String.to_float() returns 0.0 for unparseable input, which
		# would be a dangerous silent default. Use is_valid_float()
		# to distinguish "0.0 explicitly typed" from "garbage".
		if not stripped.is_valid_float():
			# Could be "+5.5" with explicit plus; to_float handles
			# this but is_valid_float rejects it on some Godot
			# versions. Strip a leading + and retry once.
			if stripped.begins_with("+") and stripped.substr(1).is_valid_float():
				stripped = stripped.substr(1)
			else:
				_hide_db_editor()
				return
		var value: float = stripped.to_float()
		# set_fader_db clamps to [FADER_MIN_DB, FADER_MAX_DB] and
		# emits db_changed, which the parent dock forwards to the
		# running game via the debugger bridge. So out-of-range
		# values like "-100" become -72 (the floor) silently —
		# matches the fader's drag behavior.
		set_fader_db(value, true)
		_hide_db_editor()

	func _cancel_db_edit() -> void:
		_hide_db_editor()

	func _hide_db_editor() -> void:
		if _db_editor == null:
			return
		_db_editor.visible = false
		# Release focus back to the strip so subsequent fader drags
		# work without an extra click.
		grab_focus()
		queue_redraw()

	func _on_db_edit_submitted(text: String) -> void:
		_commit_db_edit(text)

	func _on_db_edit_focus_exited() -> void:
		# focus_exited fires on Escape too in some Godot versions,
		# but Escape is also handled in _on_db_edit_gui_input below
		# (which cancels before focus_exited would commit). Safe to
		# treat focus_exited as "commit current text".
		if _db_editor != null and _db_editor.visible:
			_commit_db_edit(_db_editor.text)

	func _on_db_edit_gui_input(event: InputEvent) -> void:
		# Escape cancels without committing. Without this, Escape
		# would just lose focus and the focus_exited handler would
		# commit whatever's in the box — wrong UX.
		if event is InputEventKey:
			var k := event as InputEventKey
			if k.pressed and k.keycode == KEY_ESCAPE:
				_cancel_db_edit()
				_db_editor.accept_event()

	# Push a new peak value from the runtime. Snappy attack (peak
	# wins immediately on rise), exponential decay on fall.
	func push_peak(peak_linear: float) -> void:
		if peak_linear > _peak_smoothed:
			_peak_smoothed = peak_linear
		else:
			_peak_smoothed = _peak_smoothed * PEAK_DECAY \
					+ peak_linear * (1.0 - PEAK_DECAY)
		if peak_linear >= _peak_held:
			_peak_held = peak_linear
			_peak_held_age = 0.0
		queue_redraw()

	func _process(delta: float) -> void:
		_peak_held_age += delta
		if _peak_held_age > PEAK_HOLD_TIME:
			_peak_held = maxf(0.0, _peak_held - PEAK_DROP_RATE * delta)
			queue_redraw()

	# ---- Coordinate helpers ----

	# Vertical Y inside the fader/meter region (between NAME_BAND
	# top and READOUT_BAND bottom) for a given dB value. Linear in
	# dB across the visible fader range.
	func _db_to_y(db: float, top: float, height: float) -> float:
		var t: float = (FADER_MAX_DB - db) \
				/ (FADER_MAX_DB - FADER_MIN_DB)
		t = clampf(t, 0.0, 1.0)
		return top + t * height

	func _y_to_db(y: float, top: float, height: float) -> float:
		if height <= 0.0:
			return 0.0
		var t: float = clampf((y - top) / height, 0.0, 1.0)
		return FADER_MAX_DB - t * (FADER_MAX_DB - FADER_MIN_DB)

	# Linear-peak → meter Y (different range from fader: meter
	# bottoms out at DB_METER_FLOOR not -72).
	func _peak_linear_to_meter_y(peak: float, top: float, height: float) -> float:
		if peak <= 0.0:
			return top + height
		var db: float = 20.0 * (log(peak) / log(10.0))
		var t: float = (db - DB_METER_FLOOR) \
				/ (DB_METER_CEIL - DB_METER_FLOOR)
		t = clampf(t, 0.0, 1.0)
		return top + (1.0 - t) * height

	# ---- Drawing ----

	func _draw() -> void:
		var w: float = size.x
		var h: float = size.y
		var f := get_theme_default_font()
		var fs := get_theme_default_font_size()
		var fs_small := maxi(fs - 2, 8)

		# v0.27.0: fader region shifted down to make room for the
		# BUTTON_BAND (S/M/B buttons sit between the bus name and the
		# fader). Both _draw and _gui_input compute the region the
		# same way — if you change one, change the other.
		# v0.28.3: FX_BAND eats 22px from the bottom (below READOUT_BAND)
		# to host the Fx toggle button. Same math change here AND in
		# _gui_input — both compute the region identically.
		var fader_region_y: float = NAME_BAND + BUTTON_BAND + 4.0
		var fader_region_h: float = h - NAME_BAND - BUTTON_BAND - READOUT_BAND - FX_BAND - 8.0

		# --- Bus name (top) ---
		if f != null:
			var name_size := f.get_string_size(
					bus_name, HORIZONTAL_ALIGNMENT_CENTER, -1, fs)
			draw_string(f,
					Vector2((w - name_size.x) * 0.5, NAME_BAND - 4),
					bus_name,
					HORIZONTAL_ALIGNMENT_CENTER,
					-1, fs,
					COLOR_TEXT)
			# v0.28.4: dirty indicator. Small filled circle to the LEFT
			# of the bus name when this bus has unsaved local edits in
			# GoolConfigModel. Disappears once the debounced save lands.
			# Position deliberately compact — 4px diameter, 2px inside
			# the strip's left margin — so it reads as a status pip
			# rather than a separate UI element.
			if _is_dirty:
				var dot_x: float = max(2.0, (w - name_size.x) * 0.5 - 8.0)
				var dot_y: float = NAME_BAND * 0.5 - 1.0
				draw_circle(Vector2(dot_x, dot_y), 3.0,
						COLOR_DIRTY_DOT)

		# --- v0.27.0: S/M/B buttons (between name and fader region) ---
		# Each button is a filled rect with a single-letter label
		# centered inside. Active state uses a saturated color (yellow
		# for solo, red for mute, purple for bypass); inactive uses the
		# dark fader-track grey for visual cohesion with the rest of
		# the strip. Outline always visible at low contrast so the
		# button is identifiable even when inactive.
		_draw_smb_button(_solo_rect(),   "S", _is_soloed,
				COLOR_YELLOW, f, fs_small)
		_draw_smb_button(_mute_rect(),   "M", _is_muted,
				COLOR_RED, f, fs_small)
		_draw_smb_button(_bypass_rect(), "B", _is_bypassed,
				COLOR_BUTTON_BYPASS_ACTIVE, f, fs_small)

		# --- Meter (left column) ---
		var meter_x: float = 6.0
		var meter_rect := Rect2(meter_x, fader_region_y,
				METER_W, fader_region_h)
		draw_rect(meter_rect, COLOR_BG, true)
		draw_rect(meter_rect, COLOR_BG_OUTLINE, false, 1.0)

		# Meter fill from bottom up, colored by current peak zone.
		var peak_y: float = _peak_linear_to_meter_y(
				_peak_smoothed, fader_region_y, fader_region_h)
		var fill_h: float = (fader_region_y + fader_region_h) - peak_y
		if fill_h > 0.0:
			var color := COLOR_GREEN
			var peak_db: float = -INF
			if _peak_smoothed > 0.0:
				peak_db = 20.0 * (log(_peak_smoothed) / log(10.0))
			if peak_db >= DB_YELLOW_MAX:
				color = COLOR_RED
			elif peak_db >= DB_GREEN_MAX:
				color = COLOR_YELLOW
			draw_rect(Rect2(meter_x, peak_y, METER_W, fill_h), color, true)

		# Peak-hold marker.
		if _peak_held > 0.0001:
			var held_y: float = _peak_linear_to_meter_y(
					_peak_held, fader_region_y, fader_region_h)
			draw_line(Vector2(meter_x, held_y),
					Vector2(meter_x + METER_W, held_y),
					COLOR_TICK, 1.5)

		# --- Fader track (middle column) ---
		var fader_x: float = meter_x + METER_W + 14.0
		var fader_track_x: float = fader_x + (FADER_HANDLE_W - FADER_TRACK_W) * 0.5
		var fader_track_rect := Rect2(
				fader_track_x, fader_region_y,
				FADER_TRACK_W, fader_region_h)
		draw_rect(fader_track_rect, COLOR_FADER_TRACK, true)

		# Tick marks at the scale dB values, across the FULL strip
		# width so they read as horizontal "zone" lines.
		for db_v in SCALE_MARKS_DB:
			var y_t: float = _db_to_y(db_v, fader_region_y, fader_region_h)
			draw_line(Vector2(fader_x - 2.0, y_t),
					Vector2(fader_x + FADER_HANDLE_W + 2.0, y_t),
					COLOR_TICK, 1.0)

		# Fader handle, positioned at current dB.
		var handle_y: float = _db_to_y(_fader_db,
				fader_region_y, fader_region_h)
		var handle_rect := Rect2(
				fader_x, handle_y - FADER_HANDLE_H * 0.5,
				FADER_HANDLE_W, FADER_HANDLE_H)
		var handle_color := COLOR_FADER_HANDLE_ACTIVE \
				if _fader_dragging \
				else COLOR_FADER_HANDLE
		draw_rect(handle_rect, handle_color, true)
		draw_rect(handle_rect, COLOR_BG_OUTLINE, false, 1.0)

		# --- Scale labels (right column) ---
		if f != null:
			var label_x: float = fader_x + FADER_HANDLE_W + 4.0
			for db_v in SCALE_MARKS_DB:
				var y_l: float = _db_to_y(
						db_v, fader_region_y, fader_region_h)
				var label_text: String = ("%+d" % int(db_v)) if db_v != 0.0 \
						else "0"
				draw_string(f,
						Vector2(label_x, y_l + fs_small * 0.4),
						label_text,
						HORIZONTAL_ALIGNMENT_LEFT,
						-1, fs_small,
						COLOR_TEXT_DIM)

		# --- dB readout (just above FX_BAND) ---
		# v0.26.5: skip drawing the text while the LineEdit overlay
		# is visible (otherwise the static text would render behind
		# the LineEdit and produce a doubled appearance).
		# v0.28.3: y shifted by FX_BAND so the readout sits above the
		# Fx button row.
		if f != null and (_db_editor == null or not _db_editor.visible):
			var db_text: String = "%+.1f dB" % _fader_db
			var readout_size := f.get_string_size(
					db_text, HORIZONTAL_ALIGNMENT_CENTER, -1, fs)
			draw_string(f,
					Vector2((w - readout_size.x) * 0.5,
					h - FX_BAND - 4),
					db_text,
					HORIZONTAL_ALIGNMENT_CENTER,
					-1, fs,
					COLOR_TEXT)

		# --- v0.28.3: Fx toggle button (bottom band) ---
		# Drawn only when the bus actually has effects. The button
		# is the user's entry point into the effect editor: clicking
		# expands a side-panel in this strip's column showing one
		# section per effect with sliders per parameter.
		#
		# UX rationale:
		# - Visible only when relevant (no orphan clicks)
		# - Label "Fx (N)" shows count so the user knows what to
		#   expect before clicking
		# - Active color when expanded so the relationship between
		#   button state and panel visibility is obvious
		if _effect_count > 0 and f != null:
			_draw_fx_button(_fx_button_rect(), _effect_count,
					_is_fx_expanded, f, fs_small)

	# ---- Input handling for fader drag ----

	func _gui_input(event: InputEvent) -> void:
		var w: float = size.x
		var h: float = size.y
		# v0.27.0: fader region accounts for BUTTON_BAND between the
		# name and the fader. Must match the _draw math exactly.
		# v0.28.3: FX_BAND also subtracted (eats from the bottom).
		var fader_region_y: float = NAME_BAND + BUTTON_BAND + 4.0
		var fader_region_h: float = h - NAME_BAND - BUTTON_BAND - READOUT_BAND - FX_BAND - 8.0
		var fader_x: float = 6.0 + METER_W + 14.0
		var fader_full_rect := Rect2(
				fader_x, fader_region_y,
				FADER_HANDLE_W, fader_region_h)
		# v0.26.5: readout band is now READOUT_BAND pixels just above
		# the FX_BAND (was bottom-anchored pre-v0.28.3). Click here
		# activates the LineEdit overlay for type-to-set dB entry.
		# Checked BEFORE the fader rect because the regions don't
		# overlap but the explicit ordering documents intent.
		var readout_rect := Rect2(
				0.0, h - FX_BAND - READOUT_BAND,
				w, READOUT_BAND)
		# v0.28.3: Fx button rect (bottom FX_BAND of the strip). Only
		# active when the bus actually has effects — _effect_count==0
		# hides the button entirely so there's no orphan click.
		var fx_rect := _fx_button_rect()

		if event is InputEventMouseButton:
			var mb := event as InputEventMouseButton
			if mb.button_index == MOUSE_BUTTON_LEFT:
				# v0.27.0: S/M/B buttons. Checked first because they
				# sit at the top of the strip, above the fader and
				# readout regions. Click toggles local state and emits
				# the matching signal; outer dock forwards to runtime.
				if mb.pressed and _solo_rect().has_point(mb.position):
					_is_soloed = not _is_soloed
					queue_redraw()
					solo_changed.emit(bus_name, _is_soloed)
					accept_event()
					return
				if mb.pressed and _mute_rect().has_point(mb.position):
					_is_muted = not _is_muted
					queue_redraw()
					mute_changed.emit(bus_name, _is_muted)
					accept_event()
					return
				if mb.pressed and _bypass_rect().has_point(mb.position):
					_is_bypassed = not _is_bypassed
					queue_redraw()
					bypass_changed.emit(bus_name, _is_bypassed)
					accept_event()
					return
				# v0.28.3: Fx toggle. Only honored when the bus actually
				# has effects — _effect_count==0 means the button isn't
				# drawn and clicking that region falls through to the
				# fader (which won't be hit anyway since FX_BAND is
				# below fader_region_h). One panel open at a time is
				# enforced at the outer-dock level via fx_toggled.
				if mb.pressed and _effect_count > 0 and fx_rect.has_point(mb.position):
					set_fx_expanded(not _is_fx_expanded, true)
					accept_event()
					return
				# Readout click → start editing (v0.26.5).
				if mb.pressed and readout_rect.has_point(mb.position):
					_start_db_edit()
					accept_event()
					return
				if mb.pressed and fader_full_rect.has_point(mb.position):
					_fader_dragging = true
					_update_fader_from_y(
							mb.position.y, fader_region_y, fader_region_h)
					queue_redraw()
					accept_event()
				elif not mb.pressed and _fader_dragging:
					_fader_dragging = false
					queue_redraw()
		elif event is InputEventMouseMotion and _fader_dragging:
			var mm := event as InputEventMouseMotion
			_update_fader_from_y(
					mm.position.y, fader_region_y, fader_region_h)
			queue_redraw()
			accept_event()

	func _update_fader_from_y(y: float, top: float, height: float) -> void:
		var new_db: float = _y_to_db(y, top, height)
		# Snap to 0.1 dB increments so dragging produces sensible values.
		new_db = snappedf(new_db, 0.1)
		if absf(new_db - _fader_db) < 0.001:
			return  # no change, don't spam debugger
		_fader_db = new_db
		db_changed.emit(bus_name, _fader_db)

	# v0.27.0: render one S/M/B button. Active state uses the supplied
	# `active_color` as fill; inactive state uses the dark fader-track
	# grey. A 1px outline is drawn either way so the button shape is
	# always identifiable. The label is a single capital letter drawn
	# centered both horizontally and vertically (using font ascent for
	# the vertical center — Godot's draw_string takes a baseline, not
	# a top — see lessons_learned.md §"draw_string baseline vs top").
	func _draw_smb_button(rect: Rect2, letter: String, active: bool,
			active_color: Color, font: Font, font_size: int) -> void:
		var fill_color: Color = active_color if active else COLOR_BUTTON_INACTIVE
		draw_rect(rect, fill_color, true)
		draw_rect(rect, COLOR_BUTTON_OUTLINE, false, 1.0)
		if font == null:
			return
		var label_size := font.get_string_size(
				letter, HORIZONTAL_ALIGNMENT_CENTER, -1, font_size)
		# Vertical center: rect mid + (ascent - descent)/2 puts the
		# baseline so the glyph visually sits in the middle. The
		# approximation ascent ≈ font_size * 0.75 is good enough for
		# the small button labels; pixel-perfect centering would need
		# Font.get_ascent which is similar but font-dependent.
		var ascent: float = float(font_size) * 0.75
		var text_x: float = rect.position.x + (rect.size.x - label_size.x) * 0.5
		var text_y: float = rect.position.y + (rect.size.y + ascent) * 0.5
		draw_string(font,
				Vector2(text_x, text_y),
				letter,
				HORIZONTAL_ALIGNMENT_CENTER,
				-1, font_size,
				COLOR_BUTTON_LABEL)

	# v0.28.3: render the Fx toggle button. Distinct from S/M/B in
	# two ways:
	# - Label is multi-character: "Fx (N)" where N is _effect_count.
	#   The count is part of the label (not a badge) because at 84px
	#   wide there's plenty of room and "Fx (3)" is more scannable
	#   than a numeric badge in a corner.
	# - Active fill uses COLOR_FX_BUTTON_ACTIVE (blue), distinct from
	#   the S/M/B active colors (yellow/red/purple). Blue was chosen
	#   to avoid collision with the other button colors while still
	#   reading as an action-ready state. Logic Pro / Ableton both
	#   use blue for their effect/insert UI elements — familiar.
	#
	# The button uses the same outline style as _draw_smb_button so
	# the strip's bottom edge reads as a continuation of the S/M/B
	# row's visual language.
	func _draw_fx_button(rect: Rect2, count: int, active: bool,
			font: Font, font_size: int) -> void:
		var fill_color: Color = COLOR_FX_BUTTON_ACTIVE if active else COLOR_BUTTON_INACTIVE
		draw_rect(rect, fill_color, true)
		draw_rect(rect, COLOR_BUTTON_OUTLINE, false, 1.0)
		if font == null:
			return
		var label: String = "Fx (%d)" % count
		var label_size := font.get_string_size(
				label, HORIZONTAL_ALIGNMENT_CENTER, -1, font_size)
		var ascent: float = float(font_size) * 0.75
		var text_x: float = rect.position.x + (rect.size.x - label_size.x) * 0.5
		var text_y: float = rect.position.y + (rect.size.y + ascent) * 0.5
		draw_string(font,
				Vector2(text_x, text_y),
				label,
				HORIZONTAL_ALIGNMENT_CENTER,
				-1, font_size,
				COLOR_BUTTON_LABEL)


# ===================================================================
# v0.28.3 Phase 3.3c-2: inline effects panel
# ===================================================================
#
# Displayed below a strip when its Fx button is toggled on. Built
# from the latest stats poll's effects payload — see runtime_singleton
# _emit_bus_stats_to_debugger.
#
# Layout: VBoxContainer of effect sections, top-to-bottom = audio
# signal flow (first effect at top is the first to process samples).
# Each section is a header label (kind name, e.g. "Compressor") plus
# one row per parameter:
#
#     [ Label  ] [ Slider ──●──────────── ] [ Value+unit ]
#
# Data flow:
# - Outer dock receives stats. On Fx-toggle-open, looks up effects
#   for that bus from latest stats and calls build_from_effects.
# - User drags slider → param_changed signal → outer dock forwards
#   via _debugger_plugin.send_set_effect_parameter.
# - While panel is open, slider is local truth. The 30 Hz stats
#   poll does NOT overwrite slider values — otherwise dragging
#   would fight the refresh. Same convention as S/M/B which also
#   don't auto-sync from poll (set once at open, then locally
#   driven). Recovery: close and reopen to re-sync from engine.
#
# Per-param metadata (label, range, curve, units, format) lives in
# PARAM_META below, keyed by paramId (which matches
# audio::EffectParameter:: in bus.h — keep these tables in sync if
# the engine adds parameters).
class _EffectsPanel extends PanelContainer:

	# Visual constants for the panel itself
	const COLOR_HEADER_BG := Color(0.20, 0.22, 0.26)
	const COLOR_HEADER_TEXT := Color(0.92, 0.92, 0.92)
	const COLOR_PANEL_BG := Color(0.13, 0.14, 0.16)
	const PANEL_WIDTH: float = 240.0
	const LABEL_WIDTH: float = 70.0
	const VALUE_WIDTH: float = 64.0
	const ROW_MIN_H: float = 22.0

	# Per-param display metadata, keyed by paramId.
	# Fields:
	#   label  - visible param name (≤ ~12 chars to fit LABEL_WIDTH)
	#   unit   - unit suffix appended to the value display
	#   min    - minimum allowed value (engine units)
	#   max    - maximum allowed value
	#   curve  - "linear", "log", or "discrete"
	#   fmt    - printf-style format string for the value display
	#   choices - (discrete only) Array of String labels for OptionButton
	#   scale   - (optional) display-only multiplier (e.g. 100 for %)
	#
	# Range choices: matched to the conservative bounds enforced in
	# audio::Compressor::SetParameter / audio::BiquadFilter::SetParameter
	# / etc. so the engine clamp never silently moves the slider value.
	# Log curves are used for parameters that span multiple orders of
	# magnitude where users think logarithmically: Hz frequencies, ms
	# times. Linear is used for everything else.
	const PARAM_META: Dictionary = {
		# Gain
		1:  {"label": "Gain", "unit": "dB", "min": -60.0, "max": 12.0,
			"curve": "linear", "fmt": "%+0.1f"},
		# BiquadFilter
		2:  {"label": "Cutoff", "unit": "Hz", "min": 20.0, "max": 20000.0,
			"curve": "log", "fmt": "%0.0f"},
		3:  {"label": "Q", "unit": "", "min": 0.1, "max": 10.0,
			"curve": "log", "fmt": "%0.2f"},
		12: {"label": "Filter Gain", "unit": "dB", "min": -24.0, "max": 24.0,
			"curve": "linear", "fmt": "%+0.1f"},
		# Compressor
		4:  {"label": "Threshold", "unit": "dB", "min": -60.0, "max": 0.0,
			"curve": "linear", "fmt": "%0.1f"},
		5:  {"label": "Ratio", "unit": ":1", "min": 1.0, "max": 20.0,
			"curve": "linear", "fmt": "%0.1f"},
		6:  {"label": "Attack", "unit": "ms", "min": 0.1, "max": 100.0,
			"curve": "log", "fmt": "%0.1f"},
		7:  {"label": "Release", "unit": "ms", "min": 10.0, "max": 2000.0,
			"curve": "log", "fmt": "%0.0f"},
		8:  {"label": "Makeup", "unit": "dB", "min": 0.0, "max": 24.0,
			"curve": "linear", "fmt": "%+0.1f"},
		13: {"label": "Knee", "unit": "dB", "min": 0.0, "max": 18.0,
			"curve": "linear", "fmt": "%0.1f"},
		14: {"label": "Mix", "unit": "%", "min": 0.0, "max": 1.0,
			"curve": "linear", "fmt": "%0.0f", "scale": 100.0},
		15: {"label": "Max GR", "unit": "dB", "min": 0.0, "max": 60.0,
			"curve": "linear", "fmt": "%0.1f"},
		16: {"label": "SC HPF", "unit": "Hz", "min": 20.0, "max": 1000.0,
			"curve": "log", "fmt": "%0.0f"},
		17: {"label": "Hold", "unit": "ms", "min": 0.0, "max": 1000.0,
			"curve": "linear", "fmt": "%0.0f"},
		# DetectionMode is binary Peak/RMS — OptionButton, not slider.
		18: {"label": "Mode", "unit": "", "min": 0.0, "max": 1.0,
			"curve": "discrete", "choices": ["Peak", "RMS"]},
		# Reverb
		9:  {"label": "Size", "unit": "", "min": 0.0, "max": 1.0,
			"curve": "linear", "fmt": "%0.2f"},
		10: {"label": "Damping", "unit": "", "min": 0.0, "max": 1.0,
			"curve": "linear", "fmt": "%0.2f"},
		11: {"label": "Wet", "unit": "dB", "min": -60.0, "max": 6.0,
			"curve": "linear", "fmt": "%+0.1f"},
		# Saturation. Engine takes linear factors here, NOT dB —
		# verified against SaturationEffect::OnParameter
		# (saturation_effect.cpp). Drive is the pre-tanh gain factor;
		# OutputGain is the post-effect linear scale; both default to
		# 1.0 = unity. Unit "x" is shown so engineers don't confuse
		# these with dB controls elsewhere in the panel.
		19: {"label": "Drive", "unit": "x", "min": 1.0, "max": 10.0,
			"curve": "log", "fmt": "%0.1f"},
		20: {"label": "Mix", "unit": "%", "min": 0.0, "max": 1.0,
			"curve": "linear", "fmt": "%0.0f", "scale": 100.0},
		21: {"label": "Output", "unit": "x", "min": 0.0, "max": 2.0,
			"curve": "linear", "fmt": "%0.2f"},
		22: {"label": "Bias", "unit": "", "min": -1.0, "max": 1.0,
			"curve": "linear", "fmt": "%+0.2f"},
	}

	# Per-kind display order (top-to-bottom within an effect section).
	# Ordering is ergonomic — most-touched parameters at the top —
	# rather than internal field order in the engine. The Mix
	# parameter (where present) is always the LAST row in its
	# section, so the user can scan to the bottom of any effect for
	# the dry/wet balance without learning where each effect parks
	# it. Compressor.MixRatio (14), Saturation.Mix (20), and
	# Reverb.WetGainDb (11, labeled "Wet" — closest engine control
	# to a wet/dry blend for reverb) all live in the trailing slot.
	# Keys are audio::EffectKind values:
	#   1 = Gain, 2 = BiquadFilter, 3 = Compressor,
	#   4 = Reverb, 5 = Saturation
	const PARAM_ORDER_BY_KIND: Dictionary = {
		1: [1],
		2: [2, 3, 12],
		3: [4, 5, 6, 7, 8, 13, 15, 16, 17, 18, 14],
		4: [9, 10, 11],
		5: [19, 21, 22, 20],
	}

	# Emitted on slider/option change. Outer dock forwards via
	# _debugger_plugin.send_set_effect_parameter.
	signal param_changed(bus_name: String, effect_index: int,
			param_id: int, value: float)

	var bus_name: String = ""

	# Map (effect_idx:param_id) String → HSlider or OptionButton, so
	# we can update controls if needed (currently unused since panel
	# is local-truth, but cheap to maintain and useful for future
	# "refresh from poll" if we ever want that).
	var _control_map: Dictionary = {}
	# Map (effect_idx:param_id) String → value display Label, so
	# slider drags update the displayed value live.
	var _value_label_map: Dictionary = {}
	var _vbox: VBoxContainer = null

	func _init() -> void:
		custom_minimum_size = Vector2(PANEL_WIDTH, 0)
		var sb := StyleBoxFlat.new()
		sb.bg_color = COLOR_PANEL_BG
		sb.content_margin_left = 4.0
		sb.content_margin_right = 4.0
		sb.content_margin_top = 4.0
		sb.content_margin_bottom = 4.0
		add_theme_stylebox_override("panel", sb)
		_vbox = VBoxContainer.new()
		_vbox.add_theme_constant_override("separation", 6)
		add_child(_vbox)

	# Build the panel from a list of effect dicts as returned by
	# get_bus_effects: [{kind: int, kind_name: String, params: {paramId: value, ...}}, ...].
	# Clears any previous content first.
	func build_from_effects(effects: Array) -> void:
		_control_map.clear()
		_value_label_map.clear()
		for child in _vbox.get_children():
			child.queue_free()
		for i in range(effects.size()):
			var e: Dictionary = effects[i]
			var kind: int = int(e.get("kind", 0))
			var kind_name: String = String(e.get("kind_name", "Effect"))
			var params: Dictionary = e.get("params", {})
			_vbox.add_child(_build_effect_section(i, kind, kind_name, params))

	func _build_effect_section(effect_idx: int, kind: int,
			kind_name: String, params: Dictionary) -> Control:
		var section := VBoxContainer.new()
		section.add_theme_constant_override("separation", 2)

		# Header band: distinct background so the section boundary is
		# visually clear when multiple effects are stacked.
		var header := PanelContainer.new()
		var header_sb := StyleBoxFlat.new()
		header_sb.bg_color = COLOR_HEADER_BG
		header_sb.content_margin_left = 6.0
		header_sb.content_margin_right = 6.0
		header_sb.content_margin_top = 2.0
		header_sb.content_margin_bottom = 2.0
		header.add_theme_stylebox_override("panel", header_sb)
		var header_label := Label.new()
		header_label.text = kind_name
		header_label.add_theme_color_override("font_color", COLOR_HEADER_TEXT)
		header.add_child(header_label)
		section.add_child(header)

		# Param rows in the configured order
		var order_v: Variant = PARAM_ORDER_BY_KIND.get(kind, [])
		var order: Array = order_v if order_v is Array else []
		for param_id in order:
			if not PARAM_META.has(param_id):
				continue
			# Engine sends params dict with int keys, but the
			# debugger transport sometimes stringifies. Check both.
			var current_value: float = 0.0
			if params.has(param_id):
				current_value = float(params[param_id])
			elif params.has(str(param_id)):
				current_value = float(params[str(param_id)])
			section.add_child(_build_param_row(
					effect_idx, int(param_id), current_value))

		return section

	func _build_param_row(effect_idx: int, param_id: int,
			current_value: float) -> Control:
		var meta: Dictionary = PARAM_META[param_id]
		var row := HBoxContainer.new()
		row.add_theme_constant_override("separation", 4)
		row.custom_minimum_size = Vector2(0, ROW_MIN_H)

		# Param name label (left)
		var label := Label.new()
		label.text = String(meta["label"])
		label.custom_minimum_size = Vector2(LABEL_WIDTH, 0)
		label.clip_text = true
		row.add_child(label)

		# Control (middle, expand-fill): HSlider or OptionButton
		var curve: String = String(meta["curve"])
		var key: String = _key(effect_idx, param_id)

		if curve == "discrete":
			var opt := OptionButton.new()
			var choices_v: Variant = meta.get("choices", [])
			var choices: Array = choices_v if choices_v is Array else []
			for c in choices:
				opt.add_item(String(c))
			opt.selected = clampi(int(current_value), 0, choices.size() - 1)
			opt.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			opt.item_selected.connect(
					_on_option_selected.bind(effect_idx, param_id))
			row.add_child(opt)
			# No value label for discrete (the OptionButton text IS the value).
			# Spacer reserves VALUE_WIDTH so rows align across kinds.
			var spacer := Control.new()
			spacer.custom_minimum_size = Vector2(VALUE_WIDTH, 0)
			row.add_child(spacer)
			_control_map[key] = opt
		else:
			var slider := HSlider.new()
			slider.min_value = 0.0
			slider.max_value = 1.0
			slider.step = 0.001
			slider.value = _real_to_slider(current_value, meta)
			slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			slider.custom_minimum_size = Vector2(0, 18)
			slider.value_changed.connect(
					_on_slider_changed.bind(effect_idx, param_id))
			# v0.28.3: right-click → reset to the value at panel-open
			# time. Cheap "recover from mistakes" affordance. Note
			# we reset to current_value (the engine's value when the
			# panel was opened), NOT to the engine's compile-time
			# default. This is intentional: the user opened the
			# panel to tweak from a known-good state, so right-click
			# returns to *that* state rather than wiping any prior
			# session work. Future v0.28.4 may add Shift+right-click
			# for a true compile-time default reset if needed.
			slider.gui_input.connect(_on_slider_gui_input.bind(
					slider, current_value, meta))
			row.add_child(slider)
			# Value display (right)
			var value_label := Label.new()
			value_label.text = _format_value(current_value, meta)
			value_label.custom_minimum_size = Vector2(VALUE_WIDTH, 0)
			value_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
			value_label.clip_text = true
			row.add_child(value_label)
			_control_map[key] = slider
			_value_label_map[key] = value_label

		return row

	# HSlider.value_changed(value) → bind(effect_idx, param_id)
	# means the call is _on_slider_changed(value, effect_idx, param_id).
	# The bind args come AFTER the signal args (Godot 4 convention).
	func _on_slider_changed(t: float, effect_idx: int, param_id: int) -> void:
		var meta: Dictionary = PARAM_META[param_id]
		var real_value: float = _slider_to_real(t, meta)
		var k: String = _key(effect_idx, param_id)
		if _value_label_map.has(k):
			var lbl: Label = _value_label_map[k] as Label
			if lbl != null:
				lbl.text = _format_value(real_value, meta)
		param_changed.emit(bus_name, effect_idx, param_id, real_value)

	func _on_option_selected(selected_idx: int, effect_idx: int,
			param_id: int) -> void:
		param_changed.emit(bus_name, effect_idx, param_id, float(selected_idx))

	# v0.28.3: right-click handler for the slider. Resets to the
	# value the slider had when the panel was opened. Left-click
	# drag goes through HSlider's built-in handler → value_changed
	# → _on_slider_changed, untouched by this. Other input events
	# (middle button, scroll wheel) fall through to HSlider too.
	#
	# bind args (slider, initial_value, meta) come AFTER the
	# signal's event arg per Godot 4 convention.
	func _on_slider_gui_input(event: InputEvent, slider: HSlider,
			initial_value: float, meta: Dictionary) -> void:
		if event is InputEventMouseButton:
			var mb := event as InputEventMouseButton
			if mb.pressed and mb.button_index == MOUSE_BUTTON_RIGHT:
				# Assigning to slider.value triggers value_changed,
				# which formats the value label and emits param_changed
				# back through the normal path — engine gets the reset
				# value via the same channel as any drag.
				slider.value = _real_to_slider(initial_value, meta)

	# ---- value <-> slider position conversion ----

	# Map slider position 0..1 to engine value, per curve.
	# Log curve: value = min * (max/min)^t  (geometric)
	#   Requires min > 0; param table guarantees that for "log" entries.
	# Linear curve: value = min + t*(max-min)
	func _slider_to_real(t: float, meta: Dictionary) -> float:
		var min_v: float = float(meta["min"])
		var max_v: float = float(meta["max"])
		var curve: String = String(meta["curve"])
		if curve == "log":
			return min_v * pow(max_v / min_v, t)
		return min_v + t * (max_v - min_v)

	# Inverse: real engine value → slider position 0..1.
	# Clamps to range first so out-of-range values from the engine
	# don't produce NaN/negative slider positions.
	func _real_to_slider(value: float, meta: Dictionary) -> float:
		var min_v: float = float(meta["min"])
		var max_v: float = float(meta["max"])
		var curve: String = String(meta["curve"])
		var clamped: float = clampf(value, min_v, max_v)
		if curve == "log":
			if clamped <= 0.0 or min_v <= 0.0:
				return 0.0
			return log(clamped / min_v) / log(max_v / min_v)
		if max_v == min_v:
			return 0.0
		return (clamped - min_v) / (max_v - min_v)

	# Format value for display with unit suffix.
	# meta["scale"] (optional) multiplies the displayed value, used
	# for mix params stored 0..1 in the engine but shown as 0..100%.
	func _format_value(value: float, meta: Dictionary) -> String:
		var display_value: float = value
		if meta.has("scale"):
			display_value = value * float(meta["scale"])
		var fmt: String = String(meta["fmt"])
		var unit: String = String(meta["unit"])
		if unit == "":
			return fmt % display_value
		return (fmt % display_value) + " " + unit

	# Stable composite key for the control/label maps.
	func _key(effect_idx: int, param_id: int) -> String:
		return str(effect_idx) + ":" + str(param_id)
