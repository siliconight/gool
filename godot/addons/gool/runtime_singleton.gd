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
        push_error(
            "[gool] GoolAudioRuntime class not registered. This almost "
            + "always means the GDExtension binary is missing from "
            + "addons/gool/bin/. Fixes:\n"
            + "  (1) Download the addon zip from "
            + "https://github.com/siliconight/gool/releases that matches "
            + "your OS, and unzip its addons/gool/ over yours.\n"
            + "  (2) Or build from source: see SETUP.md.\n"
            + "Check addons/gool/bin/ for one of: libgool_godot.so "
            + "(Linux), gool_godot.dll (Windows), libgool_godot.dylib "
            + "(macOS). If the file is there but Godot still can't "
            + "load it, the binary likely targets a different Godot "
            + "minor version than yours — match versions or rebuild."
        )
        return
    add_child(_runtime)

    # Load the project's audio config. If config.json contains a
    # "buses" array, we route through init_with_config() so the
    # engine builds the multi-tier bus graph at startup. If the file
    # is missing, empty, or has no buses key, we fall back to plain
    # init() — which builds a single-master topology, the legacy
    # behavior.
    var cfg_text := _load_config_text()
    var cfg_dict := _parse_config_dict(cfg_text)
    var sr : int = cfg_dict.get("sample_rate", 48000)
    var bs : int = cfg_dict.get("buffer_size", 512)
    var has_bus_graph: bool = cfg_dict.has("buses") \
        and cfg_dict["buses"] is Array \
        and (cfg_dict["buses"] as Array).size() > 0

    var ok: bool
    if has_bus_graph:
        # Pass the raw JSON text through — the C++ side parses it
        # using the same loader that's unit-tested at the engine
        # level. This keeps GDScript out of the schema-translation
        # business: the binding is the only place the format is
        # interpreted.
        ok = _runtime.init_with_config(cfg_text, sr, bs)
    else:
        ok = _runtime.init(sr, bs)

    if not ok:
        if has_bus_graph:
            push_error(
                "[gool] runtime init failed: bus config rejected. "
                + "Check the prior error from the JSON parser above for "
                + "the specific line. Common causes: duplicate bus ids, "
                + "a bus that references a parent which doesn't exist, "
                + "an effect kind that isn't recognized. Fix res://gool/"
                + "config.json or delete it to regenerate defaults."
            )
        else:
            push_error(
                "[gool] runtime init failed: no audio device available. "
                + "Possible causes:\n"
                + "  (1) Sample rate or buffer size in res://gool/config.json "
                + "isn't supported by your audio device. Try sample_rate=44100 "
                + "or buffer_size=1024.\n"
                + "  (2) Another app has exclusive access to the device "
                + "(some DAWs do this on Windows).\n"
                + "  (3) Running headless without an audio device — set the "
                + "AUDIO_ENGINE_BACKEND env var to 'null' to use the silent "
                + "backend for CI / server use."
            )
        return
    # v0.22.4: audible "I'm alive" line. Previously a successful init
    # was completely silent — the autoload's _ready returned cleanly
    # with no output, making "no sound" indistinguishable from
    # "runtime never started." This single print is the
    # most-requested diagnostic from the v0.22.3 session.
    var v: Dictionary = _runtime.get_version()
    var version_str: String = v.get("full", "unknown") if v else "unknown"
    var bus_count: int = 0
    if has_bus_graph and cfg_dict.has("buses"):
        bus_count = cfg_dict["buses"].size()
    else:
        bus_count = 1   # single Master bus when no config
    var config_source: String = CONFIG_PATH if has_bus_graph else "defaults"
    print("[gool] ready: version=%s rate=%dHz buffer=%d buses=%d config=%s"
            % [version_str, sr, bs, bus_count, config_source])
    # v0.22.7: log the audio device miniaudio actually opened. If the
    # name doesn't match where the user expects sound, that's the bug
    # (wrong default output device) — no further C++ debugging needed.
    var device_desc: String = _runtime.get_backend_description()
    if device_desc != "":
        print("[gool] audio device: %s" % device_desc)
    else:
        print("[gool] audio device: (unknown — backend doesn't expose name)")
    _ready_emitted = true
    ready_to_play.emit()

# v0.22.7: render-thread health polling. Reads the diagnostic atomics
# the C++ data callback writes (callback invocations, frames rendered,
# peak sample amplitude, render-callback exceptions) and prints them
# every _RENDER_STATS_INTERVAL seconds. The output decisively answers
# "why is gool silent" without needing C++ debugging:
#
#   callback_invocations == 0  →  miniaudio's audio thread never ran.
#                                  Backend Start() returned Success
#                                  but the device wasn't actually
#                                  opened. Real bug in device init.
#
#   callback_invocations > 0
#   AND frames_rendered > 0
#   AND peak_amplitude == 0.0  →  audio thread IS running and IS
#                                  writing frames, but every sample
#                                  is zero. Silence comes from
#                                  upstream (mixer producing
#                                  silence: no active emitters, bus
#                                  at -inf dB, decoder fed empty
#                                  data). C++ mixer instrumentation
#                                  needed.
#
#   peak_amplitude > 0          →  non-silent samples ARE reaching
#                                  the device. Any inaudibility is
#                                  on the Windows side: wrong output
#                                  device, app volume muted, etc.
#                                  Not a gool bug.
#
# After each log, peak is reset so the next reading reflects samples
# written SINCE that log, not since process start. That way a "peak=0"
# reading means "silent in the last 2 seconds" not "silent at any
# point."
const _RENDER_STATS_INTERVAL: float = 2.0   # seconds between logs
var _render_stats_accum: float = 0.0
var _render_stats_last_invocations: int = 0
var _render_stats_last_frames: int = 0

func _process(delta: float) -> void:
    if _runtime != null and _runtime.is_initialized():
        _runtime.update(delta)
        if not _mirrored_buses.is_empty():
            _poll_mirrored_buses()
        _render_stats_accum += delta
        if _render_stats_accum >= _RENDER_STATS_INTERVAL:
            _render_stats_accum = 0.0
            _log_render_stats()

func _log_render_stats() -> void:
    var stats: Dictionary = _runtime.get_render_stats()
    if stats.is_empty():
        return
    var invocations: int = stats.get("callback_invocations", 0)
    var frames: int = stats.get("frames_rendered", 0)
    var peak: float = stats.get("peak_amplitude", 0.0)
    var exceptions: int = stats.get("exception_count", 0)
    # v0.22.8: mixer-level stats (may be absent on older binaries).
    var active_voices: int = stats.get("active_voices", -1)
    var mixer_peak: float = stats.get("mixer_peak", -1.0)
    var master_gain: float = stats.get("master_gain", -1.0)
    var delta_invocations: int = invocations - _render_stats_last_invocations
    var delta_frames: int = frames - _render_stats_last_frames
    _render_stats_last_invocations = invocations
    _render_stats_last_frames = frames
    # Diagnosis line — one log every 2 seconds when running.
    if active_voices >= 0:
        # v0.22.8 binary — full mixer stats available
        print("[gool] render: cb=%d (Δ%d) frames=%d (Δ%d) peak=%.4f mixer_peak=%.4f voices=%d gain=%.2f exc=%d"
                % [invocations, delta_invocations, frames, delta_frames,
                   peak, mixer_peak, active_voices, master_gain,
                   exceptions])
    else:
        # Older binary — only backend stats available
        print("[gool] render: cb=%d (Δ%d) frames=%d (Δ%d) peak=%.4f exc=%d"
                % [invocations, delta_invocations, frames, delta_frames,
                   peak, exceptions])
    # Layered diagnosis when something looks broken. v0.22.8 ordering
    # is bottom-up: most-upstream cause first (audio thread dead) →
    # most-downstream cause last (master gain silencing).
    if delta_invocations == 0 and invocations == 0:
        push_warning(
            "[gool] DEAD AIR: render callback has never been invoked. "
            + "miniaudio's audio thread didn't start. The backend "
            + "init reported success but the playback device was "
            + "never opened. This is a real backend bug — file a "
            + "report at github.com/siliconight/gool/issues."
        )
    elif delta_invocations == 0:
        push_warning(
            "[gool] DEAD AIR: render callback stopped being called. "
            + "miniaudio's audio thread may have died or paused. "
            + "Audio output frozen as of this interval."
        )
    elif peak == 0.0 and delta_frames > 0:
        # v0.22.8: now use mixer stats to discriminate the cause.
        if active_voices == 0:
            push_warning(
                "[gool] DEAD AIR (no active voices): %d frames "
                % delta_frames
                + "written this interval but the mixer's voice pool "
                + "is empty. Emitters that called create_emitter "
                + "didn't actually wire into the voice list. Likely "
                + "cause: command queue draining issue, or "
                + "DrainCommands isn't promoting the voice slot to "
                + "VoiceMode::Sound/Streaming/Voice. Check the "
                + "MixerCommand dispatch path in audio_mixer.cpp."
            )
        elif mixer_peak == 0.0 and active_voices > 0:
            push_warning(
                "[gool] DEAD AIR (mixer silent, %d voices active): "
                % active_voices
                + "voices ARE being mixed but the Master bus output "
                + "buffer is all zeros. The voices are producing "
                + "silence (decoder fed empty PCM, voice mode mismatch, "
                + "wrong bus routing), OR the bus chain is summing to "
                + "zero somewhere upstream of Master. Check decoder "
                + "PCM state and bus-graph routing."
            )
        elif mixer_peak > 0.0 and master_gain == 0.0:
            push_warning(
                "[gool] DEAD AIR (master gain = 0): mixer produced "
                + "non-silent audio (peak=%.4f) but Master bus output "
                % mixer_peak
                + "gain is 0 (-inf dB). Either config.json sets Master "
                + "gain_db to -inf, or set_bus_gain_db zeroed it at "
                + "runtime. Check config and any gain-control code."
            )
        elif mixer_peak > 0.0 and master_gain > 0.0 and peak == 0.0:
            push_warning(
                "[gool] DEAD AIR (post-gain mystery): mixer peak=%.4f, "
                % mixer_peak
                + "master gain=%.2f, but device-buffer peak is 0.0. "
                % master_gain
                + "Unexpected — file a report with this output. "
                + "Possible causes: buffer copy going to wrong "
                + "destination, miniaudio format-conversion truncating, "
                + "or a bug we haven't predicted."
            )
        else:
            push_warning(
                "[gool] DEAD AIR (unknown cause): %d frames written "
                % delta_frames
                + "this interval, all zero amplitude. Mixer stats: "
                + "voices=%d peak=%.4f gain=%.2f. "
                % [active_voices, mixer_peak, master_gain]
                + "Symptom doesn't match any predicted cause; manual "
                + "investigation needed."
            )
    elif exceptions > 0:
        push_warning(
            "[gool] %d render-callback exception(s) caught since "
            % exceptions
            + "Initialize. Engine's OnRender path is throwing — "
            + "audio frames have been dropped to silence by the "
            + "catch-all barrier."
        )
    # Reset peak for the next interval window (resets BOTH backend
    # peak AND mixer peak — v0.22.8).
    _runtime.reset_render_peak()


# Called from _process when at least one bus pair is registered for
# automatic mirroring. Cheap when nothing changed — the cached-db
# check short-circuits the C++ call. Logs a one-time warning if a
# registered Godot bus name disappears between frames.
func _poll_mirrored_buses() -> void:
    for godot_bus_name in _mirrored_buses:
        var idx := AudioServer.get_bus_index(godot_bus_name)
        if idx < 0:
            continue   # bus was renamed/removed; silently skip
        var db := AudioServer.get_bus_volume_db(idx)
        if _mirrored_last_db.get(godot_bus_name, 1e9) == db:
            continue
        _mirrored_last_db[godot_bus_name] = db
        var gool_bus_name: String = _mirrored_buses[godot_bus_name]
        _runtime.set_bus_gain_db(gool_bus_name, db)

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

## AudioFileFormat constants — pass to register_sound_from_bytes()
## as `format_hint`. FORMAT_AUTO sniffs by magic bytes (recommended);
## the others are explicit overrides for hosts that already know
## what format they're passing.
const FORMAT_AUTO:        int = 0
const FORMAT_WAV:         int = 1
const FORMAT_OGG_VORBIS:  int = 2
const FORMAT_FLAC:        int = 3
const FORMAT_OPUS:        int = 4

## Load a sound file from any Godot-readable path (including res://
## in PCK-packaged builds) and register it as a one-shot PCM asset.
## Supported formats depend on what was compiled in:
##   - WAV    (AUDIO_ENGINE_DECODERS_WAV)
##   - Vorbis (AUDIO_ENGINE_DECODERS_OGG, .ogg/.oga extension)
##   - FLAC   (AUDIO_ENGINE_DECODERS_FLAC)
##   - Opus   (AUDIO_ENGINE_DECODERS_OPUS, .opus extension)
##
## The default CMake build has all decoders OFF — projects that
## want file playback must enable the relevant flag(s). When a
## decoder is compiled out, the binding pushes a clear error.
##
## Returns the AudioSoundId (positive 64-bit int) on success, 0 on
## failure. The returned id can be paired with
## register_sound_definition() to wire spatialization, looping, bus
## routing, etc.
##
## For long music tracks where the decoded PCM would be too large
## to keep resident, see the upcoming streaming-from-file binding
## (deferred to a follow-up release; see CHANGELOG).
func register_sound_from_file(name: String, path: String) -> int:
    if not is_initialized():
        push_error("[gool] register_sound_from_file called before init")
        return 0
    return _runtime.register_sound_from_file(name, path)

## Same as register_sound_from_file but takes already-loaded bytes.
## Useful when the host wants to manage file I/O (e.g. custom asset
## packs, network downloads, encrypted blobs).
##
## `format_hint` is one of FORMAT_*; FORMAT_AUTO (the default) sniffs
## by magic bytes — RIFF/WAVE for WAV, OggS+OpusHead for Opus,
## OggS+Vorbis for Vorbis, fLaC for FLAC.
func register_sound_from_bytes(name: String, bytes: PackedByteArray,
                                  format_hint: int = FORMAT_AUTO) -> int:
    if not is_initialized():
        push_error("[gool] register_sound_from_bytes called before init")
        return 0
    return _runtime.register_sound_from_bytes(name, bytes, format_hint)

## Register a Godot AudioStream resource as a gool sound. Convenience
## wrapper over the C++ binding's `register_sound_from_stream` (added
## in v0.14.0). Two paths internally:
##
##   * If `stream.resource_path` is set (the 95% case — the stream
##     was loaded from a .wav/.ogg/.flac/.opus file), the binding
##     reads those original bytes and registers them. No re-decoding
##     through Godot's runtime; gool's own decoder owns the asset.
##   * If `stream` is an AudioStreamWAV with no resource path
##     (procedurally constructed), the raw PCM in its `data`
##     property is registered directly.
##
## Procedural stream subtypes that can't be reduced to a single PCM
## asset (AudioStreamRandomizer, AudioStreamPolyphonic,
## AudioStreamGenerator) are rejected with a diagnostic; for those
## paths, use `register_pcm_sound()` from script with the samples
## you already have.
##
## Returns the AudioSoundId on success, 0 on failure.
func register_sound_from_stream(name: String, stream: AudioStream) -> int:
    if not is_initialized():
        push_error("[gool] register_sound_from_stream called before init")
        return 0
    return _runtime.register_sound_from_stream(name, stream)

## AudioCategory enum mirrored for GDScript callers. Matches the
## C++ enum order (audio::AudioCategory in types.h). Hosts pass one
## of these to register_sound_definition() to control routing
## through `category_routing` in config.json.
const CATEGORY_SFX:      int = 0
const CATEGORY_VOICE:    int = 1
const CATEGORY_MUSIC:    int = 2
const CATEGORY_AMBIENCE: int = 3
const CATEGORY_UI:       int = 4
const CATEGORY_DIALOGUE: int = 5

## Register a sound definition.
##
## `category` controls which bus the runtime picks when no explicit
## bus override is set. Default is SFX (0). See CATEGORY_* constants.
##
## `target_bus_name` overrides category routing. Pass the bus's
## `name` from config.json (e.g. "LocalSfx", "RemoteSfx", "Music").
## Empty string (the default) → use category routing. Unknown bus
## names produce a warning and fall back to category routing.
##
## To route the same audio asset to different buses, register it
## under different gameplay names: e.g., "rifle_fire_local" →
## LocalSfx, "rifle_fire_remote" → RemoteSfx.
func register_sound_definition(name: String, spatialized: bool = true,
                                 looping: bool = false,
                                 min_distance: float = 1.0,
                                 max_distance: float = 50.0,
                                 loop_crossfade_ms: float = 0.0,
                                 category: int = CATEGORY_SFX,
                                 target_bus_name: String = "") -> void:
    _runtime.register_sound_definition(name, spatialized, looping,
                                         min_distance, max_distance,
                                         loop_crossfade_ms,
                                         category, target_bus_name)

## Resolve a bus name to its BusId. Returns -1 if no bus matches.
## Use to bridge between code that knows bus names (config files,
## hosts) and code that needs BusId tokens (set_bus_gain_db,
## set_effect_parameter). O(N) over kMaxBuses; fine for init/
## registration time, not per-frame.
func find_bus_id_by_name(name: String) -> int:
    if not is_initialized():
        return -1
    return _runtime.find_bus_id_by_name(name)

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

# ---- v0.22.0: simplified one-shot networked SFX ------------------------

## Fire-and-forget one-shot SFX across the network. The sound plays
## locally immediately on the caller, and is replicated to every
## connected peer via an unreliable RPC.
##
## Use this when you have a discrete sound event (gunshot, impact,
## button click, ability cast) that should be heard everywhere with
## drop-if-late semantics — if a packet arrives stale, the audio is
## already irrelevant, so just drop it gracefully rather than
## playing late.
##
## What this DOESN'T do:
##   * Client prediction → use NetworkedAudioEvent.predict() instead.
##   * Server authority / validation → use NetworkedAudioEvent with
##     Mode.SERVER_AUTHORITATIVE.
##   * Team filtering / audible-radius gating → use NetworkedAudioEvent
##     with default_audible_radius and team parameters.
##   * Persistent positioned sources (loops, vehicles, ambient
##     emitters) → use NetworkedAudioEmitter3D for those.
##
## This is the simplest possible networked-SFX path: one call, all
## peers hear it, no scene-tree nodes required. About 30 lines of
## code under the hood, including the RPC marshalling.
##
## When called without an active multiplayer peer, plays locally and
## skips the RPC silently — useful for testing networked events in
## single-player without a separate code path.
func play_networked(sound_name: String,
                       position: Vector3 = Vector3.ZERO,
                       volume_db: float = 0.0,
                       pitch: float = 1.0) -> void:
    if not is_initialized():
        push_error(
            "[gool] play_networked('%s') called before runtime init. "
            % sound_name
            + "Either wait for the ready_to_play signal or call from "
            + "_ready() after the autoload has finished initializing."
        )
        return
    var t_ms: int = Time.get_ticks_msec()
    # Local immediate play.
    _runtime.submit_event_local(sound_name, position, 0, 128, t_ms)
    # If no multiplayer peer is connected, the local play is all that's
    # needed. Otherwise broadcast to other peers. We use the default
    # priority of 128 (medium) which the engine routes to the
    # drop-if-late event ring.
    if multiplayer != null and multiplayer.has_multiplayer_peer() \
            and multiplayer.get_multiplayer_peer().get_connection_status() \
                == MultiplayerPeer.CONNECTION_CONNECTED:
        _rpc_play_networked.rpc(sound_name, position, volume_db, pitch, t_ms)

# RPC handler invoked on every receiving peer. Plays the sound locally
# on the receiver with the same parameters the sender used. The Vector3
# position is passed as-is (Godot's RPC machinery serializes Vector3
# natively, no per-component packing needed).
#
# `volume_db` and `pitch` are accepted for forward compatibility — the
# current submit_event_local path doesn't use them, but the C++ engine
# already supports both via the event payload and a future autoload
# expansion can wire them through without a binding compatibility break.
@rpc("any_peer", "call_remote", "unreliable")
func _rpc_play_networked(sound_name: String, position: Vector3,
                            volume_db: float, pitch: float,
                            sender_t_ms: int) -> void:
    if not is_initialized():
        return
    # Optional staleness check: if the event is more than 250ms old
    # by our clock, drop it. Matches the default category-staleness
    # for SFX (see DefaultStalenessMsForCategory in events.h). This
    # is the "drop-if-late" behaviour we promised in the doc above.
    var local_t_ms: int = Time.get_ticks_msec()
    if local_t_ms - sender_t_ms > 250:
        # Telemetry could go here if needed.
        return
    _runtime.submit_event_local(sound_name, position, 0, 128, sender_t_ms)


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

# =============================================================================
# RTPC binding facades (v0.5+)
# =============================================================================
#
# Bind a sound's per-voice parameter (Volume / Pitch / LowPass / ReverbSend)
# to a global RTPC. Each `Update` tick the runtime reads the current value of
# `param_name`, applies the configured curve and remap, and pushes the
# result through the parameter smoother.
#
# Skip-when-unset semantics: until `set_rtpc(param_name, ...)` is called at
# least once, the binding has no effect. Authored values stay in place.
#
# At most one binding per (sound, target) pair. Re-binding the same target
# replaces the old binding. A sound can have up to four bindings simultaneously
# (volume + pitch + lowpass + reverb_send, each driven by its own parameter).
#
# Examples:
#   # Heartbeat gets louder and pitches up as health drops:
#   Gool.bind_volume_rtpc("heartbeat", "health", 0, 1, 1.0, 0.0)
#   Gool.bind_pitch_rtpc("heartbeat",  "health", 0, 1, 1.4, 1.0)
#
#   # Music ducks under combat with smoothstep curve for an organic feel:
#   Gool.bind_rtpc("ambient", {
#       "parameter": "combat_intensity",
#       "target":    "volume",
#       "curve":     "scurve",
#       "min_value": 0.0, "max_value": 1.0,
#       "min_output": 1.0, "max_output": 0.3,
#       "smoothing_ms": 300.0,
#   })

# Bind a Volume target with a linear curve. The most common case.
func bind_volume_rtpc(sound_name: String, param_name: String,
                       min_value: float, max_value: float,
                       min_output: float, max_output: float,
                       smoothing_ms: float = 50.0) -> bool:
    if _runtime == null:
        return false
    return _runtime.set_sound_rtpc(sound_name, param_name,
                                     "volume", "linear",
                                     min_value, max_value,
                                     min_output, max_output,
                                     2.0, smoothing_ms)

# Bind a Pitch target with a linear curve. Output is a pitch multiplier
# (1.0 = unchanged, 2.0 = octave up, 0.5 = octave down).
func bind_pitch_rtpc(sound_name: String, param_name: String,
                      min_value: float, max_value: float,
                      min_output: float, max_output: float,
                      smoothing_ms: float = 50.0) -> bool:
    if _runtime == null:
        return false
    return _runtime.set_sound_rtpc(sound_name, param_name,
                                     "pitch", "linear",
                                     min_value, max_value,
                                     min_output, max_output,
                                     2.0, smoothing_ms)

# Bind a LowPassCutoff target. Output in [0, 1]; 0 = no filter, 1 = fully
# muffled. Combined with the spatializer's baseline via max(), so RTPC
# can add filtering on top of occlusion / air absorption but never reduce
# what the world applied.
func bind_lowpass_rtpc(sound_name: String, param_name: String,
                        min_value: float, max_value: float,
                        min_output: float, max_output: float,
                        smoothing_ms: float = 50.0) -> bool:
    if _runtime == null:
        return false
    return _runtime.set_sound_rtpc(sound_name, param_name,
                                     "lowpass", "linear",
                                     min_value, max_value,
                                     min_output, max_output,
                                     2.0, smoothing_ms)

# Bind a ReverbSend target. Output in [0, 1] is added to the global
# reverb send amount with a clamp at 1.0.
func bind_reverb_rtpc(sound_name: String, param_name: String,
                       min_value: float, max_value: float,
                       min_output: float, max_output: float,
                       smoothing_ms: float = 50.0) -> bool:
    if _runtime == null:
        return false
    return _runtime.set_sound_rtpc(sound_name, param_name,
                                     "reverb", "linear",
                                     min_value, max_value,
                                     min_output, max_output,
                                     2.0, smoothing_ms)

# Advanced: bind any target with any curve, configured via Dictionary.
# Keys (all required unless marked optional):
#   parameter:     String — global parameter name
#   target:        String — "volume" | "pitch" | "lowpass" | "reverb"
#   curve:         String (optional, default "linear") —
#                  "linear" | "exponential" | "inverse_exp" | "scurve"
#   exponent:      float (optional, default 2.0) — used by exp / inv_exp
#   min_value:     float — input range start
#   max_value:     float — input range end
#   min_output:    float — output at min_value (after curve)
#   max_output:    float — output at max_value (after curve)
#   smoothing_ms:  float (optional, default 50.0)
func bind_rtpc(sound_name: String, binding: Dictionary) -> bool:
    if _runtime == null:
        return false
    var param      = binding.get("parameter", "")
    var target     = binding.get("target",    "")
    var curve      = binding.get("curve",     "linear")
    var exponent   = binding.get("exponent",      2.0)
    var min_value  = binding.get("min_value",     0.0)
    var max_value  = binding.get("max_value",     1.0)
    var min_output = binding.get("min_output",    0.0)
    var max_output = binding.get("max_output",    1.0)
    var smoothing  = binding.get("smoothing_ms",  50.0)
    if param == "" or target == "":
        push_error("[gool] bind_rtpc requires 'parameter' and 'target' keys")
        return false
    return _runtime.set_sound_rtpc(sound_name, param, target, curve,
                                     min_value, max_value,
                                     min_output, max_output,
                                     exponent, smoothing)

# Remove one binding for (sound, target). Returns true if it existed.
func clear_rtpc_binding(sound_name: String, target: String) -> bool:
    if _runtime == null:
        return false
    return _runtime.clear_sound_rtpc(sound_name, target)

# Remove every binding for a sound. Returns the number of bindings removed.
func clear_all_rtpc_bindings(sound_name: String) -> int:
    if _runtime == null:
        return 0
    return _runtime.clear_all_sound_rtpc(sound_name)

# Backward-compat convenience: same as clear_rtpc_binding(name, "volume").
# Kept so v0.4 call sites don't break on upgrade.
func clear_volume_rtpc(sound_name: String) -> bool:
    return clear_rtpc_binding(sound_name, "volume")

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

func _load_config_text() -> String:
    # Returns the raw config.json text, or "" if missing / unreadable.
    # The text (not the parsed dict) is what gets passed to the C++
    # parser when bus graph is configured.
    if not FileAccess.file_exists(CONFIG_PATH):
        return ""
    var f := FileAccess.open(CONFIG_PATH, FileAccess.READ)
    if f == null:
        return ""
    var text := f.get_as_text()
    f.close()
    return text

func _parse_config_dict(text: String) -> Dictionary:
    # Parses the config text into a Godot Dictionary. Used to peek
    # at top-level fields (sample_rate, buffer_size, buses) before
    # deciding which runtime init() variant to call. If parsing
    # fails OR the top-level isn't an object, returns {}.
    if text.is_empty():
        return {}
    var parsed = JSON.parse_string(text)
    if parsed is Dictionary:
        return parsed
    return {}

# ---- v0.14.0: native-Godot integration helpers ---------------------

## Mirror a Godot AudioServer bus's current volume into a gool bus.
##
## One-way binding (Godot → gool); call this whenever the user
## adjusts a Godot volume slider. Example — hooking gool's master
## output into an existing settings menu:
##
##   func _on_master_slider_changed(value: float) -> void:
##       var idx := AudioServer.get_bus_index("Master")
##       AudioServer.set_bus_volume_db(idx, linear_to_db(value))
##       Gool.sync_volume_from_godot_bus("Master")
##
## If your gool bus name differs from the Godot bus name (e.g. you
## have a "SFX" bus in Godot and a "Sfx" bus in gool's config), pass
## the gool name as the second argument.
##
## Returns true on success, false if either bus name is unknown.
func sync_volume_from_godot_bus(godot_bus_name: String = "Master",
                                  gool_bus_name: String = "") -> bool:
    if _runtime == null:
        return false
    var idx := AudioServer.get_bus_index(godot_bus_name)
    if idx < 0:
        push_warning("[gool] sync_volume_from_godot_bus: no Godot bus named '%s'"
                       % godot_bus_name)
        return false
    var db := AudioServer.get_bus_volume_db(idx)
    var target := gool_bus_name if gool_bus_name != "" else godot_bus_name
    return _runtime.set_bus_gain_db(target, db)


## Continuous variant: install an every-frame poll that mirrors the
## given Godot bus volume to the matching gool bus. Cheap (one float
## read + one bus-id lookup per frame) but ensures changes from any
## external source — third-party settings plugins, MIDI controllers,
## animation tracks driving AudioServer — propagate to gool without
## manual sync calls.
##
## Pass `false` to disable polling for a specific bus pair, e.g.
## when switching back to explicit per-callback syncing.
func auto_mirror_godot_bus(godot_bus_name: String,
                            gool_bus_name: String = "",
                            enabled: bool = true) -> void:
    if enabled:
        var target := gool_bus_name if gool_bus_name != "" else godot_bus_name
        _mirrored_buses[godot_bus_name] = target
    else:
        _mirrored_buses.erase(godot_bus_name)


# Storage for auto_mirror_godot_bus pairs. Keys are Godot bus names;
# values are gool bus names. Polled inside _process when non-empty.
var _mirrored_buses: Dictionary = {}

# Cached last-known dB per Godot bus, so the poll only forwards
# updates when the value actually changed — avoids hammering
# set_bus_gain_db every frame for static volumes.
var _mirrored_last_db: Dictionary = {}

