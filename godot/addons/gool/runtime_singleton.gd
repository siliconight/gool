# addons/gool/runtime_singleton.gd
#
# Autoload wrapper around the C++ GoolAudioRuntime. Loaded at
# /root/Gool by the editor plugin. Every prefab calls into this
# singleton.
#
# The wrapper exists so:
#   - Init/shutdown is centralized (one runtime per project)
#   - Default config from res://gool/config.json is auto-applied
#   - Update is driven from _process automatically
#
# If you need to call binding methods directly, _runtime exposes
# the underlying GoolAudioRuntime instance.

extends Node

const CONFIG_PATH := "res://gool/config.json"

var _runtime: Node = null
var _ready_emitted: bool = false

signal ready_to_play

func _ready() -> void:
    _runtime = ClassDB.instantiate("GoolAudioRuntime")
    if _runtime == null:
        push_error("[gool] GoolAudioRuntime class not registered. "
                   + "Build and install the GDExtension first.")
        return
    add_child(_runtime)

    var cfg := _load_config()
    var sr  : int = cfg.get("sample_rate", 48000)
    var bs  : int = cfg.get("buffer_size", 512)
    if not _runtime.init(sr, bs):
        push_error("[gool] runtime init failed (no audio device?)")
        return
    _ready_emitted = true
    ready_to_play.emit()

func _process(delta: float) -> void:
    if _runtime != null and _runtime.is_initialized():
        _runtime.update(delta)

func _exit_tree() -> void:
    if _runtime != null and _runtime.is_initialized():
        _runtime.shutdown()

# Forward common methods so prefabs can call get_node("/root/Gool")
# directly without reaching into _runtime.

func is_initialized() -> bool:
    return _runtime != null and _runtime.is_initialized()

func register_pcm_sound(name: String, samples: PackedFloat32Array,
                         sr: int = 48000, ch: int = 1) -> int:
    return _runtime.register_pcm_sound(name, samples, sr, ch)

func register_sound_definition(name: String, spatialized: bool = true,
                                 looping: bool = false,
                                 min_distance: float = 1.0,
                                 max_distance: float = 50.0,
                                 loop_crossfade_ms: float = 0.0) -> void:
    _runtime.register_sound_definition(name, spatialized, looping,
                                         min_distance, max_distance,
                                         loop_crossfade_ms)

func play_sound_at_location(name: String, position: Vector3) -> void:
    _runtime.play_sound_at_location(name, position)

func create_emitter(name: String, position: Vector3,
                     looping: bool = false,
                     fade_in_ms: float = 0.0) -> int:
    return _runtime.create_emitter(name, position, looping, fade_in_ms)

func destroy_emitter(handle: int, fade_out_ms: float = 0.0) -> void:
    _runtime.destroy_emitter(handle, fade_out_ms)

func set_emitter_transform(handle: int, position: Vector3,
                              forward: Vector3, velocity: Vector3) -> void:
    _runtime.set_emitter_transform(handle, position, forward, velocity)

func set_emitter_playback_speed(handle: int, speed: float,
                                   smoothing_ms: float = 50.0) -> void:
    _runtime.set_emitter_playback_speed(handle, speed, smoothing_ms)

func set_listener_transform(position: Vector3, forward: Vector3,
                              velocity: Vector3 = Vector3.ZERO) -> void:
    _runtime.set_listener_transform(position, forward, velocity)

func register_voice_source(player_id: int) -> bool:
    return _runtime.register_voice_source(player_id)

func submit_voice_packet(player_id: int, bytes: PackedByteArray,
                            sequence_number: int,
                            send_timestamp_ms: int,
                            arrival_timestamp_ms: int = -1) -> bool:
    return _runtime.submit_voice_packet(
        player_id, bytes, sequence_number,
        send_timestamp_ms, arrival_timestamp_ms)

func get_voice_jitter_ms(player_id: int) -> float:
    return _runtime.get_voice_jitter_ms(player_id)

func get_voice_packet_loss_ratio(player_id: int) -> float:
    return _runtime.get_voice_packet_loss_ratio(player_id)

# ---- Replication / multiplayer ----

func on_tick_advanced(simulation_tick: int, server_time_ms: int) -> void:
    _runtime.on_tick_advanced(simulation_tick, server_time_ms)

func submit_event_local(sound_name: String, position: Vector3,
                          prediction_id: int = 0,
                          priority: int = 128,
                          timestamp_ms: int = 0) -> void:
    _runtime.submit_event_local(sound_name, position,
                                  prediction_id, priority, timestamp_ms)

func submit_replicated_event(sound_name: String, position: Vector3,
                               simulation_tick: int = 0,
                               server_time_ms: int = 0,
                               priority: int = 128) -> void:
    _runtime.submit_replicated_event(sound_name, position,
                                       simulation_tick, server_time_ms,
                                       priority)

func cancel_predicted_event(prediction_id: int,
                               fade_out_ms: float = 50.0) -> void:
    _runtime.cancel_predicted_event(prediction_id, fade_out_ms)

func update_replicated_transform(handle: int, position: Vector3,
                                    forward: Vector3, velocity: Vector3,
                                    simulation_tick: int) -> void:
    _runtime.update_replicated_transform(handle, position, forward,
                                            velocity, simulation_tick)

func make_prediction_id() -> int:
    return _runtime.make_prediction_id()

func _load_config() -> Dictionary:
    if not FileAccess.file_exists(CONFIG_PATH):
        return {}
    var f := FileAccess.open(CONFIG_PATH, FileAccess.READ)
    if f == null:
        return {}
    var text := f.get_as_text()
    f.close()
    var parsed = JSON.parse_string(text)
    if parsed is Dictionary:
        return parsed
    return {}
