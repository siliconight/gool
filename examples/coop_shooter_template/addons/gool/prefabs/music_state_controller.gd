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

# addons/gool/prefabs/music_state_controller.gd
#
# Adaptive music controller. Drop into a scene; configure named
# states (each pointing at a registered sound); call set_state()
# to crossfade between them.
#
# Usage:
#   var music: MusicStateController = $Music
#   music.add_state("explore", "music_explore", 1500.0)
#   music.add_state("combat",  "music_combat",  600.0)
#   music.add_state("victory", "music_victory", 3000.0)
#   music.set_state("explore")     # initial
#   ...later...
#   music.set_state("combat")      # crossfades to combat track
#
# Internally wraps GoolMusicChannel from the binding, which
# implements the equal-power crossfade and tracks the active sound
# id so concurrent set_state calls don't pile up overlapping fades.

@tool
class_name MusicStateController
extends Node

class MusicState:
    var sound_name: String
    var fade_ms:    float
    func _init(s: String, ms: float) -> void:
        sound_name = s
        fade_ms    = ms

## Map of state_name -> MusicState entries. Populate via add_state().
var states: Dictionary = {}

## Currently active state name; "" means stopped.
var current_state: String = ""

signal state_changed(from: String, to: String)

var _runtime: Node = null
var _channel: Node = null

func _ready() -> void:
    if Engine.is_editor_hint():
        return
    _runtime = get_node_or_null("/root/Gool")
    if _runtime == null:
        push_warning("MusicStateController: /root/Gool autoload not found")
        return
    if not _runtime.is_initialized():
        await _runtime.ready_to_play
    _channel = ClassDB.instantiate("GoolMusicChannel")
    if _channel == null:
        push_error("MusicStateController: GoolMusicChannel class not registered")
        return
    add_child(_channel)
    _channel.attach(_runtime)

func add_state(state_name: String, sound_name: String,
                fade_ms: float = 1500.0) -> void:
    states[state_name] = MusicState.new(sound_name, fade_ms)

func set_state(state_name: String) -> void:
    if not states.has(state_name):
        push_warning("MusicStateController: unknown state '%s'" % state_name)
        return
    if state_name == current_state:
        return
    var prev := current_state
    current_state = state_name
    if _channel == null:
        return
    var st: MusicState = states[state_name]
    _channel.play(st.sound_name, st.fade_ms)
    state_changed.emit(prev, state_name)

func stop(fade_ms: float = 1500.0) -> void:
    if _channel == null:
        return
    var prev := current_state
    current_state = ""
    _channel.stop(fade_ms)
    state_changed.emit(prev, "")
