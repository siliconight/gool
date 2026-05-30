# Copyright 2026 Brannen Graves
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
# implied. See the License for the specific language governing permissions
# and limitations under the License.

# addons/gool/editor/material_eq_curve_view.gd
#
# v0.59.2 — Phase 6.E.1 (read-only visualizer for per-material EQ).
#
# A custom-drawn Control that renders the cumulative magnitude
# response of a gool material EQ curve as a 2D plot:
#
#   X axis: log frequency, 20 Hz .. 20 kHz
#   Y axis: gain in dB, ±12 dB (matches the design-doc bounds)
#
# Inputs are a curve Dictionary in the shape returned by
# `Gool.get_material_eq_for_material(material_int)` plus an
# intensity scalar that uniformly multiplies the three band gains
# (the same scaling the runtime applies via `set_eq_intensity()`).
#
# Math: three biquads in series (low shelf + peaking + high shelf).
# Magnitude response is the product of each biquad's |H(jω)| at the
# frequency under test. The biquad coefficients are computed using
# the audio-EQ cookbook (RBJ) formulas — same family the engine's
# Biquad effect uses, so what you see in the plot is what you hear
# at runtime up to the engine's own numerical precision.
#
# Performance: the plot evaluates the response at the widget's
# pixel-width number of frequency points each redraw. At 600 px
# wide, that's 600 frequency evaluations per band per redraw =
# 1800 total cos/exp calls. Trivial; the widget only redraws when
# a property changes (queue_redraw on setters), not per-frame.
#
# This is editor-only code (@tool) and never runs during a packaged
# game. Pure visualization — no audio routing, no autoload
# dependencies beyond the curve Dictionary the inspector passes in.

@tool
extends Control

# ---- Plot config -------------------------------------------------

# Frequency axis: spans 20 Hz to 20 kHz on a log scale (one decade
# is one decade wide, matching every EQ visualizer ever shipped).
const _FREQ_MIN_HZ := 20.0
const _FREQ_MAX_HZ := 20000.0

# Gain axis: ±12 dB matches the engine's per-material curve
# physical-range bound (see tests/unit/material_eq_curve_test.cpp).
# A small headroom above that lets the intensity slider push to
# 2.0× without truncating display (12 × 2 = 24 dB peak possible).
const _DB_MIN := -24.0
const _DB_MAX := 24.0

# Plot padding inside the widget (px). Leaves room for the dB-axis
# labels on the left and the frequency-axis labels along the bottom.
const _PAD_LEFT   := 36
const _PAD_RIGHT  := 8
const _PAD_TOP    := 12
const _PAD_BOTTOM := 22

# Sample rate assumed for biquad coefficient calculation. The
# engine runs at the host audio device's rate (commonly 48 kHz);
# 48 kHz is what we display against. The display sample rate
# differs from runtime SR by at most a few %, well below the
# perceptual resolution of the plot.
const _DISPLAY_SR := 48000.0

# Grid lines on the dB axis. Drawn lighter than the 0 dB reference.
const _DB_GRID := [-18.0, -12.0, -6.0, 0.0, 6.0, 12.0, 18.0]

# Grid lines on the frequency axis. ISO standard octave anchors
# 31.25 / 62.5 / 125 / 250 / 500 / 1k / 2k / 4k / 8k / 16k — using
# the conventional rounded labels.
const _FREQ_GRID := [31.25, 62.5, 125.0, 250.0, 500.0, 1000.0,
					 2000.0, 4000.0, 8000.0, 16000.0]

# ---- Inputs ------------------------------------------------------

# Curve Dictionary as returned by Gool.get_material_eq_for_material.
# Expected keys: low_gain_db, low_freq_hz, mid_gain_db, mid_freq_hz,
# mid_q, high_gain_db, high_freq_hz, is_neutral. Missing keys
# default to neutral values.
var curve: Dictionary = {} :
	set(value):
		curve = value
		queue_redraw()

# Intensity multiplier applied to the three gain values, matching
# the runtime's _apply_scaled_material_eq_to_bus behavior (gains
# scale; freqs and Q stay put). Default 1.0 = curve as-tabled.
var intensity: float = 1.0 :
	set(value):
		intensity = clampf(value, 0.0, 2.0)
		queue_redraw()

# Optional label drawn in the top-left of the plot — typically the
# material's friendly name (e.g. "Concrete"). Empty string hides it.
var material_label: String = "" :
	set(value):
		material_label = value
		queue_redraw()

# v0.60.0: when true, the band dots become draggable handles —
# horizontal drag moves the band's center frequency, vertical drag
# moves the gain. Right-click on the mid band opens a Q popup.
# When false (default, matches v0.59.x behavior), the dots are
# read-only visual cues and the widget ignores mouse input.
#
# Editable mode is engaged by the inspector when the inspected
# GoolAudioMaterial resource has override_enabled=true. Other
# callers (a future mixer-dock variant showing a bus's effective
# EQ) leave it false.
var editable: bool = false :
	set(value):
		editable = value
		# Drag tracking is cleared on mode change so a partial drag
		# can't leak across editable toggles.
		_dragging_band = -1
		queue_redraw()

## v0.60.0: emitted while the user drags a band handle. Payload:
##   band_index : 0 = low shelf, 1 = peak, 2 = high shelf
##   freq_hz    : new center frequency
##   gain_db    : new gain in dB
##   q          : new Q (mid band only; low/high shelves pass 1.0)
## Inspector wires this to update the GoolAudioMaterial's per-band
## fields live. Emitted on every mouse-motion delta during drag so
## the inspector can save the resource and the curve plot refreshes
## from the resource on the next paint.
signal band_changed(band_index: int, freq_hz: float,
		gain_db: float, q: float)

## v0.60.0: emitted once when a drag operation finishes (mouse
## button released). Lets the inspector commit the change to
## disk via ResourceSaver — we don't save every mouse-motion
## frame, just the final value.
signal band_drag_ended(band_index: int)

# Drag state. _dragging_band is -1 when no drag is active; 0/1/2
# when the user is dragging the low/mid/high band's dot.
var _dragging_band: int = -1
# Hit radius (px) around each dot's center for clicks/drags.
# Generous enough that the user doesn't need pixel-perfect aim.
const _HIT_RADIUS: float = 10.0

# ---- Widget setup ------------------------------------------------

func _init() -> void:
	# Reasonable default height. Width follows the parent container.
	custom_minimum_size = Vector2(420, 200)


func _ready() -> void:
	# Editor uses theme colors so the plot doesn't fight whatever
	# theme the user has installed. We grab them in _draw rather
	# than caching here because the theme can change live in the
	# editor (light/dark toggle).
	queue_redraw()


# ---- Painting ----------------------------------------------------

func _draw() -> void:
	var rect := Rect2(_PAD_LEFT, _PAD_TOP,
			size.x - _PAD_LEFT - _PAD_RIGHT,
			size.y - _PAD_TOP - _PAD_BOTTOM)
	if rect.size.x <= 1 or rect.size.y <= 1:
		return  # widget collapsed; nothing useful to draw

	# Theme-aware colors with sensible fallbacks for non-editor
	# contexts (running the widget in a sandbox scene, etc).
	var bg_color      := _theme_color("dark_color_1",  Color(0.12, 0.12, 0.13))
	var grid_color    := _theme_color("dark_color_3",  Color(0.20, 0.20, 0.22))
	var zero_color    := _theme_color("contrast_color_1",
									   Color(0.40, 0.40, 0.45))
	var curve_color   := _theme_color("accent_color",  Color(0.40, 0.62, 1.00))
	var label_color   := _theme_color("font_color",    Color(0.85, 0.85, 0.85))
	var muted_color   := Color(label_color.r, label_color.g, label_color.b, 0.6)

	# Plot panel background
	draw_rect(rect, bg_color, true)

	# dB grid lines + labels
	var font := _ui_font()
	var font_size := 10
	for db in _DB_GRID:
		var y := rect.position.y + _db_to_y(db, rect.size.y)
		var line_color := zero_color if absf(db) < 0.01 else grid_color
		draw_line(Vector2(rect.position.x, y),
				  Vector2(rect.end.x, y),
				  line_color, 1.0)
		if font != null:
			var label_str := "%+d dB" % int(db) if absf(db) > 0.01 else " 0 dB"
			draw_string(font,
					Vector2(2, y + font_size / 3.0),
					label_str, HORIZONTAL_ALIGNMENT_LEFT,
					_PAD_LEFT - 4, font_size, muted_color)

	# Frequency grid lines + labels along the bottom
	for hz in _FREQ_GRID:
		var x := rect.position.x + _hz_to_x(hz, rect.size.x)
		draw_line(Vector2(x, rect.position.y),
				  Vector2(x, rect.end.y),
				  grid_color, 1.0)
		if font != null:
			var label_str: String
			if hz >= 1000.0:
				label_str = "%dk" % int(hz / 1000.0)
			else:
				label_str = "%d" % int(hz)
			draw_string(font,
					Vector2(x - 10, rect.end.y + font_size + 4),
					label_str, HORIZONTAL_ALIGNMENT_LEFT,
					24, font_size, muted_color)

	# Curve itself — magnitude response at every pixel column. Three
	# biquads multiplied (in linear magnitude) then converted to dB.
	var points := _compute_response_points(rect)
	if points.size() > 1:
		# Filled area under the curve, semi-transparent — gives the
		# visualization more body without overpowering the line.
		var fill_color := Color(curve_color.r, curve_color.g, curve_color.b, 0.18)
		var poly: PackedVector2Array = points.duplicate()
		poly.append(Vector2(rect.end.x, rect.position.y + _db_to_y(0.0, rect.size.y)))
		poly.append(Vector2(rect.position.x, rect.position.y + _db_to_y(0.0, rect.size.y)))
		draw_colored_polygon(poly, fill_color)

		# The curve line itself.
		draw_polyline(points, curve_color, 1.5, true)

	# Band markers: small dots at each band's center frequency on
	# the zero-dB line, sized by the band's gain magnitude. Cheap
	# visual cue for "which band is which" without legend clutter.
	_draw_band_markers(rect, curve_color, label_color)

	# Optional top-left label.
	if material_label != "" and font != null:
		var label_str := material_label
		if not is_zero_approx(intensity - 1.0):
			label_str += "  (intensity %.2f)" % intensity
		draw_string(font,
				Vector2(rect.position.x + 6, rect.position.y + font_size + 4),
				label_str, HORIZONTAL_ALIGNMENT_LEFT,
				rect.size.x - 12, font_size + 1, label_color)


# ---- Frequency-response math -------------------------------------

# Compute the polyline points for the cumulative magnitude
# response across the plot area. One sample per pixel column, so
# the polyline is automatically as smooth as the widget's display
# resolution allows.
func _compute_response_points(rect: Rect2) -> PackedVector2Array:
	if curve.is_empty():
		# No curve provided — show a flat zero-dB line.
		var p := PackedVector2Array()
		var y_zero := rect.position.y + _db_to_y(0.0, rect.size.y)
		p.append(Vector2(rect.position.x, y_zero))
		p.append(Vector2(rect.end.x,      y_zero))
		return p

	var low_freq:  float = float(curve.get("low_freq_hz",  200.0))
	var low_gain:  float = float(curve.get("low_gain_db",  0.0)) * intensity
	var mid_freq:  float = float(curve.get("mid_freq_hz",  1000.0))
	var mid_gain:  float = float(curve.get("mid_gain_db",  0.0)) * intensity
	var mid_q:     float = float(curve.get("mid_q",        1.0))
	var high_freq: float = float(curve.get("high_freq_hz", 8000.0))
	var high_gain: float = float(curve.get("high_gain_db", 0.0)) * intensity

	var pts := PackedVector2Array()
	var n_columns: int = int(rect.size.x)
	for col in n_columns:
		var x := rect.position.x + float(col)
		var hz := _x_to_hz(float(col), rect.size.x)
		var mag_db := _band_response_db(hz, "lowshelf",
				low_freq, low_gain, 1.0)
		mag_db += _band_response_db(hz, "peak",
				mid_freq, mid_gain, mid_q)
		mag_db += _band_response_db(hz, "highshelf",
				high_freq, high_gain, 1.0)
		var y := rect.position.y + _db_to_y(mag_db, rect.size.y)
		# Clamp Y to widget bounds so off-scale curves don't draw
		# outside the plot rectangle.
		y = clampf(y, rect.position.y, rect.end.y)
		pts.append(Vector2(x, y))
	return pts


# Magnitude response of one biquad in dB at frequency `hz`. Uses
# the RBJ cookbook closed-form magnitude expression so we don't
# have to instantiate and run a filter — faster and matches what
# the engine's biquad will produce for the same coefficients.
func _band_response_db(hz: float, kind: String, f0: float,
						gain_db: float, q: float) -> float:
	# Early-out: a 0 dB band is identity (regardless of f0/Q), so
	# contributes 0 dB to the cumulative response. Saves the
	# transcendental math for the common case where most bands
	# of most curves are near-zero.
	if absf(gain_db) < 0.01:
		return 0.0

	var sr: float = _DISPLAY_SR
	var a: float = pow(10.0, gain_db / 40.0)         # sqrt(A) in RBJ notation
	var w0: float = TAU * f0 / sr
	var cos_w0: float = cos(w0)
	var sin_w0: float = sin(w0)
	var alpha: float = sin_w0 / (2.0 * q)

	# Cookbook coefficients depend on filter kind.
	var b0: float; var b1: float; var b2: float
	var a0: float; var a1: float; var a2: float
	match kind:
		"lowshelf":
			var two_sqrt_a_alpha: float = 2.0 * sqrt(a) * alpha
			b0 = a * ((a + 1.0) - (a - 1.0) * cos_w0 + two_sqrt_a_alpha)
			b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cos_w0)
			b2 = a * ((a + 1.0) - (a - 1.0) * cos_w0 - two_sqrt_a_alpha)
			a0 = (a + 1.0) + (a - 1.0) * cos_w0 + two_sqrt_a_alpha
			a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cos_w0)
			a2 = (a + 1.0) + (a - 1.0) * cos_w0 - two_sqrt_a_alpha
		"highshelf":
			var two_sqrt_a_alpha2: float = 2.0 * sqrt(a) * alpha
			b0 = a * ((a + 1.0) + (a - 1.0) * cos_w0 + two_sqrt_a_alpha2)
			b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cos_w0)
			b2 = a * ((a + 1.0) + (a - 1.0) * cos_w0 - two_sqrt_a_alpha2)
			a0 = (a + 1.0) - (a - 1.0) * cos_w0 + two_sqrt_a_alpha2
			a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cos_w0)
			a2 = (a + 1.0) - (a - 1.0) * cos_w0 - two_sqrt_a_alpha2
		"peak", _:
			b0 = 1.0 + alpha * a
			b1 = -2.0 * cos_w0
			b2 = 1.0 - alpha * a
			a0 = 1.0 + alpha / a
			a1 = -2.0 * cos_w0
			a2 = 1.0 - alpha / a

	# Magnitude at the evaluation frequency hz. The transfer
	# function H(z) evaluated at z = e^(jω) with ω = 2π·hz/sr:
	#   |H(e^jω)| = sqrt(num² / den²)
	# Computed in numerator/denominator real-vs-imag form to avoid
	# complex-number bookkeeping.
	var w: float = TAU * hz / sr
	var cos_w: float = cos(w)
	var cos_2w: float = cos(2.0 * w)
	var sin_w: float = sin(w)
	var sin_2w: float = sin(2.0 * w)

	var num_re: float = b0 + b1 * cos_w + b2 * cos_2w
	var num_im: float = -(b1 * sin_w + b2 * sin_2w)
	var den_re: float = a0 + a1 * cos_w + a2 * cos_2w
	var den_im: float = -(a1 * sin_w + a2 * sin_2w)

	var num_mag2: float = num_re * num_re + num_im * num_im
	var den_mag2: float = den_re * den_re + den_im * den_im

	if den_mag2 <= 0.0 or num_mag2 <= 0.0:
		return 0.0
	# 10 * log10(magnitude²) = 20 * log10(magnitude); skip the
	# outer sqrt by using the ratio of squared magnitudes.
	return 10.0 * log(num_mag2 / den_mag2) / log(10.0)


# Draw a marker at each band's (freq_hz, gain_db) position. The
# marker sits exactly where you'd intuitively want to drag it to —
# horizontal axis = center frequency, vertical axis = gain. In
# editable mode (v0.60.0), the markers ARE the drag handles; in
# read-only mode (v0.59.x default), they're informational only.
#
# Visual conventions:
#   - read-only: small filled dot, curve color
#   - editable:  larger dot with a white outline so it reads as
#                "this is a draggable handle"
#   - dragging:  same as editable but thicker outline
func _draw_band_markers(rect: Rect2, curve_color: Color,
						 label_color: Color) -> void:
	if curve.is_empty():
		return
	var positions := _band_positions_in_widget(rect)
	for i in 3:
		var pos: Vector2 = positions[i]
		if editable:
			# Drag handle. Larger and outlined so the user can see
			# it's an interactive element.
			var is_dragging: bool = (_dragging_band == i)
			var radius: float = 6.0 if is_dragging else 5.0
			var outline_w: float = 2.5 if is_dragging else 1.5
			draw_circle(pos, radius, curve_color)
			# Outline drawn as a slightly-larger ring on top of the
			# filled dot. Using a separate draw_arc for compatibility
			# (draw_circle has no "stroke" option in Godot 4.x).
			draw_arc(pos, radius + outline_w * 0.5,
					0.0, TAU, 24,
					Color(1.0, 1.0, 1.0, 0.9), outline_w, true)
		else:
			# Read-only: small filled dot at the band's position.
			# Encodes both location AND magnitude (radius scales
			# subtly with gain magnitude) without claiming
			# interactivity.
			var band_gain := _band_gain_intensity_scaled(i)
			var radius: float = clampf(
					2.5 + absf(band_gain) * 0.25, 2.5, 5.0)
			draw_circle(pos, radius, curve_color)

# Compute screen-space (widget-local) positions of all three band
# markers. Position is (x_for_freq, y_for_gain). Shared between
# the drawing path and the hit-testing path so they never disagree.
func _band_positions_in_widget(rect: Rect2) -> Array:
	var positions: Array = []
	positions.resize(3)
	# Band 0 — low shelf
	var low_x := rect.position.x + _hz_to_x(
			float(curve.get("low_freq_hz", 200.0)), rect.size.x)
	var low_y := rect.position.y + _db_to_y(
			_band_gain_intensity_scaled(0), rect.size.y)
	positions[0] = Vector2(low_x, low_y)
	# Band 1 — mid peak
	var mid_x := rect.position.x + _hz_to_x(
			float(curve.get("mid_freq_hz", 1000.0)), rect.size.x)
	var mid_y := rect.position.y + _db_to_y(
			_band_gain_intensity_scaled(1), rect.size.y)
	positions[1] = Vector2(mid_x, mid_y)
	# Band 2 — high shelf
	var high_x := rect.position.x + _hz_to_x(
			float(curve.get("high_freq_hz", 8000.0)), rect.size.x)
	var high_y := rect.position.y + _db_to_y(
			_band_gain_intensity_scaled(2), rect.size.y)
	positions[2] = Vector2(high_x, high_y)
	return positions

# Helper: gain of the given band (0/1/2), scaled by intensity.
# Centralizes the gain-readback so the drawing, the hit-test, and
# the drag-update all see the same value.
func _band_gain_intensity_scaled(band_index: int) -> float:
	match band_index:
		0: return float(curve.get("low_gain_db",  0.0)) * intensity
		1: return float(curve.get("mid_gain_db",  0.0)) * intensity
		2: return float(curve.get("high_gain_db", 0.0)) * intensity
	return 0.0


# v0.60.0: handle mouse input for drag-handle interaction. Only
# active when editable=true; otherwise the widget is purely
# informational and we pass through to the default handler (which
# does nothing for Control).
func _gui_input(event: InputEvent) -> void:
	if not editable:
		return
	if event is InputEventMouseButton:
		_handle_mouse_button(event as InputEventMouseButton)
	elif event is InputEventMouseMotion and _dragging_band >= 0:
		_handle_mouse_motion(event as InputEventMouseMotion)

func _handle_mouse_button(event: InputEventMouseButton) -> void:
	# Left-click only. Right-click + middle-click left for future
	# affordances (Q popup on right-click is a v0.60.1 polish task).
	if event.button_index != MOUSE_BUTTON_LEFT:
		return
	var rect := Rect2(_PAD_LEFT, _PAD_TOP,
			size.x - _PAD_LEFT - _PAD_RIGHT,
			size.y - _PAD_TOP - _PAD_BOTTOM)
	if event.pressed:
		# Begin drag. Hit-test against the three band positions;
		# pick the closest match within _HIT_RADIUS. If multiple
		# bands overlap (rare — happens when two bands are tuned
		# close together), the closest wins.
		var positions := _band_positions_in_widget(rect)
		var closest_band: int = -1
		var closest_d: float = _HIT_RADIUS
		for i in 3:
			var d: float = event.position.distance_to(positions[i])
			if d < closest_d:
				closest_d = d
				closest_band = i
		if closest_band >= 0:
			_dragging_band = closest_band
			accept_event()
			queue_redraw()
	else:
		# End drag. Emit the drag-ended signal so the inspector
		# can persist the resource to disk.
		if _dragging_band >= 0:
			var ended_band := _dragging_band
			_dragging_band = -1
			band_drag_ended.emit(ended_band)
			accept_event()
			queue_redraw()

func _handle_mouse_motion(event: InputEventMouseMotion) -> void:
	# Active drag — translate mouse position to (freq, gain), emit
	# band_changed. The inspector picks up the signal, updates the
	# resource's @export fields, refreshes our `curve` dict, and
	# the next paint draws the updated marker position.
	var rect := Rect2(_PAD_LEFT, _PAD_TOP,
			size.x - _PAD_LEFT - _PAD_RIGHT,
			size.y - _PAD_TOP - _PAD_BOTTOM)
	# Clamp mouse position into the plot rect so the user can't
	# drag a handle into the axis-label margins.
	var px: float = clampf(event.position.x, rect.position.x, rect.end.x)
	var py: float = clampf(event.position.y, rect.position.y, rect.end.y)
	# Translate back to (freq_hz, gain_db).
	var new_freq: float = _x_to_hz(px - rect.position.x, rect.size.x)
	var new_gain: float = _y_to_db(py - rect.position.y, rect.size.y)
	# Undo the intensity scaling when reporting back — the resource
	# stores the un-scaled gain, intensity is layered on at apply
	# time. Otherwise dragging at intensity 2.0 and saving would
	# write a doubled gain into the resource.
	if absf(intensity) > 0.001:
		new_gain = new_gain / intensity
	# Q passes through unchanged (drag adjusts freq + gain only;
	# Q is per-band metadata, edited via the inspector's separate
	# Q slider for the mid band).
	var current_q: float = float(curve.get("mid_q", 0.7))
	band_changed.emit(_dragging_band, new_freq, new_gain, current_q)
	accept_event()
	# Note: no queue_redraw() here — the inspector emits
	# `band_changed` → updates resource → updates our `curve` dict
	# → the curve setter calls queue_redraw(). The handle motion
	# is tracked through that loop, not a direct redraw, so we
	# never have a one-frame stale state.


# ---- Coordinate mapping helpers ----------------------------------

# Convert a frequency in Hz to a pixel X within a plot rectangle
# of the given width. Log mapping; the returned X is relative to
# the rectangle's origin (caller adds rect.position.x).
func _hz_to_x(hz: float, w: float) -> float:
	hz = clampf(hz, _FREQ_MIN_HZ, _FREQ_MAX_HZ)
	var lo: float = log(_FREQ_MIN_HZ)
	var hi: float = log(_FREQ_MAX_HZ)
	var t: float = (log(hz) - lo) / (hi - lo)
	return t * w


# Inverse of _hz_to_x — convert a pixel column index to its Hz
# value under the same log mapping.
func _x_to_hz(x: float, w: float) -> float:
	var t: float = clampf(x / w, 0.0, 1.0)
	var lo: float = log(_FREQ_MIN_HZ)
	var hi: float = log(_FREQ_MAX_HZ)
	return exp(lo + t * (hi - lo))


# Convert a dB value to a pixel Y within a plot rectangle of the
# given height. Larger dB = smaller Y (top-of-screen is positive,
# standard Godot screen orientation). Returned Y is relative to
# the rectangle's origin.
func _db_to_y(db: float, h: float) -> float:
	db = clampf(db, _DB_MIN, _DB_MAX)
	var t: float = (_DB_MAX - db) / (_DB_MAX - _DB_MIN)
	return t * h

# v0.60.0: inverse of _db_to_y. Convert a pixel Y back to dB.
# Used by the drag-handle mouse-motion path to translate cursor
# position into a band gain value.
func _y_to_db(y: float, h: float) -> float:
	var t: float = clampf(y / h, 0.0, 1.0)
	return _DB_MAX - t * (_DB_MAX - _DB_MIN)


# ---- Theme helpers -----------------------------------------------

# Look up an editor theme color by name; fall back to a hardcoded
# safe value when running outside the editor (e.g. in a sandbox
# scene where Engine.is_editor_hint() is false but the widget is
# embedded).
func _theme_color(name: String, fallback: Color) -> Color:
	var theme := EditorInterface.get_editor_theme() if (
			Engine.is_editor_hint() and EditorInterface) else null
	if theme == null:
		return fallback
	if theme.has_color(name, "Editor"):
		return theme.get_color(name, "Editor")
	return fallback


# Returns the editor's UI font when in editor context, or the
# global fallback when not. Used for axis labels.
func _ui_font() -> Font:
	if Engine.is_editor_hint() and EditorInterface:
		var theme := EditorInterface.get_editor_theme()
		if theme != null and theme.has_font("main", "EditorFonts"):
			return theme.get_font("main", "EditorFonts")
	return ThemeDB.fallback_font
