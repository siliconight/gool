# addons/gool/plugin.gd
#
# Editor plugin. On enable, does the work that would otherwise be
# manual setup for a new gool project:
#
#   1. Adds /root/Gool autoload pointing at the runtime singleton
#      script. This is what every prefab calls into.
#   2. Registers the custom prefab Nodes (AudioEmitter3D,
#      VoiceChatPlayer, MusicStateController, ReverbZone,
#      FootstepSurfacePlayer) so they appear in the Add Node menu.
#   3. Writes a default config file at res://gool/config.json with
#      reasonable bus/attenuation/compression defaults if one
#      doesn't already exist. The runtime reads this on init.
#
# On disable, all of the above are reversed cleanly.

@tool
extends EditorPlugin

const AUTOLOAD_NAME := "Gool"
const AUTOLOAD_PATH := "res://addons/gool/runtime_singleton.gd"

const PREFAB_DIR := "res://addons/gool/prefabs/"

# (class_name, base_node, script_path, icon_path)
const PREFABS := [
    ["AudioEmitter3D",            "Node3D", "audio_emitter_3d.gd",            ""],
    ["VoiceChatPlayer",           "Node3D", "voice_chat_player.gd",           ""],
    ["MusicStateController",      "Node",   "music_state_controller.gd",      ""],
    ["ReverbZone",                "Area3D", "reverb_zone.gd",                 ""],
    ["FootstepSurfacePlayer",     "Node3D", "footstep_surface_player.gd",     ""],
    ["NetworkedAudioEvent",       "Node",   "networked_audio_event.gd",       ""],
    ["NetworkedAudioEmitter3D",   "Node3D", "networked_audio_emitter_3d.gd",  ""],
]

const CONFIG_PATH := "res://gool/config.json"
const DEFAULT_CONFIG := {
    "sample_rate": 48000,
    "buffer_size": 512,
    "buses": {
        "master":   { "gain_db":  0.0 },
        "music":    { "gain_db": -3.0 },
        "sfx":      { "gain_db":  0.0 },
        "voice":    { "gain_db":  0.0 },
        "ambient":  { "gain_db": -6.0 },
    },
    "default_attenuation": {
        "min_distance": 1.0,
        "max_distance": 50.0,
    },
    "default_compression": {
        "threshold_db": -18.0,
        "ratio":         3.0,
        "attack_ms":     5.0,
        "release_ms":   80.0,
        "makeup_db":     2.0,
    },
}

func _enter_tree() -> void:
    _add_autoload()
    _register_prefabs()
    _write_default_config_if_missing()
    print("[gool] plugin enabled — autoload, prefabs, default config installed.")

func _exit_tree() -> void:
    _unregister_prefabs()
    _remove_autoload()
    print("[gool] plugin disabled.")

func _add_autoload() -> void:
    add_autoload_singleton(AUTOLOAD_NAME, AUTOLOAD_PATH)

func _remove_autoload() -> void:
    remove_autoload_singleton(AUTOLOAD_NAME)

func _register_prefabs() -> void:
    for entry in PREFABS:
        var class_id   : String = entry[0]
        var base_class : String = entry[1]
        var script_path: String = PREFAB_DIR + entry[2]
        var script := load(script_path)
        if script == null:
            push_warning("[gool] missing prefab script: %s" % script_path)
            continue
        add_custom_type(class_id, base_class, script, null)

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
