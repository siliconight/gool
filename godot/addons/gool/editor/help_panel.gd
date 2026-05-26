# addons/gool/editor/help_panel.gd
#
# v0.79.3: Initial in-editor help panel.
# v0.79.7: Content rewrite for "new user, what did I buy?" reader.
#          Cut from 7 sections down to 4 + Learn-more footer. Prose
#          instead of bullet lists where prose reads better. Quick
#          Start is genuinely quick (3 lines, no preamble). API
#          reference trimmed to the 6 most-reached-for calls; full
#          API lives in the README and the script editor's autocomplete.
#
# Non-modal Window that surfaces gool's discoverable features:
# how to play a first sound, where things live in the editor, the
# most-common runtime calls, and what to do when something looks
# broken.
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
#   so it stays in sync without manual updates. Resist the urge
#   to add features to the API section — if someone needs to look
#   up a method that's not in the top 6, they're past the help
#   panel's audience and should be in the README.

@tool
extends Window

const _GITHUB_URL := "https://github.com/siliconight/gool"
const _README_URL := "https://github.com/siliconight/gool/blob/main/README.md"
const _CHANGELOG_URL := "https://github.com/siliconight/gool/blob/main/CHANGELOG.md"
const _RELEASES_URL := "https://github.com/siliconight/gool/releases"

const _CONTENT_HEADER := """[font_size=18][b]gool[/b][/font_size]
[i]Multiplayer-first audio middleware for Godot. Three autoloads — \
[b]Gool[/b], [b]DialogueDirector[/b], [b]MultiplayerBridge[/b] — are your \
entry points. For deep dives, see the [url=%s]README on GitHub[/url].[/i]
""" % _README_URL

const _CONTENT_QUICK_START := """
[font_size=16][b]Play your first sound[/b][/font_size]
Drop an audio file in [code]res://[/code] and run:
[code]Gool.register_sound_from_file("ping", "res://audio/ping.wav")
Gool.play_3d("ping", Vector3(10, 0, 0))[/code]
F5 and you'll hear it. If you'd rather start from a working scene, the \
[i]Project → Tools → gool[/i] menu has a scaffolding command that drops \
a complete 3D audio setup into the current scene.
"""

const _CONTENT_EDITOR_MAP := """
[font_size=16][b]Where to find things in the editor[/b][/font_size]
You're in the [b]Mixer Dock[/b] — bus levels, mix snapshots, bus chain editing. \
Opening any [code]GoolSoundBank[/code] or [code]GoolMaterialEQ[/code] resource \
brings up an inspector for it automatically.

The [b]Project → Tools → gool[/b] submenu has shortcuts for scaffolding \
3D audio, opening a quickstart scene, running install smoke tests when \
something feels off, and reopening this help panel.

During F5 play, the [b]Debugger[/b] dock has a [code]gool[/code] tab \
with live emitter, voice, and RTPC state, plus 15 gool-specific \
performance monitors under the Monitors panel.
"""

const _CONTENT_RUNTIME_API := """
[font_size=16][b]Common runtime calls[/b][/font_size]
The Gool autoload exposes around 100 public methods. The handful you'll reach for most:
[code]Gool.play_3d(name, position)         # 3D positional sound
Gool.play_2d(name)                    # UI / non-spatial sound
Gool.set_rtpc(name, value)            # parameter-driven mixing
Gool.set_bus_gain_db("Music", -6.0)   # server-authoritative mix
Gool.set_player_master_volume(75)     # player settings (0-100 slider)
Gool.diagnose()                       # install health check[/code]

Player-side volume (the [code]set_player_*[/code] family, plus voice \
muting) combines with server-side bus gains via dB addition — your \
game's mixes and snapshots stay audible, scaled by the listener's \
preference. Auto-saves to [code]user://[/code].

For voice chat, dialogue, sound banks, and the full method list, \
see the [url=%s]README[/url] or just type [code]Gool.[/code] in the \
script editor and let autocomplete walk you through it.
""" % _README_URL

const _CONTENT_TROUBLESHOOTING := """
[font_size=16][b]When something looks wrong[/b][/font_size]
First stop: run [code]Gool.diagnose()[/code] from any script. It \
prints a structured health check — autoloads registered, bus graph \
valid, sample rate, device working — and tells you which assumption \
broke.

If you're filing a bug report, press [b]Ctrl+Shift+G[/b] to dump the \
session log. A file browser opens to the dump directory; attach the \
file to your report. (Rebindable in [i]Project Settings → Input Map → \
gool_dump_session_log[/i]; disable with project setting \
[code]audio/gool/dump_session_log_enabled[/code].)

During F5 play, the Debugger's Monitors panel has gool-specific \
graphs (update tick μs, eviction rate, voice throughput, and more) \
if you need live performance data.
"""

const _CONTENT_RESOURCES := """[font_size=16][b]Learn more[/b][/font_size]
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
	# v0.79.7: collapsed from 7 sections to 4 + header + footer.
	return (_CONTENT_HEADER
			+ _CONTENT_QUICK_START
			+ _CONTENT_EDITOR_MAP
			+ _CONTENT_RUNTIME_API
			+ _CONTENT_TROUBLESHOOTING
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
