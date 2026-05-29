# addons/gool/editor/update_available_banner.gd
#
# v0.79.2: Banner shown when a newer gool release is available on
# GitHub. Separate file from getting_started_banner.gd because the
# two have different lifecycles:
#   - Getting Started: one-shot, dismissed forever after first
#     interaction. The user has "graduated" from needing it.
#   - Update Available: appears whenever a strictly-newer version
#     exists on GitHub, persists until the user either updates the
#     plugin or suppresses *this specific version*. When a NEWER
#     release than the suppressed one drops, the banner reappears.
#
# Lives in the mixer dock alongside getting_started_banner; mounted
# by mixer_dock.gd above the toolbar so it's the first thing the
# user sees when the dock is open. If the user never opens the
# mixer dock, the update message is ALSO logged to the editor
# output console via plugin.gd's checker callback — defense in
# depth for discoverability.
#
# DISMISS SEMANTICS
# =================
#   Clicking "Don't notify for v0.79.3" stores "0.79.3" in
#   `addons/gool/editor/update_notice_dismissed_version`. Future
#   checks still run, but if the discovered latest matches (or is
#   older than) the dismissed version, no banner shows. v0.79.4
#   landing in the future would trigger the banner again because
#   it's strictly newer than the dismissed 0.79.3.

@tool
extends PanelContainer

const _DISMISSED_VERSION_KEY := "addons/gool/editor/update_notice_dismissed_version"
const _RELEASES_URL := "https://github.com/siliconight/gool/releases"

# v0.80.16: PowerShell one-liner for updating gool in place. Same
# command the install-time `gool-install.cmd` ultimately invokes,
# centralized here so future changes to the quickinstall path
# update the in-editor instructions automatically.
const _UPDATE_COMMAND := (
	"iwr -useb https://raw.githubusercontent.com/siliconight/gool/main/"
	+ "scripts/quickinstall.ps1 | iex"
)

var _latest_version: String = ""
var _current_version: String = ""

# v0.80.16: lazy-built "How to update" help dialog and its
# interactive controls. Constructed on the first "How to update"
# click, kept for subsequent clicks. The dialog body is version-
# agnostic — same instructions regardless of which version the
# banner is currently advertising — so no need to rebuild when
# `_latest_version` changes.
var _help_dialog: AcceptDialog = null
var _help_copy_btn: Button = null
var _help_copy_reset_timer: Timer = null


# Called by the checker callback (wired in mixer_dock.gd) when a
# fresh result indicates an update is available. Idempotent —
# safe to call again with the same args; will rebuild the UI.
func show_for(latest: String, current: String) -> void:
	_latest_version = latest
	_current_version = current

	# Suppression: don't show if the user has dismissed this exact
	# version or any newer one (defensive — in practice the dismissed
	# version should never be NEWER than `latest` since latest IS the
	# latest).
	var dismissed := str(ProjectSettings.get_setting(
			_DISMISSED_VERSION_KEY, ""))
	if dismissed != "" and not _is_newer(latest, dismissed):
		visible = false
		return

	visible = true
	_build_ui()


# Numeric version comparison. Duplicated from update_checker.gd
# rather than imported to keep the banner free of script load
# dependencies — the banner runs in _ready before the checker is
# necessarily resolved.
func _is_newer(a: String, b: String) -> bool:
	var ap := a.split(".")
	var bp := b.split(".")
	var n: int = mini(ap.size(), bp.size())
	for i in range(n):
		var ai := int(ap[i])
		var bi := int(bp[i])
		if ai > bi:
			return true
		if ai < bi:
			return false
	return ap.size() > bp.size()


func _build_ui() -> void:
	# Idempotency: clear any prior children if show_for is called
	# again with new args.
	for child in get_children():
		child.queue_free()

	add_theme_constant_override("margin_left", 12)
	add_theme_constant_override("margin_right", 12)
	add_theme_constant_override("margin_top", 8)
	add_theme_constant_override("margin_bottom", 8)

	var hbox := HBoxContainer.new()
	hbox.add_theme_constant_override("separation", 10)
	add_child(hbox)

	var label := Label.new()
	label.text = "gool %s is available (you're running %s)." % [
		_latest_version, _current_version]
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	label.add_theme_font_size_override("font_size", 14)
	hbox.add_child(label)

	var btn_view := Button.new()
	btn_view.text = "View release notes"
	btn_view.tooltip_text = ("Opens the gool releases page on GitHub "
			+ "in your default browser.")
	btn_view.pressed.connect(_on_view_releases)
	hbox.add_child(btn_view)

	# v0.80.16: between informational ("View release notes") and the
	# escape hatch ("Don't notify"), the actionable choice — show
	# the user exactly how to perform the update.
	var btn_help := Button.new()
	btn_help.text = "How to update"
	btn_help.tooltip_text = ("Show the step-by-step PowerShell command "
			+ "for updating gool. Godot must be closed during the "
			+ "update because the runtime DLL is locked while the "
			+ "editor runs.")
	btn_help.pressed.connect(_on_show_help)
	hbox.add_child(btn_help)

	var btn_dismiss := Button.new()
	btn_dismiss.text = "Don't notify for v" + _latest_version
	btn_dismiss.flat = true
	btn_dismiss.tooltip_text = ("Suppress this notice for v"
			+ _latest_version + ". A newer release than v"
			+ _latest_version + " will trigger the banner again.")
	btn_dismiss.pressed.connect(_on_dismiss)
	hbox.add_child(btn_dismiss)


func _on_view_releases() -> void:
	OS.shell_open(_RELEASES_URL)


func _on_dismiss() -> void:
	ProjectSettings.set_setting(_DISMISSED_VERSION_KEY, _latest_version)
	ProjectSettings.set_initial_value(_DISMISSED_VERSION_KEY, "")
	var err := ProjectSettings.save()
	if err != OK:
		push_warning(("[gool] failed to persist update-notice "
				+ "dismissal (Error %d). The banner may reappear "
				+ "on next editor restart.") % err)
	visible = false


# v0.80.16: pop the "How to update" help dialog. Lazy-builds it
# on first click; reuses the same dialog instance for subsequent
# clicks.
func _on_show_help() -> void:
	if _help_dialog == null:
		_build_help_dialog()
	_help_dialog.popup_centered(Vector2i(580, 440))


# v0.80.16: lazy-build the help dialog. Constructs the AcceptDialog
# with the version-agnostic instructions and the command field +
# Copy button as a custom child node. Dialog is parented to this
# banner so it's freed when the banner is.
func _build_help_dialog() -> void:
	_help_dialog = AcceptDialog.new()
	_help_dialog.title = "How to update gool (Windows)"
	_help_dialog.dialog_text = (
			"Updating gool requires closing Godot first. The gool "
			+ "runtime DLL is loaded into the editor while it runs, "
			+ "and Windows won't replace a DLL that's mapped into a "
			+ "running process — the updater needs Godot fully "
			+ "closed.\n\n"
			+ "Steps:\n\n"
			+ "  1. Save your work and close Godot completely.\n\n"
			+ "  2. Open PowerShell in your Godot project folder:\n"
			+ "       Shift+right-click in the folder, choose\n"
			+ "       \"Open PowerShell window here\" (or \"Open in\n"
			+ "       Terminal\" on newer Windows builds).\n\n"
			+ "  3. Paste and run the command below.\n\n"
			+ "  4. Reopen your project in Godot — the new version\n"
			+ "     is installed and active.\n\n"
			+ "Manual alternative: download the addon zip from the\n"
			+ "releases page (use \"View release notes\" above) and\n"
			+ "replace your project's addons/gool/ folder with the\n"
			+ "one from the zip.")

	# Custom content below dialog_text: command in a read-only
	# LineEdit + a Copy button.
	var cmd_box := VBoxContainer.new()
	cmd_box.add_theme_constant_override("separation", 4)

	var cmd_label := Label.new()
	cmd_label.text = "PowerShell command:"
	cmd_box.add_child(cmd_label)

	var cmd_field := LineEdit.new()
	cmd_field.text = _UPDATE_COMMAND
	cmd_field.editable = false  # read-only; selection + Ctrl+C still work
	cmd_field.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	cmd_box.add_child(cmd_field)

	var cmd_btns := HBoxContainer.new()
	cmd_btns.alignment = BoxContainer.ALIGNMENT_END
	_help_copy_btn = Button.new()
	_help_copy_btn.text = "Copy command"
	_help_copy_btn.pressed.connect(_on_copy_command_pressed)
	cmd_btns.add_child(_help_copy_btn)
	cmd_box.add_child(cmd_btns)

	_help_dialog.add_child(cmd_box)

	# Timer that resets the "Copied ✓" label back to "Copy command"
	# 1.5 seconds after a copy. Parented to the banner (not the
	# dialog) so it survives dialog close/reopen without re-creation.
	_help_copy_reset_timer = Timer.new()
	_help_copy_reset_timer.wait_time = 1.5
	_help_copy_reset_timer.one_shot = true
	_help_copy_reset_timer.timeout.connect(_on_copy_reset_timer_timeout)
	add_child(_help_copy_reset_timer)

	# Parent the dialog to the banner so it's freed automatically
	# when the banner is freed.
	add_child(_help_dialog)


func _on_copy_command_pressed() -> void:
	DisplayServer.clipboard_set(_UPDATE_COMMAND)
	if is_instance_valid(_help_copy_btn):
		_help_copy_btn.text = "Copied ✓"
	if is_instance_valid(_help_copy_reset_timer):
		_help_copy_reset_timer.start()


func _on_copy_reset_timer_timeout() -> void:
	if is_instance_valid(_help_copy_btn):
		_help_copy_btn.text = "Copy command"
