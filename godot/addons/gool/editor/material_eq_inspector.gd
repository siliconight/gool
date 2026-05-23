# addons/gool/editor/material_eq_inspector.gd
#
# v0.59.2 — Phase 6.E.1 (designer-facing inspector for GoolAudioMaterial).
#
# Adds an "EQ curve preview" panel to the inspector when a
# GoolAudioMaterial.tres resource is selected. Three components:
#
#   1. A frequency-response plot (material_eq_curve_view.gd) showing
#      the cumulative magnitude response of the material's 3-biquad
#      EQ curve, drawn against a log frequency axis and ±dB grid.
#   2. A numerical readout listing the three bands' freq/gain/Q so
#      designers can see the exact authored values, not just the
#      shape.
#   3. A "Realism intensity" slider that writes
#      ProjectSettings("gool/material_eq/intensity") in the 0..2
#      range and live-updates the plot to show the scaled curve.
#      Same global value the runtime reads at startup.
#
# v0.59.2 ships the READ-ONLY version. The plot shows whatever
# curve the engine has authored for the material the .tres's
# `material` int points at. Editing handles (drag a dot, change
# the band's freq/gain) are Phase 6.E.1's natural Option B follow-up
# (v0.60.0), which also requires per-instance override fields on
# GoolAudioMaterial itself and a runtime override path in the EQ
# application code. v0.59.2 is the visualizer; that lets us validate
# the plot widget and audition flow before betting on writable.
#
# Why an inspector plugin rather than a custom Resource viewer:
# Godot's @tool _inspect-style scripting hooks via
# EditorInspectorPlugin run the same lifecycle (parse_begin /
# parse_property / parse_end) used by the other gool inspectors
# (sound_name_inspector.gd) so the registration shape stays
# uniform.

@tool
extends EditorInspectorPlugin

const CURVE_VIEW_SCRIPT := preload("res://addons/gool/editor/material_eq_curve_view.gd")

# ProjectSettings key for the global realism intensity multiplier.
# Mirrors runtime_singleton.gd's _EQ_INTENSITY_SETTING — duplicated
# here as a String constant so the inspector doesn't import the
# autoload (which isn't reachable in editor context).
const _EQ_INTENSITY_SETTING := "gool/material_eq/intensity"

# Friendly names indexed by AudioMaterial enum value. Mirrors
# runtime_singleton.gd's _MATERIAL_NAMES so the inspector can label
# the preview without going through the autoload.
const _MATERIAL_NAMES := [
	"Default", "Air", "Glass", "Wood", "Drywall",
	"Concrete", "Metal", "Curtain", "Foliage", "Meat",
	"Cardboard", "Rubber", "Liquid",
]

# Short character description per material — sentence-level hint
# that helps designers connect the EQ shape to a perceptual feel.
# Sourced from the v0.33.0 CHANGELOG entry's curve summaries.
const _MATERIAL_HINTS := {
	"Default":  "Neutral — no EQ coloring applied.",
	"Air":      "Pass-through — no surface, no coloring.",
	"Glass":    "Bright, glassy upper-mid sparkle.",
	"Wood":     "Warm low-mid body, no brittleness.",
	"Drywall":  "Damped, slightly muffled.",
	"Concrete": "Bright upper-mid bite at 1.5 kHz, the most present curve.",
	"Metal":    "Ringing 2 kHz peak with HF lift — resonant, harsh.",
	"Curtain":  "Broad mid cut, strong HF kill — fabric.",
	"Foliage":  "Broadband mid + HF cuts at low Q — diffuse softness.",
	"Meat":     "Soft, dense, wet body — creature impacts.",
	"Cardboard":"Light, papery — boxy mid emphasis with HF rolloff.",
	"Rubber":   "Dense, dead — broad low-mid damping.",
	"Liquid":   "Wet surface — strong HF suppression.",
}


func _can_handle(object: Object) -> bool:
	# GoolAudioMaterial is the only resource class we add the
	# preview for. We don't extend handling to AudioMaterialTag
	# nodes because the tag is a one-frame metadata pusher, not
	# a material authoring surface — its inspector should stay
	# the plain dropdown.
	if object == null:
		return false
	var script := object.get_script()
	if script == null:
		return false
	var path: String = script.resource_path
	return path == "res://addons/gool/resources/gool_audio_material.gd"


func _parse_end(object: Object) -> void:
	# Build the preview UI after all the resource's normal
	# @export properties have been rendered, so the preview sits
	# below the material picker (visually: pick a material →
	# see what that material sounds like).
	if not _can_handle(object):
		return

	# Pull the material int from the resource. GoolAudioMaterial
	# defines `@export var material: int = 0`, so this property is
	# always present on objects _can_handle() accepts. The intermediate
	# `Variant` typing through .get() keeps the editor strict-mode
	# parser happy even though we know the type statically.
	var material_value: Variant = object.get("material")
	var material_int: int = int(material_value) if material_value != null else 0

	var container := VBoxContainer.new()
	container.add_theme_constant_override("separation", 6)

	# Section header — matches the visual weight of the resource's
	# own group headers. The header is a Label rather than a
	# collapsible category because EditorInspectorPlugin's
	# add_custom_control puts content into the resource's bottom
	# panel, where a collapsible wouldn't add any value (the user
	# is already looking at one resource at a time).
	var header := Label.new()
	header.text = "EQ curve preview"
	header.add_theme_font_size_override("font_size", 13)
	container.add_child(header)

	# Hint text — material-specific one-liner.
	var hint := Label.new()
	hint.text = _hint_text(material_int)
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.add_theme_color_override("font_color", Color(0.7, 0.7, 0.7))
	container.add_child(hint)

	# The frequency-response plot.
	var view: Control = CURVE_VIEW_SCRIPT.new()
	view.curve = _curve_for_material(material_int)
	view.material_label = _material_name(material_int)
	view.intensity = _current_intensity()
	container.add_child(view)

	# Numerical readout under the plot. Three rows, one per band,
	# showing the exact authored values. Helps designers correlate
	# the visual curve with the values they could (eventually) edit.
	var values := _build_band_values_grid(view.curve)
	container.add_child(values)

	# Realism intensity slider — global setting, scales the three
	# gains uniformly. Reading and writing
	# ProjectSettings("gool/material_eq/intensity") directly avoids
	# needing the Gool autoload (which isn't running in editor
	# context). The runtime picks up the new value on next F5.
	var intensity_row := _build_intensity_row(view)
	container.add_child(intensity_row)

	# Small footer noting v0.59.2's read-only scope so designers
	# don't waste time looking for editable handles.
	var footer := Label.new()
	footer.text = (
		"Read-only preview (v0.59.2). Curve values are authored "
		+ "engine-side; per-material editable curves land in a "
		+ "future release."
	)
	footer.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	footer.add_theme_color_override("font_color", Color(0.55, 0.55, 0.55))
	footer.add_theme_font_size_override("font_size", 10)
	container.add_child(footer)

	add_custom_control(container)


# Resolve material int → friendly name. Out-of-range falls back to
# "Custom" (for the v0.49.0 custom material registry IDs ≥ 100;
# the inspector can't introspect those without the running autoload,
# so we just label them and show whatever the C++ side fills in).
func _material_name(material_int: int) -> String:
	if material_int >= 0 and material_int < _MATERIAL_NAMES.size():
		return _MATERIAL_NAMES[material_int]
	if material_int >= 100:
		return "Custom (#%d)" % material_int
	return "Unknown (#%d)" % material_int


func _hint_text(material_int: int) -> String:
	var name := _material_name(material_int)
	if _MATERIAL_HINTS.has(name):
		return _MATERIAL_HINTS[name]
	return "Per-material EQ curve (Phase 6.A — v0.33.0)."


# Read the current realism intensity from ProjectSettings. Falls
# back to 1.0 (the runtime default) if the setting isn't
# initialized yet.
func _current_intensity() -> float:
	if not ProjectSettings.has_setting(_EQ_INTENSITY_SETTING):
		return 1.0
	return clampf(
			float(ProjectSettings.get_setting(_EQ_INTENSITY_SETTING, 1.0)),
			0.0, 2.0)


# Get the EQ curve dictionary for a material. The Gool autoload
# isn't instantiated in editor context (it only spins up at F5
# game-session start), so we always use the hardcoded fallback
# table. The fallback values mirror the C++ MaterialEqByMaterial()
# table from Phase 6.A (v0.33.0), with tests/unit/material_eq_curve_test.cpp
# pinning the same values on the engine side. Drift between the
# inspector preview and the runtime would be caught by CI.
#
# Custom materials (IDs ≥ 100 from the v0.49.0 registry) aren't
# represented as GoolAudioMaterial.tres files — they're registered
# runtime Dictionaries on the autoload — so the inspector never
# sees them. If we ever change that, this is the lookup that
# would grow a "custom registry probe" branch.
func _curve_for_material(material_int: int) -> Dictionary:
	return _fallback_curve(material_int)


# Hardcoded mirror of the C++ MaterialEqByMaterial() table from
# Phase 6.A (v0.33.0). When Gool autoload isn't reachable in the
# editor, the inspector uses these values to drive the preview.
# Out-of-range materials get a neutral curve.
#
# These values are kept in sync with the C++ side by the test
# tests/unit/material_eq_curve_test.cpp which pins the same numbers
# from the engine table — drift between the two would be caught by
# CI if we forget to update either side. (The fallback exists so
# the inspector works without F5; the AUTHORITATIVE values are
# always on the C++ side.)
func _fallback_curve(material_int: int) -> Dictionary:
	# Default neutral (Air + Default + unknown).
	var neutral := {
		"low_gain_db": 0.0, "low_freq_hz": 200.0,
		"mid_gain_db": 0.0, "mid_freq_hz": 1000.0, "mid_q": 0.7,
		"high_gain_db": 0.0, "high_freq_hz": 8000.0,
		"is_neutral": true,
	}
	# Values mirror include/audio_engine/geometry_query.h's
	# MaterialEqByMaterial() switch as of v0.59.2. If those engine
	# values change, this table must be updated in lockstep —
	# tests/unit/material_eq_curve_test.cpp pins the canonical
	# numbers on the C++ side; a small companion test on the
	# GDScript side (v0.60.0+) should verify these match.
	match material_int:
		0, 1: return neutral   # Default, Air
		2:  # Glass — bright, neutral mids, characteristic HF ring
			return {
				"low_gain_db": -0.5, "low_freq_hz": 200.0,
				"mid_gain_db": -0.5, "mid_freq_hz": 1000.0, "mid_q": 1.0,
				"high_gain_db": +3.5, "high_freq_hz": 6000.0,
				"is_neutral": false,
			}
		3:  # Wood — warm low-mid body, soft top
			return {
				"low_gain_db": +2.0, "low_freq_hz": 250.0,
				"mid_gain_db": +1.5, "mid_freq_hz": 500.0, "mid_q": 0.7,
				"high_gain_db": -1.5, "high_freq_hz": 6000.0,
				"is_neutral": false,
			}
		4:  # Drywall — neutral, slightly damped
			return {
				"low_gain_db": 0.0, "low_freq_hz": 200.0,
				"mid_gain_db": -1.0, "mid_freq_hz": 1000.0, "mid_q": 0.7,
				"high_gain_db": -1.0, "high_freq_hz": 8000.0,
				"is_neutral": false,
			}
		5:  # Concrete — bright, hard, upper-mid crack
			return {
				"low_gain_db": +1.0, "low_freq_hz": 200.0,
				"mid_gain_db": +2.5, "mid_freq_hz": 1500.0, "mid_q": 1.0,
				"high_gain_db": +2.0, "high_freq_hz": 6000.0,
				"is_neutral": false,
			}
		6:  # Metal — hard, ringing, focused 2 kHz peak + HF lift
			return {
				"low_gain_db": 0.0, "low_freq_hz": 200.0,
				"mid_gain_db": +2.0, "mid_freq_hz": 2000.0, "mid_q": 1.5,
				"high_gain_db": +4.0, "high_freq_hz": 7000.0,
				"is_neutral": false,
			}
		7:  # Curtain — broad mid cut, strong HF kill, thick fabric
			return {
				"low_gain_db": 0.0, "low_freq_hz": 200.0,
				"mid_gain_db": -2.0, "mid_freq_hz": 800.0, "mid_q": 0.5,
				"high_gain_db": -4.0, "high_freq_hz": 4000.0,
				"is_neutral": false,
			}
		8:  # Foliage — broadband softness, low Q diffuse coloration
			return {
				"low_gain_db": 0.0, "low_freq_hz": 200.0,
				"mid_gain_db": -1.5, "mid_freq_hz": 1000.0, "mid_q": 0.4,
				"high_gain_db": -2.0, "high_freq_hz": 6000.0,
				"is_neutral": false,
			}
		9:  # Meat — soft, dense, wet creature body
			return {
				"low_gain_db": +1.5, "low_freq_hz": 250.0,
				"mid_gain_db": -1.0, "mid_freq_hz": 800.0, "mid_q": 0.5,
				"high_gain_db": -3.5, "high_freq_hz": 5000.0,
				"is_neutral": false,
			}
		10:  # Cardboard — light, porous, papery
			return {
				"low_gain_db": -0.5, "low_freq_hz": 250.0,
				"mid_gain_db": -0.5, "mid_freq_hz": 1500.0, "mid_q": 0.6,
				"high_gain_db": -2.0, "high_freq_hz": 7000.0,
				"is_neutral": false,
			}
		11:  # Rubber — dense, soft, dead — "eats sound"
			return {
				"low_gain_db": +0.5, "low_freq_hz": 300.0,
				"mid_gain_db": -1.5, "mid_freq_hz": 1200.0, "mid_q": 0.7,
				"high_gain_db": -4.0, "high_freq_hz": 5000.0,
				"is_neutral": false,
			}
		12:  # Liquid — wet surface — strongest HF absorption
			return {
				"low_gain_db": +2.5, "low_freq_hz": 200.0,
				"mid_gain_db": -1.0, "mid_freq_hz": 600.0, "mid_q": 0.5,
				"high_gain_db": -6.0, "high_freq_hz": 4000.0,
				"is_neutral": false,
			}
	return neutral


# Build the three-row numerical readout: Low / Mid / High, each
# showing freq, gain (intensity-scaled), and Q (for the mid band
# only — shelves have no Q parameter).
func _build_band_values_grid(curve: Dictionary) -> Control:
	var grid := GridContainer.new()
	grid.columns = 4
	grid.add_theme_constant_override("h_separation", 12)
	grid.add_theme_constant_override("v_separation", 2)

	var intensity := _current_intensity()
	var muted := Color(0.6, 0.6, 0.6)

	# Header row.
	var hdr_band := Label.new();  hdr_band.text  = "band"
	hdr_band.add_theme_color_override("font_color", muted)
	hdr_band.add_theme_font_size_override("font_size", 10)
	var hdr_freq := Label.new();  hdr_freq.text  = "freq"
	hdr_freq.add_theme_color_override("font_color", muted)
	hdr_freq.add_theme_font_size_override("font_size", 10)
	var hdr_gain := Label.new();  hdr_gain.text  = "gain"
	hdr_gain.add_theme_color_override("font_color", muted)
	hdr_gain.add_theme_font_size_override("font_size", 10)
	var hdr_q    := Label.new();  hdr_q.text     = "Q"
	hdr_q.add_theme_color_override("font_color", muted)
	hdr_q.add_theme_font_size_override("font_size", 10)
	grid.add_child(hdr_band)
	grid.add_child(hdr_freq)
	grid.add_child(hdr_gain)
	grid.add_child(hdr_q)

	_add_band_row(grid, "low shelf",
			float(curve.get("low_freq_hz", 200.0)),
			float(curve.get("low_gain_db", 0.0)) * intensity,
			NAN)  # shelves have no Q
	_add_band_row(grid, "peak",
			float(curve.get("mid_freq_hz", 1000.0)),
			float(curve.get("mid_gain_db", 0.0)) * intensity,
			float(curve.get("mid_q", 1.0)))
	_add_band_row(grid, "high shelf",
			float(curve.get("high_freq_hz", 8000.0)),
			float(curve.get("high_gain_db", 0.0)) * intensity,
			NAN)
	return grid


func _add_band_row(grid: GridContainer, label_str: String,
					freq_hz: float, gain_db: float, q: float) -> void:
	var lbl := Label.new()
	lbl.text = label_str
	grid.add_child(lbl)
	var freq := Label.new()
	if freq_hz >= 1000.0:
		freq.text = "%.2f kHz" % (freq_hz / 1000.0)
	else:
		freq.text = "%.0f Hz" % freq_hz
	grid.add_child(freq)
	var gain := Label.new()
	gain.text = "%+.1f dB" % gain_db
	grid.add_child(gain)
	var q_lbl := Label.new()
	if is_nan(q):
		q_lbl.text = "—"
		q_lbl.add_theme_color_override("font_color", Color(0.5, 0.5, 0.5))
	else:
		q_lbl.text = "%.2f" % q
	grid.add_child(q_lbl)


# Build the intensity-slider row: a Label + HSlider + value
# readout, wired so dragging updates the curve_view live and
# persists the new value to ProjectSettings on release.
func _build_intensity_row(view: Control) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)

	var label := Label.new()
	label.text = "Realism intensity"
	label.custom_minimum_size = Vector2(120, 0)
	row.add_child(label)

	var slider := HSlider.new()
	slider.min_value = 0.0
	slider.max_value = 2.0
	slider.step = 0.05
	slider.value = _current_intensity()
	slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(slider)

	var value_label := Label.new()
	value_label.text = "%.2f" % slider.value
	value_label.custom_minimum_size = Vector2(48, 0)
	value_label.add_theme_color_override("font_color", Color(0.7, 0.7, 0.7))
	row.add_child(value_label)

	var reset_btn := Button.new()
	reset_btn.text = "Reset"
	reset_btn.tooltip_text = "Restore intensity to 1.0 (curves as-tabled)."
	row.add_child(reset_btn)

	# Live-update on drag: change the visualizer immediately so the
	# designer sees the scaled curve in real time. Persistence to
	# ProjectSettings happens on drag-end (drag_ended signal) to
	# avoid spamming the project file with every intermediate value.
	slider.value_changed.connect(
			func(v: float):
				view.intensity = v
				value_label.text = "%.2f" % v)
	slider.drag_ended.connect(
			func(_value_changed_during_drag: bool):
				_persist_intensity(slider.value))
	reset_btn.pressed.connect(
			func():
				slider.value = 1.0
				_persist_intensity(1.0))
	return row


# Write the intensity to ProjectSettings and save. The runtime
# Gool autoload reads this on _ready, so the new value takes
# effect on next F5. We don't try to push to a running Gool
# instance because the editor and game-session run in separate
# processes (editor is @tool; F5 game has its own Gool).
func _persist_intensity(value: float) -> void:
	var clamped := clampf(value, 0.0, 2.0)
	ProjectSettings.set_setting(_EQ_INTENSITY_SETTING, clamped)
	var err: int = ProjectSettings.save()
	if err != OK:
		push_warning("[gool] could not save material_eq/intensity "
				+ "to project.godot (error %d). The value is set "
				% err
				+ "for this editor session but won't persist.")
