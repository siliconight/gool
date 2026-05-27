# addons/gool/plugin.gd
#
# Editor plugin. On enable, does the work that would otherwise be
# manual setup for a new gool project:
#
#   1. Adds /root/Gool autoload pointing at the runtime singleton
#      script. This is what every prefab calls into.
#   2. Registers the custom prefab Nodes (AudioEmitter3D,
#      VoiceChatPlayer, MusicStateController, ReverbZone,
#      FootstepSurfacePlayer, NetworkedAudioEvent,
#      NetworkedAudioEmitter3D, GoolListener3D, GoolSoundBankLoader)
#      so they appear in the Add Node menu.
#   3. Writes a default config file at res://gool/config.json with
#      reasonable bus/attenuation/compression defaults if one
#      doesn't already exist. The runtime reads this on init.
#   4. Registers an EditorInspectorPlugin (v0.22.0) that provides
#      a dropdown of registered sound names for any prefab's
#      `sound_name` property — replaces the bare String editor.
#
# On disable, all of the above are reversed cleanly.

@tool
extends EditorPlugin

const AUTOLOAD_NAME := "Gool"
const AUTOLOAD_PATH := "res://addons/gool/runtime_singleton.gd"

# v0.43.0: DialogueDirector is a second autoload owning NPC bark
# queue/priority/per-speaker step-on logic. Registered alongside
# Gool so callers can write `DialogueDirector.bark(...)` from any
# script without per-scene wiring.
const DIALOGUE_DIRECTOR_AUTOLOAD_NAME := "DialogueDirector"
const DIALOGUE_DIRECTOR_AUTOLOAD_PATH := "res://addons/gool/dialogue_director.gd"

# v0.46.0: MultiplayerBridge is a third autoload that wires
# Godot's MultiplayerAPI (or any custom transport via signal hooks)
# to gool's replication / VOIP primitives. Transport-agnostic by
# design — see docs/networking_bridge.md.
const MULTIPLAYER_BRIDGE_AUTOLOAD_NAME := "MultiplayerBridge"
const MULTIPLAYER_BRIDGE_AUTOLOAD_PATH := "res://addons/gool/multiplayer_bridge.gd"

# v0.75.2: first-enable restart dialog. The first time gool is enabled
# in a project, Godot's GDScript parser may have already cached symbol
# tables for project scripts WITHOUT the three autoloads (Gool,
# DialogueDirector, MultiplayerBridge) registered. The autoload
# registrations land in project.godot during _enter_tree, but the
# parser's stale cache still emits a cascade of "Identifier 'Gool' not
# declared" errors — and in some cases hard-crashes the editor before
# the user sees the cascade. Reopening the project clears the cache and
# everything parses cleanly.
#
# Fix: detect first enable via a project setting, pop a modal explaining
# the situation, offer one-click restart via EditorInterface.restart_editor().
# Project-setting-gated so the dialog never fires again in this project,
# mirroring the v0.74.x getting-started banner dismiss pattern.
const _FIRST_ENABLE_KEY := "addons/gool/editor/first_enable_completed"

const PREFAB_DIR := "res://addons/gool/prefabs/"

# (class_name, base_node, script_path, icon_path)
# Icon paths are relative to PREFAB_DIR; concrete filename matches the
# script so each prefab in the Add Node menu picks up its own glyph
# instead of the generic Node3D / Area3D fallback.
const PREFABS := [
	["AudioEmitter3D",            "Node3D", "audio_emitter_3d.gd",            "audio_emitter_3d.svg"],
	["VoiceChatPlayer",           "Node3D", "voice_chat_player.gd",           "voice_chat_player.svg"],
	["MusicStateController",      "Node",   "music_state_controller.gd",      "music_state_controller.svg"],
	["ReverbZone",                "Area3D", "reverb_zone.gd",                 "reverb_zone.svg"],
	["FootstepSurfacePlayer",     "Node3D", "footstep_surface_player.gd",     "footstep_surface_player.svg"],
	["NetworkedAudioEvent",       "Node",   "networked_audio_event.gd",       "networked_audio_event.svg"],
	["NetworkedAudioEmitter3D",   "Node3D", "networked_audio_emitter_3d.gd",  "networked_audio_emitter_3d.svg"],
	# v0.21.0: designer-friendly integration nodes. GoolListener3D
	# eliminates the per-project hand-rolled set_listener_transform
	# loop; GoolSoundBankLoader pairs with the GoolSoundBank resource
	# so sound registration becomes a drag-and-drop inspector step
	# instead of a script-only chore.
	["GoolListener3D",            "Node3D", "gool_listener_3d.gd",            "gool_listener_3d.svg"],
	["GoolSoundBankLoader",       "Node",   "gool_sound_bank_loader.gd",      "gool_sound_bank_loader.svg"],
	# v0.23.1: drop-in debug HUD. Polls Gool.get_render_stats() at
	# 4 Hz and renders a corner overlay showing callback rate, peak
	# amplitude, active voices, audio device, etc. Useful for
	# development; shipping builds can leave it added with
	# visible_at_startup=false + toggle_key bound to a hidden hotkey.
	["GoolDebugOverlay",          "CanvasLayer", "gool_debug_overlay.gd",     "gool_debug_overlay.svg"],
	# v0.43.0: convenience prefabs for L4D2-style shooter wiring.
	# AudioMaterialTag is the inspector-friendly path to set a
	# parent collision body's gool_audio_material metadata without
	# making a .tres file. DialogueDirector is registered as a
	# separate autoload (see DIALOGUE_DIRECTOR_AUTOLOAD_* above)
	# rather than appearing here.
	["AudioMaterialTag",          "Node",   "audio_material_tag.gd",          "audio_material_tag.svg"],
	# v0.62.0: scene-level acoustic profile prefab. Drop into the
	# scene root, pick a GoolAcousticProfile from the dropdown (8
	# built-ins ship with gool at res://addons/gool/acoustic_profiles/;
	# user profiles live at res://gool/acoustic_profiles/), F5. The
	# scene's reverb bus is configured at scene load. ReverbZones
	# still override per-region on top.
	["GoolSceneProfile",          "Node",   "gool_scene_profile.gd",          "gool_scene_profile.svg"],
	# v0.63.0: Phase 7 Master FX Lite profile prefab. Drop into the
	# scene tree, pick a GoolMasterFxPreset from the dropdown (5
	# built-ins ship with gool at res://addons/gool/master_fx_presets/;
	# user presets save to res://gool/master_fx_presets/), F5. The
	# master bus chain is reconfigured at scene load. Pairs with
	# GoolSceneProfile (which handles per-scene reverb).
	["GoolMasterFxProfile",       "Node",   "gool_master_fx_profile.gd",      "gool_master_fx_profile.svg"],
]

const CONFIG_PATH := "res://gool/config.json"

# Default audio config written on plugin enable. Uses the v0.10
# richer schema:
#   - "buses" is an array of { name, parent, gain_db, silent, effects }
#   - effects are dicts with kind + per-kind fields
#   - sidechain_bus references resolve by bus name at engine init
#
# This default builds a ready-to-use multi-tier ducking topology
# (LocalSfx > RemoteSfx > Music) that gives the L4D2-style mix
# behavior out of the box. Projects that want a simpler graph can
# overwrite res://gool/config.json after the plugin enables.
const DEFAULT_CONFIG := {
	"sample_rate": 48000,
	"buffer_size": 512,
	"buses": [
		# v0.63.0: Master bus ships with the Phase 7 Master FX Lite
		# chain pre-installed — glue compressor + LUFS gain rider +
		# true-peak limiter. Defaults are the "Standard FPS" preset
		# (target -16 LUFS, ceiling -1 dBTP). Fresh projects sound
		# loudness-safe and glued without any setup. Drop a
		# GoolMasterFxProfile node to switch presets at runtime.
		#
		# To opt out: remove the master_control effect from this
		# bus, or bypass each stage by setting mc_*_enabled = false.
		{ "name": "Master", "gain_db": 0.0,
		  "effects": [
			{ "kind": "master_control",
			  "mc_glue_enabled":         true,
			  "mc_rider_enabled":        true,
			  "mc_limiter_enabled":      true,
			  "mc_glue_threshold_db":    -12.0,
			  "mc_glue_ratio":           2.0,
			  "mc_glue_attack_ms":       10.0,
			  "mc_glue_release_ms":      250.0,
			  "mc_glue_knee_db":         6.0,
			  "mc_glue_makeup_db":       0.0,
			  "mc_rider_target_lufs":    -16.0,
			  "mc_rider_time_constant_ms": 3000.0,
			  "mc_rider_max_gain_db":    6.0,
			  "mc_rider_min_gain_db":    -6.0,
			  "mc_rider_freeze_below_lufs": -6.0,
			  "mc_limiter_ceiling_dbtp": -1.0,
			  "mc_limiter_release_ms":   50.0,
			  "mc_limiter_lookahead_ms": 5.0 }
		  ] },

		# Music bus: ducks under the local-player SFX so the player's
		# own gun wins the mix, AND ducks under Dialogue so NPC
		# callouts ("TANK!") cut through the soundtrack. Two
		# compressors in series — each sidechained to a different
		# trigger bus.
		{ "name": "Music",  "parent": "Master", "gain_db": -3.0,
		  "effects": [
			{ "kind": "compressor",
			  "threshold_db": -30.0, "ratio": 8.0,
			  "attack_ms": 5.0,  "release_ms": 250.0,
			  "makeup_db": 0.0,
			  "knee_width_db": 4.0,
			  "sidechain_bus": "LocalSfx" },
			{ "kind": "compressor",
			  "threshold_db": -25.0, "ratio": 8.0,
			  "attack_ms": 3.0,  "release_ms": 250.0,
			  "makeup_db": 0.0,
			  "knee_width_db": 4.0,
			  "max_reduction_db": 12.0,
			  "sidechain_bus": "Dialogue" }
		  ] },

		# Submix that holds both local + remote SFX. Per-tier
		# processing happens on its children, not here.
		{ "name": "SfxAll", "parent": "Master" },

		# Local-player SFX — your gun, your footsteps, your reload.
		# Ducks under Dialogue so callouts cut through your own
		# gunfire (the core L4D2 mix feel). Drives the sidechain
		# triggers on Music + RemoteSfx — leave its trigger path
		# clean (this compressor only ducks LocalSfx, doesn't
		# alter what LocalSfx sends to the sidechains).
		{ "name": "LocalSfx", "parent": "SfxAll",
		  "effects": [
			{ "kind": "compressor",
			  "threshold_db": -22.0, "ratio": 6.0,
			  "attack_ms": 3.0,  "release_ms": 200.0,
			  "knee_width_db": 4.0,
			  "max_reduction_db": 10.0,
			  "sidechain_bus": "Dialogue" }
		  ] },

		# Remote-player SFX — teammate guns, NPC barks, ambient
		# impacts. Ducks under LocalSfx so the local action wins
		# over teammate action, AND ducks under Dialogue so
		# callouts cut through teammate gunfire.
		{ "name": "RemoteSfx", "parent": "SfxAll",
		  "effects": [
			{ "kind": "compressor",
			  "threshold_db": -30.0, "ratio": 8.0,
			  "attack_ms": 5.0,  "release_ms": 250.0,
			  "sidechain_bus": "LocalSfx" },
			{ "kind": "compressor",
			  "threshold_db": -22.0, "ratio": 6.0,
			  "attack_ms": 3.0,  "release_ms": 200.0,
			  "knee_width_db": 4.0,
			  "max_reduction_db": 10.0,
			  "sidechain_bus": "Dialogue" }
		  ] },

		# Voice chat — separate bus, not ducked (intelligibility
		# priority). If you want voice to also win over music,
		# add it as a sidechain bus on Music's compressor.
		{ "name": "Voice",   "parent": "Master", "gain_db": 0.0 },

		# v0.43.0: Dialogue — NPC barks, callouts, narration.
		# Drives ducking on Music + LocalSfx + RemoteSfx via the
		# sidechain compressors above. Itself has no effects, so
		# dialogue plays at full level uncolored. Route bark
		# sounds in your sound bank with "bus": "Dialogue" — the
		# DialogueDirector autoload calls Gool.play_3d which uses
		# the sound's bank-defined bus.
		{ "name": "Dialogue", "parent": "Master", "gain_db": 0.0 },

		# Ambient world bed — quiet, doesn't trigger any ducker.
		{ "name": "Ambient", "parent": "Master", "gain_db": -6.0 }
	],

	# Default category routing. Hosts can override per-emitter when
	# registering sounds; this is the fallback for emitters that
	# don't specify a target bus explicitly.
	"category_routing": {
		"music":    "Music",
		"sfx":      "LocalSfx",   # safe default: assume "your" sfx
		"voice":    "Voice",
		"ambience": "Ambient",
		"ui":       "Master",
		"dialogue": "Dialogue"   # v0.43.0: now routes to the dedicated bus
	}
}

const INSPECTOR_PLUGIN_PATH := "res://addons/gool/editor/sound_name_inspector.gd"

# v0.59.2: Phase 6.E.1 — per-material EQ curve preview in the
# inspector. Triggers when a GoolAudioMaterial.tres is selected;
# renders a frequency-response plot of the engine's curve for the
# selected material, plus a numerical readout and a realism-
# intensity slider tied to ProjectSettings("gool/material_eq/
# intensity"). See addons/gool/editor/material_eq_inspector.gd
# for the full surface description.
const MATERIAL_EQ_INSPECTOR_PATH := "res://addons/gool/editor/material_eq_inspector.gd"

# v0.23.0: paths the auto-scaffolder creates on plugin enable.
# Idempotent — anything that already exists is left alone, so a
# project that's manually arranged its sounds/ folder differently
# (or doesn't want our defaults at all) only sees the scaffolder
# add what's missing, never overwrite. Same goes for bank.tres:
# if the user's already got one (their own custom bank, or our
# bank from a previous enable), we don't touch it.
const SOUNDS_ROOT := "res://sounds"
const SOUND_SUBFOLDERS := ["sfx", "music", "voice", "ambience", "ui"]
const BANK_PATH := "res://sounds/bank.tres"
const FOLDER_SOUND_BANK_SCRIPT := "res://addons/gool/resources/gool_folder_sound_bank.gd"

# v0.23.0: prefab script paths used by the "Add gool 3D audio
# scaffolding to current scene" menu command. Centralized here
# rather than duplicating the PREFAB_DIR + filename concatenation
# in the scaffolding function.
const LISTENER_3D_SCRIPT := "res://addons/gool/prefabs/gool_listener_3d.gd"
const EMITTER_3D_SCRIPT := "res://addons/gool/prefabs/audio_emitter_3d.gd"
const BANK_LOADER_SCRIPT := "res://addons/gool/prefabs/gool_sound_bank_loader.gd"

# Submenu label under Project → Tools.
const TOOLS_MENU_NAME := "Gool"

# v0.24.0: read-only mixer dock script path + bottom-panel label.
const MIXER_DOCK_SCRIPT := "res://addons/gool/editor/mixer_dock.gd"
const MIXER_DOCK_LABEL := "Gool Mixer"

# v0.25.0: cross-process bridge from running game → editor mixer
# dock. EditorDebuggerPlugin instance is created in _enter_tree,
# registered via add_debugger_plugin, removed in _exit_tree.
const DEBUGGER_PLUGIN_SCRIPT := "res://addons/gool/editor/debugger_plugin.gd"

# Held instance of the inspector plugin. Stored so _exit_tree can
# unregister the same instance we registered (Godot's
# remove_inspector_plugin requires the original reference, not just
# a script path).
var _sound_name_inspector: EditorInspectorPlugin = null

# v0.59.2: Phase 6.E.1 inspector for GoolAudioMaterial resources.
# Held alongside the sound-name inspector since both follow the
# same EditorInspectorPlugin lifecycle.
var _material_eq_inspector: EditorInspectorPlugin = null

# v0.24.0: instance of the mixer dock Control, stored for symmetric
# removal in _exit_tree via remove_control_from_bottom_panel.
var _mixer_dock: Control = null

# v0.25.0: instance of the EditorDebuggerPlugin that receives
# bus stats from the running game and feeds them to the mixer dock.
var _debugger_plugin: EditorDebuggerPlugin = null

func _enter_tree() -> void:
	_add_autoload()
	_register_prefabs()
	_write_default_config_if_missing()
	_scaffold_sounds_tree_if_missing()   # v0.23.0
	_register_inspector_plugin()
	_register_material_eq_inspector()    # v0.59.2 — Phase 6.E.1
	_register_debugger_plugin()          # v0.25.0 (before mixer dock!)
	_register_mixer_dock()               # v0.24.0
	_connect_filesystem_watch()
	_register_tools_menu()               # v0.23.0
	_register_update_check_setting()     # v0.79.2
	print("[gool] plugin enabled — autoload, prefabs, default config, inspector, scaffolding, debugger bridge, mixer dock, tools menu installed.")
	# v0.75.2: prompt for editor restart on first enable so the
	# GDScript parser sees the autoloads on its next sweep with a
	# clean cache. No-op if this isn't the first enable. Called LAST
	# so all setup has settled before we offer to restart.
	_maybe_show_first_enable_restart_prompt()

func _exit_tree() -> void:
	_unregister_tools_menu()             # v0.23.0
	_disconnect_filesystem_watch()
	_unregister_mixer_dock()             # v0.24.0
	_unregister_debugger_plugin()        # v0.25.0
	_unregister_material_eq_inspector()  # v0.59.2 — Phase 6.E.1
	_unregister_inspector_plugin()
	_unregister_prefabs()
	_remove_autoload()
	print("[gool] plugin disabled.")

# v0.22.0: sound_name autocomplete dropdown for prefabs that
# reference registered sounds (AudioEmitter3D, NetworkedAudioEvent,
# NetworkedAudioEmitter3D, MusicStateController, etc). Scans the
# project for GoolSoundBank and GoolFolderSoundBank resources,
# aggregates their sound names, and replaces the default String
# editor with a dropdown showing those names. The user can still
# type a custom name via the "(custom)" option.
func _register_inspector_plugin() -> void:
	var script := load(INSPECTOR_PLUGIN_PATH)
	if script == null:
		push_warning(
			"[gool] could not load %s; sound_name autocomplete "
			% INSPECTOR_PLUGIN_PATH
			+ "dropdown is unavailable. The plain text editor still "
			+ "works as a fallback."
		)
		return
	_sound_name_inspector = script.new()
	add_inspector_plugin(_sound_name_inspector)

func _unregister_inspector_plugin() -> void:
	if _sound_name_inspector == null:
		return
	remove_inspector_plugin(_sound_name_inspector)
	_sound_name_inspector = null

# v0.59.2: Phase 6.E.1 per-material EQ curve preview. Inspector
# plugin that recognizes GoolAudioMaterial resources and adds a
# frequency-response plot + numerical readout + realism intensity
# slider under the resource's normal property fields. Read-only —
# the engine's per-material curve table is the source of truth;
# this is a designer-facing visualizer for it. See
# addons/gool/editor/material_eq_inspector.gd for the full
# implementation.
func _register_material_eq_inspector() -> void:
	var script := load(MATERIAL_EQ_INSPECTOR_PATH)
	if script == null:
		push_warning(
			"[gool] could not load %s; the per-material EQ "
			% MATERIAL_EQ_INSPECTOR_PATH
			+ "curve preview is unavailable. GoolAudioMaterial "
			+ "resources still work; you just won't see the "
			+ "curve visualization in the inspector."
		)
		return
	_material_eq_inspector = script.new()
	add_inspector_plugin(_material_eq_inspector)

func _unregister_material_eq_inspector() -> void:
	if _material_eq_inspector == null:
		return
	remove_inspector_plugin(_material_eq_inspector)
	_material_eq_inspector = null

# v0.24.0: read-only mixer dock — bottom-panel Control that polls
# Gool.get_bus_stats() at 30 Hz and renders per-bus peak meters.
# Lifecycle is symmetric with the inspector plugin: instantiated in
# _enter_tree, freed in _exit_tree.
#
# Why the bottom panel (vs. a sidebar dock): meters are something
# you glance at while debugging audio, not a permanent reference
# panel competing for screen space with the inspector. The bottom
# panel collapses out of the way when not in use, like Output and
# Debugger. Same convention DAWs use for their meter views.
#
# The dock script is loaded as a script (not preloaded as a class)
# so plugin disable / re-enable doesn't keep a stale class_name
# registered. The freshly loaded script is instantiated each time.
func _register_mixer_dock() -> void:
	var script := load(MIXER_DOCK_SCRIPT)
	if script == null:
		push_warning(
			"[gool] could not load %s; mixer dock unavailable. "
			% MIXER_DOCK_SCRIPT
			+ "The audio runtime still works without it; you just "
			+ "won't have visual bus-level metering in the editor."
		)
		return
	_mixer_dock = script.new()
	if _mixer_dock == null:
		push_warning("[gool] mixer dock script.new() returned null")
		return
	_mixer_dock.name = "GoolMixerDock"
	# v0.25.0: hand the dock a reference to the debugger plugin so
	# it can pull cached bus stats from the running game process.
	# Done BEFORE add_control_to_bottom_panel so the dock's first
	# _process tick can already see the plugin.
	if _debugger_plugin != null and _mixer_dock.has_method("set_debugger_plugin"):
		_mixer_dock.set_debugger_plugin(_debugger_plugin)
	add_control_to_bottom_panel(_mixer_dock, MIXER_DOCK_LABEL)

func _unregister_mixer_dock() -> void:
	if _mixer_dock == null:
		return
	remove_control_from_bottom_panel(_mixer_dock)
	_mixer_dock.queue_free()
	_mixer_dock = null

# v0.25.0: cross-process bridge for the mixer dock. The
# EditorDebuggerPlugin lives on the editor side and receives
# `EngineDebugger.send_message("gool:bus_stats", [...])` calls
# from the game-side Gool autoload during F5 playback. The
# mixer dock polls the plugin's cached snapshot at 30 Hz.
#
# Why a debugger plugin and not get_tree().root.get_node("Gool"):
# Godot 4 runs the editor and the running game in separate
# processes — they don't share a SceneTree. The debugger channel
# is the supported way to push per-frame data from game to editor.
# Same channel will carry editor→game commands in 3.3b/c/d
# (faders, S/M/B buttons, effect param edits).
func _register_debugger_plugin() -> void:
	var script := load(DEBUGGER_PLUGIN_SCRIPT)
	if script == null:
		push_warning(
			"[gool] could not load %s; mixer dock will not "
			% DEBUGGER_PLUGIN_SCRIPT
			+ "show meters during F5 playback. The audio runtime "
			+ "still works; only editor-side visibility is affected."
		)
		return
	_debugger_plugin = script.new()
	if _debugger_plugin == null:
		push_warning("[gool] debugger plugin script.new() returned null")
		return
	add_debugger_plugin(_debugger_plugin)

func _unregister_debugger_plugin() -> void:
	if _debugger_plugin == null:
		return
	remove_debugger_plugin(_debugger_plugin)
	_debugger_plugin = null

# v0.22.3: live filesystem watching for the sound_name autocomplete.
#
# The inspector plugin caches the discovered sound-name list (a
# project-wide scan of .tres files is expensive to redo on every
# inspector render). Before v0.22.3 that cache only refreshed when
# the plugin was re-enabled — so dropping a new audio file in, or
# adding a new GoolSoundBank, wouldn't appear in the dropdown until
# you toggled the plugin or restarted Godot.
#
# Now plugin.gd subscribes to EditorFileSystem.filesystem_changed
# (the editor signal that fires after any project file is added,
# removed, moved, or reimported) and invalidates the inspector's
# static cache. The next inspector render then does a fresh scan
# and picks up the new files/banks automatically.
#
# We own this connection here, in the EditorPlugin, rather than in
# the EditorInspectorPlugin itself, because EditorInspectorPlugin
# is a RefCounted with no _enter_tree/_exit_tree lifecycle — there
# is no clean place there to connect and (more importantly)
# disconnect the signal. plugin.gd has a well-defined lifecycle, so
# the connection is established in _enter_tree and torn down in
# _exit_tree, with no leak on plugin disable/re-enable.
func _connect_filesystem_watch() -> void:
	var efs := EditorInterface.get_resource_filesystem()
	if efs == null:
		push_warning(
			"[gool] EditorFileSystem unavailable; sound_name "
			+ "autocomplete won't auto-refresh on file changes. "
			+ "Toggle the plugin or restart Godot to refresh the "
			+ "dropdown after adding sound banks."
		)
		return
	if not efs.filesystem_changed.is_connected(_on_filesystem_changed):
		efs.filesystem_changed.connect(_on_filesystem_changed)

func _disconnect_filesystem_watch() -> void:
	var efs := EditorInterface.get_resource_filesystem()
	if efs == null:
		return
	if efs.filesystem_changed.is_connected(_on_filesystem_changed):
		efs.filesystem_changed.disconnect(_on_filesystem_changed)

# Filesystem-changed handler. Invalidates the inspector plugin's
# static name cache so the next inspector render re-scans. Cheap —
# just flips a bool; the actual rescan is lazy, happening only when
# an inspector with a sound_name property is next rendered.
#
# Note this fires several times during a single import (raw file,
# then .import sidecar, etc). That's fine here: clear_cache() is
# idempotent and near-free, and the expensive part (the actual
# project scan) is deferred to the next _parse_property call, which
# only happens once regardless of how many times the cache was
# cleared in between.
func _on_filesystem_changed() -> void:
	if _sound_name_inspector == null:
		return
	# The inspector plugin exposes a static clear_cache(). Call it
	# through the script so we don't need a typed reference to the
	# inner class.
	var inspector_script := load(INSPECTOR_PLUGIN_PATH)
	if inspector_script != null and inspector_script.has_method("clear_cache"):
		inspector_script.clear_cache()

func _add_autoload() -> void:
	add_autoload_singleton(AUTOLOAD_NAME, AUTOLOAD_PATH)
	# v0.43.0: DialogueDirector autoload. Registered AFTER Gool so
	# the director's _ready() can safely reach the gool autoload.
	add_autoload_singleton(DIALOGUE_DIRECTOR_AUTOLOAD_NAME,
			DIALOGUE_DIRECTOR_AUTOLOAD_PATH)
	# v0.46.0: MultiplayerBridge autoload. Registered AFTER Gool
	# (depends on the autoload) and AFTER DialogueDirector (peer
	# events from the bridge can route to dialogue bark cleanup).
	add_autoload_singleton(MULTIPLAYER_BRIDGE_AUTOLOAD_NAME,
			MULTIPLAYER_BRIDGE_AUTOLOAD_PATH)

func _remove_autoload() -> void:
	# Remove in reverse order — Bridge first (depends on Gool +
	# DialogueDirector), then DialogueDirector (depends on Gool),
	# then Gool itself.
	remove_autoload_singleton(MULTIPLAYER_BRIDGE_AUTOLOAD_NAME)
	remove_autoload_singleton(DIALOGUE_DIRECTOR_AUTOLOAD_NAME)
	remove_autoload_singleton(AUTOLOAD_NAME)

# v0.75.2: first-enable restart prompt. See _FIRST_ENABLE_KEY's comment
# at the top of this file for the underlying parser-cache problem this
# solves. Idempotent: after the first call, the project setting is
# flipped and subsequent calls return immediately.
func _maybe_show_first_enable_restart_prompt() -> void:
	# Already prompted in this project — nothing to do.
	# (Stays true across enable/disable cycles, so the dialog is
	# strictly first-enable-per-project, not first-enable-per-session.)
	if ProjectSettings.has_setting(_FIRST_ENABLE_KEY) \
			and bool(ProjectSettings.get_setting(_FIRST_ENABLE_KEY)):
		return

	# Mark BEFORE showing the dialog. If the user accepts the restart,
	# the editor will save project.godot (the restart_editor(true) call
	# below does save-on-exit), so the setting persists. If the user
	# dismisses, we still want the dialog to not reappear — so flip
	# the setting first and call ProjectSettings.save() to persist it
	# regardless of the user's choice.
	ProjectSettings.set_setting(_FIRST_ENABLE_KEY, true)
	# `set_initial_value(false)` ensures the setting is treated as
	# "non-default" by Godot's project settings editor — it'll show
	# up as a customized value rather than being filtered out as a
	# default. Mirrors how the getting-started banner's dismiss flag
	# is stored.
	ProjectSettings.set_initial_value(_FIRST_ENABLE_KEY, false)
	var save_result := ProjectSettings.save()
	if save_result != OK:
		# Saving project.godot can fail on read-only filesystems or
		# when the file is checked out exclusive in some VCS layouts.
		# Not fatal — we'll just show the dialog again next enable.
		# Surface a warning so users on those setups know what's up.
		push_warning(
			"[gool] could not persist first-enable flag "
			+ "(ProjectSettings.save returned %d). The first-time "
			+ "restart prompt may reappear next enable." % save_result)

	var dialog := ConfirmationDialog.new()
	dialog.title = "gool: first-time setup"
	# v0.80.5: Explicitly non-exclusive. Default ConfirmationDialog
	# behavior in Godot 4 is exclusive=true, which fails with the
	# "another exclusive child" error when this dialog appears while
	# Project Settings → Plugins is also open (the most common path
	# users take to enable the plugin). Non-exclusive lets both
	# windows coexist; the dialog is informational with a primary
	# action, not a hard block.
	dialog.exclusive = false
	# v0.80.5: Body rewritten. Pre-v0.80.4 this dialog warned about
	# a parse-error cascade that could "crash the editor on the next
	# project open" — that cascade originated in verify_install.gd's
	# bare-identifier reference (v0.80.4 fix). With the cascade
	# closed, the old copy was misleading: it described a problem
	# that no longer existed. Replaced with honest copy that frames
	# the restart as recommended-for-cleanliness, not required-to-
	# avoid-disaster.
	dialog.dialog_text = (
		"gool installed.\n\n"
		+ "The three autoloads — Gool, DialogueDirector, "
		+ "MultiplayerBridge — are registered, and the mixer + "
		+ "sound-bank docks are ready to use.\n\n"
		+ "A quick editor restart is recommended so any scripts "
		+ "you have open in the script editor pick up the new "
		+ "autoload definitions for autocomplete. It's not "
		+ "required and skipping it won't cause errors.\n\n"
		+ "Restart Godot now?")
	# Button labels mirror "I get it, do it" vs "I'll handle it myself."
	dialog.ok_button_text       = "Restart Editor Now"
	dialog.cancel_button_text   = "I'll Restart Manually"
	# Best-effort dialog ownership: parent to the editor base control
	# so it floats correctly and is destroyed when the editor closes.
	# If EditorInterface isn't ready yet (rare; shouldn't happen
	# inside _enter_tree but defensive coding), fall back to the
	# plugin's own scene tree.
	var editor_base := EditorInterface.get_base_control()
	if editor_base != null:
		editor_base.add_child(dialog)
	else:
		add_child(dialog)
	# Wire signals. `confirmed` fires on OK; `canceled` on Cancel or
	# escape. Both clean up the dialog node.
	dialog.confirmed.connect(_on_first_enable_restart_confirmed.bind(dialog))
	dialog.canceled.connect(_on_first_enable_restart_dismissed.bind(dialog))
	# Defer popup until the editor finishes its current frame —
	# popping up inside _enter_tree itself can interleave badly with
	# Godot's plugin-init UI updates.
	dialog.popup_centered.call_deferred(Vector2(560, 240))

func _on_first_enable_restart_confirmed(dialog: ConfirmationDialog) -> void:
	dialog.queue_free()
	# `true` = save before restart, so the user's project settings
	# (including our just-flipped _FIRST_ENABLE_KEY) get persisted
	# along with anything else that's dirty. Available since Godot
	# 4.0; the bool parameter is the save-on-exit flag.
	EditorInterface.restart_editor(true)

func _on_first_enable_restart_dismissed(dialog: ConfirmationDialog) -> void:
	# User chose to handle it themselves. The first-enable flag is
	# already persisted so the dialog won't reappear. v0.80.4 closed
	# the parse-error cascade that used to make skipping the restart
	# painful (verify_install.gd:64); skipping it now is a clean
	# no-op — autocomplete in open scripts may be stale until the
	# user restarts on their own schedule, but nothing breaks.
	dialog.queue_free()

func _register_prefabs() -> void:
	for entry in PREFABS:
		var class_id   : String = entry[0]
		var base_class : String = entry[1]
		var script_path: String = PREFAB_DIR + entry[2]
		var icon_name  : String = entry[3]
		var script := load(script_path)
		if script == null:
			push_warning("[gool] missing prefab script: %s" % script_path)
			continue
		# Icon loading is best-effort: a missing or invalid SVG falls
		# back to the base class's default icon rather than failing
		# registration. This keeps the Add Node menu working even if
		# someone deletes an icon file by mistake.
		var icon: Texture2D = null
		if icon_name != "":
			var icon_path := PREFAB_DIR + icon_name
			if ResourceLoader.exists(icon_path):
				icon = load(icon_path)
		add_custom_type(class_id, base_class, script, icon)

func _unregister_prefabs() -> void:
	for entry in PREFABS:
		remove_custom_type(entry[0])

func _write_default_config_if_missing() -> void:
	if FileAccess.file_exists(CONFIG_PATH):
		return
	DirAccess.make_dir_recursive_absolute("res://gool")
	var f := FileAccess.open(CONFIG_PATH, FileAccess.WRITE)
	if f == null:
		push_warning("[gool] could not write default config at %s" % CONFIG_PATH)
		return
	f.store_string(JSON.stringify(DEFAULT_CONFIG, "  "))
	f.close()

# v0.23.0: auto-scaffolding ----------------------------------------------------
#
# Eliminates the "create folders, create bank.tres, configure folder_path"
# manual setup that used to be the first 5 minutes of every new gool
# project. Run once on plugin enable; idempotent so subsequent enables
# (or reinstalls via gool-install.cmd) are no-ops.
#
# Anything the user has already set up — even partially — is preserved.
# We only create directories that don't exist and only write bank.tres
# if the file doesn't exist. A user who's organized their audio
# differently (e.g. assets/audio/ instead of sounds/) just gets one
# unused res://sounds/ directory tree they can delete if they want.
func _scaffold_sounds_tree_if_missing() -> void:
	var created_anything := false

	# Step 1: the sounds/ root and its category subfolders. Each one
	# is checked independently so a partial existing tree (e.g. user
	# had sounds/sfx/ but not sounds/music/) gets completed.
	if not DirAccess.dir_exists_absolute(SOUNDS_ROOT):
		DirAccess.make_dir_recursive_absolute(SOUNDS_ROOT)
		created_anything = true
	for sub in SOUND_SUBFOLDERS:
		var path := "%s/%s" % [SOUNDS_ROOT, sub]
		if not DirAccess.dir_exists_absolute(path):
			DirAccess.make_dir_recursive_absolute(path)
			created_anything = true

	# Step 2: bank.tres. Only create if absent. The bank's folder_path
	# is set to SOUNDS_ROOT so any audio file dropped under any of the
	# subfolders is picked up automatically by the recursive scan.
	# Category-from-subfolder is enabled so a file under sounds/music/
	# routes to the Music bus, sounds/sfx/ to LocalSfx, etc.
	if not FileAccess.file_exists(BANK_PATH):
		var script := load(FOLDER_SOUND_BANK_SCRIPT)
		if script == null:
			push_warning(
				"[gool] could not load GoolFolderSoundBank script "
				+ "at %s; skipping bank.tres scaffolding. "
				% FOLDER_SOUND_BANK_SCRIPT
				+ "You can create the bank manually later via "
				+ "FileSystem dock → New Resource → GoolFolderSoundBank."
			)
			return
		var bank: Resource = script.new()
		bank.folder_path = SOUNDS_ROOT
		bank.recursive = true
		bank.category_from_subfolder = true
		bank.apply_category_defaults = true
		var err: int = ResourceSaver.save(bank, BANK_PATH)
		if err != OK:
			push_warning(
				"[gool] failed to save default bank to %s "
				% BANK_PATH
				+ "(ResourceSaver error %d). You can create the bank "
				% err
				+ "manually later via FileSystem dock → New Resource."
			)
			return
		created_anything = true

	if created_anything:
		print(
			"[gool] scaffolded sounds/ tree + bank.tres. Drop audio files "
			+ "(.wav / .ogg / .mp3 / .flac) into res://sounds/{sfx,music,"
			+ "voice,ambience,ui}/ — the bank picks them up automatically "
			+ "and they appear in the AudioEmitter3D sound_name dropdown."
		)

# v0.23.0: Project → Tools → Gool menu -----------------------------------------
#
# Adds editor commands for the common scene-setup tasks that used to
# require manually adding three nodes and configuring four inspector
# fields. The submenu lives under Project → Tools so users discover
# it the same way they discover other editor tooling.
#
# add_tool_submenu_item takes a PopupMenu instance we own; the
# Tools menu adopts it but doesn't take ownership. We hold the
# reference in _tools_menu so we can clean up on _exit_tree.
# Note: Godot uses `remove_tool_menu_item(name)` for cleanup of
# BOTH `add_tool_menu_item` and `add_tool_submenu_item` — there is
# no separate `remove_tool_submenu_item`. v0.23.9 fix.
# PopupMenu emits id_pressed when an item is clicked, which we
# dispatch to the appropriate handler.
var _tools_menu: PopupMenu = null

# Menu item IDs. Using an enum keeps the dispatcher table readable
# and lets us reorder/insert without breaking handlers.
enum ToolsMenuItem {
	ADD_3D_SCAFFOLDING = 0,
	NEW_FOLDER_BANK    = 1,
	OPEN_QUICKSTART    = 2,
	ADD_DEBUG_OVERLAY  = 3,   # v0.23.1
	RUN_PREFAB_SMOKE_TEST = 4,  # v0.45.0
	RUN_FPS_SCENE_SMOKE_TEST = 5,  # v0.50.0
	OPEN_HELP_PANEL = 6,  # v0.79.3
}

func _register_tools_menu() -> void:
	_tools_menu = PopupMenu.new()
	_tools_menu.add_item("Add gool 3D audio scaffolding to current scene",
		ToolsMenuItem.ADD_3D_SCAFFOLDING)
	_tools_menu.add_item("Add debug overlay to current scene",
		ToolsMenuItem.ADD_DEBUG_OVERLAY)
	_tools_menu.add_item("Create new GoolFolderSoundBank...",
		ToolsMenuItem.NEW_FOLDER_BANK)
	_tools_menu.add_separator()
	_tools_menu.add_item("Open quickstart_3d.tscn (verify gool works)",
		ToolsMenuItem.OPEN_QUICKSTART)
	# v0.45.0: static-analysis smoke test that catches the
	# missing-autoload-wrapper bug class. Runs in seconds, no F5
	# needed. See addons/gool/tools/prefab_smoke_test.gd.
	_tools_menu.add_separator()
	_tools_menu.add_item("Run prefab smoke test (find missing autoload wrappers)",
		ToolsMenuItem.RUN_PREFAB_SMOKE_TEST)
	# v0.50.0: scene-level FPS smoke test. Scans every .tscn for
	# gool prefab usage and cross-references with config.json to
	# detect "added a feature but didn't wire its dependency"
	# bugs (ReverbZone with no Reverb in bus chain, VoiceChatPlayer
	# with no Voice bus, etc.). See addons/gool/tools/fps_scene_smoke_test.gd.
	_tools_menu.add_item("Run FPS scene smoke test (find missing config dependencies)",
		ToolsMenuItem.RUN_FPS_SCENE_SMOKE_TEST)
	# v0.79.3: Help panel — second entry point for the help content
	# (alongside the "?" button in the Mixer Dock). Users who don't
	# open the dock can still find help from the Project menu.
	_tools_menu.add_separator()
	_tools_menu.add_item("Help (keyboard shortcuts, API, tools)",
		ToolsMenuItem.OPEN_HELP_PANEL)
	_tools_menu.id_pressed.connect(_on_tools_menu_pressed)
	add_tool_submenu_item(TOOLS_MENU_NAME, _tools_menu)

func _unregister_tools_menu() -> void:
	if _tools_menu == null:
		return
	# v0.23.9: was `remove_tool_submenu_item(...)` — that method
	# doesn't exist on EditorPlugin. The correct API for cleaning
	# up a submenu added via add_tool_submenu_item is the same as
	# for a single item: remove_tool_menu_item(name). Godot's
	# docs are explicit: "This submenu should be cleaned up using
	# remove_tool_menu_item(name)."
	remove_tool_menu_item(TOOLS_MENU_NAME)
	# PopupMenu is a Node; if we added it to the scene tree somewhere
	# we'd queue_free, but add_tool_submenu_item doesn't reparent it,
	# so we just drop our reference and let the GC collect.
	_tools_menu = null

func _on_tools_menu_pressed(id: int) -> void:
	match id:
		ToolsMenuItem.ADD_3D_SCAFFOLDING:
			_add_3d_scaffolding_to_current_scene()
		ToolsMenuItem.NEW_FOLDER_BANK:
			_create_new_folder_bank()
		ToolsMenuItem.OPEN_QUICKSTART:
			EditorInterface.open_scene_from_path(
				"res://addons/gool/templates/quickstart_3d.tscn")
		ToolsMenuItem.ADD_DEBUG_OVERLAY:
			_add_debug_overlay_to_current_scene()
		ToolsMenuItem.RUN_PREFAB_SMOKE_TEST:
			_run_prefab_smoke_test()
		ToolsMenuItem.RUN_FPS_SCENE_SMOKE_TEST:
			_run_fps_scene_smoke_test()
		ToolsMenuItem.OPEN_HELP_PANEL:
			_open_help_panel()


# v0.79.3: Open the help panel from the Tools menu. Second entry
# point alongside the "?" button in the Mixer Dock toolbar — users
# who don't open the dock can still find help. Mirrors the
# idempotency check from mixer_dock.gd::_on_help_button_pressed
# so clicking Help twice doesn't spawn duplicate panels.
func _open_help_panel() -> void:
	var base := EditorInterface.get_base_control()
	if base == null:
		return
	for child in base.get_children():
		if child is Window and child.title == "gool — Help":
			child.grab_focus()
			child.move_to_foreground()
			return
	var help_script := load("res://addons/gool/editor/help_panel.gd")
	if help_script == null:
		push_warning("[gool] help_panel.gd not found — was the "
				+ "addon fully extracted?")
		return
	var panel: Window = help_script.new()
	base.add_child(panel)
	panel.popup_centered()

# v0.45.0: run the static-analysis smoke test from the Tools menu.
# Tool prints findings to the Output panel. Pure analysis — no F5
# session needed.
func _run_prefab_smoke_test() -> void:
	var tool_script := load("res://addons/gool/tools/prefab_smoke_test.gd")
	if tool_script == null:
		push_error("[gool] prefab_smoke_test.gd not found")
		return
	var tool = tool_script.new()
	var ok: bool = tool.run()
	if ok:
		_show_info_dialog(
			"gool: prefab smoke test PASSED",
			"All prefab callsites have matching autoload wrappers. "
			+ "See the Output panel for details."
		)
	else:
		_show_info_dialog(
			"gool: prefab smoke test FAILED",
			"Missing autoload wrapper(s) detected. See the Output "
			+ "panel for the list of missing methods and where each "
			+ "is called from. Same bug shape as v0.44.1 / v0.44.2."
		)

# v0.50.0: run the scene-level FPS smoke test from the Tools menu.
# Walks all .tscn under res:// (excluding addons/), counts gool
# prefab usage per scene, and cross-references with config.json
# to find missing dependencies (ReverbZone with no Reverb in any
# bus chain, VoiceChatPlayer with no Voice bus, etc.).
func _run_fps_scene_smoke_test() -> void:
	var tool_script := load("res://addons/gool/tools/fps_scene_smoke_test.gd")
	if tool_script == null:
		push_error("[gool] fps_scene_smoke_test.gd not found")
		return
	var tool = tool_script.new()
	var ok: bool = tool.run()
	if ok:
		_show_info_dialog(
			"gool: FPS scene smoke test PASSED",
			"No errors detected. Some scenes may have warnings or "
			+ "info notes — see the Output panel for the full report."
		)
	else:
		_show_info_dialog(
			"gool: FPS scene smoke test FAILED",
			"One or more error-severity findings detected. See the "
			+ "Output panel for the list. Common causes: ReverbZone "
			+ "without a Reverb effect in any bus chain, "
			+ "VoiceChatPlayer without a Voice bus in config.json."
		)

# v0.23.0: scene scaffolding command.
#
# Inserts the three gool nodes a 3D audio scene needs:
#   - GoolListener3D   (the "ears")
#   - GoolSoundBankLoader (registers all sounds in bank.tres at runtime)
#   - AudioEmitter3D   (placeholder; user picks a sound from the dropdown)
#
# All three become direct children of the scene root, with their
# owner set to the root so they survive scene-save. The user can
# reparent them later if needed (Listener under Player, etc).
func _add_3d_scaffolding_to_current_scene() -> void:
	var root: Node = EditorInterface.get_edited_scene_root()
	if root == null:
		_show_info_dialog(
			"No scene open",
			"Open a scene first (Scene → New Scene, or open an "
			+ "existing one), then try Project → Tools → Gool again."
		)
		return
	if not (root is Node3D):
		_show_info_dialog(
			"3D scene required",
			"The current scene's root is %s, not a Node3D-derived "
			% root.get_class()
			+ "node. gool 3D audio scaffolding adds GoolListener3D "
			+ "and AudioEmitter3D, which are 3D nodes. Open a 3D "
			+ "scene (Scene → New Scene → 3D Scene) and try again."
		)
		return

	# Three nodes to add. Each entry: (script_path, node_name).
	# We use the prefab script paths directly so the class_name
	# registration order at plugin-enable time doesn't matter.
	var to_add := [
		[LISTENER_3D_SCRIPT,  "GoolListener3D"],
		[BANK_LOADER_SCRIPT,  "GoolSoundBankLoader"],
		[EMITTER_3D_SCRIPT,   "AudioEmitter3D"],
	]
	var added_names: PackedStringArray = PackedStringArray()
	for entry in to_add:
		var script: Script = load(entry[0])
		if script == null:
			push_warning("[gool] missing prefab script: %s" % entry[0])
			continue
		var node: Node
		# Listener and emitter need a Node3D base; loader is a plain Node.
		if entry[1] == "GoolSoundBankLoader":
			node = Node.new()
		else:
			node = Node3D.new()
		node.set_script(script)
		node.name = entry[1]
		root.add_child(node)
		node.set_owner(root)
		added_names.append(entry[1])

	# Pre-assign bank.tres on the loader so the user doesn't have
	# to do the drag-and-drop step manually. The setter is exposed
	# as a property on the loader script.
	var loader: Node = root.get_node_or_null("GoolSoundBankLoader")
	if loader != null and FileAccess.file_exists(BANK_PATH):
		var bank_resource: Resource = load(BANK_PATH)
		if bank_resource != null:
			loader.bank = bank_resource

	# Notify the user. The scene is now dirty (add_child marks it),
	# but we surface a dialog so they know what happened and what
	# to do next.
	_show_info_dialog(
		"Added gool 3D scaffolding",
		"Added the following to '%s':\n\n" % root.name
		+ "\n".join(added_names.duplicate())
		+ "\n\nNext steps:\n"
		+ "  1. Save the scene (Ctrl+S).\n"
		+ "  2. Drop audio files into res://sounds/sfx/ (or any\n"
		+ "     other sounds/ subfolder).\n"
		+ "  3. Select the AudioEmitter3D, click its 'Sound Name'\n"
		+ "     field — your file appears in the dropdown.\n"
		+ "  4. Check Autoplay → On.\n"
		+ "  5. F5 to verify."
	)

# v0.23.0: create-new-bank command.
#
# Opens a save-file dialog and writes a configured GoolFolderSoundBank
# at the chosen path. Useful when a project wants a second bank
# (e.g. per-level audio organization) beyond the default
# res://sounds/bank.tres that auto-scaffolding creates.
func _create_new_folder_bank() -> void:
	var dialog := EditorFileDialog.new()
	dialog.file_mode = EditorFileDialog.FILE_MODE_SAVE_FILE
	dialog.add_filter("*.tres", "GoolFolderSoundBank resource")
	dialog.current_path = "res://sounds/my_bank.tres"
	dialog.title = "Save new GoolFolderSoundBank as..."
	dialog.file_selected.connect(_on_new_bank_path_selected)
	EditorInterface.get_base_control().add_child(dialog)
	dialog.popup_centered_ratio(0.6)

func _on_new_bank_path_selected(path: String) -> void:
	var script := load(FOLDER_SOUND_BANK_SCRIPT)
	if script == null:
		push_warning("[gool] could not load GoolFolderSoundBank script")
		return
	var bank: Resource = script.new()
	# Default folder_path to the directory the bank itself lives in,
	# which is usually the user's intent (scan this folder + subfolders).
	bank.folder_path = path.get_base_dir()
	bank.recursive = true
	bank.category_from_subfolder = true
	bank.apply_category_defaults = true
	var err: int = ResourceSaver.save(bank, path)
	if err != OK:
		_show_info_dialog("Save failed",
			"Couldn't save bank to %s\n\nResourceSaver error: %d"
			% [path, err])
		return
	print("[gool] created new GoolFolderSoundBank at %s "
		% path
		+ "(folder_path=%s)" % bank.folder_path)
	# Ping the FileSystem dock so the new file appears immediately.
	EditorInterface.get_resource_filesystem().scan()

# Helper for popping up a small info/notice dialog. Editor-side only;
# does NOT block execution (Godot's AcceptDialog is modeless by default).
func _show_info_dialog(title: String, body: String) -> void:
	var dlg := AcceptDialog.new()
	dlg.title = title
	dlg.dialog_text = body
	dlg.get_label().autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	dlg.min_size = Vector2i(500, 200)
	EditorInterface.get_base_control().add_child(dlg)
	dlg.popup_centered()
	# Self-clean: the dialog stays in the editor tree until dismissed,
	# then queue_free's itself.
	dlg.confirmed.connect(dlg.queue_free)
	dlg.canceled.connect(dlg.queue_free)

# v0.23.1: scene-level debug-overlay command.
#
# Adds a GoolDebugOverlay node under the current scene root. Unlike
# the 3D-scaffolding command, this works in 2D scenes, UI-only
# scenes, even an empty Node-rooted scene — the overlay is a
# CanvasLayer and renders regardless of camera setup. Idempotent:
# if a GoolDebugOverlay already exists under the root, nothing is
# added (one is enough per scene).
func _add_debug_overlay_to_current_scene() -> void:
	var root: Node = EditorInterface.get_edited_scene_root()
	if root == null:
		_show_info_dialog(
			"No scene open",
			"Open a scene first (Scene → New Scene), then try "
			+ "Project → Tools → Gool → Add debug overlay again."
		)
		return
	# Check for existing overlay; one per scene is the right model.
	for child in root.get_children():
		if child.get_script() != null and \
				child.get_script().resource_path == \
				"res://addons/gool/prefabs/gool_debug_overlay.gd":
			_show_info_dialog(
				"Already present",
				"This scene already has a GoolDebugOverlay node "
				+ "('%s'). Select it to configure visibility, "
				% child.name
				+ "toggle key, or position."
			)
			return
	var script: Script = load(
		"res://addons/gool/prefabs/gool_debug_overlay.gd")
	if script == null:
		push_warning("[gool] missing gool_debug_overlay.gd")
		return
	var overlay := CanvasLayer.new()
	overlay.set_script(script)
	overlay.name = "GoolDebugOverlay"
	root.add_child(overlay)
	overlay.set_owner(root)
	_show_info_dialog(
		"Added GoolDebugOverlay",
		"A GoolDebugOverlay node has been added under '%s'.\n\n"
		% root.name
		+ "Save the scene (Ctrl+S), then F5. The overlay shows in "
		+ "the top-left corner with real-time stats.\n\n"
		+ "Toggle visibility in-game by pressing F3 (default), or "
		+ "configure a custom toggle_action / toggle_key in the "
		+ "Inspector. For shipping builds, set "
		+ "visible_at_startup=false."
	)


# v0.79.2: Register the update-check opt-out setting so it appears
# in Project Settings → audio → gool → check_for_updates. Default is
# `true` (check enabled); users in restricted environments or those
# who simply don't want the editor to make outbound HTTPS calls can
# untick this. The setting is read by addons/gool/editor/
# update_checker.gd before any network activity.
func _register_update_check_setting() -> void:
	var setting_path := "audio/gool/check_for_updates"
	if not ProjectSettings.has_setting(setting_path):
		ProjectSettings.set_setting(setting_path, true)
	ProjectSettings.set_initial_value(setting_path, true)
	ProjectSettings.add_property_info({
		"name": setting_path,
		"type": TYPE_BOOL,
		"hint": PROPERTY_HINT_NONE,
		"hint_string": ("Check GitHub for new gool releases when the "
				+ "editor opens (once per 24h, cached). Disable to "
				+ "prevent outbound HTTPS calls from the editor."),
	})
