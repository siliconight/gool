# addons/gool/editor/help_panel.gd
#
# v0.79.3: In-editor help panel.
#
# Non-modal Window that surfaces gool's discoverable features:
# keyboard shortcuts, editor tools, the most-used runtime API,
# the v0.79.0 player preferences API, diagnostics, and links to
# deeper docs on GitHub.
#
# DESIGN
# ======
#   - Extends Window (not PopupPanel / AcceptDialog). Window is
#     non-modal by default in Godot 4, can be moved/resized
#     independently of the main editor window, and stays open
#     while the user explores the API or tries things in code.
#   - Content lives as multiline string constants in this file.
#     One file to edit when adding a feature. No separate docs
#     dependency, no markdown parser.
#   - Self-contained for shortcuts, tools, and API quick reference
#     (the things users look up in-flight). Linked-out via [url]
#     for README/CHANGELOG/releases (the deep dives).
#   - Mounted via EditorInterface.get_base_control() so it
#     positions over the editor correctly across DPI scales and
#     monitor setups.
#
# MAINTENANCE NOTE
# ================
#   When adding a new feature, update the relevant _CONTENT_*
#   constant below. The version footer auto-reads from plugin.cfg
#   so it stays in sync without manual updates.

@tool
extends Window

const _GITHUB_URL := "https://github.com/siliconight/gool"
const _README_URL := "https://github.com/siliconight/gool/blob/main/README.md"
const _CHANGELOG_URL := "https://github.com/siliconight/gool/blob/main/CHANGELOG.md"
const _RELEASES_URL := "https://github.com/siliconight/gool/releases"

const _CONTENT_HEADER := """[font_size=18][b]gool quick reference[/b][/font_size]
[i]Multiplayer-first audio middleware for Godot. This panel \
summarizes the most useful surfaces; for deep dives, see the \
[url=%s]README on GitHub[/url].[/i]
""" % _README_URL

const _CONTENT_QUICK_START := """
[font_size=16][b]Quick Start[/b][/font_size]
The [b]Gool[/b] autoload is your entry point. Three lines to play a 3D sound:
[code]Gool.register_sound_from_file("footstep", "res://audio/footstep.wav")
Gool.play_3d("footstep", Vector3(10, 0, 0))
Gool.set_rtpc("combat_intensity", 0.7)[/code]
Three autoloads are registered when the plugin is enabled:
[indent]• [b]Gool[/b] — main runtime: playback, registration, RTPCs, voice
• [b]DialogueDirector[/b] — speaker priority and dialogue events
• [b]MultiplayerBridge[/b] — bridges Godot's MultiplayerAPI to gool's event replication[/indent]
"""

const _CONTENT_SHORTCUTS := """
[font_size=16][b]Keyboard Shortcuts[/b][/font_size]
[indent]• [b]Ctrl+Shift+G[/b] — Dump session log to a file. Opens the dump \
directory in your OS file browser. Rebindable in \
[i]Project Settings → Input Map → gool_dump_session_log[/i]. \
Disable entirely with project setting \
[code]audio/gool/dump_session_log_enabled[/code].[/indent]
"""

const _CONTENT_EDITOR_TOOLS := """
[font_size=16][b]Editor Tools[/b][/font_size]
[indent]• [b]Mixer Dock[/b] (you're here) — Visualize bus levels, save and load mix snapshots, edit bus chains
• [b]Sound Bank Panel[/b] — Browse and edit GoolSoundBank resources
• [b]Material EQ Inspector[/b] — Edit acoustic material EQ presets; appears when you open any GoolMaterialEQ .tres
• [b]Debugger → Monitors[/b] — 15 custom gool perf graphs (update tick μs, eviction rate, voice throughput, etc.) during F5 play
• [b]Debugger → gool tab[/b] — Live runtime state (active emitters, voice peer stats, current RTPCs) during F5 play[/indent]
"""

const _CONTENT_TOOLS_MENU := """
[font_size=16][b]Project → Tools → gool[/b][/font_size]
[indent]• [b]Add gool 3D audio scaffolding[/b] — Drops a working 3D audio setup into the current scene
• [b]Add debug overlay[/b] — Runtime visualization of emitters and listener
• [b]Create new GoolFolderSoundBank...[/b] — Folder-based bank resource
• [b]Open quickstart_3d.tscn[/b] — Minimal scene to verify install works
• [b]Run prefab smoke test[/b] — Catches missing autoload wrappers
• [b]Run FPS scene smoke test[/b] — Catches missing config dependencies
• [b]Help[/b] — Opens this panel[/indent]
"""

const _CONTENT_RUNTIME_API := """
[font_size=16][b]Runtime API Essentials[/b][/font_size]
[i]Most-used methods on the Gool autoload. There are 104 public methods \
total; this is the subset most game code touches.[/i]

[b]Playback[/b]
[code]Gool.play_3d(name, position)            # → emitter_id
Gool.play_2d(name)                      # → emitter_id
Gool.play_event(event_name, position)   # sound-definition events[/code]

[b]Sound Registration[/b]
[code]Gool.register_sound_from_file(name, path)
Gool.register_pcm_sound(name, samples, sample_rate, channels)
Gool.register_sound_from_stream(name, audio_stream)
Gool.register_sound_definition(name, opts)[/code]

[b]Real-time Parameter Control[/b]
[code]Gool.set_rtpc(name, value)
Gool.get_rtpc(name)                     # → float[/code]

[b]Bus Control (server-authoritative)[/b]
[code]Gool.set_master_volume_db(db)
Gool.set_bus_gain_db("Music", -6.0)[/code]

[b]Voice Chat[/b]
[code]Gool.submit_voice_packet(peer_id, bytes, seq, ts)
Gool.register_voice_source(peer_id, peer_position)[/code]
"""

const _CONTENT_PLAYER_SETTINGS := """
[font_size=16][b]Player Audio Settings (v0.79.0+)[/b][/font_size]
[i]Client-side controls for player settings menus. Combine with \
server-authoritative bus controls via dB addition — game mixes and \
mix snapshots stay audible, scaled by player preference.[/i]
[code]Gool.set_player_master_volume(75)               # 0-100 slider
Gool.set_player_category_volume("music", 50)
Gool.mute_voice_player(peer_id)
Gool.get_muted_voice_players()                  # → Array[int]
Gool.clear_muted_voice_players()
Gool.reset_player_preferences()[/code]
Categories: [code]master, sfx, music, voice, ambience, dialogue, ui[/code]
Auto-saves to [code]user://gool_player_preferences.cfg[/code] on every set_* call.
"""

const _CONTENT_DIAGNOSTICS := """
[font_size=16][b]Diagnostics[/b][/font_size]
[indent]• [b]Gool.diagnose()[/b] — Prints install health check to output (autoload registered? bus graph valid? sample rate? device working?). Run this when install feels broken.
• [b]Debugger → Monitors panel[/b] — 15 custom gool monitors (gool/*) for engine perf during F5 play.
• [b]Ctrl+Shift+G[/b] — Session log dump. Writes a structured log file with the last N events. Attach to bug reports.[/indent]
"""

const _CONTENT_RESOURCES := """[font_size=16][b]Resources[/b][/font_size]
[indent]• [url=https://github.com/siliconight/gool]GitHub repository[/url]
• [url=https://github.com/siliconight/gool/blob/main/README.md]README[/url]
• [url=https://github.com/siliconight/gool/blob/main/CHANGELOG.md]CHANGELOG[/url]
• [url=https://github.com/siliconight/gool/releases]Releases page[/url][/indent]
"""


func _init() -> void:
	title = "gool — Help"
	size = Vector2i(720, 640)
	min_size = Vector2i(480, 360)
	transient = false
	exclusive = false
	unresizable = false
	close_requested.connect(_on_close_requested)


func _ready() -> void:
	_build_ui()


func _build_ui() -> void:
	# Root margin so content doesn't crash into the window edges.
	var margin := MarginContainer.new()
	margin.set_anchors_preset(Control.PRESET_FULL_RECT)
	margin.add_theme_constant_override("margin_left", 12)
	margin.add_theme_constant_override("margin_right", 12)
	margin.add_theme_constant_override("margin_top", 8)
	margin.add_theme_constant_override("margin_bottom", 8)
	add_child(margin)

	var vbox := VBoxContainer.new()
	vbox.add_theme_constant_override("separation", 8)
	margin.add_child(vbox)

	# Scrollable content
	var scroll := ScrollContainer.new()
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	vbox.add_child(scroll)

	var rtl := RichTextLabel.new()
	rtl.bbcode_enabled = true
	rtl.fit_content = true
	rtl.scroll_active = false  # outer ScrollContainer handles scrolling
	rtl.selection_enabled = true  # users want to copy code snippets
	rtl.context_menu_enabled = true
	rtl.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	rtl.text = _build_content()
	rtl.meta_clicked.connect(_on_meta_clicked)
	scroll.add_child(rtl)

	# Footer with version and action buttons
	var footer := HBoxContainer.new()
	footer.add_theme_constant_override("separation", 8)
	vbox.add_child(footer)

	var version_label := Label.new()
	version_label.text = "gool " + _get_current_gool_version()
	version_label.add_theme_color_override(
			"font_color", Color(0.7, 0.7, 0.7))
	footer.add_child(version_label)

	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	footer.add_child(spacer)

	var github_btn := Button.new()
	github_btn.text = "View on GitHub"
	github_btn.tooltip_text = "Opens the gool repository in your browser"
	github_btn.pressed.connect(func() -> void: OS.shell_open(_GITHUB_URL))
	footer.add_child(github_btn)

	var close_btn := Button.new()
	close_btn.text = "Close"
	close_btn.pressed.connect(_on_close_requested)
	footer.add_child(close_btn)


func _build_content() -> String:
	# Concatenate all sections. The header lives first; everything
	# else flows underneath separated by newlines for visual breathing.
	return (_CONTENT_HEADER
			+ _CONTENT_QUICK_START
			+ _CONTENT_SHORTCUTS
			+ _CONTENT_EDITOR_TOOLS
			+ _CONTENT_TOOLS_MENU
			+ _CONTENT_RUNTIME_API
			+ _CONTENT_PLAYER_SETTINGS
			+ _CONTENT_DIAGNOSTICS
			+ _CONTENT_RESOURCES)


func _on_meta_clicked(meta: Variant) -> void:
	# RichTextLabel's [url=...] tags fire this with the URL as `meta`.
	# We just shell-open it; OS handles the protocol routing.
	var url := str(meta)
	if url.begins_with("http://") or url.begins_with("https://"):
		OS.shell_open(url)


func _on_close_requested() -> void:
	# Defensive: schedule free rather than free directly, so any
	# in-flight signals (e.g. a button press handler that called
	# close) finish cleanly first.
	queue_free()


# Read the current gool version from plugin.cfg. Mirrors the helper
# in mixer_dock.gd; duplicated to keep the help panel free of
# cross-script dependencies.
func _get_current_gool_version() -> String:
	var cfg := ConfigFile.new()
	if cfg.load("res://addons/gool/plugin.cfg") != OK:
		return "(version unknown)"
	return "v" + str(cfg.get_value("plugin", "version", "0.0.0"))
