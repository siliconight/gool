# addons/gool/editor/sound_bank_panel.gd
#
# v0.53.0: Sound Bank panel for the mixer dock.
#
# Lives as the second tab next to "Mixer" in the dock. Lets sound
# designers browse every GoolSoundBank / GoolFolderSoundBank in
# the project and audition .wav/.ogg/.mp3/.flac entries directly
# from the editor — closes the "I want to hear what 'gunshot_remote'
# sounds like without F5'ing the whole game" gap.
#
# Audition mechanism:
#
# The audio flows through Godot's built-in AudioStreamPlayer, NOT
# through the gool engine. gool's engine only runs during F5 play
# sessions; spinning it up for editor-time auditioning would be
# expensive and tangle a lot of state. The pragmatic compromise
# is: the panel plays the raw sample so the designer can hear the
# *source* material; for full pipeline auditioning (with reverb,
# compression, sidechain ducking, etc.) they still F5.
#
# This is called out in the panel's tooltip so it isn't a surprise.
#
# Bank discovery:
#
#   1. Walk res:// recursively, collect every .tres file.
#   2. For each candidate, peek the first ~256 bytes to find the
#      [gd_resource type="..."] header. Cheap — no full resource
#      load. Filter to types we recognize.
#   3. load() only the matches. Sort banks by file path for stable
#      display order.
#   4. For each bank, enumerate its sounds dict. Folder banks
#      also expose sounds_category + sounds_looping per entry;
#      base GoolSoundBanks fall back to default_category.
#   5. AudioStreams expose get_length() → seconds; we use that for
#      the duration column.

@tool
class_name GoolSoundBankPanel
extends Control

# Icons drawn as text rather than images so the panel doesn't need
# .svg assets. Glyphs render via Godot's default UI font.
const ICON_PLAY: String   = "▶"
const ICON_STOP: String   = "■"
const ICON_LOOP: String   = "⟲"
const ICON_EXPAND: String = "▼"
const ICON_COLLAPSE: String = "▶"

# Class names we recognize as sound banks. Strings rather than
# class references because the .tres header peek doesn't load the
# resource — we work in raw text first to skip the expensive
# load() for files that aren't banks.
const _BANK_CLASS_NAMES: Array = [
	"GoolSoundBank",
	"GoolFolderSoundBank",
]

# How many bytes to read off the head of each .tres for the class
# detection. The header is always something like:
#   [gd_resource type="GoolSoundBank" script_class="GoolSoundBank" ...]
# Well within 256 bytes.
const _TRES_HEADER_PEEK_BYTES: int = 256

# Category int → display label. Matches runtime_singleton.gd's
# CATEGORY_* constants.
const _CATEGORY_LABELS: Array = [
	"SFX", "Voice", "Music", "Ambience", "UI", "Dialogue",
]

# ─── State ─────────────────────────────────────────────────────

# Discovered banks: array of {path: String, resource: GoolSoundBank}.
# Sorted by path. Rebuilt on rescan; partially preserved across
# refreshes if you re-load the same path (we drop the resource ref
# every time, deliberately, so .tres changes on disk are picked up).
var _banks: Array = []

# Bank path → expanded bool. Persists across rebuilds within the
# same panel lifetime so collapsing/expanding survives a rescan.
var _expanded_banks: Dictionary = {}

# Active filter text (from the search LineEdit). Lowercased at set
# time so per-entry matching is a single contains() call.
var _search_text: String = ""

# The single AudioStreamPlayer that handles audition. Created in
# _ready, reused for every ▶ click. Audition is non-spatial,
# non-mixed — the raw sample, full bus 0.
var _player: AudioStreamPlayer = null

# Tracks what's currently playing so we can:
#   - swap the ▶ icon to ■ on the active row
#   - stop it when the user clicks another row's ▶
#   - clear the indicator when finished
# Format: {bank_path: String, sound_name: String} or empty Dict.
var _current_playing: Dictionary = {}

# UI roots, cached so _rebuild_bank_list can find them without
# tree traversal.
var _root_vbox: VBoxContainer = null
var _toolbar_stats_label: Label = null
var _search_edit: LineEdit = null
var _bank_list_vbox: VBoxContainer = null
var _empty_state: Control = null
var _now_playing_label: Label = null

# Lazy reference to the parent dock — used for the theme helpers
# (_theme_color and friends). Walked up the tree on first access.
var _mixer_dock = null

# ─── Lifecycle ─────────────────────────────────────────────────

func _ready() -> void:
	custom_minimum_size = Vector2(0, 200)
	_player = AudioStreamPlayer.new()
	# Bus 0 is "Master" in every Godot project by default; auditioning
	# bypasses gool's bus topology (see header comment).
	_player.bus = "Master"
	_player.finished.connect(_on_player_finished)
	add_child(_player)

	_root_vbox = VBoxContainer.new()
	_root_vbox.anchor_right = 1.0
	_root_vbox.anchor_bottom = 1.0
	_root_vbox.add_theme_constant_override("separation", 4)
	add_child(_root_vbox)

	_root_vbox.add_child(_build_toolbar())
	_root_vbox.add_child(_build_search_row())

	# Bank list goes inside a scroll container so very large
	# projects with many banks remain scannable.
	var scroll := ScrollContainer.new()
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_root_vbox.add_child(scroll)

	_bank_list_vbox = VBoxContainer.new()
	_bank_list_vbox.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_bank_list_vbox.add_theme_constant_override("separation", 6)
	scroll.add_child(_bank_list_vbox)

	# Empty-state card lives ALSO inside the scroll so it centers
	# nicely. Hidden when we have banks.
	_empty_state = _build_empty_state()
	_bank_list_vbox.add_child(_empty_state)

	_root_vbox.add_child(_build_now_playing_bar())

	# Initial scan + render.
	rescan_and_rebuild()


# Public entry — re-scan the project + rebuild the list. Called
# from _ready and from the toolbar's Rescan button. Also safe to
# call externally if other code (the dock's mixer save flow, e.g.)
# adds new banks and wants the panel refreshed.
func rescan_and_rebuild() -> void:
	_banks = _discover_banks()
	_rebuild_bank_list()
	_update_toolbar_stats()


# ─── UI construction ───────────────────────────────────────────

func _build_toolbar() -> Control:
	var panel := PanelContainer.new()
	panel.add_theme_stylebox_override("panel",
			_get_chrome_stylebox(10))

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 12)
	panel.add_child(row)

	var title_block := VBoxContainer.new()
	title_block.add_theme_constant_override("separation", 2)
	row.add_child(title_block)

	var title := Label.new()
	title.text = "Sound Bank"
	title.add_theme_color_override("font_color",
			_get_text_primary())
	title.add_theme_font_size_override("font_size", 14)
	title_block.add_child(title)

	_toolbar_stats_label = Label.new()
	_toolbar_stats_label.text = "Scanning…"
	_toolbar_stats_label.add_theme_color_override("font_color",
			_get_text_secondary())
	_toolbar_stats_label.add_theme_font_size_override("font_size", 11)
	title_block.add_child(_toolbar_stats_label)

	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(spacer)

	var rescan_btn := Button.new()
	rescan_btn.text = "Rescan"
	rescan_btn.tooltip_text = (
			"Walk res:// for .tres files and reload any "
			+ "GoolSoundBank / GoolFolderSoundBank found.")
	rescan_btn.pressed.connect(rescan_and_rebuild)
	row.add_child(rescan_btn)

	return panel


func _build_search_row() -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)

	var lbl := Label.new()
	lbl.text = "Filter:"
	lbl.add_theme_color_override("font_color", _get_text_secondary())
	row.add_child(lbl)

	_search_edit = LineEdit.new()
	_search_edit.placeholder_text = "Type to filter by sound name…"
	_search_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_search_edit.clear_button_enabled = true
	_search_edit.text_changed.connect(_on_search_text_changed)
	row.add_child(_search_edit)

	return row


func _build_empty_state() -> Control:
	var center := CenterContainer.new()
	center.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	center.size_flags_vertical = Control.SIZE_EXPAND_FILL
	center.custom_minimum_size = Vector2(360, 120)

	var card := PanelContainer.new()
	card.add_theme_stylebox_override("panel", _get_chrome_stylebox(20))
	center.add_child(card)

	var vbox := VBoxContainer.new()
	vbox.add_theme_constant_override("separation", 8)
	card.add_child(vbox)

	var heading := Label.new()
	heading.text = "No sound banks found"
	heading.add_theme_color_override("font_color", _get_text_primary())
	heading.add_theme_font_size_override("font_size", 14)
	heading.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	vbox.add_child(heading)

	var body := Label.new()
	body.text = (
			"Create a GoolSoundBank or GoolFolderSoundBank "
			+ ".tres anywhere in res:// and click Rescan.")
	body.add_theme_color_override("font_color", _get_text_secondary())
	body.add_theme_font_size_override("font_size", 11)
	body.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	body.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	body.custom_minimum_size = Vector2(320, 0)
	vbox.add_child(body)

	return center


func _build_now_playing_bar() -> Control:
	var panel := PanelContainer.new()
	panel.add_theme_stylebox_override("panel", _get_chrome_stylebox(8))

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	panel.add_child(row)

	_now_playing_label = Label.new()
	_now_playing_label.text = "Idle. Click ▶ on any entry to audition."
	_now_playing_label.add_theme_color_override("font_color",
			_get_text_secondary())
	_now_playing_label.add_theme_font_size_override("font_size", 11)
	_now_playing_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(_now_playing_label)

	var stop_btn := Button.new()
	stop_btn.text = ICON_STOP + " Stop"
	stop_btn.tooltip_text = "Stop the current audition"
	stop_btn.pressed.connect(_stop_audition)
	row.add_child(stop_btn)

	return panel


# ─── Bank discovery ────────────────────────────────────────────

# Walk res:// for .tres files, peek each header to find sound bank
# resources, load and return them sorted by file path.
func _discover_banks() -> Array:
	var paths := PackedStringArray()
	_collect_tres_files("res://", paths)
	var matched: Array = []
	for p in paths:
		var class_name_str: String = _peek_tres_class(p)
		if class_name_str == "":
			continue
		if not (class_name_str in _BANK_CLASS_NAMES):
			continue
		var res: Resource = load(p)
		if res == null:
			# Could happen if the file is malformed or references a
			# missing dependency. We logged at load time; just skip.
			continue
		# Trust the load — Godot will have set the right class.
		matched.append({"path": p, "resource": res})
	# Stable display order regardless of filesystem iteration.
	matched.sort_custom(func(a, b): return a.path < b.path)
	return matched


# Recursively collect .tres file paths. Skips addons/, .godot/,
# build/, dist/ which never contain user banks and burn time.
func _collect_tres_files(dir_path: String, out: PackedStringArray) -> void:
	var dir := DirAccess.open(dir_path)
	if dir == null:
		return
	dir.list_dir_begin()
	while true:
		var entry: String = dir.get_next()
		if entry == "":
			break
		if entry == "." or entry == "..":
			continue
		if entry == "addons" or entry == ".godot" \
				or entry == "build" or entry == "dist" \
				or entry.begins_with("."):
			continue
		var full_path: String = dir_path.path_join(entry)
		if dir.current_is_dir():
			_collect_tres_files(full_path, out)
		elif entry.ends_with(".tres"):
			out.append(full_path)
	dir.list_dir_end()


# Read the first ~256 bytes of a .tres file, parse out the
# script_class attribute from the [gd_resource …] header. Returns
# "" if not parseable. Much cheaper than load() — most .tres files
# in a project aren't sound banks so we want to fail fast.
func _peek_tres_class(path: String) -> String:
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		return ""
	var header: String = f.get_buffer(_TRES_HEADER_PEEK_BYTES) \
			.get_string_from_utf8()
	f.close()
	# Look for `script_class="..."` first; falls back to `type="..."`
	# for older-format files that don't carry script_class.
	var script_class: String = _extract_attr(header, "script_class")
	if script_class != "":
		return script_class
	return _extract_attr(header, "type")


# Extract `attr="value"` from a string fragment. Returns the
# value or "". Simple state machine — avoids pulling in RegEx.
func _extract_attr(text: String, attr: String) -> String:
	var needle: String = attr + "=\""
	var start: int = text.find(needle)
	if start == -1:
		return ""
	start += needle.length()
	var end: int = text.find("\"", start)
	if end == -1:
		return ""
	return text.substr(start, end - start)


# ─── Bank list rendering ───────────────────────────────────────

func _rebuild_bank_list() -> void:
	# Free all current children except the empty state (we toggle
	# its visibility instead of freeing/recreating it).
	for child in _bank_list_vbox.get_children():
		if child == _empty_state:
			continue
		child.queue_free()

	if _banks.is_empty():
		_empty_state.visible = true
		return
	_empty_state.visible = false

	for bank_entry in _banks:
		var bank_path: String = bank_entry.path
		var bank = bank_entry.resource
		# Default expansion: first bank expanded, others collapsed.
		# Stable across rebuilds via the _expanded_banks dict.
		if not _expanded_banks.has(bank_path):
			_expanded_banks[bank_path] = (bank_path == _banks[0].path)
		_bank_list_vbox.add_child(_build_bank_group(bank_path, bank))


func _build_bank_group(bank_path: String, bank) -> Control:
	var group := VBoxContainer.new()
	group.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	group.add_theme_constant_override("separation", 2)

	group.add_child(_build_bank_header(bank_path, bank))

	if _expanded_banks.get(bank_path, false):
		var sounds_dict: Dictionary = bank.sounds
		# Sort entries alphabetically by name for predictable scan
		# order. Designer-authored banks usually have meaningful
		# names so alpha is a reasonable default.
		var names: Array = sounds_dict.keys()
		names.sort()
		var any_visible: bool = false
		for n in names:
			var name_str: String = String(n)
			if not _matches_filter(name_str):
				continue
			var stream = sounds_dict[n]
			if stream == null:
				continue
			any_visible = true
			group.add_child(_build_entry_row(
					bank_path, bank, name_str, stream))
		if not any_visible and _search_text != "":
			var msg := Label.new()
			msg.text = "  (no entries match the filter)"
			msg.add_theme_color_override("font_color",
					_get_text_secondary())
			msg.add_theme_font_size_override("font_size", 11)
			group.add_child(msg)

	return group


func _build_bank_header(bank_path: String, bank) -> Control:
	var panel := PanelContainer.new()
	panel.add_theme_stylebox_override("panel", _get_chrome_stylebox(6))

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	panel.add_child(row)

	# Expand/collapse toggle. Click anywhere in the header band
	# would be nice but PanelContainer doesn't take gui_input cleanly
	# — keep it scoped to the toggle button.
	var expanded: bool = _expanded_banks.get(bank_path, false)
	var toggle := Button.new()
	toggle.text = ICON_EXPAND if expanded else ICON_COLLAPSE
	toggle.flat = true
	toggle.custom_minimum_size = Vector2(20, 0)
	toggle.focus_mode = Control.FOCUS_NONE
	toggle.pressed.connect(_on_toggle_bank.bind(bank_path))
	row.add_child(toggle)

	# Bank name = basename of the path. Path itself shown as
	# subtitle.
	var name_block := VBoxContainer.new()
	name_block.add_theme_constant_override("separation", 0)
	name_block.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(name_block)

	var name_label := Label.new()
	name_label.text = bank_path.get_file()
	name_label.add_theme_color_override("font_color",
			_get_text_primary())
	name_label.add_theme_font_size_override("font_size", 13)
	name_block.add_child(name_label)

	var path_label := Label.new()
	path_label.text = bank_path
	path_label.add_theme_color_override("font_color",
			_get_text_secondary())
	path_label.add_theme_font_size_override("font_size", 10)
	name_block.add_child(path_label)

	# Entry count + bank type indicator (folder vs base).
	var stats_label := Label.new()
	var entry_count: int = bank.sounds.size() if bank.sounds else 0
	var type_str: String = "folder" if bank.has_method("rescan") else "base"
	stats_label.text = "%d entries · %s" % [entry_count, type_str]
	stats_label.add_theme_color_override("font_color",
			_get_text_secondary())
	stats_label.add_theme_font_size_override("font_size", 11)
	row.add_child(stats_label)

	# Inspect button — opens the bank in the editor inspector.
	var inspect := Button.new()
	inspect.text = "Inspect"
	inspect.flat = true
	inspect.tooltip_text = "Open this bank in the Godot inspector"
	inspect.pressed.connect(_on_inspect_bank.bind(bank))
	row.add_child(inspect)

	return panel


func _build_entry_row(bank_path: String, bank, sound_name: String,
		stream) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	row.custom_minimum_size = Vector2(0, 24)

	# Indent so entries hang under their bank header.
	var indent := Control.new()
	indent.custom_minimum_size = Vector2(24, 0)
	row.add_child(indent)

	# Sound name — primary text, larger weight.
	var name_label := Label.new()
	name_label.text = sound_name
	name_label.add_theme_color_override("font_color",
			_get_text_primary())
	name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(name_label)

	# Category column — fixed width so columns align across rows.
	var category_label := Label.new()
	category_label.text = _resolve_category_label(bank, sound_name)
	category_label.add_theme_color_override("font_color",
			_get_text_secondary())
	category_label.add_theme_font_size_override("font_size", 11)
	category_label.custom_minimum_size = Vector2(70, 0)
	row.add_child(category_label)

	# Loop indicator. Streams in folder banks tagged Music/Ambience
	# get marked looping by the bank's scan logic; base banks don't
	# expose per-entry loop info, so we just check if the AudioStream
	# itself has loop_mode set.
	var loop_label := Label.new()
	loop_label.text = ICON_LOOP if _is_entry_looping(bank, sound_name,
			stream) else " "
	loop_label.add_theme_color_override("font_color",
			_get_text_secondary())
	loop_label.custom_minimum_size = Vector2(16, 0)
	row.add_child(loop_label)

	# Duration column.
	var duration_label := Label.new()
	duration_label.text = _format_duration(stream)
	duration_label.add_theme_color_override("font_color",
			_get_text_secondary())
	duration_label.add_theme_font_size_override("font_size", 11)
	duration_label.custom_minimum_size = Vector2(50, 0)
	duration_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	row.add_child(duration_label)

	# Play button. Icon swaps to STOP when this row is the one
	# currently playing.
	var is_playing: bool = (
			_current_playing.has("bank_path")
			and _current_playing.bank_path == bank_path
			and _current_playing.sound_name == sound_name)
	var play_btn := Button.new()
	play_btn.text = ICON_STOP if is_playing else ICON_PLAY
	play_btn.tooltip_text = (
			"Stop" if is_playing else
			"Audition via AudioStreamPlayer (not gool's pipeline)")
	play_btn.custom_minimum_size = Vector2(28, 0)
	play_btn.pressed.connect(_on_play_pressed.bind(
			bank_path, sound_name, stream))
	row.add_child(play_btn)

	return row


# ─── Per-entry helpers ─────────────────────────────────────────

func _resolve_category_label(bank, sound_name: String) -> String:
	# Folder bank: per-entry category in sounds_category.
	if bank.has_method("rescan") and "sounds_category" in bank:
		var per_entry: Dictionary = bank.sounds_category
		if per_entry.has(sound_name):
			var cat_int: int = int(per_entry[sound_name])
			if cat_int >= 0 and cat_int < _CATEGORY_LABELS.size():
				return _CATEGORY_LABELS[cat_int]
	# Base bank or missing entry: default_category for the whole bank.
	if "default_category" in bank:
		var cat: int = int(bank.default_category)
		if cat >= 0 and cat < _CATEGORY_LABELS.size():
			return _CATEGORY_LABELS[cat]
	return "—"


func _is_entry_looping(bank, sound_name: String, stream) -> bool:
	# Folder bank: per-entry looping flag.
	if bank.has_method("rescan") and "sounds_looping" in bank:
		var per_entry: Dictionary = bank.sounds_looping
		if per_entry.has(sound_name):
			return bool(per_entry[sound_name])
	# Fall back to inspecting the AudioStream itself. Different
	# stream types expose loop via different properties; check the
	# common ones.
	if stream == null:
		return false
	if "loop" in stream:
		return bool(stream.loop)
	if "loop_mode" in stream:
		# Godot's AudioStreamWAV: loop_mode 0 = disabled.
		return int(stream.loop_mode) != 0
	return false


func _format_duration(stream) -> String:
	if stream == null:
		return "—"
	if not stream.has_method("get_length"):
		return "—"
	var seconds: float = float(stream.get_length())
	if seconds <= 0.0:
		# Some procedurally-defined streams return 0 — better to
		# say "—" than confusingly say "0.0s".
		return "—"
	if seconds < 10.0:
		return "%.1fs" % seconds
	if seconds < 60.0:
		return "%.0fs" % seconds
	var minutes: int = int(seconds / 60.0)
	var rem: int = int(seconds) - minutes * 60
	return "%d:%02d" % [minutes, rem]


func _matches_filter(sound_name: String) -> bool:
	if _search_text == "":
		return true
	return sound_name.to_lower().contains(_search_text)


# ─── Audition controls ─────────────────────────────────────────

func _on_play_pressed(bank_path: String, sound_name: String,
		stream) -> void:
	# Click on the active row stops it; click on any other row
	# stops the current and starts the new one.
	var is_active: bool = (
			_current_playing.has("bank_path")
			and _current_playing.bank_path == bank_path
			and _current_playing.sound_name == sound_name)
	if is_active:
		_stop_audition()
		return
	if _player == null:
		return
	_player.stop()
	_player.stream = stream
	_player.play()
	_current_playing = {
		"bank_path": bank_path,
		"sound_name": sound_name,
	}
	_now_playing_label.text = "▶  Playing  %s  ·  %s" % [
		sound_name, bank_path.get_file()]
	_now_playing_label.add_theme_color_override("font_color",
			_get_accent())
	# Rebuild so the active row's button flips to STOP and others
	# remain PLAY. Cheap — only the visible bank's children change.
	_rebuild_bank_list()


func _stop_audition() -> void:
	if _player != null:
		_player.stop()
	_current_playing = {}
	if _now_playing_label != null:
		_now_playing_label.text = (
				"Idle. Click ▶ on any entry to audition.")
		_now_playing_label.add_theme_color_override("font_color",
				_get_text_secondary())
	_rebuild_bank_list()


func _on_player_finished() -> void:
	_stop_audition()


# ─── Handlers ──────────────────────────────────────────────────

func _on_search_text_changed(new_text: String) -> void:
	_search_text = new_text.to_lower().strip_edges()
	_rebuild_bank_list()


func _on_toggle_bank(bank_path: String) -> void:
	var current: bool = _expanded_banks.get(bank_path, false)
	_expanded_banks[bank_path] = not current
	_rebuild_bank_list()


func _on_inspect_bank(bank) -> void:
	if EditorInterface != null and bank != null:
		EditorInterface.inspect_object(bank)


func _update_toolbar_stats() -> void:
	if _toolbar_stats_label == null:
		return
	var total_sounds: int = 0
	for entry in _banks:
		var bank = entry.resource
		if bank != null and bank.sounds is Dictionary:
			total_sounds += bank.sounds.size()
	if _banks.is_empty():
		_toolbar_stats_label.text = "No banks found"
	else:
		_toolbar_stats_label.text = (
				"%d bank%s · %d sounds total"
				% [_banks.size(),
				   "" if _banks.size() == 1 else "s",
				   total_sounds])


# ─── Theme helpers ─────────────────────────────────────────────
#
# We delegate to the parent dock's v0.51.0 _theme_* helpers if
# available, so this panel picks up the same EditorTheme-aware
# colors as the mixer. If the parent doesn't have them (panel
# instantiated standalone), we fall back to safe defaults.

func _ensure_dock() -> void:
	if _mixer_dock != null:
		return
	var n: Node = get_parent()
	while n != null:
		if n.has_method("_theme_color"):
			_mixer_dock = n
			return
		n = n.get_parent()


func _get_chrome_stylebox(padding: int) -> StyleBoxFlat:
	_ensure_dock()
	if _mixer_dock != null and _mixer_dock.has_method("_build_chrome_stylebox"):
		return _mixer_dock._build_chrome_stylebox(padding)
	# Fallback: build a sensible default stylebox.
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.16, 0.17, 0.20)
	sb.border_color = Color(0.30, 0.32, 0.36)
	sb.border_width_left = 1
	sb.border_width_right = 1
	sb.border_width_top = 1
	sb.border_width_bottom = 1
	sb.corner_radius_top_left = 4
	sb.corner_radius_top_right = 4
	sb.corner_radius_bottom_left = 4
	sb.corner_radius_bottom_right = 4
	sb.content_margin_left = padding
	sb.content_margin_right = padding
	sb.content_margin_top = max(4, padding - 2)
	sb.content_margin_bottom = max(4, padding - 2)
	return sb


func _get_text_primary() -> Color:
	_ensure_dock()
	if _mixer_dock != null and _mixer_dock.has_method("_theme_text_primary"):
		return _mixer_dock._theme_text_primary()
	return Color(0.88, 0.90, 0.93)


func _get_text_secondary() -> Color:
	_ensure_dock()
	if _mixer_dock != null and _mixer_dock.has_method("_theme_text_secondary"):
		return _mixer_dock._theme_text_secondary()
	return Color(0.55, 0.58, 0.62)


func _get_accent() -> Color:
	_ensure_dock()
	if _mixer_dock != null and _mixer_dock.has_method("_theme_accent"):
		return _mixer_dock._theme_accent()
	return Color(0.42, 0.65, 0.95)
