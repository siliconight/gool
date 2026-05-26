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

var _latest_version: String = ""
var _current_version: String = ""


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
