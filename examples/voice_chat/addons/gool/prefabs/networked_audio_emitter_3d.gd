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

# addons/gool/prefabs/networked_audio_emitter_3d.gd
#
## Persistent 3D audio emitter that replicates its transform across
## peers. Built on top of AudioEmitter3D's local-playback semantics,
## adds:
#
##   - Multiplayer authority: one peer (typically the server, or the
##     player who owns the entity) is authoritative for transform.
##   - Network-rate transform broadcast: authority RPCs the
##     transform every `network_tick_ms`; receivers update the
##     engine's emitter via update_replicated_transform.
##   - Receiver interpolation: between received transforms,
##     receivers smoothly interpolate position so movement isn't
##     stairstepped at 30/60Hz tick rate.
##   - Distance culling: transform updates are only RPC'd to peers
##     within audible range (via the relevancy filter, if set).
#
## Use this for moving spatial sources owned by one peer that other
## peers should hear: a vehicle's engine, a moving turret, an
## enemy's footsteps. For one-shot events, use NetworkedAudioEvent.
## For purely local sounds (UI, local listener music), use
## AudioEmitter3D directly.

@tool
class_name NetworkedAudioEmitter3D
extends Node3D

## Sound to play. Must be registered.
@export var sound_name: String = ""

## Whether the sound loops.
@export var looping: bool = true

## Crossfade length when a looping sound restarts from the end.
## 0 = hard restart at the loop point. A few tens of ms eliminates
## the click some loops have at the boundary.
@export_range(0.0, 500.0, 1.0, "suffix:ms") var loop_crossfade_ms: float = 0.0

## Fade-in when the emitter is created on receivers.
@export_range(0.0, 5000.0, 1.0, "suffix:ms") var fade_in_ms: float = 100.0

## Fade-out when the emitter is destroyed on receivers.
@export_range(0.0, 5000.0, 1.0, "suffix:ms") var fade_out_ms: float = 100.0

## Distance attenuation. Below `min_distance` the sound plays at full
## volume; between `min_distance` and `max_distance` it falls off;
## past `max_distance` it's culled.
@export_range(0.0, 1000.0, 0.1, "suffix:m") var min_distance: float = 1.0

## Past this distance from the listener the sound is fully culled
## (no audio rendered, saves CPU on far-away events).
@export_range(1.0, 10000.0, 0.1, "suffix:m") var max_distance: float = 50.0

## Network-tick interval for transform broadcasting (ms). The
## authority sends an update every N ms to all relevant peers.
## Lower = smoother but more bandwidth.
@export_range(16.0, 1000.0, 1.0, "suffix:ms") var network_tick_ms: float = 50.0

## Audible radius for distance-based receiver culling.
@export_range(1.0, 1000.0, 1.0, "suffix:m") var audible_radius: float = 50.0

## When true, receivers smoothly interpolate position between
## network ticks instead of snapping. Adds ~one-tick of latency
## but eliminates stairstep movement.
@export var interpolate_received: bool = true

## Auto-create the emitter on _ready.
@export var autoplay: bool = true

signal emitter_created
signal emitter_destroyed

var relevancy_filter: AudioRelevancyFilter = null

var _runtime: Node = null
var _handle: int = 0
var _broadcast_timer: float = 0.0

# Receiver-side interpolation state.
var _last_received_position: Vector3 = Vector3.ZERO
var _previous_received_position: Vector3 = Vector3.ZERO
var _last_received_velocity: Vector3 = Vector3.ZERO
var _last_received_tick: int = 0
var _last_received_local_time: float = 0.0

func _ready() -> void:
	if Engine.is_editor_hint():
		return
	_runtime = get_node_or_null("/root/Gool")
	if _runtime == null:
		push_warning("NetworkedAudioEmitter3D: /root/Gool autoload not found. The gool plugin is installed but not enabled. Fix: open Project Settings → Plugins, find 'gool' in the list, tick the Enable checkbox. (If gool is not in the list, the addon folder is missing — see https://github.com/siliconight/gool for install instructions.)")
		return
	if not _runtime.is_initialized():
		await _runtime.ready_to_play
	_runtime.register_sound_definition(sound_name, true, looping,
										  min_distance, max_distance,
										  loop_crossfade_ms)
	if autoplay:
		play()

# ---- Public API ----

func play() -> void:
	if _runtime == null or sound_name == "":
		return
	if _handle != 0:
		return
	_handle = _runtime.create_emitter(
		sound_name, global_transform.origin, looping, fade_in_ms)
	if _handle != 0:
		emitter_created.emit()

func stop() -> void:
	if _handle != 0 and _runtime != null:
		_runtime.destroy_emitter(_handle, fade_out_ms)
		_handle = 0
		await get_tree().create_timer(fade_out_ms / 1000.0).timeout
		emitter_destroyed.emit()

# ---- Process loop ----

func _process(delta: float) -> void:
	if _runtime == null or _handle == 0:
		return

	if _is_authority():
		# Update local engine state immediately every frame so the
		# authority hears its own emitter without latency.
		var fwd := -global_transform.basis.z
		_runtime.set_emitter_transform(
			_handle, global_transform.origin, fwd, _estimate_velocity(delta))
		# Broadcast at network tick rate to other peers.
		_broadcast_timer += delta * 1000.0
		if _broadcast_timer >= network_tick_ms:
			_broadcast_timer = 0.0
			_broadcast_transform()
	else:
		# Non-authority: interpolate between last two received transforms.
		if interpolate_received and _last_received_tick > 0:
			var local_dt := (Time.get_ticks_msec() / 1000.0) - _last_received_local_time
			var t := clamp(local_dt / (network_tick_ms / 1000.0), 0.0, 1.5)
			var interpolated := _previous_received_position.lerp(
				_last_received_position, t)
			_runtime.set_emitter_transform(
				_handle, interpolated, Vector3.FORWARD, _last_received_velocity)
			global_transform.origin = interpolated

func _exit_tree() -> void:
	if _runtime != null and _handle != 0:
		_runtime.destroy_emitter(_handle, fade_out_ms)
		_handle = 0

# ---- Authority-side broadcast ----

func _broadcast_transform() -> void:
	if not multiplayer.has_multiplayer_peer():
		return
	var pos := global_transform.origin
	var fwd := -global_transform.basis.z
	var vel := _estimate_velocity(network_tick_ms / 1000.0)
	var sim_tick := Time.get_ticks_msec() / 16
	var targets := _filter_targets(pos)
	for pid in targets:
		rpc_id(pid, "_receive_transform", pos, fwd, vel, sim_tick)

@rpc("authority", "unreliable_ordered", "call_remote")
func _receive_transform(position: Vector3, forward: Vector3,
						  velocity: Vector3, simulation_tick: int) -> void:
	if _runtime == null or _handle == 0:
		return
	# Save for interpolation; the actual engine update happens in
	# _process to keep the rate consistent with display.
	_previous_received_position = _last_received_position
	_last_received_position = position
	_last_received_velocity = velocity
	_last_received_tick = simulation_tick
	_last_received_local_time = Time.get_ticks_msec() / 1000.0
	if not interpolate_received:
		_runtime.update_replicated_transform(_handle, position, forward,
												velocity, simulation_tick)
		global_transform.origin = position

# ---- Helpers ----

func _is_authority() -> bool:
	if not multiplayer.has_multiplayer_peer():
		return true   # singleplayer / offline: this peer is authority
	return is_multiplayer_authority()

var _last_position_for_velocity: Vector3 = Vector3.ZERO
var _has_velocity_baseline: bool = false

func _estimate_velocity(dt: float) -> Vector3:
	if dt < 0.001:
		return Vector3.ZERO
	var current := global_transform.origin
	if not _has_velocity_baseline:
		_last_position_for_velocity = current
		_has_velocity_baseline = true
		return Vector3.ZERO
	var v := (current - _last_position_for_velocity) / dt
	_last_position_for_velocity = current
	return v

func _filter_targets(position: Vector3) -> PackedInt32Array:
	if relevancy_filter != null:
		return relevancy_filter.filter(position, audible_radius, 0,
										  multiplayer.get_unique_id())
	var out := PackedInt32Array()
	if multiplayer.has_multiplayer_peer():
		for pid in multiplayer.get_peers():
			out.push_back(pid)
	return out
