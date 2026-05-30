# Copyright 2026 Brannen Graves
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
# implied. See the License for the specific language governing permissions
# and limitations under the License.

# addons/gool/tools/fps_scene_smoke_test.gd
#
# Scene-level smoke test for FPS-shaped gool integrations.
# Complements prefab_smoke_test.gd (v0.45.0) — that one does
# static analysis of GDScript source for cross-reference bugs;
# this one scans .tscn files for gool prefab usage and cross-
# references with gool/config.json to detect common scene-level
# misconfigurations that don't crash but produce broken audio.
#
# Catches the bug class:
#   "I added a ReverbZone but my bus chain has no Reverb effect,
#    so the zone fires its enter event but nothing audibly
#    changes — and I have no idea why."
#
# How it works:
#
#   1. Walk every .tscn file under res:// (excluding addons/)
#   2. For each scene, detect references to gool prefab scripts:
#        - GoolListener3D
#        - AudioEmitter3D
#        - ReverbZone
#        - VoiceChatPlayer
#        - AudioMaterialTag
#        - NetworkedAudioEvent / NetworkedAudioEmitter3D
#        - MusicStateController
#   3. Load gool/config.json — enumerate buses + check for
#        Reverb effect anywhere in the chain.
#   4. Cross-reference: flag scenes that use a prefab without
#        the corresponding config support.
#   5. Print a structured report.
#
# Bugs of the "added a feature but didn't wire its dependency"
# shape light up before you waste 20 minutes wondering why audio
# is silent.
#
# How to run:
#
#   Editor menu: Project ▸ Tools ▸ "gool: Run FPS scene smoke
#   test" (registered by plugin.gd when the addon is enabled).
#
#   Or programmatically:
#       var t := preload("res://addons/gool/tools/fps_scene_smoke_test.gd").new()
#       var ok: bool = t.run()
#       # ok == true means no issues found

@tool
extends RefCounted

# Gool prefab .gd script paths. Scenes that reference any of
# these in their script= or instance= properties are using gool
# prefabs. Keyed by short label for report readability.
const _GOOL_PREFAB_SCRIPTS := {
	"GoolListener3D":            "res://addons/gool/prefabs/gool_listener_3d.gd",
	"AudioEmitter3D":            "res://addons/gool/prefabs/audio_emitter_3d.gd",
	"ReverbZone":                "res://addons/gool/prefabs/reverb_zone.gd",
	"VoiceChatPlayer":           "res://addons/gool/prefabs/voice_chat_player.gd",
	"AudioMaterialTag":          "res://addons/gool/prefabs/audio_material_tag.gd",
	"NetworkedAudioEvent":       "res://addons/gool/prefabs/networked_audio_event.gd",
	"NetworkedAudioEmitter3D":   "res://addons/gool/prefabs/networked_audio_emitter_3d.gd",
	"MusicStateController":      "res://addons/gool/prefabs/music_state_controller.gd",
	"FootstepSurfacePlayer":     "res://addons/gool/prefabs/footstep_surface_player.gd",
	"GoolDebugOverlay":          "res://addons/gool/prefabs/gool_debug_overlay.gd",
}

# Findings accumulated during run. Each entry:
#   { "scene": String, "severity": String, "issue": String }
# Severity: "error" | "warning" | "info"
var _findings: Array = []

# Per-scene usage records — for the summary table.
#   { scene_path: { prefab_label: count, ... } }
var _scene_usage: Dictionary = {}

# Cached config.json analysis.
var _config: Dictionary = {}

# ─── Entry point ───────────────────────────────────────────────

func run() -> bool:
	print("[gool-fps-smoke] Starting FPS scene analysis…")
	_findings = []
	_scene_usage = {}

	_config = _analyze_config_json()
	if _config.is_empty():
		print("[gool-fps-smoke]   res://gool/config.json not found or invalid")
		print("[gool-fps-smoke]   FPS scene checks need a config — aborting.")
		return false

	_print_config_summary()

	var scenes := _list_tscn_files_under_res()
	print("[gool-fps-smoke]   Scanning %d .tscn files (excluding addons/)…"
			% scenes.size())

	for path in scenes:
		_analyze_scene(path)

	_cross_reference()
	_print_report()
	return _findings.filter(func(f): return f.severity == "error").is_empty()

# ─── Config analysis ───────────────────────────────────────────

func _analyze_config_json() -> Dictionary:
	var path := "res://gool/config.json"
	if not FileAccess.file_exists(path):
		return {}
	var text: String = FileAccess.get_file_as_string(path)
	var parsed = JSON.parse_string(text)
	if not (parsed is Dictionary):
		return {}

	var buses_v: Variant = parsed.get("buses", [])
	if not (buses_v is Array):
		return {}
	var buses: Array = buses_v

	# Collect bus names + which buses have a reverb effect anywhere
	# in the chain. ReverbZone needs at least one bus with a Reverb
	# effect to do anything audible.
	var bus_names: Array = []
	var buses_with_reverb: Array = []
	var buses_with_reverb_eq_slots: Array = []  # adjacent biquad + reverb pattern
	for b_v in buses:
		if not (b_v is Dictionary):
			continue
		var b: Dictionary = b_v
		var name: String = String(b.get("name", ""))
		bus_names.append(name)
		var effects_v: Variant = b.get("effects", [])
		if not (effects_v is Array):
			continue
		var effects: Array = effects_v
		var reverb_idx: int = -1
		for i in range(effects.size()):
			if effects[i] is Dictionary \
					and String(effects[i].get("kind", "")) == "reverb":
				reverb_idx = i
				break
		if reverb_idx >= 0:
			buses_with_reverb.append(name)
			# Check for adjacent biquads (v0.47.0 EQ shaping)
			var has_biquad_before: bool = (reverb_idx > 0
					and effects[reverb_idx - 1] is Dictionary
					and String(effects[reverb_idx - 1].get("kind", "")) == "biquad")
			var has_biquad_after: bool = (reverb_idx + 1 < effects.size()
					and effects[reverb_idx + 1] is Dictionary
					and String(effects[reverb_idx + 1].get("kind", "")) == "biquad")
			if has_biquad_before and has_biquad_after:
				buses_with_reverb_eq_slots.append(name)

	return {
		"buses": bus_names,
		"has_voice_bus": "Voice" in bus_names,
		"has_dialogue_bus": "Dialogue" in bus_names,
		"has_music_bus": "Music" in bus_names,
		"has_ambient_bus": "Ambient" in bus_names,
		"buses_with_reverb": buses_with_reverb,
		"buses_with_reverb_eq_slots": buses_with_reverb_eq_slots,
	}

func _print_config_summary() -> void:
	print("[gool-fps-smoke]   config.json buses: %s"
			% str(_config.get("buses", [])))
	var reverb_buses: Array = _config.get("buses_with_reverb", [])
	if reverb_buses.is_empty():
		print("[gool-fps-smoke]   No bus has a Reverb effect.")
	else:
		print("[gool-fps-smoke]   Buses with Reverb: %s" % str(reverb_buses))
	var eq_buses: Array = _config.get("buses_with_reverb_eq_slots", [])
	if not eq_buses.is_empty():
		print("[gool-fps-smoke]   Buses with v0.47.0 EQ shaping (biquad+reverb+biquad): %s"
				% str(eq_buses))

# ─── Scene scanning ────────────────────────────────────────────

# Walk res:// (skipping addons/, .godot/, .git/) and collect
# every .tscn path.
func _list_tscn_files_under_res() -> PackedStringArray:
	var out := PackedStringArray()
	_collect_tscn_into("res://", out)
	return out

func _collect_tscn_into(dir_path: String, out: PackedStringArray) -> void:
	var dir := DirAccess.open(dir_path)
	if dir == null:
		return
	dir.list_dir_begin()
	while true:
		var entry := dir.get_next()
		if entry == "":
			break
		if entry == "." or entry == "..":
			continue
		# Skip directories that don't host game scenes.
		if entry == "addons" or entry.begins_with(".") \
				or entry == "build" or entry == "dist":
			continue
		var full_path := dir_path.path_join(entry)
		if dir.current_is_dir():
			_collect_tscn_into(full_path, out)
		elif entry.ends_with(".tscn"):
			out.append(full_path)
	dir.list_dir_end()

# For each scene, count occurrences of each gool prefab script.
# Done by regex on the .tscn text (it's a key=value text format
# that always includes script paths as `path="res://..."`).
func _analyze_scene(scene_path: String) -> void:
	if not FileAccess.file_exists(scene_path):
		return
	var text: String = FileAccess.get_file_as_string(scene_path)
	var usage: Dictionary = {}
	for label in _GOOL_PREFAB_SCRIPTS:
		var prefab_path: String = _GOOL_PREFAB_SCRIPTS[label]
		var count: int = _count_occurrences(text, prefab_path)
		if count > 0:
			usage[label] = count
	if not usage.is_empty():
		_scene_usage[scene_path] = usage

# Count distinct occurrences of `path="<needle>"` in the scene
# text. Each match represents one ext_resource or sub_resource
# reference; instances of the same resource share the same line
# so this approximates "how many of this prefab are in scene".
func _count_occurrences(haystack: String, needle: String) -> int:
	var count: int = 0
	var idx: int = 0
	while true:
		var found: int = haystack.find(needle, idx)
		if found == -1:
			break
		count += 1
		idx = found + needle.length()
	return count

# ─── Cross-referencing ─────────────────────────────────────────

# Inspect the collected scene usage against config.json and emit
# findings for missing-dependency cases.
func _cross_reference() -> void:
	var any_listener: bool = false
	var any_3d_audio: bool = false
	var any_voice_chat: bool = false

	for scene_path in _scene_usage:
		var usage: Dictionary = _scene_usage[scene_path]
		var has_listener: bool = usage.has("GoolListener3D")
		var has_3d_emitter: bool = usage.has("AudioEmitter3D") \
				or usage.has("NetworkedAudioEmitter3D")
		var has_reverb_zone: bool = usage.has("ReverbZone")
		var has_voice_player: bool = usage.has("VoiceChatPlayer")

		if has_listener:
			any_listener = true
		if has_3d_emitter or has_reverb_zone:
			any_3d_audio = true
		if has_voice_player:
			any_voice_chat = true

		# Per-scene: ReverbZone in a scene without a listener — the
		# zone fires its enter event but the listener that's supposed
		# to "be in" the zone doesn't exist, so nothing audibly
		# changes. Common when devs add reverb zones to a level
		# scene but forget the listener is on a player scene that
		# instances separately.
		if has_reverb_zone and not has_listener:
			_findings.append({
				"scene": scene_path,
				"severity": "info",
				"issue": "Has ReverbZone but no GoolListener3D in this scene. "
						+ "OK if the listener is added at runtime (e.g. on a player "
						+ "instanced into this level). Warns only if your listener "
						+ "is meant to live in this scene file.",
			})

	# Project-level findings (across all scenes):

	if _config.get("buses_with_reverb", []).is_empty() and any_3d_audio:
		_findings.append({
			"scene": "(project-wide)",
			"severity": "error",
			"issue": "Scenes use ReverbZone or AudioEmitter3D but no bus in "
					+ "gool/config.json has a Reverb effect. Reverb zones "
					+ "will fire enter/exit events but produce no audible "
					+ "change. Add a Reverb effect to your Sfx bus or use "
					+ "the templates/config_fps.json template.",
		})

	if any_voice_chat and not _config.get("has_voice_bus", false):
		_findings.append({
			"scene": "(project-wide)",
			"severity": "error",
			"issue": "Scenes use VoiceChatPlayer but gool/config.json has no "
					+ "Voice bus. Voice will route to the fallback category "
					+ "bus, which is shared with other categories and probably "
					+ "ducked under Dialogue/Sfx — voice will be unintelligible "
					+ "under gunfire.",
		})

	if any_3d_audio and not any_listener:
		_findings.append({
			"scene": "(project-wide)",
			"severity": "warning",
			"issue": "Scenes use 3D audio (emitters/reverb zones) but no "
					+ ".tscn defines a GoolListener3D. If you instantiate "
					+ "the listener dynamically at runtime, this is fine. "
					+ "If you forgot to add one, all 3D audio will play at "
					+ "the world origin with broken spatialization.",
		})

	# Info: surface buses with the v0.47.0 EQ-shaping pattern so
	# devs know which buses ReverbZone's send_hpf_hz/return_lpf_hz
	# will actually affect.
	var eq_buses: Array = _config.get("buses_with_reverb_eq_slots", [])
	if not eq_buses.is_empty() and _scene_usage.values().any(
			func(u): return u.has("ReverbZone")):
		_findings.append({
			"scene": "(project-wide)",
			"severity": "info",
			"issue": "v0.47.0 EQ shaping (send_hpf_hz/return_lpf_hz) is "
					+ "available on these buses: %s. ReverbZones targeting "
					+ "any other bus will ignore EQ shaping @exports."
					% str(eq_buses),
		})

# ─── Report ────────────────────────────────────────────────────

func _print_report() -> void:
	print("")
	print("[gool-fps-smoke] ═══════════════════════════════════════════════")
	print("[gool-fps-smoke]   Per-scene gool prefab usage:")
	print("[gool-fps-smoke] ═══════════════════════════════════════════════")
	if _scene_usage.is_empty():
		print("[gool-fps-smoke]   (no scenes use gool prefabs)")
	else:
		var paths := _scene_usage.keys()
		paths.sort()
		for path in paths:
			var usage: Dictionary = _scene_usage[path]
			var parts: Array = []
			for label in usage:
				parts.append("%s×%d" % [label, usage[label]])
			print("[gool-fps-smoke]   %s" % path)
			print("[gool-fps-smoke]       %s" % ", ".join(parts))

	print("")
	print("[gool-fps-smoke] ═══════════════════════════════════════════════")
	if _findings.is_empty():
		print("[gool-fps-smoke]   ✓ No findings — FPS scene shape looks good.")
		print("[gool-fps-smoke] ═══════════════════════════════════════════════")
		return

	var errors: int = 0
	var warnings: int = 0
	var infos: int = 0
	for f in _findings:
		match f.severity:
			"error":   errors += 1
			"warning": warnings += 1
			"info":    infos += 1
	print("[gool-fps-smoke]   Findings: %d error(s), %d warning(s), %d info"
			% [errors, warnings, infos])
	print("[gool-fps-smoke] ═══════════════════════════════════════════════")
	for f in _findings:
		var prefix: String = ""
		match f.severity:
			"error":   prefix = "[ERROR]  "
			"warning": prefix = "[warn]   "
			"info":    prefix = "[info]   "
		print("[gool-fps-smoke] %s%s" % [prefix, f.scene])
		print("[gool-fps-smoke]            %s" % f.issue)
