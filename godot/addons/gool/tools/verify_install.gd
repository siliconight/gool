# addons/gool/tools/verify_install.gd
#
# v0.78.6 — headless install verification driver.
#
# Designed to be invoked by gool-install.cmd / quickinstall.ps1 /
# quickinstall.sh after the addon is deployed into a user's project.
# The shell script runs Godot with this scene as the main entry point:
#
#   godot --headless --audio-driver Dummy --quit-after 5 \
#         --path <project> res://addons/gool/tools/verify_install.tscn
#
# What happens:
#   1. Godot starts in headless mode (no window, dummy audio backend).
#   2. The Gool autoload's _ready runs as usual — that's the install
#      step we're verifying: did the binding load, did the runtime
#      initialize, did monitors register.
#   3. THIS script's _ready then runs, calls Gool.diagnose(), prints
#      the report, sets the OS exit code (0 if no failures, 1
#      otherwise), and quits.
#   4. The shell script reads the exit code and prints a final
#      pass/fail line.
#
# Design notes:
#   - We pass --audio-driver Dummy in the shell scripts so gool's
#     init doesn't fail on a CI box / headless invocation that has
#     no real audio device. Without Dummy, init returns false and
#     diagnose() would report FAIL even on a healthy install.
#   - We rely on the keyword "FAILED:" in diagnose's verdict line
#     (the only place it appears) to decide pass/fail. Warnings
#     don't fail — "audio device returned empty description" is
#     expected under the Dummy driver, not an install bug.
#   - --quit-after is a safety net. In practice we call
#     get_tree().quit() ourselves once the report is printed; the
#     CLI flag is the fallback if something hangs.

extends Node

func _ready() -> void:
	# Print a banner so the shell script's captured output is easy
	# to grep for and the user sees something distinct from regular
	# log noise. The separator lines are 60 chars wide to match
	# diagnose()'s own formatting.
	print("")
	print("============================================================")
	print("  gool install verification (v0.78.6)")
	print("============================================================")

	# Defensive: if the Gool autoload didn't load, the install is
	# broken in a way diagnose() can't even report on. Surface that
	# specifically.
	if not Engine.has_singleton("Gool") and get_node_or_null("/root/Gool") == null:
		print("")
		print("  [fail] Gool autoload is not present at /root/Gool.")
		print("         The addon was deployed but Godot did not register")
		print("         the autoload. Open Project Settings → Autoload and")
		print("         add res://addons/gool/runtime_singleton.gd as 'Gool'.")
		print("")
		print("VERIFICATION FAILED — Gool autoload missing")
		_quit_with_code(1)
		return

	# Hand off to diagnose. The string it returns is already formatted
	# with [ok]/[warn]/[fail] tags and a verdict line.
	#
	# v0.80.4: Use path-based lookup, not the bare `Gool` identifier.
	# GDScript resolves bare autoload identifiers at PARSE time, even
	# inside function bodies. When Godot parses every .gd file in the
	# project on editor open — including this addon file — and the
	# plugin hasn't activated yet, the `Gool` identifier isn't
	# registered. The script fails to parse with "Identifier 'Gool'
	# not declared in the current scope" even though we'd never
	# execute this code path. Using get_node("/root/Gool") passes the
	# string through unresolved at parse time; the lookup happens at
	# runtime, where the defensive guard above has already proved the
	# autoload is registered.
	var gool := get_node("/root/Gool")
	var report: String = gool.diagnose()
	print(report)

	# Pass/fail is decided by the diagnose verdict, not by hand-rolling
	# our own check logic. Keeps the two surfaces consistent.
	var failed: bool = report.find("FAILED:") != -1
	print("")
	if failed:
		print("VERIFICATION FAILED — see [fail] lines above")
		_quit_with_code(1)
	else:
		print("VERIFICATION PASSED")
		_quit_with_code(0)


# Helper: set exit code, then quit. Godot 4's SceneTree.quit() accepts
# an exit code directly; OS.set_exit_code() is the older equivalent and
# also works. Using SceneTree.quit() because it's the documented
# canonical form.
func _quit_with_code(code: int) -> void:
	get_tree().quit(code)
