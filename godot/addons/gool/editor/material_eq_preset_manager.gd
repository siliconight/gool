# addons/gool/editor/material_eq_preset_manager.gd
#
# v0.61.2 — Phase 6.E.4 (first cut): preset library scanner / saver.
#
# Tiny helper used by the inspector's "Save preset..." / "Load
# preset..." buttons. Hides the directory-scan and filename-
# sanitization details so the inspector's logic stays focused on UI.
#
# Storage convention:
#   res://gool/material_eq_presets/<sanitized_name>.tres
#
# Filenames are derived from preset_name via a path-safe pass:
# anything not in [A-Za-z0-9_-] becomes underscore. Cross-platform
# safe; no spaces, no quotes, no slashes. Designers see their
# free-form preset_name in the picker; the sanitized form is just
# the on-disk key.

@tool
extends RefCounted

const PRESET_DIR: String = "res://gool/material_eq_presets"
const PRESET_SCRIPT_PATH: String = (
		"res://addons/gool/resources/gool_material_eq_preset.gd")


# List every preset .tres file in PRESET_DIR. Returns an Array of
# Dictionaries:
#   { "name": String, "path": String, "preset": Resource }
# where "name" is the preset's display name (preset_name field, or
# the filename basename if preset_name is empty). Sorted by name
# for stable picker display.
#
# Returns [] if PRESET_DIR doesn't exist yet — that's a normal
# "this project has no presets saved" state, not an error.
static func list_presets() -> Array:
	var out: Array = []
	if not DirAccess.dir_exists_absolute(PRESET_DIR):
		return out
	var dir: DirAccess = DirAccess.open(PRESET_DIR)
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
		var path: String = PRESET_DIR.path_join(entry)
		var res: Resource = load(path)
		if res == null:
			continue
		# Filter out other .tres files designers might park in this
		# directory by accident. We accept anything that looks like
		# a preset (has the seven band fields). Strict type check
		# would require a script comparison which is fragile across
		# editor reloads; duck-typing is more forgiving.
		if not _looks_like_preset(res):
			continue
		var display_name: String = String(res.get("preset_name"))
		if display_name.is_empty():
			display_name = entry.get_basename()
		out.append({
			"name":   display_name,
			"path":   path,
			"preset": res,
		})
	dir.list_dir_end()
	out.sort_custom(
			func(a: Dictionary, b: Dictionary) -> bool:
				return String(a["name"]) < String(b["name"]))
	return out


# Save `preset` under the given display name. The display name is
# sanitized to a path-safe filename root; the resulting .tres
# replaces any existing file with the same sanitized name.
#
# Returns the saved path on success, "" on failure (failure
# warnings are pushed via push_warning so the editor's output
# panel surfaces them).
static func save_preset(preset: Resource, display_name: String) -> String:
	_ensure_dir()
	var safe: String = _sanitize_filename(display_name)
	if safe.is_empty():
		push_warning("[gool] save_preset: empty name after sanitization "
				+ "(input was '%s')" % display_name)
		return ""
	var path: String = PRESET_DIR.path_join(safe + ".tres")
	# Stamp the display name onto the resource before saving so the
	# in-file label matches what the user typed (the filename is the
	# sanitized version; the friendly label preserved in the file).
	preset.set("preset_name", display_name)
	var err: int = ResourceSaver.save(preset, path)
	if err != OK:
		push_warning("[gool] save_preset(%s): error %d" % [path, err])
		return ""
	return path


# Does a path exist that would be overwritten by save_preset(name)?
# Used by the inspector to put up an overwrite-confirmation dialog
# before the save fires.
static func would_overwrite(display_name: String) -> bool:
	var safe: String = _sanitize_filename(display_name)
	if safe.is_empty():
		return false
	var path: String = PRESET_DIR.path_join(safe + ".tres")
	return FileAccess.file_exists(path)


# Create PRESET_DIR if it doesn't already exist. Recursive so the
# parent `res://gool/` is created too if a fresh project doesn't
# have it yet.
static func _ensure_dir() -> void:
	if DirAccess.dir_exists_absolute(PRESET_DIR):
		return
	var err: int = DirAccess.make_dir_recursive_absolute(PRESET_DIR)
	if err != OK:
		push_warning("[gool] could not create %s: error %d"
				% [PRESET_DIR, err])


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
