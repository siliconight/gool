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

@tool
class_name GoolDebuggerPlugin
extends EditorDebuggerPlugin

# v0.25.0: cross-process bridge for the mixer dock.
#
# Godot 4 runs the editor and the running game in separate
# processes. When F5 is pressed, the editor spawns a child game
# process with the debugger attached. The game pushes per-bus
# stats via `EngineDebugger.send_message("gool:bus_stats",
# [stats])`; this plugin (subscribed to the "gool" prefix)
# receives them in `_capture` and caches the latest payload.
# The mixer dock polls the plugin every editor frame.
#
# This channel is also what Phase 3.3b/c/d will use for the
# editor→game direction (faders, S/M/B, effect param edits).
#
# v0.25.1: simplified session tracking. v0.25.0 tracked a dict
# of "which sessions are active" and only returned stats from
# active ones — that introduced a race where on second F5,
# session 0's stale data could be returned, or session 1's data
# could be ignored because its "active" flag had been cleared
# by a late-firing session-0 stopped signal. The simpler design:
# one slot for "latest stats", one int for "which session
# produced them", and on session_stopped only clear if it's the
# *same* session we last got stats from.
var _latest_stats: Array = []
var _current_session_id: int = -1

# v0.44.0: cache for the engine-wide render-stats payload sent
# over the gool:render_stats channel. Single Dictionary (not Array
# of per-bus dicts like _latest_stats) because render stats are
# engine-wide. Empty {} when no session is running.
var _latest_render_stats: Dictionary = {}

# Diagnostic counters. Printed at session lifecycle events so
# we can see in Output whether stats are actually arriving.
var _capture_count: int = 0


# Called by the editor when a new debug session starts (every F5).
# Hook the session's stopped signal so we can clear cached stats
# when F8 is pressed.
func _setup_session(session_id: int) -> void:
	print("[gool] debugger session %d started" % session_id)
	_current_session_id = session_id
	_capture_count = 0
	var session := get_session(session_id)
	if session == null:
		push_warning("[gool] get_session(%d) returned null" % session_id)
		return
	# Bind session_id into the callback so it carries through and we
	# can compare on stopped. Avoids races where session 0's stopped
	# signal fires after session 1 started feeding us stats.
	if not session.stopped.is_connected(_on_session_stopped):
		session.stopped.connect(_on_session_stopped.bind(session_id))


# Tells the editor which message-prefix this plugin claims. Godot
# routes "<prefix>:<rest>" messages to plugins whose _has_capture
# returns true for "<prefix>". We claim "gool".
func _has_capture(prefix: String) -> bool:
	return prefix == "gool"


# Called for every "gool:*" message arriving from a running game.
# Return true if handled. The payload format matches the game's
# `EngineDebugger.send_message("gool:bus_stats", [stats])` call:
# data is [stats], data[0] is the actual stats Array.
func _capture(message: String, data: Array, session_id: int) -> bool:
	if message == "gool:bus_stats":
		if data.size() >= 1 and data[0] is Array:
			_latest_stats = data[0]
			_current_session_id = session_id
			_capture_count += 1
			# Diagnostic: print on first stats received so user can
			# see the bridge connected. Subsequent messages are silent
			# (30 Hz, prints would spam Output).
			if _capture_count == 1:
				print("[gool] receiving bus stats from session %d (%d strips)"
						% [session_id, _latest_stats.size()])
		return true
	# v0.44.0: engine-wide render stats for the Live Stats panel.
	# Same cadence and session as bus_stats; cached separately so
	# the mixer dock can read both independently.
	if message == "gool:render_stats":
		if data.size() >= 1 and data[0] is Dictionary:
			_latest_render_stats = data[0]
			_current_session_id = session_id
		return true
	return false


# Polled by the mixer dock at editor frame rate. Returns the
# most recent stats, or [] when no session is running.
func get_latest_bus_stats() -> Array:
	return _latest_stats

## v0.44.0: polled by the mixer dock's Live Stats panel. Returns
## the most recent engine-wide render stats, or {} when no session
## is running. Shape matches Gool.get_render_stats() plus an
## additional "voice_chat" sub-dict mapping player_id → {jitter_ms,
## packet_loss}.
func get_latest_render_stats() -> Dictionary:
	return _latest_render_stats


# Fired when the session ends (F8, game crash, etc). Only clear
# stats if it's the session we're currently tracking. Protects
# against late-firing signals from a stopped older session
# clobbering data from a newly-started session.
func _on_session_stopped(session_id: int) -> void:
	print("[gool] debugger session %d stopped (received %d stats messages)"
			% [session_id, _capture_count])
	if session_id == _current_session_id:
		_latest_stats = []
		_latest_render_stats = {}
		_current_session_id = -1
		_capture_count = 0


# v0.26.0: editor → game command channel. The mixer dock calls these
# helpers when the user interacts with a fader / button. We push the
# command over the current session's send_message channel; the game-
# side `Gool._on_debugger_capture` handler routes it to the runtime.
#
# All commands are best-effort: if no session is active, the call is
# silently dropped (faders work in editor mode too, just without
# affecting any running game). If multiple sessions are active —
# rare but possible — we send to the most recently active one.
#
# Returns true if a session was available and the message was sent.
func send_set_bus_gain(bus_name: String, db: float) -> bool:
	return _send_to_current_session("gool:set_bus_gain", [bus_name, db])


# v0.27.0: per-bus mute / solo / effect-bypass send helpers.
# Mirror the send_set_bus_gain pattern.
func send_set_bus_mute(bus_name: String, muted: bool) -> bool:
	return _send_to_current_session("gool:set_bus_mute", [bus_name, muted])


func send_set_bus_solo(bus_name: String, soloed: bool) -> bool:
	return _send_to_current_session("gool:set_bus_solo", [bus_name, soloed])


func send_set_bus_bypass(bus_name: String, bypassed: bool) -> bool:
	return _send_to_current_session("gool:set_bus_bypass", [bus_name, bypassed])


# v0.28.0 (Phase 3.3c-1): live effect parameter edit. Mirrors the
# v0.27.0 send_set_bus_* helpers. The 3.3c-2 dock UI (effect-edit
# panel) will call this when a slider value changes. Data layout
# matches the game-side _handle_set_effect_parameter expects.
func send_set_effect_parameter(bus_name: String, effect_index: int,
		param_id: int, value: float) -> bool:
	return _send_to_current_session(
			"gool:set_effect_parameter",
			[bus_name, effect_index, param_id, value])


# Common helper. Looks up the current session, sends the message.
# Defensive against session lifecycle races: if _current_session_id
# was set but the session is gone, get_session returns null and we
# silently drop.
func _send_to_current_session(msg: String, data: Array) -> bool:
	if _current_session_id < 0:
		return false
	var session := get_session(_current_session_id)
	if session == null:
		return false
	session.send_message(msg, data)
	return true
