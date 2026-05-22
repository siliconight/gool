# addons/gool/prefabs/gool_debug_overlay.gd
#
# Drop-in debug HUD that displays gool's real-time runtime stats.
# Add a single GoolDebugOverlay node to your scene root and the
# overlay appears in-game with no further setup.
#
# What it shows:
#   - Engine info: version, audio device name, sample rate, buffer
#     size, bus count, config source
#   - Render health: callback rate per second, peak amplitude in
#     the last window, master bus pre-gain peak, active voice count,
#     master gain, exception count
#   - Cumulative: total frames rendered since engine init
#
# Polled every `update_interval_ms` milliseconds (default 250 = 4 Hz).
# The render stats are all lock-free atomic reads on the C++ side,
# so polling cost is negligible. The Label text replacement is the
# only non-trivial cost, and at 4 Hz it's invisible.
#
# Use cases:
#   - Verify gool is running healthy during local development
#   - Spot voice-budget exhaustion or peak-zero (silent mixer) in
#     production playtests
#   - Compare audio thread health across machines / sessions
#   - Capture screenshots of the overlay for bug reports
#
# Production note: ship your release without this node added, OR
# leave it added but set `visible_at_startup = false` and bind a
# secret keybinding. The overlay's CPU cost while hidden is zero
# (the polling timer pauses).

@tool
class_name GoolDebugOverlay
extends CanvasLayer

# How often to poll Gool for fresh stats and redraw the label.
# 250 ms (4 Hz) is plenty for a human reading the values; lower
# numbers update faster but redraw the label more often.
@export_range(50, 5000, 10, "suffix:ms")
var update_interval_ms: int = 250

# Whether the overlay is visible when the scene starts. Set to
# false in shipping builds; the user can toggle it on with the
# toggle_action below.
@export var visible_at_startup: bool = true

# Optional InputMap action name to toggle visibility at runtime.
# Leave empty to disable the toggle. If you bind it, the overlay
# only redraws when visible — hidden overlays have zero runtime
# cost.
@export var toggle_action: String = ""

# Direct keycode toggle (used if toggle_action is empty). Default
# F3 matches Minecraft / many engines. Set to KEY_NONE (0) to
# disable.
@export var toggle_key: Key = KEY_F3

# Where the overlay sits on screen. Useful for avoiding overlap
# with your own UI elements.
@export_enum("Top Left:0", "Top Right:1", "Bottom Left:2", "Bottom Right:3")
var anchor_corner: int = 0   # default: top-left

# Background opacity. 0 = transparent (text reads against world);
# 1 = fully opaque. Default 0.6 is readable over most game art.
@export_range(0.0, 1.0, 0.05)
var background_opacity: float = 0.6

# Text color. White on dark background is readable in most games;
# override per-project if you want to match your HUD style.
@export var text_color: Color = Color(0.95, 0.95, 0.95)

# When true, use the bundled Ubuntu Regular font (v0.59.1+) shipped
# under res://addons/gool/fonts/. When false, inherit the host
# project's theme font — useful when the project already standardizes
# on Roboto/Inter/etc. and you want the HUD to match. The property
# name is a v0.37.0 holdover; pre-v0.59.1 this loaded
# `ThemeDB.fallback_font` which advertised itself as monospace but was
# actually proportional Noto. Ubuntu Regular is proportional with
# tabular figures, which keeps numeric columns aligned.
@export var monospace: bool = true

# ─── private state ────────────────────────────────────────────

# UI children. Created in _ready, kept as references so we can
# update text and reposition without re-creating each frame.
var _label: Label = null
var _bg_panel: Panel = null
var _container: Control = null

# Polling timer (created in _ready). Pauses when overlay is hidden
# to keep cost at literal zero in hidden state.
var _timer: Timer = null

# Tracking for delta-per-second calculations. The atomic counters
# on the C++ side are monotonic, so we subtract last-tick value
# to get per-interval delta.
var _last_callback_count: int = 0
var _last_frame_count: int = 0
var _last_poll_time_ms: int = 0

# v0.54.1: timestamp of the first refresh that observed
# `not _runtime.is_initialized()`. Used to display elapsed wait
# time in the "starting up" message, and to escalate to a
# troubleshooting hint if init takes pathologically long. Reset
# to 0 once init completes so a future re-init (e.g. soft reset)
# gets a fresh count.
var _wait_started_at_ms: int = 0

# Cached runtime reference. Avoid re-resolving the autoload every
# tick.
var _runtime: Node = null

# ─── lifecycle ────────────────────────────────────────────────

func _ready() -> void:
	if Engine.is_editor_hint():
		# In the editor we don't have a running game; build the UI
		# so the user can preview position/styling but don't poll.
		_build_ui()
		return
	_runtime = get_node_or_null("/root/Gool")
	if _runtime == null:
		push_warning(
			"[GoolDebugOverlay] /root/Gool autoload not found. "
			+ "The overlay needs the gool plugin enabled to display "
			+ "stats. Enable the plugin in Project Settings → Plugins."
		)
		# Build the UI anyway so the user sees we tried; first
		# _refresh will just show "runtime not available."
	_build_ui()
	_container.visible = visible_at_startup
	_timer = Timer.new()
	_timer.wait_time = update_interval_ms / 1000.0
	_timer.one_shot = false
	_timer.autostart = true
	_timer.timeout.connect(_refresh)
	add_child(_timer)
	_last_poll_time_ms = Time.get_ticks_msec()
	_refresh()

func _process(_delta: float) -> void:
	if Engine.is_editor_hint():
		return
	# Toggle handling. Both action-based and direct-keycode are
	# supported; action takes precedence if set.
	var pressed := false
	if toggle_action != "" and InputMap.has_action(toggle_action):
		pressed = Input.is_action_just_pressed(toggle_action)
	elif toggle_key != KEY_NONE:
		pressed = Input.is_key_pressed(toggle_key) and not _was_key_held
		_was_key_held = Input.is_key_pressed(toggle_key)
	if pressed:
		_container.visible = not _container.visible
		# When showing, force a refresh so the overlay isn't blank
		# for up to 250ms. When hiding, pause the timer to save CPU.
		if _container.visible:
			_refresh()
			_timer.start()
		else:
			_timer.stop()

# Edge-detect helper for the direct-keycode path (Input.is_key_pressed
# returns true while held; we want one toggle per press).
var _was_key_held: bool = false

# ─── UI construction ──────────────────────────────────────────

func _build_ui() -> void:
	_container = Control.new()
	_container.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_container.set_anchors_preset(_anchor_preset())
	_container.offset_left = 8
	_container.offset_top = 8
	_container.offset_right = -8
	_container.offset_bottom = -8
	add_child(_container)

	_bg_panel = Panel.new()
	_bg_panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.05, 0.05, 0.05, background_opacity)
	sb.border_width_left = 1
	sb.border_width_top = 1
	sb.border_width_right = 1
	sb.border_width_bottom = 1
	sb.border_color = Color(0.2, 0.2, 0.2, background_opacity)
	sb.corner_radius_top_left = 4
	sb.corner_radius_top_right = 4
	sb.corner_radius_bottom_left = 4
	sb.corner_radius_bottom_right = 4
	sb.content_margin_left = 8
	sb.content_margin_top = 6
	sb.content_margin_right = 8
	sb.content_margin_bottom = 6
	_bg_panel.add_theme_stylebox_override("panel", sb)
	_bg_panel.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_container.add_child(_bg_panel)

	_label = Label.new()
	_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_label.add_theme_color_override("font_color", text_color)
	if monospace:
		# v0.59.1: load the bundled Ubuntu Regular font shipped under
		# res://addons/gool/fonts/. Pre-v0.59.1 this branch loaded
		# ThemeDB.fallback_font with a comment claiming it was the
		# monospace built-in — in practice that fallback is Noto, a
		# proportional font, so column alignment was already
		# approximate. The new Ubuntu Regular has tabular figures and
		# pins typography across hosts so HUD screenshots from
		# different machines and Godot versions look identical.
		#
		# `load()` + `ResourceLoader.exists()` rather than `preload()`
		# so the overlay degrades gracefully if someone stripped the
		# /fonts/ subdir to shrink their addon footprint — falls
		# through to the host theme's default font in that case
		# instead of hard-erroring at script parse time.
		#
		# Note on the `monospace` export name: it's a v0.37.0 holdover.
		# Ubuntu Regular is technically proportional (letters vary in
		# width) but its digits are tabular (fixed-width), which is
		# what matters for the numeric columns in the stats display.
		# When `monospace = false`, the overlay falls through to the
		# host theme's UI font — useful if your project has its own
		# Roboto/Inter/etc. and wants the HUD to match.
		const FONT_PATH := "res://addons/gool/fonts/Ubuntu-Regular.ttf"
		if ResourceLoader.exists(FONT_PATH):
			var font: Font = load(FONT_PATH)
			if font != null:
				_label.add_theme_font_override("font", font)
	_label.position = Vector2(8, 6)
	_label.text = "gool debug overlay\n(waiting for first refresh...)"
	_bg_panel.add_child(_label)
	# Resize the panel to fit the label after first text update.

# Maps the anchor_corner enum to Godot's Control anchor preset
# constants. Couldn't just store the constant directly in the
# export since Control.LayoutPreset isn't easily exportable as an
# enum-typed property in @tool scripts.
func _anchor_preset() -> int:
	match anchor_corner:
		0: return Control.PRESET_TOP_LEFT
		1: return Control.PRESET_TOP_RIGHT
		2: return Control.PRESET_BOTTOM_LEFT
		3: return Control.PRESET_BOTTOM_RIGHT
	return Control.PRESET_TOP_LEFT

# ─── stats polling and rendering ──────────────────────────────

func _refresh() -> void:
	if _label == null:
		return
	if Engine.is_editor_hint() or _runtime == null:
		_label.text = _build_offline_text()
		_resize_panel_to_label()
		return
	if not _runtime.is_initialized():
		# v0.54.1: redesigned "still warming up" message.
		# Old message ("gool — initializing... (autoload found, runtime
		# not yet ready)") was engineer-speak that read like "the
		# system is half-broken." In reality, native init normally
		# takes 50-200ms and the user sees this for at most one poll
		# cycle (~250ms).
		#
		# New message:
		#  - Plain-English status line ("starting up")
		#  - Animated dots so the overlay visibly shows polling is
		#    alive — distinguishes "still waiting, normal" from
		#    "dock froze, abnormal" without any wait-bar visuals
		#  - Elapsed counter once we're past the normal-init window
		#    (>= 0.5s) so the user has a number to gauge against
		#  - Troubleshooting escalation past 3s — by then init has
		#    almost certainly failed and the user wants to know
		#    where to look
		if _wait_started_at_ms == 0:
			_wait_started_at_ms = Time.get_ticks_msec()
		var elapsed_ms: int = Time.get_ticks_msec() - _wait_started_at_ms
		var elapsed_s: float = elapsed_ms / 1000.0
		# 1 → 2 → 3 → 1 dots, cycling at ~3 Hz (one dot per 333 ms)
		var dot_count: int = 1 + int((elapsed_ms / 333) % 3)
		var dots: String = ".".repeat(dot_count)
		var msg: String = "gool: starting up%s" % dots
		if elapsed_s >= 0.5:
			msg += "\n%.1fs" % elapsed_s
		if elapsed_s >= 3.0:
			msg += " — taking longer than expected"
			msg += "\nCheck the Output panel for errors."
		_label.text = msg
		_resize_panel_to_label()
		return

	# Reset the wait timer now that init has completed. A future
	# re-init (soft reset, device change, scene reload) gets a
	# clean elapsed count rather than continuing from the original
	# startup time.
	_wait_started_at_ms = 0

	var stats: Dictionary = _runtime.get_render_stats()
	var now_ms: int = Time.get_ticks_msec()
	var interval_s: float = max(0.001, (now_ms - _last_poll_time_ms) / 1000.0)
	_last_poll_time_ms = now_ms

	var cb_total: int = stats.get("callback_invocations", 0)
	var frames_total: int = stats.get("frames_rendered", 0)
	var cb_rate: float = (cb_total - _last_callback_count) / interval_s
	var frame_rate: float = (frames_total - _last_frame_count) / interval_s
	_last_callback_count = cb_total
	_last_frame_count = frames_total

	var peak: float = stats.get("peak_amplitude", 0.0)
	var mixer_peak: float = stats.get("mixer_peak", 0.0)
	var voices: int = stats.get("active_voices", 0)
	var master_gain: float = stats.get("master_gain", 1.0)
	var exc: int = stats.get("exception_count", 0)

	# Reset peak so the next interval reads samples-since-now.
	_runtime.reset_render_peak()

	var version: Dictionary = _runtime.get_version()
	var version_str: String = version.get("full", "?")
	var device: String = _runtime.get_backend_description()
	if device == "":
		device = "(unknown)"

	var lines: PackedStringArray = PackedStringArray()
	lines.append("gool %s" % version_str)
	lines.append("device: %s" % _truncate(device, 40))
	lines.append("──────────────────────────")
	lines.append("cb_rate:     %6.1f /s" % cb_rate)
	lines.append("frame_rate:  %6.0f /s" % frame_rate)
	lines.append("peak:        %6.4f" % peak)
	lines.append("mixer_peak:  %6.4f" % mixer_peak)
	lines.append("voices:      %6d  gain: %.2f" % [voices, master_gain])
	if exc > 0:
		lines.append("⚠ exceptions: %d (audio frames dropped)" % exc)
	lines.append("──────────────────────────")
	lines.append("frames total: %s" % _fmt_int(frames_total))

	_label.text = "\n".join(lines)
	_resize_panel_to_label()

# Editor / no-runtime fallback text. Lets the user preview the
# overlay's positioning and styling without a running game.
func _build_offline_text() -> String:
	if Engine.is_editor_hint():
		return (
			"gool debug overlay (preview)\n"
			+ "device: (live in-game)\n"
			+ "──────────────────────────\n"
			+ "cb_rate:      93.8 /s\n"
			+ "peak:        0.4521\n"
			+ "voices:           1  gain: 1.00\n"
			+ "──────────────────────────\n"
			+ "(live data shown when game is running)"
		)
	return (
		"gool debug overlay\n"
		+ "/root/Gool autoload not found.\n"
		+ "Enable the gool plugin in Project Settings."
	)

# Resize the background panel to wrap the label exactly. Called
# after every label text change since the dimensions may change
# (longer device names, more lines, etc).
func _resize_panel_to_label() -> void:
	if _label == null or _bg_panel == null:
		return
	var label_size: Vector2 = _label.get_minimum_size()
	var pad := Vector2(16, 12)  # matches stylebox content margins
	_bg_panel.size = label_size + pad

# Compact int formatting with thousands separators. 1234567 → "1,234,567".
# Pure GDScript so it works in @tool context.
func _fmt_int(n: int) -> String:
	var s: String = str(n)
	var out: String = ""
	var count: int = 0
	var i: int = s.length() - 1
	while i >= 0:
		if count == 3:
			out = "," + out
			count = 0
		out = s[i] + out
		count += 1
		i -= 1
	return out

# Truncate to fit when device names get long (e.g. "WASAPI / High
# Definition Audio Device (Intel SST Audio Controller)" exceeds
# the overlay's width).
func _truncate(s: String, max_len: int) -> String:
	if s.length() <= max_len:
		return s
	return s.substr(0, max_len - 1) + "…"
