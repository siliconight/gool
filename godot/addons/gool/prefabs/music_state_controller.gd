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
## Adaptive music controller. Drop into a scene; configure named
## states (each pointing at a registered sound); call set_state()
## to crossfade between them.
#
## Usage:
##   var music: MusicStateController = $Music
##   music.add_state("explore", "music_explore", 1500.0)
##   music.add_state("combat",  "music_combat",  600.0)
##   music.add_state("victory", "music_victory", 3000.0)
##   music.set_state("explore")     # initial
##   ...later...
##   music.set_state("combat")      # crossfades to combat track
#
## Internally wraps GoolMusicChannel from the binding, which
## implements the equal-power crossfade and tracks the active sound
## id so concurrent set_state calls don't pile up overlapping fades.

@tool
class_name MusicStateController
extends Node


## Declarative state definitions populated into the controller at
## `_ready()`. Each entry is a `GoolMusicState` resource: name +
## sound_name + crossfade ms. Designers can drag .tres files into
## this array or create them inline via the inspector.
##
## This is purely additive — `add_state()` continues to work and
## entries added via code coexist with declarative ones. If a
## declarative entry and an imperative `add_state()` call use the
## same name, last-write-wins (the dictionary key is overwritten).
##
## v0.81.9: added so designers can author music states in the
## inspector instead of having to write code. The runtime data
## model is unchanged.
@export var states_at_startup: Array[GoolMusicState] = []


## State to enter automatically after declarative hydration
## finishes. Empty string (default) leaves the controller stopped
## — your game code calls `set_state()` when it wants music to
## begin. Useful for "music starts in the explore state when the
## scene loads" pattern.
@export var initial_state: String = ""


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
		push_warning("MusicStateController: /root/Gool autoload not found. The gool plugin is installed but not enabled. Fix: open Project Settings → Plugins, find 'gool' in the list, tick the Enable checkbox. (If gool is not in the list, the addon folder is missing — see https://github.com/siliconight/gool for install instructions.)")
		return
	if not _runtime.is_initialized():
		await _runtime.ready_to_play
	_channel = ClassDB.instantiate("GoolMusicChannel")
	if _channel == null:
		push_error("MusicStateController: GoolMusicChannel class not registered")
		return
	add_child(_channel)
	_channel.attach(_runtime)

	# v0.81.9: Hydrate declarative states from the inspector array.
	# Runs AFTER channel.attach so set_state() (if initial_state is
	# configured) has a fully-wired channel to dispatch through.
	_hydrate_declarative_states()
	if initial_state != "":
		set_state(initial_state)


# Walk the states_at_startup export array and populate the runtime
# states dictionary. Skips entries that fail GoolMusicState.is_valid()
# (empty name or empty sound_name) with a clear per-entry warning;
# the controller keeps working with the valid entries.
func _hydrate_declarative_states() -> void:
	for entry in states_at_startup:
		if entry == null:
			# Empty array slot — common during inspector editing
			# before the designer fills the row. Silent skip.
			continue
		if not entry.is_valid():
			push_warning(
				"MusicStateController: skipping declarative state "
				+ "entry with name='%s' sound_name='%s' — both must "
					% [entry.name, entry.sound_name]
				+ "be non-empty. Fill them in via the inspector or "
				+ "remove the entry from states_at_startup."
			)
			continue
		add_state(entry.name, entry.sound_name, entry.fade_ms)

func add_state(state_name: String, sound_name: String,
				fade_ms: float = 1500.0) -> void:
	states[state_name] = MusicState.new(sound_name, fade_ms)

func set_state(state_name: String) -> void:
	if not states.has(state_name):
		var known: Array = states.keys()
		if known.is_empty():
			push_warning(
				"MusicStateController: set_state('%s') called but no "
				% state_name
				+ "states have been added yet. Call add_state(name, "
				+ "sound_name, fade_ms) for each state before calling "
				+ "set_state."
			)
		else:
			push_warning(
				"MusicStateController: unknown state '%s'. "
				% state_name
				+ "Known states: %s. Did you mean to add_state() this "
				% str(known)
				+ "first, or did the name get a typo?"
			)
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
