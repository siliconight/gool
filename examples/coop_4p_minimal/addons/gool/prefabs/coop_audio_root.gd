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

# addons/gool/prefabs/coop_audio_root.gd
#
## Drop-in coordinator for small-party coop audio (typically 2-8 peers,
## tuned for 4). Composes the three primitives a coop game always
## needs into one Node:
#
##   1. GoolSoundBankLoader — registers your sounds on _ready.
##   2. NetworkedAudioEvent  — broadcasts one-shot events over Godot's
##                             MultiplayerAPI with one of three
##                             replication modes (server-authoritative,
##                             client-predicted, or client-authoritative).
##   3. GoolListener3D       — attached to the local player so they
##                             hear the world from their character's
##                             position.
#
## WHY THIS PREFAB EXISTS
## =====================
#
## Without it, the minimum wiring for 4-player RPC event audio is:
##   - Add Gool autoload (one-time setup, handled by the editor plugin)
##   - Add GoolSoundBankLoader, assign a SoundBank
##   - Add NetworkedAudioEvent, pick a mode, configure radius/priority
##   - Add GoolListener3D as a child of your local player's camera
##   - Wire your player's input → call NetworkedAudioEvent.play(...)
##   - Handle the player-spawn-after-scene-load timing (listener has
##     to attach AFTER the player exists)
##
## That's 5 nodes plus glue code per game. The drop-in case — "I want
## 4-player coop with shared audio, replicating events as they happen" —
## is common enough that hand-wiring it every time is friction.
#
## CoopAudioRoot collapses that to:
##   - Add CoopAudioRoot to your scene
##   - Set sound_bank in the inspector
##   - When your local player spawns, call set_local_player($Player)
##   - Trigger events with $CoopAudioRoot.play_event_3d("gunshot", pos)
#
## WHEN NOT TO USE THIS
## ====================
#
## CoopAudioRoot is opinionated for the small-party event-driven case.
## Use the underlying prefabs directly if:
##
##   - You need fine-grained control over each event channel (different
##     radii, priorities, or replication modes per sound). CoopAudioRoot
##     uses one shared NetworkedAudioEvent with global defaults; per-
##     event overrides are possible but if you find yourself overriding
##     every call, drop down to NetworkedAudioEvent.
##
##   - You're building large-scale multiplayer (16+ peers). At that
##     scale you want per-channel audio relevancy budgets, area-of-
##     interest filtering, and event coalescing — none of which this
##     prefab handles. Use the lower-level primitives + a custom
##     relevancy filter.
##
##   - Your game has persistent moving emitters (vehicle engines,
##     enemy footsteps, turret hums) — those need NetworkedAudioEmitter3D
##     for transform replication, which CoopAudioRoot doesn't manage.
##     The two are complementary: use CoopAudioRoot for one-shots,
##     NetworkedAudioEmitter3D nodes for continuous spatial sources.
#
##   - You need voice chat. That's a separate concern with its own
##     prefab (VoiceChatPlayer). Add VoiceChatPlayer to each player
##     scene; the two prefabs don't conflict.
#
## USAGE
## =====
#
##   1. Add CoopAudioRoot as a child of your main scene root:
##
##         Main (Node3D)
##         ├── CoopAudioRoot                     # this prefab
##         ├── World (Node3D)
##         │   └── ...your level geometry
##         └── Players (Node3D)
##             └── ...player spawns
#
##   2. In the inspector, set:
##         sound_bank          — your GoolSoundBank resource
##         default_event_mode  — SERVER_AUTHORITATIVE (recommended)
##         default_radius      — typical audible range (m)
#
##   3. When your local player spawns, attach the listener:
##
##         func _on_local_player_spawned(player: Node3D) -> void:
##             $CoopAudioRoot.set_local_player(player)
#
##   4. Trigger events from any peer:
##
##         # Positional one-shot — gunshot, explosion, footstep
##         $CoopAudioRoot.play_event_3d("gunshot",
##                                      $Player.global_position)
#
##         # Non-positional — UI sound, music sting, narrator line
##         $CoopAudioRoot.play_event_2d("ui_objective_complete")
##
##   The replication mode (who hears it, who validates it) is set by
##   default_event_mode. To override per-call, pass the mode argument:
##
##         $CoopAudioRoot.play_event_3d("footstep", pos,
##                 NetworkedAudioEvent.Mode.CLIENT_AUTHORITATIVE)
#
## SIGNALS
## =======
#
##   sound_bank_loaded(results: Dictionary)
##       Emitted once after the SoundBank registration completes.
##       `results` is the same Dictionary GoolSoundBankLoader emits
##       (per-sound success/fail). Useful for gating "ready to play"
##       UI on actual audio readiness rather than scene-load timing.
#
##   event_played(sound_name: String, position: Vector3,
##                source_peer_id: int)
##       Fired locally every time an event plays on this peer, whether
##       triggered locally or received via replication. `source_peer_id`
##       is 0 for local triggers, otherwise the peer ID that initiated
##       it. Useful for HUD indicators ("Player 2 fired"), spatial
##       chat bubbles, replay logs.
#
##   listener_attached(target: Node3D)
##       Emitted when set_local_player() successfully attaches the
##       GoolListener3D. Useful as a "audio system is fully wired"
##       signal for boot sequencing.

@tool
class_name CoopAudioRoot
extends Node

## Sound bank to register on _ready. Required. Without one, no sound
## names will resolve and play_event_*() calls will fail silently.
@export var sound_bank: GoolSoundBank = null

## Default replication mode for events fired via play_event_*().
## SERVER_AUTHORITATIVE is the safest default for coop games — every
## peer agrees on what happened because the server decides. Use
## CLIENT_PREDICTED for player abilities where latency matters.
## CLIENT_AUTHORITATIVE is fine for cosmetic sounds (footsteps,
## locomotion) where cheating cost is low.
@export var default_event_mode: int = 0  # NetworkedAudioEvent.Mode.SERVER_AUTHORITATIVE

## Default audible radius in meters for positional events. Peers
## outside this distance from the event position won't receive the
## RPC at all (bandwidth optimization). Override per-call if you
## need a quieter or louder event.
@export_range(1.0, 1000.0, 1.0, "suffix:m") var default_radius: float = 50.0

## Default priority for events (0 = lowest, 255 = highest). When the
## mixer is at its voice cap, higher-priority events steal voices
## from lower-priority ones. 128 is the conventional "normal" value;
## bump UI sounds and critical events to 200+, drop ambience to <64.
@export_range(0, 255, 1) var default_priority: int = 128

## Drop replicated events older than this threshold. Network hitches
## can deliver a "gunshot at position X" RPC 800ms after the fact,
## which sounds wrong (the shooter has already moved). Default 200ms
## matches NetworkedAudioEvent's default. Set higher for slower-paced
## games where late events are acceptable.
@export_range(50.0, 5000.0, 10.0, "suffix:ms") var late_threshold_ms: float = 200.0

## Whether the loader should warn (via push_warning) if sound_bank
## is null at _ready time. Off by default for tool-mode editing
## where the bank may not yet be assigned.
@export var warn_if_bank_unassigned: bool = false


signal sound_bank_loaded(results: Dictionary)
signal event_played(sound_name: String, position: Vector3, source_peer_id: int)
signal listener_attached(target: Node3D)


# Lazily-created children. Created during _ready() in run mode;
# in @tool mode they aren't created to avoid leaking state during
# editor saves.
var _bank_loader: Node = null
var _event_channel: Node = null
var _listener: Node = null
var _local_player: Node3D = null

# Scene paths to the child prefabs. Resolved at _ready time so this
# prefab works correctly whether installed as `res://addons/gool/`
# (typical) or copied into an example project at a different path.
const _BANK_LOADER_SCRIPT := "res://addons/gool/prefabs/gool_sound_bank_loader.gd"
const _EVENT_SCRIPT       := "res://addons/gool/prefabs/networked_audio_event.gd"
const _LISTENER_SCRIPT    := "res://addons/gool/prefabs/gool_listener_3d.gd"


func _ready() -> void:
	# @tool mode: don't create runtime children, just exist for the
	# inspector. Same convention as the other prefabs.
	if Engine.is_editor_hint():
		return

	_create_bank_loader()
	_create_event_channel()
	# Listener is deferred until set_local_player() — players often
	# spawn after the scene tree is set up (via lobby flow), so we
	# can't assume a target exists at _ready time.


# ---------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------

## Bind the local player to the audio listener. Call this after the
## local player's character spawns. Safe to call multiple times —
## re-binding to a different node moves the listener accordingly
## (useful for spectator-mode handoff or possession changes).
##
## `target` should be the Node3D you want sound to be heard from —
## typically a head bone, camera, or the player root.
func set_local_player(target: Node3D) -> void:
	if target == null:
		push_warning("[CoopAudioRoot] set_local_player(null) — detaching listener")
		_detach_listener()
		return

	_local_player = target
	_attach_listener_to(target)
	listener_attached.emit(target)


## Play a positional one-shot event. Replicates per default_event_mode
## to all peers within default_radius of `position`. Override the mode
## or other parameters via the optional arguments.
##
## Returns the prediction_id from NetworkedAudioEvent.play() (only
## meaningful in CLIENT_PREDICTED mode; opaque otherwise). Use it with
## cancel_event() to abort a prediction.
func play_event_3d(sound_name: String, position: Vector3,
		mode: int = -1, radius: float = -1.0,
		priority: int = -1) -> int:
	if _event_channel == null:
		push_error("[CoopAudioRoot] play_event_3d called before _ready " +
				"completed; ignoring '%s'" % sound_name)
		return -1

	# Resolve defaults. -1 sentinels let callers override individual
	# fields without specifying the whole tail.
	var resolved_mode := mode if mode >= 0 else default_event_mode
	var resolved_radius := radius if radius > 0.0 else default_radius
	var resolved_priority := priority if priority >= 0 else default_priority

	var pid: int = _event_channel.play(sound_name, position,
			resolved_mode, resolved_radius, resolved_priority)
	event_played.emit(sound_name, position, 0)
	return pid


## Play a non-positional one-shot event (UI sound, music sting,
## narrator line). Same replication semantics as play_event_3d but
## without spatial filtering — every peer receives it regardless of
## distance.
func play_event_2d(sound_name: String, mode: int = -1) -> int:
	# Implementation: a "non-positional" event in our model is just a
	# positional event with infinite audible radius and a position
	# that the listener treats as "centered on me." We use
	# Vector3.ZERO as the canonical position; receivers see it as
	# attached to their listener (because attenuation never kicks in
	# at the infinite-radius / zero-distance combination).
	return play_event_3d(sound_name, Vector3.ZERO, mode, 1e9)


## Cancel an in-flight prediction. Only meaningful for events fired
## in CLIENT_PREDICTED mode where the server later rejects the
## prediction. `fade_out_ms` softens the cancellation so the audio
## doesn't click off.
func cancel_event(prediction_id: int, fade_out_ms: float = 50.0) -> void:
	if _event_channel == null:
		return
	_event_channel.cancel(prediction_id, fade_out_ms)


## Detach the listener from the current local player without binding
## a new one. Useful for spectator transitions or pause-menu states
## where no character is "the player."
func detach_listener() -> void:
	_detach_listener()


# ---------------------------------------------------------------------
# Internals
# ---------------------------------------------------------------------

func _create_bank_loader() -> void:
	var script := load(_BANK_LOADER_SCRIPT)
	if script == null:
		push_error("[CoopAudioRoot] Couldn't load GoolSoundBankLoader script " +
				"at %s. Is the gool addon installed correctly?" % _BANK_LOADER_SCRIPT)
		return

	_bank_loader = Node.new()
	_bank_loader.set_script(script)
	_bank_loader.name = "BankLoader"
	# Pass our export through to the loader.
	_bank_loader.bank = sound_bank
	_bank_loader.warn_if_unassigned = warn_if_bank_unassigned
	# Forward the registration_complete signal so callers can listen
	# from CoopAudioRoot directly without reaching into the child.
	_bank_loader.registration_complete.connect(_on_bank_registered)
	add_child(_bank_loader)


func _create_event_channel() -> void:
	var script := load(_EVENT_SCRIPT)
	if script == null:
		push_error("[CoopAudioRoot] Couldn't load NetworkedAudioEvent script " +
				"at %s. Is the gool addon installed correctly?" % _EVENT_SCRIPT)
		return

	_event_channel = Node.new()
	_event_channel.set_script(script)
	_event_channel.name = "EventChannel"
	_event_channel.mode = default_event_mode
	_event_channel.default_audible_radius = default_radius
	_event_channel.default_priority = default_priority
	_event_channel.late_threshold_ms = late_threshold_ms
	# Forward received_remote so callers can observe events that
	# arrived via replication (versus ones they triggered locally).
	_event_channel.received_remote.connect(_on_remote_event)
	add_child(_event_channel)


func _attach_listener_to(target: Node3D) -> void:
	# If we already have a listener, just move it. Otherwise create.
	if _listener == null:
		var script := load(_LISTENER_SCRIPT)
		if script == null:
			push_error("[CoopAudioRoot] Couldn't load GoolListener3D script " +
					"at %s." % _LISTENER_SCRIPT)
			return
		_listener = Node3D.new()
		_listener.set_script(script)
		_listener.name = "Listener"
	else:
		# Re-parenting: remove from current parent before re-adding.
		var parent := _listener.get_parent()
		if parent != null:
			parent.remove_child(_listener)
	target.add_child(_listener)


func _detach_listener() -> void:
	if _listener == null:
		return
	var parent := _listener.get_parent()
	if parent != null:
		parent.remove_child(_listener)
	_listener.queue_free()
	_listener = null
	_local_player = null


# Forward the loader's signal up to consumers of this prefab.
func _on_bank_registered(results: Dictionary) -> void:
	sound_bank_loaded.emit(results)


# Surface replicated events so consumers can drive HUD indicators,
# replay logs, or "Player 2 fired" overlays.
func _on_remote_event(sound_name: String, position: Vector3,
		peer_id: int) -> void:
	event_played.emit(sound_name, position, peer_id)


func _exit_tree() -> void:
	# Defensive cleanup. Godot will free children automatically, but
	# detaching the listener explicitly ensures the engine-side
	# emitter handle is released before the C++ side is torn down.
	if _listener != null and is_instance_valid(_listener):
		_detach_listener()
