# addons/gool/tools/prefab_smoke_test.gd
#
# Static-analysis smoke test for the gool addon. Catches the bug
# class that broke v0.44.1 and v0.44.2: "a prefab calls
# `_runtime.X(...)` but the Gool autoload doesn't expose X, so
# the call fails at runtime with either a crash ('Nonexistent
# function') or a silent no-op (if guarded by has_method)".
#
# How it works:
#
#   1. Parse `addons/gool/runtime_singleton.gd` and extract every
#      `func name()` declaration → set of methods the autoload
#      exposes.
#   2. Walk every .gd file under `addons/gool/prefabs/` and
#      `addons/gool/resources/`.
#   3. Find every `_runtime.X(...)` and `Gool.X(...)` callsite.
#   4. For each X: if not in the autoload's method set AND not a
#      built-in Object/Node method (has_method, get_node, etc),
#      AND not an autoload signal or constant — report it.
#
# Bugs of the v0.44.1 / v0.44.2 shape light up immediately, before
# anyone has to instantiate the prefab in a real scene.
#
# How to run:
#
#   Editor menu: Project ▸ Tools ▸ "gool: Run prefab smoke test"
#   (registered by plugin.gd when the addon is enabled).
#
#   Or programmatically:
#       var t := preload("res://addons/gool/tools/prefab_smoke_test.gd").new()
#       var ok: bool = t.run()
#       # ok == true means no issues found

@tool
extends RefCounted

# Methods that ALL Godot Objects / Nodes provide — these aren't
# defined on the gool autoload and shouldn't be flagged.
const _BUILTIN_OBJECT_METHODS := [
	"has_method", "has_signal", "has_meta", "set_meta", "get_meta",
	"get", "set", "get_node", "get_node_or_null", "get_parent",
	"get_children", "get_child", "get_child_count", "find_child",
	"get_property_list", "get_class", "is_class", "is_inside_tree",
	"queue_free", "free", "call", "call_deferred", "callv",
	"connect", "disconnect", "emit_signal", "is_connected",
	"add_child", "remove_child", "tree_exited", "tree_entered",
	"_to_string", "_get", "_set", "_init", "_ready", "_process",
	"add_to_group", "remove_from_group", "is_in_group",
	"duplicate", "instantiate",
]

# Symbols accessed via `_runtime.X` or `Gool.X` that aren't methods —
# signals, instance variables, constants. These pass through the
# regex but aren't expected to be func declarations.
const _AUTOLOAD_NON_METHOD_SYMBOLS := [
	"ready_to_play",                   # signal
	"_eq_intensity",                   # var (private but accessed cross-prefab in some paths)
	"_listener_eq_bus_name",           # var (same)
	"_runtime",                        # the C++ runtime instance itself
]

var _autoload_methods: PackedStringArray = PackedStringArray()
var _findings: Array = []

# Entry point — runs the analysis and prints a report. Returns
# true if no issues were found, false if any were.
func run() -> bool:
	print("[gool-smoke] Starting static analysis...")
	_autoload_methods = _enumerate_autoload_methods()
	print("[gool-smoke]   Autoload exposes %d public methods" \
			% _autoload_methods.size())

	var files := _list_gd_files_under(["res://addons/gool/prefabs/",
			"res://addons/gool/resources/"])
	print("[gool-smoke]   Scanning %d .gd files" % files.size())

	_findings = []
	for f in files:
		_scan_file(f)

	_print_report()
	return _findings.is_empty()

# ─── Analysis steps ────────────────────────────────────────────

func _enumerate_autoload_methods() -> PackedStringArray:
	var path := "res://addons/gool/runtime_singleton.gd"
	if not FileAccess.file_exists(path):
		push_error("[gool-smoke] %s not found" % path)
		return PackedStringArray()
	var src: String = FileAccess.get_file_as_string(path)
	var methods := PackedStringArray()
	var regex := RegEx.new()
	# Match `func name(` at start-of-line. Also catch the
	# class-level signal declarations and consts so we don't
	# flag them as missing.
	regex.compile("^(?:func|signal|const|var)\\s+([a-zA-Z_][a-zA-Z_0-9]*)")
	for match_obj in regex.search_all(src):
		methods.append(match_obj.get_string(1))
	return methods

func _list_gd_files_under(roots: Array) -> PackedStringArray:
	var out := PackedStringArray()
	for root in roots:
		_collect_gd_files_into(root, out)
	return out

func _collect_gd_files_into(dir_path: String, out: PackedStringArray) -> void:
	var dir := DirAccess.open(dir_path)
	if dir == null:
		return
	dir.list_dir_begin()
	var entry := dir.get_next()
	while entry != "":
		if entry.begins_with("."):
			entry = dir.get_next()
			continue
		var full := dir_path.path_join(entry)
		if dir.current_is_dir():
			_collect_gd_files_into(full, out)
		elif entry.ends_with(".gd"):
			out.append(full)
		entry = dir.get_next()
	dir.list_dir_end()

func _scan_file(path: String) -> void:
	var src: String = FileAccess.get_file_as_string(path)
	if src.is_empty():
		return
	# Match either `_runtime.X(` or `Gool.X(` — open paren after the
	# identifier filters to method calls, not property reads.
	# Property reads still happen for signals (connect/.emit) — but
	# those routes are handled in the autoload symbol allowlist.
	var regex := RegEx.new()
	regex.compile("(_runtime|Gool)\\.([a-zA-Z_][a-zA-Z_0-9]*)\\s*\\(")
	for match_obj in regex.search_all(src):
		var qualifier: String = match_obj.get_string(1)
		var method: String = match_obj.get_string(2)
		if _is_known_symbol(method):
			continue
		if not _autoload_methods.has(method):
			_findings.append({
				"file": path,
				"qualifier": qualifier,
				"method": method,
			})

func _is_known_symbol(name: String) -> bool:
	if _BUILTIN_OBJECT_METHODS.has(name):
		return true
	if _AUTOLOAD_NON_METHOD_SYMBOLS.has(name):
		return true
	return false

# ─── Reporting ─────────────────────────────────────────────────

func _print_report() -> void:
	if _findings.is_empty():
		print("[gool-smoke] ✓ PASS — no missing autoload wrappers detected")
		print("[gool-smoke]   Every prefab callsite has a matching autoload method")
		return
	# Aggregate by missing method name so the report doesn't repeat
	# the same problem 5 times if 5 different files call it.
	var by_method: Dictionary = {}
	for f in _findings:
		var key: String = f.method
		if not by_method.has(key):
			by_method[key] = []
		by_method[key].append(f)
	print("[gool-smoke] ✗ FAIL — %d distinct missing wrappers across %d callsites:"
			% [by_method.size(), _findings.size()])
	print("")
	for method in by_method.keys():
		print("  %s" % method)
		for f in by_method[method]:
			var short_file: String = String(f.file).replace(
					"res://addons/gool/", "")
			print("    called as %s.%s in %s" % [f.qualifier, method, short_file])
		print("")
	print("[gool-smoke] If C++ binding for the method exists, add an")
	print("[gool-smoke] autoload wrapper in runtime_singleton.gd. Same")
	print("[gool-smoke] shape as the v0.44.1 / v0.44.2 fixes.")
