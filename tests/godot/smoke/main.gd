# tests/godot/smoke/main.gd
#
# Headless smoke test for the gool Godot addon. Run via the CI
# `godot-headless-smoke` job:
#
#   godot --headless --quit-after 5 --path tests/godot/smoke
#
# What it does:
#   1. Recursively walks res://addons/gool/, collecting every .gd
#      file (addon scripts, prefab scripts, the resource type).
#   2. Calls load() on each path. In Godot 4.x, load() returns
#      null on parse failure and pushes a "Parse Error" diagnostic
#      to stderr — both the null return and the stderr line are
#      independent CI failure signals.
#   3. Tallies failures, prints a SMOKE OK / SMOKE FAIL summary,
#      and sets the process exit code accordingly.
#
# Why this exists:
#   Before v0.21.1, the project had no Godot-side test surface.
#   Seven prefabs shipped from v0.13.0 onward had a copy-pasted
#   GDScript syntax error (unescaped quotes around "gool" inside a
#   double-quoted string) that made every prefab silently fail to
#   parse in a real Godot project. No automated check caught it
#   across eight release cycles. This smoke closes that gap.
#
# What this does NOT cover:
#   - GDExtension binding behavior. The compiled .so/.dll/.dylib
#     is not loaded here; that's the build-gdextension job's
#     domain. Adding a full binding-integrated smoke is a
#     follow-up (would need to pull the build artifact in).
#   - Runtime behavior of the prefabs. Each prefab's _ready
#     would hit the "Gool autoload not found" path here since we
#     don't enable the plugin or wire the autoload. We're testing
#     parse correctness, not behavior.

extends Node

func _ready() -> void:
    print("[smoke] starting; cwd=%s" % OS.get_executable_path().get_base_dir())

    var addon_root: String = "res://addons/gool"
    if not DirAccess.dir_exists_absolute(addon_root):
        push_error(
            "SMOKE FAIL: %s does not exist. The CI step that copies "
            % addon_root
            + "the addon into the smoke project did not run, or the "
            + "smoke project layout has drifted from what main.gd "
            + "expects."
        )
        # Note: pre-v0.21.4 versions used OS.set_exit_code(N) + quit(),
        # which fails to parse on Godot 4.2 with "Static function
        # set_exit_code() not found in base GDScriptNativeClass" — the
        # method exists in Godot 3.x but not 4.x. get_tree().quit(N)
        # is the 4.x idiom and accepts the exit code directly.
        get_tree().quit(1)
        return

    var paths: Array[String] = []
    _collect_scripts(addon_root, paths)
    if paths.is_empty():
        push_error(
            "SMOKE FAIL: no .gd files found under %s. Addon directory "
            % addon_root
            + "exists but contains no scripts — the copy step likely "
            + "only copied SVG/cfg files and missed the scripts."
        )
        get_tree().quit(1)
        return

    print("[smoke] found %d GDScript files under %s" % [paths.size(), addon_root])
    var failures: Array[String] = []
    for path in paths:
        var script: Resource = load(path)
        if script == null:
            failures.append(path)
            push_error(
                "SMOKE FAIL: load() returned null for %s — most likely "
                % path
                + "a GDScript parse error. Look for a 'Parse Error:' "
                + "line above this one in the log for the specific "
                + "issue (line number + reason)."
            )
        else:
            print("[smoke] OK: %s" % path)

    if failures.is_empty():
        print("[smoke] SMOKE OK: all %d scripts parsed and loaded." % paths.size())
        get_tree().quit(0)
    else:
        push_error(
            "SMOKE FAIL: %d of %d scripts failed to load: %s"
            % [failures.size(), paths.size(), str(failures)]
        )
        get_tree().quit(1)


# Recursive directory walk. Appends every *.gd path found under
# `dir_path` (and its subdirectories) to `out`. Skips hidden files
# (anything starting with "." — chiefly .godot/, .gd.uid, dotfiles).
func _collect_scripts(dir_path: String, out: Array[String]) -> void:
    var dir: DirAccess = DirAccess.open(dir_path)
    if dir == null:
        push_warning(
            "[smoke] could not open directory %s (error %d); skipping."
            % [dir_path, DirAccess.get_open_error()]
        )
        return
    dir.list_dir_begin()
    while true:
        var name: String = dir.get_next()
        if name == "":
            break
        if name.begins_with("."):
            continue
        var child_path: String = dir_path.path_join(name)
        if dir.current_is_dir():
            _collect_scripts(child_path, out)
        elif name.ends_with(".gd"):
            out.append(child_path)
    dir.list_dir_end()
