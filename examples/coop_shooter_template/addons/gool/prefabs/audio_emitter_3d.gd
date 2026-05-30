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
@export var sound_name: String = ""

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
        push_warning("AudioEmitter3D: /root/Gool autoload not found")
        return
    if not _runtime.is_initialized():
        await _runtime.ready_to_play
    # Register with sane defaults if the host hasn't already.
    _runtime.register_sound_definition(sound_name, true, looping,
                                          min_distance, max_distance,
                                          loop_crossfade_ms)
    if autoplay:
        play()

func play() -> void:
    if _runtime == null or sound_name == "":
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
        sound_started.emit()

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
