# addons/gool/prefabs/networked_audio_event.gd
#
# Bridge between game code and Godot's MultiplayerAPI for one-shot
# audio events. Encapsulates three replication patterns:
#
#   1. SERVER_AUTHORITATIVE
#      Server fires the event locally and RPCs all relevant
#      clients. Clients hear it after one network round-trip.
#      Use for: explosions, gunshots, environment events, anything
#      where authority must be the server (anti-cheat, fairness).
#
#   2. CLIENT_PREDICTED
#      Client predicts locally (instant), RPCs the server to
#      validate, server reconciles. On rejection, client cancels
#      the predicted sound with a quick fade. On approval, server
#      RPCs OTHER clients (not the predictor, who already heard it).
#      Use for: weapon fire, ability casts — anything where client
#      latency would be unacceptable but server still owns truth.
#
#   3. CLIENT_AUTHORITATIVE
#      Client fires locally and RPCs other clients directly. No
#      server validation. Cheap, low-latency; vulnerable to clients
#      sending junk events. Use for: footsteps, locomotion sounds,
#      cosmetic effects — things where the cheating cost is low.
#
# All three use the same play() entry point; the `mode` property
# selects the pattern.
#
# Peer relevancy is filtered through an AudioRelevancyFilter the
# host attaches at runtime. See examples/quickstart for the wiring.

@tool
class_name NetworkedAudioEvent
extends Node

enum Mode {
	SERVER_AUTHORITATIVE,
	CLIENT_PREDICTED,
	CLIENT_AUTHORITATIVE,
}

@export var mode: Mode = Mode.SERVER_AUTHORITATIVE

## Default audible radius for distance culling (meters). Per-event
## override is passed to play() if non-zero.
@export_range(1.0, 1000.0, 1.0, "suffix:m") var default_audible_radius: float = 50.0

## Default priority for scheduled events. Higher priorities survive
## culling under load. 0=lowest, 64=low, 128=normal, 192=high, 255=critical.
@export_range(0, 255, 1) var default_priority: int = 128

## When true, the event is suppressed if it arrives later than
## `late_threshold_ms` after its sim tick. Lets a fast-moving game
## drop "old" events instead of playing them retroactively.
@export var late_event_dropping: bool = true
@export_range(50.0, 5000.0, 10.0, "suffix:ms") var late_threshold_ms: float = 200.0

signal predicted_locally(prediction_id: int, sound_name: String)
signal prediction_rejected(prediction_id: int)
signal received_remote(sound_name: String, position: Vector3, peer_id: int)

var relevancy_filter: AudioRelevancyFilter = null
var _runtime: Node = null
var _outstanding_predictions: Dictionary = {}    # prediction_id -> sound_name

func _ready() -> void:
	if Engine.is_editor_hint():
		return
	_runtime = get_node_or_null("/root/Gool")
	if _runtime == null:
		push_warning("NetworkedAudioEvent: /root/Gool autoload not found. The gool plugin is installed but not enabled. Fix: open Project Settings → Plugins, find 'gool' in the list, tick the Enable checkbox. (If gool is not in the list, the addon folder is missing — see https://github.com/siliconight/gool for install instructions.)")

# ---- Public API ----

## Trigger this event. Behavior depends on `mode`:
##   - SERVER_AUTHORITATIVE: must be called on the server. The
##     server plays it locally and RPCs all relevant clients.
##   - CLIENT_PREDICTED: must be called on a client. Plays locally
##     immediately, asks the server to validate. Returns the
##     prediction_id so the caller can cancel later if needed.
##   - CLIENT_AUTHORITATIVE: can be called on any peer. Plays
##     locally and RPCs all relevant non-self peers.
##
## Returns the prediction_id (or 0 if no prediction is in flight).
func play(sound_name: String, position: Vector3,
			audible_radius: float = -1.0,
			priority: int = -1,
			team: int = 0) -> int:
	if _runtime == null or not _runtime.is_initialized():
		return 0
	if priority < 0:
		priority = default_priority
	var radius := audible_radius if audible_radius >= 0.0 else default_audible_radius
	match mode:
		Mode.SERVER_AUTHORITATIVE:
			return _play_server_authoritative(sound_name, position, radius,
											   priority, team)
		Mode.CLIENT_PREDICTED:
			return _play_client_predicted(sound_name, position, radius,
											priority, team)
		Mode.CLIENT_AUTHORITATIVE:
			return _play_client_authoritative(sound_name, position, radius,
												priority, team)
	return 0

## Cancel an in-flight predicted event (server rejected it).
func cancel(prediction_id: int, fade_out_ms: float = 50.0) -> void:
	if _runtime == null:
		return
	_runtime.cancel_predicted_event(prediction_id, fade_out_ms)
	_outstanding_predictions.erase(prediction_id)
	prediction_rejected.emit(prediction_id)

# ---- SERVER_AUTHORITATIVE ----

func _play_server_authoritative(sound_name: String, position: Vector3,
								  radius: float, priority: int,
								  team: int) -> int:
	if not _is_server():
		push_warning("NetworkedAudioEvent (server-auth): play() called on non-server peer; suppressed")
		return 0
	var sim_tick := _current_simulation_tick()
	var t_ms := Time.get_ticks_msec()
	# Server hears it locally.
	_runtime.submit_replicated_event(sound_name, position, sim_tick, t_ms, priority)
	# Server RPCs to relevant clients (excluding itself).
	var targets := _filter_targets(position, radius, team, multiplayer.get_unique_id())
	for pid in targets:
		rpc_id(pid, "_remote_play", sound_name, position, sim_tick, t_ms, priority)
	return 0

# ---- CLIENT_PREDICTED ----

func _play_client_predicted(sound_name: String, position: Vector3,
							  radius: float, priority: int,
							  team: int) -> int:
	if _is_server():
		# Server can't predict for itself; degrade to authoritative.
		return _play_server_authoritative(sound_name, position, radius,
											priority, team)
	var pid: int = _runtime.make_prediction_id()
	_outstanding_predictions[pid] = sound_name
	var t_ms := Time.get_ticks_msec()
	# Client hears it immediately.
	_runtime.submit_event_local(sound_name, position, pid, priority, t_ms)
	predicted_locally.emit(pid, sound_name)
	# Ask the server to validate.
	rpc_id(1, "_server_validate_prediction", sound_name, position, pid,
			radius, priority, team)
	return pid

@rpc("any_peer", "reliable", "call_remote")
func _server_validate_prediction(sound_name: String, position: Vector3,
									prediction_id: int,
									radius: float, priority: int,
									team: int) -> void:
	# Runs on server. The host's higher-level code decides whether
	# this prediction is allowed (e.g. by checking ammo/cooldowns/
	# game rules). For the prefab default, we accept everything —
	# subclass NetworkedAudioEvent and override
	# _on_validate_prediction() to add your validation logic.
	if not _is_server():
		return
	var requesting_peer := multiplayer.get_remote_sender_id()
	if not _on_validate_prediction(sound_name, position, requesting_peer):
		rpc_id(requesting_peer, "_remote_reject_prediction", prediction_id)
		return
	var sim_tick := _current_simulation_tick()
	var t_ms := Time.get_ticks_msec()
	# Server hears it.
	_runtime.submit_replicated_event(sound_name, position, sim_tick, t_ms, priority)
	# RPC to all relevant clients EXCEPT the predictor (who already heard it).
	var targets := _filter_targets(position, radius, team, requesting_peer)
	for pid in targets:
		rpc_id(pid, "_remote_play", sound_name, position, sim_tick, t_ms, priority)

@rpc("authority", "reliable", "call_remote")
func _remote_reject_prediction(prediction_id: int) -> void:
	var sound_name: String = _outstanding_predictions.get(prediction_id, "")
	cancel(prediction_id)
	if sound_name != "":
		push_warning("NetworkedAudioEvent: server rejected prediction "
					  + str(prediction_id) + " for '" + sound_name + "'")

## Override this in a subclass (or set an external validator
## callable) to add your server-side prediction validation logic.
## Default accepts everything.
func _on_validate_prediction(sound_name: String, position: Vector3,
								requesting_peer: int) -> bool:
	return true

# ---- CLIENT_AUTHORITATIVE ----

func _play_client_authoritative(sound_name: String, position: Vector3,
								  radius: float, priority: int,
								  team: int) -> int:
	var t_ms := Time.get_ticks_msec()
	# Local play.
	_runtime.submit_event_local(sound_name, position, 0, priority, t_ms)
	# RPC to other peers in audible range.
	var targets := _filter_targets(position, radius, team, multiplayer.get_unique_id())
	for pid in targets:
		rpc_id(pid, "_remote_play", sound_name, position,
				_current_simulation_tick(), t_ms, priority)
	return 0

# ---- Remote receive ----

@rpc("any_peer", "reliable", "call_remote")
func _remote_play(sound_name: String, position: Vector3,
					simulation_tick: int, server_time_ms: int,
					priority: int) -> void:
	if _runtime == null:
		return
	# Drop late events if configured.
	if late_event_dropping:
		var age := Time.get_ticks_msec() - server_time_ms
		if age > int(late_threshold_ms):
			return
	_runtime.submit_replicated_event(sound_name, position,
									   simulation_tick, server_time_ms,
									   priority)
	received_remote.emit(sound_name, position, multiplayer.get_remote_sender_id())

# ---- Helpers ----

func _is_server() -> bool:
	return multiplayer.has_multiplayer_peer() and multiplayer.is_server()

func _current_simulation_tick() -> int:
	# The host's tick clock. We expose a simple wall-clock-derived
	# default; for deterministic replay, set this from the host's
	# simulation tick via runtime_singleton.gd's on_tick_advanced().
	return Time.get_ticks_msec() / 16     # ~60Hz tick

func _filter_targets(position: Vector3, radius: float,
					   team: int, exclude_peer: int) -> PackedInt32Array:
	if relevancy_filter != null:
		return relevancy_filter.filter(position, radius, team, exclude_peer)
	# No filter installed: send to all peers (minus exclude).
	var out := PackedInt32Array()
	if multiplayer.has_multiplayer_peer():
		for pid in multiplayer.get_peers():
			if pid != exclude_peer:
				out.push_back(pid)
	return out
