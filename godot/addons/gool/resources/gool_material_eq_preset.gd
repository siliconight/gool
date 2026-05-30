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

# addons/gool/resources/gool_material_eq_preset.gd
#
# v0.61.2 — Phase 6.E.4 (first cut): named EQ curve presets.
#
# A saved-and-named copy of the seven band parameters from a
# GoolAudioMaterial's override fields. Lets designers capture a
# tweak they like ("Brawler concrete (punchier)", "Cathedral
# foliage", etc.) and apply it back to any material later, on the
# same project or shared across projects via the .tres file.
#
# This is the EQ-only first cut. The acoustic-presence-EQ design
# doc envisions full material PROFILES (occlusion + EQ + reverb
# overrides bundled together), which would extend this resource
# with additional fields. Forward-compatible: existing preset
# files saved by v0.61.2 will load fine into a future version
# that adds occlusion/reverb fields; the new fields default to
# their unset values.
#
# Storage convention:
#   res://gool/material_eq_presets/<sanitized_name>.tres
#
# Naming: the `preset_name` field is the human-readable label
# shown in the picker dropdown. The filename is derived from
# `preset_name` via a path-safe sanitization pass — designers
# don't pick filenames directly. Two presets with the same
# sanitized name would clobber each other on save; the inspector
# warns before overwrite.

@tool
class_name GoolMaterialEqPreset
extends Resource


## Human-readable label shown in the preset picker. Distinct from
## the .tres filename: the filename is derived from this via path-
## safe sanitization. Free-form spaces, punctuation, and unicode
## are all fine here.
@export var preset_name: String = ""


## Optional one-line description. Shown in the picker dialog's
## tooltip / detail line. Designers can park context here — what
## the preset's for, what kind of source it was tuned against,
## etc.
@export var description: String = ""


## Schema version. Bumped if/when GoolMaterialEqPreset gains
## fields (occlusion overrides, reverb preset binding, etc.).
## Loaders should accept anything ≤ their own known version and
## default missing fields. Mostly defensive — Godot's .tres
## loader already tolerates unknown fields gracefully, but an
## explicit version field makes intent clear when a designer
## opens an old preset in a future editor.
@export var preset_schema_version: int = 1


# --- The seven EQ band parameters (mirrors GoolAudioMaterial) ---

## Low-shelf knee frequency in Hz.
@export_range(20.0, 2000.0, 1.0, "or_greater", "or_less", "suffix:Hz")
var low_freq_hz: float = 200.0

## Low-shelf gain in dB. Positive boosts below the knee; negative cuts.
@export_range(-12.0, 12.0, 0.1, "suffix:dB")
var low_gain_db: float = 0.0

## Peaking band center frequency in Hz.
@export_range(50.0, 12000.0, 1.0, "or_greater", "or_less", "suffix:Hz")
var mid_freq_hz: float = 1000.0

## Peaking band gain in dB.
@export_range(-12.0, 12.0, 0.1, "suffix:dB")
var mid_gain_db: float = 0.0

## Peaking band Q (sharpness). 0.5 broad, 1.0 moderate, 2.0+ surgical.
@export_range(0.1, 10.0, 0.05)
var mid_q: float = 0.7

## High-shelf knee frequency in Hz.
@export_range(1000.0, 20000.0, 1.0, "or_greater", "or_less", "suffix:Hz")
var high_freq_hz: float = 8000.0

## High-shelf gain in dB.
@export_range(-12.0, 12.0, 0.1, "suffix:dB")
var high_gain_db: float = 0.0


## Return the seven band params as a Dictionary in the shape
## MaterialEqCurveView.curve expects (same shape Gool.get_material_eq_for_material
## returns). Used by the inspector's preset-load path to push the
## preset into the visualizer without going through the GoolAudioMaterial.
func to_curve() -> Dictionary:
	return {
		"low_freq_hz":  low_freq_hz,
		"low_gain_db":  low_gain_db,
		"mid_freq_hz":  mid_freq_hz,
		"mid_gain_db":  mid_gain_db,
		"mid_q":        mid_q,
		"high_freq_hz": high_freq_hz,
		"high_gain_db": high_gain_db,
		"is_neutral":   false,
	}


## Populate a fresh preset's band params from a GoolAudioMaterial's
## current override fields. Caller fills in `preset_name` and
## `description` separately. Reads via .get() so a malformed
## material doesn't crash here.
static func from_material(material: Resource) -> Resource:
	var p: Resource = load(
			"res://addons/gool/resources/gool_material_eq_preset.gd").new()
	p.low_freq_hz  = float(material.get("low_freq_hz"))
	p.low_gain_db  = float(material.get("low_gain_db"))
	p.mid_freq_hz  = float(material.get("mid_freq_hz"))
	p.mid_gain_db  = float(material.get("mid_gain_db"))
	p.mid_q        = float(material.get("mid_q"))
	p.high_freq_hz = float(material.get("high_freq_hz"))
	p.high_gain_db = float(material.get("high_gain_db"))
	return p


## Push this preset's band params onto a GoolAudioMaterial's
## override fields. Caller is responsible for setting
## override_enabled = true and saving the resource if persistence
## is wanted; this method only mutates the in-memory object.
func apply_to_material(material: Resource) -> void:
	material.set("low_freq_hz",  low_freq_hz)
	material.set("low_gain_db",  low_gain_db)
	material.set("mid_freq_hz",  mid_freq_hz)
	material.set("mid_gain_db",  mid_gain_db)
	material.set("mid_q",        mid_q)
	material.set("high_freq_hz", high_freq_hz)
	material.set("high_gain_db", high_gain_db)
