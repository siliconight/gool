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
# v0.64.0: Master strip is ~35% wider than a submix strip — Phase 2
# of the UI evolution plan. Wider strip carries more visual weight,
# signaling "this is the final output stage." Master also gets a
# blue-purple outline (COLOR_MASTER_OUTLINE on _BusStrip) and an
# all-caps "MASTER" label; per-strip width is the column-sizing
# part of that treatment.
const MASTER_STRIP_WIDTH: float = 130.0
const STRIP_HEIGHT: float = 384.0  # v0.28.3: +22 for FX_BAND below readout. v0.64.0: +22 for ROUTE_BAND below FX_BAND.
const STRIP_GAP: float = 4.0

# v0.52.0: vertical offset per nesting depth for bus hierarchy
# indenting. Each level of nesting (child of a child of root, etc.)
# offsets the strip column down by this many pixels so the topology
# is visible at a glance. Capped at depth 3 in _rebuild_strips_from_config
# so pathologically deep configs don't push strips off-screen.
#
# v0.54.0: defaults to OFF — horizontal mixer rows in pro DAWs
# (FMOD, Wwise, Pro Tools, REAPER) keep strips flat and put
# hierarchy in a separate tree pane. The v0.52.0 default of "on"
# made multi-level configs look misaligned. Now gated behind the
# toolbar's [Tree] CheckButton; preference persists via EditorSettings.
const HIERARCHY_INDENT_PX: float = 28.0

# v0.54.0: EditorSettings key for persisting the hierarchy-mode
# toggle across editor sessions. Scoped under gool/mixer_dock/ so
# future per-dock prefs can sit alongside.
const SETTING_HIERARCHY_MODE: String = "gool/mixer_dock/hierarchy_mode_tree"

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

# v0.51.0: theme-aware chrome colors. Custom-drawn meters keep their
# literal palette (audio convention: green/yellow/red at fixed dB
# thresholds is the same in every DAW regardless of editor theme).
# But the dock's chrome — toolbar, empty state, stats panel, dividers —
# should follow the editor theme so the dock looks integrated rather
# than grafted on.
#
# Each helper returns the editor-theme color if available, falling
# back to the hardcoded dark-theme equivalent. Safe to call before
# EditorInterface is ready (returns fallback).
func _theme_color(name: String, fallback: Color) -> Color:
	if not Engine.is_editor_hint():
		return fallback
	var theme := EditorInterface.get_editor_theme() if EditorInterface else null
	if theme == null:
		return fallback
	if theme.has_color(name, "Editor"):
		return theme.get_color(name, "Editor")
	return fallback

func _theme_accent() -> Color:
	return _theme_color("accent_color", Color(0.42, 0.65, 0.95))

func _theme_panel_bg() -> Color:
	return _theme_color("dark_color_1", Color(0.13, 0.14, 0.16))

func _theme_panel_bg_alt() -> Color:
	return _theme_color("dark_color_2", Color(0.16, 0.17, 0.20))

func _theme_separator() -> Color:
	return _theme_color("contrast_color_1", Color(0.30, 0.32, 0.36))

func _theme_text_primary() -> Color:
	return _theme_color("font_color", Color(0.88, 0.90, 0.93))

func _theme_text_secondary() -> Color:
	return _theme_color("font_color_disabled", Color(0.55, 0.58, 0.62))

# Build a flat, themed StyleBox suitable for toolbar/panel chrome.
# Slightly raised feel (no shadow, just a clean fill + subtle border)
# matching modern editor tools. Used by the v0.51.0 toolbar + empty
# state + stats card backgrounds.
func _build_chrome_stylebox(extra_padding: int = 8) -> StyleBoxFlat:
	var sb := StyleBoxFlat.new()
	sb.bg_color = _theme_panel_bg_alt()
	sb.border_color = _theme_separator()
	sb.border_width_left = 1
	sb.border_width_right = 1
	sb.border_width_top = 1
	sb.border_width_bottom = 1
	sb.corner_radius_top_left = 4
	sb.corner_radius_top_right = 4
	sb.corner_radius_bottom_left = 4
	sb.corner_radius_bottom_right = 4
	sb.content_margin_left = extra_padding
	sb.content_margin_right = extra_padding
	sb.content_margin_top = max(4, extra_padding - 2)
	sb.content_margin_bottom = max(4, extra_padding - 2)
	return sb

# v0.51.0: build the empty-state onboarding card. Returns a Control
# (CenterContainer) that takes the full strip area and centers a
# card inside it. The card holds a heading, body copy, and two
# action buttons: "Create default config" / "Use FPS template".
#
# References to the heading + body labels are stashed in metadata
# so _set_empty_state_message() can update the text without
# rebuilding the card.
func _build_empty_state_card() -> Control:
	var center := CenterContainer.new()
	center.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	center.size_flags_vertical = Control.SIZE_EXPAND_FILL
	center.custom_minimum_size = Vector2(360, 200)

	var card := PanelContainer.new()
	card.add_theme_stylebox_override("panel", _build_chrome_stylebox(20))
	center.add_child(card)

	var vbox := VBoxContainer.new()
	vbox.add_theme_constant_override("separation", 12)
	card.add_child(vbox)

	var heading := Label.new()
	heading.text = "No config found"
	heading.add_theme_color_override("font_color", _theme_text_primary())
	heading.add_theme_font_size_override("font_size", 16)
	heading.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	vbox.add_child(heading)

	var body := Label.new()
	body.text = (
			"gool needs a res://gool/config.json to define its bus topology. "
			+ "Create one to get started.")
	body.add_theme_color_override("font_color", _theme_text_secondary())
	body.add_theme_font_size_override("font_size", 12)
	body.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	body.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	body.custom_minimum_size = Vector2(320, 0)
	vbox.add_child(body)

	# Small spacer before the action row
	var spacer := Control.new()
	spacer.custom_minimum_size = Vector2(0, 4)
	vbox.add_child(spacer)

	var action_row := HBoxContainer.new()
	action_row.add_theme_constant_override("separation", 8)
	action_row.alignment = BoxContainer.ALIGNMENT_CENTER
	vbox.add_child(action_row)

	var create_default_btn := Button.new()
	create_default_btn.text = "Create default config"
	create_default_btn.tooltip_text = (
			"Write a minimal gool/config.json with Master + Music + Sfx + "
			+ "Voice buses. Good starting point for any project.")
	create_default_btn.pressed.connect(_on_create_default_config_pressed)
	action_row.add_child(create_default_btn)

	var use_fps_btn := Button.new()
	use_fps_btn.text = "Use FPS template"
	use_fps_btn.tooltip_text = (
			"Copy addons/gool/templates/config_fps.json into res://gool/. "
			+ "Includes the v0.47.0 reverb chain + sidechain ducking pre-wired "
			+ "for FPS-style mixes.")
	use_fps_btn.pressed.connect(_on_use_fps_template_pressed)
	action_row.add_child(use_fps_btn)

	# Cache references on metadata so _set_empty_state_message can
	# find them without traversing the tree.
	center.set_meta("heading_label", heading)
	center.set_meta("body_label", body)
	return center

# v0.51.0: update the empty-state card's title + body without
# rebuilding. Safe to call before _ready() completes; falls through
# to nothing if the card isn't built yet.
func _set_empty_state_message(heading_text: String, body_text: String) -> void:
	if _empty_label == null:
		return
	if _empty_label.has_meta("heading_label"):
		var h: Label = _empty_label.get_meta("heading_label")
		if h != null:
			h.text = heading_text
	if _empty_label.has_meta("body_label"):
		var b: Label = _empty_label.get_meta("body_label")
		if b != null:
			b.text = body_text

# v0.51.0: action handlers for the empty-state buttons.
#
# Create default config writes a minimal viable bus topology. It's
# small but valid — Master / Music / Sfx / Voice — so the dock can
# render strips immediately and the user can iterate.
#
# Use FPS template copies the shipping config_fps.json into the
# project. Adds the full reverb chain + sidechain ducking + Dialogue
# routing recommended for FPS projects.
func _on_create_default_config_pressed() -> void:
	var path: String = "res://gool/config.json"
	var dir := DirAccess.open("res://")
	if dir == null:
		push_error("[gool] mixer dock: cannot open res:// for writing")
		return
	if not dir.dir_exists("gool"):
		dir.make_dir("gool")
	var cfg: Dictionary = {
		"sample_rate": 48000,
		"buffer_size": 512,
		"buses": [
			{ "name": "Master", "gain_db": 0.0 },
			{ "name": "Music",  "parent": "Master", "gain_db": -3.0 },
			{ "name": "Sfx",    "parent": "Master", "gain_db": 0.0 },
			{ "name": "Voice",  "parent": "Master", "gain_db": 0.0 },
		],
		"category_routing": {
			"music": "Music",
			"sfx": "Sfx",
			"voice": "Voice",
		},
	}
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		push_error("[gool] mixer dock: cannot open %s for writing" % path)
		return
	f.store_string(JSON.stringify(cfg, "  "))
	f.close()
	print("[gool] mixer dock: wrote default config to %s" % path)
	# Reload the model from disk so the dock rebuilds.
	if _config_model != null:
		_config_model.load_from_disk()
	_load_static_layout_from_config()

func _on_use_fps_template_pressed() -> void:
	var src: String = "res://addons/gool/templates/config_fps.json"
	var dst: String = "res://gool/config.json"
	if not FileAccess.file_exists(src):
		push_warning("[gool] mixer dock: FPS template not found at %s" % src)
		return
	var dir := DirAccess.open("res://")
	if dir == null:
		push_error("[gool] mixer dock: cannot open res:// for writing")
		return
	if not dir.dir_exists("gool"):
		dir.make_dir("gool")
	# v0.80.8: byte-for-byte copy via DirAccess.copy(). Pre-v0.80.8
	# this read the template via FileAccess.get_file_as_string and
	# wrote via store_string, which converts to platform-default
	# line endings (CRLF on Windows). The parser handles both fine,
	# but byte-for-byte consistency means anyone diffing their on-
	# disk config against the canonical upstream template sees a
	# clean diff instead of every line flagged changed.
	var copy_err: int = dir.copy(src, dst)
	if copy_err != OK:
		push_error("[gool] mixer dock: cannot copy %s → %s (err=%d)"
				% [src, dst, copy_err])
		return
	print("[gool] mixer dock: copied FPS template → %s" % dst)
	if _config_model != null:
		_config_model.load_from_disk()
	_load_static_layout_from_config()

# Meter display range — minimum dBFS visible.
const DB_METER_FLOOR: float = -60.0
const DB_METER_CEIL: float = 6.0

# Path to the project's gool config.json. Editor reads this at
# project-load time to build the static strip layout.
const CONFIG_PATH: String = "res://gool/config.json"


# ---- State -----------------------------------------------------------

var _strip_container: HBoxContainer = null
var _empty_label: Control = null
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
# v0.53.0: TabContainer at the dock root. Hosts the Mixer tab
# (everything from v0.52.0 and earlier) plus the new Sound Bank
# tab. Future tabs (Materials, Bus Hierarchy view, etc.) hang
# off this same container.
var _tab_container: TabContainer = null
# v0.53.0: Sound Bank panel instance. Lazily loaded so a parse
# failure in sound_bank_panel.gd doesn't take down the dock.
# Type left as Node (Variant) rather than a concrete class so
# we don't have to forward-declare the class name.
var _sound_bank_panel: Node = null
# v0.54.0: hierarchy display mode. False = flat (default, matches
# pro DAW convention); True = tree (children indented under
# parents, the v0.52.0 default). Toggled via the toolbar CheckButton
# and persisted to EditorSettings under SETTING_HIERARCHY_MODE.
var _hierarchy_mode_tree: bool = false
# v0.54.0: reference to the toolbar's hierarchy CheckButton, cached
# so the button's pressed state can be set programmatically when
# the saved pref is loaded.
var _hierarchy_toggle: CheckButton = null

# v0.44.0: Live Stats panel — compact health-monitoring strip
# below the bus strips showing engine-wide observability data
# (voice count, master peak, dropouts, per-player VOIP jitter).
# Always-visible at the bottom of the dock when an F5 session is
# providing data; shows "—" placeholders otherwise. See the
# v0.44.0 CHANGELOG entry for the rationale: "Stats are exposed
# but a non-audio Godot dev won't build their own visualizer."
var _stats_panel: PanelContainer = null
# Map of label key → Label node. Keys: "voices", "emitters",
# "master_peak", "peak_amplitude", "drops". Updated each _poll.
var _stats_labels: Dictionary = {}
# v0.51.0: parallel map of card key → Control (the per-stat
# card containing the label + value + optional meter/sparkline).
# Used by _update_card_meter / _update_card_sparkline to find the
# child widgets without tree traversal.
var _stats_cards: Dictionary = {}
# v0.51.0: "LIVE" leader label on the stats row. Color pulses to
# accent when data is flowing, dims when poll returns empty.
var _stats_leader: Label = null
# Voice-chat health sub-row. Hidden when no voice players are
# registered; populated from the engine's voice_chat dict in
# render_stats. Children are dynamically rebuilt when the set of
# registered players changes.
var _voice_chat_row: HBoxContainer = null
# Cached set of player_ids whose voice-chat labels are currently
# built into _voice_chat_row. Reused to detect player-set changes
# so we only rebuild children when the set actually changes (not
# every 30 Hz poll).
var _voice_chat_players_cached: PackedInt32Array = PackedInt32Array()


# ---- Plugin lifecycle ------------------------------------------------

func set_debugger_plugin(p: EditorDebuggerPlugin) -> void:
	_debugger_plugin = p


func _ready() -> void:
	custom_minimum_size = Vector2(0, STRIP_HEIGHT + 24)

	# v0.53.0: TabContainer at the dock root, hosting:
	#   1. "Mixer" — the existing toolbar + strip area + stats panel
	#      (formerly the direct contents of the dock)
	#   2. "Sound Bank" — the new sound bank browser panel
	#
	# TabContainer uses each direct child Node's .name property as
	# the tab label, so we set them explicitly. Future tabs (Bus
	# Hierarchy view, Materials catalog, etc.) hang off this same
	# TabContainer.
	_tab_container = TabContainer.new()
	_tab_container.anchor_right = 1.0
	_tab_container.anchor_bottom = 1.0
	_tab_container.tab_changed.connect(_on_tab_changed)
	add_child(_tab_container)

	var root_vbox := VBoxContainer.new()
	root_vbox.name = "Mixer"
	root_vbox.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	root_vbox.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_tab_container.add_child(root_vbox)

	# v0.79.2: Update-available banner. Self-hides unless the
	# checker emits an "update available" signal. The checker queries
	# GitHub's releases API (cached 24h) and emits the signal on the
	# main thread. Output: a one-line banner above the getting-
	# started banner if a newer stable release exists, plus a log
	# line in the editor console for users who never open this dock.
	# Opt-out via Project Settings → audio/gool/check_for_updates.
	var update_banner_script := load(
			"res://addons/gool/editor/update_available_banner.gd")
	var checker_script := load(
			"res://addons/gool/editor/update_checker.gd")
	if update_banner_script != null and checker_script != null:
		var update_banner: PanelContainer = PanelContainer.new()
		update_banner.set_script(update_banner_script)
		update_banner.visible = false
		root_vbox.add_child(update_banner)
		var checker: RefCounted = checker_script.new()
		var current_version := _get_current_gool_version()
		checker.update_check_complete.connect(
			func(latest: String, is_newer: bool) -> void:
				if not is_newer or latest == "":
					return
				if is_instance_valid(update_banner):
					update_banner.show_for(latest, current_version)
				# Always log to console for users who don't open
				# the mixer dock. ANSI-tolerant; Godot's output
				# panel renders this as a normal print line.
				print("[gool] update available: v%s (you have v%s). %s"
					% [latest, current_version,
					   "https://github.com/siliconight/gool/releases"])
		)
		# Use the editor's scene root as the HTTPRequest parent.
		# get_tree() works in @tool scripts when added to the scene.
		var tree := get_tree()
		if tree != null and tree.root != null:
			checker.check(current_version, tree.root)

	# v0.72.0: Getting Started banner. Self-decides whether to show
	# based on whether res://gool/config.json exists and whether the
	# user has dismissed it. No-op visually for any project that
	# already has a config, so this is safe to mount unconditionally.
	var banner_script := load(
			"res://addons/gool/editor/getting_started_banner.gd")
	if banner_script != null:
		var banner: PanelContainer = PanelContainer.new()
		banner.set_script(banner_script)
		# Banner goes ABOVE the toolbar (first child of root_vbox)
		# so it's the first thing the user sees if they need it.
		root_vbox.add_child(banner)

	# v0.51.0: themed toolbar — banner with a title on the left, the
	# Save Mix to Config action on the right. Replaces the v0.48.0
	# plain HBox of [Button + Label] which used Godot defaults and
	# stuck out as unstyled vs the custom-drawn strips below.
	#
	# Visual recipe:
	#   - PanelContainer with chrome stylebox (matches editor theme)
	#   - Section title "Mixer" + monospace path subtitle (de-emphasized)
	#   - Spacer takes remaining width
	#   - Save button right-aligned, themed flat
	var toolbar_panel := PanelContainer.new()
	toolbar_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	toolbar_panel.add_theme_stylebox_override("panel", _build_chrome_stylebox(10))
	root_vbox.add_child(toolbar_panel)

	var toolbar := HBoxContainer.new()
	toolbar.add_theme_constant_override("separation", 12)
	toolbar_panel.add_child(toolbar)

	# Left: title block (heading + monospace path)
	var title_block := VBoxContainer.new()
	title_block.add_theme_constant_override("separation", 2)
	toolbar.add_child(title_block)

	var title_label := Label.new()
	title_label.text = "Mixer"
	title_label.add_theme_color_override("font_color", _theme_text_primary())
	title_label.add_theme_font_size_override("font_size", 14)
	title_block.add_child(title_label)

	var path_label := Label.new()
	path_label.text = "res://gool/config.json"
	path_label.add_theme_color_override("font_color", _theme_text_secondary())
	path_label.add_theme_font_size_override("font_size", 11)
	# Monospace gives the path a "code" feel that scans as
	# "file path, not prose" — same convention Godot's own
	# inspector uses for resource paths.
	var mono := ThemeDB.fallback_font
	if mono != null:
		path_label.add_theme_font_override("font", mono)
	title_block.add_child(path_label)

	# Spacer pushes the action button to the right edge.
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	toolbar.add_child(spacer)

	# v0.54.0: hierarchy display toggle. CheckButton "Tree" — checked
	# means "indent child buses under parents (v0.52.0 behavior)";
	# unchecked is the new default "flat row" mode that matches pro
	# DAW convention. Preference persists via EditorSettings so it
	# survives editor restarts.
	_hierarchy_mode_tree = _load_hierarchy_pref()
	_hierarchy_toggle = CheckButton.new()
	_hierarchy_toggle.text = "Tree"
	_hierarchy_toggle.button_pressed = _hierarchy_mode_tree
	_hierarchy_toggle.focus_mode = Control.FOCUS_NONE
	_hierarchy_toggle.tooltip_text = (
			"Tree mode: indent child buses under parents to visualize "
			+ "the bus topology.\n\nFlat mode (default): strips in a "
			+ "single horizontal row, matching pro DAW convention.\n\n"
			+ "Preference is saved across editor sessions.")
	_hierarchy_toggle.toggled.connect(_on_hierarchy_toggle_toggled)
	toolbar.add_child(_hierarchy_toggle)

	# Right: themed save button. flat=true + manual stylebox keeps
	# the focus on the strips below rather than the toolbar shouting.
	var save_button := Button.new()
	save_button.text = "Save Mix to Config"
	save_button.tooltip_text = (
			"Write the current model state to res://gool/config.json "
			+ "as a clean full rewrite. Most edits auto-save in the "
			+ "background — use this when you want to force a clean "
			+ "rewrite or recover from external edits."
	)
	save_button.pressed.connect(_on_save_mix_to_config_pressed)
	toolbar.add_child(save_button)

	# v0.79.3: Help button. Small "?" icon-style button that opens
	# the help panel (categorized list of keyboard shortcuts, editor
	# tools, runtime API, diagnostics, links). Flat styling so it
	# doesn't compete visually with Save.
	var help_button := Button.new()
	help_button.text = "?"
	help_button.flat = true
	help_button.tooltip_text = ("Open gool help — keyboard shortcuts, "
			+ "editor tools, runtime API quick reference, diagnostics, "
			+ "and links to the full docs on GitHub.")
	help_button.pressed.connect(_on_help_button_pressed)
	toolbar.add_child(help_button)

	var scroll := ScrollContainer.new()
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	root_vbox.add_child(scroll)

	_strip_container = HBoxContainer.new()
	_strip_container.add_theme_constant_override("separation", int(STRIP_GAP))
	scroll.add_child(_strip_container)

	# v0.51.0: themed empty state. Replaces the bare-Label
	# "No gool/config.json found" with a centered card containing
	# a heading, body copy, and two action buttons. First-time
	# users land here; the actions need to be obvious + recoverable.
	_empty_label = _build_empty_state_card()
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
	# v0.80.12: parallel handler for the externally-removed-file case.
	_config_model.external_removal_detected.connect(
			_on_model_external_removal_detected)
	# v0.28.8 topology signals.
	_config_model.topology_changed.connect(_on_model_topology_changed)
	_config_model.bus_added.connect(_on_model_bus_added)
	_config_model.bus_removed.connect(_on_model_bus_removed)
	# v0.80.17: bus rename signal — handler propagates the rename to
	# ProjectSettings bus-name strings that the model itself doesn't
	# own (material EQ impact/listener bus pointers).
	_config_model.bus_renamed.connect(_on_model_bus_renamed)
	var load_err: int = _config_model.load_from_disk()
	if load_err != OK and load_err != ERR_FILE_NOT_FOUND:
		push_warning("[gool] config_model load_from_disk: error %d" % load_err)

	# v0.26.0: build static layout from config.json IMMEDIATELY.
	# Strips are visible at editor time; live data comes later
	# when F5 starts (via debugger plugin).
	_load_static_layout_from_config()

	# v0.44.0: Live Stats panel below the strip area. Always built
	# (so the layout is stable regardless of whether F5 is running);
	# shows "—" placeholders when no data is available.
	_stats_panel = _build_stats_panel()
	root_vbox.add_child(_stats_panel)

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

	# v0.53.0: instantiate the Sound Bank panel as the second tab.
	# Loaded dynamically rather than `preload`ed so a parse-time
	# issue in sound_bank_panel.gd doesn't take down the whole dock.
	# If the load fails (file missing, parse error), the Mixer tab
	# still works and we log a warning.
	var bank_script := load("res://addons/gool/editor/sound_bank_panel.gd")
	if bank_script != null:
		_sound_bank_panel = bank_script.new()
		_sound_bank_panel.name = "Sound Bank"
		_sound_bank_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_sound_bank_panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
		_tab_container.add_child(_sound_bank_panel)
	else:
		push_warning("[gool] mixer dock: sound_bank_panel.gd "
				+ "failed to load — Mixer tab still available")


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
	# v0.29.1 fix: read from the in-memory model, NOT from disk.
	#
	# Model saves are debounced, so when bus_added / bus_removed fires
	# immediately on add/remove, the disk file still has the OLD
	# content. Reading config.json here would rebuild the strip row
	# from stale bytes, hiding the user's change until the debounced
	# save flushed (which, in practice, didn't visibly happen until
	# something else forced a rebuild — like starting the game and
	# letting the runtime poll trigger _rebuild_strips_from_runtime).
	#
	# Fix is to use _config_model.get_buses(), which returns the
	# authoritative in-memory parsed buses array. Model.load_from_disk
	# was already called in _ready before this function fires for the
	# first time, so this is safe at startup too.
	if _config_model == null:
		_set_empty_state_message(
				"No config found",
				"gool needs a res://gool/config.json to define its bus topology. "
				+ "Create one to get started.")
		_empty_label.visible = true
		return
	var buses: Array = _config_model.get_buses()
	if buses.is_empty():
		_set_empty_state_message(
				"Config is empty",
				"res://gool/config.json was loaded but has no buses defined. "
				+ "Add buses to see them here.")
		_empty_label.visible = true
		return
	_rebuild_strips_from_config(buses)
	_last_bus_names = _strip_names_packed(buses)


# _read_buses_from_config was the v0.28.4–v0.29.0 implementation that
# parsed config.json directly. Removed in v0.29.1: the in-memory model
# is the single source of truth for at-rest layout decisions. Disk
# loading is the model's job (load_from_disk / reload_from_disk_discarding_edits).


func _strip_names_packed(buses: Array) -> PackedStringArray:
	var out := PackedStringArray()
	for b in buses:
		out.append(String(b.get("name", "")))
	return out


func _rebuild_strips_from_config(buses: Array) -> void:
	_empty_label.visible = false
	# v0.28.9: clear EVERY child of _strip_container, not just the
	# tracked _columns. The "+ Add Bus" trailing column is added by
	# this function but intentionally not tracked in _columns, so
	# freeing only _columns leaked one Add Bus column per rebuild.
	# v0.28.10: ...except _empty_label, which is also a child of
	# _strip_container (added once in _ready) but must persist
	# across rebuilds. Pre-v0.28.10 nuked it, causing the next
	# rebuild to access a freed object.
	for child in _strip_container.get_children():
		if child == _empty_label:
			continue
		child.queue_free()
	_strips.clear()
	_columns.clear()
	_expanded_bus = ""

	# v0.52.0: hierarchy-aware ordering + indenting. Buses in
	# config.json can be in any order; we re-sort so parents appear
	# before their children (topological order). Then each column's
	# top padding is set to depth × INDENT_PX so child strips visibly
	# "hang" below their parents in the horizontal layout, making
	# the bus topology readable at a glance.
	#
	# Example: with Master / Sfx (child of Master) / LocalSfx (child
	# of Sfx) / RemoteSfx (child of Sfx) / Music (child of Master):
	#
	#  ┌Master┐  ┌Music ┐
	#  │      │  │      │
	#  │ ─    │  │ ─    │
	#  └──────┘  └──────┘
	#              ┌ Sfx ┐
	#              │     │
	#              │ ─   │
	#              └─────┘
	#                       ┌LocalSfx┐  ┌RemoteSfx┐
	#                       │        │  │         │
	#                       │   ─    │  │    ─    │
	#                       └────────┘  └─────────┘
	# v0.54.0: only sort + compute depths when tree mode is on. Flat
	# mode uses config.json order verbatim with no padding — strips
	# render in a single horizontal row.
	var sorted_buses: Array
	var depth_map: Dictionary
	if _hierarchy_mode_tree:
		sorted_buses = _topologically_sort_buses(buses)
		depth_map = _compute_bus_depths(buses)
	else:
		sorted_buses = buses
		depth_map = {}

	for d in sorted_buses:
		var bus_name: String = String(d.get("name", "(unnamed)"))
		var initial_db: float = float(d.get("gain_db", 0.0))
		# v0.64.0: routing-related state. is_master is true when the
		# bus has no parent field in config. parent_string is the
		# parent's name (empty for root). The strip uses both — see
		# _BusStrip.set_parent_name / set_is_master.
		var parent_string: String = String(d.get("parent", ""))
		var is_master_bus: bool = parent_string == ""
		var strip := _BusStrip.new()
		strip.bus_name = bus_name
		strip.set_fader_db(initial_db, false)  # silent (no signal emit)
		strip.set_parent_name(parent_string)
		strip.set_is_master(is_master_bus)
		# v0.64.0 Phase 2: Master strip is wider than submix strips.
		var strip_w: float = MASTER_STRIP_WIDTH if is_master_bus else STRIP_WIDTH
		strip.custom_minimum_size = Vector2(strip_w, STRIP_HEIGHT)
		strip.db_changed.connect(_on_strip_db_changed)
		# v0.27.0: S/M/B signal forwarding.
		strip.mute_changed.connect(_on_strip_mute_changed)
		strip.solo_changed.connect(_on_strip_solo_changed)
		strip.bypass_changed.connect(_on_strip_bypass_changed)
		# v0.28.3: Fx toggle.
		strip.fx_toggled.connect(_on_strip_fx_toggled)
		# v0.28.8 context menu (right-click) for bus topology ops.
		strip.context_menu_requested.connect(_on_strip_context_menu_requested)
		if _config_model != null:
			# v0.64.0 Phase 5: surface effect kind_abbrev to the
			# strip for chain pill rendering. set_effect_count is
			# kept as a parallel call because the strip's renderer
			# falls back to "Fx (N)" when abbrevs is empty.
			var effects: Array = _config_model.get_effects(bus_name)
			strip.set_effect_count(effects.size())
			strip.set_effect_abbrevs(_extract_effect_abbrevs(effects))
		# Wrap in a column so the effects panel can stack below.
		var col := VBoxContainer.new()
		col.add_theme_constant_override("separation", 4)

		# v0.52.0: hierarchy indenting. Only applied in tree mode
		# (v0.54.0). depth 0 (root) = no offset; depth 1 = one level
		# of vertical hang; etc. Capped at depth 3 to prevent runaway
		# indent in pathologically deep configs.
		if _hierarchy_mode_tree:
			var depth: int = clampi(int(depth_map.get(bus_name, 0)), 0, 3)
			if depth > 0:
				var top_pad := Control.new()
				top_pad.custom_minimum_size = Vector2(0, depth * HIERARCHY_INDENT_PX)
				top_pad.mouse_filter = Control.MOUSE_FILTER_IGNORE
				col.add_child(top_pad)

		col.add_child(strip)
		_strip_container.add_child(col)
		_strips.append(strip)
		_columns.append(col)
	# v0.28.8: trailing "+ Add Bus" column. Not added to _strips/_columns
	# since it doesn't represent a real bus — it's a UI affordance only.
	_strip_container.add_child(_build_add_bus_column())


# v0.52.0: compute the parent-chain depth for each bus. Depth 0
# means root (no parent or parent missing from the bus list).
# Walks parent references up to a max-depth guard against cycles.
func _compute_bus_depths(buses: Array) -> Dictionary:
	# Build a name → parent_name map first so we don't re-scan the
	# bus list for every depth lookup.
	var parent_map: Dictionary = {}
	var known_names: Dictionary = {}
	for d in buses:
		if not (d is Dictionary):
			continue
		var bus_name: String = String(d.get("name", ""))
		known_names[bus_name] = true
		var parent_name: String = String(d.get("parent", ""))
		if not parent_name.is_empty():
			parent_map[bus_name] = parent_name

	var depths: Dictionary = {}
	for d in buses:
		if not (d is Dictionary):
			continue
		var bus_name: String = String(d.get("name", ""))
		var depth: int = 0
		var current: String = bus_name
		# Walk up the parent chain. Guard against cycles (depth > 16
		# means something's wrong with the config; bail).
		while depth < 16:
			var parent_name: String = String(parent_map.get(current, ""))
			# Parent missing from the bus list = effectively root.
			# This handles the common case where parent="Master" but
			# Master isn't a real bus in the user's config (it's a
			# convention).
			if parent_name.is_empty() or not known_names.has(parent_name):
				break
			depth += 1
			current = parent_name
		depths[bus_name] = depth
	return depths


# v0.52.0: topologically sort buses so parents come before children.
# Stable: within the same depth, preserves config.json order.
# Buses with missing parents are treated as roots.
func _topologically_sort_buses(buses: Array) -> Array:
	var depths: Dictionary = _compute_bus_depths(buses)
	# Pair each bus dict with its depth, then sort. Stable sort keeps
	# original-order siblings together within each depth level.
	var pairs: Array = []
	for i in range(buses.size()):
		var d = buses[i]
		var bus_name: String = String(d.get("name", "")) if d is Dictionary else ""
		var depth: int = int(depths.get(bus_name, 0))
		pairs.append({"idx": i, "depth": depth, "data": d})
	# Sort by depth ascending. Tie-break by original index to
	# preserve config.json order for siblings.
	pairs.sort_custom(func(a, b):
		if a.depth != b.depth:
			return a.depth < b.depth
		return a.idx < b.idx
	)
	var out: Array = []
	for p in pairs:
		out.append(p.data)
	return out


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
			var effects_at_rest: Array = []
			if _config_model != null:
				effects_at_rest = _config_model.get_effects(s.bus_name)
				count_at_rest = effects_at_rest.size()
			s.set_effect_count(count_at_rest)
			# v0.64.0 Phase 5: also push abbrevs so the chain pills
			# render in the dock at rest (not just during F5).
			s.set_effect_abbrevs(_extract_effect_abbrevs(effects_at_rest))
		# v0.44.0: reset Live Stats labels to placeholders when no
		# F5 session is providing data. Without this, the panel
		# would keep showing stale numbers from the previous session.
		_update_stats_panel()
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
		# v0.64.0 Phase 5: push abbrevs so the chain pills stay
		# in sync if effects are added/removed in a running game.
		_strips[i].set_effect_abbrevs(_extract_effect_abbrevs(effects_arr))

	# v0.44.0: refresh the Live Stats panel from the render-stats
	# debugger channel. Cheap — just label.text writes; only the
	# voice-chat row does any child manipulation, and only when the
	# set of registered players changes (which is rare).
	_update_stats_panel()


# Runtime topology differs from config — build strips from runtime
# stats. Fader values default to 0 dB since runtime stats don't
# currently include them (a future addition could).
func _rebuild_strips_from_runtime(stats: Array) -> void:
	_empty_label.visible = false
	# v0.28.10: skip _empty_label (see _rebuild_strips_from_config).
	for child in _strip_container.get_children():
		if child == _empty_label:
			continue
		child.queue_free()
	_strips.clear()
	_columns.clear()
	_expanded_bus = ""
	# v0.64.0: build a parent-index → bus-name map so each strip's
	# route footer can show "→ Music" / "→ Master" / etc. Runtime
	# stats encode parent as an int (-1 == root) per get_bus_stats
	# in gool_godot.cpp; this loop translates that to a name string
	# the strip can render directly.
	var name_by_index: Dictionary = {}
	for i in range(stats.size()):
		var s: Variant = stats[i]
		if s is Dictionary:
			name_by_index[i] = String((s as Dictionary).get("name", ""))
	for i in range(stats.size()):
		var d: Variant = stats[i]
		if not (d is Dictionary):
			continue
		var d_dict: Dictionary = d as Dictionary
		var bus_name: String = String(d_dict.get("name", "(unnamed)"))
		var parent_idx: int = int(d_dict.get("parent", -1))
		var parent_string: String = ""
		var is_master_bus: bool = false
		if parent_idx < 0:
			is_master_bus = true
		elif name_by_index.has(parent_idx):
			parent_string = String(name_by_index[parent_idx])
		# else: parent index set but unresolved → leaves
		# parent_string=="" and is_master=false, which the strip
		# renders as "→ (unrouted)" in warning color. Shouldn't
		# happen with sane runtime stats but defensively handled.
		var strip := _BusStrip.new()
		strip.bus_name = bus_name
		strip.set_fader_db(0.0, false)
		strip.set_parent_name(parent_string)
		strip.set_is_master(is_master_bus)
		# v0.64.0 Phase 2: Master strip is wider than submix strips.
		var strip_w: float = MASTER_STRIP_WIDTH if is_master_bus else STRIP_WIDTH
		strip.custom_minimum_size = Vector2(strip_w, STRIP_HEIGHT)
		strip.db_changed.connect(_on_strip_db_changed)
		# v0.27.0: S/M/B signal forwarding (matches _rebuild_strips_from_config).
		strip.mute_changed.connect(_on_strip_mute_changed)
		strip.solo_changed.connect(_on_strip_solo_changed)
		strip.bypass_changed.connect(_on_strip_bypass_changed)
		# v0.28.3: Fx toggle.
		strip.fx_toggled.connect(_on_strip_fx_toggled)
		# v0.28.8: context menu for bus topology ops, parity with the
		# static-config build path.
		strip.context_menu_requested.connect(_on_strip_context_menu_requested)
		# v0.64.0 Phase 5: per-effect abbreviations from runtime stats.
		# The "effects" field on each bus stat carries kind_abbrev
		# from the engine binding (gool_godot.cpp::get_bus_effects).
		var effects_v: Variant = d_dict.get("effects", [])
		if effects_v is Array:
			var effects_arr: Array = effects_v as Array
			strip.set_effect_count(effects_arr.size())
			strip.set_effect_abbrevs(_extract_effect_abbrevs(effects_arr))
		var col := VBoxContainer.new()
		col.add_theme_constant_override("separation", 4)
		col.add_child(strip)
		_strip_container.add_child(col)
		_strips.append(strip)
		_columns.append(col)
	# v0.28.8: + Add Bus column (parity with _rebuild_strips_from_config).
	_strip_container.add_child(_build_add_bus_column())


# v0.64.0 Phase 5: helper to turn an effects array (one dict per
# effect, as emitted by either _config_model.get_effects or the
# runtime substrate's bus_stats[i].effects) into a PackedStringArray
# of abbreviations ready for _BusStrip.set_effect_abbrevs.
#
# Each effect dict carries a "kind_abbrev" field after v0.64.0 (added
# to gool_godot.cpp _gool_effect_kind_abbreviation and to config_model
# KIND_INT_TO_ABBREV). Falls back to "FX" if the field is missing,
# which only happens with a stale runtime/binding mismatch.
func _extract_effect_abbrevs(effects: Array) -> PackedStringArray:
	var out: PackedStringArray = PackedStringArray()
	for e in effects:
		if e is Dictionary:
			out.append(String((e as Dictionary).get("kind_abbrev", "FX")))
		else:
			out.append("FX")
	return out


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
		# v0.28.8 topology hooks.
		panel.add_effect_requested.connect(_on_panel_add_effect_requested)
		panel.remove_effect_requested.connect(_on_panel_remove_effect_requested)
		panel.move_effect_requested.connect(_on_panel_move_effect_requested)
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


# ===================================================================
# v0.28.8 Phase 3.3d: topology operations
# ===================================================================
#
# Three categories of handlers:
#
#   1. Panel-side signals (add/remove/move effect button presses) →
#      call into _config_model, which validates and re-serializes.
#
#   2. Model topology signals (topology_changed, bus_added,
#      bus_removed) → rebuild the affected UI: just the open Fx
#      panel for an effect change, or the whole strip row for a
#      bus change.
#
#   3. Strip context-menu signal (right-click on strip) → show the
#      "Remove bus..." PopupMenu; on selection, gather refs and
#      either block with an error dialog or confirm with a dialog.

# --- Effect topology: _EffectsPanel signal handlers ---

func _on_panel_add_effect_requested(bus_name: String, kind_string: String,
		preset_id: String) -> void:
	if _config_model == null:
		return
	if not _config_model.add_effect(bus_name, kind_string, preset_id):
		# v0.64.2: include preset_id in the warning if it was set.
		var ctx: String = kind_string
		if preset_id != "":
			ctx = "%s (preset '%s')" % [kind_string, preset_id]
		push_warning("[gool] add_effect failed for bus '%s' kind '%s'"
				% [bus_name, ctx])


func _on_panel_remove_effect_requested(bus_name: String,
		effect_index: int) -> void:
	if _config_model == null:
		return
	# ConfirmationDialog before commit. We dismiss immediately on
	# Cancel and call _config_model.remove_effect on confirm.
	var dlg := ConfirmationDialog.new()
	dlg.title = "Remove effect"
	# Get the effect's kind name for the confirm prompt.
	var effects := _config_model.get_effects(bus_name)
	var kind_name: String = "(unknown)"
	if effect_index >= 0 and effect_index < effects.size():
		kind_name = String((effects[effect_index] as Dictionary)
				.get("kind_name", "(unknown)"))
	dlg.dialog_text = "Remove %s effect from bus '%s'?\n\nThis cannot be undone via the dock — restore from gool/config.json.gool-backup if needed." % [kind_name, bus_name]
	dlg.confirmed.connect(_on_remove_effect_confirmed.bind(
			bus_name, effect_index, dlg))
	dlg.canceled.connect(dlg.queue_free)
	dlg.close_requested.connect(dlg.queue_free)
	add_child(dlg)
	dlg.popup_centered()


func _on_remove_effect_confirmed(bus_name: String, effect_index: int,
		dlg: ConfirmationDialog) -> void:
	if _config_model != null:
		if not _config_model.remove_effect(bus_name, effect_index):
			push_warning("[gool] remove_effect failed for bus '%s' idx %d"
					% [bus_name, effect_index])
	dlg.queue_free()


func _on_panel_move_effect_requested(bus_name: String,
		effect_index: int, direction: int) -> void:
	if _config_model == null:
		return
	var target: int = effect_index + direction
	if not _config_model.reorder_effect(bus_name, effect_index, target):
		push_warning("[gool] reorder_effect failed for bus '%s' %d→%d"
				% [bus_name, effect_index, target])


# --- Model topology signal handlers ---

# Effect chain changed on bus_name. If that bus's Fx panel is open,
# rebuild it from the current model state. Also refresh the strip's
# effect_count (drives the Fx button label "Fx (N)").
func _on_model_topology_changed(bus_name: String) -> void:
	# Update the strip's Fx button count.
	var idx: int = _index_of_bus(bus_name)
	if idx >= 0 and idx < _strips.size():
		var n: int = 0
		if _config_model != null:
			n = _config_model.get_effects(bus_name).size()
		(_strips[idx] as _BusStrip).set_effect_count(n)
	# Rebuild the open panel if this is the one being viewed.
	# v0.28.9: rebuild even when n==0 so the "+ Add Effect" button
	# stays reachable after removing the last effect.
	if _expanded_bus == bus_name and idx >= 0 and idx < _columns.size():
		var col: VBoxContainer = _columns[idx] as VBoxContainer
		_remove_panel_from_column(col)
		var effects: Array = _lookup_effects_for_bus(bus_name)
		var panel := _EffectsPanel.new()
		panel.bus_name = bus_name
		panel.param_changed.connect(_on_effect_param_changed)
		panel.add_effect_requested.connect(_on_panel_add_effect_requested)
		panel.remove_effect_requested.connect(_on_panel_remove_effect_requested)
		panel.move_effect_requested.connect(_on_panel_move_effect_requested)
		col.add_child(panel)
		panel.build_from_effects(effects)


# A bus was added to the config. Rebuild the strip row to include it.
func _on_model_bus_added(_bus_name: String) -> void:
	_load_static_layout_from_config()


# A bus was removed. Rebuild the strip row.
func _on_model_bus_removed(_bus_name: String) -> void:
	_load_static_layout_from_config()


# v0.80.17: bus rename propagation to external state. The
# ConfigModel.rename_bus mutator updates every reference inside
# config.json; this handler covers the two ProjectSettings entries
# that hold bus names by string for material EQ routing
# (`gool/material_eq/impact_bus` and `gool/material_eq/listener_bus`,
# read by runtime_singleton.gd at startup). Without this propagation,
# renaming the bus that was the impact-EQ or listener-EQ target
# would silently break the material EQ wiring — no error, no log,
# just no EQ. Also rebuilds the strip row so the visible bus header
# updates to the new name.
func _on_model_bus_renamed(old_name: String, new_name: String) -> void:
	const _IMPACT_BUS_SETTING := "gool/material_eq/impact_bus"
	const _LISTENER_BUS_SETTING := "gool/material_eq/listener_bus"
	var changed := false
	if ProjectSettings.get_setting(_IMPACT_BUS_SETTING, "") == old_name:
		ProjectSettings.set_setting(_IMPACT_BUS_SETTING, new_name)
		changed = true
	if ProjectSettings.get_setting(_LISTENER_BUS_SETTING, "") == old_name:
		ProjectSettings.set_setting(_LISTENER_BUS_SETTING, new_name)
		changed = true
	if changed:
		var err := ProjectSettings.save()
		if err != OK:
			push_warning(("[gool] failed to persist bus-rename in "
					+ "ProjectSettings (Error %d). Material EQ wiring "
					+ "may need to be reset manually in Project "
					+ "Settings → audio → gool.") % err)
		else:
			print("[gool] bus rename propagated to ProjectSettings "
					+ "material EQ wiring: %s → %s"
					% [old_name, new_name])
	# Rebuild the strip row so the renamed bus's header updates.
	_load_static_layout_from_config()


# --- Bus context menu (right-click on strip) ---

func _on_strip_context_menu_requested(bus_name: String,
		global_pos: Vector2) -> void:
	# v0.80.19: PopupMenu with Rename + Remove. Pre-v0.80.19 only
	# Remove existed; Rename was noted at this line as future work.
	var menu := PopupMenu.new()
	menu.add_item("Rename bus '%s'..." % bus_name, 1)
	# Master can't be renamed — see ConfigModel.rename_bus contract
	# (C++ bus parser hardcodes "Master" as the root sentinel).
	if bus_name == "Master":
		var rename_idx: int = menu.get_item_index(1)
		menu.set_item_disabled(rename_idx, true)
		menu.set_item_tooltip(rename_idx,
				"'Master' is reserved as the engine root bus and "
				+ "cannot be renamed.")
	menu.add_item("Remove bus '%s'..." % bus_name, 0)
	menu.id_pressed.connect(_on_strip_context_menu_id_pressed.bind(bus_name))
	menu.close_requested.connect(menu.queue_free)
	menu.popup_hide.connect(menu.queue_free)
	add_child(menu)
	menu.popup(Rect2i(Vector2i(global_pos), Vector2i(0, 0)))


# Signature note: in Godot 4, Callable.bind appends bound args AFTER
# the signal's own args. So `id_pressed(id)` connected with
# `.bind(bus_name)` is called as `(id, bus_name)` — signal first,
# bound last. (Got this backwards in v0.28.8; see lessons_learned.)
func _on_strip_context_menu_id_pressed(id: int, bus_name: String) -> void:
	if id == 0:
		_attempt_remove_bus(bus_name)
	elif id == 1:
		_attempt_rename_bus(bus_name)


# Pre-check refs. If any → error dialog with the list. Else →
# ConfirmationDialog. On confirm → model.remove_bus.
func _attempt_remove_bus(bus_name: String) -> void:
	if _config_model == null:
		return
	var refs: Array = _config_model.collect_bus_references(bus_name)
	if not refs.is_empty():
		var err_dlg := AcceptDialog.new()
		err_dlg.title = "Cannot remove bus '%s'" % bus_name
		err_dlg.dialog_text = (
				"This bus is still referenced from %d place(s):\n\n  - %s\n\n"
				% [refs.size(), "\n  - ".join(refs)]
				+ "Clear the references first, then try again.")
		# v0.28.10: AcceptDialog extends Window directly (not via
		# Popup), so it doesn't have popup_hide. Connect both the
		# OK button (confirmed) and the window-close button
		# (close_requested) to queue_free so the dialog cleans up
		# regardless of which path the user dismisses it through.
		# queue_free is idempotent, so connecting both is safe.
		err_dlg.confirmed.connect(err_dlg.queue_free)
		err_dlg.close_requested.connect(err_dlg.queue_free)
		add_child(err_dlg)
		err_dlg.popup_centered()
		return
	# No refs — confirm and delete.
	var dlg := ConfirmationDialog.new()
	dlg.title = "Remove bus"
	dlg.dialog_text = "Remove bus '%s'?\n\nThis cannot be undone via the dock — restore from gool/config.json.gool-backup if needed." % bus_name
	dlg.confirmed.connect(_on_remove_bus_confirmed.bind(bus_name, dlg))
	dlg.canceled.connect(dlg.queue_free)
	dlg.close_requested.connect(dlg.queue_free)
	add_child(dlg)
	dlg.popup_centered()


func _on_remove_bus_confirmed(bus_name: String,
		dlg: ConfirmationDialog) -> void:
	if _config_model != null:
		var err: int = _config_model.remove_bus(bus_name)
		if err != OK:
			push_warning("[gool] remove_bus failed for '%s': %d"
					% [bus_name, err])
	dlg.queue_free()


# v0.80.19: open the rename dialog for `bus_name`. The dialog has a
# pre-filled LineEdit (selected for instant retype) plus an inline
# status label that reports validation problems in real time. OK is
# kept disabled until the new name is non-empty, different from
# `bus_name`, and not a collision with another existing bus —
# mirroring ConfigModel.rename_bus's same three validation checks
# so the dialog refuses to commit anything the model would reject.
#
# Master is a hard "can't be renamed" — the context-menu builder
# disables the menu item for Master, but this function also rejects
# defensively in case the dispatch reaches us anyway.
func _attempt_rename_bus(bus_name: String) -> void:
	if _config_model == null:
		return
	if bus_name == "Master":
		return  # defense in depth — menu should have prevented this

	var dlg := ConfirmationDialog.new()
	dlg.title = "Rename bus"

	# Dialog body: prompt + LineEdit + inline status label
	var content := VBoxContainer.new()
	content.add_theme_constant_override("separation", 8)

	var prompt := Label.new()
	prompt.text = "Rename bus '%s' to:" % bus_name
	content.add_child(prompt)

	var name_edit := LineEdit.new()
	name_edit.text = bus_name
	name_edit.select_all()
	name_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	content.add_child(name_edit)

	var status_label := Label.new()
	status_label.add_theme_color_override("font_color",
			Color(0.95, 0.4, 0.4))
	status_label.visible = false
	content.add_child(status_label)

	dlg.add_child(content)

	# Build a snapshot of existing bus names (excluding the one
	# being renamed) so collision validation is fast and live.
	var existing_names: PackedStringArray = PackedStringArray()
	for b in _config_model.get_buses():
		if not (b is Dictionary):
			continue
		var n: String = String((b as Dictionary).get("name", ""))
		if n != "" and n != bus_name:
			existing_names.append(n)

	# Live validation. Same three rules as ConfigModel.rename_bus:
	# empty (ERR_INVALID_PARAMETER), same-as-old (no-op return OK),
	# collision (ERR_ALREADY_EXISTS). Disabling OK on no-op also
	# prevents the user from confirming what would be a wasted save.
	var validate := func(new_text: String) -> void:
		var trimmed: String = new_text.strip_edges()
		var ok_btn: Button = dlg.get_ok_button()
		if trimmed.is_empty():
			status_label.text = "Name cannot be empty."
			status_label.visible = true
			ok_btn.disabled = true
		elif trimmed == bus_name:
			status_label.visible = false  # no-op, not really an error
			ok_btn.disabled = true
		elif existing_names.has(trimmed):
			status_label.text = ("A bus named '%s' already exists. "
					+ "Two buses can't share a name.") % trimmed
			status_label.visible = true
			ok_btn.disabled = true
		else:
			status_label.visible = false
			ok_btn.disabled = false

	name_edit.text_changed.connect(validate)

	# Enter in the LineEdit acts as OK when valid. Emitting the OK
	# button's `pressed` signal triggers the dialog's own confirmed-
	# and-hide pathway — same as a real click.
	name_edit.text_submitted.connect(func(_t: String) -> void:
		var ok_btn: Button = dlg.get_ok_button()
		if not ok_btn.disabled:
			ok_btn.pressed.emit()
	)

	dlg.confirmed.connect(_on_rename_bus_confirmed.bind(bus_name,
			name_edit, dlg))
	dlg.canceled.connect(dlg.queue_free)
	dlg.close_requested.connect(dlg.queue_free)
	add_child(dlg)

	# Initial state: text == bus_name, which is a no-op. OK disabled.
	dlg.get_ok_button().disabled = true

	dlg.popup_centered(Vector2i(420, 0))
	name_edit.grab_focus()


# Confirmed-OK handler for the rename dialog. UI pre-validation
# should have made the rename_bus call succeed in the common case,
# but the error path stays robust against rare scenarios — race
# with an external config edit, a defensive reject from the model
# for Master, or any future ConfigModel-side validation we don't
# duplicate in the dialog. On non-OK, surface a specific message
# rather than just logging.
func _on_rename_bus_confirmed(old_name: String, name_edit: LineEdit,
		dlg: ConfirmationDialog) -> void:
	if _config_model == null:
		dlg.queue_free()
		return
	var new_name: String = name_edit.text.strip_edges()
	var result: int = _config_model.rename_bus(old_name, new_name)
	if result != OK:
		var err_dlg := AcceptDialog.new()
		err_dlg.title = "Rename failed"
		var msg: String = ""
		match result:
			ERR_INVALID_PARAMETER:
				msg = "The new name is empty or invalid."
			ERR_ALREADY_EXISTS:
				msg = ("A bus named '%s' already exists. "
						+ "Two buses can't share a name.") % new_name
			ERR_INVALID_DATA:
				if old_name == "Master":
					msg = ("'Master' is reserved by the engine and "
							+ "cannot be renamed.")
				else:
					msg = ("Bus '%s' no longer exists in the model. "
							+ "Refresh the dock and try again.") % old_name
			_:
				msg = "Rename returned error code %d." % result
		err_dlg.dialog_text = msg
		err_dlg.confirmed.connect(err_dlg.queue_free)
		err_dlg.close_requested.connect(err_dlg.queue_free)
		add_child(err_dlg)
		err_dlg.popup_centered()
	dlg.queue_free()


# --- Add bus button + name input ---

# Builds a strip-shaped "+ Add Bus" column appended to the strip row.
# Called from _rebuild_strips_from_config after all real strips.
func _build_add_bus_column() -> VBoxContainer:
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 4)
	var btn := Button.new()
	btn.text = "+\nAdd Bus"
	btn.custom_minimum_size = Vector2(STRIP_WIDTH, STRIP_HEIGHT)
	btn.focus_mode = Control.FOCUS_NONE
	btn.tooltip_text = "Add a new bus to the mixer"
	btn.pressed.connect(_on_add_bus_button_pressed)
	col.add_child(btn)
	return col


func _on_add_bus_button_pressed() -> void:
	# A ConfirmationDialog with a LineEdit child for the bus name.
	# Godot doesn't have a built-in "prompt for a string" dialog,
	# so we compose one. register_text_enter wires Enter-to-confirm
	# natively — no manual text_submitted handler needed.
	var dlg := ConfirmationDialog.new()
	dlg.title = "Add bus"
	dlg.dialog_text = "Bus name:"
	var input := LineEdit.new()
	input.placeholder_text = "Bus name (e.g. 'Footsteps')"
	input.custom_minimum_size = Vector2(240, 0)
	dlg.add_child(input)
	dlg.register_text_enter(input)
	dlg.confirmed.connect(_on_add_bus_dialog_confirmed.bind(input, dlg))
	dlg.canceled.connect(dlg.queue_free)
	dlg.close_requested.connect(dlg.queue_free)
	add_child(dlg)
	dlg.popup_centered()
	input.grab_focus()


func _on_add_bus_dialog_confirmed(input: LineEdit,
		dlg: ConfirmationDialog) -> void:
	var bus_name_input: String = input.text.strip_edges()
	dlg.queue_free()
	if bus_name_input.is_empty():
		return
	if _config_model == null:
		return
	var err: int = _config_model.add_bus(bus_name_input)
	if err == ERR_ALREADY_EXISTS:
		var err_dlg := AcceptDialog.new()
		err_dlg.title = "Add bus failed"
		err_dlg.dialog_text = "A bus named '%s' already exists." % bus_name_input
		# v0.28.10: AcceptDialog has no popup_hide signal — see the
		# matching site in _attempt_remove_bus for the explanation.
		err_dlg.confirmed.connect(err_dlg.queue_free)
		err_dlg.close_requested.connect(err_dlg.queue_free)
		add_child(err_dlg)
		err_dlg.popup_centered()
	elif err != OK:
		push_warning("[gool] add_bus failed for '%s': %d" % [bus_name_input, err])


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


# v0.48.0: explicit Save Mix to Config button handler. Complements
# the debounced auto-save (v0.28.4) which writes patched edits in
# place. This one does a clean full rewrite via overwrite_disk() —
# useful when the patch-based auto-save has lost confidence (after
# external edits, formatting damage, or merge conflicts) or when
# the dev wants to commit the current mix state as a baseline.
func _on_save_mix_to_config_pressed() -> void:
	if _config_model == null:
		push_warning("[gool] Save Mix to Config: no config model loaded")
		return
	var result: int = _config_model.overwrite_disk()
	if result == OK:
		print("[gool] Saved mix to res://gool/config.json (full rewrite)")
		_refresh_dirty_indicators()
	else:
		push_warning("[gool] Save Mix to Config: overwrite_disk returned error %d" % result)


# v0.54.0: hierarchy mode toggle. CheckButton "Tree" emits toggled
# when the user clicks; we update state, persist to EditorSettings,
# and rebuild strips from the config model so the new layout takes
# effect immediately.
func _on_hierarchy_toggle_toggled(button_pressed: bool) -> void:
	if _hierarchy_mode_tree == button_pressed:
		return  # idempotent — no-op if state already matches
	_hierarchy_mode_tree = button_pressed
	_save_hierarchy_pref(button_pressed)
	# Rebuild from the model. _load_static_layout_from_config does
	# the right thing — reads the current bus list and feeds it to
	# _rebuild_strips_from_config, which now consults
	# _hierarchy_mode_tree to decide whether to sort + pad.
	_load_static_layout_from_config()


# v0.54.0: read the persisted hierarchy-mode preference from
# EditorSettings. Safe in non-editor contexts (returns false).
# Falls back to the v0.54.0 default (flat) if the setting isn't
# present or EditorSettings isn't reachable.
func _load_hierarchy_pref() -> bool:
	if not Engine.is_editor_hint():
		return false
	if EditorInterface == null:
		return false
	var es = EditorInterface.get_editor_settings()
	if es == null:
		return false
	if not es.has_setting(SETTING_HIERARCHY_MODE):
		return false
	return bool(es.get_setting(SETTING_HIERARCHY_MODE))


# v0.54.0: persist the hierarchy-mode preference to EditorSettings.
# No-op outside the editor.
func _save_hierarchy_pref(value: bool) -> void:
	if not Engine.is_editor_hint():
		return
	if EditorInterface == null:
		return
	var es = EditorInterface.get_editor_settings()
	if es == null:
		return
	es.set_setting(SETTING_HIERARCHY_MODE, value)


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


# v0.80.12: parallel to _on_model_external_change_detected, but for
# the case where config.json was REMOVED externally (renamed,
# deleted) since the dock last loaded it. Same dialog, same two
# actions — semantics map cleanly: "Reload" honors the removal
# (reload_from_disk_discarding_edits gets ERR_FILE_NOT_FOUND and
# the dock switches to empty-state); "Overwrite with dock state"
# recreates config.json from the current in-memory model. Text is
# adapted so the user understands they removed the file (the
# external-change phrasing would be misleading).
func _on_model_external_removal_detected(pending_dirty_buses: Array) -> void:
	if _mtime_conflict_dialog == null:
		return
	var bus_list: String = ", ".join(pending_dirty_buses)
	if bus_list.is_empty():
		bus_list = "(none)"
	_mtime_conflict_dialog.dialog_text = (
		"gool/config.json was removed outside the editor (renamed, "
		+ "deleted, or moved). The dock still has in-memory state "
		+ "with unsaved edits to: " + bus_list + ".\n\n"
		+ "Pre-v0.80.12 the dock would silently recreate the file "
		+ "from this in-memory state. It no longer does that — choose:\n\n"
		+ "  Reload from disk: accept the removal. The dock discards "
		+ "its in-memory edits and switches to the empty-state "
		+ "(\"No config.json yet — pick a template\").\n"
		+ "  Overwrite with dock state: recreate config.json from "
		+ "the current dock state (including unsaved edits)."
	)
	_mtime_conflict_dialog.popup_centered()


# v0.53.0: tab-switch handler. When the user clicks over to the
# Sound Bank tab, kick off a fresh rescan so banks added since
# the last view (or since dock startup) show up automatically.
# Cheap when no banks exist; bounded by project size when they do.
func _on_tab_changed(tab_idx: int) -> void:
	if _tab_container == null or _sound_bank_panel == null:
		return
	# Tab indices match the order children were added: 0 = Mixer,
	# 1 = Sound Bank. Refresh only the Sound Bank tab.
	if tab_idx <= 0:
		return
	var tab_node: Node = _tab_container.get_tab_control(tab_idx)
	if tab_node == _sound_bank_panel \
			and _sound_bank_panel.has_method("rescan_and_rebuild"):
		_sound_bank_panel.rescan_and_rebuild()


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


# ---- Live Stats panel (v0.44.0) --------------------------------------
#
# Compact observability strip below the bus strips. Shows engine
# health from the gool:render_stats debugger channel: voice count,
# emitter count, master peak (dB), pre-mixer peak (dB), dropout
# count, and per-player VOIP jitter health when voice chat is
# active.
#
# Rendering cost is trivial — Label.text writes on a handful of
# Labels, only on _poll (30 Hz). The voice-chat row only rebuilds
# its children when the registered-player set changes, which is
# rare in typical sessions.
#
# Design choice — single-line, always-visible:
# We considered a collapsible expand-on-demand panel. Rejected
# because the L4D2-shaped dev needs to SEE the numbers without
# clicking — silent-disaster prevention is the value prop, and an
# expandable panel adds the "did I remember to expand it?" failure
# mode. Single line, always visible, takes ~30 pixels of vertical
# space below the strips.
#
# Known limitations (Tier 2, not in v0.44.0):
# - No per-bus compressor-reduction display ("how much is bus X
#   being ducked right now?"). Needs a new C++ API to expose
#   current reduction-in-dB per compressor. The L4D2-relevant
#   answer is in the bus_stats payload (we have peak_linear per
#   bus already) but the *cause* of reduction isn't exposed yet.
# - No active-voice list with per-voice metadata (priority, age,
#   source emitter). The count is shown; the list isn't.
# - No eviction counter over a rolling window. Drops counter is
#   shown but isn't broken out by reason.

func _build_stats_panel() -> PanelContainer:
	# v0.51.0: themed stats panel. Per-stat "cards" with uppercase
	# label / monospace value / optional inline meter or sparkline.
	# Replaces v0.44.0's row of plain "Voices: 12" Labels.
	var panel := PanelContainer.new()
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	panel.add_theme_stylebox_override("panel", _build_chrome_stylebox(10))

	var vbox := VBoxContainer.new()
	vbox.add_theme_constant_override("separation", 6)
	panel.add_child(vbox)

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 24)
	vbox.add_child(row)

	# Leader label — small caps "LIVE" in accent color when data is
	# flowing, dim when idle. Stat card values switch to placeholders
	# when idle so the leader doubles as a "data freshness" cue.
	_stats_leader = Label.new()
	_stats_leader.text = "LIVE"
	_stats_leader.add_theme_color_override("font_color", _theme_text_secondary())
	_stats_leader.add_theme_font_size_override("font_size", 10)
	row.add_child(_stats_leader)

	_stats_labels = {}
	_stats_cards = {}

	# Voices: count + 0..max meter. Max is the spatial-emitter budget
	# from config.h (64). Bar fills as the count climbs; user spots
	# saturation visually.
	_stats_cards["voices"] = _build_stat_card(row, "VOICES", true, false)
	_stats_labels["voices"] = _stats_cards["voices"].get_meta("value_label")

	_stats_cards["emitters"] = _build_stat_card(row, "EMITTERS", true, false)
	_stats_labels["emitters"] = _stats_cards["emitters"].get_meta("value_label")

	# Master peak gets the sparkline — at-a-glance "is it pumping?"
	# without watching numbers tick.
	_stats_cards["master_peak"] = _build_stat_card(row, "MASTER", false, true)
	_stats_labels["master_peak"] = _stats_cards["master_peak"].get_meta("value_label")

	_stats_cards["peak_amplitude"] = _build_stat_card(row, "PRE-MIX", false, false)
	_stats_labels["peak_amplitude"] = _stats_cards["peak_amplitude"].get_meta("value_label")

	_stats_cards["drops"] = _build_stat_card(row, "DROPS", false, false)
	_stats_labels["drops"] = _stats_cards["drops"].get_meta("value_label")

	# Voice-chat sub-row — same hidden-until-needed behavior as v0.44.0.
	_voice_chat_row = HBoxContainer.new()
	_voice_chat_row.add_theme_constant_override("separation", 12)
	_voice_chat_row.visible = false
	vbox.add_child(_voice_chat_row)

	return panel

# v0.51.0: build a single stat "card" — small uppercase label
# stacked above the value, with an optional meter or sparkline
# below. Cards stash references to their value Label, meter, and
# sparkline in metadata so the update path can find them without
# tree traversal.
func _build_stat_card(parent: HBoxContainer, label_text: String,
		with_meter: bool, with_sparkline: bool) -> Control:
	var card := VBoxContainer.new()
	card.add_theme_constant_override("separation", 2)
	parent.add_child(card)

	var label := Label.new()
	label.text = label_text
	label.add_theme_color_override("font_color", _theme_text_secondary())
	label.add_theme_font_size_override("font_size", 9)
	card.add_child(label)

	var value := Label.new()
	value.text = "—"
	value.add_theme_color_override("font_color", _theme_text_primary())
	value.add_theme_font_size_override("font_size", 14)
	# Try to use the monospace font from the editor theme; falls
	# back to default if not available. Monospace gives numbers a
	# uniform column width — so "100" and "0" don't shift the
	# layout as they change.
	var mono := _theme_color_get_mono_font()
	if mono != null:
		value.add_theme_font_override("font", mono)
	card.add_child(value)

	# Bottom row: meter or sparkline (whichever was requested).
	if with_meter:
		var meter := ProgressBar.new()
		meter.show_percentage = false
		meter.max_value = 100.0
		meter.value = 0.0
		meter.custom_minimum_size = Vector2(72, 4)
		card.add_child(meter)
		card.set_meta("meter", meter)
	elif with_sparkline:
		var spark := _PeakSparkline.new()
		spark.custom_minimum_size = Vector2(72, 12)
		spark.theme_color = _theme_accent()
		card.add_child(spark)
		card.set_meta("sparkline", spark)
	else:
		# Spacer to align card heights regardless of meter/spark
		# presence. 4px matches ProgressBar default thickness.
		var pad := Control.new()
		pad.custom_minimum_size = Vector2(0, 4)
		card.add_child(pad)

	card.set_meta("value_label", value)
	card.set_meta("text_label", label)
	return card

# Pull a monospace font from the editor theme. Returns null if
# nothing usable is available; callers fall back to the default
# sans font.
func _theme_color_get_mono_font() -> Font:
	if not Engine.is_editor_hint():
		return null
	var theme := EditorInterface.get_editor_theme() if EditorInterface else null
	if theme == null:
		return null
	if theme.has_font("source", "EditorFonts"):
		return theme.get_font("source", "EditorFonts")
	return null

# Called from _poll() at 30 Hz. Reads the cached render_stats
# from the debugger plugin and updates the labels. When the
# session isn't running (or hasn't sent stats yet), the dict is
# empty and we show "—" placeholders so the dock visibly conveys
# "no live data" rather than stale numbers.
func _update_stats_panel() -> void:
	if _stats_labels.is_empty():
		# Panel not built yet — defensive guard, _ready may not have
		# completed before the first _poll fires in some editor states.
		return
	if _debugger_plugin == null:
		_set_stats_placeholders()
		return
	var rs: Dictionary = _debugger_plugin.get_latest_render_stats()
	if rs.is_empty():
		_set_stats_placeholders()
		return

	# Voices and emitters: direct integer fields from get_render_stats.
	# v0.51.0: card label is "VOICES", so the value is just the number
	# (no redundant "Voices:" prefix). Push meter ratio against the
	# configured spatial/active emitter budgets — 64 and 128 by default
	# (AudioRuntimeBudget from config.h).
	var voices: int = int(rs.get("active_voices", 0))
	var emitters: int = int(rs.get("active_emitters", 0))
	_stats_labels["voices"].text = "%d" % voices
	_stats_labels["emitters"].text = "%d" % emitters
	_update_card_meter("voices", float(voices) / 64.0)
	_update_card_meter("emitters", float(emitters) / 128.0)

	# Master peak (post-mixer, what's actually leaving the engine).
	# Convert linear → dB; clamp the floor at -inf for display.
	# v0.51.0: also push to the sparkline so the master card shows a
	# rolling 30-second trace.
	var mp_lin: float = float(rs.get("mixer_peak", 0.0))
	_stats_labels["master_peak"].text = _format_db(mp_lin)
	_update_card_sparkline("master_peak", mp_lin)
	# Tint the master peak red when clipping (> 0 dBFS) — at-a-glance
	# danger cue without having to read the number.
	if mp_lin > 1.0:
		_stats_labels["master_peak"].add_theme_color_override(
				"font_color", COLOR_RED)
	else:
		_stats_labels["master_peak"].add_theme_color_override(
				"font_color", _theme_text_primary())

	# Pre-mixer peak (peak_amplitude is the raw render-thread peak
	# before master gain). Useful to spot clipping at the source
	# even when master gain is pulled down.
	var pa_lin: float = float(rs.get("peak_amplitude", 0.0))
	_stats_labels["peak_amplitude"].text = _format_db(pa_lin)

	# Dropout / exception count — runs as a monotonic counter,
	# so a non-zero value indicates the engine has hit at least
	# one exception since the session started. Worth surfacing
	# prominently because audio dropouts are otherwise silent.
	# v0.51.0: tint red when nonzero so it can't be missed at a glance.
	var drops: int = int(rs.get("exception_count", 0))
	_stats_labels["drops"].text = "%d" % drops
	if drops > 0:
		_stats_labels["drops"].add_theme_color_override(
				"font_color", COLOR_RED)
	else:
		_stats_labels["drops"].add_theme_color_override(
				"font_color", _theme_text_primary())

	# Leader: pulse to accent color when data is fresh (the v0.51.0
	# cue that this row is live). Stays dim when poll returns empty.
	if _stats_leader != null:
		_stats_leader.add_theme_color_override("font_color", _theme_accent())

	# Voice chat sub-row: rebuild when the player set changes,
	# update text every tick otherwise.
	var vc: Dictionary = rs.get("voice_chat", {})
	_update_voice_chat_row(vc)

func _set_stats_placeholders() -> void:
	_stats_labels["voices"].text         = "—"
	_stats_labels["emitters"].text       = "—"
	_stats_labels["master_peak"].text    = "—"
	_stats_labels["peak_amplitude"].text = "—"
	_stats_labels["drops"].text          = "—"
	# v0.51.0: zero out meters + reset clip-warning tints when going idle
	_update_card_meter("voices", 0.0)
	_update_card_meter("emitters", 0.0)
	for k in ["master_peak", "drops"]:
		if _stats_labels.has(k):
			_stats_labels[k].add_theme_color_override(
					"font_color", _theme_text_primary())
	if _stats_leader != null:
		_stats_leader.add_theme_color_override(
				"font_color", _theme_text_secondary())
	if _voice_chat_row != null:
		_voice_chat_row.visible = false

# v0.51.0: helper to update a stat card's optional inline meter.
# Ratio 0..1 maps to 0..100 on the ProgressBar. Clamped because
# voice count CAN exceed the budget for a frame (drops happen
# immediately after) and the visual should peg at full, not
# wrap or hide.
func _update_card_meter(card_key: String, ratio: float) -> void:
	if not _stats_cards.has(card_key):
		return
	var card: Control = _stats_cards[card_key]
	if not card.has_meta("meter"):
		return
	var meter: ProgressBar = card.get_meta("meter")
	if meter != null:
		meter.value = clamp(ratio, 0.0, 1.0) * 100.0

# v0.51.0: helper to push a sample into a stat card's sparkline.
func _update_card_sparkline(card_key: String, linear_value: float) -> void:
	if not _stats_cards.has(card_key):
		return
	var card: Control = _stats_cards[card_key]
	if not card.has_meta("sparkline"):
		return
	var spark = card.get_meta("sparkline")
	if spark != null and spark.has_method("push_sample"):
		spark.push_sample(linear_value)

func _format_db(linear: float) -> String:
	# Clamp tiny values to "-∞" rather than displaying e.g. "-120.0
	# dB" — visually noisy and not useful information.
	if linear <= 0.00001:
		return "-∞ dB"
	var db: float = linear_to_db(linear)
	return "%.1f dB" % db

# Sync the voice-chat row to the current set of registered VOIP
# players. Rebuilds child labels only when the player set changes
# (set inequality, not value inequality — values change every tick
# and rebuilding for those would be wasteful).
func _update_voice_chat_row(vc: Dictionary) -> void:
	if vc.is_empty():
		_voice_chat_row.visible = false
		_voice_chat_players_cached = PackedInt32Array()
		# Defer freeing children — cheap to leave them in the hidden
		# parent, and rebuilding on every show/hide would churn.
		return

	# Detect player-set change
	var current_players := PackedInt32Array()
	for pid in vc.keys():
		current_players.append(int(pid))
	current_players.sort()
	if current_players != _voice_chat_players_cached:
		# Rebuild children with the new set
		for child in _voice_chat_row.get_children():
			child.queue_free()
		var leader := Label.new()
		leader.text = "Voice chat"
		leader.add_theme_color_override("font_color", COLOR_TEXT_DIM)
		_voice_chat_row.add_child(leader)
		for pid in current_players:
			var lbl := Label.new()
			lbl.name = "player_%d" % pid
			_voice_chat_row.add_child(lbl)
		_voice_chat_players_cached = current_players

	# Update each player's label text
	_voice_chat_row.visible = true
	for pid in current_players:
		var lbl := _voice_chat_row.get_node_or_null("player_%d" % pid) as Label
		if lbl == null:
			continue
		var pdata: Dictionary = vc.get(pid, {})
		# Some engines send the dict keyed by int, some by string-ish.
		# Defensive: also try the string key if int didn't hit.
		if pdata.is_empty():
			pdata = vc.get(str(pid), {})
		var jitter: float = float(pdata.get("jitter_ms", 0.0))
		var loss: float = float(pdata.get("packet_loss", 0.0))
		lbl.text = "Player %d: %.0f ms / %.0f%% loss" \
				% [pid, jitter, loss * 100.0]


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

	# v0.64.0 (Phase 1, UI evolution plan): routing data. Both fields
	# are set externally by the outer GoolMixerDock when populating
	# each strip from config.json or runtime stats; the strip itself
	# doesn't look up topology. parent_name is the bus this strip
	# routes to ("Master", "SfxAll", etc.) or "" if this bus has no
	# parent (root, i.e. Master itself). is_master is true when this
	# strip represents a root bus — drives the route footer text
	# ("→ Output" vs "→ ParentName") and Phase 2's visual emphasis.
	#
	# Why two fields instead of inferring is_master from parent_name
	# being empty: an orphan bus (parent string set but doesn't
	# resolve to a known bus) ALSO has parent_name=="" after the
	# outer dock's resolve step. is_master disambiguates the two
	# cases so the orphan can render "→ (unrouted)" in the warning
	# color without being mistaken for the actual Master strip.
	var parent_name: String = ""
	var is_master: bool = false

	# v0.64.0 (Phase 5, UI evolution plan): per-effect abbreviation
	# strings supplied by the engine binding (`kind_abbrev` field on
	# each effect dict from get_bus_effects / config_model.get_effects).
	# Drives the FX_BAND chain pills — one pill per entry, rendered
	# left-to-right in chain order. Stays a parallel array to
	# _effect_count rather than replacing it because _effect_count is
	# read by the outer dock for the "has any effects, draw button at
	# all?" check, and keeping both lets the strip work even if a
	# caller forgets to call set_effect_abbrevs (falls back to a
	# generic "Fx (N)" pill — see _draw_fx_chain).
	var _effect_abbrevs: PackedStringArray = PackedStringArray()

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
	# v0.64.0 (Phase 1 of the UI evolution plan): ROUTE_BAND sits at
	# the bottom of every strip, below FX_BAND. Renders "→ ParentName"
	# (or "→ Output" for Master, or "→ (unrouted)" for an orphan bus
	# whose parent doesn't resolve). 22px tall to match FX_BAND so the
	# two bottom bands form a visually consistent pair. Drawn in
	# COLOR_ROUTE_TEXT — deliberately muted, ~half the contrast of the
	# bus name, so it reads as a footer caption rather than a label
	# competing for attention.
	const ROUTE_BAND: float = 22.0
	const ROUTE_SEPARATOR_INSET: float = 6.0
	const COLOR_ROUTE_TEXT := Color(0.49, 0.50, 0.56)
	const COLOR_ROUTE_TEXT_MASTER := Color(0.64, 0.65, 0.75)
	const COLOR_ROUTE_TEXT_WARN := Color(0.95, 0.62, 0.32)
	const COLOR_ROUTE_SEPARATOR := Color(0.16, 0.17, 0.20)
	# v0.64.0 (Phase 2): Master strip outline. Submix strips don't
	# draw an outer outline (the meter/fader rects define their
	# bounds visually); Master gets one in a muted blue-purple to
	# signal "terminal stage" without being loud. 2px stroke gives
	# it weight without crowding the inner content.
	#
	# Color rationale: avoids the saturated purple already used by
	# the active Bypass button state — a desaturated, slightly
	# bluer variant reads as "system/special" rather than "user
	# state." Audit-trail: this color was the option labeled
	# "Muted blue-purple stroke (as mocked up)" in the design
	# review for the 0.64.0 visual refresh.
	const COLOR_MASTER_OUTLINE := Color(0.48, 0.49, 0.72)
	const MASTER_STROKE_W: float = 2.0

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

	# v0.28.8 right-click for bus context menu (remove, future ops).
	# global_pos is where the click landed in screen coords, used to
	# popup the menu next to the cursor.
	signal context_menu_requested(bus_name: String, global_pos: Vector2)

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
		# v0.28.9: do NOT auto-collapse when count drops to 0.
		# Pre-v0.28.9 collapsed here, but keeping the panel open
		# lets the user immediately add a new effect after
		# removing the last one. Manual collapse via the Fx button
		# is always available.
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

	# v0.64.0 (Phase 1, UI evolution plan): routing setter. Outer
	# dock resolves the bus's parent (from config.json's "parent"
	# field, or from runtime stats' parent-index lookup) and pushes
	# the name string here. Empty string + is_master=true → render
	# "→ Output". Empty string + is_master=false → orphan (render
	# "→ (unrouted)" in warning color). Otherwise → "→ <name>".
	func set_parent_name(name: String) -> void:
		if parent_name == name:
			return
		parent_name = name
		queue_redraw()

	# v0.64.0 (Phase 1 + Phase 2): root-bus flag. Drives BOTH the
	# route footer text ("→ Output" vs "→ ParentName") AND the
	# Phase 2 visual emphasis (wider strip, blue-purple outline,
	# all-caps name). The outer dock sets this true for any bus
	# with no parent in the topology.
	func set_is_master(b: bool) -> void:
		if is_master == b:
			return
		is_master = b
		queue_redraw()

	# v0.64.0 (Phase 5): set the per-effect abbreviation list
	# (e.g. ["EQ", "COMP", "REVERB"]). Drives the FX_BAND chain
	# pills. Order matches the effect chain on the bus. Pass an
	# empty PackedStringArray to clear (also makes set_effect_count
	# the authoritative count; the strip falls back to a generic
	# "Fx (N)" pill if abbrevs are empty but count > 0).
	func set_effect_abbrevs(abbrevs: PackedStringArray) -> void:
		# PackedStringArray doesn't implement ==; compare manually.
		if _effect_abbrevs.size() == abbrevs.size():
			var same: bool = true
			for i in range(abbrevs.size()):
				if _effect_abbrevs[i] != abbrevs[i]:
					same = false
					break
			if same:
				return
		_effect_abbrevs = abbrevs.duplicate()
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
	#
	# v0.64.0: FX_BAND is no longer flush with the bottom of the
	# strip — ROUTE_BAND sits below it now. Y offset accounts for
	# that. Old math: `size.y - FX_BAND + 2.0`. If you're updating
	# this method, also update _start_db_edit's position math which
	# uses the same offset chain.
	func _fx_button_rect() -> Rect2:
		return Rect2(FX_BUTTON_INSET_X,
				size.y - FX_BAND - ROUTE_BAND + 2.0,
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
		# v0.64.0: also shifted by ROUTE_BAND — readout sits above
		# FX_BAND which sits above ROUTE_BAND.
		var pad: float = 4.0
		_db_editor.position = Vector2(pad, size.y - FX_BAND - READOUT_BAND - ROUTE_BAND)
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
		# v0.28.7: removed grab_focus() on self — the strip's
		# focus_mode is FOCUS_NONE (intentional, so click-and-drag
		# on the fader doesn't take keyboard focus), so the call was
		# always producing a "This control can't grab focus" warning.
		# Setting _db_editor.visible = false auto-releases its focus,
		# which is sufficient — subsequent fader drags hit _gui_input
		# regardless of focus state.
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

	# v0.28.5: click-anywhere-outside dismisses the dB editor.
	#
	# Why this exists: focus_exited only fires when focus transfers
	# to another focusable Control. Clicking on the strip background,
	# the dock empty space, or another non-focusable Control doesn't
	# transfer focus — so the LineEdit stays open with no obvious
	# way to dismiss it short of pressing Enter or Escape.
	#
	# Fix: listen for unhandled mouse clicks at the input layer.
	# If the LineEdit is visible and the click position falls
	# OUTSIDE its global rect, commit the current text (matching
	# the focus_exited behavior — clicks-outside = commit, Escape
	# = cancel). Inside-rect clicks fall through to the LineEdit
	# itself for normal text-cursor positioning.
	#
	# Using _input (not _unhandled_input) so we see the event even
	# if some other Control will eventually claim it. We don't
	# accept_event — we just observe and react.
	func _input(event: InputEvent) -> void:
		if _db_editor == null or not _db_editor.visible:
			return
		if not (event is InputEventMouseButton):
			return
		var mb := event as InputEventMouseButton
		if not mb.pressed:
			return
		if mb.button_index != MOUSE_BUTTON_LEFT \
				and mb.button_index != MOUSE_BUTTON_RIGHT \
				and mb.button_index != MOUSE_BUTTON_MIDDLE:
			return
		# Use the LineEdit's GLOBAL rect (its position is relative
		# to this strip, but we compare against the event's GLOBAL
		# mouse position).
		var lr: Rect2 = _db_editor.get_global_rect()
		if lr.has_point(mb.global_position):
			return  # click inside the editor: let LineEdit handle it
		# Click outside — commit current text and hide.
		_commit_db_edit(_db_editor.text)

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

		# v0.64.0 Phase 2: Master strip outline. Drawn first so the
		# inner content (meter, fader, pills, footer) sits over it.
		# Submix strips skip this — only Master gets the outline,
		# carrying the "this is the final stage" visual weight.
		if is_master:
			draw_rect(
					Rect2(Vector2.ZERO, Vector2(w, h)),
					COLOR_MASTER_OUTLINE, false, MASTER_STROKE_W)

		# v0.27.0: fader region shifted down to make room for the
		# BUTTON_BAND (S/M/B buttons sit between the bus name and the
		# fader). Both _draw and _gui_input compute the region the
		# same way — if you change one, change the other.
		# v0.28.3: FX_BAND eats 22px from the bottom (below READOUT_BAND)
		# to host the Fx toggle button. Same math change here AND in
		# _gui_input — both compute the region identically.
		# v0.64.0: ROUTE_BAND eats another 22px below FX_BAND for
		# the per-strip routing footer (Phase 1 of UI evolution plan).
		# Same change here AND in _gui_input.
		var fader_region_y: float = NAME_BAND + BUTTON_BAND + 4.0
		var fader_region_h: float = h - NAME_BAND - BUTTON_BAND - READOUT_BAND - FX_BAND - ROUTE_BAND - 8.0

		# --- Bus name (top) ---
		if f != null:
			# v0.64.0 Phase 2: Master gets "MASTER" in all caps as a
			# secondary visual differentiator (alongside the wider
			# strip and the blue-purple outline). The actual bus
			# name from config might be "Master" or "MasterBus" or
			# whatever — we deliberately override the displayed
			# label to a uniform "MASTER" so multiple Masters (in a
			# misconfigured topology with multiple roots) all read
			# as the terminal stage, and the strip's identity stays
			# consistent across configs.
			var display_name: String = "MASTER" if is_master else bus_name
			var name_size := f.get_string_size(
					display_name, HORIZONTAL_ALIGNMENT_CENTER, -1, fs)
			draw_string(f,
					Vector2((w - name_size.x) * 0.5, NAME_BAND - 4),
					display_name,
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

		# --- v0.28.3 / v0.64.0: Fx chain pills (above ROUTE_BAND) ---
		# Renders one variable-width pill per effect in the bus's
		# chain, labeled with the effect's short uppercase abbrev
		# from `_effect_abbrevs` (set by the outer dock from the
		# engine binding's kind_abbrev field). Falls back to the
		# pre-v0.64.0 "Fx (N)" single-button rendering when abbrevs
		# are empty but count > 0 (graceful: older external code
		# paths that only set_effect_count still get a usable UI).
		#
		# UX rationale:
		# - Each pill shows what kind of processing is happening, at
		#   a glance — no panel-open needed to see "EQ → COMP".
		# - Whole band is the click target (toggles the panel), same
		#   as the pre-v0.64.0 Fx button. Per-pill click-to-jump is
		#   future work.
		# - Active state (panel expanded) shows on the band's
		#   surrounding rect, not on individual pills, so the click-
		#   target affordance is unambiguous.
		# v0.28.9: always draw the Fx band, even when count==0, so
		# empty buses still have an entry point to "+ Add Effect".
		if f != null:
			_draw_fx_chain(_fx_button_rect(), _effect_count,
					_effect_abbrevs, _is_fx_expanded, f, fs_small)

		# --- v0.64.0 Phase 1: route footer (bottom band) ---
		# "→ ParentName" for non-root buses, "→ Output" for Master,
		# "→ (unrouted)" in warning color for orphans whose parent
		# string doesn't resolve to a known bus. Drawn in muted text
		# so it reads as a footer caption — visible without competing
		# with the bus name or the dB readout for attention.
		_draw_route_footer(f, fs_small)

	# v0.64.0 Phase 1: draw the route footer at the bottom of the
	# strip. Pulls all positioning from size.y/ROUTE_BAND so it stays
	# anchored if STRIP_HEIGHT changes. Thin separator line above the
	# text band sets it apart from FX_BAND visually — same as a
	# table footer rule.
	func _draw_route_footer(font: Font, font_size: int) -> void:
		var w: float = size.x
		var h: float = size.y
		var band_top: float = h - ROUTE_BAND
		# Thin separator above the footer. Inset slightly from the
		# strip edges so it doesn't fight the outer strip outline.
		draw_line(
				Vector2(ROUTE_SEPARATOR_INSET, band_top),
				Vector2(w - ROUTE_SEPARATOR_INSET, band_top),
				COLOR_ROUTE_SEPARATOR, 1.0)
		if font == null:
			return
		# Decide footer text + color from the three states:
		#   1. is_master                   → "→ Output" (system blue-purple)
		#   2. parent_name not empty       → "→ <parent_name>" (muted)
		#   3. neither (orphan: parent set in config but unresolved,
		#      or parent missing entirely from a non-root bus)
		#                                  → "→ (unrouted)" (warning)
		var footer_text: String
		var footer_color: Color
		if is_master:
			footer_text = "→ Output"
			footer_color = COLOR_ROUTE_TEXT_MASTER
		elif parent_name != "":
			footer_text = "→ %s" % parent_name
			footer_color = COLOR_ROUTE_TEXT
		else:
			footer_text = "→ (unrouted)"
			footer_color = COLOR_ROUTE_TEXT_WARN
		var text_size := font.get_string_size(
				footer_text, HORIZONTAL_ALIGNMENT_CENTER, -1, font_size)
		var ascent: float = float(font_size) * 0.75
		var text_x: float = (w - text_size.x) * 0.5
		var text_y: float = band_top + (ROUTE_BAND + ascent) * 0.5
		draw_string(font,
				Vector2(text_x, text_y),
				footer_text,
				HORIZONTAL_ALIGNMENT_CENTER,
				-1, font_size,
				footer_color)

	# ---- Input handling for fader drag ----

	func _gui_input(event: InputEvent) -> void:
		var w: float = size.x
		var h: float = size.y
		# v0.27.0: fader region accounts for BUTTON_BAND between the
		# name and the fader. Must match the _draw math exactly.
		# v0.28.3: FX_BAND also subtracted (eats from the bottom).
		# v0.64.0: ROUTE_BAND also subtracted — routing footer sits
		# below FX_BAND.
		var fader_region_y: float = NAME_BAND + BUTTON_BAND + 4.0
		var fader_region_h: float = h - NAME_BAND - BUTTON_BAND - READOUT_BAND - FX_BAND - ROUTE_BAND - 8.0
		var fader_x: float = 6.0 + METER_W + 14.0
		var fader_full_rect := Rect2(
				fader_x, fader_region_y,
				FADER_HANDLE_W, fader_region_h)
		# v0.26.5: readout band is now READOUT_BAND pixels just above
		# the FX_BAND (was bottom-anchored pre-v0.28.3). Click here
		# activates the LineEdit overlay for type-to-set dB entry.
		# Checked BEFORE the fader rect because the regions don't
		# overlap but the explicit ordering documents intent.
		# v0.64.0: y offset bumped by ROUTE_BAND — readout sits above
		# FX_BAND which sits above ROUTE_BAND.
		var readout_rect := Rect2(
				0.0, h - FX_BAND - READOUT_BAND - ROUTE_BAND,
				w, READOUT_BAND)
		# v0.28.3: Fx button rect (bottom FX_BAND of the strip). Only
		# active when the bus actually has effects — _effect_count==0
		# hides the button entirely so there's no orphan click.
		var fx_rect := _fx_button_rect()

		if event is InputEventMouseButton:
			var mb := event as InputEventMouseButton
			# v0.28.8: right-click anywhere on the strip → context menu.
			# Handled before the LEFT-click branch so MOUSE_BUTTON_RIGHT
			# doesn't fall through to the readout/fader handlers.
			if mb.pressed and mb.button_index == MOUSE_BUTTON_RIGHT:
				context_menu_requested.emit(
						bus_name, get_global_mouse_position())
				accept_event()
				return
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
				# v0.28.3 Fx toggle, v0.28.9: also valid when the
				# bus has zero effects — the panel opens with just
				# the "+ Add Effect" button visible, which is how
				# a freshly-added empty bus gets its first effect.
				# One panel open at a time is enforced at the outer
				# dock level via fx_toggled.
				if mb.pressed and fx_rect.has_point(mb.position):
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

	# v0.64.0 Phase 5: chain renderer. Replaces the single "Fx (N)"
	# button with one pill per effect, left-to-right in chain order,
	# pill widths hugging their text. The active state (panel open)
	# is shown on the surrounding band background so the whole band
	# remains the click target — pill-level clicks are future work.
	#
	# Fallback path: if abbrevs is empty (caller forgot or doesn't
	# know about v0.64.0), defers to _draw_fx_button so the strip
	# still renders something sensible. The dock's own rebuild paths
	# always populate abbrevs, but external scripts that talk to
	# _BusStrip directly might not.
	#
	# Overflow: if pills don't fit horizontally, truncate and append
	# an ellipsis pill "[…]" to signal more effects exist. Clicking
	# anywhere in the band still opens the panel which shows the
	# full chain.
	func _draw_fx_chain(rect: Rect2, count: int, abbrevs: PackedStringArray,
			active: bool, font: Font, font_size: int) -> void:
		# Fallback: empty abbrevs but non-zero count → old-style button.
		if abbrevs.is_empty():
			_draw_fx_button(rect, count, active, font, font_size)
			return
		# Background band: faint surrounding rect that responds to
		# the active (panel-expanded) state. Same color vocabulary
		# as _draw_fx_button so the visual "click here to expand"
		# affordance is preserved.
		var band_color: Color = COLOR_FX_BUTTON_ACTIVE if active else COLOR_BUTTON_INACTIVE
		draw_rect(rect, band_color, true)
		draw_rect(rect, COLOR_BUTTON_OUTLINE, false, 1.0)
		if font == null:
			return
		# Pill geometry. Width hugs text + padding; height a few px
		# smaller than the band so the band's outline shows around.
		var pill_inset_y: float = 2.0
		var pill_h: float = rect.size.y - pill_inset_y * 2.0
		var pill_pad_x: float = 4.0
		var pill_gap: float = 3.0
		var pill_y: float = rect.position.y + pill_inset_y
		var avail_w: float = rect.size.x - 4.0  # 2px breathing room each side
		var cursor_x: float = rect.position.x + 2.0
		var ascent: float = float(font_size) * 0.75
		# Reserve space for the overflow indicator IF we might need it.
		# Approximate: ellipsis pill needs about 18px (3 dots + padding).
		var ellipsis_reserve: float = 18.0 if abbrevs.size() > 1 else 0.0
		for i in range(abbrevs.size()):
			var abbrev: String = abbrevs[i]
			var label_size := font.get_string_size(
					abbrev, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size)
			var pill_w: float = label_size.x + pill_pad_x * 2.0
			# Will this pill (plus reserved ellipsis if more follow)
			# overflow? If yes, draw the ellipsis instead and stop.
			var remaining: float = avail_w - (cursor_x - rect.position.x)
			var needs_ellipsis: bool = (i < abbrevs.size() - 1)
			var room_needed: float = pill_w + (ellipsis_reserve + pill_gap if needs_ellipsis else 0.0)
			if room_needed > remaining:
				# Draw ellipsis pill in the remaining space if any.
				var ell_w: float = ellipsis_reserve
				if ell_w <= remaining and ell_w > 8.0:
					var ell_rect := Rect2(cursor_x, pill_y, ell_w, pill_h)
					draw_rect(ell_rect, COLOR_BG, true)
					draw_rect(ell_rect, COLOR_BUTTON_OUTLINE, false, 1.0)
					var ell_label := "…"
					var ell_size := font.get_string_size(
							ell_label, HORIZONTAL_ALIGNMENT_CENTER, -1, font_size)
					draw_string(font,
							Vector2(cursor_x + (ell_w - ell_size.x) * 0.5,
									pill_y + (pill_h + ascent) * 0.5),
							ell_label,
							HORIZONTAL_ALIGNMENT_CENTER,
							-1, font_size,
							COLOR_TEXT_DIM)
				break
			# Draw the pill.
			var pill_rect := Rect2(cursor_x, pill_y, pill_w, pill_h)
			draw_rect(pill_rect, COLOR_BG, true)
			draw_rect(pill_rect, COLOR_BUTTON_OUTLINE, false, 1.0)
			draw_string(font,
					Vector2(cursor_x + pill_pad_x,
							pill_y + (pill_h + ascent) * 0.5),
					abbrev,
					HORIZONTAL_ALIGNMENT_LEFT,
					-1, font_size,
					COLOR_TEXT)
			cursor_x += pill_w + pill_gap


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

# v0.61.1 — Phase 6.E.2: bus EQ visualization.
#
# When a bus's first three effects are BiquadFilters configured as
# LowShelf → Peak → HighShelf (the convention apply_material_eq_to_bus
# enforces — the same convention the material EQ designer authors
# against), show their cumulative frequency response above the flow
# row of effect tiles. Designer can see at a glance what shape this
# bus is imposing on its signal: "this bus is cutting 4 dB at 4 kHz,
# that's why my impact sound feels muffled."
#
# Reuses the v0.59.2 MaterialEqCurveView widget read-only (editable
# off). The widget docstring at material_eq_curve_view.gd:107 planted
# this hook explicitly: "Other callers (a future mixer-dock variant
# showing a bus's effective EQ) leave it false."
#
# Data flow:
#   1. _EffectsPanel.build_from_effects(effects) is called by the
#      dock on every poll (~30 Hz during F5) and on config edits.
#   2. _rebuild_panel_ui calls _BusEqVisualizer.update_from_effects(),
#      which inspects effects[0..2] and either populates the inner
#      MaterialEqCurveView's curve dict (returning true) or signals
#      "not an EQ chain" (returning false — caller hides it).
#
# Positional convention: biquad subtype (LowShelf/Peak/HighShelf vs
# LPF/HPF/BPF) is internal to BiquadFilterEffect and not exposed via
# the engine's effect-introspection API. apply_material_eq_to_bus
# assumes positional ordering today; the visualizer matches that
# assumption. A bus with three biquads that ISN'T shaped as a
# material EQ will render a misleading curve (but won't crash, and
# the per-effect param sliders below still show the truth). Future
# work: expose biquad type so the visualizer can verify the chain
# shape and fall back gracefully.
class _BusEqVisualizer extends VBoxContainer:

	const CURVE_VIEW_SCRIPT := preload(
			"res://addons/gool/editor/material_eq_curve_view.gd")

	# Engine EffectKind value for BiquadFilter. Mirrors the comment
	# block at line ~2742 — keep these in sync. Hardcoded rather
	# than imported because mixer_dock.gd is editor-side and doesn't
	# link against the engine's C++ enum.
	const _EFFECT_KIND_BIQUAD: int = 2

	# Engine EffectParameter IDs for biquad params. Match PARAM_META
	# entries 2 / 3 / 12 in _EffectsPanel above.
	const _PARAM_CUTOFF_HZ: int = 2
	const _PARAM_Q:         int = 3
	const _PARAM_GAIN_DB:   int = 12

	# Colors: match _EffectsPanel's COLOR_HEADER_BG / TEXT so the
	# plot's surrounding header reads as part of the same panel.
	const _COLOR_HEADER_BG   := Color(0.20, 0.22, 0.26)
	const _COLOR_HEADER_TEXT := Color(0.92, 0.92, 0.92)

	# Plot height. The inspector's curve view defaults to 180 px;
	# 140 is a touch shorter to keep the mixer-dock footprint compact
	# without sacrificing readability of the ±12 dB axis.
	const _PLOT_HEIGHT: float = 140.0

	var _curve_view: Control = null
	var _header: Label = null

	func _init() -> void:
		add_theme_constant_override("separation", 2)

		# Header bar so the plot reads as "this is what's happening
		# to this bus", not "this is an editor for the bus".
		_header = Label.new()
		_header.text = "Effective EQ (live)"
		_header.add_theme_color_override("font_color", _COLOR_HEADER_TEXT)
		var hdr_panel := PanelContainer.new()
		var sb := StyleBoxFlat.new()
		sb.bg_color = _COLOR_HEADER_BG
		sb.content_margin_left = 6.0
		sb.content_margin_right = 6.0
		sb.content_margin_top = 2.0
		sb.content_margin_bottom = 2.0
		hdr_panel.add_theme_stylebox_override("panel", sb)
		hdr_panel.add_child(_header)
		add_child(hdr_panel)

		_curve_view = CURVE_VIEW_SCRIPT.new()
		_curve_view.custom_minimum_size = Vector2(0, _PLOT_HEIGHT)
		_curve_view.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_curve_view.editable = false   # read-only in the mixer dock
		_curve_view.material_label = ""
		add_child(_curve_view)

	# Inspect the effects array. If the first three entries are
	# biquads, extract their cutoff / Q / gain_db params, pack them
	# into the curve-view's Dictionary shape (matching what
	# Gool.get_material_eq_for_material returns), push to the inner
	# widget, and return true. Otherwise return false — caller is
	# expected to remove this visualizer from the panel.
	#
	# The intensity multiplier is 1.0 here: the per-bus biquad gain
	# values ALREADY reflect any intensity scaling that
	# _apply_scaled_material_eq_to_bus applied at impact time.
	# Visualizing at intensity=1.0 shows exactly what's hitting the
	# signal right now, no further multiplication.
	func update_from_effects(effects: Array) -> bool:
		if effects.size() < 3:
			return false
		for i in range(3):
			var e_v: Variant = effects[i]
			if not (e_v is Dictionary):
				return false
			if int((e_v as Dictionary).get("kind", 0)) != _EFFECT_KIND_BIQUAD:
				return false

		var curve: Dictionary = _curve_from_three_biquads(effects)
		_curve_view.curve = curve
		return true

	# Static-ish helper: pack three biquad effect dicts into the
	# MaterialEqCurveView curve dict shape. Caller has verified the
	# three entries are biquads. Missing params default to neutral
	# values (0 dB gain at the band's typical knee/center) so a
	# partially-bypassed chain still renders without crashing.
	static func _curve_from_three_biquads(effects: Array) -> Dictionary:
		var low_params: Dictionary = (effects[0] as Dictionary).get(
				"params", {}) as Dictionary
		var mid_params: Dictionary = (effects[1] as Dictionary).get(
				"params", {}) as Dictionary
		var high_params: Dictionary = (effects[2] as Dictionary).get(
				"params", {}) as Dictionary
		return {
			"low_freq_hz":   float(low_params.get(_PARAM_CUTOFF_HZ, 200.0)),
			"low_gain_db":   float(low_params.get(_PARAM_GAIN_DB,   0.0)),
			"mid_freq_hz":   float(mid_params.get(_PARAM_CUTOFF_HZ, 1000.0)),
			"mid_q":         float(mid_params.get(_PARAM_Q,         1.0)),
			"mid_gain_db":   float(mid_params.get(_PARAM_GAIN_DB,   0.0)),
			"high_freq_hz":  float(high_params.get(_PARAM_CUTOFF_HZ, 6000.0)),
			"high_gain_db":  float(high_params.get(_PARAM_GAIN_DB,   0.0)),
			"is_neutral":    false,
		}


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
		# Reverb (v0.29.0+ Dattorro plate). IDs 9 and 10 were renamed
		# from "Size" and "Damping" — same numeric IDs, refreshed labels.
		# IDs 23/24/25 are new.
		9:  {"label": "Decay", "unit": "", "min": 0.0, "max": 1.0,
			"curve": "linear", "fmt": "%0.2f"},
		10: {"label": "HF Damp", "unit": "", "min": 0.0, "max": 1.0,
			"curve": "linear", "fmt": "%0.2f"},
		11: {"label": "Wet", "unit": "dB", "min": -60.0, "max": 6.0,
			"curve": "linear", "fmt": "%+0.1f"},
		23: {"label": "Predelay", "unit": "ms", "min": 0.0, "max": 200.0,
			"curve": "linear", "fmt": "%0.0f"},
		24: {"label": "LF Damp", "unit": "", "min": 0.0, "max": 1.0,
			"curve": "linear", "fmt": "%0.2f"},
		25: {"label": "Diffusion", "unit": "", "min": 0.0, "max": 1.0,
			"curve": "linear", "fmt": "%0.2f"},
		# Reverb dry passthrough (v0.29.5). Matches Wet's range so the
		# pair feels symmetric in the dock; default 0 dB means the
		# source signal passes through unchanged alongside the wet
		# field. For send/return routing, drag this to -60.
		26: {"label": "Dry", "unit": "dB", "min": -60.0, "max": 6.0,
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
		# v0.69.0: Mode and Tone — exposing existing engine params.
		# Mode (id 27) is the SaturationMode enum (Tanh/Tube/Tape/Diode)
		# — each maps to a different shape function with its own
		# useful drive range internally, so swapping modes is a real
		# tonal change, not just a curve tweak. Rendered via the same
		# discrete-OptionButton path the Compressor DetectionMode
		# (id 18) already uses.
		# Tone (id 28) is the v0.59.0 Phase 4 tilt: -1 darkens (more
		# lows, less highs), +1 brightens. Centered at 0 = flat.
		27: {"label": "Mode", "unit": "", "min": 0.0, "max": 3.0,
			"curve": "discrete",
			"choices": ["Tanh", "Tube", "Tape", "Diode"]},
		28: {"label": "Tone", "unit": "", "min": -1.0, "max": 1.0,
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
		# Reverb (v0.29.0): Predelay → Decay → LF/HF Damp → Diffusion → Dry → Wet.
		# Predelay at top because it's the strongest "what size of space?"
		# cue; the damping pair sits together since designers usually tune
		# them in tandem; Dry/Wet pair stays trailing per the dock
		# convention (Dry added v0.29.5).
		4: [23, 9, 24, 10, 25, 26, 11],
		# v0.69.0: Saturation expanded from [19, 21, 22, 20] to include
		# Mode (27) at top — the architectural choice that determines
		# everything else — followed by Drive, Tone, Output, Bias,
		# Mix (Mix last per dock convention).
		5: [27, 19, 28, 21, 22, 20],
	}

	# Emitted on slider/option change. Outer dock forwards via
	# _debugger_plugin.send_set_effect_parameter.
	signal param_changed(bus_name: String, effect_index: int,
			param_id: int, value: float)

	# v0.28.8 topology signals. Outer dock routes to GoolConfigModel.
	# Direction is -1 for "move up" (toward index 0, earlier in signal
	# flow) and +1 for "move down".
	# v0.64.2: add_effect_requested gains preset_id (third arg).
	# Empty string means "use kind defaults" (the v0.64.1 behavior);
	# non-empty means "use this preset" — currently only
	# master_control honors a non-empty preset_id, but the signal
	# shape is generic for future use.
	signal add_effect_requested(bus_name: String, kind_string: String,
			preset_id: String)
	signal remove_effect_requested(bus_name: String, effect_index: int)
	signal move_effect_requested(bus_name: String, effect_index: int,
			direction: int)

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

	# v0.52.0: flow-diagram layout for the effect chain.
	#
	# Replaces v0.28.x's all-effects-stacked-vertically layout. Now
	# the panel renders as:
	#
	#   ┌────────────────────────────────────────────────┐
	#   │ [Reverb]→[Compressor]→[Biquad]      [+ Add]    │  ← flow row
	#   │  (selected tile highlighted)                    │
	#   ├────────────────────────────────────────────────┤
	#   │ Reverb                            [↑] [↓] [×]  │  ← param section
	#   │ Decay   ━━━●━━━━━  0.65                        │     (selected effect only)
	#   │ HF Damp ━━━━━●━━━  0.30                        │
	#   │ ...                                             │
	#   └────────────────────────────────────────────────┘
	#
	# Click any tile to switch which effect's params are displayed.
	# Signal flow (left → right) is now spatially literal — you can
	# see at a glance "send goes through HPF, then Reverb, then LPF"
	# instead of scrolling through a tall list.
	var _selected_effect_idx: int = 0
	var _effects_cache: Array = []
	# v0.52.0: remember which bus we last built for. build_from_effects
	# may be called periodically with refreshed values (poll-driven);
	# resetting selection on every call would constantly snap the
	# user's chosen effect back to the first one. Only reset selection
	# when the bus context actually changes.
	var _last_built_bus_name: String = ""
	const FLOW_ARROW := " → "
	const COLOR_TILE_SELECTED := Color(0.36, 0.55, 0.85)
	const COLOR_TILE_INACTIVE := Color(0.20, 0.22, 0.26)
	const COLOR_ARROW := Color(0.55, 0.58, 0.62)

	func build_from_effects(effects: Array) -> void:
		_effects_cache = effects.duplicate()
		# v0.52.0: reset selection if this is a different bus than
		# we last built for. Same bus = preserve selection through
		# poll refreshes.
		if bus_name != _last_built_bus_name:
			_selected_effect_idx = 0
			_last_built_bus_name = bus_name
		# Clamp selection in case the chain shrank (e.g. user removed
		# the previously-selected effect; default back to first).
		if _selected_effect_idx >= _effects_cache.size():
			_selected_effect_idx = 0
		if _selected_effect_idx < 0:
			_selected_effect_idx = 0
		_rebuild_panel_ui()

	# Internal: rebuild the entire panel UI from _effects_cache +
	# _selected_effect_idx. Called from build_from_effects and from
	# the tile-click handler.
	func _rebuild_panel_ui() -> void:
		_control_map.clear()
		_value_label_map.clear()
		for child in _vbox.get_children():
			child.queue_free()

		var n: int = _effects_cache.size()

		# v0.61.1 — Phase 6.E.2: bus EQ visualization. If the chain
		# starts with three biquads (the material EQ convention), show
		# their cumulative frequency response above the flow row. The
		# visualizer is recreated on every rebuild, which mirrors how
		# the flow row and param section also get recreated — keeps
		# the UI state simple, costs one Control allocation per poll
		# (cheap; n=1 here, not per-effect).
		var eq_viz := _BusEqVisualizer.new()
		if eq_viz.update_from_effects(_effects_cache):
			_vbox.add_child(eq_viz)
		else:
			eq_viz.queue_free()

		# Flow row at top. Even with zero effects we still want the
		# "+ Add Effect" affordance visible.
		_vbox.add_child(_build_flow_row())

		# Detail section for the selected effect. Skip if empty chain.
		if n > 0 and _selected_effect_idx < n:
			var e: Dictionary = _effects_cache[_selected_effect_idx]
			var kind: int = int(e.get("kind", 0))
			var kind_name: String = String(e.get("kind_name", "Effect"))
			var params: Dictionary = e.get("params", {})
			_vbox.add_child(_build_effect_section(
					_selected_effect_idx, n, kind, kind_name, params))

	# v0.52.0: build the horizontal flow row of effect tiles with
	# arrows between, plus a trailing "+ Add" button.
	func _build_flow_row() -> Control:
		var outer := PanelContainer.new()
		var sb := StyleBoxFlat.new()
		sb.bg_color = COLOR_PANEL_BG
		sb.content_margin_left = 4.0
		sb.content_margin_right = 4.0
		sb.content_margin_top = 4.0
		sb.content_margin_bottom = 4.0
		outer.add_theme_stylebox_override("panel", sb)

		var row := HBoxContainer.new()
		row.add_theme_constant_override("separation", 0)
		outer.add_child(row)

		var n: int = _effects_cache.size()
		for i in range(n):
			var e: Dictionary = _effects_cache[i]
			var kind_name: String = String(e.get("kind_name", "Effect"))
			row.add_child(_build_effect_tile(i, kind_name,
					i == _selected_effect_idx))
			# Arrow separator between tiles (not after the last).
			if i < n - 1:
				row.add_child(_build_flow_arrow())

		# Spacer pushes the Add button right.
		var spacer := Control.new()
		spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(spacer)

		row.add_child(_build_add_effect_button())
		return outer

	# Build one clickable effect tile. Selected state is rendered
	# via background fill + bold label tint; inactive tiles get a
	# muted appearance so the eye lands on the selected one first.
	func _build_effect_tile(idx: int, kind_name: String,
			selected: bool) -> Control:
		var btn := Button.new()
		btn.text = kind_name
		btn.focus_mode = Control.FOCUS_NONE
		btn.custom_minimum_size = Vector2(0, 24)
		btn.tooltip_text = "Click to edit %s params" % kind_name
		# Highlight via StyleBoxFlat. Selected = filled with accent;
		# inactive = subtle dark fill.
		var sb := StyleBoxFlat.new()
		sb.bg_color = COLOR_TILE_SELECTED if selected else COLOR_TILE_INACTIVE
		sb.corner_radius_top_left = 3
		sb.corner_radius_top_right = 3
		sb.corner_radius_bottom_left = 3
		sb.corner_radius_bottom_right = 3
		sb.content_margin_left = 8
		sb.content_margin_right = 8
		sb.content_margin_top = 4
		sb.content_margin_bottom = 4
		# Hover style — slightly lighter than the inactive fill, never
		# brighter than the selected accent so the affordance stays
		# subtle. Same shape for both selected and inactive (Godot
		# Buttons render their own hover frame if we don't override).
		var sb_hover := StyleBoxFlat.new()
		sb_hover.bg_color = (COLOR_TILE_SELECTED if selected
				else Color(0.28, 0.30, 0.34))
		sb_hover.corner_radius_top_left = 3
		sb_hover.corner_radius_top_right = 3
		sb_hover.corner_radius_bottom_left = 3
		sb_hover.corner_radius_bottom_right = 3
		sb_hover.content_margin_left = 8
		sb_hover.content_margin_right = 8
		sb_hover.content_margin_top = 4
		sb_hover.content_margin_bottom = 4
		btn.add_theme_stylebox_override("normal", sb)
		btn.add_theme_stylebox_override("hover", sb_hover)
		btn.add_theme_stylebox_override("pressed", sb)
		btn.add_theme_color_override("font_color",
				Color.WHITE if selected else Color(0.85, 0.88, 0.92))
		btn.pressed.connect(_on_effect_tile_pressed.bind(idx))
		return btn

	# Build a small arrow glyph (Label with "→") between tiles. Done
	# as a Label rather than custom draw because Godot's text layout
	# handles vertical centering with the tile heights automatically.
	func _build_flow_arrow() -> Control:
		var arrow := Label.new()
		arrow.text = "→"
		arrow.add_theme_color_override("font_color", COLOR_ARROW)
		arrow.add_theme_constant_override("outline_size", 0)
		arrow.custom_minimum_size = Vector2(20, 0)
		arrow.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		arrow.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
		return arrow

	# Tile-click handler — update selection and rebuild. The
	# Button's toggle_mode would be cleaner but Godot's button group
	# state doesn't survive _rebuild_panel_ui (which frees children),
	# so we manage selection state explicitly here.
	func _on_effect_tile_pressed(idx: int) -> void:
		if idx == _selected_effect_idx:
			return
		_selected_effect_idx = idx
		_rebuild_panel_ui()

	# v0.28.8: "+ Add Effect" button that opens a PopupMenu of the
	# five effect kinds. Selection emits add_effect_requested for
	# the outer dock to route to the model.
	func _build_add_effect_button() -> Control:
		var btn := Button.new()
		btn.text = "+ Add Effect"
		btn.focus_mode = Control.FOCUS_NONE
		btn.tooltip_text = "Append a new effect to this bus's chain"
		btn.pressed.connect(_on_add_effect_button_pressed.bind(btn))
		return btn

	# Kind labels matching EFFECT_KIND_ORDER in GoolConfigModel.
	# Kept local so _EffectsPanel doesn't need to import the model.
	#
	# v0.64.2: master_control moved out of this flat list and into
	# its own submenu (see _MASTER_CONTROL_PRESETS below). The
	# Add Effect picker now has 5 top-level entries plus one
	# "Master Control >" submenu_item that expands to 5 preset
	# choices — better UX than picking the effect type and getting
	# silently fixed Standard FPS defaults.
	const _KIND_PICKER_LABELS: Array = [
		["gain", "Gain"],
		["biquad", "Biquad Filter"],
		["compressor", "Compressor"],
		["saturation", "Saturation"],
		["reverb", "Reverb"],
	]
	# v0.64.2: preset entries for the Master Control submenu.
	# [preset_id, display_label] tuples — the preset_id is the key
	# into config_model.gd's MASTER_CONTROL_PRESETS dict. Order is
	# Standard first (most-common case), then ascending intensity,
	# with Bypass last. Display labels match what the .tres files
	# carry in their preset_name fields for consistency.
	const _MASTER_CONTROL_PRESETS: Array = [
		["standard_fps",        "Standard FPS"],
		["subtle_glue",         "Subtle glue"],
		["cinema_quiet",        "Cinema / quiet"],
		["loud_and_aggressive", "Loud and aggressive"],
		["none_bypass",         "None / bypass"],
	]

	func _on_add_effect_button_pressed(anchor_btn: Button) -> void:
		var menu := PopupMenu.new()
		# IDs match the index in _KIND_PICKER_LABELS so id_pressed
		# can look up the kind string. Five basic kinds get top-level
		# entries; master_control is a submenu (see below).
		for i in range(_KIND_PICKER_LABELS.size()):
			menu.add_item(String((_KIND_PICKER_LABELS[i] as Array)[1]), i)
		# v0.64.2: master_control submenu. add_submenu_item attaches
		# a child PopupMenu by name; the parent menu doesn't fire
		# id_pressed for submenu items (instead the child menu fires
		# its OWN id_pressed when one of its items is picked).
		var preset_menu := PopupMenu.new()
		preset_menu.name = "MasterControlPresets"
		for i in range(_MASTER_CONTROL_PRESETS.size()):
			preset_menu.add_item(
					String((_MASTER_CONTROL_PRESETS[i] as Array)[1]), i)
		preset_menu.id_pressed.connect(_on_master_preset_selected)
		menu.add_child(preset_menu)
		menu.add_submenu_item("Master Control", preset_menu.name)
		menu.id_pressed.connect(_on_kind_picker_selected)
		# Free the menu (and its submenu, transitively) when it
		# closes so we don't leak one per click. The submenu is a
		# child of the parent menu, so queue_freeing the parent
		# also frees the child.
		menu.close_requested.connect(menu.queue_free)
		menu.popup_hide.connect(menu.queue_free)
		anchor_btn.add_child(menu)
		# v0.64.1: position fix. PopupMenu.popup() interprets its
		# Rect2i argument in SCREEN coordinates, not viewport coords.
		# Pre-v0.64.1 used Control.global_position, which returns
		# the button's position inside its viewport — fine for a
		# fullscreen window but wrong inside the Godot editor where
		# the dock lives in an embedded SubViewport offset from the
		# screen origin. Result was the menu jumping to the lower-
		# left of the screen instead of appearing next to the
		# button.
		#
		# Control.get_screen_position() walks the window hierarchy
		# and returns the actual screen-space position, so the
		# menu now lands directly below the button regardless of
		# the editor's window layout.
		var origin: Vector2 = anchor_btn.get_screen_position() \
				+ Vector2(0, anchor_btn.size.y)
		menu.popup(Rect2i(Vector2i(origin), Vector2i(0, 0)))

	func _on_kind_picker_selected(id: int) -> void:
		# Only fires for the 5 basic kinds. master_control comes
		# through _on_master_preset_selected since it's a submenu.
		if id < 0 or id >= _KIND_PICKER_LABELS.size():
			return
		var kind_string: String = String((_KIND_PICKER_LABELS[id] as Array)[0])
		add_effect_requested.emit(bus_name, kind_string, "")

	# v0.64.2: master_control submenu's id_pressed handler. Fires
	# when the user picks a specific preset from the "Master
	# Control >" submenu. Looks up the preset_id from the parallel
	# _MASTER_CONTROL_PRESETS list and emits add_effect_requested
	# with kind="master_control" plus the preset_id, so the outer
	# dock can route it through GoolConfigModel.add_effect with
	# the matching params dict.
	func _on_master_preset_selected(id: int) -> void:
		if id < 0 or id >= _MASTER_CONTROL_PRESETS.size():
			return
		var preset_id: String = String((_MASTER_CONTROL_PRESETS[id] as Array)[0])
		add_effect_requested.emit(bus_name, "master_control", preset_id)

	func _on_move_up_pressed(idx: int) -> void:
		move_effect_requested.emit(bus_name, idx, -1)

	func _on_move_down_pressed(idx: int) -> void:
		move_effect_requested.emit(bus_name, idx, +1)

	func _on_remove_pressed(idx: int) -> void:
		remove_effect_requested.emit(bus_name, idx)

	func _build_effect_section(effect_idx: int, total_count: int,
			kind: int, kind_name: String, params: Dictionary) -> Control:
		var section := VBoxContainer.new()
		section.add_theme_constant_override("separation", 2)

		# Header band: distinct background so the section boundary is
		# visually clear when multiple effects are stacked. v0.28.8:
		# header now hosts ↑/↓/× topology buttons on the right edge.
		var header := PanelContainer.new()
		var header_sb := StyleBoxFlat.new()
		header_sb.bg_color = COLOR_HEADER_BG
		header_sb.content_margin_left = 6.0
		header_sb.content_margin_right = 6.0
		header_sb.content_margin_top = 2.0
		header_sb.content_margin_bottom = 2.0
		header.add_theme_stylebox_override("panel", header_sb)

		var header_row := HBoxContainer.new()
		header_row.add_theme_constant_override("separation", 2)
		header.add_child(header_row)

		var header_label := Label.new()
		header_label.text = kind_name
		header_label.add_theme_color_override("font_color", COLOR_HEADER_TEXT)
		header_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		header_row.add_child(header_label)

		# ↑ Move up: disabled on the first effect in the chain.
		var btn_up := Button.new()
		btn_up.text = "↑"
		btn_up.flat = true
		btn_up.focus_mode = Control.FOCUS_NONE
		btn_up.custom_minimum_size = Vector2(20, 18)
		btn_up.disabled = (effect_idx == 0)
		btn_up.tooltip_text = "Move effect earlier in signal flow"
		# capture-by-value via bind() since the lambda's idx would
		# otherwise close over the loop variable.
		btn_up.pressed.connect(_on_move_up_pressed.bind(effect_idx))
		header_row.add_child(btn_up)

		# ↓ Move down: disabled on the last effect.
		var btn_down := Button.new()
		btn_down.text = "↓"
		btn_down.flat = true
		btn_down.focus_mode = Control.FOCUS_NONE
		btn_down.custom_minimum_size = Vector2(20, 18)
		btn_down.disabled = (effect_idx >= total_count - 1)
		btn_down.tooltip_text = "Move effect later in signal flow"
		btn_down.pressed.connect(_on_move_down_pressed.bind(effect_idx))
		header_row.add_child(btn_down)

		# × Remove. Outer dock catches the signal and shows a
		# ConfirmationDialog before actually removing.
		var btn_remove := Button.new()
		btn_remove.text = "×"
		btn_remove.flat = true
		btn_remove.focus_mode = Control.FOCUS_NONE
		btn_remove.custom_minimum_size = Vector2(20, 18)
		btn_remove.tooltip_text = "Remove this effect"
		btn_remove.add_theme_color_override("font_color",
				Color(1.0, 0.55, 0.55))
		btn_remove.pressed.connect(_on_remove_pressed.bind(effect_idx))
		header_row.add_child(btn_remove)

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


# v0.51.0: tiny rolling-history sparkline for the master peak card.
# Renders a polyline of the last N samples behind/below the dB
# number. Samples are pushed each _poll (~30 Hz) so a 60-sample
# buffer covers ~2 seconds — short enough to feel responsive,
# long enough to read a pumping compressor's shape at a glance.
class _PeakSparkline extends Control:
	const _BUFFER_SIZE: int = 60
	const _DB_FLOOR: float = -36.0
	const _DB_CEIL: float = 6.0

	var _samples: PackedFloat32Array = PackedFloat32Array()
	var theme_color: Color = Color(0.42, 0.65, 0.95)

	func _ready() -> void:
		_samples.resize(_BUFFER_SIZE)
		for i in range(_BUFFER_SIZE):
			_samples[i] = _DB_FLOOR

	# Push one linear-domain sample. Converted to dB and clamped to
	# the visible range. Oldest sample is dropped.
	func push_sample(linear_value: float) -> void:
		if _samples.is_empty():
			_ready()
		var db: float
		if linear_value <= 0.00001:
			db = _DB_FLOOR
		else:
			db = linear_to_db(linear_value)
		db = clamp(db, _DB_FLOOR, _DB_CEIL)
		# Shift left, append newest at the tail.
		for i in range(_BUFFER_SIZE - 1):
			_samples[i] = _samples[i + 1]
		_samples[_BUFFER_SIZE - 1] = db
		queue_redraw()

	func _draw() -> void:
		var w: float = size.x
		var h: float = size.y
		if w < 4.0 or h < 2.0:
			return
		# Faint baseline at 0 dBFS so users have a "clip line" cue.
		var zero_y: float = h * (1.0 - (0.0 - _DB_FLOOR) / (_DB_CEIL - _DB_FLOOR))
		var baseline_color: Color = theme_color
		baseline_color.a = 0.20
		draw_line(Vector2(0, zero_y), Vector2(w, zero_y),
				baseline_color, 1.0)
		# Polyline of samples
		var pts := PackedVector2Array()
		var step: float = w / float(_BUFFER_SIZE - 1)
		for i in range(_BUFFER_SIZE):
			var db: float = _samples[i]
			var norm: float = (db - _DB_FLOOR) / (_DB_CEIL - _DB_FLOOR)
			var y: float = h * (1.0 - norm)
			pts.append(Vector2(i * step, y))
		if pts.size() >= 2:
			# Use draw_polyline (anti-aliased) for the trace.
			draw_polyline(pts, theme_color, 1.5, true)


# v0.79.2: Read the current gool version from plugin.cfg. plugin.cfg
# is the authoritative source for the addon-side version (verified
# in sync with engine version.h via the CI version-sync job). Falls
# back to "0.0.0" if the file is somehow missing or malformed, which
# makes the update check effectively always-fire (every release will
# look newer than 0.0.0). That's the right failure mode — if we
# can't determine our version, prefer to over-notify rather than
# silently skip.
func _get_current_gool_version() -> String:
	var cfg := ConfigFile.new()
	var err := cfg.load("res://addons/gool/plugin.cfg")
	if err != OK:
		return "0.0.0"
	return str(cfg.get_value("plugin", "version", "0.0.0"))


# v0.79.3: Open the help panel. The panel is a Window subclass that
# mounts itself onto the editor's base control, opens non-modally
# (user can keep working while the panel is open), and self-frees
# on close_requested. Idempotent: if a help panel is already showing
# we bring it to the foreground rather than spawning a duplicate.
func _on_help_button_pressed() -> void:
	var existing := _find_existing_help_panel()
	if existing != null:
		existing.grab_focus()
		existing.move_to_foreground()
		return
	var help_script := load("res://addons/gool/editor/help_panel.gd")
	if help_script == null:
		push_warning("[gool] help_panel.gd not found — was the addon "
				+ "fully extracted? Try re-running the install script.")
		return
	var panel: Window = help_script.new()
	# Parent on the editor's base control so the popup is positioned
	# relative to the Godot main window, not the dock.
	EditorInterface.get_base_control().add_child(panel)
	panel.popup_centered()


# Search the editor's base control children for an existing help
# panel instance. Returns the Window if found, else null. Lets us
# avoid spawning duplicate panels when the user clicks Help while
# one is already open.
func _find_existing_help_panel() -> Window:
	var base := EditorInterface.get_base_control()
	if base == null:
		return null
	for child in base.get_children():
		if child is Window and child.title == "gool — Help":
			return child
	return null
