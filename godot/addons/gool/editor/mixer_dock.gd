@tool
class_name GoolMixerDock
extends Control

# MixerDock — v0.24.0 read-only
#
# Bottom-panel dock for Godot's editor that visualizes gool's bus
# graph with one strip per bus and live peak meters. Polls
# Gool.get_bus_stats() at 30 Hz, smooths the linear peak with an
# exponential decay envelope, and renders DAW-style segmented
# vertical meters with tick marks at 0 / -6 / -12 / -24 dB.
#
# Read-only in v0.24.0 (item 3.3a on the roadmap). v0.24.x adds:
#   - 3.3b: faders + S/M/B buttons
#   - 3.3c: effect chain editor
#   - 3.3d: bus topology editor + config.json round-trip
#
# Lifecycle: plugin.gd creates one instance via add_control_to_bottom_panel
# in _enter_tree and removes it via remove_control_from_bottom_panel in
# _exit_tree. The dock processes regardless of whether the user has
# the bottom panel open (queue_redraw is a no-op when hidden, so it's
# nearly free when collapsed).
#
# Why a single dock + custom strip widgets (vs. a scene with
# ColorRects per strip): the bus list is dynamic (depends on
# config.json), strips are added/removed when config changes, and
# the segmented meter look needs _draw() anyway. Building this as
# code-driven Controls is simpler than scene-driven once we have
# more than 2-3 buses.

# Polling cadence. 30 Hz gives ~33ms peak windows which match human
# meter perception (faster reads flicker, slower miss transients).
const POLL_HZ: float = 30.0

# Visual smoothing — exponential decay applied to the linear peak
# value displayed on the bar. Lower = snappier (more flicker);
# higher = lazier (smoother). 0.85 per poll is "Pro Tools snappy".
const PEAK_DECAY: float = 0.85

# Per-strip dimensions (px).
const STRIP_WIDTH: float = 64.0
const STRIP_HEIGHT: float = 220.0
const STRIP_GAP: float = 6.0

# Color thresholds (dBFS) — Pro Tools / Logic convention.
const DB_GREEN_MAX: float = -12.0
const DB_YELLOW_MAX: float = -6.0
# Above DB_YELLOW_MAX → red (clipping warning zone)

# Color palette for the segmented meter.
const COLOR_GREEN := Color(0.36, 0.83, 0.46)   # below -12dB
const COLOR_YELLOW := Color(0.95, 0.82, 0.32)  # -12 to -6dB
const COLOR_RED := Color(0.93, 0.40, 0.32)     # above -6dB
const COLOR_BG := Color(0.10, 0.10, 0.12)      # meter background
const COLOR_BG_OUTLINE := Color(0.20, 0.20, 0.22)
const COLOR_TEXT := Color(0.85, 0.88, 0.92)
const COLOR_TICK := Color(0.45, 0.45, 0.48)

# Meter range — minimum dBFS visible (below this all bars are empty).
const DB_FLOOR: float = -60.0
const DB_CEIL: float = 6.0  # slight headroom above 0 for visual overshoot

# State.
var _strip_container: HBoxContainer = null
var _empty_label: Label = null
var _strips: Array = []  # of BusStrip Controls
var _last_bus_names: PackedStringArray = PackedStringArray()
var _accum: float = 0.0
var _is_polling: bool = false


func _ready() -> void:
	custom_minimum_size = Vector2(0, STRIP_HEIGHT + 28)

	# Title row + spacer.
	var root_vbox := VBoxContainer.new()
	root_vbox.anchor_right = 1.0
	root_vbox.anchor_bottom = 1.0
	add_child(root_vbox)

	# The horizontal strip row sits inside a ScrollContainer in case
	# the user has many buses configured.
	var scroll := ScrollContainer.new()
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	root_vbox.add_child(scroll)

	_strip_container = HBoxContainer.new()
	_strip_container.add_theme_constant_override("separation", int(STRIP_GAP))
	scroll.add_child(_strip_container)

	# Empty-state label shown when gool isn't running yet.
	_empty_label = Label.new()
	_empty_label.text = "  Gool audio runtime not initialized.\n  Press Play to start metering."
	_empty_label.add_theme_color_override("font_color", Color(0.6, 0.6, 0.65))
	_strip_container.add_child(_empty_label)


func _process(delta: float) -> void:
	_accum += delta
	var poll_period := 1.0 / POLL_HZ
	if _accum >= poll_period:
		_accum = 0.0
		_poll()


# Poll Gool for current bus stats and refresh strip widgets.
# Tolerant of:
#   - Gool autoload not present (Tool scripts in the editor can't
#     guarantee autoloads exist when the plugin first loads)
#   - Gool present but runtime not initialized (returns [])
#   - Bus list changed since last poll (rebuild strips)
func _poll() -> void:
	# Defensive: in the editor, autoloads exist but Gool's _runtime
	# may not be created until the project actually plays. has_node
	# is the safest probe.
	var tree := get_tree()
	if tree == null:
		return
	var root := tree.root
	if root == null or not root.has_node("Gool"):
		_show_empty()
		return

	var gool: Node = root.get_node("Gool")
	if not gool.has_method("get_bus_stats"):
		_show_empty()
		return

	var stats: Array = gool.get_bus_stats()
	if stats.is_empty():
		_show_empty()
		return

	# Rebuild strips if the bus topology changed.
	var names := PackedStringArray()
	for d in stats:
		names.append(String(d.get("name", "")))
	if names != _last_bus_names:
		_rebuild_strips(stats)
		_last_bus_names = names

	# Push peaks into each strip.
	for i in stats.size():
		if i >= _strips.size():
			break
		var d: Dictionary = stats[i]
		var peak: float = float(d.get("peak_linear", 0.0))
		_strips[i].push_peak(peak)


func _show_empty() -> void:
	if _strips.size() == 0 and _empty_label.visible:
		return
	for s in _strips:
		s.queue_free()
	_strips.clear()
	_last_bus_names = PackedStringArray()
	_empty_label.visible = true


func _rebuild_strips(stats: Array) -> void:
	_empty_label.visible = false
	for s in _strips:
		s.queue_free()
	_strips.clear()
	for d in stats:
		var strip := _BusStrip.new()
		strip.bus_name = String(d.get("name", "(unnamed)"))
		strip.custom_minimum_size = Vector2(STRIP_WIDTH, STRIP_HEIGHT)
		_strip_container.add_child(strip)
		_strips.append(strip)


# Per-bus strip widget. Inner class so it ships with the dock as a
# single file and doesn't pollute global class_name space.
class _BusStrip extends Control:
	var bus_name: String = ""
	var _peak_smoothed: float = 0.0   # linear, decayed
	var _peak_held: float = 0.0       # linear, peak-hold for tick line
	var _peak_held_age: float = 0.0   # seconds since hold value was set

	const PEAK_HOLD_TIME: float = 1.5   # seconds before held peak drops
	const PEAK_DROP_RATE: float = 0.5   # linear units per second after hold

	func _ready() -> void:
		set_process(true)

	func push_peak(peak_linear: float) -> void:
		# Exponential decay toward the new value (downward) or
		# instant attack on rises. Classic VU + peak-hold pattern.
		if peak_linear > _peak_smoothed:
			_peak_smoothed = peak_linear
		else:
			_peak_smoothed = _peak_smoothed * GoolMixerDock.PEAK_DECAY \
					+ peak_linear * (1.0 - GoolMixerDock.PEAK_DECAY)
		# Peak-hold marker: lifts to new max, sits for hold time,
		# then drops slowly.
		if peak_linear >= _peak_held:
			_peak_held = peak_linear
			_peak_held_age = 0.0
		queue_redraw()

	func _process(delta: float) -> void:
		_peak_held_age += delta
		if _peak_held_age > PEAK_HOLD_TIME:
			_peak_held = maxf(0.0, _peak_held - PEAK_DROP_RATE * delta)
			queue_redraw()

	# Convert linear peak (0..∞) to dBFS in the displayable range
	# [DB_FLOOR..DB_CEIL]. Returns 0..1 mapped position for drawing.
	func _linear_to_meter_y(peak_linear: float) -> float:
		if peak_linear <= 0.0:
			return 0.0
		var db: float = 20.0 * (log(peak_linear) / log(10.0))
		var t: float = (db - GoolMixerDock.DB_FLOOR) \
				/ (GoolMixerDock.DB_CEIL - GoolMixerDock.DB_FLOOR)
		return clampf(t, 0.0, 1.0)

	func _draw() -> void:
		var rect_size := size
		var meter_w: float = rect_size.x * 0.6
		var meter_x: float = (rect_size.x - meter_w) * 0.5
		var name_height: float = 18.0
		var db_height: float = 18.0
		var meter_y: float = name_height + 4.0
		var meter_h: float = rect_size.y - name_height - db_height - 8.0

		# Bus name label (drawn here so the inner class is self-contained).
		var f := get_theme_default_font()
		var fs := get_theme_default_font_size()
		if f != null:
			var name_size := f.get_string_size(bus_name, HORIZONTAL_ALIGNMENT_CENTER, -1, fs)
			draw_string(f,
					Vector2((rect_size.x - name_size.x) * 0.5, name_height - 4),
					bus_name,
					HORIZONTAL_ALIGNMENT_CENTER,
					-1, fs,
					GoolMixerDock.COLOR_TEXT)

		# Meter background + outline.
		var meter_rect := Rect2(meter_x, meter_y, meter_w, meter_h)
		draw_rect(meter_rect, GoolMixerDock.COLOR_BG, true)
		draw_rect(meter_rect, GoolMixerDock.COLOR_BG_OUTLINE, false, 1.0)

		# Tick lines at 0 / -6 / -12 / -24 dB.
		var ticks := [0.0, -6.0, -12.0, -24.0]
		for db_tick in ticks:
			var t: float = (db_tick - GoolMixerDock.DB_FLOOR) \
					/ (GoolMixerDock.DB_CEIL - GoolMixerDock.DB_FLOOR)
			t = clampf(t, 0.0, 1.0)
			var y_tick: float = meter_y + meter_h * (1.0 - t)
			draw_line(Vector2(meter_x, y_tick),
					Vector2(meter_x + meter_w, y_tick),
					GoolMixerDock.COLOR_TICK, 1.0)

		# Fill the meter from bottom up in segmented green/yellow/red.
		var t_peak: float = _linear_to_meter_y(_peak_smoothed)
		var fill_h: float = meter_h * t_peak
		_draw_segmented_fill(meter_x, meter_y + meter_h - fill_h,
				meter_w, fill_h, t_peak)

		# Peak-hold marker (thin line at last-held max).
		var t_held: float = _linear_to_meter_y(_peak_held)
		if t_held > 0.001:
			var y_held: float = meter_y + meter_h * (1.0 - t_held)
			var hold_color := GoolMixerDock.COLOR_RED \
					if t_held >= _db_to_meter_t(GoolMixerDock.DB_YELLOW_MAX) \
					else (GoolMixerDock.COLOR_YELLOW \
					if t_held >= _db_to_meter_t(GoolMixerDock.DB_GREEN_MAX) \
					else GoolMixerDock.COLOR_GREEN)
			draw_line(Vector2(meter_x, y_held),
					Vector2(meter_x + meter_w, y_held),
					hold_color, 2.0)

		# Numeric dB readout under the meter.
		if f != null:
			var db_text: String
			if _peak_smoothed <= 0.0001:
				db_text = "-∞"
			else:
				var db: float = 20.0 * (log(_peak_smoothed) / log(10.0))
				db_text = "%.1f" % db
			var db_size := f.get_string_size(db_text, HORIZONTAL_ALIGNMENT_CENTER, -1, fs)
			draw_string(f,
					Vector2((rect_size.x - db_size.x) * 0.5,
					meter_y + meter_h + db_height - 4),
					db_text,
					HORIZONTAL_ALIGNMENT_CENTER,
					-1, fs,
					GoolMixerDock.COLOR_TEXT)

	func _draw_segmented_fill(x: float, y: float, w: float, h: float,
			t_peak: float) -> void:
		# Draw the fill as a single rectangle, but pick the color
		# based on the current peak level (top color wins). This is
		# the "what zone is the peak in" indicator, simpler than a
		# true segmented bar (which we may add in 3.3b).
		if h <= 0.0:
			return
		var color := GoolMixerDock.COLOR_GREEN
		if t_peak >= _db_to_meter_t(GoolMixerDock.DB_YELLOW_MAX):
			color = GoolMixerDock.COLOR_RED
		elif t_peak >= _db_to_meter_t(GoolMixerDock.DB_GREEN_MAX):
			color = GoolMixerDock.COLOR_YELLOW
		draw_rect(Rect2(x, y, w, h), color, true)

	func _db_to_meter_t(db: float) -> float:
		var t: float = (db - GoolMixerDock.DB_FLOOR) \
				/ (GoolMixerDock.DB_CEIL - GoolMixerDock.DB_FLOOR)
		return clampf(t, 0.0, 1.0)
