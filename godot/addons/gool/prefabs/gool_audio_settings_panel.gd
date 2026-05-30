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

## v0.37.0 (Phase 4): reference player audio settings panel.
##
## Drop this Control into your pause menu / options scene and it
## handles the rest:
##   - Builds its own UI layout in _ready() (volumes + acoustic
##     settings + reset button)
##   - Loads current settings from disk via GoolAudioSettings.load_from_disk()
##   - Pushes initial values to the Gool autoload via apply_to_runtime()
##   - Wires slider/checkbox signals so changes apply LIVE to the
##     engine (player hears the change immediately)
##   - Debounces saves: applies to engine on every tick, saves to
##     disk on slider drag_ended (no disk hammering during scrubs)
##   - Emits a "closed" signal when the user hits Close, so parent
##     scenes can hide the panel
##
## Sized for "drop in and ship." If you want different styling,
## a different layout, or extra controls (audio device picker,
## subtitles), copy this script and modify — the persistence
## layer (GoolAudioSettings static methods) is what you actually
## want to keep; the UI here is replaceable.
##
## Usage:
##   var panel = preload("res://addons/gool/prefabs/gool_audio_settings_panel.gd").new()
##   add_child(panel)
##   panel.closed.connect(_on_settings_closed)

@tool
class_name GoolAudioSettingsPanel
extends Control

## Emitted when the user hits the Close button. Parent menus
## connect this to hide the panel.
signal closed

# In-memory mirror of the on-disk settings. The UI sliders are
# the visual representation of this dictionary; updates flow
# user-input → _on_*_changed → update this dict → apply to
# engine → (debounced) save to disk.
var _settings: Dictionary = {}

# Cached control refs by setting key, populated in _ready() so
# we can update them without re-walking the tree (e.g., when
# resetting to defaults). Maps "volumes.master_db" → HSlider.
var _slider_controls: Dictionary = {}
var _value_labels: Dictionary = {}
var _checkbox_controls: Dictionary = {}

# Visual config — keep style hooks here so designers can tweak
# without diving into the build code.
const _DB_RANGE_MIN: float = -60.0
const _DB_RANGE_MAX: float =   6.0
const _DB_STEP:      float =   0.5
const _INTENSITY_RANGE_MIN: float = 0.0
const _INTENSITY_RANGE_MAX: float = 2.0
const _INTENSITY_STEP:      float = 0.05
const _ROW_HEIGHT: float = 32.0
const _LABEL_WIDTH: float = 180.0
const _VALUE_LABEL_WIDTH: float = 80.0

func _ready() -> void:
	# Editor tool mode — just show a placeholder. The full build
	# only runs in-game so we don't spam the editor with engine
	# calls or trigger Gool autoload load order issues.
	if Engine.is_editor_hint():
		_build_editor_placeholder()
		return

	# Load settings before building UI — that way slider initial
	# values reflect what's on disk, not the DEFAULTS hard-coded
	# in the script.
	_settings = GoolAudioSettings.load_from_disk()
	await GoolAudioSettings.apply_to_runtime(_settings)

	_build_ui()
	_populate_from_settings()

func _build_editor_placeholder() -> void:
	# Minimal: just a label saying what this is. Avoids the full
	# layout building (which calls into Gool) in editor context.
	var label := Label.new()
	label.text = "GoolAudioSettingsPanel (placeholder — populates in-game)"
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.vertical_alignment   = VERTICAL_ALIGNMENT_CENTER
	label.anchor_right  = 1.0
	label.anchor_bottom = 1.0
	add_child(label)

func _build_ui() -> void:
	# v0.59.1: set a Theme on the panel root with Ubuntu Regular as
	# the default font. Theme inheritance means every Label, Button,
	# SpinBox, HSlider, etc. inside this panel renders in Ubuntu
	# without needing per-control overrides. Skipped silently if the
	# font file is missing (e.g. someone stripped /fonts/ from the
	# addon to save space); descendants fall back to the host theme.
	_apply_gool_font_theme()

	# Outer: scroll container so the panel works in small windows
	# or with future expansion (more sections).
	var scroll := ScrollContainer.new()
	scroll.anchor_right  = 1.0
	scroll.anchor_bottom = 1.0
	scroll.size_flags_horizontal = SIZE_EXPAND_FILL
	scroll.size_flags_vertical   = SIZE_EXPAND_FILL
	add_child(scroll)

	var vbox := VBoxContainer.new()
	vbox.size_flags_horizontal = SIZE_EXPAND_FILL
	vbox.add_theme_constant_override("separation", 8)
	scroll.add_child(vbox)

	# Section: Volumes
	_add_section_header(vbox, "Volumes")
	_add_db_slider_row(vbox, "Master",   "volumes", "master_db")
	_add_db_slider_row(vbox, "SFX",      "volumes", "sfx_db")
	_add_db_slider_row(vbox, "Music",    "volumes", "music_db")
	_add_db_slider_row(vbox, "UI",       "volumes", "ui_db")
	_add_db_slider_row(vbox, "Voice",    "volumes", "voice_db")
	_add_db_slider_row(vbox, "Dialogue", "volumes", "dialogue_db")
	_add_db_slider_row(vbox, "Ambience", "volumes", "ambience_db")

	vbox.add_child(HSeparator.new())

	# Section: Acoustic
	_add_section_header(vbox, "Acoustic")
	_add_checkbox_row(vbox, "Occlusion enabled", "occlusion", "enabled")
	_add_intensity_slider_row(vbox, "Occlusion intensity",
			"occlusion", "intensity")
	_add_intensity_slider_row(vbox, "Material EQ intensity",
			"material_eq", "intensity")

	vbox.add_child(HSeparator.new())

	# Buttons
	var button_row := HBoxContainer.new()
	button_row.alignment = BoxContainer.ALIGNMENT_END
	vbox.add_child(button_row)

	var reset_btn := Button.new()
	reset_btn.text = "Reset to defaults"
	reset_btn.pressed.connect(_on_reset_pressed)
	button_row.add_child(reset_btn)

	var close_btn := Button.new()
	close_btn.text = "Close"
	close_btn.pressed.connect(_on_close_pressed)
	button_row.add_child(close_btn)

func _add_section_header(parent: Node, text: String) -> void:
	var label := Label.new()
	label.text = text
	label.add_theme_font_size_override("font_size", 18)
	parent.add_child(label)

# v0.59.1: build a small Theme with Ubuntu Regular as the
# default_font and assign it to self. Theme inheritance pushes the
# font down to every themed descendant (Labels, Buttons, sliders'
# label parts, etc.) without per-node overrides. ResourceLoader's
# `exists()` check keeps this graceful if someone has stripped the
# /fonts/ subdir to shrink their addon footprint; in that case we
# silently fall back to the host theme.
func _apply_gool_font_theme() -> void:
	const FONT_PATH := "res://addons/gool/fonts/Ubuntu-Regular.ttf"
	if not ResourceLoader.exists(FONT_PATH):
		return
	var font: Font = load(FONT_PATH)
	if font == null:
		return
	var th := Theme.new()
	th.default_font = font
	self.theme = th

func _add_db_slider_row(parent: Node, label_text: String,
						 section: String, key: String) -> void:
	_add_slider_row(parent, label_text, section, key,
			_DB_RANGE_MIN, _DB_RANGE_MAX, _DB_STEP, " dB")

func _add_intensity_slider_row(parent: Node, label_text: String,
								section: String, key: String) -> void:
	_add_slider_row(parent, label_text, section, key,
			_INTENSITY_RANGE_MIN, _INTENSITY_RANGE_MAX,
			_INTENSITY_STEP, "")

func _add_slider_row(parent: Node, label_text: String,
					  section: String, key: String,
					  min_value: float, max_value: float,
					  step: float, value_suffix: String) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size.y = _ROW_HEIGHT
	parent.add_child(row)

	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size.x = _LABEL_WIDTH
	row.add_child(label)

	var slider := HSlider.new()
	slider.min_value = min_value
	slider.max_value = max_value
	slider.step      = step
	slider.size_flags_horizontal = SIZE_EXPAND_FILL
	row.add_child(slider)

	var value_label := Label.new()
	value_label.custom_minimum_size.x = _VALUE_LABEL_WIDTH
	value_label.horizontal_alignment  = HORIZONTAL_ALIGNMENT_RIGHT
	row.add_child(value_label)

	# Track for later access (populate, reset)
	var setting_path := "%s.%s" % [section, key]
	_slider_controls[setting_path] = slider
	_value_labels[setting_path]    = value_label

	# Live-apply on every tick; debounce save to drag_ended.
	# Bound args carry the setting identity to the handler.
	slider.value_changed.connect(_on_slider_value_changed.bind(
			section, key, value_label, value_suffix))
	slider.drag_ended.connect(_on_slider_drag_ended)

func _add_checkbox_row(parent: Node, label_text: String,
						section: String, key: String) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size.y = _ROW_HEIGHT
	parent.add_child(row)

	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size.x = _LABEL_WIDTH
	row.add_child(label)

	var checkbox := CheckBox.new()
	row.add_child(checkbox)

	# Filler so the checkbox doesn't stretch
	var spacer := Control.new()
	spacer.size_flags_horizontal = SIZE_EXPAND_FILL
	row.add_child(spacer)

	var setting_path := "%s.%s" % [section, key]
	_checkbox_controls[setting_path] = checkbox

	checkbox.toggled.connect(_on_checkbox_toggled.bind(section, key))

func _populate_from_settings() -> void:
	# Walk every cached control and set its value from the
	# current _settings dict. Called on _ready (after load) and
	# after reset_to_defaults() rewrites _settings.
	for setting_path in _slider_controls.keys():
		var parts: Array = setting_path.split(".", false, 2)
		var section: String = parts[0]
		var key: String     = parts[1]
		var value: float    = float(_settings[section][key])
		var slider: HSlider = _slider_controls[setting_path]
		slider.value = value
		# Trigger the value label update by manually firing the
		# same path the signal would. (Setting slider.value
		# DOES emit value_changed, which DOES update the label
		# via the connected handler — so actually we don't need
		# to do anything more here. Leaving the comment for
		# anyone reading the code to understand why this looks
		# incomplete.)
	for setting_path in _checkbox_controls.keys():
		var parts: Array = setting_path.split(".", false, 2)
		var section: String = parts[0]
		var key: String     = parts[1]
		var value: bool     = bool(_settings[section][key])
		var checkbox: CheckBox = _checkbox_controls[setting_path]
		# Setting checkbox.button_pressed DOES emit toggled. To
		# avoid persisting on UI-driven population, we disconnect,
		# set, reconnect. Marginal but avoids a spurious save on
		# every menu open.
		checkbox.toggled.disconnect(_get_checkbox_connection(setting_path))
		checkbox.button_pressed = value
		checkbox.toggled.connect(_on_checkbox_toggled.bind(section, key))

func _get_checkbox_connection(_setting_path: String) -> Callable:
	# Reconstructs the bound callable that we connected in
	# _add_checkbox_row, so we can disconnect+reconnect cleanly
	# during _populate_from_settings.
	var parts: Array = _setting_path.split(".", false, 2)
	return _on_checkbox_toggled.bind(parts[0], parts[1])

func _on_slider_value_changed(value: float, section: String, key: String,
							   value_label: Label, suffix: String) -> void:
	# Update in-memory mirror
	_settings[section][key] = value
	# Update the displayed value label
	value_label.text = "%.1f%s" % [value, suffix]
	# Push to the engine IMMEDIATELY so the player hears the
	# change as they drag. Save-to-disk is debounced to drag_ended.
	GoolAudioSettings.apply_to_runtime(_settings)

func _on_slider_drag_ended(_value_changed: bool) -> void:
	# Persist whatever the current settings dict reflects.
	# (Multiple sliders share this same callback — that's fine,
	# the dict is single-source-of-truth.)
	GoolAudioSettings.save_to_disk(_settings)

func _on_checkbox_toggled(button_pressed: bool, section: String,
						   key: String) -> void:
	_settings[section][key] = button_pressed
	GoolAudioSettings.apply_to_runtime(_settings)
	# No drag concept for checkboxes — save immediately. Disk
	# writes from checkboxes are infrequent enough that
	# debouncing isn't needed.
	GoolAudioSettings.save_to_disk(_settings)

func _on_reset_pressed() -> void:
	# Restore factory defaults, then refresh UI to match.
	_settings = await GoolAudioSettings.reset_to_defaults()
	_populate_from_settings()

func _on_close_pressed() -> void:
	closed.emit()
