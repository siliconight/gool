@tool
class_name GoolDebuggerPlugin
extends EditorDebuggerPlugin

# v0.25.0: cross-process bridge for the mixer dock.
#
# Godot 4 runs the editor and the running game in separate
# processes. When F5 is pressed, the editor spawns a child game
# process with the debugger attached. Anything the dock wants to
# display has to traverse that process boundary — there's no
# shared memory, no shared SceneTree, no `Gool.get_bus_stats()`
# call that would just work editor-side.
#
# Godot's solution is the EngineDebugger message channel: the
# game calls `EngineDebugger.send_message("gool:bus_stats",
# [payload])`, and an editor-side EditorDebuggerPlugin can
# subscribe to messages whose prefix it claims (here, "gool").
# The plugin caches the latest payload; the mixer dock polls
# the plugin every editor frame.
#
# This same channel is what 3.3b/c/d will use for editor→game
# direction (faders, S/M/B, effect param edits): we'll add a
# matching outbound `send_message("gool:command_X", [...])`
# from the dock, and the game-side runtime will listen for it.
# Building this scaffolding now means no rip-and-replace later.

# Latest bus stats from the game, keyed by debugger session_id
# so multi-session debugging (rare but possible) doesn't crash.
# In single-session mode (the common case) there's just one entry.
var _latest_stats_by_session: Dictionary = {}

# Track which sessions are still active. When a session stops we
# drop its cached stats so the dock doesn't render stale data
# after F8 (stop) is pressed.
var _active_sessions: Dictionary = {}


# Called by the editor when a debug session starts. We register
# message handlers here. Override _capture below handles the
# actual messages.
func _setup_session(session_id: int) -> void:
	_active_sessions[session_id] = true
	# Listen for the session stopping so we can clear stats.
	var session := get_session(session_id)
	if session != null:
		# Godot 4: EditorDebuggerSession has started/stopped signals.
		# When stopped, drop this session's cached stats so the dock
		# immediately falls back to the empty state.
		if not session.stopped.is_connected(_on_session_stopped):
			session.stopped.connect(_on_session_stopped.bind(session_id))


# Tells the editor which message-prefix this plugin claims. Godot
# routes messages of the form "<prefix>:<rest>" to plugins whose
# _has_capture returns true for "<prefix>". We claim "gool".
func _has_capture(prefix: String) -> bool:
	return prefix == "gool"


# Called for every "gool:*" message arriving from a running game.
# Return true if we handled the message; false lets other plugins
# try (we shouldn't conflict with anything since "gool" is
# project-specific).
func _capture(message: String, data: Array, session_id: int) -> bool:
	if message == "gool:bus_stats":
		# Payload is data[0], which is the Array of bus dicts
		# the game sent via send_message("gool:bus_stats", [stats]).
		# Godot wraps the sent-payload in another Array, hence [0].
		if data.size() >= 1 and data[0] is Array:
			_latest_stats_by_session[session_id] = data[0]
		return true
	return false


# Returns the most recent bus stats from any active session. The
# common case is a single F5 session, so this is just whichever
# session's data we last received. Returns empty Array when no
# active session has reported stats yet.
func get_latest_bus_stats() -> Array:
	# Prefer the most recently active session. With one session
	# this is trivially deterministic.
	for session_id in _latest_stats_by_session:
		if _active_sessions.get(session_id, false):
			return _latest_stats_by_session[session_id]
	return []


func _on_session_stopped(session_id: int) -> void:
	_active_sessions.erase(session_id)
	_latest_stats_by_session.erase(session_id)
