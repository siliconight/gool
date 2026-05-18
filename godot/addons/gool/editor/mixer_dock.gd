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
const STRIP_HEIGHT: float = 340.0
const STRIP_GAP: float = 4.0

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
var _last_bus_names: PackedStringArray = PackedStringArray()
var _accum: float = 0.0
var _debugger_plugin: EditorDebuggerPlugin = null


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

	# v0.26.0: build static layout from config.json IMMEDIATELY.
	# Strips are visible at editor time; live data comes later
	# when F5 starts (via debugger plugin).
	_load_static_layout_from_config()


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
	for s in _strips:
		s.queue_free()
	_strips.clear()
	for d in buses:
		var bus_name: String = String(d.get("name", "(unnamed)"))
		var initial_db: float = float(d.get("gain_db", 0.0))
		var strip := _BusStrip.new()
		strip.bus_name = bus_name
		strip.set_fader_db(initial_db, false)  # silent (no signal emit)
		strip.custom_minimum_size = Vector2(STRIP_WIDTH, STRIP_HEIGHT)
		strip.db_changed.connect(_on_strip_db_changed)
		_strip_container.add_child(strip)
		_strips.append(strip)


# ---- Live data polling (during F5) -----------------------------------

func _poll() -> void:
	if _debugger_plugin == null:
		# No debugger plugin wired yet (very early editor startup).
		# Keep showing the static config layout.
		return
	var stats: Array = _debugger_plugin.get_latest_bus_stats()
	if stats.is_empty():
		# Game not running. Reset all meters to floor; faders keep
		# whatever the user has set them to.
		for s in _strips:
			s.push_peak(0.0)
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
	for i in stats.size():
		if i >= _strips.size():
			break
		var d: Dictionary = stats[i]
		var peak: float = float(d.get("peak_linear", 0.0))
		_strips[i].push_peak(peak)


# Runtime topology differs from config — build strips from runtime
# stats. Fader values default to 0 dB since runtime stats don't
# currently include them (a future addition could).
func _rebuild_strips_from_runtime(stats: Array) -> void:
	_empty_label.visible = false
	for s in _strips:
		s.queue_free()
	_strips.clear()
	for d in stats:
		var strip := _BusStrip.new()
		strip.bus_name = String(d.get("name", "(unnamed)"))
		strip.set_fader_db(0.0, false)
		strip.custom_minimum_size = Vector2(STRIP_WIDTH, STRIP_HEIGHT)
		strip.db_changed.connect(_on_strip_db_changed)
		_strip_container.add_child(strip)
		_strips.append(strip)


# Strip fader was dragged. Forward to the running game via the
# debugger plugin's send helper. Silently dropped if no game
# running (correct behavior — fader still moves in the UI).
func _on_strip_db_changed(bus_name: String, db: float) -> void:
	if _debugger_plugin == null:
		return
	if _debugger_plugin.has_method("send_set_bus_gain"):
		_debugger_plugin.send_set_bus_gain(bus_name, db)


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

	var bus_name: String = ""
	var _peak_smoothed: float = 0.0
	var _peak_held: float = 0.0
	var _peak_held_age: float = 0.0
	var _fader_db: float = 0.0
	var _fader_dragging: bool = false

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

	func _ready() -> void:
		set_process(true)
		mouse_filter = Control.MOUSE_FILTER_STOP

	# Set the fader value programmatically. emit_signal=false used at
	# init time to avoid feedback before the parent has connected.
	func set_fader_db(db: float, emit: bool = true) -> void:
		_fader_db = clampf(db, FADER_MIN_DB, FADER_MAX_DB)
		queue_redraw()
		if emit:
			db_changed.emit(bus_name, _fader_db)

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

		var fader_region_y: float = NAME_BAND + 4.0
		var fader_region_h: float = h - NAME_BAND - READOUT_BAND - 8.0

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

		# --- dB readout (bottom band) ---
		if f != null:
			var db_text: String = "%+.1f dB" % _fader_db
			var readout_size := f.get_string_size(
					db_text, HORIZONTAL_ALIGNMENT_CENTER, -1, fs)
			draw_string(f,
					Vector2((w - readout_size.x) * 0.5,
					h - 4),
					db_text,
					HORIZONTAL_ALIGNMENT_CENTER,
					-1, fs,
					COLOR_TEXT)

	# ---- Input handling for fader drag ----

	func _gui_input(event: InputEvent) -> void:
		var w: float = size.x
		var h: float = size.y
		var fader_region_y: float = NAME_BAND + 4.0
		var fader_region_h: float = h - NAME_BAND - READOUT_BAND - 8.0
		var fader_x: float = 6.0 + METER_W + 14.0
		var fader_full_rect := Rect2(
				fader_x, fader_region_y,
				FADER_HANDLE_W, fader_region_h)

		if event is InputEventMouseButton:
			var mb := event as InputEventMouseButton
			if mb.button_index == MOUSE_BUTTON_LEFT:
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
