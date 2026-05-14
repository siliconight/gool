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
        { "name": "Master", "gain_db": 0.0 },

        # Music bus: ducks under the local-player SFX so the player's
        # own gun wins the mix.
        { "name": "Music",  "parent": "Master", "gain_db": -3.0,
          "effects": [
            { "kind": "compressor",
              "threshold_db": -30.0, "ratio": 8.0,
              "attack_ms": 5.0,  "release_ms": 250.0,
              "makeup_db": 0.0,
              "knee_width_db": 4.0,
              "sidechain_bus": "LocalSfx" }
          ] },

        # Submix that holds both local + remote SFX. Per-tier
        # processing happens on its children, not here.
        { "name": "SfxAll", "parent": "Master" },

        # Local-player SFX — your gun, your footsteps, your reload.
        # No effects; flat path so the trigger signal for the
        # sidechain compressors reaches them clean.
        { "name": "LocalSfx", "parent": "SfxAll" },

        # Remote-player SFX — teammate guns, NPC barks, ambient
        # impacts. Ducks under LocalSfx so the local action wins
        # over teammate action.
        { "name": "RemoteSfx", "parent": "SfxAll",
          "effects": [
            { "kind": "compressor",
              "threshold_db": -30.0, "ratio": 8.0,
              "attack_ms": 5.0,  "release_ms": 250.0,
              "sidechain_bus": "LocalSfx" }
          ] },

        # Voice chat — separate bus, not ducked (intelligibility
        # priority). If you want voice to also win over music,
        # add it as a sidechain bus on Music's compressor.
        { "name": "Voice",   "parent": "Master", "gain_db": 0.0 },

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
        "dialogue": "Voice"
    }
}

const INSPECTOR_PLUGIN_PATH := "res://addons/gool/editor/sound_name_inspector.gd"

# Held instance of the inspector plugin. Stored so _exit_tree can
# unregister the same instance we registered (Godot's
# remove_inspector_plugin requires the original reference, not just
# a script path).
var _sound_name_inspector: EditorInspectorPlugin = null

func _enter_tree() -> void:
    _add_autoload()
    _register_prefabs()
    _write_default_config_if_missing()
    _register_inspector_plugin()
    _connect_filesystem_watch()
    print("[gool] plugin enabled — autoload, prefabs, default config, inspector installed.")

func _exit_tree() -> void:
    _disconnect_filesystem_watch()
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

func _remove_autoload() -> void:
    remove_autoload_singleton(AUTOLOAD_NAME)

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
