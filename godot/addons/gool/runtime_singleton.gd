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

# Lazy-instantiated GoolMusicChannel for the play_music_state facade.
# Created on first call so projects that don't use music never pay
# any setup cost.
var _music_channel: Node = null
var _music_state: String = ""

# Counter feeding unique sound-bank names for play_voice clips
# extracted from AudioStreamWAV resources. Each call registers a
# fresh ephemeral sound; reuse is intentionally avoided so concurrent
# voice playback for the same player works without state coordination.
var _voice_counter: int = 0

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

# Returns the engine version as a Dictionary:
#   { "major": int, "minor": int, "patch": int,
#     "full":  String, "commit": String }
# Useful in debug overlays, crash reports, and bug-report forms.
# Available before init() since the version is compile-time.
func get_version() -> Dictionary:
    if _runtime == null:
        return {}
    return _runtime.get_version()

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

# =============================================================================
# Tiny API facade
# =============================================================================
#
# These four methods are the canonical entry points for fast prototyping.
# Each is a thin wrapper around lower-level APIs. Drop down to the raw
# bindings (submit_event_local, register_pcm_sound, submit_voice_packet,
# set_global_parameter) when you outgrow them.
#
#   Gool.play_3d("rifle_fire", global_position)
#   Gool.play_music_state("combat")
#   Gool.play_voice(player_id, audio_stream)
#   Gool.set_rtpc("health", hp)

# Play a one-shot 3D sound by authored name at `position`. Sound must be
# registered (via sound bank or register_pcm_sound) ahead of this call.
# `priority` is 0..255; higher survives culling under voice budgets.
# Returns true if the event was queued; false if the runtime is not
# initialized or the queue is full.
func play_3d(name: String, position: Vector3, priority: int = 128) -> bool:
    if _runtime == null:
        return false
    var rc: int = _runtime.submit_event_local(name, position, 0, priority, 0)
    return rc == 0  # AudioResult::Success

# Switch the music channel to `state_name` with an equal-power crossfade.
# Idempotent: passing the currently-playing state is a no-op so callers
# can poll-style invoke this every frame without churn. The first call
# lazily creates a GoolMusicChannel under this autoload.
func play_music_state(state_name: String, fade_ms: float = 500.0) -> bool:
    if _runtime == null:
        return false
    if _music_channel == null:
        _music_channel = ClassDB.instantiate("GoolMusicChannel")
        if _music_channel == null:
            push_error("[gool] GoolMusicChannel class not registered. "
                       + "Build and install the GDExtension first.")
            return false
        add_child(_music_channel)
        _music_channel.attach(_runtime)
    if state_name == _music_state and _music_channel.is_playing():
        return true  # already in this state
    _music_state = state_name
    _music_channel.play(state_name, fade_ms)
    return true

# Stop the music channel with a fade-out. Subsequent play_music_state
# calls work normally afterward.
func stop_music(fade_ms: float = 500.0) -> void:
    if _music_channel == null:
        return
    _music_state = ""
    _music_channel.stop(fade_ms)

# Play `audio_stream` as voice for `player_id`. The clip is decoded to
# PCM, registered as an ephemeral sound, and dispatched through the
# normal play path.
#
# Currently supports AudioStreamWAV with FORMAT_16_BITS only. For
# AudioStreamOggVorbis, decode upstream to AudioStreamWAV (or use the
# raw voice path: Gool.submit_voice_packet for Opus traffic from a
# real network layer). AudioStreamOggVorbis support is on the roadmap.
#
# Returns true if the clip was queued; false on input errors or
# missing runtime.
func play_voice(player_id: int, audio_stream: AudioStream) -> bool:
    if _runtime == null:
        return false
    if not (audio_stream is AudioStreamWAV):
        push_error("[gool] play_voice currently supports AudioStreamWAV only. "
                   + "AudioStreamOggVorbis support is on the roadmap. For "
                   + "Opus voice packets from a network layer, use "
                   + "Gool.submit_voice_packet directly.")
        return false
    var wav: AudioStreamWAV = audio_stream
    var samples := _wav_to_pcm_mono(wav)
    if samples.is_empty():
        return false
    _voice_counter += 1
    var clip_name := "_voice_%d_%d" % [player_id, _voice_counter]
    var sample_rate: int = int(wav.mix_rate)
    if sample_rate <= 0:
        sample_rate = 48000
    _runtime.register_pcm_sound(clip_name, samples, sample_rate, 1)
    _runtime.register_sound_definition(clip_name, false)
    _runtime.play_sound_at_location(clip_name, Vector3.ZERO)
    return true

# Set a real-time parameter ("RTPC" in middleware lingo) by name.
# Stored as a key-value pair in the runtime; authored sound definitions
# can reference these in future updates. For now: useful for game-state
# polling, debug overlays, and getting comfortable with the API shape.
# Returns true if the value was stored; false on budget exhaustion or
# missing runtime. See AudioConfig::maxGlobalParameters (default 256).
func set_rtpc(name: String, value: float) -> bool:
    if _runtime == null:
        return false
    return _runtime.set_global_parameter(name, value)

# Read the current value of an RTPC. Returns 0.0 if the parameter has
# never been set; use has_rtpc() to disambiguate "set to zero" from
# "never set."
func get_rtpc(name: String) -> float:
    if _runtime == null:
        return 0.0
    return _runtime.get_global_parameter(name)

func has_rtpc(name: String) -> bool:
    if _runtime == null:
        return false
    return _runtime.has_global_parameter(name)

func clear_rtpc(name: String) -> bool:
    if _runtime == null:
        return false
    return _runtime.clear_global_parameter(name)

# Bind a sound's volume to a global parameter (RTPC). Each Update tick
# the runtime reads the current value of `param_name`, linearly remaps
# it from [min_value, max_value] to [min_volume, max_volume], and pushes
# the result through the parameter smoother as the per-voice gain.
#
# Skip-when-unset semantics: until the parameter is set via set_rtpc()
# at least once, the binding has no effect — the authored volume stays
# in place. Binding-installation order is therefore independent of
# gameplay state.
#
# Examples:
#   # Heartbeat gets louder as health drops:
#   Gool.bind_volume_rtpc("heartbeat", "health",
#       /*min_value*/ 0.0, /*max_value*/ 1.0,
#       /*min_volume*/ 1.0, /*max_volume*/ 0.0)
#
#   # Music ducks under intense combat:
#   Gool.bind_volume_rtpc("ambient_music", "combat_intensity",
#       0.0, 1.0,    # 0 = peace, 1 = max combat
#       1.0, 0.3,    # full volume at peace, 30% during combat
#       300.0)       # 300 ms smoothing on the duck
#
# Returns true if the binding was registered or updated; false on
# invalid arguments or budget exhaustion (see AudioConfig::maxSoundRtpcBindings).
func bind_volume_rtpc(sound_name: String, param_name: String,
                       min_value: float, max_value: float,
                       min_volume: float, max_volume: float,
                       smoothing_ms: float = 50.0) -> bool:
    if _runtime == null:
        return false
    return _runtime.set_sound_volume_rtpc(sound_name, param_name,
                                            min_value, max_value,
                                            min_volume, max_volume,
                                            smoothing_ms)

# Remove the volume binding for a sound. Voices currently playing keep
# their last computed smoothed volume (no snap-back to authored level).
func clear_volume_rtpc(sound_name: String) -> bool:
    if _runtime == null:
        return false
    return _runtime.clear_sound_volume_rtpc(sound_name)

# =============================================================================
# Internal helpers
# =============================================================================

# Convert a 16-bit signed little-endian PCM AudioStreamWAV to mono
# float32 samples in [-1, 1]. Stereo sources are downmixed by
# averaging L+R per frame (no fancy panning preservation; voice
# clips are typically mono anyway).
#
# Returns an empty array if the format is unsupported or the buffer
# is malformed; the caller has already pushed an error message in
# that case so we just bail.
func _wav_to_pcm_mono(wav: AudioStreamWAV) -> PackedFloat32Array:
    var out := PackedFloat32Array()
    if wav.format != AudioStreamWAV.FORMAT_16_BITS:
        push_error("[gool] play_voice supports FORMAT_16_BITS WAVs only. "
                   + "Re-import your asset as 16-bit PCM in Godot's "
                   + "import dock.")
        return out
    var data: PackedByteArray = wav.data
    var byte_count: int = data.size()
    if byte_count == 0 or byte_count % 2 != 0:
        return out
    var sample_count: int = byte_count / 2
    var stereo: bool = wav.stereo
    if stereo and sample_count % 2 != 0:
        return out
    var frames: int = (sample_count / 2) if stereo else sample_count
    out.resize(frames)
    var inv: float = 1.0 / 32768.0
    if stereo:
        for i in range(frames):
            var lo_l: int = data[i * 4 + 0]
            var hi_l: int = data[i * 4 + 1]
            var lo_r: int = data[i * 4 + 2]
            var hi_r: int = data[i * 4 + 3]
            var l: int = (hi_l << 8) | lo_l
            var r: int = (hi_r << 8) | lo_r
            if l >= 32768:
                l -= 65536
            if r >= 32768:
                r -= 65536
            out[i] = ((l + r) * 0.5) * inv
    else:
        for i in range(frames):
            var lo: int = data[i * 2 + 0]
            var hi: int = data[i * 2 + 1]
            var s: int = (hi << 8) | lo
            if s >= 32768:
                s -= 65536
            out[i] = float(s) * inv
    return out

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
