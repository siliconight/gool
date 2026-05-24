# addons/gool/editor/getting_started_banner.gd
#
# v0.72.0: First-launch onboarding banner for the mixer dock.
#
# Shows at the top of the Mixer tab when:
#   - The project has no res://gool/config.json file yet, AND
#   - The user hasn't dismissed the banner permanently via the
#     "Don't show again" button.
#
# Three actions, all idempotent and reversible by hand-editing
# the resulting JSON file:
#
#   1. "Use FPS template" — copies addons/gool/templates/config_fps.json
#      to res://gool/config.json. This is the bus graph used by
#      examples/04_coop_shooter_template and audition; sensible
#      starting point for an FPS, action game, or anything with
#      gameplay SFX + music + dialogue.
#
#   2. "Use minimal template" — writes a 3-bus config to
#      res://gool/config.json: Master, Sfx (with one reverb effect),
#      Music. The leanest baseline you can run gool with. Good for
#      smaller games, tutorials, or "just want sound to work."
#
#   3. "Don't show again" — sets the editor-meta flag below and
#      hides the banner permanently for this project. The banner
#      can be brought back by deleting the metadata file or
#      manually editing project.godot.
#
# Once a config.json exists, the banner self-removes on next
# mixer-dock open. No-op if the user already has one.
@tool
extends PanelContainer

const _PROJECT_CONFIG_PATH: String = "res://gool/config.json"
const _ADDON_FPS_TEMPLATE:  String = "res://addons/gool/templates/config_fps.json"
const _DISMISS_META_KEY:    String = "addons/gool/editor/getting_started_dismissed"

const _MINIMAL_CONFIG_JSON: String = """{
  "buses": [
    {
      "name": "Master",
      "gain_db": 0.0,
      "effects": []
    },
    {
      "name": "Sfx",
      "parent": "Master",
      "gain_db": 0.0,
      "effects": [
        {
          "kind": "reverb",
          "predelay_ms": 30.0,
          "decay": 0.55,
          "lf_damping": 0.20,
          "hf_damping": 0.30,
          "diffusion": 0.75,
          "wet_gain_db": -8.0,
          "send_hpf_hz": 200.0,
          "return_lpf_hz": 6000.0
        }
      ]
    },
    {
      "name": "Music",
      "parent": "Master",
      "gain_db": -3.0,
      "effects": []
    }
  ]
}
"""

# Set true to bypass the auto-hide-when-config-exists check, useful
# for editing the banner's appearance in the editor.
const _DEBUG_FORCE_VISIBLE: bool = false


func _ready() -> void:
	# Build the banner UI. We construct it programmatically rather
	# than via .tscn so this is a single self-contained file with
	# no scene-resource dependency.
	if not _should_show():
		visible = false
		return
	_build_ui()


func _should_show() -> bool:
	if _DEBUG_FORCE_VISIBLE:
		return true
	# Auto-hide if a config already exists.
	if FileAccess.file_exists(_PROJECT_CONFIG_PATH):
		return false
	# Auto-hide if the user dismissed it for this project.
	if ProjectSettings.has_setting(_DISMISS_META_KEY) \
			and bool(ProjectSettings.get_setting(_DISMISS_META_KEY)):
		return false
	return true


func _build_ui() -> void:
	# Visual treatment: a chrome panel with a chunky header label, a
	# short explanation, three action buttons in a row.
	add_theme_constant_override("margin_left", 12)
	add_theme_constant_override("margin_right", 12)
	add_theme_constant_override("margin_top", 8)
	add_theme_constant_override("margin_bottom", 8)
	var vbox := VBoxContainer.new()
	vbox.add_theme_constant_override("separation", 8)
	add_child(vbox)

	var title := Label.new()
	title.text = "Welcome to gool"
	title.add_theme_font_size_override("font_size", 18)
	vbox.add_child(title)

	var subtitle := Label.new()
	subtitle.text = ("No gool/config.json found in this project yet. "
			+ "Pick a starting bus layout — you can always edit it "
			+ "later from this dock or by opening the JSON in any "
			+ "text editor.")
	subtitle.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	subtitle.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	vbox.add_child(subtitle)

	var buttons := HBoxContainer.new()
	buttons.add_theme_constant_override("separation", 6)
	vbox.add_child(buttons)

	var btn_fps := Button.new()
	btn_fps.text = "Use FPS template"
	btn_fps.tooltip_text = ("11-bus layout: Master, Music, "
			+ "SfxAll → LocalSfx/RemoteSfx/Explosions/Footsteps/ImpactEq, "
			+ "Dialogue, Voice, Ambient. Includes a Master Control "
			+ "preset on Master. Good for any action game.")
	btn_fps.pressed.connect(_on_use_fps_template)
	buttons.add_child(btn_fps)

	var btn_min := Button.new()
	btn_min.text = "Use minimal template"
	btn_min.tooltip_text = ("3-bus layout: Master, Sfx (with one "
			+ "reverb), Music. The leanest config gool runs with — "
			+ "good for tutorials or small games.")
	btn_min.pressed.connect(_on_use_minimal_template)
	buttons.add_child(btn_min)

	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	buttons.add_child(spacer)

	var btn_dismiss := Button.new()
	btn_dismiss.text = "Don't show again"
	btn_dismiss.flat = true
	btn_dismiss.tooltip_text = ("Hide this banner permanently for "
			+ "this project. You can re-enable it by deleting the "
			+ "addons/gool/editor/getting_started_dismissed setting "
			+ "from project.godot.")
	btn_dismiss.pressed.connect(_on_dismiss)
	buttons.add_child(btn_dismiss)


# ─── Actions ───────────────────────────────────────────────────────

func _on_use_fps_template() -> void:
	if not FileAccess.file_exists(_ADDON_FPS_TEMPLATE):
		_log_error("FPS template not found at " + _ADDON_FPS_TEMPLATE
				+ ". This is unexpected — the gool addon ships this "
				+ "file by default. Reinstall the addon or check the "
				+ "templates folder.")
		return
	var src := FileAccess.open(_ADDON_FPS_TEMPLATE, FileAccess.READ)
	if src == null:
		_log_error("Could not read FPS template (err=%d)."
				% FileAccess.get_open_error())
		return
	var content: String = src.get_as_text()
	src.close()
	_write_config_and_finish(content, "FPS template")


func _on_use_minimal_template() -> void:
	_write_config_and_finish(_MINIMAL_CONFIG_JSON, "minimal template")


func _on_dismiss() -> void:
	ProjectSettings.set_setting(_DISMISS_META_KEY, true)
	# Make the setting persist across editor restarts.
	ProjectSettings.set_initial_value(_DISMISS_META_KEY, false)
	var save_err: int = ProjectSettings.save()
	if save_err != OK:
		push_warning("[gool] Could not persist dismiss flag (err=%d). "
				% save_err
				+ "Banner hidden for this session but may reappear next "
				+ "editor launch.")
	visible = false
	print("[gool] Getting Started banner dismissed. To re-enable, "
			+ "remove the 'addons/gool/editor/getting_started_dismissed' "
			+ "setting from project.godot.")


# ─── Helpers ───────────────────────────────────────────────────────

func _write_config_and_finish(content: String, label: String) -> void:
	# Ensure res://gool/ directory exists. DirAccess.make_dir_recursive
	# is idempotent (no-op if already present).
	var dir_err: int = DirAccess.make_dir_recursive_absolute(
			ProjectSettings.globalize_path("res://gool"))
	if dir_err != OK and dir_err != ERR_ALREADY_EXISTS:
		_log_error("Could not create res://gool/ directory (err=%d)."
				% dir_err)
		return
	# Write the config.
	var f := FileAccess.open(_PROJECT_CONFIG_PATH, FileAccess.WRITE)
	if f == null:
		_log_error("Could not open %s for write (err=%d)."
				% [_PROJECT_CONFIG_PATH, FileAccess.get_open_error()])
		return
	f.store_string(content)
	f.close()
	print("[gool] Wrote %s to %s using the %s. "
			% [_PROJECT_CONFIG_PATH.get_file(),
					_PROJECT_CONFIG_PATH, label]
			+ "Restart the running game (or close-and-reopen the editor) "
			+ "to apply.")
	visible = false


func _log_error(msg: String) -> void:
	push_error("[gool] Getting Started: " + msg)
