# addons/gool/multiplayer_bridge.gd
#
# Autoload — registered by plugin.gd alongside Gool and
# DialogueDirector. Wires Godot's MultiplayerAPI (or any custom
# transport you plug in) to gool's existing replication primitives
# (submit_event_local, submit_replicated_event, on_tick_advanced,
# register_voice_source, etc).
#
# Design constraint: TRANSPORT-AGNOSTIC.
#
# The network backend may land on Steam P2P, ENet, raw UDP, a custom
# protocol, or a hybrid of these. The bridge defaults to MultiplayerAPI
# but every send / receive can be redirected through signals so a
# custom transport plugs in without changing the high-level API a game
# script calls.
#
# Three API tiers, each independently overridable:
#
# 1. HIGH-LEVEL  (what game code calls):
#       fire_predicted_event(sound_name, position, priority) -> prediction_id
#       update_replicated_transform_networked(handle, transform, tick)
#       advance_tick(simulation_tick, server_time_ms)
#       send_voice_packet_to_peer(peer_id, bytes, sequence, send_ts_ms)
#       capture_network_snapshot() / apply_network_snapshot(snap)
#
# 2. LOW-LEVEL HOOKS  (for custom transports):
#       Signals the bridge emits when it wants to send a packet:
#           send_replicated_event       (payload: Dictionary)
#           send_replicated_transform   (payload: Dictionary)
#           send_voice_packet           (payload: Dictionary)
#       Public methods custom transports call when a packet arrives:
#           receive_replicated_event(...)
#           receive_replicated_transform(...)
#           receive_voice_packet(...)
#
# 3. AUTO BEHAVIORS  (each opt-out-able):
#       transport_mode  ∈ AUTO | MULTIPLAYER_API | CUSTOM | DISABLED
#       auto_register_voice_sources  (default true)
#       route_voice_packets          (default true — disable for Steam)
#       staleness_threshold_ms       (default 250 — drop stale events)
#
# Relationship to the existing Gool.play_networked():
#
# play_networked is the "one-call simple SFX" path, kept for backward
# compatibility and as the simplest possible networked-sound recipe.
# Bridge's fire_predicted_event is the "predicted event with a
# returned prediction_id you can cancel later" path — the right
# choice for client-predicted gunshots, footsteps, anything where
# the firing client should hear it at 0ms latency and other peers
# get a replicated version. Both layers are local-first; both use
# the same submit_event_local under the hood.
#
# Critical invariant honored throughout: the firing client NEVER
# waits on a round trip before playing its own audio. Local play
# happens before the network call in every send path.

extends Node

# ─── Configuration ─────────────────────────────────────────────

enum TransportMode {
	## Detect: use MultiplayerAPI if a peer is connected, else fall
	## back to local-only. Suitable for projects that use Godot's
	## standard multiplayer (any MultiplayerPeer subclass — ENet,
	## WebRTC, SteamMultiplayerPeer, etc).
	AUTO = 0,

	## Force MultiplayerAPI. Use when you want to require MultiplayerAPI
	## to be set up (errors if no peer connected) — typically inside
	## a "match started" state where networking is guaranteed.
	MULTIPLAYER_API = 1,

	## All sends go through signals (send_replicated_event etc.) for
	## a custom transport to dispatch. All receives come through the
	## receive_* public methods, which your transport code calls
	## directly when a packet arrives.
	CUSTOM = 2,

	## Pure local — disables all network routing. Single-player mode,
	## tutorials, replay scrubbing, headless test runs. Bridge still
	## plays events locally via submit_event_local but never emits
	## a send signal or RPC.
	DISABLED = 3,
}

## Which transport the bridge sends through. See TransportMode enum.
@export var transport_mode: TransportMode = TransportMode.AUTO

## Auto-register voice sources for new peers on connect, and clean up
## on disconnect. Set false if your game code wants to control voice-
## source lifetimes manually (e.g. only register on chat-channel join).
@export var auto_register_voice_sources: bool = true

## Route voice packets through this bridge. Set false when running on
## Steam P2P — the Steamworks SDK handles voice transport natively
## and routing packets twice (once through Steam, once through Godot
## RPC) is wasted bandwidth. The bridge still surfaces VOIP playback
## via Gool.submit_voice_packet on the receiver side; only the SEND
## path is short-circuited.
@export var route_voice_packets: bool = true

## Drop replicated events older than this. Matches Gool.play_networked's
## existing default. Tune up for slow / relayed connections, down for
## time-sensitive sounds (combat audio in competitive modes).
@export var staleness_threshold_ms: int = 250

# ─── Signals ───────────────────────────────────────────────────

## Emitted when a remote peer joins. Bridge has already registered
## the voice source (if auto_register_voice_sources is true) before
## this fires. Use to update HUD player list, spawn the peer's
## avatar, etc.
signal player_joined(peer_id: int)

## Emitted when a remote peer leaves. Bridge has cleaned up voice
## source bookkeeping before this fires. Use to despawn avatars,
## update HUD, cancel in-flight predictions tagged to that peer.
signal player_left(peer_id: int)

## CUSTOM transport hook — emitted when bridge wants to send a
## replicated audio event. payload Dictionary contains:
##   { sound_name, position, simulation_tick, priority, ts_ms }
## Listener (your transport code) routes via whatever it wants.
signal send_replicated_event(payload: Dictionary)

## CUSTOM transport hook — emitted when bridge wants to send a
## replicated transform update. payload Dictionary contains:
##   { handle, position, forward, velocity, simulation_tick }
signal send_replicated_transform(payload: Dictionary)

## CUSTOM transport hook — emitted when bridge wants to send a voice
## packet. payload Dictionary contains:
##   { peer_id, bytes, sequence_number, send_timestamp_ms }
signal send_voice_packet(payload: Dictionary)

# ─── Internal state ────────────────────────────────────────────

# Current simulation tick — set by advance_tick. The bridge stamps
# outbound events with this and forwards inbound events to the
# autoload's on_tick_advanced.
var _current_simulation_tick: int = 0
var _last_server_time_ms: int = 0

# ─── Lifecycle ─────────────────────────────────────────────────

func _ready() -> void:
	# Wire to MultiplayerAPI's peer signals. We connect unconditionally
	# at autoload time and let the transport_mode decide whether to
	# act on them. This way, switching modes at runtime doesn't
	# require re-wiring.
	if multiplayer != null:
		if not multiplayer.peer_connected.is_connected(_on_peer_connected):
			multiplayer.peer_connected.connect(_on_peer_connected)
		if not multiplayer.peer_disconnected.is_connected(_on_peer_disconnected):
			multiplayer.peer_disconnected.connect(_on_peer_disconnected)

func _on_peer_connected(peer_id: int) -> void:
	if transport_mode == TransportMode.DISABLED:
		return
	if auto_register_voice_sources:
		Gool.register_voice_source(peer_id)
	player_joined.emit(peer_id)

func _on_peer_disconnected(peer_id: int) -> void:
	if transport_mode == TransportMode.DISABLED:
		return
	# Bridge doesn't currently unregister voice sources on the C++
	# side (no unregister binding exists yet). The autoload's
	# _known_voice_player_ids list keeps it but the player is gone
	# from the engine's perspective once submit_voice_packet stops
	# receiving for them. Emit the signal so game code can do its
	# own cleanup.
	player_left.emit(peer_id)

# ─── HIGH-LEVEL API (what game code calls) ─────────────────────

## Advance the simulation tick. Game code calls this once per
## network tick (per the 20–30 Hz target in the network arch doc).
## Bridge forwards to Gool.on_tick_advanced and caches the tick
## for stamping outbound events.
##
## On the network lead's listen-server architecture, the LISTEN
## SERVER calls this and broadcasts tick advance to clients (which
## also call advance_tick locally to keep gool's per-peer event
## scheduler aligned).
func advance_tick(simulation_tick: int, server_time_ms: int) -> void:
	_current_simulation_tick = simulation_tick
	_last_server_time_ms = server_time_ms
	Gool.on_tick_advanced(simulation_tick, server_time_ms)

## Fire a client-predicted audio event. Local play happens IMMEDIATELY
## (0ms latency — honors the network lead's hard requirement). The
## event is then replicated to other peers via the active transport.
##
## Returns a prediction_id you can later pass to
## Gool.cancel_predicted_event if the prediction is invalidated
## (e.g. server says the shot missed).
##
## Typical use — firing client's gun-fire handler:
##   var pid := MultiplayerBridge.fire_predicted_event(
##           "gunshot_pistol", muzzle_pos, 200)
##   _pending_predictions[pid] = ...   # track for later cancel
func fire_predicted_event(sound_name: String, position: Vector3,
		priority: int = 128) -> int:
	# Local play FIRST — never block on network for own audio.
	var prediction_id: int = Gool.make_prediction_id()
	var ts_ms: int = Time.get_ticks_msec()
	Gool.submit_event_local(sound_name, position, prediction_id,
			priority, ts_ms)

	if transport_mode == TransportMode.DISABLED:
		return prediction_id

	# Then replicate.
	var payload: Dictionary = {
		"sound_name": sound_name,
		"position": position,
		"simulation_tick": _current_simulation_tick,
		"priority": priority,
		"ts_ms": ts_ms,
	}
	_dispatch_send_replicated_event(payload)
	return prediction_id

## Replicate a transform update for a networked emitter. Bridge
## applies it locally + replicates to other peers.
##
## Replication rate is the caller's choice — typical is 10 Hz for
## moving emitters per the network lead's "release 2 or 3 decision."
## Bridge does not throttle internally; that's the caller's call.
func update_replicated_transform_networked(handle: int,
		position: Vector3, forward: Vector3, velocity: Vector3) -> void:
	Gool.update_replicated_transform(handle, position, forward, velocity,
			_current_simulation_tick)
	if transport_mode == TransportMode.DISABLED:
		return
	var payload: Dictionary = {
		"handle": handle,
		"position": position,
		"forward": forward,
		"velocity": velocity,
		"simulation_tick": _current_simulation_tick,
	}
	_dispatch_send_replicated_transform(payload)

## Submit a voice packet for transmission. On Steam (route_voice_packets
## = false) this is a no-op — Steamworks handles voice transport. On
## ENet / custom transports the bridge routes via the active transport.
##
## Receivers should call receive_voice_packet() when packets arrive
## on their side; the bridge will forward to Gool.submit_voice_packet.
func send_voice_packet_to_peer(peer_id: int, bytes: PackedByteArray,
		sequence_number: int, send_timestamp_ms: int) -> void:
	if transport_mode == TransportMode.DISABLED:
		return
	if not route_voice_packets:
		return  # Steam — let the SDK handle it
	var payload: Dictionary = {
		"peer_id": peer_id,
		"bytes": bytes,
		"sequence_number": sequence_number,
		"send_timestamp_ms": send_timestamp_ms,
	}
	_dispatch_send_voice_packet(payload)

## Capture a host-migration sync snapshot. Network code calls this on
## the current host at the configured sync interval (default ~1 Hz per
## the network arch doc). The resulting GoolNetworkSnapshot is what
## ships in the sync state packet.
##
## buses: which buses to capture mix state for. Pass an empty array
##   to skip mix snapshot capture (e.g. if your game doesn't need to
##   preserve fader positions across migration).
func capture_network_snapshot(buses: PackedStringArray
		= PackedStringArray()) -> GoolNetworkSnapshot:
	var snap := GoolNetworkSnapshot.new()
	snap.simulation_tick = _current_simulation_tick
	snap.server_time_ms = _last_server_time_ms
	var ids: Array = Gool.get_known_voice_player_ids()
	var pid_array := PackedInt32Array()
	for pid in ids:
		pid_array.append(int(pid))
	snap.voice_player_ids = pid_array
	if buses.size() > 0:
		snap.mix_snapshot = Gool.capture_mix_snapshot(buses)
	# Music state name: bridge doesn't track MusicStateController
	# directly (it's a separate prefab). Game code can populate
	# snap.music_state_name before sending if it cares.
	return snap

## Apply a host-migration sync snapshot. Network code calls this on
## the NEW host after migration. Restores tick alignment, re-registers
## voice sources, and reapplies the mix snapshot.
func apply_network_snapshot(snap: GoolNetworkSnapshot) -> void:
	if snap == null:
		return
	# Restore tick alignment — engine event scheduler keys off this.
	advance_tick(snap.simulation_tick, snap.server_time_ms)
	# Re-register voice sources the old host knew about.
	for pid in snap.voice_player_ids:
		Gool.register_voice_source(int(pid))
	# Reapply mix snapshot if one was captured.
	if snap.mix_snapshot != null:
		Gool.apply_mix_snapshot(snap.mix_snapshot)
	# Music state is the caller's responsibility — bridge doesn't
	# reach into MusicStateController.

# ─── LOW-LEVEL receive hooks (custom transports call these) ────

## Custom transport code calls this when a replicated event packet
## arrives from another peer. Bridge applies staleness check + forwards
## to Gool.submit_replicated_event. Public so non-MultiplayerAPI
## transports can inject inbound events directly.
func receive_replicated_event(sound_name: String, position: Vector3,
		simulation_tick: int, priority: int, ts_ms: int) -> void:
	var now_ms: int = Time.get_ticks_msec()
	if now_ms - ts_ms > staleness_threshold_ms:
		return   # stale — drop per category convention
	Gool.submit_replicated_event(sound_name, position, simulation_tick,
			ts_ms, priority)

## Custom transport code calls this when a replicated transform
## update arrives. Bridge forwards to Gool.update_replicated_transform.
func receive_replicated_transform(handle: int, position: Vector3,
		forward: Vector3, velocity: Vector3,
		simulation_tick: int) -> void:
	Gool.update_replicated_transform(handle, position, forward, velocity,
			simulation_tick)

## Custom transport code calls this when a voice packet arrives.
## Bridge forwards to Gool.submit_voice_packet (which feeds the
## jitter buffer + decodes + plays).
func receive_voice_packet(peer_id: int, bytes: PackedByteArray,
		sequence_number: int, send_timestamp_ms: int,
		arrival_timestamp_ms: int = -1) -> void:
	Gool.submit_voice_packet(peer_id, bytes, sequence_number,
			send_timestamp_ms, arrival_timestamp_ms)

# ─── Internal: dispatch to active transport ────────────────────

func _resolved_transport_mode() -> TransportMode:
	if transport_mode == TransportMode.AUTO:
		if multiplayer != null \
				and multiplayer.has_multiplayer_peer() \
				and multiplayer.get_multiplayer_peer().get_connection_status() \
					== MultiplayerPeer.CONNECTION_CONNECTED:
			return TransportMode.MULTIPLAYER_API
		return TransportMode.DISABLED
	return transport_mode

func _dispatch_send_replicated_event(payload: Dictionary) -> void:
	match _resolved_transport_mode():
		TransportMode.MULTIPLAYER_API:
			_rpc_replicated_event.rpc(
				payload.sound_name, payload.position,
				payload.simulation_tick, payload.priority, payload.ts_ms)
		TransportMode.CUSTOM:
			send_replicated_event.emit(payload)
		_:
			pass  # DISABLED — local play was already done in fire_predicted_event

func _dispatch_send_replicated_transform(payload: Dictionary) -> void:
	match _resolved_transport_mode():
		TransportMode.MULTIPLAYER_API:
			_rpc_replicated_transform.rpc(
				payload.handle, payload.position, payload.forward,
				payload.velocity, payload.simulation_tick)
		TransportMode.CUSTOM:
			send_replicated_transform.emit(payload)
		_:
			pass

func _dispatch_send_voice_packet(payload: Dictionary) -> void:
	match _resolved_transport_mode():
		TransportMode.MULTIPLAYER_API:
			_rpc_voice_packet.rpc(
				payload.peer_id, payload.bytes,
				payload.sequence_number, payload.send_timestamp_ms)
		TransportMode.CUSTOM:
			send_voice_packet.emit(payload)
		_:
			pass

# ─── MultiplayerAPI RPC endpoints ──────────────────────────────
#
# These fire on EVERY peer except the sender. Each forwards to the
# corresponding receive_* method, which applies staleness checks
# (where relevant) and dispatches to gool.
#
# Reliability tier per RPC:
#   replicated_event:     unreliable — drop-if-late audio per
#                          play_networked's existing convention
#   replicated_transform: unreliable_ordered — periodic state, only
#                          newest matters
#   voice_packet:         unreliable — VOIP handles its own ordering
#                          via sequence_number + jitter buffer

@rpc("any_peer", "call_remote", "unreliable")
func _rpc_replicated_event(sound_name: String, position: Vector3,
		simulation_tick: int, priority: int, ts_ms: int) -> void:
	receive_replicated_event(sound_name, position, simulation_tick,
			priority, ts_ms)

@rpc("any_peer", "call_remote", "unreliable_ordered")
func _rpc_replicated_transform(handle: int, position: Vector3,
		forward: Vector3, velocity: Vector3,
		simulation_tick: int) -> void:
	receive_replicated_transform(handle, position, forward, velocity,
			simulation_tick)

@rpc("any_peer", "call_remote", "unreliable")
func _rpc_voice_packet(peer_id: int, bytes: PackedByteArray,
		sequence_number: int, send_timestamp_ms: int) -> void:
	# arrival_timestamp_ms = current local time, populated here so the
	# jitter buffer math runs against the receiving clock.
	receive_voice_packet(peer_id, bytes, sequence_number,
			send_timestamp_ms, Time.get_ticks_msec())
