# addons/gool/prefabs/audio_emitter_3d.gd
#
# Drag-and-drop 3D positional audio emitter. Equivalent to
# AudioStreamPlayer3D but routed through gool — gets occlusion,
# Doppler, reverb sends, distance attenuation, and sidechain
# ducking from the engine's bus graph.
#
# Usage:
#   1. Add an AudioEmitter3D node to your scene
#   2. Set sound_name in the inspector (must match a registered sound)
#   3. Tick autoplay if you want it to play on _ready, or call play()
#
# All scripting beyond this is optional.

@tool
class_name AudioEmitter3D
extends Node3D

## Sound to play. Must be registered with the runtime via
## register_pcm_sound() or load_sound_bank_from_json() before this
## node enters the scene tree.
##
## If left empty AND `stream` is set, the prefab derives a name
## from the stream's resource path and registers it automatically
## on _ready. This is the convenience path — drop an emitter, drag
## a .wav onto the `stream` field, ignore everything else, done.
@export var sound_name: String = ""

## Optional inline AudioStream. When set, the emitter registers
## this stream with the runtime on _ready (lazy registration)
## using a name derived from the stream's resource path. The
## intent is the drag-and-drop case where you don't want to author
## a separate GoolSoundBank for a one-off sound.
##
## Precedence: if `sound_name` is non-empty, it wins and `stream`
## is ignored (the emitter assumes the host already registered
## that name). If `sound_name` is empty and `stream` is set, the
## stream is registered and used. If both are empty, the emitter
## warns on _ready and doesn't play.
##
## Restrictions match GoolSoundBankLoader: the stream needs a
## valid resource_path (i.e. was loaded from a .wav/.ogg/.flac/
## .opus file) or be an AudioStreamWAV with raw PCM data.
## Procedural stream types (Randomizer, Polyphonic, Generator)
## are rejected with a diagnostic — for those, register the
## samples via Gool.register_pcm_sound() from a script and set
## `sound_name` explicitly.
@export var stream: AudioStream = null

## Whether the sound loops. Looping sounds also benefit from
## loop_crossfade_ms to eliminate boundary clicks.
@export var looping: bool = false

## Crossfade duration applied at the loop boundary (looping sounds
## only). 50-200 ms is typical for music; 5-20 ms for SFX loops.
## 0 disables — the cursor wraps with a hard fmod.
@export_range(0.0, 500.0, 1.0, "suffix:ms") var loop_crossfade_ms: float = 0.0

## Fade-in duration when play() is called or autoplay fires.
@export_range(0.0, 5000.0, 1.0, "suffix:ms") var fade_in_ms: float = 0.0

## Fade-out duration applied when stop() is called or this node
## leaves the tree.
@export_range(0.0, 5000.0, 1.0, "suffix:ms") var fade_out_ms: float = 50.0

## Minimum distance at which the sound is at full volume.
@export_range(0.0, 1000.0, 0.1, "suffix:m") var min_distance: float = 1.0

## Distance at which the sound becomes inaudible.
@export_range(1.0, 10000.0, 0.1, "suffix:m") var max_distance: float = 50.0

## Play automatically on _ready.
@export var autoplay: bool = false

## Forwarded to runtime.set_emitter_playback_speed; 1.0 = original.
@export_range(0.25, 4.0, 0.01) var playback_speed: float = 1.0:
    set(value):
        playback_speed = value
        if _handle != 0 and _runtime:
            _runtime.set_emitter_playback_speed(_handle, playback_speed, 50.0)

signal sound_started
signal sound_finished       # fired after fade_out_ms in stop()

var _handle: int = 0
var _runtime: Node = null    # GoolAudioRuntime

func _ready() -> void:
    if Engine.is_editor_hint():
        return
    _runtime = get_node_or_null("/root/Gool")
    if _runtime == null:
        push_warning("AudioEmitter3D: /root/Gool autoload not found. The gool plugin is installed but not enabled. Fix: open Project Settings → Plugins, find 'gool' in the list, tick the Enable checkbox. (If gool is not in the list, the addon folder is missing — see https://github.com/siliconight/gool for install instructions.)")
        return
    if not _runtime.is_initialized():
        await _runtime.ready_to_play
    # Resolve sound_name. Three cases:
    #   1. sound_name set explicitly — host registered the sound, use as-is.
    #   2. sound_name empty, stream set — lazy-register the stream and
    #      use a name derived from its resource_path.
    #   3. neither set — warn and bail; nothing to play.
    if sound_name == "" and stream != null:
        var derived := _derive_name_from_stream(stream)
        if derived == "":
            push_warning(
                "AudioEmitter3D: inline `stream` has no resource_path "
                + "and isn't an AudioStreamWAV — can't be registered "
                + "automatically. Either save the stream as a .tres/"
                + ".wav/.ogg/.mp3 file, or register the samples via "
                + "Gool.register_pcm_sound() from a script and set "
                + "sound_name explicitly."
            )
            return
        var handle: int = _runtime.register_sound_from_stream(derived, stream)
        if handle == 0:
            push_warning(
                "AudioEmitter3D: register_sound_from_stream failed "
                + "for inline stream (derived name '%s'). Check that "
                % derived
                + "the underlying decoder is compiled in "
                + "(AUDIO_ENGINE_DECODERS_* in CMake)."
            )
            return
        sound_name = derived
    elif sound_name == "":
        push_warning(
            "AudioEmitter3D: neither `sound_name` nor `stream` is set. "
            + "Set one of them in the inspector: `sound_name` (string) "
            + "references a sound the host pre-registered via "
            + "Gool.register_pcm_sound() or a GoolSoundBankLoader; "
            + "`stream` (AudioStream) is auto-registered for one-off "
            + "use."
        )
        return
    # Register with sane defaults if the host hasn't already.
    _runtime.register_sound_definition(sound_name, true, looping,
                                          min_distance, max_distance,
                                          loop_crossfade_ms)
    if autoplay:
        play()

# Derives a stable sound_name from an AudioStream. Uses the resource
# path when available (the 95% case — Godot import-pipeline assets),
# else falls back to a uniqueness-via-instance-id name for in-memory
# AudioStreamWAVs. Returns "" if no derivation strategy applies.
func _derive_name_from_stream(s: AudioStream) -> String:
    var path: String = s.resource_path
    if path != "":
        # res://sfx/coin.wav → "auto:res://sfx/coin.wav"
        # Prefix avoids accidental collisions with host-chosen names.
        return "auto:" + path
    if s is AudioStreamWAV:
        # Programmatically-built WAV — make the name stable per
        # instance so repeated _ready calls hit the cached registration.
        return "auto:wav:%d" % s.get_instance_id()
    return ""

func play() -> void:
    # v0.22.4: explicit early-bailout warnings instead of silent
    # returns. Previously this method would return silently when
    # _runtime was null or sound_name was empty — making it
    # impossible to tell "I called play() and nothing happened"
    # from "I called play() and the runtime is broken."
    if _runtime == null:
        push_warning(
            "AudioEmitter3D '%s': play() called but /root/Gool is not "
            % name
            + "available. The plugin may not be enabled, or _ready "
            + "hasn't completed yet."
        )
        return
    if sound_name == "":
        push_warning(
            "AudioEmitter3D '%s': play() called but sound_name is "
            % name
            + "empty. Set sound_name in the inspector (or set `stream` "
            + "and let _ready auto-derive a name)."
        )
        return
    if _handle != 0:
        # Replace existing — fade out the old one immediately.
        _runtime.destroy_emitter(_handle, fade_out_ms)
        _handle = 0
    _handle = _runtime.create_emitter(
        sound_name, global_transform.origin, looping, fade_in_ms)
    if playback_speed != 1.0 and _handle != 0:
        _runtime.set_emitter_playback_speed(_handle, playback_speed, 0.0)
    if _handle != 0:
        # v0.22.4: success log. Makes "play() was called and produced
        # an emitter" visible in the Output panel — distinguishes
        # successful playback from silent failure modes downstream
        # (muted bus, wrong sample format, wrong audio device, etc).
        # v0.23.2: routed via GoolLog. INFO-level so it's visible by
        # default; silence with `emitter:warn` in the categories
        # override if the per-play noise becomes too much.
        GoolLog.info("emitter", "play", {
            "node": name,
            "sound": sound_name,
            "pos": global_transform.origin,
            "looping": looping,
        })
        sound_started.emit()
    else:
        # create_emitter returned 0 — the sound name isn't registered.
        # Most common cause: emitter ran before the bank loader
        # finished registering, or the name has a typo.
        GoolLog.warn("emitter", "create_emitter returned 0",
            {"node": name, "sound_name": sound_name,
             "causes": "(1) GoolSoundBankLoader hasn't run yet; "
                     + "(2) bank doesn't contain this name (open bank.tres "
                     + "and check `sounds` dict); "
                     + "(3) typo (sound_name is case-sensitive, uses "
                     + "forward slashes, e.g. 'sfx/explosion')"})

func stop() -> void:
    if _runtime != null and _handle != 0:
        _runtime.destroy_emitter(_handle, fade_out_ms)
        _handle = 0
        # Defer the signal by fade_out_ms so listeners can release
        # references after the fade completes.
        await get_tree().create_timer(fade_out_ms / 1000.0).timeout
        sound_finished.emit()

func _process(_delta: float) -> void:
    # Stream transform updates to the engine while playing. Movement
    # is what drives Doppler shift and updated distance attenuation.
    if _handle != 0 and _runtime != null:
        var fwd := -global_transform.basis.z       # Godot convention
        _runtime.set_emitter_transform(
            _handle, global_transform.origin, fwd, Vector3.ZERO)

func _exit_tree() -> void:
    if _runtime != null and _handle != 0:
        _runtime.destroy_emitter(_handle, fade_out_ms)
        _handle = 0
