extends Node

# voice_loopback_receiver.gd
#
# Receives packets from voice_capture.gd (via the
# packet_dispatched signal) and feeds them back into gool as if
# they came from a remote peer. Demonstrates how the receive half
# of a voice chat pipeline wires up:
#
#   1. Register a voice source for the peer
#   2. On each incoming packet, call submit_voice_packet with
#      sequence + timestamp metadata
#   3. Monitor jitter / packet loss via the runtime API
#
# In production, the VoiceChatPlayer prefab (shipping with gool)
# does this for you — you just add one per peer and route your
# network's voice RPC to its receive method. This script reproduces
# the same logic inline so you can see what happens without the
# prefab indirection.

@export var capture_node_path: NodePath
@export var peer_id: int = 9999  # Must match LOOPBACK_PLAYER_ID in voice_capture

var _registered: bool = false
var _stats_timer: float = 0.0
const _STATS_INTERVAL_S: float = 2.0

func _ready() -> void:
	# Register the loopback peer as a voice source so gool's
	# scheduler knows to expect packets for it. Without this,
	# submit_voice_packet returns false (the C++ side wants to
	# know the player_id space ahead of time).
	if Gool.has_method("register_voice_source"):
		_registered = Gool.register_voice_source(peer_id)
		if not _registered:
			push_warning("[voice_chat] register_voice_source(%d) failed" % peer_id)

	# Wire up to the capture node's packet stream.
	if capture_node_path.is_empty():
		push_warning("[voice_chat] capture_node_path not set — receiver inactive")
		return
	var capture := get_node_or_null(capture_node_path)
	if capture == null:
		push_warning("[voice_chat] capture node not found at " + str(capture_node_path))
		return
	if not capture.has_signal("packet_dispatched"):
		push_warning("[voice_chat] capture node has no packet_dispatched signal")
		return
	capture.packet_dispatched.connect(_on_packet_received)

func _process(delta: float) -> void:
	if not _registered:
		return
	_stats_timer += delta
	if _stats_timer < _STATS_INTERVAL_S:
		return
	_stats_timer = 0.0
	# Sample voice quality. In a real game you'd surface these in
	# a HUD widget or use them to drive a "weak signal" indicator.
	if Gool.has_method("get_voice_jitter_ms"):
		var jitter_ms: float = Gool.get_voice_jitter_ms(peer_id)
		var loss_ratio: float = Gool.get_voice_packet_loss_ratio(peer_id)
		print("[voice_chat] peer %d — jitter %.1f ms · loss %.1f%%"
				% [peer_id, jitter_ms, loss_ratio * 100.0])

# Called whenever voice_capture has a packet to send. In a real
# system this would be the @rpc method body on a VoiceChatPlayer
# node — receiver and sender would NOT be in the same process.
func _on_packet_received(player_id: int, bytes: PackedByteArray,
		seq: int, send_ts_ms: int) -> void:
	if not _registered:
		return
	# Submit to gool. arrival_timestamp_ms = -1 lets the engine use
	# its own arrival time (Time.get_ticks_msec internally); pass an
	# explicit value if your network layer provides a tighter
	# clock-synced arrival timestamp.
	var arrival_ts_ms: int = Time.get_ticks_msec()
	var ok: bool = Gool.submit_voice_packet(
			player_id, bytes, seq, send_ts_ms, arrival_ts_ms)
	if not ok:
		# Common causes: not initialized, peer_id not registered,
		# bytes empty. Drop silently in production — voice loss is
		# normal; the jitter buffer will reorder + interpolate.
		pass
