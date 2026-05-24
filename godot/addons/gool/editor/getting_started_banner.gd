# addons/gool/editor/getting_started_banner.gd
#
# v0.72.0: First-launch onboarding banner for the mixer dock.
# v0.74.1: visibility logic fixed (see note at the bottom of this
# header). The original v0.72.0 form never actually rendered on a
# fresh install because plugin.gd writes a default config.json on
# enable, and the banner was checking "config.json absent" as its
# "not yet onboarded" signal — a contradiction the plugin always
# satisfied. Two whole releases (v0.72.0 + v0.73.x) shipped with
# the banner permanently invisible; v0.74.1 makes the dismiss flag
# the sole hide-trigger.
#
# Shows at the top of the Mixer tab when:
#   - The user hasn't dismissed the banner via the "Don't show
#     again" button, AND
#   - The user hasn't picked a template yet (template-pick also
#     persists the dismiss flag, treating it as completed onboarding).
#
# Three actions, all idempotent and reversible by hand-editing
# the resulting JSON file:
#
#   1. "Use FPS template" — copies addons/gool/templates/config_fps.json
#      to res://gool/config.json. This is the bus graph used by
#      examples/04_coop_shooter_template and audition; sensible
#      starting point for an FPS, action game, or anything with
#      gameplay SFX + music + dialogue. Also persists the dismiss
#      flag so the banner stays gone next editor open.
#
#   2. "Use minimal template" — writes a 3-bus config to
#      res://gool/config.json: Master, Sfx (with one reverb effect),
#      Music. The leanest baseline you can run gool with. Good for
#      smaller games, tutorials, or "just want sound to work." Also
#      persists the dismiss flag.
#
#   3. "Don't show again" — persists the dismiss flag without
#      touching the config. Use when the user already knows what
#      they want and doesn't need the template helper.
#
# To re-enable the banner on a project where it's been dismissed,
# remove the 'addons/gool/editor/getting_started_dismissed' setting
# from project.godot (or set it to false).
#
# v0.74.1 visibility-fix rationale (more detail):
# The previous form had `if FileAccess.file_exists(_PROJECT_CONFIG_PATH):
# return false` as a hide-trigger. The intent was "if the user already
# has a config, they're not new — hide the banner." But plugin.gd's
# enable() writes that exact file as one of its setup steps, before
# the user has a chance to see the banner. Net effect: the banner
# could never show on a fresh project. The fix is to remove that
# trigger and route all "user is done" signals through the dismiss
# flag, which is set by all three button handlers.
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
	# v0.74.1 fix: the only valid "user already onboarded" signal is the
	# dismiss flag in ProjectSettings. The previous check (auto-hide when
	# res://gool/config.json exists) was always satisfied on fresh
	# installs because plugin.gd writes a default config as part of
	# enable(). Result: the banner shipped in v0.72.0 never actually
	# appeared to any new user. Fixed by letting the dismiss flag be the
	# sole "I'm done with onboarding" signal; the flag is set when the
	# user clicks any of the three buttons (template-pick or explicit
	# dismiss), so the banner stays gone after the user has engaged.
	if ProjectSettings.has_setting(_DISMISS_META_KEY) \
			and bool(ProjectSettings.get_setting(_DISMISS_META_KEY)):
		return false
	return true


# v0.74.1: extracted from _on_dismiss so _write_config_and_finish can
# reuse it. Persists the "user has interacted with the banner" flag in
# ProjectSettings so the banner stays hidden across editor restarts.
# Returns true if the flag persisted successfully, false if the
# ProjectSettings.save() call failed (caller may surface a warning).
func _persist_dismiss_flag() -> bool:
	ProjectSettings.set_setting(_DISMISS_META_KEY, true)
	ProjectSettings.set_initial_value(_DISMISS_META_KEY, false)
	var save_err: int = ProjectSettings.save()
	return save_err == OK


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
	# v0.74.1: shared helper with _write_config_and_finish.
	if not _persist_dismiss_flag():
		push_warning("[gool] Could not persist dismiss flag. "
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
	# v0.74.1: persist the dismiss flag so the banner doesn't reappear on
	# next editor restart. Picking a template is an act of completed
	# onboarding — the user is engaged and set up. Without this, the
	# banner would re-show on every editor open because _should_show no
	# longer treats config.json existence as a dismiss signal (see
	# comment on _should_show for why).
	if not _persist_dismiss_flag():
		push_warning("[gool] Wrote config but could not persist banner "
				+ "dismiss flag. Banner will be hidden this session but "
				+ "may reappear next editor launch.")
	print("[gool] Wrote %s to %s using the %s. "
			% [_PROJECT_CONFIG_PATH.get_file(),
					_PROJECT_CONFIG_PATH, label]
			+ "Restart the running game (or close-and-reopen the editor) "
			+ "to apply.")
	visible = false


func _log_error(msg: String) -> void:
	push_error("[gool] Getting Started: " + msg)
