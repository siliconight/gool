# addons/gool/editor/material_eq_preset_manager.gd
#
# v0.61.2 — Phase 6.E.4 (first cut): preset library scanner / saver.
# v0.61.3 — Phase 6.E.4 (mostly complete): built-in preset library.
#
# Tiny helper used by the inspector's "Save preset..." / "Load
# preset..." buttons. Hides the directory-scan and filename-
# sanitization details so the inspector's logic stays focused on UI.
#
# Storage convention:
#   res://addons/gool/material_eq_presets/<Name>.tres  - built-in, ships with gool
#   res://gool/material_eq_presets/<sanitized_name>.tres - user, per-project
#
# Built-in presets are read-only from a UX perspective — they can be
# loaded onto a material like any other preset, but can't be saved
# OVER (save_preset only writes to USER_PRESET_DIR). To "tweak a
# built-in", a designer loads it onto their material, drags the
# handles to taste, and Save…s under a new name; the new preset
# lands in the user directory alongside any other custom presets.
#
# Filenames are derived from preset_name via a path-safe pass:
# anything not in [A-Za-z0-9_-] becomes underscore. Cross-platform
# safe; no spaces, no quotes, no slashes. Designers see their
# free-form preset_name in the picker; the sanitized form is just
# the on-disk key.

@tool
extends RefCounted

# v0.61.3: split into builtin (ships with gool) and user (per-project).
# Built-in presets give Godot devs using gool an immediate library
# of recognizable tonal characters — "Tile bathroom", "Wooden cabin",
# "Cathedral stone", etc. — so a fresh project can sound acoustically
# grounded without waiting for the audio designer to author from scratch.
const BUILTIN_PRESET_DIR: String = "res://addons/gool/material_eq_presets"
const USER_PRESET_DIR: String = "res://gool/material_eq_presets"

# Kept as PRESET_DIR for backward compat with v0.61.2 (alias for the
# user dir; save_preset writes here, would_overwrite checks here).
const PRESET_DIR: String = USER_PRESET_DIR

const PRESET_SCRIPT_PATH: String = (
		"res://addons/gool/resources/gool_material_eq_preset.gd")


# List every preset .tres file in BUILTIN_PRESET_DIR + USER_PRESET_DIR.
# Returns an Array of Dictionaries:
#   {
#     "name":       String,    # display label
#     "path":       String,    # res:// path of the .tres
#     "preset":     Resource,  # loaded GoolMaterialEqPreset
#     "is_builtin": bool,      # true if from BUILTIN_PRESET_DIR
#   }
# Sorted: built-ins first (by name), then user (by name). The picker
# UX uses this ordering so designers see the curated starting points
# before their own variants.
#
# Returns [] if neither dir has any presets. Each dir is independently
# tolerant of missing-ness — a fresh project with no user presets
# still gets the full built-in list; an installation without the
# addon dir still gets the user list.
static func list_presets() -> Array:
	var builtin: Array = _list_dir(BUILTIN_PRESET_DIR, true)
	var user: Array = _list_dir(USER_PRESET_DIR, false)
	# Sort each bucket alphabetically by name, then concat with
	# builtins first. Within-bucket alphabetical keeps the order
	# stable across sessions.
	builtin.sort_custom(_compare_by_name)
	user.sort_custom(_compare_by_name)
	var out: Array = []
	out.append_array(builtin)
	out.append_array(user)
	return out


# Internal: scan a single directory, return the entries with the
# is_builtin flag set. Tolerant of missing directory (returns []),
# unloadable .tres files (skipped), and non-preset .tres files
# parked in the directory by accident (duck-typed out).
static func _list_dir(dir_path: String, is_builtin: bool) -> Array:
	var out: Array = []
	if not DirAccess.dir_exists_absolute(dir_path):
		return out
	var dir: DirAccess = DirAccess.open(dir_path)
	if dir == null:
		return out
	dir.list_dir_begin()
	while true:
		var entry: String = dir.get_next()
		if entry == "":
			break
		if entry.begins_with(".") or dir.current_is_dir():
			continue
		if not entry.ends_with(".tres"):
			continue
		var path: String = dir_path.path_join(entry)
		var res: Resource = load(path)
		if res == null:
			continue
		if not _looks_like_preset(res):
			continue
		var display_name: String = String(res.get("preset_name"))
		if display_name.is_empty():
			display_name = entry.get_basename()
		out.append({
			"name":       display_name,
			"path":       path,
			"preset":     res,
			"is_builtin": is_builtin,
		})
	dir.list_dir_end()
	return out


static func _compare_by_name(a: Dictionary, b: Dictionary) -> bool:
	return String(a["name"]) < String(b["name"])


# Save `preset` under the given display name. The display name is
# sanitized to a path-safe filename root; the resulting .tres
# replaces any existing file with the same sanitized name.
#
# Always writes to USER_PRESET_DIR. Built-in presets are read-only
# from this helper's perspective — to capture a tweak of a built-in,
# the designer saves their tweaked variant here with a new (or same)
# name, which lands alongside other user presets without touching
# the addon directory.
#
# Returns the saved path on success, "" on failure (failure
# warnings are pushed via push_warning so the editor's output
# panel surfaces them).
static func save_preset(preset: Resource, display_name: String) -> String:
	_ensure_user_dir()
	var safe: String = _sanitize_filename(display_name)
	if safe.is_empty():
		push_warning("[gool] save_preset: empty name after sanitization "
				+ "(input was '%s')" % display_name)
		return ""
	var path: String = USER_PRESET_DIR.path_join(safe + ".tres")
	# Stamp the display name onto the resource before saving so the
	# in-file label matches what the user typed (the filename is the
	# sanitized version; the friendly label preserved in the file).
	preset.set("preset_name", display_name)
	var err: int = ResourceSaver.save(preset, path)
	if err != OK:
		push_warning("[gool] save_preset(%s): error %d" % [path, err])
		return ""
	return path


# Does a path exist in USER_PRESET_DIR that would be overwritten by
# save_preset(name)? Used by the inspector to put up an overwrite-
# confirmation dialog before the save fires.
#
# Does NOT consider BUILTIN_PRESET_DIR — saving a preset with the
# same name as a built-in is allowed (creates a user preset that
# co-exists in the picker with the built-in). This is the natural
# "I want my project's 'Concrete bunker' to differ from gool's
# default" workflow.
static func would_overwrite(display_name: String) -> bool:
	var safe: String = _sanitize_filename(display_name)
	if safe.is_empty():
		return false
	var path: String = USER_PRESET_DIR.path_join(safe + ".tres")
	return FileAccess.file_exists(path)


# Create USER_PRESET_DIR if it doesn't already exist. Recursive so
# the parent `res://gool/` is created too if a fresh project doesn't
# have it yet. BUILTIN_PRESET_DIR is never created here — it's
# shipped with the addon and present in any installation.
static func _ensure_user_dir() -> void:
	if DirAccess.dir_exists_absolute(USER_PRESET_DIR):
		return
	var err: int = DirAccess.make_dir_recursive_absolute(USER_PRESET_DIR)
	if err != OK:
		push_warning("[gool] could not create %s: error %d"
				% [USER_PRESET_DIR, err])


# Path-safe filename: keep ASCII letters, digits, underscore, hyphen.
# Anything else collapses to a single underscore. Leading/trailing
# underscores trimmed. Empty result means "name was nothing but
# punctuation" — caller treats that as invalid.
static func _sanitize_filename(name: String) -> String:
	var s: String = ""
	var i: int = 0
	while i < name.length():
		var ch: String = name.substr(i, 1)
		var b: int = ch.unicode_at(0)
		var is_alnum: bool = (
				(b >= 0x30 and b <= 0x39)
				or (b >= 0x41 and b <= 0x5A)
				or (b >= 0x61 and b <= 0x7A))
		if is_alnum or ch == "_" or ch == "-":
			s += ch
		else:
			# Collapse runs of non-alnum into a single underscore
			# so "A & B" becomes "A_B" not "A___B".
			if not s.ends_with("_"):
				s += "_"
		i += 1
	# Trim leading/trailing underscores.
	while s.begins_with("_"):
		s = s.substr(1)
	while s.ends_with("_"):
		s = s.substr(0, s.length() - 1)
	return s


# Duck-typed preset detection. Returns true if the resource has the
# seven band fields we'd expect from a GoolMaterialEqPreset. We don't
# do a strict `is GoolMaterialEqPreset` check because the class_name
# registry may not be populated when the editor inspector first runs,
# and a stale class registration after a v0.61.2 install would cause
# all previously-saved presets to vanish from the picker. Duck typing
# is more robust here for what is, in practice, "is this resource
# something I can apply as an EQ preset".
static func _looks_like_preset(res: Resource) -> bool:
	for prop in [
			"low_freq_hz", "low_gain_db",
			"mid_freq_hz", "mid_gain_db", "mid_q",
			"high_freq_hz", "high_gain_db"]:
		# Resource.get returns null for unknown properties.
		if res.get(prop) == null:
			return false
	return true
