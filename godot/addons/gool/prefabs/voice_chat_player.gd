# addons/gool/prefabs/voice_chat_player.gd
#
# Per-player voice chat node. Add one of these to each player's
# scene (or instantiate dynamically when a peer connects). It:
#
#   - Registers a voice source with the gool runtime on _ready
#   - Receives RPC voice packets from the network and routes them
#     to runtime.submit_voice_packet
#   - Polls jitter/loss every second and emits a signal if the
#     connection degrades past warning thresholds
#
# The host's networking layer is responsible for capturing the
# local microphone, encoding through libopus (or any codec the
# host wires through IVoiceCodec), and sending the encoded bytes
# via send_voice_packet RPC. This script handles only the receive
# side — capturing audio is platform-specific in Godot and outside
# the scope of a generic prefab.
#
# Multiplayer model: this node uses Godot's standard MultiplayerAPI.
# Set the multiplayer authority (typically the player_id) and the
# RPC will fan out automatically.

@tool
class_name VoiceChatPlayer
extends Node3D

## Numeric player id. Must match the engine-side AudioPlayerId. We
## recommend using Godot's multiplayer peer id directly (returned
## by multiplayer.get_unique_id()).
@export var player_id: int = 0

## Whether to auto-poll voice quality and emit warnings.
@export var quality_monitoring: bool = true

## Jitter threshold (in ms) above which voice_quality_warning fires.
@export_range(10.0, 500.0, 1.0, "suffix:ms") var jitter_warning_ms: float = 80.0

## Packet-loss ratio (0..1) above which voice_quality_warning fires.
@export_range(0.0, 1.0, 0.01) var loss_warning_ratio: float = 0.10

## Mute this player's voice locally. Packets still arrive but are
## dropped at the decode boundary (CPU savings real and measurable).
## Persistence across sessions is YOUR job — gool doesn't own the
## player database; save this value with the player's other prefs and
## restore on reconnect.
@export var muted: bool = false:
	set(value):
		muted = value
		if _registered and _runtime != null:
			_runtime.set_voice_source_muted(player_id, value)

## Per-player volume attenuation. 1.0 = unchanged, 0.0 = silence, >1
## = boost above unity (clamped to int16 at the engine boundary).
## Default 1.0. Range exposed as 0..2 — values above 2 work but are
## almost always wrong.
@export_range(0.0, 2.0, 0.01) var volume: float = 1.0:
	set(value):
		volume = value
		if _registered and _runtime != null:
			_runtime.set_voice_source_volume(player_id, value)

signal voice_quality_warning(jitter_ms: float, loss_ratio: float)

var _runtime: Node = null
var _registered: bool = false
var _quality_timer: float = 0.0
const _QUALITY_POLL_INTERVAL := 1.0    # seconds

func _ready() -> void:
	if Engine.is_editor_hint():
		return
	_runtime = get_node_or_null("/root/Gool")
	if _runtime == null:
		push_warning("VoiceChatPlayer: /root/Gool autoload not found. The gool plugin is installed but not enabled. Fix: open Project Settings → Plugins, find 'gool' in the list, tick the Enable checkbox. (If gool is not in the list, the addon folder is missing — see https://github.com/siliconight/gool for install instructions.)")
		return
	if not _runtime.is_initialized():
		await _runtime.ready_to_play
	if player_id == 0:
		# Default to this peer's multiplayer id when one is available.
		if multiplayer != null and multiplayer.has_multiplayer_peer():
			player_id = multiplayer.get_unique_id()
	if player_id != 0:
		_registered = _runtime.register_voice_source(player_id)
		if not _registered:
			push_warning(
				"VoiceChatPlayer: register_voice_source(%d) failed. "
				% player_id
				+ "Three things to check:\n"
				+ "  (1) The voice source budget is full. "
				+ "AudioConfig.budget.maxVoiceSources defaults to 16 — "
				+ "if you have more remote players, raise it in "
				+ "res://gool/config.json.\n"
				+ "  (2) This player_id is already registered. "
				+ "RegisterVoiceSource is idempotent (returns the same "
				+ "handle), so this is rare — but if your code re-creates "
				+ "VoiceChatPlayer nodes for the same peer, the "
				+ "old node may not have unregistered cleanly.\n"
				+ "  (3) The runtime isn't initialized yet. The prefab "
				+ "awaits Gool.ready_to_play before registering, but if "
				+ "init failed earlier (see prior errors), registration "
				+ "won't succeed."
			)
		else:
			# Apply any property values set before _ready (e.g. via
			# editor inspector or pre-instantiation assignment).
			if muted:
				_runtime.set_voice_source_muted(player_id, true)
			if volume != 1.0:
				_runtime.set_voice_source_volume(player_id, volume)

func _process(delta: float) -> void:
	if not _registered or not quality_monitoring:
		return
	_quality_timer += delta
	if _quality_timer < _QUALITY_POLL_INTERVAL:
		return
	_quality_timer = 0.0
	var jitter: float = _runtime.get_voice_jitter_ms(player_id)
	var loss: float   = _runtime.get_voice_packet_loss_ratio(player_id)
	if jitter >= jitter_warning_ms or loss >= loss_warning_ratio:
		voice_quality_warning.emit(jitter, loss)

# Receive a voice packet over the network. Hosts call this via RPC
# from the player's networking peer. We unreliably-ordered route the
# bytes; sequence_number / send_timestamp_ms must come from the
# sender for jitter calculation to be meaningful.
@rpc("any_peer", "unreliable_ordered")
func receive_voice_packet(bytes: PackedByteArray,
							sequence_number: int,
							send_timestamp_ms: int) -> void:
	if not _registered or _runtime == null:
		return
	# Use the host's tick clock as the deterministic arrival
	# timestamp. Time.get_ticks_msec() is wall-clock-based but
	# consistent within a single run; for replay determinism, pass
	# the host's simulation tick time instead via a custom override.
	var arrival := Time.get_ticks_msec()
	_runtime.submit_voice_packet(player_id, bytes, sequence_number,
								   send_timestamp_ms, arrival)
