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
	# below the per-band fields.
	if not _can_handle(object):
		return

	# Resource handle — we hold a strong ref via the closures below
	# so the inspector's signal connections stay valid even if the
	# user is rapidly clicking between resources.
	var resource: Resource = object

	# Read the material int and override state. GoolAudioMaterial
	# defines material/override_enabled/per-band fields, but we
	# read defensively via .get() so a malformed resource (mid-edit
	# of the schema script) doesn't crash the inspector.
	var material_value: Variant = resource.get("material")
	var material_int: int = int(material_value) if material_value != null else 0
	var override_enabled: bool = bool(resource.get("override_enabled"))

	var container := VBoxContainer.new()
	container.add_theme_constant_override("separation", 6)

	# Section header — matches the visual weight of the resource's
	# own group headers.
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

	# v0.60.0: resolve the effective curve. When override is on,
	# read from the resource's per-band fields. When off, use the
	# inspector's engine-table mirror (the Gool autoload isn't
	# reachable in editor context, so we can't go through the
	# resource's get_curve() — its autoload fallback would return
	# neutral). The mirror values match the C++ table and are
	# verified by material_eq_curve_test.cpp on the engine side.
	var effective_curve: Dictionary = _effective_curve(resource, material_int)

	# The frequency-response plot.
	var view: Control = CURVE_VIEW_SCRIPT.new()
	view.curve = effective_curve
	view.material_label = _material_name(material_int)
	view.intensity = _current_intensity()
	view.editable = override_enabled
	container.add_child(view)

	# v0.60.0: when override is on, wire the drag-handle signals
	# from the plot back to resource property writes. The motion
	# signal fires per mouse-motion frame (cheap — just writes a
	# few floats). The drag-ended signal fires once when the user
	# releases the mouse (heavier — saves the resource to disk).
	if override_enabled:
		view.band_changed.connect(
				func(band_index: int, freq_hz: float,
						gain_db: float, q: float):
					_apply_band_drag(resource, band_index,
							freq_hz, gain_db, q))
		view.band_drag_ended.connect(
				func(_band_index: int):
					_save_resource(resource))

	# Live refresh of the plot when the resource changes (drag
	# motion + explicit field edits both flow through the same
	# `changed` signal). We connect on every _parse_end run; Godot
	# tracks duplicate connections so the same callback isn't
	# wired twice if the inspector rebuilds quickly. Connection
	# is auto-disconnected when the view is freed (inspector
	# rebuild), so no leak.
	resource.changed.connect(
			func():
				if is_instance_valid(view):
					view.curve = _effective_curve(resource, material_int))

	# Numerical readout under the plot.
	var values := _build_band_values_grid(view.curve)
	container.add_child(values)

	# v0.60.0: when override is on, add a Q slider for the mid band.
	# (Drag handles adjust freq + gain; Q is the third per-band
	# parameter that's not amenable to 2D dragging, so a slider is
	# the cleanest control surface. Low and high shelves use Q=1.0
	# fixed — the runtime apply path doesn't expose shelf Q.)
	if override_enabled:
		container.add_child(_build_mid_q_slider(resource))

	# Realism intensity slider — global setting, scales the three
	# gains uniformly.
	var intensity_row := _build_intensity_row(view)
	container.add_child(intensity_row)

	# v0.60.0: Override toggle. Above the audition row so designers
	# see the toggle before the audition (auditioning the same
	# material before-and-after override is the natural workflow).
	var override_row := _build_override_toggle(resource)
	container.add_child(override_row)

	# Audition button. v0.59.3 introduced this; v0.60.0 makes it
	# override-aware (routes through process_buffer_through_curve
	# when override_enabled=true so the audition reflects the
	# designer's tweaks, not the engine table).
	var audition_row := _build_audition_row(resource, material_int)
	container.add_child(audition_row)

	# v0.61.2 — Phase 6.E.4 (first cut): preset row.
	# "Save preset..." captures the current override curve as a
	# named .tres under res://gool/material_eq_presets/. Enabled
	# only when override is on (no point saving the engine table —
	# that's already addressable via the material int).
	# "Load preset..." pops up the picker, copies the chosen
	# preset's band values into the resource's override fields,
	# flips override_enabled on, and saves. Available always:
	# loading a preset onto an override-off resource is a valid
	# "start from this curve" workflow.
	var preset_row := _build_preset_row(resource, override_enabled)
	container.add_child(preset_row)

	# Small footer reflecting current Phase 6.E status.
	var footer := Label.new()
	footer.text = (
		"v0.61.3: Phase 6.E.1/2/4 shipped. Override + drag handles "
		+ "for per-instance EQ; mixer dock shows live bus EQ; "
		+ "Load… picker includes ~12 built-in tonal-character "
		+ "presets shipping with gool, alongside any project-level "
		+ "presets at res://gool/material_eq_presets/."
	)
	footer.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	footer.add_theme_color_override("font_color", Color(0.55, 0.55, 0.55))
	footer.add_theme_font_size_override("font_size", 10)
	container.add_child(footer)

	add_custom_control(container)


# v0.60.0: resolve the effective curve for inspector display.
# When override_enabled is true on the resource, build the curve
# dict from the resource's per-band fields. When false, use the
# inspector's hardcoded engine-table mirror (since the Gool
# autoload isn't reachable in editor and the resource's own
# fallback returns neutral in that context).
func _effective_curve(resource: Resource, material_int: int) -> Dictionary:
	if bool(resource.get("override_enabled")):
		return {
			"low_gain_db":  float(resource.get("low_gain_db")),
			"low_freq_hz":  float(resource.get("low_freq_hz")),
			"mid_gain_db":  float(resource.get("mid_gain_db")),
			"mid_freq_hz":  float(resource.get("mid_freq_hz")),
			"mid_q":        float(resource.get("mid_q")),
			"high_gain_db": float(resource.get("high_gain_db")),
			"high_freq_hz": float(resource.get("high_freq_hz")),
			"is_neutral":   false,
		}
	return _fallback_curve(material_int)


# Apply a drag-motion delta to the resource's per-band fields. The
# resource's setters re-emit `changed`, which feeds back through to
# the plot view's `curve` property to redraw. Motion is cheap; no
# disk write here (that happens on drag-ended).
func _apply_band_drag(resource: Resource, band_index: int,
		freq_hz: float, gain_db: float, q: float) -> void:
	match band_index:
		0:
			resource.set("low_freq_hz", freq_hz)
			resource.set("low_gain_db", gain_db)
		1:
			resource.set("mid_freq_hz", freq_hz)
			resource.set("mid_gain_db", gain_db)
			# Q comes from the curve dict (current_q in the curve
			# view), which is already on the resource — no change
			# unless the slider sent it.
		2:
			resource.set("high_freq_hz", freq_hz)
			resource.set("high_gain_db", gain_db)


# Save the resource to disk via ResourceSaver. Called on drag-ended
# so we don't hammer disk during dragging. The resource's existing
# path is used (the .tres the user is editing).
func _save_resource(resource: Resource) -> void:
	if resource.resource_path.is_empty():
		# In-memory resource not yet saved (the user created it
		# but hasn't picked a path yet). Skip; values are still
		# preserved in the resource object, just not on disk.
		return
	var err: int = ResourceSaver.save(resource, resource.resource_path)
	if err != OK:
		push_warning("[gool] could not save %s after drag: error %d"
				% [resource.resource_path, err])


# Build the override-enable toggle. A CheckBox with a small hint.
# Toggling fires resource.override_enabled = !current, which in
# turn (via the resource's setter) seeds the override fields from
# the engine table on false → true. Godot's inspector rebuilds on
# property change, so _parse_end runs again with the new state.
func _build_override_toggle(resource: Resource) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)

	var checkbox := CheckBox.new()
	checkbox.text = "Override curve (edit per-band values)"
	checkbox.button_pressed = bool(resource.get("override_enabled"))
	checkbox.tooltip_text = (
			"When on, this resource's per-band EQ fields take "
			+ "effect at runtime instead of the engine's built-in "
			+ "curve. Use the drag handles on the plot to author. "
			+ "Saved to disk on drag-end. Toggle off to revert to "
			+ "the engine table (zero runtime overhead)."
	)
	row.add_child(checkbox)

	checkbox.toggled.connect(
			func(pressed: bool):
				resource.set("override_enabled", pressed)
				# Saving immediately preserves the toggle state
				# across editor restarts. The full inspector
				# rebuild happens via the property-change side
				# of the Godot inspector pipeline.
				_save_resource(resource))
	return row


# Build the mid-band Q slider, shown only when override is on.
# Drag handles cover freq + gain; Q is the third per-band
# parameter that doesn't fit a 2D drag, so it gets its own slider.
func _build_mid_q_slider(resource: Resource) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)

	var label := Label.new()
	label.text = "Mid Q"
	label.custom_minimum_size = Vector2(120, 0)
	row.add_child(label)

	var slider := HSlider.new()
	slider.min_value = 0.1
	slider.max_value = 10.0
	slider.step = 0.05
	slider.value = float(resource.get("mid_q"))
	slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	slider.tooltip_text = (
			"Peak band sharpness. 0.5 is broad, 1.0 moderate, "
			+ "2.0+ surgical. Only the mid peak has Q; low and "
			+ "high shelves use Q=1.0 fixed."
	)
	row.add_child(slider)

	var value_label := Label.new()
	value_label.text = "%.2f" % slider.value
	value_label.custom_minimum_size = Vector2(48, 0)
	value_label.add_theme_color_override("font_color", Color(0.7, 0.7, 0.7))
	row.add_child(value_label)

	slider.value_changed.connect(
			func(v: float):
				resource.set("mid_q", v)
				value_label.text = "%.2f" % v)
	slider.drag_ended.connect(
			func(_value_changed_during_drag: bool):
				_save_resource(resource))
	return row


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


# ---- v0.59.3: Audition button ------------------------------------
#
# Generates a short pink-noise burst in GDScript, routes it through
# GoolAudioRuntime.process_buffer_through_material_eq (a STATIC
# C++ method, no autoload required), and plays the result inline
# through an editor-local AudioStreamPlayer + AudioStreamWAV.
#
# Why pink noise specifically: white noise (equal energy per Hz) is
# perceptually bright because human hearing weights high frequencies
# more. Pink noise (equal energy per OCTAVE) sounds spectrally
# "balanced" — the perceived loudness is roughly flat across the
# audible spectrum, so EQ shaping is audibly obvious (a +3 dB peak
# at 1.5 kHz is heard as ~+3 dB at 1.5 kHz, not buried under HF
# brightness).
#
# Why GDScript pink noise (not a shipped .wav): pink noise is
# trivially generated procedurally with a Voss-McCartney filter —
# half a dozen lines of code, no asset bloat, deterministic so
# every audition for the same material sounds the same.
#
# Why int16 PCM via AudioStreamWAV (not AudioStreamGenerator):
# AudioStreamWAV is a one-shot finite-buffer player; the audition
# IS a finite buffer playback. AudioStreamGenerator is for
# streaming/continuous synthesis where the producer pushes buffers
# at audio-rate; overkill and adds threading complexity here.

# Audition buffer length in samples at AUDITION_SAMPLE_RATE Hz.
# 1.0 s gives biquads ~50 ms of pre-roll to settle (well past the
# longest filter group delay at these frequencies) and ~950 ms of
# steady-state audible character — comfortable but not tedious.
const AUDITION_DURATION_S: float = 1.0
const AUDITION_SAMPLE_RATE: int = 48000

# Output target peak amplitude. The pink-noise generator produces
# unit-amplitude samples; we attenuate to leave headroom for boosts
# (Concrete at intensity 2.0 can add ~5 dB net gain on broadband
# noise — clipping above ~0.55 would be likely without this
# attenuation). -12 dBFS leaves plenty of headroom in every case.
const AUDITION_PEAK: float = 0.25


func _build_audition_row(resource: Resource, material_int: int) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)

	var label := Label.new()
	label.text = "Audition"
	label.custom_minimum_size = Vector2(120, 0)
	row.add_child(label)

	# The playback button. Pressing it (re)generates pink noise,
	# pushes it through the audition C++ method, builds a fresh
	# AudioStreamWAV, plays it.
	var play_btn := Button.new()
	play_btn.text = "▶ Play (pink noise, 1 s)"
	play_btn.tooltip_text = (
			"Generates 1 second of pink noise, runs it through the "
			+ "material's effective EQ curve at the current realism "
			+ "intensity, and plays the result. When 'Override curve' "
			+ "is on, the per-band override values are used; "
			+ "otherwise the engine's built-in curve for this "
			+ "material. Uses the same biquad chain as the runtime "
			+ "impact-EQ path — what you hear is what the player "
			+ "will hear."
	)
	play_btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(play_btn)

	# Tiny inline player. AudioStreamPlayer must be in the editor's
	# scene tree to actually emit audio; we add it as a child of
	# the row Control. When the row is freed (inspector rebuild
	# on selection change), Godot frees its descendants including
	# the player — no leak.
	var player := AudioStreamPlayer.new()
	player.bus = "Master"
	row.add_child(player)

	# v0.60.0: capture the resource by reference so the audition
	# always reads the resource's CURRENT override state at click
	# time, not whatever was captured at inspector-build time.
	# This way a user can drag a band, immediately press audition,
	# and hear the post-drag curve.
	play_btn.pressed.connect(
			func():
				_audition_play(player, resource, material_int))
	return row


# Run the audition pipeline end-to-end. v0.60.0: branches on
# resource.override_enabled. When on, uses process_buffer_through_curve
# with the resource's per-band fields. When off, uses
# process_buffer_through_material_eq with the engine table.
func _audition_play(player: AudioStreamPlayer,
		resource: Resource, material_int: int) -> void:
	var noise := _generate_pink_noise(
			int(AUDITION_DURATION_S * float(AUDITION_SAMPLE_RATE)),
			material_int * 7919 + 13)   # seed varies per-material

	var intensity := _current_intensity()
	var processed: PackedFloat32Array
	if bool(resource.get("override_enabled")):
		# Override path: pass per-band values directly.
		processed = GoolAudioRuntime.process_buffer_through_curve(
				noise,
				float(resource.get("low_freq_hz")),
				float(resource.get("low_gain_db")),
				float(resource.get("mid_freq_hz")),
				float(resource.get("mid_gain_db")),
				float(resource.get("mid_q")),
				float(resource.get("high_freq_hz")),
				float(resource.get("high_gain_db")),
				intensity,
				AUDITION_SAMPLE_RATE)
	else:
		# Engine-table path: same code path v0.59.3 used. The
		# parity test (material_eq_audition_test.cpp's
		# TestCurveMatchesMaterial) pins this against the curve
		# path at bit equality, so toggling override off and on
		# at the same material's engine-table values would produce
		# bit-identical audition output.
		processed = GoolAudioRuntime.process_buffer_through_material_eq(
				noise, material_int, intensity,
				AUDITION_SAMPLE_RATE)
	if processed.is_empty():
		push_warning("[gool] audition: process returned empty. "
				+ "Material %d, intensity %.2f, override=%s."
				% [material_int, intensity,
				   str(bool(resource.get("override_enabled")))])
		return

	# Build the AudioStreamWAV from the processed buffer and play.
	var stream := _stream_from_float_buffer(processed, AUDITION_SAMPLE_RATE)
	player.stream = stream
	player.play()


# ---- Helpers -----------------------------------------------------

# Generate `length` samples of pink-ish noise using a 5-octave
# Voss-McCartney approximation. Pink-ish, not strictly pink — the
# Voss-McCartney method overlays a handful of slow-updating random
# sources at progressively halved update rates. The resulting PSD
# approximates 1/f within ~2 dB across most of the audible range,
# more than good enough for audition purposes.
#
# Deterministic given a seed (uses a Linear Congruential Generator
# rather than randf()), so the same material always sounds the
# same on repeated button presses.
func _generate_pink_noise(length: int, seed: int) -> PackedFloat32Array:
	var out := PackedFloat32Array()
	out.resize(length)

	# LCG state (Numerical Recipes parameters).
	var lcg_state: int = (seed if seed != 0 else 12345)

	# Five octave-band random sources, each updating at half the
	# rate of the previous. Voss-McCartney style.
	const N_OCTAVES: int = 5
	var rows := PackedFloat32Array()
	rows.resize(N_OCTAVES)
	# rows starts as zeros — first iteration fills them.

	# Running sum across the rows; tracking the delta lets us avoid
	# recomputing the full sum every sample.
	var running_sum: float = 0.0

	# Find the highest bit set in a counter; equivalently the
	# largest power-of-two divisor.
	var counter: int = 0
	for i in length:
		# Decide which row to update based on bit count.
		# Row k updates every 2^k samples.
		counter += 1
		var row_to_update: int = -1
		for k in N_OCTAVES:
			if (counter & (1 << k)) != 0:
				row_to_update = k
				break
		if row_to_update >= 0:
			lcg_state = (lcg_state * 1103515245 + 12345) & 0x7FFFFFFF
			# Map u31 to [-1, 1).
			var rnd: float = (float(lcg_state) / 1073741824.0) - 1.0
			var old: float = rows[row_to_update]
			rows[row_to_update] = rnd
			running_sum += rnd - old

		# Also add one fast-updating "white" row that updates every
		# sample. Without this, the highest octave wouldn't be
		# represented (the per-sample variation would only come
		# from rows[0] updates which happen every other sample).
		lcg_state = (lcg_state * 1103515245 + 12345) & 0x7FFFFFFF
		var fast: float = (float(lcg_state) / 1073741824.0) - 1.0

		# Sum of slow rows + fast row, normalized by sqrt(N) so
		# RMS is independent of N_OCTAVES. The /sqrt formula
		# preserves perceived loudness regardless of how many
		# octave bands we use.
		var sample: float = (running_sum + fast) / sqrt(float(N_OCTAVES + 1))
		out[i] = sample * AUDITION_PEAK

	return out


# Convert a float buffer into an AudioStreamWAV at the given
# sample rate, mono, 16-bit PCM. The conversion clamps samples
# to [-1, 1] before scaling — handles any boosts that pushed
# beyond unity (rare given AUDITION_PEAK, but safe to guard).
func _stream_from_float_buffer(buffer: PackedFloat32Array,
		sample_rate: int) -> AudioStreamWAV:
	var n: int = buffer.size()
	var bytes := PackedByteArray()
	bytes.resize(n * 2)   # 2 bytes per int16 sample
	for i in n:
		var s: float = clampf(buffer[i], -1.0, 1.0)
		var int_sample: int = int(s * 32767.0)
		# Little-endian 16-bit signed.
		bytes[i * 2]     = int_sample & 0xFF
		bytes[i * 2 + 1] = (int_sample >> 8) & 0xFF
	var stream := AudioStreamWAV.new()
	stream.format = AudioStreamWAV.FORMAT_16_BITS
	stream.stereo = false
	stream.mix_rate = sample_rate
	stream.data = bytes
	return stream


# v0.61.2 — Phase 6.E.4 (first cut): preset row.
#
# Two buttons side by side: Save and Load. Save is enabled only
# when the resource's override_enabled is true (no point saving
# the engine table, which is already addressable by material int).
# Load is always enabled — loading a preset onto an override-off
# resource is a valid "I want to start from THIS curve" workflow,
# and it implicitly flips override on.
#
# The PRESET_MANAGER preload is wrapped in a const at this scope
# (not above the file's existing CURVE_VIEW_SCRIPT block) so the
# preset-related code stays grouped at the bottom of the file
# alongside the rest of v0.61.2 additions. Cheap to preload at
# parse time; doesn't affect inspector cold-start cost.
const _PRESET_MANAGER := preload(
		"res://addons/gool/editor/material_eq_preset_manager.gd")
const _PRESET_SCRIPT := preload(
		"res://addons/gool/resources/gool_material_eq_preset.gd")


func _build_preset_row(resource: Resource, override_on: bool) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)

	var label := Label.new()
	label.text = "Presets"
	label.custom_minimum_size = Vector2(120, 0)
	row.add_child(label)

	var save_btn := Button.new()
	save_btn.text = "Save…"
	save_btn.tooltip_text = (
			"Save the current override curve as a named preset "
			+ "under res://gool/material_eq_presets/. Enabled when "
			+ "'Override curve' is on."
	)
	save_btn.disabled = not override_on
	save_btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	save_btn.pressed.connect(
			func() -> void:
				_show_save_preset_dialog(resource))
	row.add_child(save_btn)

	var load_btn := Button.new()
	load_btn.text = "Load…"
	load_btn.tooltip_text = (
			"Apply a saved preset's curve to this material. Loads "
			+ "from res://gool/material_eq_presets/. Will turn "
			+ "'Override curve' on if it isn't already."
	)
	load_btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	load_btn.pressed.connect(
			func() -> void:
				_show_load_preset_dialog(resource))
	row.add_child(load_btn)

	return row


# Pop the "Save as preset…" dialog. Single LineEdit for the name,
# with a sensible default. On accept: build a preset from the
# resource's current override fields, write it through the
# manager, push a status message.
func _show_save_preset_dialog(resource: Resource) -> void:
	var dlg := AcceptDialog.new()
	dlg.title = "Save EQ preset"
	dlg.ok_button_text = "Save"
	dlg.add_cancel_button("Cancel")

	var vbox := VBoxContainer.new()
	vbox.add_theme_constant_override("separation", 6)

	var hint := Label.new()
	hint.text = (
			"Saves the current override curve as a named .tres "
			+ "under res://gool/material_eq_presets/. The name "
			+ "becomes the picker label; punctuation is replaced "
			+ "with underscores in the filename."
	)
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.add_theme_color_override("font_color", Color(0.7, 0.7, 0.7))
	hint.custom_minimum_size = Vector2(360, 0)
	vbox.add_child(hint)

	var name_label := Label.new()
	name_label.text = "Preset name:"
	vbox.add_child(name_label)

	var name_edit := LineEdit.new()
	# Default: the material's friendly name as a starting point.
	# Designer probably wants to refine it but a populated field
	# is faster than a blank one.
	name_edit.text = _material_name(int(resource.get("material")))
	name_edit.placeholder_text = "e.g. Brawler concrete (punchier)"
	name_edit.custom_minimum_size = Vector2(360, 0)
	vbox.add_child(name_edit)

	dlg.add_child(vbox)
	dlg.register_text_enter(name_edit)
	dlg.confirmed.connect(
			func() -> void:
				_on_save_preset_confirmed(resource, name_edit.text, dlg))
	dlg.canceled.connect(dlg.queue_free)
	dlg.close_requested.connect(dlg.queue_free)
	# Add to the editor scene tree via EditorInterface — same path
	# the mixer dock uses for its ConfirmationDialog instances. The
	# inspector itself isn't in a viewport-rooted tree until added,
	# so we attach to the base control.
	EditorInterface.get_base_control().add_child(dlg)
	dlg.popup_centered()
	name_edit.grab_focus()


func _on_save_preset_confirmed(resource: Resource, name: String,
		dlg: AcceptDialog) -> void:
	dlg.queue_free()
	var trimmed: String = name.strip_edges()
	if trimmed.is_empty():
		push_warning("[gool] preset name was empty — nothing saved")
		return
	# Check for overwrite. If a preset by this sanitized name
	# already exists, confirm before clobbering. The previous
	# author may have used the same display name with a different
	# tuning, and the v0.61.2 sanitization is destructive (only
	# one .tres per sanitized name).
	if _PRESET_MANAGER.would_overwrite(trimmed):
		var confirm := ConfirmationDialog.new()
		confirm.title = "Overwrite existing preset?"
		confirm.dialog_text = (
				"A preset with this name already exists at "
				+ "res://gool/material_eq_presets/. Overwrite?"
		)
		confirm.ok_button_text = "Overwrite"
		confirm.confirmed.connect(
				func() -> void:
					_do_save_preset(resource, trimmed)
					confirm.queue_free())
		confirm.canceled.connect(confirm.queue_free)
		confirm.close_requested.connect(confirm.queue_free)
		EditorInterface.get_base_control().add_child(confirm)
		confirm.popup_centered()
	else:
		_do_save_preset(resource, trimmed)


func _do_save_preset(resource: Resource, display_name: String) -> void:
	var preset: Resource = _PRESET_SCRIPT.from_material(resource)
	var saved_path: String = _PRESET_MANAGER.save_preset(preset, display_name)
	if saved_path.is_empty():
		# save_preset already pushed a warning explaining why.
		return
	print("[gool] saved preset to ", saved_path)
	# Filesystem rescan so the new preset shows up immediately in
	# any subsequent picker dialog without needing a manual refresh.
	EditorInterface.get_resource_filesystem().scan()


# Pop the "Load preset…" dialog. Lists all presets in the directory
# via ItemList. On accept: copy the chosen preset's band values
# into the resource's override fields, flip override on if it isn't,
# save the resource so the change persists.
func _show_load_preset_dialog(resource: Resource) -> void:
	var presets: Array = _PRESET_MANAGER.list_presets()
	var dlg := AcceptDialog.new()
	dlg.title = "Load EQ preset"
	dlg.ok_button_text = "Load"
	dlg.add_cancel_button("Cancel")

	var vbox := VBoxContainer.new()
	vbox.add_theme_constant_override("separation", 6)

	if presets.is_empty():
		# v0.61.3 ships with built-in presets at res://addons/gool/
		# material_eq_presets/, so a completely empty picker should
		# never happen in normal installations — it implies the addon
		# directory is missing or unreadable. Message names both
		# directories so the user can investigate.
		var empty_label := Label.new()
		empty_label.text = (
				"No presets found. Built-in presets are normally "
				+ "shipped at res://addons/gool/material_eq_presets/; "
				+ "user presets are saved to "
				+ "res://gool/material_eq_presets/. If the addon "
				+ "directory looks empty, reinstall gool. Otherwise, "
				+ "save your first preset via the 'Save…' button "
				+ "after tweaking an override curve."
		)
		empty_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		empty_label.custom_minimum_size = Vector2(360, 0)
		empty_label.add_theme_color_override("font_color",
				Color(0.7, 0.7, 0.7))
		vbox.add_child(empty_label)
		dlg.add_child(vbox)
		dlg.ok_button_text = "OK"
		dlg.get_ok_button().disabled = true
		dlg.close_requested.connect(dlg.queue_free)
		dlg.canceled.connect(dlg.queue_free)
		EditorInterface.get_base_control().add_child(dlg)
		dlg.popup_centered()
		return

	var hint := Label.new()
	hint.text = (
			"Select a preset to apply. ★ prefix marks built-in "
			+ "presets that ship with gool; un-prefixed are your "
			+ "project's saved presets. The curve values will be "
			+ "copied onto this material's override fields and "
			+ "'Override curve' will be turned on."
	)
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.add_theme_color_override("font_color", Color(0.7, 0.7, 0.7))
	hint.custom_minimum_size = Vector2(360, 0)
	vbox.add_child(hint)

	var item_list := ItemList.new()
	item_list.custom_minimum_size = Vector2(360, 200)
	item_list.allow_reselect = true
	# v0.61.3: distinguish built-in presets (★ prefix, slightly muted
	# tint) from user presets. Designers immediately see "these are
	# the ones gool shipped with" vs "these are mine". Tooltip on
	# each item carries the full path so the source is unambiguous
	# when a user has saved a preset with the same name as a built-in.
	for entry in presets:
		var e: Dictionary = entry as Dictionary
		var is_builtin: bool = bool(e.get("is_builtin", false))
		var display: String = String(e.get("name", "(unnamed)"))
		var desc: String = String((e.get("preset") as Resource).get(
				"description"))
		var line: String = display
		if is_builtin:
			line = "★  " + line
		if not desc.is_empty():
			line += "  —  " + desc
		var item_idx: int = item_list.add_item(line)
		# Tooltip carries the path so the user can disambiguate a
		# built-in vs a same-named user preset, or copy the path
		# for FileSystem-dock navigation.
		var tip: String = String(e.get("path", ""))
		if is_builtin:
			tip = "Built-in (ships with gool)\n" + tip
		else:
			tip = "User preset\n" + tip
		item_list.set_item_tooltip(item_idx, tip)
		# Slight color tint on built-ins so a glance at the picker
		# distinguishes them even without the prefix.
		if is_builtin:
			item_list.set_item_custom_fg_color(item_idx,
					Color(0.85, 0.95, 1.0))
	if item_list.item_count > 0:
		item_list.select(0)
	vbox.add_child(item_list)

	dlg.add_child(vbox)
	dlg.confirmed.connect(
			func() -> void:
				_on_load_preset_confirmed(resource, presets,
						item_list.get_selected_items(), dlg))
	dlg.canceled.connect(dlg.queue_free)
	dlg.close_requested.connect(dlg.queue_free)
	# Double-click in the list = accept.
	item_list.item_activated.connect(
			func(_idx: int) -> void:
				dlg.confirmed.emit()
				dlg.queue_free())
	EditorInterface.get_base_control().add_child(dlg)
	dlg.popup_centered()


func _on_load_preset_confirmed(resource: Resource, presets: Array,
		selection: PackedInt32Array, dlg: AcceptDialog) -> void:
	dlg.queue_free()
	if selection.is_empty():
		return
	var idx: int = selection[0]
	if idx < 0 or idx >= presets.size():
		return
	var entry: Dictionary = presets[idx] as Dictionary
	var preset: Resource = entry.get("preset") as Resource
	if preset == null:
		push_warning("[gool] selected preset entry had no preset Resource")
		return
	# Order matters: flipping override_enabled false → true triggers
	# the resource's setter to seed override fields from the engine
	# table. If we applied the preset BEFORE flipping, the seed
	# would clobber the preset values. So we flip first (no-op if
	# override was already on; seeds engine table if it was off),
	# then apply the preset over the top — the preset wins.
	resource.set("override_enabled", true)
	preset.apply_to_material(resource)
	_save_resource(resource)
	# emit_changed so the inspector's curve view + readout refresh
	# immediately. The per-field setters already emit, but a single
	# explicit emit at the end guarantees the inspector rebuild sees
	# a coherent post-load state rather than chasing seven partial
	# updates.
	resource.emit_changed()
	print("[gool] applied preset '%s'" % String(entry.get("name", "(unnamed)")))
